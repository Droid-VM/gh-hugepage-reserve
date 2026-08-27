# gh_hugepage_reserve

[English](README.md) · [繁體中文](README_zh.md) · **简体中文**

Gunyah VM 的 2MB 大页池内核模块。**`POOL_DESIGN.md` 是单一事实来源**——本文是导览,
细节与依据(含实测数据)以该文档为准;文中 §n 均指向它的章节。

以可加载 `.ko` 发布(KernelSU / APatch / Magisk 友好)。支持 KMI:`android14-6.1`
… `android16-6.12`,含 6.6 厂商内核(如 SM8750);更旧的 KMI(5.10 / 5.15)只放
无功能 placeholder,让 Magisk 模块仍装得上。

---

### 免责声明——风险自负

> 本模块从内核内部改动内核内存管理行为:它在运行期解析未导出符号,并刻意迁移、
> 回收系统内存。与你的内核、厂商改过的 mm 不合,或单纯运气不好,都可能造成
> **崩溃、重启、数据遗失、设备内存耗尽,或任何我们没预期到的后果**。它只在
> 少数设备与内核上开发测试,其他一律是未知领域。不提供任何明示或默示的担保——
> 作者不对你的设备或数据的任何损害负责。安装前请先确保有离线移除模块的手段
> (recovery / 安全模式 / `adb`),并先备份数据。

### 前置条件

客体 RAM 必须真的是 2MB shmem THP,否则根本没有 order-9 分配可拦:

```sh
echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

以 Magisk/KernelSU 模块安装时,这件事每次开机自动做
(`package/module/service.sh`,挂在 late_start 才盖得过厂商默认)。DroidVM app
也会在每次开 VM 时设一次,而 crosvm 必须主动要求大页。

---

## 用途

Gunyah 手机(demand-paging 客体)上,VM 的 guest RAM 以 2MB(order-9)为单位向内核
分配。系统开机一段时间后内存必然碎片化——实测 84% 的 2MB 窗口含无法迁移的
hard straggler(§前言)——届时再想凑 2MB 连续页已经来不及;**early-boot 是唯一能
拿满的时机**。

本模块因此在开机时预先取得并长期监护一批 2MB 页:

- **VM 要用时**:拦截 VM 的 2MB 分配改发池页,VM 关闭后精确接回,下次开 VM 再用。
- **VM 没在用时**:超出池份额的监护页翻成 `MIGRATE_CMA` 储备池借给系统
  (page cache / mTHP),不让内存闲着;要用时再迁走借用者拿回。

两个用户目标值:`pool_want`(池份额,I1)与 `pool_want_with_cma`(总监护,I2)。
外部消费者是管理 app(DroidVM)、`load.sh` 与 `serve_test`,sysfs 接口是既存契约(§10)。

---

## 机制

### 粗略

只讲功能,不含细节:

1. **insmod 同步建池**:module_init 在自己的上下文把池填到目标值才返回——
   加载脚本与 app 以「insmod 返回时池已建好」为前提。
2. **serve**:hook 拦截 VM 的 order-9 分配,从池发页;池空或来源不对就放行原始分配。
3. **回收**:VM 放掉页时 free hook 依 pfn 精确接回池;VM 关机后 release worker
   主动归还借出页,超时(10s)仍拿不回的放弃追踪。
4. **CMA 储备池**:池份额之上的监护页整块翻成 CMA 借给系统;池短缺时从储备池
   迁走借用者拿回。搭配 unlock_cma 旁路放宽一般 movable 分配进 CMA,储备池才真的
   会被借用。
5. **sysfs 控制/观测**:写两个 want 目标、按 `acquire` 主动采集(含全区 sweep + evict
   的重压模式)、读 `refill_stat`/`cma_usage` 等统计。

### 详细

含细节与不变量。核心概念是 **custody(监护)**:一个 2MB 格属于我们,或属于系统,
整份设计只有这一条边界。

**页的状态**(§1):每个 2MB 格恰处于下列之一——

| 状态 | 意义 |
|---|---|
| `not_useable` | 非 RAM / carveout / 厂商 CMA / ZONE_MOVABLE,insmod 判定一次,永不参与 |
| `external` | 系统的(不在监护内) |
| `avail` | 池中待命 |
| `served` | 借给 VM(GUP pin + 模块一份保护引用) |
| `released` | VM 已放手、去向未定(等 free hook 接回或超时放弃) |
| `cand` | collect_cma 组装中,不可 serve/shed |
| `verify` | CMA 参数验证窗暂存(单次调用内) |
| `cma` | 储备池,借给系统 |

**帐目定义与不变量**(§1):

```
custody   = avail + served + released + cand + verify
held      = avail + served + released + verify
held_cma  = held + cma

I1  held     → pool_want               (收敛目标,非瞬间结构不变式)
I2  held_cma → effective pool_want_with_cma
W   恒有 pool_want <= pool_want_with_cma <= pool_size_max,或 with_cma == 0(CMA 禁用);
    S>1 时两者皆为 S 的倍数(S = 一个 CMA 块含几个 2MB 格)
P   0 <= pool_total <= configured_total <= pool_size_max
    (pool_total = 已实际取得、允许背景补回的「已证明容量」,不是 held 的别名)
