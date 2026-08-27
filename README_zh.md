# gh_hugepage_reserve

[English](README.md) · **繁體中文** · [简体中文](README_cn.md)

Gunyah VM 的 2MB 大頁池核心模組。**`POOL_DESIGN.md` 是單一事實來源**——本文是導覽,
細節與依據(含實測數據)以該文件為準;文中 §n 均指向它的章節。

以可載入 `.ko` 發布(KernelSU / APatch / Magisk 友善)。支援 KMI:`android14-6.1`
… `android16-6.12`,含 6.6 廠商核心(如 SM8750);更舊的 KMI(5.10 / 5.15)只放
無功能 placeholder,讓 Magisk 模組仍裝得上。

---

### 免責聲明——風險自負

> 本模組從核心內部改動核心記憶體管理行為:它在執行期解析未匯出符號,並刻意遷移、
> 回收系統記憶體。與你的核心、廠商改過的 mm 不合,或單純運氣不好,都可能造成
> **當機、重開機、資料遺失、裝置記憶體耗盡,或任何我們沒預期到的後果**。它只在
> 少數裝置與核心上開發測試,其他一律是未知領域。不提供任何明示或默示的擔保——
> 作者不對你的裝置或資料的任何損害負責。安裝前請先確保有離線移除模組的手段
> (recovery / 安全模式 / `adb`),並先備份資料。

### 前置條件

客體 RAM 必須真的是 2MB shmem THP,否則根本沒有 order-9 配置可攔:

```sh
echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

以 Magisk/KernelSU 模組安裝時,這件事每次開機自動做
(`package/module/service.sh`,掛在 late_start 才蓋得過廠商預設)。DroidVM app
也會在每次開 VM 時設一次,而 crosvm 必須主動要求大頁。

---

## 用途

Gunyah 手機(demand-paging 客體)上,VM 的 guest RAM 以 2MB(order-9)為單位向核心
配置。系統開機一段時間後記憶體必然碎片化——實測 84% 的 2MB 視窗含無法遷移的
hard straggler(§前言)——屆時再想湊 2MB 連續頁已經來不及;**early-boot 是唯一能
拿滿的時機**。

本模組因此在開機時預先取得並長期監護一批 2MB 頁:

- **VM 要用時**:攔截 VM 的 2MB 配置改發池頁,VM 關閉後精確接回,下次開 VM 再用。
- **VM 沒在用時**:超出池份額的監護頁翻成 `MIGRATE_CMA` 儲備池借給系統
  (page cache / mTHP),不讓記憶體閒著;要用時再遷走借用者拿回。

兩個使用者目標值:`pool_want`(池份額,I1)與 `pool_want_with_cma`(總監護,I2)。
外部消費者是管理 app(DroidVM)、`load.sh` 與 `serve_test`,sysfs 介面是既存契約(§10)。

---

## 機制

### 粗略

只講功能,不含細節:

1. **insmod 同步建池**:module_init 在自己的上下文把池填到目標值才返回——
   載入腳本與 app 以「insmod 返回時池已建好」為前提。
2. **serve**:hook 攔截 VM 的 order-9 配置,從池發頁;池空或來源不對就放行原始配置。
3. **回收**:VM 放掉頁時 free hook 依 pfn 精確接回池;VM 關機後 release worker
   主動歸還借出頁,逾時(10s)仍拿不回的放棄追蹤。
4. **CMA 儲備池**:池份額之上的監護頁整塊翻成 CMA 借給系統;池短缺時從儲備池
   遷走借用者拿回。搭配 unlock_cma 旁路放寬一般 movable 配置進 CMA,儲備池才真的
   會被借用。
5. **sysfs 控制/觀測**:寫兩個 want 目標、按 `acquire` 主動採集(含全區 sweep + evict
   的重壓模式)、讀 `refill_stat`/`cma_usage` 等統計。

### 詳細

含細節與不變量。核心概念是 **custody(監護)**:一個 2MB 格屬於我們,或屬於系統,
整份設計只有這一條邊界。

**頁的狀態**(§1):每個 2MB 格恰處於下列之一——

| 狀態 | 意義 |
|---|---|
| `not_useable` | 非 RAM / carveout / 廠商 CMA / ZONE_MOVABLE,insmod 判定一次,永不參與 |
| `external` | 系統的(不在監護內) |
| `avail` | 池中待命 |
| `served` | 借給 VM(GUP pin + 模組一份保護參照) |
| `released` | VM 已放手、去向未定(等 free hook 接回或逾時放棄) |
| `cand` | collect_cma 組裝中,不可 serve/shed |
| `verify` | CMA 參數驗證窗暫存(單次呼叫內) |
| `cma` | 儲備池,借給系統 |

**帳目定義與不變量**(§1):

```
custody   = avail + served + released + cand + verify
held      = avail + served + released + verify
held_cma  = held + cma

I1  held     → pool_want               (收斂目標,非瞬間結構不變式)
I2  held_cma → effective pool_want_with_cma
W   恆有 pool_want <= pool_want_with_cma <= pool_size_max,或 with_cma == 0(CMA 停用);
    S>1 時兩者皆為 S 的倍數(S = 一個 CMA 塊含幾個 2MB 格)
P   0 <= pool_total <= configured_total <= pool_size_max
    (pool_total = 已實際取得、允許背景補回的「已證明容量」,不是 held 的別名)