U   CMA 整块一致:同一 CMA 块的 S 格要嘛全 CMA 要嘛全非 CMA
Q   最小化 custody 中不属于 cma_able 块的页(碎块最少化)
G   簿记一致:state / 链表 / 计数器互相吻合(debug=1 时 pool_check 验证)
```

`held` 把 `released` 算进去是刻意的:released 的页大概率几微秒内就回来,不算它,
VM 关机瞬间 deficit 暴增,worker 会去买一堆「正要回家的页」的替代品。
**放弃(purge)那一刻,deficit 才真的打开。**

**四支柱**(§前言):

1. **cma_able = custody 判定**:CMA 块的全部子格都在监护范围即成立。不存块标志,
   扫相邻 S 格实时导出(S≤4 时同一条 cacheline)。
2. **cma_ready = 全部子格 AVAIL**:flip 进 CMA 的唯一合法对象。served 页是
   `FOLL_LONGTERM` pin,含 pin 的块翻进 CMA 是死路(GUP longterm 会先迁移再 pin)。
3. **升降级只在 custody 边界**,全部在 worker(可睡)上下文;atomic 热路径最坏 O(S)。
4. **两层寻址、空洞免疫**:chunk 化的绝对坐标表(`mem_section` 同款构造),
   ARM 物理地址大空洞只花顶层一个 NULL 指针。

**serve 细节**(§3.2/§4):kretprobe 拦 `__alloc_pages` 返回,四道快滤
(order ≠ 9 / 池空 / 非 owner / gfp 无 `__GFP_MOVABLE`)全过才进转移。转移在
pid_lock(read)→pool_lock 固定锁序内一次完成 owner 验证 + AVAIL→SERVED,离开 AVAIL 前
`get_page()`——这份保护引用让外借期间 order-9 compound 不能 split/migrate。
取页三阶序:先碎块(avail_non)→ 再 cma_able → 最后才拆 cma_ready 整块。

**free return 细节**(§3.2):free hook(tracepoint)命中自家 RELEASED 页后,
**在同一次 pool_lock 内**比较 held↔want、held_cma↔want_cma 决定 KEEP(重建 order-9
compound、转 AVAIL、bypass 原始 free)或 DROP(转 EXT,让原 free 继续进 buddy)。
拆成 query + edge 两次调用会有 TOCTOU,所以是单一事件 api。

**release 细节**(§6):release worker 逐 SERVED 三分类——`refcount==1 && !mapping`
→ 先标 RELEASED 再锁外 `put_page`;`refcount>1` → pin 未放,下轮再看;
`page_count==0` → orphan(hook 漏接),直接转 RELEASED 交给既有的 sweep/purge 出路。
**放手 = pid 死亡事件 + 3 秒宽限**(owner 活着——不论 vm_count——一律不放手;
vm_count 只做 serve 的 gate;宽限让 exit 清算的 free 走回收而非放手,时钟只绑
死亡、永不清除);`released_at + GRACE=10s` 只管 RELEASED 的 purge。

**CMA 储备池细节**(§3.2):

- flip(avail→cma)只取 cma_ready 整块,前置是 `cma_params_state == VERIFIED`——
  第一次 flip 前由 `pool_verify_cma_params()` 用完整持有的验证窗实地验
  pageblock_order 边界、CmaFree delta、CMA-mode grab,失败即终态 UNAVAILABLE。
- stage_in(cma→avail)与 drop(cma→ext)都要先整块 contig grab 迁走借用者,
  **会失败**(pin/writeback);失败块本 run 跳过、保留 CMA 标签并**继续计入
  pool_cma**——帐与标签必须一致,**绝不「只翻标签」**(free 页还躺在 CMA freelist,
  翻了就是 CmaFree 帐漂移 + 页对 unmovable 隐形)。
- acquire 的 gfp 铁律:GFP_KERNEL 系、禁 movable/`__GFP_CMA`——否则 unlock_cma
  放宽后会从自家储备池捞页,且 movable 页会在 FOLL_LONGTERM pin 前被 GUP 迁走。

**并行契约**(§2,race 安全的结构性理由):

- **单调用原子、无跨调用不变式**:每个 pool api 在 pool_lock 内原子完成簿记,
  不存在需要事后修复的结构,ABA 一族没有寄生点。
- **api 自卫**:调用者的条件检查全是锁外建议性的;每个 public api 锁内重验前置,
  不成立就 no-op,绝不信任调用者。
- **清单 = pfn 值,消费点重验**:扫描集存 pfn 值不存指针;slot 表静态、memmap 恒在,
  悬空在结构上不可能,会发生的只有过期,由消费点锁内重验仲裁。
- **迭代 = 索引序表扫,不跟链接**:「游标指的 next 被并行搬走」这个问题类别不存在。
- 两条硬纪律:(a) **锁内禁 put_page/__free_pages**(free tracepoint 会重入 pool_lock);
  (b) **先改 state 再放手**(release 先标 RELEASED 再 put,shed 先标 EXT 再 free,
  顺序反了各是一种数据遗失)。

---

## 架构

### 总架构

三层:**consumer / worker / pool api + kapi**。consumer 依「调用目标」分组,
每组只碰一层;完整图与边的说明见 §0.1。

```
consumer 层(依调用目标分组;insmod/rmmod 例外——触碰全层)
┌────────────────────────┐ ┌───────────────────┐ ┌───────────────────┐ ┌────────────┐
│ -> pool api            │ │ -> release worker │ │ -> adjust worker  │ │ -> kapi    │
│ serve hook (atomic)    │ │ vm_shutdown       │ │ sysfs want shrink │ │ unlock_cma │
│ free hook (atomic)     │ │ vm_unshare        │ │ sysfs acquire / 0 │ │ pinprobe   │
│ sysfs 读查询, vm_boot  │ │ pid-exit hooks    │ │                   │ │ (旁路)     │
└───────────┬────────────┘ └─────────┬─────────┘ └─────────┬─────────┘ └──────┬─────┘
            │                        ▼                     ▼                  │
            │            ┌───────────────────────┐   ┌────────────────────┐  │
            │            │ release_worker (1s×5) │──►│ adjust_worker      │  │
            │            │ served→released,      │   │ (10ms×MAX)         │  │
            │            │ 超时放手, owner GC    │   │ 7-phase pipeline   │  │
            │            └───────────┬───────────┘   └─────────┬──────────┘  │
            ▼                        ▼                         ▼             │
┌─────────────────────────────────────────────────────────────────────┐      │
│ pool api   public: pool_{src}_to_{dst}(一条边一个函数)+ 查询/action │      │
│            private: pool_private_{slot,classify,promote,demote,...} │      │
└──────────────────────────────────┬──────────────────────────────────┘      │
                                   ▼                                         │
┌─────────────────────────────────────────────────────────────────────┐      │
│ kapi   alloc_pages / alloc_contig_range / set_pageblock_migratetype │◄─────┘
│        walk_system_ram_range / evict_range / drop_slab / drain / …  │
└─────────────────────────────────────────────────────────────────────┘
```

要点:

- **hook 组只碰 pool api**,不做政策;**worker 是状态机唯二的驱动者**,所有转换都经
  public api;release 每轮 `adjust_try(RELEASE)` 触发 adjust(背景补页走这条边)。
- **内核符号只活在 kapi 一层**;unlock_cma 是旁路,只用 kapi,不碰 pool/worker/状态机(§9)。
- **insmod/rmmod 是唯二触碰全层的 consumer**:kapi 解析、slot 表构建、hook 装卸、
  worker 启停、同步清理,固定序见 §8(卸载铁律:先卸 hook → 停 worker → 交还引用)。

构建是 unity build:`gh_hugepage_reserve.c` 依序 include `parts/`。pool 对象与两个
worker 环境无关,同一份源代码也在 userspace 对 `tests/shim.h` 编译——CI mock harness
(§3.3)以决定性 fake buddy 重放 34 个场景;kapi/hooks/sysfs/unlock_cma 只在真机验证。

### kernel api(kapi)

`parts/gh_kapi.c.inc`(契约见 §3.3)。**全模块唯一碰内核符号的层**,对上提供一张
粗粒度 adapter 表 `gh_kapi`:

```c
bool kapi.cap(feature);                        /* CONTIG_RANGE / DROP_SLAB / ... */
struct page *kapi.alloc_try(order, strong);
int  kapi.contig_range(start, end, noretry);   /* 半开 pfn 范围 */
bool kapi.candidate_range(start, end);         /* 纯读可行性探针 */
void kapi.evict_range(mode, start, end);
bool kapi.cma_floor_ok(nblocks);
void kapi.drop_slab(); kapi.drain_pages(); kapi.lru_add_drain_all();
/* … 完整表见 parts/gh_defs.h struct gh_kapi */
```

设计规则:

- **kapi 是模块自己的稳定 ABI**:调用端一律用规范化的 `kapi.op()`;逐内核版本分歧的
  真实符号(名称、签名、参数/返回语义)活在私有 `kraw` 结构 + shim 里,不外泄。
- **解析在加载时**(throwaway kprobe 查地址),缺就留 NULL、对应 `cap()` 回 false——
  整个 feature 前置拒绝,不会走到一半经错型指针调用(kCFI type-id 不符 = panic)。
- **ABI guard**:`load.sh` 的 `kapi_check` 读 `/sys/kernel/btf` 预检签名,不兼容的
  符号经 `disable_kapi` 参数列入禁用名单;`abi/kapi_abi.tsv` 是符号登记表。
- **folio 标志判读只活在这里**(candidate/evict 的内核实现);pool 对象对页的直接
  触碰限定在一小组可重定义原语,test build 以 shim 换掉它们——这就是整层 kapi
  可以被 mock backend 替换、pool/worker 能在 userspace 跑 CI 的前提。

### pool api

`parts/gh_pool.c.inc`。**pool 就是那个对象**(C 没有对象,pool api 即对象):slot 表、
三条 avail 链表、计数器、游标、扫描集与它们的锁**全部在 pool 内**;hook/worker/sysfs
只调用 public api、拿回值(pfn、计数、bool),从不持 pool_lock、从不看 `top[]`/slot。

命名规约(§3):public 是 `pool_{src}_to_{dst}`,**一条边一个函数**,同一条边的变体用
mode 参数;private 是 `pool_private_xxx`,只准 pool 内部调用。每个 public api 锁内
重验自己的前置条件,不成立就 no-op。

状态机(一条边 = 一个 public api,§3.1):

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                  external                                    │
└──┬─────────▲──────▲─────────┬─────▲───────────────────▲───────────────▲──────┘
   │acquire! │shed* │drop!    │grab!│flush              │expired/rmmod* │purge*
   ▼         │      │         │     │                   │               │
┌────────────┴─┐    │         │     │             ┌─────┴──────┐        │
│    avail     ├────┼─────────┼─────┼─serve──────►│   served   │        │
│              │◄───┼ret/sweep┼─────┼─────┐       └─────┬──────┘        │
│              │◄───┼──┐      │     │     │             │release        │
└──┬──────▲────┘    │  │      ▼     │     │             ▼               │
   ▼flip  │stage_in!│  │promote     │     │       ┌────────────┐        │
┌─────────┴────┐    │  │  ┌─────────┴──┐  └───────┤  released  ├────────┘
│     cma      ├────┘  └──┤    cand    │          └────────────┘
└──────────────┘          └────────────┘

*  = custody 出口(块降级点)     ! = 此边存在会失败的 api
```

| 边 | api | 调用者 |
|---|---|---|
| acquire(ext→avail) | `pool_ext_to_avail(how, …)`,五种取法是 mode | adjust |
| serve(avail→served) | `pool_avail_to_served(pid)` | serve hook(atomic) |
| release(served→released) | `pool_served_to_released()` | release worker |
| return(released→avail/ext) | `pool_released_return(pg)`(KEEP/DROP/MISS 事件 api) | free hook(atomic) |
| sweep(released→avail) | `pool_released_to_avail_sweep(pfn)` | adjust |
| purge(released→ext) | `pool_released_to_ext(EXPIRED/ALL)` | adjust / rmmod |
| expired(served→ext) | `pool_served_to_ext(EXPIRED/ALL)` | release / rmmod |
| flip(avail→cma) | `pool_avail_to_cma(n)` | adjust |
| stage_in(cma→avail) | `pool_cma_to_avail(n)` | adjust |
| drop(cma→ext) | `pool_cma_to_ext(n/ALL)` | adjust / rmmod |
| shed(avail→ext) | `pool_avail_to_ext(n)` | adjust / rmmod |
| grab(ext→cand)/ flush | `pool_ext_to_cand(pfn)` / `pool_cand_to_ext()` | adjust |
| promote(cand→avail) | 无 public——`pool_private_promote` 内部运行 | — |

内部结构(§0.2):两层寻址 `top[off>>30][(off>>21)&511]`(O(1) 最坏,空洞 = NULL
指针);链表只维护 avail 三条(`avail_non` / `avail_cma_able` / `avail_cma_ready`)——
唯一需要 atomic「给我下一个可用的」的消费者是 serve;其他状态一律索引序表扫或
O(1) 阈值标量(released 的 `(count, oldest)`)。块属性 cma_able/cma_ready 不存字段,
扫相邻 S 格实时导出。运行期零 kmalloc,所有扫描集 insmod 预配。

owner 注册表同属 pool facade,用独立 `pid_lock`(rwlock):entry 活到 pid 死亡,
serve 热路径先 lockless `pool_owner_maybe()` 快滤,命中才按 pid→pool 固定锁序转移。

### consumer

#### hook

`parts/gh_hooks.c.inc`(§4)。原则:**每个 hook 只做「一个 pool/facade 调用」,
不做政策**;政策(保留/放弃、目标比较)封装在被调用的 api 里。