U   CMA 整塊一致:同一 CMA 塊的 S 格要嘛全 CMA 要嘛全非 CMA
Q   最小化 custody 中不屬於 cma_able 塊的頁(碎塊最少化)
G   簿記一致:state / 串列 / 計數器互相吻合(debug=1 時 pool_check 驗證)
```

`held` 把 `released` 算進去是刻意的:released 的頁大概率幾微秒內就回來,不算它,
VM 關機瞬間 deficit 暴增,worker 會去買一堆「正要回家的頁」的替代品。
**放棄(purge)那一刻,deficit 才真的打開。**

**四支柱**(§前言):

1. **cma_able = custody 判定**:CMA 塊的全部子格都在監護範圍即成立。不存塊旗標,
   掃相鄰 S 格即時導出(S≤4 時同一條 cacheline)。
2. **cma_ready = 全部子格 AVAIL**:flip 進 CMA 的唯一合法對象。served 頁是
   `FOLL_LONGTERM` pin,含 pin 的塊翻進 CMA 是死路(GUP longterm 會先遷移再 pin)。
3. **升降級只在 custody 邊界**,全部在 worker(可睡)上下文;atomic 熱路徑最壞 O(S)。
4. **兩層定址、空洞免疫**:chunk 化的絕對座標表(`mem_section` 同款構造),
   ARM 實體位址大空洞只花頂層一個 NULL 指標。

**serve 細節**(§3.2/§4):kretprobe 攔 `__alloc_pages` 返回,四道快濾
(order ≠ 9 / 池空 / 非 owner / gfp 無 `__GFP_MOVABLE`)全過才進轉移。轉移在
pid_lock(read)→pool_lock 固定鎖序內一次完成 owner 驗證 + AVAIL→SERVED,離開 AVAIL 前
`get_page()`——這份保護參照讓外借期間 order-9 compound 不能 split/migrate。
取頁三階序:先碎塊(avail_non)→ 再 cma_able → 最後才拆 cma_ready 整塊。

**free return 細節**(§3.2):free hook(tracepoint)命中自家 RELEASED 頁後,
**在同一次 pool_lock 內**比較 held↔want、held_cma↔want_cma 決定 KEEP(重建 order-9
compound、轉 AVAIL、bypass 原始 free)或 DROP(轉 EXT,讓原 free 繼續進 buddy)。
拆成 query + edge 兩次呼叫會有 TOCTOU,所以是單一事件 api。

**release 細節**(§6):release worker 逐 SERVED 三分類——`refcount==1 && !mapping`
→ 先標 RELEASED 再鎖外 `put_page`;`refcount>1` → pin 未放,下輪再看;
`page_count==0` → orphan(hook 漏接),直接轉 RELEASED 交給既有的 sweep/purge 出路。
**放手 = pid 死亡事件 + 3 秒寬限**(owner 活著——不論 vm_count——一律不放手;
vm_count 只做 serve 的 gate;寬限讓 exit 清算的 free 走回收而非放手,時鐘只綁
死亡、永不清除);`released_at + GRACE=10s` 只管 RELEASED 的 purge。

**CMA 儲備池細節**(§3.2):

- flip(avail→cma)只取 cma_ready 整塊,前置是 `cma_params_state == VERIFIED`——
  第一次 flip 前由 `pool_verify_cma_params()` 用完整持有的驗證窗實地驗
  pageblock_order 邊界、CmaFree delta、CMA-mode grab,失敗即終態 UNAVAILABLE。
- stage_in(cma→avail)與 drop(cma→ext)都要先整塊 contig grab 遷走借用者,
  **會失敗**(pin/writeback);失敗塊本 run 跳過、保留 CMA 標籤並**繼續計入
  pool_cma**——帳與標籤必須一致,**絕不「只翻標籤」**(free 頁還躺在 CMA freelist,
  翻了就是 CmaFree 帳漂移 + 頁對 unmovable 隱形)。
- acquire 的 gfp 鐵律:GFP_KERNEL 系、禁 movable/`__GFP_CMA`——否則 unlock_cma
  放寬後會從自家儲備池撈頁,且 movable 頁會在 FOLL_LONGTERM pin 前被 GUP 遷走。

**並行契約**(§2,race 安全的結構性理由):

- **單呼叫原子、無跨呼叫不變式**:每個 pool api 在 pool_lock 內原子完成簿記,
  不存在需要事後修復的結構,ABA 一族沒有寄生點。
- **api 自衛**:呼叫者的條件檢查全是鎖外建議性的;每個 public api 鎖內重驗前置,
  不成立就 no-op,絕不信任呼叫者。
- **清單 = pfn 值,消費點重驗**:掃描集存 pfn 值不存指標;slot 表靜態、memmap 恆在,
  懸空在結構上不可能,會發生的只有過期,由消費點鎖內重驗仲裁。
- **迭代 = 索引序表掃,不跟連結**:「游標指的 next 被並行搬走」這個問題類別不存在。
- 兩條硬紀律:(a) **鎖內禁 put_page/__free_pages**(free tracepoint 會重入 pool_lock);
  (b) **先改 state 再放手**(release 先標 RELEASED 再 put,shed 先標 EXT 再 free,
  順序反了各是一種資料遺失)。

---

## 架構

### 總架構

三層:**consumer / worker / pool api + kapi**。consumer 依「呼叫目標」分組,
每組只碰一層;完整圖與邊的說明見 §0.1。

```
consumer 層(依呼叫目標分組;insmod/rmmod 例外——觸碰全層)
┌────────────────────────┐ ┌───────────────────┐ ┌───────────────────┐ ┌────────────┐
│ -> pool api            │ │ -> release worker │ │ -> adjust worker  │ │ -> kapi    │
│ serve hook (atomic)    │ │ vm_shutdown       │ │ sysfs want shrink │ │ unlock_cma │
│ free hook (atomic)     │ │ vm_unshare        │ │ sysfs acquire / 0 │ │ pinprobe   │
│ sysfs 讀查詢, vm_boot  │ │ pid-exit hooks    │ │                   │ │ (旁路)     │
└───────────┬────────────┘ └─────────┬─────────┘ └─────────┬─────────┘ └──────┬─────┘
            │                        ▼                     ▼                  │
            │            ┌───────────────────────┐   ┌────────────────────┐  │
            │            │ release_worker (1s×5) │──►│ adjust_worker      │  │
            │            │ served→released,      │   │ (10ms×MAX)         │  │
            │            │ 死亡放手, owner GC    │   │ 7-phase pipeline   │  │
            │            └───────────┬───────────┘   └─────────┬──────────┘  │
            ▼                        ▼                         ▼             │
┌─────────────────────────────────────────────────────────────────────┐      │
│ pool api   public: pool_{src}_to_{dst}(一條邊一個函數)+ 查詢/action │      │
│            private: pool_private_{slot,classify,promote,demote,...} │      │
└──────────────────────────────────┬──────────────────────────────────┘      │
                                   ▼                                         │