| hook | 挂载点 | 动作 |
|---|---|---|
| serve | kretprobe `__alloc_pages` 返回 | 四道快滤全过 → `pool_avail_to_served(pid)`,NULL 则放行原分配 |
| free | tracepoint `android_vh_free_one_page_bypass` | 只认 order-9 → `pool_released_return(page)`,KEEP 时设 bypass 拦下 |
| vm_boot | kprobe(`GH_CREATE_VM` ioctl 路径) | `pool_owner_add(current)` |
| vm_shutdown / unshare | kprobe(vendor 符号,可用参数覆盖) | `release_vm_shutdown/unshare(current)`(facade 内更新 owner + arm release) |
| pid 死亡 | tracepoint `sched_process_exit`(core,GKI 必有) | group_dead 且在 owner 表 → `release_vm_exit(current)` |

serve 的四道快滤依成本排序:order ≠ 2MB → 池空 → 非 owner(lockless)→
gfp 无 `__GFP_MOVABLE`(实测:dma_heap 的 unmovable order-9 永不回 buddy,
不滤每 VM 偷走 2 页)。serve/free 都在 **atomic** 上下文,只碰 O(1)~O(S) 的
pool api;`hook_enable` / `reclaim_enable` 可个别开关,读回实际挂载状态。

#### worker

`parts/gh_release.c.inc` / `parts/gh_adjust.c.inc`(§5–§7)。hook 在 atomic 上下文,
只能做 O(1)~O(S) 的快动作;所有慢动作——向系统要页、迁移、扫全表、验证——都需要
可睡上下文,全部集中在这两个 worker。它们是状态机唯二的驱动者。

**共同模型:run 计数器**(§5)

worker 不是常驻线程,而是 workqueue 上的 delayed_work:醒来 → 做一小片工作 →
存进度 → 排下一次醒来。切小片的理由:pool_lock 只在片内短暂持有,serve/free hook
永远不会被 worker 的慢动作卡住。

ctx 里的 `run` 同时是**开关**和**看门狗**:每轮先 `run--`,归零就收尾;任何人随时
可写——写 0 = 叫停(最多再跑完当轮),写 N = 启动或续命。ctx 的其余字段从 init 到
finish 只有 worker 自己能改;外人要换工作内容一律**交棒**(`next_profile`),
绝不当场改 ctx——worker 可能正在锁外做慢动作,轮尾还会把自己的 ctx 存回来。

**release_worker——「VM 放手的页,收回来」**(每秒一轮)

三个 VM 事件(vm_shutdown / vm_unshare / pid 死亡)各 arm 5 轮;事件 facade 同时
更新 owner 表(vm_count 归因递减——纯 serve gate、死亡标记)。5 轮是
保证下限,不是上限。每轮做五件事:

1. **收页(served→released)**:索引扫全表找 SERVED,逐页看 refcount 三分类——
   - `refcount==1`(只剩模块自己那份保护引用)= VM 真的放手了 → 锁内先标
     RELEASED、盖时戳,再锁外 `put_page`。put 之后页走一般 free 路径,流过
     free hook 时由 `pool_released_return` 决定接回或放行。「先标再放」是硬纪律:
     顺序反了 free hook 会不认得这页,页就丢了。
   - `refcount>1` = VM/Gunyah 的 pin 还没放 → 这轮不碰,下轮再看。
   - `page_count==0` = **orphan**:free 已经发生但 hook 漏接(pcp-lag 或挂载空窗),
     模块那份引用已随 free 蒸发 → 不 put,直接标 RELEASED,交给既有的
     sweep(抢回)/ purge(放弃)出路,不开新路径。
2. **死亡放手(served→ext EXPIRED)**:逐 SERVED 查 owner 表——owner **活着就
   一律不碰**(不论 vm_count;vm_count 只是 serve 的 gate);owner 已死(标记)
   或已不在表 = 放弃追踪:标 EXT、放掉自己的引用、记 purge_log 与
   `owner.abandoned++`。从此页跟着还 pin 它的人活,与模块无关。放手 = pid 死亡
   **事件 + DEATH_GRACE(3s)宽限**(死亡才启动、永不清除的单向时钟——旧制的
   idle+10s 会与 VM 重开 1→0→1 竞态;宽限让 exit 清算的 free 走回收)。
3. **第 3 轮 `drain_pages`**:order-9 的 free 会先停在 per-CPU 缓存(pcp),
   不立刻流过 hook——实测每 VM 漏 ~2 页。第 3 轮(≈3 秒,关机 free 潮已退)
   drain 一次把它们逼出来。
4. **提早收工 / final**:「没有页还可能回来(released==0)、也没有页在等
   unpin(pending==0)」→ run 归 0 提早结束。final 轮再 drain、再收一次,并做
   owner GC——只清「已死且名下无页」的 entry;vm_count==0 但 pid 还活着的**保留**,
   app 要看得到「关了 VM 却不还页」的可疑 pid(D-state 检测,§10)。
5. **叫醒 adjust**:每轮 `adjust_try(RELEASE)`。等待期间 held 仍含 served/released,
   帐面没缺口,adjust 实际只有 precise 有事做;等 DROP/EXPIRED 真的打开 deficit,
   cheap/full/stage_in 才动起来——时序由条件系统自己排对,不需要两套 profile。

**自我续命**:轮次数完但 pending>0(还有页在等 unpin/宽限)→ `run=1` 续一轮。
有界:死亡放手在下一轮完成,RELEASED 由 purge 收敛,不会无限续。

设计核心:**放手是事件+短宽限(pid 死亡 + 3s),purge 是时钟(released_at+10s),
轮次只是载具**——所以 `manual_release` 高频触发、多个事件重复 arm,都只是多几次
无害的扫描,不会提早任何放手或 purge。

**adjust_worker——「把池调到目标值」**(每 10ms 一轮)

谁触发,决定拿到多痛的授权(profile,§5.1):

| profile | 触发时机 | 授权的取页力度 |
|---|---|---|
| `INSMOD` | 开机,module_init 同步驱动到完成 | cheap + full |
| `RELEASE` | release worker 每轮 | precise + stage_in(+ cheap + full,受 `refill_enable`) |
| `SHRINK` | want 目标调小 | 只有 stage_in |
| `USER` | 用户按 `acquire` | 全部(唯一含 main) |

profile 只决定 ACQUIRE phase 内开哪些力度(**政策**);七个 phase 骨架(**机制**)
不受 profile 管,每次 run 都依序走完。RELEASE 的目标快照另被 cap 到
`pool_total`(已证明容量)——背景补回只补「历史上真的拿到过」的量,用户把 knob
写大不会偷偷授权背景 reclaim。

**一次 run = 单趟 pipeline,不循环**:每个 phase 跑到自然完成才前进(可跨很多轮,
每轮只做一小片),走完即收工;没达标 = partial,记 stop_reason,重试属于下一个
trigger。acquire 在一次 run 里只有一次机会——后段 shed 挖开的缺口不会回头触发前段
再抓,「抓 → 凑不成 → 丢 → 再抓」的振荡在结构上不存在。

每轮开场固定动作:先做 released 超时 purge(O(1) 阈值挡住,过 GRACE 才真扫——
不等 phase 游标绕回来,它可能在 main 停很久);然后**重算五个数字,绝不缓存**
(池况随时被 hook 改变):

```
held           = avail + served + released     现在监护多少
diff_want      = want − held                   池份额缺(+)/多(−)多少
new_page_need  = want_cma − (held + cma)       总监护还缺多少 → 才需要向系统拿
reserve_target = want_cma − want               储备池应有的大小
cma_excess     = cma − reserve_target          储备池超额多少
```

七个 phase,依序:

1. **PREPARE(备料)**:五个数字全达标 → 直接收工(空跑 O(1) 出场)。USER run
   且有缺口才付一次 `drop_slab` + pcp drain(dentry/inode 缓存不在 LRU,任何采集
   都收不回它,但一页 dentry 就毒死一个 2MB 窗;丢出来的 order-0 落在 pcp,
   不 drain 合不进 buddy 高端,刚开的窗对后续采集隐形)。一趟走表建好本 run
   的扫描集:precise 清单
   (自己 released 还躺在 buddy 的页)与 main 清单(全部 EXT 窗:缺口优先、
   从上次游标接续绕一圈)。储备池有超额才付一次 CMA 占用探针 + bucket 排序
   (搬家成本低的块排前面)。
2. **ACQUIRE(取页)**——唯一受 profile 管的 phase。五级力度依「谁会痛」递增,
   一级做完或做不动才降到下一级:
   - **precise**:contig 抓回自己的 released 页(它们还躺在 buddy 没人用)。
     免费,谁都不痛,永远第一。
   - **cheap**:向 buddy「捡现成」(NORETRY,拿不到立刻放弃)。先试**整块**
     (一次 S 个 2MB,天生可翻储备)——开机大块充裕,池 + 储备池就是这样一次
     建成的;整块败一次就永久降到单页,本 run 不回头(大块耗尽不会自己长回来)。
   - **stage_in(cma→avail)**:池短、但总监护够(页都在储备池)→ 从自己储备池
     整块拿回,把借用者(page cache 等)迁出去。中痛,只痛自己人。会失败
     (借用者被 pin);血量 `cta_hp` 控制:有进展回满 8,连续零进展 8 批放弃。
   - **full**:alloc_pages RETRY_MAYFAIL——让内核自己 compaction + reclaim
     (可能 swap)。比 cheap 重、比 main 轻;内核自管,不需用户授权。失败即停:
     内核已经尽力,再问也是同一个答案。
   - **main**:重压,只有 USER。两种打法:`acquire=1` 是 CONTIG_ANY(让内核自选
     位置盲拿;血量 `main_hp` 成功 +3、连败 −1,归零 = "migration exhausted");
     `acquire=2/3` 是 CONTIG_AT **整区 sweep**——沿 main 清单一轮一窗:先纯读
     512 个 struct page 做可行性判定,**不过闸就不 evict**(「never white-kick」:
     实测 84% 的窗有搬不走的 straggler,没这个闸就是对 84% 的窗白丢 page cache);
     过闸先试便宜的 async 抓,不行才 evict(模式 A:每 8 个失败窗做一次全系统
     memcg reclaim;模式 B:只逐出该窗自己的 folio)再 sync 抓一次,成败都往前走。
     每窗前查 `mem_available`,低于 `acquire_mem_floor_mb`(512MB)立即刹车
     (实测不刹会 RCU stall 到重启)。游标「先前进再做事」:run 被中断,
     下个 run 从断点之后接续,不重试同一批。
   - **轮尾 eager flip**:不分力度,每轮顺手检查——池份额已满、储备未满、有整块
     ready → 翻 1 块进 CMA。早翻早好:avail 页对系统完全不可用,cma 页至少可被借用。
3. **VERIFY_CMA(验证)**:CMA 参数还是 PENDING 且是 INSMOD/USER run → 用完整持有
   的验证窗实地验一次(pageblock 边界、CmaFree 帐目、CMA-mode 抓回)。过了才进
   VERIFIED、才准 flip;结构性失败 = 永久禁用 CMA,当轮折回纯池模式。
4. **COLLECT_CMA(凑块)**:授权跟 main 走。对「差几格就凑满」的块抓缺口窗
   (同样过可行性闸、可 evict),抓到的先进 cand 隔离区**不入池**——直接入池会把
   held 顶超过 want,叫醒 shed 拆自己的台。凑满整块 → promote:cand 转 AVAIL、
   块内兄弟归队,同时守恒 free 掉等量碎页(held 不变、Q 质量变好)。run 结束时
   凑不成的 cand 全部 flush 还给系统(它的块本来就凑不齐,留着没意义)。
5. **AVAIL_TO_CMA(正式翻)**:把超出池份额的 ready 块批量(每轮 ≤8 块)翻满到
   `reserve_target`。每翻先问 `cma_floor_ok`——每翻一块,unmovable 工作集
   (~3.5GB,进不了 CMA)的活动空间就少 2MB×S,低于 floor(默认 1024MB)不翻。
   本 phase 翻过就 drain 一次,让 CmaFree 帐目跟上(GUI 读它)。
6. **CMA_FREE(缩储备)**:want_cma 调小 → 超额的储备块还系统。每块:整块抢回
   (迁走借用者,**会失败**)→ 全块在手才翻标签回 MOVABLE → free。抢不到的块
   保留 CMA 标签、继续计入帐(帐与标签必须一致,绝不只翻标签);血量 `drop_hp`
   归零 = "cma sources stuck",下个 trigger 重新探针再试。例外让路:池还短且本
   run 还有 stage_in 授权 → 跳过此 phase,超额留给 stage_in 变现,不放生。
7. **AVAIL_FREE(还多余)**:最后,真正多余的 avail 页(每轮 ≤32)还给系统。
   avail 已空但帐面还超(多余的页全在 VM 手上)→ 不等,直接收工——它们回来时
   free hook 的 DROP gate 会按当下目标放行,或由下个 trigger 的 run 来 shed;
   没有这个出口,这轮会空转到看门狗上限(20 分钟)。