┌─────────────────────────────────────────────────────────────────────┐      │
│ kapi   alloc_pages / alloc_contig_range / set_pageblock_migratetype │◄─────┘
│        walk_system_ram_range / evict_range / drop_slab / drain / …  │
└─────────────────────────────────────────────────────────────────────┘
```

要點:

- **hook 組只碰 pool api**,不做政策;**worker 是狀態機唯二的驅動者**,所有轉換都經
  public api;release 每輪 `adjust_try(RELEASE)` 觸發 adjust(背景補頁走這條邊)。
- **核心符號只活在 kapi 一層**;unlock_cma 是旁路,只用 kapi,不碰 pool/worker/狀態機(§9)。
- **insmod/rmmod 是唯二觸碰全層的 consumer**:kapi 解析、slot 表建置、hook 裝卸、
  worker 啟停、同步清理,固定序見 §8(卸載鐵律:先卸 hook → 停 worker → 交還參照)。

建置是 unity build:`gh_hugepage_reserve.c` 依序 include `parts/`。pool 物件與兩個
worker 環境無關,同一份原始碼也在 userspace 對 `tests/shim.h` 編譯——CI mock harness
(§3.3)以決定性 fake buddy 重放 34 個情境;kapi/hooks/sysfs/unlock_cma 只在真機驗證。

### kernel api(kapi)

`parts/gh_kapi.c.inc`(契約見 §3.3)。**全模組唯一碰核心符號的層**,對上提供一張
粗粒度 adapter 表 `gh_kapi`:

```c
bool kapi.cap(feature);                        /* CONTIG_RANGE / DROP_SLAB / ... */
struct page *kapi.alloc_try(order, strong);
int  kapi.contig_range(start, end, noretry);   /* 半開 pfn 範圍 */
bool kapi.candidate_range(start, end);         /* 純讀可行性探針 */
void kapi.evict_range(mode, start, end);
bool kapi.cma_floor_ok(nblocks);
void kapi.drop_slab(); kapi.drain_pages(); kapi.lru_add_drain_all();
/* … 完整表見 parts/gh_defs.h struct gh_kapi */
```

設計規則:

- **kapi 是模組自己的穩定 ABI**:呼叫端一律用正規化的 `kapi.op()`;逐核心版本分歧的
  真實符號(名稱、簽章、參數/回傳語意)活在私有 `kraw` 結構 + shim 裡,不外洩。
- **解析在載入時**(throwaway kprobe 查位址),缺就留 NULL、對應 `cap()` 回 false——
  整個 feature 前置拒絕,不會走到一半經錯型指標呼叫(kCFI type-id 不符 = panic)。
- **ABI guard**:`load.sh` 的 `kapi_check` 讀 `/sys/kernel/btf` 預檢簽章,不相容的
  符號經 `disable_kapi` 參數列入禁用名單;`abi/kapi_abi.tsv` 是符號登記表。
- **folio 旗標判讀只活在這裡**(candidate/evict 的核心實作);pool 物件對頁的直接
  觸碰限定在一小組可重定義原語,test build 以 shim 換掉它們——這就是整層 kapi
  可以被 mock backend 替換、pool/worker 能在 userspace 跑 CI 的前提。

### pool api

`parts/gh_pool.c.inc`。**pool 就是那個物件**(C 沒有物件,pool api 即物件):slot 表、
三條 avail 串列、計數器、游標、掃描集與它們的鎖**全部在 pool 內**;hook/worker/sysfs
只呼叫 public api、拿回值(pfn、計數、bool),從不持 pool_lock、從不看 `top[]`/slot。

命名規約(§3):public 是 `pool_{src}_to_{dst}`,**一條邊一個函數**,同一條邊的變體用
mode 參數;private 是 `pool_private_xxx`,只准 pool 內部呼叫。每個 public api 鎖內
重驗自己的前置條件,不成立就 no-op。

狀態機(一條邊 = 一個 public api,§3.1):

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

*  = custody 出口(塊降級點)     ! = 此邊存在會失敗的 api
```

| 邊 | api | 呼叫者 |
|---|---|---|
| acquire(ext→avail) | `pool_ext_to_avail(how, …)`,五種取法是 mode | adjust |
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
| promote(cand→avail) | 無 public——`pool_private_promote` 內部執行 | — |

內部結構(§0.2):兩層定址 `top[off>>30][(off>>21)&511]`(O(1) 最壞,空洞 = NULL
指標);串列只維護 avail 三條(`avail_non` / `avail_cma_able` / `avail_cma_ready`)——
唯一需要 atomic「給我下一個可用的」的消費者是 serve;其他狀態一律索引序表掃或
O(1) 門檻標量(released 的 `(count, oldest)`)。塊屬性 cma_able/cma_ready 不存欄位,
掃相鄰 S 格即時導出。執行期零 kmalloc,所有掃描集 insmod 預配。

owner 註冊表同屬 pool facade,用獨立 `pid_lock`(rwlock):entry 活到 pid 死亡,
serve 熱路徑先 lockless `pool_owner_maybe()` 快濾,命中才按 pid→pool 固定鎖序轉移。

### consumer

#### hook

`parts/gh_hooks.c.inc`(§4)。原則:**每個 hook 只做「一個 pool/facade 呼叫」,
不做政策**;政策(保留/放棄、目標比較)封裝在被呼叫的 api 裡。

| hook | 掛載點 | 動作 |
|---|---|---|
| serve | kretprobe `__alloc_pages` 返回 | 四道快濾全過 → `pool_avail_to_served(pid)`,NULL 則放行原配置 |
| free | tracepoint `android_vh_free_one_page_bypass` | 只認 order-9 → `pool_released_return(page)`,KEEP 時設 bypass 攔下 |
| vm_boot | kprobe(`GH_CREATE_VM` ioctl 路徑) | `pool_owner_add(current)` |
| vm_shutdown / unshare | kprobe(vendor 符號,可用參數覆寫) | `release_vm_shutdown/unshare(current)`(facade 內更新 owner + arm release) |
| pid 死亡 | tracepoint `sched_process_exit`(core,GKI 必有) | group_dead 且在 owner 表 → `release_vm_exit(current)` |

serve 的四道快濾依成本排序:order ≠ 2MB → 池空 → 非 owner(lockless)→
gfp 無 `__GFP_MOVABLE`(實測:dma_heap 的 unmovable order-9 永不回 buddy,
不濾每 VM 偷走 2 頁)。serve/free 都在 **atomic** 上下文,只碰 O(1)~O(S) 的
pool api;`hook_enable` / `reclaim_enable` 可個別開關,讀回實際掛載狀態。

#### worker

`parts/gh_release.c.inc` / `parts/gh_adjust.c.inc`(§5–§7)。hook 在 atomic 上下文,
只能做 O(1)~O(S) 的快動作;所有慢動作——向系統要頁、遷移、掃全表、驗證——都需要
可睡上下文,全部集中在這兩個 worker。它們是狀態機唯二的驅動者。

**共同模型:run 計數器**(§5)