**抢占与交棒**(`adjust_try`):没 run 在跑 → 立即开跑。USER run 在跑 → 谁都不能抢
(用户按的最大;再按一次回 -EBUSY,要先 `acquire=0` 取消)。背景 run 在跑 →
新触发者把 run 写 0(中断)并登记 `next_profile`(取优先序最高:USER > SHRINK >
RELEASE > INSMOD),自己立即返回;worker 在收尾时看到 next_profile 就以新 profile
重新开跑。交棒延迟 ≤ 一轮(10ms),ctx 从头到尾只有 worker 自己写。

**收尾**(`adjust_finish`):任何 run 归 0 的路径都走同一条,无分支——凑不成的
cand 还给 buddy、run-local 扫描集作废、USER run 记 stop_reason(app 逐字显示:
reached target / migration exhausted / low-memory floor / …)、有 next_profile
就交棒。`ADJUST_RUN_MAX=120000`(×10ms ≈ 20 分钟)是看门狗不是循环长度:
正常 run 在 pipeline 走完时就自然结束。

---

## 源代码布局、构建与测试

| 文件 | 层 | 测试 |
|---|---|---|
| `parts/gh_defs.h` | 共用类型、kapi adapter 契约、enum | — |
| `parts/gh_owner.c.inc` | owner 注册表(entry 活到 pid 死亡;vm_count 纯 serve gate) | mock |
| `parts/gh_pool.c.inc` | **pool 对象**:所有状态与锁都住这里 | mock |
| `parts/gh_release.c.inc` | release worker(served→released、超时放手) | mock |
| `parts/gh_adjust.c.inc` | adjust worker(七 phase 的取页/翻转 pipeline) | mock |
| `parts/gh_kernel_env.h` | 内核原语黏合层(锁、时钟、页操作) | 真机 |
| `parts/gh_kapi.c.inc` | 内核 kapi backend(符号解析、版本 shim) | 真机 |
| `parts/gh_hooks.c.inc` | kretprobe / kprobe / tracepoint 挂载 | 真机 |
| `parts/gh_sysfs.c.inc` | sysfs / 模块参数(对外 ABI,§10) | 真机 |
| `parts/gh_unlock_cma.c.inc` | movable→CMA 旁路 lever(§9) | 真机 |
| `parts/gh_pinprobe.c.inc` | `/dev/gh_pinprobe` 只读 pin 可行性探针 | 真机 |
| `tests/` | mock harness:shim、fake buddy、场景 | CI |
| `tier1/` | QEMU 集成测试台(companion `.ko` + exerciser) | CI |

```sh
make test              # Tier-0:userspace mock harness(ASan+UBSan,再跑 TSan);不需内核
make module KDIR=...   # 内核 .ko(对 GKI build tree)
make -C tier1 ...      # Tier-1:QEMU 集成台(见 tier1/README.md)
./build.sh             # 在 Docker 内构建全部 KMI + 打包 Magisk/KernelSU 模块
```

测试分三层。**Tier 0**(`tests/`,本 repo 的 CI)在没有内核的情况下验证池逻辑:
`make test` 编 `tests/harness.c`,以决定性 fake buddy 重放 **34 个场景**——
serve/归还、超时放手 + D-state、orphan/purge、S=2 块分类、CMA 储备池
(flip/stage-in/drop/verify,含 pageblock-order 差一级)、全区 sweep 含 evict、
shrink、profile 抢占/交棒、hook 缺席(temp-root)的 release→sweep 回路,外加一支
TSan race harness。**Tier 1**(`tier1/`,QEMU,本地跑)编出真的 `.ko`,在**人为制造的**
碎片化下对真实页触发 kprobe/tracepoint——不需设备、不需 Gunyah。**Tier 2** 是真机
(真的 `FOLL_LONGTERM`、真的碎片化、厂商差异)。

---

## sysfs 与参数对照

全部位于 `/sys/module/gh_hugepage_reserve/parameters/`。文件名、格式与错误码是与三个
外部消费者的**契约**(§10):管理 app(逐行轮询 `refill_stat`、写两个 want、按
`acquire`、统计读取前写 `reconcile`(一份 app 通吃两代:旧版模块必要、v12 为
no-op)、比对 `stop_reason` 字符串、用 `cma_usage` 画图)、
`load.sh`(preflight 参数与 fallback 阶梯)、`serve_test`。

### 目标与控制

| 文件 | 权限 | 用途 |
|---|---|---|
| `pool_want` | 0600 | 池份额目标(2MB 页数) |
| `pool_want_with_cma` | 0600 | 含储备池的总监护目标;`0` = 禁用 CMA |
| `acquire` | 0200 | `0` 中断 / `1` CONTIG_ANY / `2` sweep + 全系统 reclaim(模式 A)/ `3` sweep + 逐窗 evict(模式 B) |
| `reconcile` | 0200 | 旧版兼容 no-op——统计已即时,无账可对 |
| `manual_release` | 0200 | 写 `1` → 多跑一轮 release;写再频繁也不会提早放手 |
| `manual_refill` | 0200 | 写 `1` → `adjust_try(RELEASE)` |
| `hook_enable` | 0600 | serve kretprobe 开关;读回**实际**挂载状态 |
| `reclaim_enable` | 0600 | free tracepoint 开关;读回实际挂载状态 |

写入规则(§4):两个 want 都先 clamp 到 `pool_size_max`,CMA 可用且 S>1 时向上对齐
S 的倍数;coupling 维持 `want <= want_cma`(写大的 want 会拉高 with_cma;写低于
want 的 with_cma 会被擡到 want,**绝不反向缩 want**)。任一值调小 → clamp
`pool_total` 并异步触发 `adjust_try(SHRINK)`;只**调大**则只记录——还是要有人按
`acquire`。

错误码:`-EINVAL`(值不合法)、`-ENODEV`(insmod 未完成)、`-EBUSY`(want:
**任何** adjust run 进行中;`acquire`:USER run 进行中)、`-ENOSYS`(必要符号没
解析到——`acquire`:1 需 contig_pages、2 需 contig_range、3 另需
folio_isolate_lru + reclaim_pages;`pool_want_with_cma`:CMA 为 UNAVAILABLE)。
run 期间写 want 回 `-EBUSY` 是**故意的,不是防御 code**:每个 run 从启动快照起
追固定目标,要改只有 `acquire=0`(停掉在跑的 run)→ 写新值 → 重新按 `acquire`
这一条路;`refill_stat` 的 `acquire_active=1` 就是「现在写会被挡」的信号。

### 观测

| 文件 | 权限 | 内容 |
|---|---|---|
| `refill_stat` | 0400 | 17 行 `key=value`(见下) |
| `pool_avail` | 0400 | 池中待命页数 |
| `pool_cma` | 0400 | 储备池大小(2MB 页等量) |
| `pool_avail_cma_able` | 0400 | 属于 cma_able 块的 avail 页(非 VERIFIED 恒 0) |
| `pool_size_max` | 0400 | 由 RAM 导出的两个 want 上限 |
| `reclaim_debug` | 0400 | `o9_seen`、`del_hit`、`del_miss`、`gate_drop`、`skip_unmovable`、`reject`、`orphan`、`purged`、`in_hook`、`in_sweep`、`in_cma`、`in_user`、`in_refill` |
| `vm_owners` | 0400 | 逐行 pid / vm_count / served / abandoned / comm |
| `served_summary` | 0400 | tracked / live / orphan 统计,release 轮次顺手算 |
| `purge_log` | 0400 | 放弃页的 pfn / refcount / 现况(需 `debug=1`) |
| `cma_usage` | 0400 | 储备池占用(free/anon/file MB、块三态),~1s 缓存 |

`refill_stat` 字段:`state`、`pool_avail`、`pool_total`(已证明容量,§5.1)、
`served`(= served + released)、`pool_want`、`total_served`、`total_refilled`、
`active_vms`、`acquire_active`、`acquire_mode`、`acquire_stop_reason`、
`refill_enable`、`free_reclaim`(free hook 的真实状态)、`pool_want_with_cma`、
`pool_cma`、`pool_avail_cma_able`、`cma_pb_order`。`acquire_active` 反映 adjust
worker 是否有 run 在跑(**任何 profile**,含 release 触发的背景 refill 与
SHRINK),=1 同时表示写 want 会 -EBUSY、CMA 旁路暂停借出;`acquire_mode` /
`acquire_stop_reason` **只反映 USER run**,背景 run 不会盖掉。`cma_pb_order=-1`
表示这次开机整个 CMA 侧是关的(符号/preflight 值缺,或验证失败)。

`acquire_stop_reason` 是固定字符串集,app 逐字比对:`idle` / `acquiring` /
`already at target` / `pool capacity full` / `cma headroom floor` /
`cma flip failed (systemic)` / `stopped by user` / `reached target` /
`reached target,with_cma` / `migration exhausted` / `cma sources exhausted` /
`scanned all present memory` / `low-memory floor` / `evict-B unavailable` /
`quality converged` / `cma sources stuck`。

`vm_owners` 同时是卡死检测器。`abandoned` = **pid 死亡时**仍持有而被放手的页数;
活着的 pid 恒 0(不再有 abandon-while-alive)。活 pid 的卡死信号改看:vm_count==0
而 served>0 持续——crosvm 收尾偏慢或卡住,页仍在 SERVED 账上;app 可读
`/proc/<pid>/stat`,见 D state 即提示。

### movable→CMA lever(§9)

| 文件 | 权限 | 用途 |
|---|---|---|
| `moveable_to_cma_vender_already_allowed` | 0400 | `1` = 厂商内核本来就放行 movable→CMA,写 lever 是 no-op |
| `moveable_to_cma_gfp_cma_hook` | 0600 | 外科式,优先:vendor hook 对单纯 movable 请求 OR 上 `ALLOC_CMA` |
| `moveable_to_cma_restrict_cma_redirect_disabled` | 0600 | 全局:翻掉内核的 `restrict_cma_redirect` static key;`1` = movable 可以进去 |

没有这块,储备池就是「监护着、没人借得到」:原厂内核只把带 `__GFP_CMA` 的分配导向
CMA freelist,而 app 真正的工作集(page cache、mTHP anon)不带这个标志。两个 lever
默认关,前置是储备池已建好且采集静止(prefill 收工、adjust worker **无任何 run**
在跑——任何 profile 都可能翻动 CMA 区,不只用户按的 acquire),且 free CMA 低于
`cma_bypass_floor_mb` 就不放行。

### 模块参数

| 参数 | 权限 | 默认 | 用途 |
|---|---|---|---|
| `pool_want` / `pool_want_with_cma` | 0600 | 0 | 同上;insmod 时亦可给 |
| `system_reserve_mb` | 0400 | 6144(下限 64) | 算 `pool_size_max = min(ram − min(ram/2, system_reserve_mb), 表上限)` 时留给系统的量 |
| `system_reserve_mb_default` | 0400 | init 时算出 | **本机**的默认保留量 `min(RAM/2, 6144)`,不是那个内建常数——`RAM/2` 上限让两者在小 RAM 机上不同(8GB:默认 6144 实际只留 4096,设成 4096 以上都是 no-op)。设定值会覆盖 `system_reserve_mb`,所以这是默认值唯一还留着的地方;app 拿它当「调低保留量」的警告门槛 |
| `migrate_cma_val` | 0400 | −1 | preflight:运行期的 `MIGRATE_CMA` 值(来自 BTF) |
| `pageblock_order_val` | 0400 | −1 | preflight:pageblock order(来自 `/proc/pagetypeinfo`) |
| `disable_kapi` | 0400 | — | preflight:BTF 签名不符的符号禁用名单(逗号分隔) |
| `cma_reservoir_floor_mb` | 0600 | 512 | 非 CMA 可用量低于此则拒绝 flip |
| `acquire_mem_floor_mb` | 0600 | 512 | `MemAvailable` 低于此,sweep 刹车 |
| `cma_bypass_floor_mb` | 0600 | 256 | free CMA 低于此,movable→CMA hook 停止放行 |
| `acquire_drop_slab` | 0600 | 1 | USER run 开场丢一次可回收 slab |
| `refill_enable` | 0600 | 1 | 只 gate RELEASE 的 cheap+full;free 归还、precise、stage_in、EXPIRED 照常 |
| `debug` | 0644 | 0 | 每轮跑 `pool_check`,并开出 `purge_log` |
| `sim_cma_order` | 0400 | 0 | 测试用:在 S==1 设备上强制走 S>1 的簿记路径 |
| `vm_create_sym` / `vm_destroy_sym` / `vm_reclaim_sym` | 0400 | — | 覆写 kprobe 目标符号名 |
| `refill_delay_ms` | 0600 | 0 | **接受即忽略**——旧加载脚本兼容(拒绝会打断 insmod 行,`load.sh` 会跌到更低级梯) |

三个 preflight 值缺任一 → CMA 为 UNAVAILABLE;齐全也只到 PENDING——要 flip 仍得先
过验证。

### `load.sh` / `settings.prop` 契约