worker 不是常駐執行緒,而是 workqueue 上的 delayed_work:醒來 → 做一小片工作 →
存進度 → 排下一次醒來。切小片的理由:pool_lock 只在片內短暫持有,serve/free hook
永遠不會被 worker 的慢動作卡住。

ctx 裡的 `run` 同時是**開關**和**看門狗**:每輪先 `run--`,歸零就收尾;任何人隨時
可寫——寫 0 = 叫停(最多再跑完當輪),寫 N = 啟動或續命。ctx 的其餘欄位從 init 到
finish 只有 worker 自己能改;外人要換工作內容一律**交棒**(`next_profile`),
絕不當場改 ctx——worker 可能正在鎖外做慢動作,輪尾還會把自己的 ctx 存回來。

**release_worker——「VM 放手的頁,收回來」**(每秒一輪)

三個 VM 事件(vm_shutdown / vm_unshare / pid 死亡)各 arm 5 輪;事件 facade 同時
更新 owner 表(vm_count 歸因遞減——純 serve gate、死亡標記)。5 輪是
保證下限,不是上限。每輪做五件事:

1. **收頁(served→released)**:索引掃全表找 SERVED,逐頁看 refcount 三分類——
   - `refcount==1`(只剩模組自己那份保護參照)= VM 真的放手了 → 鎖內先標
     RELEASED、蓋時戳,再鎖外 `put_page`。put 之後頁走一般 free 路徑,流過
     free hook 時由 `pool_released_return` 決定接回或放行。「先標再放」是硬紀律:
     順序反了 free hook 會不認得這頁,頁就丟了。
   - `refcount>1` = VM/Gunyah 的 pin 還沒放 → 這輪不碰,下輪再看。
   - `page_count==0` = **orphan**:free 已經發生但 hook 漏接(pcp-lag 或掛載空窗),
     模組那份參照已隨 free 蒸發 → 不 put,直接標 RELEASED,交給既有的
     sweep(搶回)/ purge(放棄)出路,不開新路徑。
2. **死亡放手(served→ext EXPIRED)**:逐 SERVED 查 owner 表——owner **活著就
   一律不碰**(不論 vm_count;vm_count 只是 serve 的 gate);owner 已死(標記)
   或已不在表 = 放棄追蹤:標 EXT、放掉自己的參照、記 purge_log 與
   `owner.abandoned++`。從此頁跟著還 pin 它的人活,與模組無關。放手 = pid 死亡
   **事件 + DEATH_GRACE(3s)寬限**(死亡才啟動、永不清除的單向時鐘——舊制的
   idle+10s 會與 VM 重開 1→0→1 競態;寬限讓 exit 清算的 free 走回收)。
3. **第 3 輪 `drain_pages`**:order-9 的 free 會先停在 per-CPU 快取(pcp),
   不立刻流過 hook——實測每 VM 漏 ~2 頁。第 3 輪(≈3 秒,關機 free 潮已退)
   drain 一次把它們逼出來。
4. **提早收工 / final**:「沒有頁還可能回來(released==0)、也沒有頁在等
   unpin(pending==0)」→ run 歸 0 提早結束。final 輪再 drain、再收一次,並做
   owner GC——只清「已死且名下無頁」的 entry;vm_count==0 但 pid 還活著的**保留**,
   app 要看得到「關了 VM 卻不還頁」的可疑 pid(D-state 偵測,§10)。
5. **叫醒 adjust**:每輪 `adjust_try(RELEASE)`。等待期間 held 仍含 served/released,
   帳面沒缺口,adjust 實際只有 precise 有事做;等 DROP/EXPIRED 真的打開 deficit,
   cheap/full/stage_in 才動起來——時序由條件系統自己排對,不需要兩套 profile。

**自我續命**:輪次數完但 pending>0(還有頁在等 unpin/寬限)→ `run=1` 續一輪。
有界:寬限至多 10 秒後由 EXPIRED 收走,RELEASED 由 purge 收斂,不會無限續。

設計核心:**放手是事件+短寬限(pid 死亡 + 3s),purge 是時鐘(released_at+10s),
輪次只是載具**——所以 `manual_release` 高頻觸發、多個事件重複 arm,都只是多幾次
無害的掃描,不會提早任何放手或 purge。

**adjust_worker——「把池調到目標值」**(每 10ms 一輪)

誰觸發,決定拿到多痛的授權(profile,§5.1):

| profile | 觸發時機 | 授權的取頁力度 |
|---|---|---|
| `INSMOD` | 開機,module_init 同步驅動到完成 | cheap + full |
| `RELEASE` | release worker 每輪 | precise + stage_in(+ cheap + full,受 `refill_enable`) |
| `SHRINK` | want 目標調小 | 只有 stage_in |
| `USER` | 使用者按 `acquire` | 全部(唯一含 main) |

profile 只決定 ACQUIRE phase 內開哪些力度(**政策**);七個 phase 骨架(**機制**)
不受 profile 管,每次 run 都依序走完。RELEASE 的目標快照另被 cap 到
`pool_total`(已證明容量)——背景補回只補「歷史上真的拿到過」的量,使用者把 knob
寫大不會偷偷授權背景 reclaim。

**一次 run = 單趟 pipeline,不迴圈**:每個 phase 跑到自然完成才前進(可跨很多輪,
每輪只做一小片),走完即收工;沒達標 = partial,記 stop_reason,重試屬於下一個
trigger。acquire 在一次 run 裡只有一次機會——後段 shed 挖開的缺口不會回頭觸發前段
再抓,「抓 → 湊不成 → 丟 → 再抓」的振盪在結構上不存在。

每輪開場固定動作:先做 released 逾時 purge(O(1) 門檻擋住,過 GRACE 才真掃——
不等 phase 游標繞回來,它可能在 main 停很久);然後**重算五個數字,絕不快取**
(池況隨時被 hook 改變):

```
held           = avail + served + released     現在監護多少
diff_want      = want − held                   池份額缺(+)/多(−)多少
new_page_need  = want_cma − (held + cma)       總監護還缺多少 → 才需要向系統拿
reserve_target = want_cma − want               儲備池應有的大小
cma_excess     = cma − reserve_target          儲備池超額多少
```

七個 phase,依序:

1. **PREPARE(備料)**:五個數字全達標 → 直接收工(空跑 O(1) 出場)。USER run
   且有缺口才付一次 `drop_slab` + pcp drain(dentry/inode 快取不在 LRU,任何採集
   都收不回它,但一頁 dentry 就毒死一個 2MB 窗;丟出來的 order-0 落在 pcp,
   不 drain 合不進 buddy 高階,剛開的窗對後續採集隱形)。一趟走表建好本 run
   的掃描集:precise 清單
   (自己 released 還躺在 buddy 的頁)與 main 清單(全部 EXT 窗:缺口優先、
   從上次游標接續繞一圈)。儲備池有超額才付一次 CMA 佔用探針 + bucket 排序
   (搬家成本低的塊排前面)。
2. **ACQUIRE(取頁)**——唯一受 profile 管的 phase。五級力度依「誰會痛」遞增,
   一級做完或做不動才降到下一級:
   - **precise**:contig 抓回自己的 released 頁(它們還躺在 buddy 沒人用)。
     免費,誰都不痛,永遠第一。
   - **cheap**:向 buddy「撿現成」(NORETRY,拿不到立刻放棄)。先試**整塊**
     (一次 S 個 2MB,天生可翻儲備)——開機大塊充裕,池 + 儲備池就是這樣一次
     建成的;整塊敗一次就永久降到單頁,本 run 不回頭(大塊耗盡不會自己長回來)。
   - **stage_in(cma→avail)**:池短、但總監護夠(頁都在儲備池)→ 從自己儲備池
     整塊拿回,把借用者(page cache 等)遷出去。中痛,只痛自己人。會失敗
     (借用者被 pin);血量 `cta_hp` 控制:有進展回滿 8,連續零進展 8 批放棄。
   - **full**:alloc_pages RETRY_MAYFAIL——讓核心自己 compaction + reclaim
     (可能 swap)。比 cheap 重、比 main 輕;核心自管,不需使用者授權。失敗即停:
     核心已經盡力,再問也是同一個答案。
   - **main**:重壓,只有 USER。兩種打法:`acquire=1` 是 CONTIG_ANY(讓核心自選
     位置盲拿;血量 `main_hp` 成功 +3、連敗 −1,歸零 = "migration exhausted");
     `acquire=2/3` 是 CONTIG_AT **整區 sweep**——沿 main 清單一輪一窗:先純讀
     512 個 struct page 做可行性判定,**不過閘就不 evict**(「never white-kick」:
     實測 84% 的窗有搬不走的 straggler,沒這個閘就是對 84% 的窗白丟 page cache);
     過閘先試便宜的 async 抓,不行才 evict(模式 A:每 8 個失敗窗做一次全系統
     memcg reclaim;模式 B:只逐出該窗自己的 folio)再 sync 抓一次,成敗都往前走。
     每窗前查 `mem_available`,低於 `acquire_mem_floor_mb`(512MB)立即剎車
     (實測不剎會 RCU stall 到重開機)。游標「先前進再做事」:run 被中斷,
     下個 run 從斷點之後接續,不重試同一批。
   - **輪尾 eager flip**:不分力度,每輪順手檢查——池份額已滿、儲備未滿、有整塊
     ready → 翻 1 塊進 CMA。早翻早好:avail 頁對系統完全不可用,cma 頁至少可被借用。
3. **VERIFY_CMA(驗證)**:CMA 參數還是 PENDING 且是 INSMOD/USER run → 用完整持有
   的驗證窗實地驗一次(pageblock 邊界、CmaFree 帳目、CMA-mode 抓回)。過了才進
   VERIFIED、才准 flip;結構性失敗 = 永久停用 CMA,當輪折回純池模式。
4. **COLLECT_CMA(湊塊)**:授權跟 main 走。對「差幾格就湊滿」的塊抓缺口窗
   (同樣過可行性閘、可 evict),抓到的先進 cand 隔離區**不入池**——直接入池會把
   held 頂超過 want,叫醒 shed 拆自己的台。湊滿整塊 → promote:cand 轉 AVAIL、
   塊內兄弟歸隊,同時守恆 free 掉等量碎頁(held 不變、Q 品質變好)。run 結束時
   湊不成的 cand 全部 flush 還給系統(它的塊本來就湊不齊,留著沒意義)。
5. **AVAIL_TO_CMA(正式翻)**:把超出池份額的 ready 塊批次(每輪 ≤8 塊)翻滿到
   `reserve_target`。每翻先問 `cma_floor_ok`——每翻一塊,unmovable 工作集
   (~3.5GB,進不了 CMA)的活動空間就少 2MB×S,低於 floor(預設 1024MB)不翻。
   本 phase 翻過就 drain 一次,讓 CmaFree 帳目跟上(GUI 讀它)。
6. **CMA_FREE(縮儲備)**:want_cma 調小 → 超額的儲備塊還系統。每塊:整塊搶回
   (遷走借用者,**會失敗**)→ 全塊在手才翻標籤回 MOVABLE → free。搶不到的塊
   保留 CMA 標籤、繼續計入帳(帳與標籤必須一致,絕不只翻標籤);血量 `drop_hp`
   歸零 = "cma sources stuck",下個 trigger 重新探針再試。例外讓路:池還短且本
   run 還有 stage_in 授權 → 跳過此 phase,超額留給 stage_in 變現,不放生。
7. **AVAIL_FREE(還多餘)**:最後,真正多餘的 avail 頁(每輪 ≤32)還給系統。
   avail 已空但帳面還超(多餘的頁全在 VM 手上)→ 不等,直接收工——它們回來時
   free hook 的 DROP gate 會按當下目標放行,或由下個 trigger 的 run 來 shed;
   沒有這個出口,這輪會空轉到看門狗上限(20 分鐘)。

**搶占與交棒**(`adjust_try`):沒 run 在跑 → 立即開跑。USER run 在跑 → 誰都不能搶
(使用者按的最大;再按一次回 -EBUSY,要先 `acquire=0` 取消)。背景 run 在跑 →
新觸發者把 run 寫 0(中斷)並登記 `next_profile`(取優先序最高:USER > SHRINK >
RELEASE > INSMOD),自己立即返回;worker 在收尾時看到 next_profile 就以新 profile
重新開跑。交棒延遲 ≤ 一輪(10ms),ctx 從頭到尾只有 worker 自己寫。

**收尾**(`adjust_finish`):任何 run 歸 0 的路徑都走同一條,無分支——湊不成的
cand 還給 buddy、run-local 掃描集作廢、USER run 記 stop_reason(app 逐字顯示:
reached target / migration exhausted / low-memory floor / …)、有 next_profile
就交棒。`ADJUST_RUN_MAX=120000`(×10ms ≈ 20 分鐘)是看門狗不是迴圈長度:
正常 run 在 pipeline 走完時就自然結束。