`package/module/load.sh` 是「这个模块怎么 insmod」的单一事实来源(开机路径与 app
的运行期重新激活都跑它),并以一串 insmod 阶梯降级,好让缺少某参数的旧 `.ko` 仍载
得起来。阶梯机制刻意仰赖严格内核会**拒绝**未知参数——所以本模块**绝不可定义名为
`pool_target` 的参数**(那是阶梯行里带的历史名)。

`settings.prop` 键:`pool_want`、`pool_want_with_cma`、`cma_movable_lever`
(`hook` | `flag` → insmod 时要武装哪个 lever 档)、`system_reserve_mb`(有设才传)、`cma_reservoir_floor_mb`(有设才传,挂在 v10
CMA 参数组而非新的顶层阶梯级——它是 v10 就有的参数;而且**必须走 insmod**,不能
事后写它的 0600 档:insmod 内的同步 prefill 已经在建储备池、每次翻牌都问这个
floor,事后才写晚了一整个开机。另注意参数自 v10 就存在,存在性**证明不了**本包
的 load.sh 会传它,app 要靠重载后 readback 比对或看 versionCode)、
以及开机按压三件组:`boot_acquire`(0–3,默认 0:模块在位后 `load.sh` 对 `acquire`
写入该 mode,遇 `-ENOSYS` 逐级降 3→2→1,失败不致命)、`boot_acquire_runs`(默认 1)、
`boot_acquire_wait`(秒,默认 0)。三者都是 **loader 政策不是模块参数**——按下去
就是一个 USER run,app 照 `refill_stat` 显示进度,`acquire=0` 可取消。这三个模块参数
是可写的(0600),**正因为模块对它们全盲**:写入改变不了任何行为,却让存下来的设置
在下次加载前就有地方看得见。模块会据以行动的一律只读——`system_reserve_mb` 定的表
只建一次,事后写入只会说谎。

这三件组是 temp-root 机种手上唯一的「按钮」。temp-root 的**软重启**只重启 Android
userspace、内核不落地,所以本脚本会在 .ko 还载着的情况下重跑、insmod 回 EEXIST——
因此按压的条件是**模块在位**而不是本次 insmod 成功:userspace 倒下、GUI 未起的
那一刻是模块能见到最干净的内存,也是重压唯一的窗口,赔给一个 EEXIST 就没了。
`runs` 让那个 USER run 按不只一次,前一次跑完才按下一次——一个 run 是单向单趟,
被自己 evict 出来、落在 sweep 游标后方的窗只有下一 run 捡得到;已达标的 run 会在
达标快查 O(1) 出场,所以多按不花钱。`wait` 是 `load.sh` 可以在 post-fs-data 阻塞
等它们的秒数,这才是真的把 zygote 挡在后面让 sweep 独占内存;要低于 root 管理器
的 post-fs-data 超时(Magisk 40s),没等完的照样在后台跑完。阻塞属于开机路径,
只有 `GH_BOOT=1` 的调用者(post-fs-data.sh)能花这个预算——app 的运行期「启用」
跑同一支脚本、照按同样的 run,但永不阻塞。默认(0 / 1 / 0)即原
行为;三个键住在 `/data` 的 `settings.prop`,每次软重启都会重新读取。

### `/dev/gh_pinprobe`

一支只读的「这段范围会不会过不了 `FOLL_LONGTERM` pin?」探针(`GH_PINPROBE_RANGE`、
`'P'` magic、`struct gh_pinprobe_range`),ABI 与原本在 `gh_unmovable.ko` 的版本
逐字节相同,crosvm 客户端不需改动。每次 gunyah 内存转移最后都是
`pin_user_pages_fast(FOLL_LONGTERM)`,而位于 CMA / isolate / ZONE_MOVABLE
pageblock 的页没办法这样 pin——内核会先把它迁走,迁不动时失败会发生在 hypervisor
调用深处。探针改问便宜的那个问题(「这里面有没有页落在需要迁移的 pageblock?」),
以 `FOLL_NOFAULT` 每 2MB 采样一次,绝不 fault 进任何页。这支探针归本模块是因为
CMA 状态本来就是它在管,运行期的 `MIGRATE_CMA` 值也只有它知道。

---

## 用法

```sh
# 带开机储备加载(开机时内存未碎片化,远比事后可靠)。
insmod gh_hugepage_reserve.ko pool_want=1024                    # 1024 × 2MB = 2GB

# 同上,另外开 2GB 储备池,没 VM 用时借给系统。
insmod gh_hugepage_reserve.ko pool_want=1024 pool_want_with_cma=2048 \
       migrate_cma_val=... pageblock_order_val=...              # 这两个由 load.sh 带入

# 事后主动补(模式 B:逐窗 evict),并观察进度。
echo 3 > .../parameters/acquire
while grep -q 'acquire_active=1' .../parameters/refill_stat; do sleep 1; done
grep -E 'pool_avail|pool_cma|acquire_stop_reason' .../parameters/refill_stat

# 用户 acquire 进行中要改目标:先取消,再写。
echo 0    > .../parameters/acquire
echo 2048 > .../parameters/pool_want
echo 3    > .../parameters/acquire

# 单独验证「零泄漏归还」(此时池只会从 VM 真的还回来的页补,永不新分配)。
insmod gh_hugepage_reserve.ko pool_want=256 refill_enable=0
# ... 开 VM、用一用、关机,等 ~10 秒 ...
cat .../parameters/refill_stat
cat .../parameters/reclaim_debug     # in_hook 应对得上归还量;reject/orphan ≈ 0
```

(其中 `...` 是 `/sys/module/gh_hugepage_reserve/parameters`)

---

## 现况

可 mock 测试的核心(pool 对象 + 两个 worker)已完成且 CI 全绿(30 场景 + 一支 TSan
race harness)。内核 backend(kapi / hooks / sysfs / unlock_cma / pinprobe + root)
对 android14-6.1 / android15-6.6 / android16-6.12 编译零警告,Tier-1 在 QEMU 上三个
内核全过(2026-08-28):kapi 全部解析成功、CMA verify 走到 VERIFIED(含一次真实的
DEFERRED 重试)、六个 hook 全挂上、场景 A–E 通过、卸载干净。

剩下的是 Tier-2:三台目标设备的真机回归(真的 `FOLL_LONGTERM`、厂商差异、kCFI),
以及两则 `NOTE(on-device)` 待确认项(owner 写锁的 irq 上下文;kapi 粗粒度 adapter
的注意事项)。