---

## 原始碼佈局、建置與測試

| 檔案 | 層 | 測試 |
|---|---|---|
| `parts/gh_defs.h` | 共用型別、kapi adapter 契約、enum | — |
| `parts/gh_owner.c.inc` | owner 註冊表(entry 活到 pid 死亡;vm_count 純 serve gate) | mock |
| `parts/gh_pool.c.inc` | **pool 物件**:所有狀態與鎖都住這裡 | mock |
| `parts/gh_release.c.inc` | release worker(served→released、逾時放手) | mock |
| `parts/gh_adjust.c.inc` | adjust worker(七 phase 的取頁/翻轉 pipeline) | mock |
| `parts/gh_kernel_env.h` | 核心原語黏合層(鎖、時鐘、頁操作) | 真機 |
| `parts/gh_kapi.c.inc` | 核心 kapi backend(符號解析、版本 shim) | 真機 |
| `parts/gh_hooks.c.inc` | kretprobe / kprobe / tracepoint 掛載 | 真機 |
| `parts/gh_sysfs.c.inc` | sysfs / 模組參數(對外 ABI,§10) | 真機 |
| `parts/gh_unlock_cma.c.inc` | movable→CMA 旁路 lever(§9) | 真機 |
| `parts/gh_pinprobe.c.inc` | `/dev/gh_pinprobe` 唯讀 pin 可行性探針 | 真機 |
| `tests/` | mock harness:shim、fake buddy、情境 | CI |
| `tier1/` | QEMU 整合測試台(companion `.ko` + exerciser) | CI |

```sh
make test              # Tier-0:userspace mock harness(ASan+UBSan,再跑 TSan);不需核心
make module KDIR=...   # 核心 .ko(對 GKI build tree)
make -C tier1 ...      # Tier-1:QEMU 整合台(見 tier1/README.md)
./build.sh             # Docker 內建全部 KMI + 打包 Magisk/KernelSU 模組
```

測試分三層。**Tier 0**(`tests/`,本 repo 的 CI)在沒有核心的情況下驗證池邏輯:
`make test` 編 `tests/harness.c`,以決定性 fake buddy 重放 **34 個情境**——
serve/歸還、逾時放手 + D-state、orphan/purge、S=2 塊分類、CMA 儲備池
(flip/stage-in/drop/verify,含 pageblock-order 差一級)、全區 sweep 含 evict、
shrink、profile 搶占/交棒、hook 缺席(temp-root)的 release→sweep 迴路,外加一支
TSan race harness。**Tier 1**(`tier1/`,QEMU,本地跑)編出真的 `.ko`,在**人為製造的**
碎片化下對真實頁觸發 kprobe/tracepoint——不需裝置、不需 Gunyah。**Tier 2** 是真機
(真的 `FOLL_LONGTERM`、真的碎片化、廠商差異)。

---

## sysfs 與參數對照

全部位於 `/sys/module/gh_hugepage_reserve/parameters/`。檔名、格式與錯誤碼是與三個
外部消費者的**契約**(§10):管理 app(逐行輪詢 `refill_stat`、寫兩個 want、按
`acquire`、統計讀取前寫 `reconcile`(一份 app 通吃兩代:舊版模組必要、v12 為
no-op)、比對 `stop_reason` 字串、用 `cma_usage` 畫圖)、
`load.sh`(preflight 參數與 fallback 階梯)、`serve_test`。

### 目標與控制

| 檔案 | 權限 | 用途 |
|---|---|---|
| `pool_want` | 0600 | 池份額目標(2MB 頁數) |
| `pool_want_with_cma` | 0600 | 含儲備池的總監護目標;`0` = 停用 CMA |
| `acquire` | 0200 | `0` 中斷 / `1` CONTIG_ANY / `2` sweep + 全系統 reclaim(模式 A)/ `3` sweep + 逐窗 evict(模式 B) |
| `reconcile` | 0200 | 舊版相容 no-op——統計已即時,無帳可對 |
| `manual_release` | 0200 | 寫 `1` → 多跑一輪 release;寫再頻繁也不會提早放手 |
| `manual_refill` | 0200 | 寫 `1` → `adjust_try(RELEASE)` |
| `hook_enable` | 0600 | serve kretprobe 開關;讀回**實際**掛載狀態 |
| `reclaim_enable` | 0600 | free tracepoint 開關;讀回實際掛載狀態 |

寫入規則(§4):兩個 want 都先 clamp 到 `pool_size_max`,CMA 可用且 S>1 時向上對齊
S 的倍數;coupling 維持 `want <= want_cma`(寫大的 want 會拉高 with_cma;寫低於
want 的 with_cma 會被抬到 want,**絕不反向縮 want**)。任一值調小 → clamp
`pool_total` 並非同步觸發 `adjust_try(SHRINK)`;只**調大**則只記錄——還是要有人按
`acquire`。

錯誤碼:`-EINVAL`(值不合法)、`-ENODEV`(insmod 未完成)、`-EBUSY`(want:
**任何** adjust run 進行中;`acquire`:USER run 進行中)、`-ENOSYS`(必要符號沒
解析到——`acquire`:1 需 contig_pages、2 需 contig_range、3 另需
folio_isolate_lru + reclaim_pages;`pool_want_with_cma`:CMA 為 UNAVAILABLE)。
run 期間寫 want 回 `-EBUSY` 是**故意的,不是防禦 code**:每個 run 從啟動快照起
追固定目標,要改只有 `acquire=0`(停掉在跑的 run)→ 寫新值 → 重新按 `acquire`
這一條路;`refill_stat` 的 `acquire_active=1` 就是「現在寫會被擋」的訊號。

### 觀測

| 檔案 | 權限 | 內容 |
|---|---|---|
| `refill_stat` | 0400 | 17 行 `key=value`(見下) |
| `pool_avail` | 0400 | 池中待命頁數 |
| `pool_cma` | 0400 | 儲備池大小(2MB 頁等量) |
| `pool_avail_cma_able` | 0400 | 屬於 cma_able 塊的 avail 頁(非 VERIFIED 恆 0) |
| `pool_size_max` | 0400 | 由 RAM 導出的兩個 want 上限 |
| `reclaim_debug` | 0400 | `o9_seen`、`del_hit`、`del_miss`、`gate_drop`、`skip_unmovable`、`reject`、`orphan`、`purged`、`in_hook`、`in_sweep`、`in_cma`、`in_user`、`in_refill` |
| `vm_owners` | 0400 | 逐行 pid / vm_count / served / abandoned / comm |
| `served_summary` | 0400 | tracked / live / orphan 統計,release 輪次順手算 |
| `purge_log` | 0400 | 放棄頁的 pfn / refcount / 現況(需 `debug=1`) |
| `cma_usage` | 0400 | 儲備池佔用(free/anon/file MB、塊三態),~1s 快取 |

`refill_stat` 欄位:`state`、`pool_avail`、`pool_total`(已證明容量,§5.1)、
`served`(= served + released)、`pool_want`、`total_served`、`total_refilled`、
`active_vms`、`acquire_active`、`acquire_mode`、`acquire_stop_reason`、
`refill_enable`、`free_reclaim`(free hook 的真實狀態)、`pool_want_with_cma`、
`pool_cma`、`pool_avail_cma_able`、`cma_pb_order`。`acquire_active` 反映 adjust
worker 是否有 run 在跑(**任何 profile**,含 release 觸發的背景 refill 與
SHRINK),=1 同時表示寫 want 會 -EBUSY、CMA 旁路暫停借出;`acquire_mode` /
`acquire_stop_reason` **只反映 USER run**,背景 run 不會蓋掉。`cma_pb_order=-1`
表示這次開機整個 CMA 側是關的(符號/preflight 值缺,或驗證失敗)。

`acquire_stop_reason` 是固定字串集,app 逐字比對:`idle` / `acquiring` /
`already at target` / `pool capacity full` / `cma headroom floor` /
`cma flip failed (systemic)` / `stopped by user` / `reached target` /
`reached target,with_cma` / `migration exhausted` / `cma sources exhausted` /
`scanned all present memory` / `low-memory floor` / `evict-B unavailable` /
`quality converged` / `cma sources stuck`。

`vm_owners` 同時是卡死偵測器。`abandoned` = **pid 死亡時**仍持有而被放手的頁數;
活著的 pid 恆 0(不再有 abandon-while-alive)。活 pid 的卡死訊號改看:vm_count==0
而 served>0 持續——crosvm 收尾偏慢或卡住,頁仍在 SERVED 帳上;app 可讀
`/proc/<pid>/stat`,見 D state 即提示。

### movable→CMA lever(§9)

| 檔案 | 權限 | 用途 |
|---|---|---|
| `moveable_to_cma_vender_already_allowed` | 0400 | `1` = 廠商核心本來就放行 movable→CMA,寫 lever 是 no-op |
| `moveable_to_cma_gfp_cma_hook` | 0600 | 外科式,優先:vendor hook 對單純 movable 請求 OR 上 `ALLOC_CMA` |
| `moveable_to_cma_restrict_cma_redirect_disabled` | 0600 | 全域:翻掉核心的 `restrict_cma_redirect` static key;`1` = movable 可以進去 |

沒有這塊,儲備池就是「監護著、沒人借得到」:原廠核心只把帶 `__GFP_CMA` 的配置導向
CMA freelist,而 app 真正的工作集(page cache、mTHP anon)不帶這個旗標。兩個 lever
預設關,前置是儲備池已建好且採集靜止(prefill 收工、adjust worker **無任何 run**
在跑——任何 profile 都可能翻動 CMA 區,不只使用者按的 acquire),且 free CMA 低於
`cma_bypass_floor_mb` 就不放行。

### 模組參數

| 參數 | 權限 | 預設 | 用途 |
|---|---|---|---|
| `pool_want` / `pool_want_with_cma` | 0600 | 0 | 同上;insmod 時亦可給 |
| `system_reserve_mb` | 0400 | 6144(下限 64) | 算 `pool_size_max = min(ram − min(ram/2, system_reserve_mb), 表上限)` 時留給系統的量 |
| `system_reserve_mb_default` | 0400 | init 時算出 | **本機**的預設保留量 `min(RAM/2, 6144)`,不是那個內建常數——`RAM/2` 上限讓兩者在小 RAM 機上不同(8GB:預設 6144 實際只留 4096,設成 4096 以上都是 no-op)。設定值會覆蓋 `system_reserve_mb`,所以這是預設值唯一還留著的地方;app 拿它當「調低保留量」的警告門檻 |
| `migrate_cma_val` | 0400 | −1 | preflight:執行期的 `MIGRATE_CMA` 值(來自 BTF) |
| `pageblock_order_val` | 0400 | −1 | preflight:pageblock order(來自 `/proc/pagetypeinfo`) |
| `disable_kapi` | 0400 | — | preflight:BTF 簽章不符的符號禁用名單(逗號分隔) |
| `cma_reservoir_floor_mb` | 0600 | 512 | 非 CMA 可用量低於此則拒絕 flip |
| `acquire_mem_floor_mb` | 0600 | 512 | `MemAvailable` 低於此,sweep 剎車 |
| `cma_bypass_floor_mb` | 0600 | 256 | free CMA 低於此,movable→CMA hook 停止放行 |
| `acquire_drop_slab` | 0600 | 1 | USER run 開場丟一次可回收 slab |
| `refill_enable` | 0600 | 1 | 只 gate RELEASE 的 cheap+full;free 歸還、precise、stage_in、EXPIRED 照常 |
| `debug` | 0644 | 0 | 每輪跑 `pool_check`,並開出 `purge_log` |
| `sim_cma_order` | 0400 | 0 | 測試用:在 S==1 裝置上強制走 S>1 的簿記路徑 |
| `vm_create_sym` / `vm_destroy_sym` / `vm_reclaim_sym` | 0400 | — | 覆寫 kprobe 目標符號名 |
| `refill_delay_ms` | 0600 | 0 | **接受即忽略**——舊載入腳本相容(拒絕會打斷 insmod 行,`load.sh` 會跌到更低階梯) |

三個 preflight 值缺任一 → CMA 為 UNAVAILABLE;齊全也只到 PENDING——要 flip 仍得先
過驗證。

### `load.sh` / `settings.prop` 契約

`package/module/load.sh` 是「這個模組怎麼 insmod」的單一事實來源(開機路徑與 app
的執行期重新啟用都跑它),並以一串 insmod 階梯降級,好讓缺少某參數的舊 `.ko` 仍載
得起來。階梯機制刻意仰賴嚴格核心會**拒絕**未知參數——所以本模組**絕不可定義名為
`pool_target` 的參數**(那是階梯行裡帶的歷史名)。

`settings.prop` 鍵:`pool_want`、`pool_want_with_cma`、`cma_movable_lever`
(`hook` | `flag` → insmod 時要武裝哪個 lever 檔)、`system_reserve_mb`(有設才傳)、`cma_reservoir_floor_mb`(有設才傳,掛在 v10
CMA 參數組而非新的頂層階梯級——它是 v10 就有的參數;而且**必須走 insmod**,不能
事後寫它的 0600 檔:insmod 內的同步 prefill 已經在建儲備池、每次翻牌都問這個
floor,事後才寫晚了一整個開機。另注意參數自 v10 就存在,存在性**證明不了**本包
的 load.sh 會傳它,app 要靠重載後 readback 比對或看 versionCode)、
以及開機按壓三件組:`boot_acquire`(0–3,預設 0:模組在位後 `load.sh` 對 `acquire`
寫入該 mode,遇 `-ENOSYS` 逐級降 3→2→1,失敗不致命)、`boot_acquire_runs`(預設 1)、
`boot_acquire_wait`(秒,預設 0)。三者都是 **loader 政策不是模組參數**——按下去
就是一個 USER run,app 照 `refill_stat` 顯示進度,`acquire=0` 可取消。這三個模組參數
是可寫的(0600),**正因為模組對它們全盲**:寫入改變不了任何行為,卻讓存下來的設定
在下次載入前就有地方看得見。模組會據以行動的一律唯讀——`system_reserve_mb` 定的表
只建一次,事後寫入只會說謊。

這三件組是 temp-root 機種手上唯一的「按鈕」。temp-root 的**軟重啟**只重啟 Android
userspace、核心不落地,所以本腳本會在 .ko 還載著的情況下重跑、insmod 回 EEXIST——
因此按壓的條件是**模組在位**而不是本次 insmod 成功:userspace 倒下、GUI 未起的
那一刻是模組能見到最乾淨的記憶體,也是重壓唯一的窗口,賠給一個 EEXIST 就沒了。
`runs` 讓那個 USER run 按不只一次,前一次跑完才按下一次——一個 run 是單向單趟,
被自己 evict 出來、落在 sweep 游標後方的窗只有下一 run 撿得到;已達標的 run 會在
達標快查 O(1) 出場,所以多按不花錢。`wait` 是 `load.sh` 可以在 post-fs-data 阻塞
等它們的秒數,這才是真的把 zygote 擋在後面讓 sweep 獨佔記憶體;要低於 root 管理器
的 post-fs-data 逾時(Magisk 40s),沒等完的照樣在背景跑完。阻塞屬於開機路徑,
只有 `GH_BOOT=1` 的呼叫者(post-fs-data.sh)能花這個預算——app 的執行期「啟用」
跑同一支腳本、照按同樣的 run,但永不阻塞。預設(0 / 1 / 0)即原
行為;三個鍵住在 `/data` 的 `settings.prop`,每次軟重啟都會重新讀取。

### `/dev/gh_pinprobe`

一支唯讀的「這段範圍會不會過不了 `FOLL_LONGTERM` pin?」探針(`GH_PINPROBE_RANGE`、
`'P'` magic、`struct gh_pinprobe_range`),ABI 與原本在 `gh_unmovable.ko` 的版本
逐位元組相同,crosvm 客戶端不需改動。每次 gunyah 記憶體轉移最後都是
`pin_user_pages_fast(FOLL_LONGTERM)`,而位於 CMA / isolate / ZONE_MOVABLE
pageblock 的頁沒辦法這樣 pin——核心會先把它遷走,遷不動時失敗會發生在 hypervisor
呼叫深處。探針改問便宜的那個問題(「這裡面有沒有頁落在需要遷移的 pageblock?」),
以 `FOLL_NOFAULT` 每 2MB 取樣一次,絕不 fault 進任何頁。這支探針歸本模組是因為
CMA 狀態本來就是它在管,執行期的 `MIGRATE_CMA` 值也只有它知道。

---

## 用法

```sh
# 帶開機儲備載入(開機時記憶體未碎片化,遠比事後可靠)。
insmod gh_hugepage_reserve.ko pool_want=1024                    # 1024 × 2MB = 2GB

# 同上,另外開 2GB 儲備池,沒 VM 用時借給系統。
insmod gh_hugepage_reserve.ko pool_want=1024 pool_want_with_cma=2048 \
       migrate_cma_val=... pageblock_order_val=...              # 這兩個由 load.sh 帶入

# 事後主動補(模式 B:逐窗 evict),並觀察進度。
echo 3 > .../parameters/acquire
while grep -q 'acquire_active=1' .../parameters/refill_stat; do sleep 1; done
grep -E 'pool_avail|pool_cma|acquire_stop_reason' .../parameters/refill_stat

# 使用者 acquire 進行中要改目標:先取消,再寫。
echo 0    > .../parameters/acquire
echo 2048 > .../parameters/pool_want
echo 3    > .../parameters/acquire

# 單獨驗證「零洩漏歸還」(此時池只會從 VM 真的還回來的頁補,永不新配置)。
insmod gh_hugepage_reserve.ko pool_want=256 refill_enable=0
# ... 開 VM、用一用、關機,等 ~10 秒 ...
cat .../parameters/refill_stat
cat .../parameters/reclaim_debug     # in_hook 應對得上歸還量;reject/orphan ≈ 0
```

(其中 `...` 是 `/sys/module/gh_hugepage_reserve/parameters`)

---

## 現況

可 mock 測試的核心(pool 物件 + 兩個 worker)已完成且 CI 全綠(30 情境 + 一支 TSan
race harness)。核心 backend(kapi / hooks / sysfs / unlock_cma / pinprobe + root)
對 android14-6.1 / android15-6.6 / android16-6.12 編譯零警告,Tier-1 在 QEMU 上三個
核心全過(2026-08-28):kapi 全部解析成功、CMA verify 走到 VERIFIED(含一次真實的
DEFERRED 重試)、六個 hook 全掛上、情境 A–E 通過、卸載乾淨。

剩下的是 Tier-2:三台目標裝置的真機回歸(真的 `FOLL_LONGTERM`、廠商差異、kCFI),
以及兩則 `NOTE(on-device)` 待確認項(owner 寫鎖的 irq 上下文;kapi 粗粒度 adapter
的注意事項)。
