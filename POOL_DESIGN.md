# gh_hugepage_reserve:2MB 大頁池

2026-08-28(v2 精簡版;取代 2026-08-23 版,舊版全文另存於工作備份)。
**單一事實來源**——實作以本文件為準。對外介面(sysfs 檔名/格式/錯誤碼、模組參數、
load.sh 行為)是既存外部消費者(管理 app、load.sh、serve_test)的解析對象,契約見 §10。
標「實測」者為真機量測結論,是設計依據。

職責:在 Gunyah 手機(demand-paging 客體)上維護 2MB(order-9)大頁池——serve hook
攔截 VM 的 order-9 配置改發池頁,free hook 在頁被放掉時依 pfn 精確接回;超出池份額的
監護頁翻成 MIGRATE_CMA 儲備池借給系統(page cache/mTHP),要用時拿回。兩個目標值:
`pool_want`(池份額,I1)與 `pool_want_with_cma`(總監護,I2)。

S := CMA_SUBBLKS(CMA 塊的 2MB 子格數;cma_order=10 → 4MB 塊 → S=2)。
S==1 退化:塊=格,零額外成本。

---

## 架構原則(正確性的來源)

正確性來自結構規則,不來自防禦性檢查。**審查標準:一段 code 若只為「擋住某種情境」
而存在,而該情境已被結構規則排除,它就是待刪的防禦 code。**(歷史教訓:v1 的
free-hook gate、fallback_sweep、hook_enable 空池拒開,三者都是防禦 code,都藏了 bug。)

1. **pool 是純狀態機**。每格恰處一狀態;一條邊 = 一個 public api。每個 api 在自己的
   鎖內:重驗前置(只讀 pool 自己的狀態)→ 轉移 → 忠實回報。前置不成立 = no-op 回
   0/false,永不因「外面的世界怎樣」拒跑。pool api 只關注 page 狀態變化;它不知道
   呼叫者是誰、環境長怎樣。
2. **分層單向**:consumer → worker → pool api → kapi。呼叫向下、值向上;**下層永不讀
   上層狀態**。執法點 = mock build:tests/shim.h 只准提供 §3.3 的環境原語,pool/worker
   需要 shim 造假的其他符號 = link 失敗。上層讀下層一律經 accessor/snapshot,不裸讀
   內部變數(§0.1 契約表)。
3. **一套流程,條件驅動**。無模式旗標、無環境嗅探。worker 每輪從當下狀態重算目標差
   (絕不快取);環境差異(hook 有無、符號缺失、alloc 失敗)只表現為狀態與計數的
   不同,同一條 pipeline 自行適應(§6 hook 缺席行為是樣板)。唯二合法的環境查詢:
   `kapi.cap()`(能力)與模組參數(政策),都只在 worker/consumer 層消費。
4. **失敗 = 誠實回報 + 既有出路**。會失敗的邊回報實收;收不回的東西總有一條已畫好的
   邊在等(sweep 不回→purge;grab 不到→留 CMA 續計;湊不成→flush)。不為失敗開新
   路徑、不因「可能失敗」拒絕執行。
5. **放手是事件+短寬限,purge 是時鐘,輪次只是載具**。served 頁的放手由 **pid
   死亡事件**驅動,外加 `DEATH_GRACE(=ROUNDS−2=3s)` 的短寬限讓 exit 路徑自己的
   free(delayed_fput、晚到 unpin)以**回收**而非放手回家——時鐘只在死亡時啟動且
   永不清除(死亡單向),不存在可與 VM 重開競態的 idle 計時;owner 活著——不論
   vm_count——一律不放手。released 頁的 purge 由 `slot.released_at + GRACE(10s)`
   承擔。輪次重跑、高頻觸發、被續命都不改變任何事件或時限——冪等。
6. **帳實相符**。先改 state 再放手(release 先標再 put、shed 先標再 free,反序即資料
   遺失);CMA 標籤與 freelist 一致(絕不只翻標籤);計數/串列/state 互證(G,
   debug=1 時 pool_check)。

### SOLID 對映與執法(C 語境;2026-08-28 全面驗收通過)

- **SRP 單一職責**:每個部件恰一個變更理由——`gh_pool` 狀態機+簿記(機制,零政策
  零環境知識)、`gh_release` 歸還生命週期(政策:何時放手)、`gh_adjust` 收斂
  pipeline(政策:拿多少、多痛)、`gh_kapi` 版本適配(唯一碰核心符號處)、
  `gh_hooks` 事件轉接(一 hook 一呼叫,零政策)、`gh_sysfs` ABI 解析/轉發、
  `gh_owner` 註冊表、`unlock_cma`/`pinprobe` 獨立旁路、root 組裝序列。
- **OCP 開放封閉**:擴充點——新核心版本 = kraw 版本真簽章 + shim,adapter 表不動
  (6.1/6.6/6.12 同一份上層碼);新取法 = 邊上加 mode;新 profile = stages 表加一行
  + 優先序,phase 骨架不動;新 vendor hook 世代 = 按名多找一個;新 insmod 參數 =
  load.sh 階梯頂端加一級(舊 .ko 拒絕自動降級)。封閉粒度 = owning api 內的一個
  switch——C 無 vtable,這是語言上限,已是慣用最佳。
- **LSP 里氏替換**:三組替身都守契約——mock kapi(同一張表、同失敗語意、行為忠實:
  如 mock contig_range 同樣先 drain pcp,§3.3)、shim env(原語白名單、free 路徑
  行為等價含 hook dispatch/pcp-lag)、kraw 各版本原型經 shim 正規化成單一 adapter
  語意。mock 刻意不模擬的(atomic 合法性、鎖序、真遷移)以 Tier 邊界明文宣告,
  是契約範圍聲明不是缺口。
- **ISP 介面隔離**:§0.1 契約表每列 = 一個 consumer 的最小依賴面(serve hook 3 個
  函數、sysfs 走 snapshot+accessor、pinprobe 兩個 kapi 入口…);不在表上的跨層
  讀寫即是違規。
- **DIP 依賴反轉**:呼叫向下、值向上;高層政策(worker)依賴狀態機抽象(pool
  public api),pool 不認識任何 worker;pool 依賴 kapi 函數表,不依賴核心符號;
  parts 依賴環境原語(kernel_env/shim 兩實作);worker 依賴 `gh_sched_*`
  (root/harness 各自實作)。政策在邊界翻譯——`pool_targets_snapshot(bool capped)`
  收概念不收 profile。**pool 的反向依賴恆為零。**

**執法點**(原則不靠自覺):(1) shim.h 白名單 = 分層的 **link-time** 執法——pool
需要 shim 造假的任何符號即 link 失敗;(2) §0.1 契約表 = 介面的 **grep-time** 執法;
(3) §3.3 mock 忠實規則 = LSP 的 **test-time** 執法(38 情境 + race + 三版 QEMU);
(4) §4 的「顯式契約 vs 防禦 code」判準——擋住的行為若是使用者該以顯式序列表達的
意圖,它是契約(如 run 進行中 want 寫入的 -EBUSY);否則才是待刪的防禦。

**記錄在案的妥協**(權衡,非欠帳;審查時勿「修」):
1. `pool_released_return` 內含 KEEP/DROP 政策公式——必須是單鎖事件 api(拆開即
   TOCTOU),且政策只讀 pool 自身狀態,屬「pool 對自身狀態的政策」(SRP)。
2. `pool_ext_to_avail` 兼做 alloc+扣帳+兄弟補齊——「acquire 這條邊」的原子契約,
   拆開需要跨呼叫不變式,§2 明文禁止(SRP)。
3. mode/stages 的 switch 粒度(OCP,C 語言上限)。
4. `gh_kapi` 單一函數表全員共用——C 慣例,`cap()` 按能力分割緩解;拆 per-consumer
   表只加管線不加安全(ISP)。
5. `gh_param_debug` 定義於 pool、worker 直讀後呼叫 pool debug api——debug 儀表,
   方向合法(向下),不值一個 accessor。

---

## 0. 兩張圖

### 0.1 整體架構與互動點契約

```
consumer 層(依呼叫目標分組;insmod/rmmod 例外——觸碰全層)
┌────────────────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────┐
│ -> pool api                │  │ -> release worker    │  │ -> adjust worker     │  │ -> kapi      │
│ serve hook (atomic)        │  │ vm_shutdown/unshare  │  │ sysfs want shrink    │  │ unlock_cma   │
│ free hook (atomic)         │  │ pid-exit hooks       │  │ sysfs acquire / =0   │  │ (旁路)       │
│ sysfs reads, vm_boot(pid)  │  │ (release_run(5))     │  │ (adjust_try, run=0)  │  │ pinprobe     │
└─────────────┬──────────────┘  └───────────┬──────────┘  └───────────┬──────────┘  └─────────────┬┘
              │                             ▼                         ▼                           │
              │             ┌────────────────────────────────┐      ┌────────────────────────────┐│
              │             │ release_worker (1s x 5)        ├─────►│ adjust_worker (10ms x MAX) ││
              │             └───────────────┬────────────────┘      └─────────────┬──────────────┘│
              ▼                             ▼                                     ▼               │
┌──────────────────────────────────────────────────────────────────────────────────────┐          │
│ pool api   public: pool_{src}_to_{dst}(一邊一函數)+ 查詢/action                     │          │
│            private: pool_private_{slot,classify,avail_in/out,promote,demote,free}    │          │
└─────────────────────────────────────────┬────────────────────────────────────────────┘          │
                                          ▼                                                       │
┌──────────────────────────────────────────────────────────────────────────────────────┐          │
│ kapi  alloc / contig / pageblock MT / walk_ram / probes / reclaim / evict / floors   │◄─────────┘
└──────────────────────────────────────────────────────────────────────────────────────┘
```

**互動點契約表**(每列 = 一個介面;不在表上的跨層讀寫即是違規):

| 誰 → 誰 | 允許的介面 | 備註 |
|---|---|---|
| serve hook → pool | `pool_avail()`、`pool_owner_maybe()`(lockless 快濾)、`pool_avail_to_served()` | 一 hook 一呼叫,無政策 |
| free hook → pool | `pool_served()/pool_released()`(快濾)、`pool_released_return()` | 事件 api,單鎖內決策 |
| vm_boot hook → pool | `pool_owner_add()` | owner api 屬 pool facade |
| VM 事件 hook → release | `pool_owner_any()`(lockless 快濾)→ `release_vm_shutdown/unshare/vm_exit()` facade | facade 內 = owner api + release_run;owner 表空即略(vendor Gunyah 流量) |
| sysfs → pool | `pool_stats_snapshot` / `pool_owner_stats_snapshot` / `pool_purge_log_snapshot` / `pool_set_target` / 查詢 | 不持鎖、不讀欄位、單快照不混世代 |
| sysfs → worker | `adjust_user_acquire/cancel/try`、`release_run(1)`、**accessor**:`adjust_running()`、`adjust_stop_reason_str()` | 不裸讀 worker 內部變數 |
| sysfs → hooks | `hooks_serve_set/reclaim_set`、`hooks_*_active()` | 讀回實際掛載狀態 |
| sysfs ↔ 政策 param | `module_param` 綁 worker 的 gh_param_*(refill_enable / drop_slab / mem_floor / debug) | param 即介面,knob 住在消費者 |
| release → pool | `pool_served_to_released/served_to_ext(EXPIRED)/owner_sweep` | |
| release → kapi | `drain_pages`(第 3 輪 pcp-lag) | §0 (8):不帶頁身分的重活可直走 kapi |
| release → adjust | `adjust_try(RELEASE)` 每輪 | 圖上唯一的 worker→worker 邊 |
| adjust → pool | 全部經 public(轉移邊、prepare/scan/cursor、targets_snapshot、window_candidate/evict) | 狀態機唯二驅動者之一 |
| adjust → kapi | `drop_slab / mem_available_mb / cma_floor_ok / drain_pages / cap()` | 同上,非池動作 |
| pool → kapi | `struct gh_kapi` 函數表(§3.3) | pool 唯一的向下依賴;版本分歧在 kraw/shim 內 |
| pool/worker → 環境 | §3.3 原語白名單(鎖/時鐘/頁原語/print/alloc)+ `gh_sched_*` | kernel_env.h 與 tests/shim.h 是同一契約的兩個實作 |
| unlock_cma → kapi + pool 查詢 | kapi 若干 + `pool_ready/pool_cma/pool_cma_capable()` + `adjust_running()` | 只讀查詢,不動狀態機、不碰轉移邊 |
| pinprobe → kapi | `get_pfnblock_mt / in_zone_movable` | 最小介面 |
| insmod/rmmod(root)→ 全層 | 組裝者例外;固定序 §8 | |

**pool 的反向依賴 = 零**:pool 需要的一切經呼叫參數或自有狀態進來
(例:`pool_targets_snapshot(bool capped)` 收的是「要不要 cap 到已證明容量」,
不是 profile——pool 不認識 profile)。

### 0.2 slot 表:兩層定址 + avail 三串列 + 索引掃

```
定址(空洞免疫,mem_section 同款):
  chunk = 1GB 的 entry 塊(512 格 × 20B ≈ 10KB,kzalloc);1GB 是 CMA 塊的整數倍
          → 兄弟掃描永不跨 chunk
  top[] = span_GB 個 chunk 指標,NULL = 整 GB 非 RAM
  slot(pfn): top[off>>30][(off>>21)&511]          /* 兩次解參考,O(1) 最壞 */

每格 20B:state / origin / owner_id(SERVED/RELEASED 有效;generation+index,防
pid reuse)/ released_at(秒)/ prev,next(僅 AVAIL 掛串列時有效,NIL=~0)

串列只有 avail 三條——維護串列的唯一正當理由是 atomic 上下文要 O(1)「下一個可用」,
全設計只有 serve 符合:
  avail_non        塊 !cma_able(straggler)
  avail_cma_able   塊 cma_able 但有兄弟在外(served/released/cand)
  avail_cma_ready  塊全 AVAIL;flip 唯一來源

其他狀態不掛串列,消費者全在 worker/rmmod(可睡),用索引序表掃或 O(1) 標量門檻:
  SERVED    全表索引掃(O(span),1s 節奏)
  RELEASED  (released_count, released_oldest) 門檻:count==0 或未過 GRACE → O(1) 返回
  CAND      pool 內 FIFO 小陣列(≤S)
  VERIFY    只存在於 verify_cma_params 單次呼叫內
  CMA       run-local bucket 掃描集(§3.2 prepare_cma_scan)
  EXT       prepare_scan 建的有序掃描集:缺口 → ext_cursor 後 → cursor 前(繞回)
  NOT_USEABLE 靜態,無人迭代

索引掃的免疫性:走靜態表的索引序,不跟連結——「游標指的 next 被搬走」這個問題
類別不存在;並行變動只影響單格,鎖內逐格重驗。

serve 取頁:pop non → able → ready(斷 ready:S-1 兄弟降 able)→ NULL(回歸原始 alloc)
shed 取頁:同序;碰 able/ready 先整塊降級再 pop non → 整塊一起走
flip 取塊:ready 取頭反推塊基址,整塊 S 格解鏈
serve 不降級(頁仍 custody)/ shed 降級(頁出 custody)——不對稱即 custody 原則本身。
```

---

## 1. 狀態與不變式

| 狀態 | 意義 |
|---|---|
| `not_useable` | 非 RAM / carveout / 廠商 CMA / ZONE_MOVABLE,insmod 判定一次,永不參與 |
| `external` | 系統的 |
| `avail` | 池中待命(三串列之一) |
| `served` | 借給 VM(GUP pin + 我們一份保護參照) |
| `released` | 已放手,去向未定(等 hook return / sweep / purge) |
| `cand` | collect_cma 組裝中,不可 serve/shed |
| `verify` | 驗證窗暫存(單次呼叫內,不可 serve/shed) |
| `cma` | 儲備池,借給系統 |

```
custody   = avail + served + released + cand + verify(cand 只算塊判定,不算 held)
held      = avail + served + released + verify
held_cma  = held + cma

I1  held     → pool_want            (worker 的收斂目標,非瞬間結構不變式)
I2  held_cma → effective pool_want_with_cma
W   恆有 pool_want <= pool_want_with_cma <= pool_size_max,或 with_cma == 0(停用);
    cma 可用且 S>1 時兩者(非 0)皆為 S 的倍數——不對齊則 reserve_target 非整塊,
    flip/stage_in/drop 每動一次就震盪,對齊是 W 的一部分
P   0 <= pool_total <= configured_total <= pool_size_max
    (pool_total = 已實際取得、允許背景補回的「已證明容量」,不是 held 的別名,§5.1)
U   CMA 整塊一致:同塊 S 格要嘛全 CMA 要嘛全非 CMA
Q   最小化 custody 中不屬於 cma_able 塊的頁(碎塊最少化)
G   簿記一致:state/串列/計數器互證(debug=1 pool_check 驗 G/U/W/P)
```

**held 含 released 是刻意的**:released 大概率會回來,不算它,VM 關機瞬間 worker 會
去買「正要回家的頁」的替代品。**放棄(purge)那一刻 deficit 才真的打開。**
代價與對策:達標檢查因此看不見「待掃回的自家頁」,§7 PREPARE 的 released 條款補上。

**塊屬性即時導出,不存欄位**:cma_able = 塊的 S 格全在 custody;cma_ready = 全 AVAIL
(flip 唯一合法對象——served 是 FOLL_LONGTERM pin,含 pin 的塊翻 CMA 是死路)。
升級點(promote,worker):alloc 落進部分持有塊 / 湊滿缺口 / CMA 整塊拿回;
降級點(demote,custody 出口):purge、EXPIRED 放手、DROP、shed 碰 able+。
cma_ready 起落(非升降級)發生在 atomic 熱路徑:serve 斷 ready、return 湊滿,O(S)。
含 not_useable 格的塊永遠成不了 cma_able → 缺口清單直接跳過
(實測:84% 的 2MB 窗有 hard straggler,省掉注定失敗的 contig+evict)。

---

## 2. pool 物件:鎖與並行契約

兩把鎖:

- **pid_lock(rwlock)**:owner 註冊表。entry 活到 pid 死亡。**vm_count 只做一件事:
  serve 的 gate**(vm_boot ++、vm_shutdown --;1→0→1 的 0 期間不 serve)——不驅動
  任何放手、不蓋任何時戳。**放手是 pid 死亡的事件後果**(sched_process_exit 標
  dead + 蓋 died_at,過 `DEATH_GRACE(3s)` 後的輪次放手——寬限讓 exit 清算自己的
  free 還來得及走回收),不是 shutdown 後的計時:舊制的 idle_since + 10s 在
  「1→0 蓋戳 → 放手 → 0→1 重開」序列上有誤放手/誤標 abandoned 的競態窗口;
  死亡是單向事件、died_at 永不清除,無此窗口(2026-08-28 定案)。
  DEATH_GRACE = GH_RELEASE_ROUNDS − 2:死亡事件自己 arm 的保證輪次(5×1s)必定
  蓋過寬限到期,不依賴自續;兩者皆編譯期常數,改任一都要重核這個配對
  (_Static_assert 擋 ROUNDS<2)。shutdown 因此與 unshare 等價,
  僅多維護 gate(歸因序:帶 mm 直查 → 比 current->tgid → 唯一活 owner → 放棄記
  log——歸因失敗只影響 gate 準度,放手不受影響,由 pid 死亡兜底)。owner sweep
  只清「已死且名下無頁」者,mmdrop 鎖外。serve 熱路徑先 lockless
  `pool_owner_maybe()` 快濾,不對就返回不碰任何鎖。
- **pool_lock(raw spinlock)**:slot 表、三串列、計數器、游標、掃描集全在它下面。

唯一巢狀點:atomic serve 以 pid_lock(read) → pool_lock 固定序把「owner 驗證 + 轉移」
做成一次事件(拆兩次呼叫會有 TOCTOU);其他 api 不反向巢狀。

**所有狀態與其鎖都在 pool 物件內**(C 沒有物件,pool api 就是那個物件):consumer /
worker 對池只呼叫 public api、拿回值,從不持 pool_lock、從不看 top[]/slot。
**反向同樣成立:pool 從不讀 consumer/worker/hook 的狀態**——它需要的一切經呼叫參數
或自有狀態進來。執法點 = mock build(shim.h 白名單,link 失敗立刻現形)。

**並行契約**(race 安全的結構性理由):

- **單呼叫原子、無跨呼叫不變式**:每個 api 在 pool_lock 內原子完成簿記;沒有需要
  跨呼叫修復的結構,ABA 一族沒有寄生點(格子不配置不釋放,state 歸 EXT 即「不在池」)。
- **api 自衛**:呼叫者的條件檢查全是鎖外建議性的;api 鎖內重驗前置,不成立 no-op。
- **清單 = pfn 值,消費點重驗**:掃描集存值不存指標;slot 表靜態、memmap 恆在
  → 懸空在結構上不可能,會發生的只有過期,由消費點鎖內重驗仲裁。
- **迭代 = 索引序表掃**(§0.2);掃描中要做慢動作的:鎖內收集 pfn 快照 → 鎖外逐項、
  每項鎖內重驗。
- 兩條硬紀律:(a) **鎖內禁 put_page/__free_pages**(free tracepoint 重入 pool_lock
  = 自鎖死);(b) **先改 state 再放手**(§原則 6)。

記憶體帳:entry 20B;16GB≈160KB、32GB≈320KB;空洞只花頂層 NULL 指標;
執行期零 kmalloc(chunk 與掃描集陣列全在 insmod 預配;64GB span 掃描集最壞 ~288KB)。

---

## 3. pool api

**命名規約**:public = `pool_{src}_to_{dst}`,一條邊一個函數,同邊變體用 mode 參數。
例外一:free hook 的歸還是事件 api `pool_released_return()`(KEEP/DROP 必須同鎖內
決定)。例外二:released→avail 的 hook return 與 worker sweep 語意差太大,後者留
`_sweep` 後綴。promote(cand→avail)無 public——`pool_private_promote` 在完成檢查裡
執行。查詢/清單/debug 不是邊,不套規約。private = `pool_private_xxx`,僅 api 內部。

### 3.1 狀態機(一條邊 = 一個 public api)

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
* = custody 出口(塊降級點)   ! = 會失敗的 api(失敗語意見 §3.2)
```

| 邊 | api(呼叫者) | 失敗語意 |
|---|---|---|
| acquire(ext→avail) | `pool_ext_to_avail(how, pfn, budget, flags)`(adjust) | 會失敗→0;成功回實收 2MB 頁數 |
| serve(avail→served) | `pool_avail_to_served(pid)`(serve hook,atomic) | 可 NULL(未註冊/vm_count==0/三列空)= 不發生 |
| release(served→released) | `pool_served_to_released()`(release) | 逐頁條件式;回 pending |
| return(released→avail/ext) | `pool_released_return(pg)`(free hook,atomic) | MISS / KEEP / DROP |
| sweep(released→avail) | `pool_released_to_avail_sweep(pfn)`(adjust) | 會失敗→留 released 等 purge |
| purge(released→ext) | `pool_released_to_ext(EXPIRED/ALL)`(adjust/rmmod) | 保證成功(頁早在 buddy) |
| expired(served→ext) | `pool_served_to_ext(EXPIRED/ALL)`(release/rmmod) | 保證成功(先改 state 再放手) |
| flip(avail→cma) | `pool_avail_to_cma(n)`(adjust) | 不失敗(取自 ready,結構保證) |
| stage_in(cma→avail) | `pool_cma_to_avail(n)`(adjust) | 會失敗(pin/writeback),回實拿塊數 |
| drop(cma→ext) | `pool_cma_to_ext(n/ALL)`(adjust/rmmod) | 會失敗;搶不到的塊留 CMA 續計 |
| shed(avail→ext) | `pool_avail_to_ext(n)`(adjust/rmmod) | 保證成功 |
| grab(ext→cand) | `pool_ext_to_cand(pfn)`(adjust) | 會失敗;FIFO 溢出淘汰最舊 |
| flush(cand→ext) | `pool_cand_to_ext()`(run 結束/rmmod) | 保證成功 |
| promote(cand→avail) | 無 public(private,完成檢查內) | 不失敗;守恆 free 同步執行 |

`not_useable` 不進轉移圖(insmod 靜態判定);`verify` 只存在於
`pool_verify_cma_params()` 單次呼叫內;`cma_able/ready` 是導出屬性不是狀態。

### 3.2 方法契約(每法:做什麼 + 失敗語意 + 不可省的細節)

**hook 用(atomic-safe)**

- `pool_avail_to_served(pid)`:三階序 pop(non O(1) → able O(1) → ready O(S),斷
  ready 時 S-1 兄弟降 able)。離開 AVAIL 前 `get_page()`——保護參照使外借期間
  order-9 compound 不能 split/migrate。**只 serve vm_count>0 的 owner**(entry 活到
  pid 死亡,沒這條會把池頁發給 VM 已關的 crosvm 的一般 THP)。
- `pool_released_return(pg)`:slot(pfn) 兩次解參考,非 RELEASED → MISS。命中後全部
  判斷在同一次 pool_lock 內:
  `effective_cma = (VERIFIED && with_cma!=0) ? with_cma : want`;
  `keep = held <= want || (held_cma <= effective_cma && 塊 cma_able)`
  (S>1 時只有「有機會湊整塊」的頁才准為儲備份額留下;S==1 恆 able 無差)。
  KEEP:free path 已拆 compound,同一 atomic 事件內 `rebuild_order9` 再入列;
  必要時抬 pool_total(cap 到 configured total);hook 設 bypass。
  DROP:RELEASED→EXT + demote,不 bypass,記 gate_drop(不是失敗)。

**release worker 用**

- `pool_served_to_released()`:**無條件執行,不讀 hook 掛載狀態**——pool api 只關注
  狀態變化並忠實回報;RELEASED 之後誰接手(hook µs 級 / sweep 秒級)或走 purge,由
  條件系統仲裁(§6)。索引掃全表逐 SERVED 三分類:
  `refcount==1 && !mapping` → 鎖內標 RELEASED 蓋戳 → 鎖外 put_page;
  `refcount>1` → pin 未放,下輪再看;
  `page_count==0` → **orphan**(free 漏接,參照已蒸發)→ 不 put,標 RELEASED 交給
  sweep/purge 既有出路。回傳 pending = 仍 pinned 且 **owner 已亡**的 SERVED 數
  (release 自續命的燃料——撐到死亡放手完成,天然有界;活 owner 的頁不論
  vm_count 都不算)。
- `pool_served_to_ext(EXPIRED|ALL)`:EXPIRED = **owner 已亡且死亡寬限已過**
  (dead 且 now−died_at ≥ DEATH_GRACE,∨ generation 不在表;鎖內 state→EXT +
  demote,鎖外 put,purge_log + owner.abandoned++)。**活著的 owner——不論
  vm_count——一律不碰**:放手是 pid 死亡的事件後果 + 3s 寬限(讓 exit 清算的
  free 走回收),不是 idle 計時;stuck pin 在 crosvm 活著期間就留在 SERVED(§11)。
  ALL = rmmod 全清,不等 unpin(頁跟著原 holder 活)。

**adjust worker 用(可睡)**

- `pool_released_to_ext(EXPIRED|ALL)`:EXPIRED 由 (count,oldest) 門檻擋,O(1) 返回;
  過 GRACE 才索引掃 purge 全部過期者並重算 oldest,每命中 demote。
- `pool_released_to_avail_sweep(pfn)`:前置 `page_count==0`(真在 buddy/pcp;
  contig_range 內部 drain pcp,parked 頁會先流過 hook 或落 buddy)。contig 抓回 →
  rebuild → 以 return KEEP 語意入列(ORIGIN_SWEEP)。`page_count>0` = 已被再配置,
  不遷移新佔用者,留給 purge。
- `pool_ext_to_avail(how, pfn, budget, flags)`:how = ALLOC_BLOCK(整塊 NORETRY,
  天生 ready,回 S)| ALLOC_LIGHT(單頁撿現成)| ALLOC_FULL(RETRY_MAYFAIL,核心
  compaction+reclaim,prefill/背景主力)| CONTIG_ANY | CONTIG_AT(指定窗,async
  NORETRY / sync 兩檔)。**預算**:caller 傳 `{single, ready}`(2MB 頁單位),api 依
  實際結果扣帳並鎖內重算 hard cap(caller budget 只是上限):
  `ready = new_page_need`;`single = S==1 ? ready : min(max(diff_want,0), new_page_need)`
  ——**越 want 的 intake 只准以天生 ready 整塊形式發生**(否則 S>1 時 cheap 灌到
  want_cma、輪尾 shed 全丟)。S>1 單頁到手後兄弟補齊:≤S−1 次 contig NORETRY
  (ext_to_cand),湊滿 promote。**gfp 鐵律:GFP_KERNEL 系,禁 movable/__GFP_CMA**
  (unmovable 進不了 CMA freelist;movable 頁會在 FOLL_LONGTERM pin 前被 GUP 遷走)。
  被 avail_in 拒收 → 立即 free(**不改 state**——它若是自家 RELEASED,order-9 free
  會經 hook 接回)+ reject 計數 + 當作該次失敗,不原地重試(ping-pong)。
  成功後同鎖內 `pool_total = max(pool_total, min(held_cma, configured_total))`。
- `pool_ext_to_cand(pfn)` / `pool_cand_to_ext()`:contig 抓窗(會失敗)→ 鎖外
  rebuild → 鎖內重驗 EXT 後發布 CAND,FIFO ≤S,溢出淘汰最舊(它的塊湊不齊,自清);
  flush 全部 free 回 buddy。
- `pool_cma_to_avail(n)`(stage_in):自本 run 的 CMA bucket 掃描集低分往高分 pop,
  整塊遷移借用者(會失敗:pin/writeback);失敗候選標 SKIP_THIS_SCAN 本 run 不重試。
  整塊拿回直入 ready。n 以塊計(CTA_BATCH=8/輪——實測連續 grab ~10s/8GB,節奏要跟上)。
- `pool_cma_to_ext(n|ALL)`(drop):每塊 CMA-mode contig grab → **全塊在手才**翻標籤
  回 MOVABLE → free 進 MOVABLE freelist → S 格 CMA→EXT。搶不到的塊**保留 CMA、續計
  pool_cma**(帳與標籤一致)。**禁「只翻標籤」**:free 頁還在 CMA freelist,標籤一翻
  = CmaFree 帳漂移 + 頁對 unmovable 隱形;kernel 自己翻也走 move_freepages_block,
  該符號不可用,「拿在手裡再翻」是唯一合法路徑。
- `pool_verify_cma_params()`:唯一能確立 cma_order/migrate_cma 的地方。pool 自選
  完整持有的驗證窗(2 相鄰候選塊 + 依支援 cma_order 上下界擴到「錯一級可能被觸及」
  的 guard;**絕不借未持有鄰塊**);鎖內標 VERIFY(不可 serve/shed)→ 鎖外做邊界讀、
  label readback、CmaFree delta、CMA-mode grab → 還原。CmaFree 讀取要對:init 解出
  池頁所在 zone,delta 用含 per-cpu vm_stat_diff 的 snapshot 讀(否則恆讀 0)。
  結果:無安全窗 = DEFERRED(PENDING 續);delta 太小 = 暫態,DEFERRED 重試到
  CMA_VERIFY_MISS_MAX(8)才轉 FAILED;結構性不符(readback 打臉、grab-back 失敗)
  = 立即 FAILED。FAILED ⇒ UNAVAILABLE(終態)+ with_cma 清 0 + pool_total clamp。
- `pool_avail_to_cma(n)`(flip):ready 取頭反推塊基址,整塊解鏈 AVAIL→CMA,O(S)/塊。
  前置 VERIFIED;呼叫者另問 `kapi.cma_floor_ok`(§7)。頁釋入 CMA freelist 用
  `private_free_to_cma`。
- `pool_avail_to_ext(n)`(shed):三階序攤還 O(1)/頁;碰 able/ready 先整塊 demote。

**頁窗 action(不是邊)**

- `pool_window_candidate(token, purpose)` / `pool_evict(mode, token, purpose)`:
  token 只來自 `pool_scan_pop`;api 重驗 provenance/state/當下 policy 仍授權該
  purpose,再正規化成 `[start,end)` 呼叫 `kapi.candidate_range/evict_range`。
  pool_lock 只用於短暫重驗,絕不跨 reclaim。worker/kapi 都不解 slot。

**查詢/掃描集**

- O(1) 查詢:`pool_avail/served/released/held/cma/noncma_able/avail_cma_able/
  cma_params_state/cma_capable`;sysfs 用 `pool_stats_snapshot` /
  `pool_owner_stats_snapshot`(單快照不混世代)。
- `pool_targets_snapshot(bool capped)`:pool_lock 下回正規化快照(ctx 恆
  want<=want_cma);capped(RELEASE 用)另把 want_cma cap 到 pool_total。
  with_cma==0 或 UNAVAILABLE(或 PENDING 且非 INSMOD/USER)→ want_cma 折回 want,
  下游零特判。
- `pool_set_target(which, v, &change)`:sysfs 唯一寫入口;clamp、S 對齊、coupling、
  pool_total 向下 clamp 全在同鎖內(規則見 §4),回傳 GROW/SAME/SHRINK。
- `pool_prepare_scan(stages)`:一趟索引掃建 run-local 掃描集——precise(RELEASED 且
  count==0,需 A_PRECISE+cap(CONTIG_RANGE))、main(缺口段:部分持有塊的 EXT 格,
  同塊相鄰位址序、跳過含 not_useable 的塊 → cursor 後 → cursor 前;需 A_MAIN)。
  建 main 時先 `kapi.lru_add_drain_all()`(全 CPU IPI,只在 A_MAIN 付)。
- `pool_prepare_cma_scan(RUN|ALL)`:cma_excess>0 || stage_in_budget>0 才呼叫。
  `pool_cma_scan_done()`:pop 無可再取(未建 scan 讀 true——scan_finish 歸零;
  在意的 caller 自己 prepare 後再問);CTA/CMA_FREE 的「試完」退出憑據。鎖內快照 CMA 塊 → 鎖外逐
  base page 探針 occupied → counting sort(score 0 = 全 free 快路徑;非零 =
  1+min(7,(occupied-1)/width),width=⌈CMA_PB_NR/8⌉)。pop 鎖內重驗 CMA;探針是 racy
  排名不是 correctness。rr_cursor 跨 run 持久。
- `pool_scan_pop(PRECISE|MAIN|GAP, &is_gap)`:回 pfn 值,PFN_NONE=取盡;GAP 只吃
  缺口段。`pool_scan_gap_n()` O(1)。`pool_set_cursor(pfn)`:worker 每 pop 非缺口目標
  先前進再做事(中斷後下個 run 從斷點接)。`pool_scan_finish()`:run 結束全作廢。
- owner api:`pool_owner_maybe/any/add/vm_dec/mark_dead/sweep/stats_snapshot`(§2;
  maybe/any 為 lockless advisory,facade/api 鎖內重驗)。

**private(共用點)**

- `avail_in(pfn, expect)`:**唯一防禦漏斗**——驗 slot 存在且 state==expect(各邊宣告
  的 src),不符拒收 + reject 計數(呼叫者 free、不改 state)。通過:state→AVAIL、
  classify 入列、湊滿觸發 promote。共用:return KEEP、sweep、acquire 五模式、
  stage_in、promote。
- `promote(blk, victims)`:non→able/ready 搬移、cand→AVAIL、守恆 free(pop avail_non
  k 次,victim 鎖內出列改 EXT,pfn 交呼叫者鎖外 free)。`demote(blk)`:able/ready→non。
- `rebuild_order9(pg)`:head ref 1 + prep_compound + 全 tail ref 0 + 清 ->mapping +
  INIT_LIST_HEAD(->lru)。後兩項必須:殘留 mapping → 日後 free 觸 bad_page;殘留 lru
  連結 → 當新 THP serve 時汙染 lru_gen。所有「非現成 order-9 → AVAIL/CAND」都過這裡。
- `free(pfn)`:state→EXT + __free_pages(自家 hook 看到但 state 已 EXT → 拒收無害)。
- `free_to_cma(pfn)`:整個 2MB 子格當一個 order-9 compound 釋放(拆低階 free 會在
  tail 留非零 mapcount → bad_page)。order-9 free 先落 pcp,要立刻讀 CmaFree 的路徑
  (verify/flip 後)必須先 `kapi.drain_pages`。hook 看到此 free 但 state=CMA → MISS。

### 3.3 kapi backend 契約

pool/worker 用的是邏輯 adapter 表(`struct gh_kapi`,gh_defs.h),不是散落的核心
symbol。可用性由 `kapi.cap(feature)` 查;內部依核心版本選 ABI(kraw 版本真簽章 +
正規化 shim),以 `abi/kapi_abi.tsv` 預檢(BTF 簽章不符 → disable_kapi 名單 →
留 NULL → cap false → 整個 feature 前置拒絕,不走錯型指標——kCFI 不符 = panic)。
`candidate_range/evict_range` 只收半開 pfn 範圍;slot/token/purpose 由 pool 解讀。

**mock backend(CI)**:kapi 是唯一碰核心符號的層,整層可被 `tests/mock_kapi.c.inc`
換掉(假 memmap + 決定性 fake buddy)。**LSP 紀律:mock 必須是核心行為的忠實替身**
——例:mock contig_range 同樣先 drain mock pcp(真 alloc_contig_range 內部會 drain,
§3.2 sweep 依賴此行為);mock cma_floor_ok 有真公式模式(mock_floor_mb>0:
noncma = 底值 + fake buddy 實際 free − CMA free,§7 AVAIL_FREE 的自平衡處置
**依賴**「shed 抬 noncma、flip 不動它」這條核心語意,純布林 verdict 表達不了)。前提紀律:pool 對頁的直接觸碰限定在可重定義原語
(pfn_to_page/page_to_pfn/page_count/get_page/put_page/set_page_count/__free_pages/
prep_compound_page + rebuild 內部);folio 旗標判讀只活在 kapi kernel 實作。
mock 能測:狀態機/預算/驗證協定/失敗序列/pcp-lag/off-by-one/S=1、S=2/交錯時序。
mock 不能測:atomic 合法性、鎖序、掛載、真實遷移——真機仍是最終驗證。

### 3.4 複雜度

atomic 熱路徑(serve/return)最壞 O(S),三階序把 O(S) 推到最後選項;S≤4 時塊的
S 格同 1–2 條 cacheline。O(span) 動作全在 worker(1s/run 節奏):served 索引掃、
prepare_scan、purge 過門檻後的一次掃。查詢與定址 O(1)。執行期零配置、零事後修復。

---

## 4. 觸發總表

```
insmod → 建表(§8)→ 同步 adjust_try(INSMOD) 驅動到 finish → 掛 hook
rmmod  → 卸 hook → 停 worker → 同步 *→ext(§8)

hook(全部「一個 pool/facade 呼叫,無政策」):
  vm_boot(GH_CREATE_VM kprobe)   → pool_owner_add(current)
  vm_shutdown                     → owner 表空即略(lockless pool_owner_any())
                                    → release_vm_shutdown(current)
  vm_unshare                      → 同上快濾 → release_vm_unshare()
    /* vendor 自家 Gunyah VM(trusted/OEM/TA VM)踩同一批 destroy/reclaim 符號;
     * 實測 SM8750:vendor memparcel 活動會把 release 榨成 idle/running 抖動。
     * 沒有 tracked owner 就沒有可回收之物,不 arm */
  pid 死亡(sched_process_exit tracepoint,core、GKI 必有;group_dead 且
    **lockless 比對 tgid ∈ owner 表**——不可用 mm 比對:此 tracepoint 在 exit_mm
    之後 fire,current->mm 已 NULL,mm 濾波必然 miss、死亡放手永不 arm。這是
    mock 測不到的時序 bug(mock 直接呼叫 mark_dead),Tier-1 fork-child 情境專屬)
    → release_vm_exit(current)
  serve(kretprobe ret)入口快濾依成本序:order≠9 → 池空 → 非 owner(lockless)
    → gfp 無 __GFP_MOVABLE(實測:dma_heap 的 unmovable order-9 永不回 buddy,
    不濾每 VM 偷 2 頁)→ pool_avail_to_served(pid);NULL 則放行原配置
  free(tracepoint,只認 order-9)→ pool_released_return(page);KEEP 則 bypass
```

**sysfs 寫入 gating**:**任何 adjust run 進行中**(`adjust_running()`,任何
profile)`pool_want*` 寫入 -EBUSY——**故意擋住,不是防禦 code**:每個 run 從
init_ctx 快照起就追固定目標;要改目標的唯一序列是 `acquire=0`(停掉在跑的 run)
→ 寫新值 → 重新 `acquire`,三步都是顯式意圖,不存在「run 追到一半目標被偷換」的
隱式行為(不擋的替代方案是 retarget——中斷後以新快照重啟——曾評估過,棄:它把
目標偷換變成合法,app 端反而要防)。背景 run 一視同仁是刻意的一刀切(2026-08-28,
原本只擋 USER run):少一個「這個 run 擋不擋」的分支就少一片 bug 面,且
`acquire_active` 因此**就是**可寫訊號(=1 ⇔ 寫 want 會 -EBUSY ⇔ app 轉圈)。
代價:VM 關機觸發的背景 run(單趟、秒級)期間寫 want 吃 -EBUSY——app 重試或先
`acquire=0` 即可。`acquire` 寫入不在此列:USER run 中才 -EBUSY(§5.1 adjust_try),
對背景 run 是合法搶占。

**want 寫入規則**(`pool_set_target` 同鎖內完成):
- clamp 到 pool_size_max(§8)。
- 對齊:cma 可用且 S>1 時向上對齊 S 的倍數(越過 size_max 則向下)。
- coupling(維持 W):寫 want 且新值 > with_cma≠0 → 拉高 with_cma;寫 with_cma 非 0
  且 < want → 抬到 want(**絕不反向縮 want**);寫 0 = 停用 CMA(want 不動);
  UNAVAILABLE 且寫非 0 → -ENOSYS。
- 觸發:任一值調小 → pool_total clamp 到新 effective total → `adjust_try(SHRINK)`
  (shrink 非同步:寫入立即返回,shed 以批次在 ~1s 內完成);只調大 → 只記錄
  (要人按 acquire);`acquire=0` → `adjust_cancel()`;`acquire={1,2,3}` → mode 映射
  approach/evict 後 `adjust_try(USER)`(錯誤碼見 §10)。

---

## 5. worker 模型

ctx 裡的 `run` 同時是開關和看門狗:每輪 `run--`,歸零收尾;任何人可寫(0=中斷,
最多再跑完當輪;N=啟動/續命)。其餘 ctx 從 init_ctx 到 finish(g_active)只有 worker
自己更新——**搶占一律交棒(next_profile),不當場改 ctx**(worker 可能正在鎖外做
慢動作,輪尾還會存 ctx)。一次 run = 單趟 pipeline,走完自然結束,倒數只是兜底;
PREPARE 的 O(span) 掃描是單次完整動作(內部 cond_resched),10ms 是排程間隔不是
wall-time 上限。release 間隔 1000ms、run 初值 5;adjust 間隔 10ms、run 初值
ADJUST_RUN_MAX(120000,≈20 分鐘看門狗)。

### 5.1 profile:acquire_stage 授權集合

```
A_PRECISE(1)  sweep 自己 released 的頁:免費,誰都不痛
A_CHEAP(2)    NORETRY 撿現成(整塊優先→單頁)
A_CTA(4)      cma→avail:逐出自己儲備池的借用者(中痛,只痛自己人)。
              預算分層(§7 stage_in_budget):USER 拿到 avail-first 特權——儲備
              可穿透 reserve_target 全量變現(短缺記在 cma,不記在 avail);
              背景只收超額,不在系統壓力下自行鎖走已借出的頁。先例同
              PENDING fold:政策表達是 USER 的事,背景永遠保守
A_FULL(8)     RETRY_MAYFAIL:核心自己 compaction+reclaim(核心自管,不需授權)
A_MAIN(16)    全區 sweep + evict + contig:重壓,使用者授權

SHRINK  = CTA                      (縮小/停用:不向系統拿,但可從儲備池拿回)
INSMOD  = CHEAP | FULL             (prefill:整塊優先,開機大塊充裕一次建成)
RELEASE = PRECISE | CTA | (refill_enable ? CHEAP|FULL : 0)
USER    = 全部                     (使用者按 acquire;collect_cma 授權跟 A_MAIN 走)
搶占優先序:USER > SHRINK > RELEASE > INSMOD;USER 進行中誰都不能搶(-EBUSY)。
```

**pool_total 是背景補回 ceiling,不是 held 的別名**:INSMOD/USER 的實收與 KEEP 的頁
可提高它(cap 到 configured total);調小 policy 同步壓低;調大 knob 不提高——
「寫大」不會偷偷授權背景 reclaim。RELEASE 的 ctx 用 `min(pool_total, configured)`。

```
adjust_try(profile):
  無 run → init_ctx(profile)、g_active、run=MAX、排程
  USER run 在跑 → USER 觸發回 -EBUSY,背景觸發靜默 0
  背景 run 在跑 → run=0(中斷)+ next_profile=max_priority(...);finish 時交棒(≤一輪)
adjust_cancel():run=0、next_profile=0(ctx 仍歸 worker,finish 收攤)
```

---

## 6. release_worker

職責:**served→released 全歸它管**、死亡放手(served→ext EXPIRED,owner 已亡
且過 DEATH_GRACE 才做)、owner GC、每輪 `adjust_try(RELEASE)` 觸發背景補回。
**放手 = pid 死亡事件 + 3s 寬限**(§2):shutdown 不啟動任何時鐘,只收 gate 並
arm 回收輪;5 輪是下限且必定蓋過寬限(grace=ROUNDS−2),
**pending>0(仍 pinned 且 owner 已亡)時自續 1 輪**——撐到放手完成,天然有界。
活 owner 的晚到 unpin 若錯過本窗,頁留在 SERVED(held 記著,帳不開洞),
由下一個事件的輪次收回。

```
release_round()(每 1000ms;ctx 只有 {run, round}):
  run 檢查/遞減
  pending = pool_served_to_released()      /* 三分類 + 蓋戳,§3.2 */
  pool_served_to_ext(EXPIRED)              /* owner 已亡+寬限過即放手;活著不碰 */
  round==3 → kapi.drain_pages()            /* pcp-lag:實測每 VM 漏 ~2 頁 */
  drained = (released==0 && pending==0) → run=0
  final → drain + 再收一次 + owner_sweep(只清死者;vm_count==0 活 pid 保留,UI 要看)
  adjust_try(RELEASE)                      /* 等待期 held 含 served/released ⇒ 帳面
                                            * 無缺口,只有 precise 有事;DROP/死亡
                                            * 放手打開 deficit 後 cheap/full/cta 才動 */
  run==0 && pending>0 → run=1(自續)
事件 facade(hook 只碰這層):
  release_vm_shutdown → owner_vm_dec(歸因,純 serve-gate 維護)+ release_run(5)
                        /* 與 unshare 等價,僅多 gate--;不蓋戳、不啟動放手 */
  release_vm_unshare  → release_run(5)
  release_vm_exit     → owner_mark_dead(標死+蓋 died_at)+ release_run(5)
                        /* 放手的唯一事件源;grace(3s)過後的保證輪執行 */
release_run(n):n=0 取消;run==0 → round=0 起跑;進行中只續命不縮短
```

`manual_release=1` = `release_run(1)`,與平常輪次同款;放手判準是死亡事件,
高頻寫無害。(`reconcile` 已降為舊版相容 no-op——v12 統計即時,舊 app 的輪詢式
寫入自動得到它要的效果,§10。)

**free hook 缺席時(tracepoint 掛不上 / reclaim_enable=0)同一套流程自適應,無模式
旗標、無特判**:released_return 不會發生,RELEASED 累積在表上;release 每輪照常
三分類、蓋戳、算 pending;每輪觸發的 adjust 以
AS_PRECISE 批次 contig 掃回(§7),掃不回的過 GRACE 由 purge 放棄,deficit 打開後
refill 補。hook 有無的差別只是 RELEASED 的停留時間(µs vs 秒)與損耗率(put 到
sweep 之間被 pcp-steal 的頁走 purge),帳都在 reclaim_debug / purge_log。

狀態名要分清:「仍被 hold」= 保護 ref 未 put = SERVED,收尾邊 `served_to_ext(EXPIRED)`
(**事件驅動**:owner 已亡 + DEATH_GRACE);已 put、在 buddy/handover 的才是
RELEASED,由
`released_to_ext(EXPIRED)` 收(**時鐘驅動**:released_at + GRACE=10s——全設計
僅存的放棄時鐘)。

## 7. adjust_worker

職責:除 serve(hook)與 release 外的**所有**轉換。骨架兩層:phase(固定序,機制)
× acquire_stage(ACQUIRE 內的力度子序,profile 授權,政策)。一次 run = 單趟
pipeline,phase 只單向往下,走完即 finish;**acquire 在一次 run 只有一次機會**——
後段 shed 挖開的缺口不回頭觸發前段,「抓→湊不成→丟→再抓」的振盪結構上不存在;
重試屬於下一個 trigger。

ctx:run/profile/want/want_cma(init_ctx 正規化快照,§5.1)/phase/astage/
approach(CONTIG_ANY|CONTIG_AT)/evict(MEMCG|ISOLATE)/血量 main_hp、cta_hp、
drop_hp(初值 8;成功回血、連續零進展遞減,歸零放棄進下段)/since_a/cheap_lvl。
掃描集不在 ctx——它是 pool 狀態(prepare 建、pop 消費、finish 作廢)。

```
adjust_round()(每 10ms):
  run 檢查;歸零 → adjust_finish()
  pool_released_to_ext(EXPIRED)            /* 每輪輪首,O(1) 門檻;不等 phase 游標 */
  每輪重算,絕不快取:
    held           = pool_held()
    diff_want      = want − held
    new_page_need  = want_cma − (held + cma)
    reserve_target = want_cma − want       /* ≥0;S 的倍數(§4 對齊) */
    cma_excess     = cma − reserve_target  /* 只餵 CMA_FREE 與達標檢查 */
    desired_cma    = clamp(held+cma − want, 0, reserve_target)
                     /* 組成應然水位:total 先餵 want,溢流停 cma,上限 reserve。
                        「拿到先給 VM,VM 不用給系統」的單式編碼 */
    cma_gap        = desired_cma − cma     /* 有號組成誤差:>0 翻入,<0 變現 */
    stage_in_budget= USER ? max(−cma_gap, 0)  /* avail-first:短缺永遠記在 cma,
                                                 不記在 avail——儲備可穿透
                                                 reserve_target 變現(case 3) */
                          : max(cma_excess, 0) /* 背景只收超額:不在壓力下
                                                  自行鎖走已借出的頁 */
    ready_budget   = new_page_need
    single_budget  = S==1 ? ready_budget : min(max(diff_want,0), new_page_need)
  (所有條件檢查是鎖外建議性的;api 鎖內重驗,失敗 no-op,§2)

  PREPARE:
    全達標(diff_want==0 && new_page_need==0 && cma_excess<=0 &&
      !(PENDING && (INSMOD||USER)) &&
      (!(stages & A_PRECISE) || released==0) &&   ← released 條款:RELEASED 計入
        held,達標檢查因此看不見「自家頁待掃回」;有授權且 released>0 = 還有
        免費工作,不出場
      noncma_able==0)→ finish "already at target"
    new_page_need>0 && A_MAIN && acquire_drop_slab && cap → kapi.drop_slab()
      + kapi.drain_pages()(dentry/inode 不在 LRU 收不回,一頁毒死一個 2MB 窗;
      丟出來的 order-0 落 pcp,不 drain 合不進 buddy 高階,剛開的窗對後續
      cheap/full/sweep 隱形。每 run 一次、只在 USER;lru drain 不在此——
      prepare_scan 建 main 掃描集時已做,不重複付 IPI)
    pool_prepare_scan(stages);cma_capable && (cma_excess>0 || stage_in_budget>0)
      → pool_prepare_cma_scan(RUN)
    → ACQUIRE, astage=PRECISE

  ACQUIRE(唯一受 profile 管;子游標依痛度序):
    PRECISE: released==0 → 下段;否則每輪至多 PRECISE_BATCH(16) 次
      pop precise → pool_released_to_avail_sweep
      (批次是 hook 缺席時的主通道規格:1/輪掃 2GB 要 10s+,輸給 GRACE;
       hook 在場時清單只剩零星漏接,立即 pop 到底,零成本)
    CHEAP: 兩級游標——CHEAP_BLOCK(ALLOC_BLOCK 整塊,天生 ready)敗一次永久降
      CHEAP_SINGLE(ALLOC_LIGHT),本 run 不回頭;每輪至多 CHEAP_BATCH(64) 次,
      預算盡/失敗 → 下段
    CTA: 需 diff_want>0 && stage_in_budget>0;
      pool_cma_to_avail(min(CTA_BATCH=8, stage_in_budget/S))
      無血量:退出 = 滿足(diff_want<=0/預算盡)或試完(cma scan pop 乾)——
      每 run 每塊恰一次 contig 嘗試;CTA_BATCH 只是每輪工作量上限,
      佔用度排序讓「滿足」通常早退,試完只發生在真卡死的 run
    FULL: single_budget>0 才做;每輪至多 FULL_BATCH(16) 次 ALLOC_FULL;失敗即停
      (核心已盡力,再問同答案)
    MAIN: 需 A_MAIN && new_page_need>0。
      CONTIG_ANY(acquire=1):盲拿,main_hp 血量(成功 +3 上限 8,連敗 −1,
        歸零 = "migration exhausted")
      CONTIG_AT(acquire=2/3)整區 sweep,每輪一窗:
        mem_available < acquire_mem_floor_mb → 停 "low-memory floor"
          (實測:sweep 不停手 → RCU stall → 重開機)
        至多 SWEEP_SCAN(256) 次 pop MAIN;非缺口先 pool_set_cursor(先前進再做事);
        pool_window_candidate 可行性閘——**不過閘不 evict**(never white-kick:
        84% 的窗有 hard straggler,沒這閘就是對 84% 的窗白丟 page cache)
        過閘:async NORETRY contig → 失敗才 evict(ISOLATE 逐窗;MEMCG 每
        A_STRIDE(8) 個失敗窗做一次 strided 全系統 reclaim)→ sync contig,
        成敗都往前走(sweep 無 hp,以繞完一圈/達標/floor/使用者收束)
    輪尾結算(不分 astage):live 重算 cma_gap——結算的是本輪剛進的貨,輪首快照
      看不見它——cma_gap>=S && ready>0 → flip min(cma_gap/S, SETTLE_BATCH=64)
      塊;floor 擋整批退 1 塊,再擋 0。
      SETTLE_BATCH = 最快進貨速率(=CHEAP_BATCH):flip 是 label+free 零遷移,
      速率配平後 avail 的瞬態鎖定上界 = 一輪進貨,大 reserve 冷建不再榨系統
      (舊制每輪 1 塊:7G/1G 冷建,6G 鎖定要 ~15s 才排完,lmkd 開殺)。
      早翻早好:avail 頁對系統不可用,cma 頁可被借;無悔——翻了就算 main 拿不滿
      也不比「拿滿才翻但拿不滿」差

  VERIFY_CMA: PENDING && (INSMOD||USER) → pool_verify_cma_params();
    FAILED → ctx.want_cma=want(當輪折回純池)。→ COLLECT_CMA

  COLLECT_CMA(湊塊,Q;授權跟 A_MAIN):需 cma_capable && cap(CONTIG_RANGE) &&
    reserve_target>0 && noncma_able>0 && gap_n>0;floor 同 MAIN。
    pop GAP → 可行性閘(同上)→ evict(同上)→ pool_ext_to_cand(pfn)
    (先入 cand 隔離不入池——直接入池 held 暫超 want 會叫醒 shed 拆台);
    湊滿 → private promote(兄弟歸隊 + 守恆 free)。段盡 → flush 殘餘 cand

  AVAIL_TO_CMA(收尾翻):條件 cma_capable && cma_gap>=S(舊三重 clamp 的單項寫法:
    cma_gap = min(−diff_want, reserve_target−cma))&& ready>0 && cma_floor_ok;
    每輪 min(cma_gap/S, FLIP_BATCH=8) 塊——大宗已由輪尾結算做完,這裡撿尾輪
    零頭與 floor 暫擋塊。
    條件不成立 → 本 phase 翻過 ≥1 塊則 kapi.drain_pages()(order-9 free_to_cma 落
    pcp,drain 後 NR_FREE_CMA_PAGES 才反映,floor/GUI 讀它)→ CMA_FREE

  CMA_FREE(縮儲備):需 cma_excess>0——丟 ext 只認真超額,絕不用 stage_in_budget
    (diff_want>0 時它含 stage_in 份額,丟掉是 custody 損失;這是兩個預算項
    必須分開的原因)。
    讓路:diff_want>0 && stages 含 CTA && stage_in_budget>0 &&(scan 未建 ||
    未 pop 乾)→ 跳過(excess 留給 stage_in 變現;本 run 沒建 scan 的 mid-run
    excess 留給下 run 的 CTA)。scan 已 pop 乾 → 不讓也不丟:"tried-all",
    剩下的塊同一支 contig 剛失敗過 → "cma sources stuck"。
    pool_cma_to_ext(min(DROP_BATCH=8, excess/S));退出 = 滿足或試完,無血量

  AVAIL_FREE(最後,surplus 處置自平衡):diff_want<0 時每輪二選一——
    (1) cma_gap>=S && ready>0 && floor 放行(整批退 1 塊,再擋則否)→ flip 一批;
    (2) 否則 shed 每輪 ≤SHED_BATCH(32)。
    物理:只有 shed 抬 noncma(flip 讓 MemAvailable 與 CmaFree 同步動),所以
    floor 缺口只有 shed 能償還,每 shed 一批門就墊開一分,中途解鎖後其餘 surplus
    就地翻入——大 avail 釋放時只 shed「floor 缺口」那段,不再整批經 buddy 裸奔
    等下次 acquire 買回(custody 風險 + 白做工;實測 15G 機 8.79G 全鎖時
    noncma≈965MB<floor,舊行為把 1925 slot 全 shed)。無新震盪源:此 phase 無
    cma→avail 動作,且沒有任何動作使 noncma 下降,floor 的門單向開啟。
    flip 落 pcp:finish 前 flipped_this_phase>0 補一次 drain(AVAIL_TO_CMA 出口
    drain 後歸零計數,各管各的)。
    shed 回 0(avail 空,多餘全在外)→ **不等,直接 finish**——它們回來時 return
    DROP gate 按當下目標放行,或下個 trigger 的 run 來 shed;沒這出口會空轉到
    看門狗上限。pipeline 終點:無條件 finish;未達標 = partial,g_stop_reason 記實況

adjust_finish()(所有 run=0 路徑同一條,冪等):
  pool_cand_to_ext() + pool_scan_finish();USER run 記 stop_reason
  (stop_reason 只反映 USER,背景 run 不蓋;acquire_active 反映任何 run,§10);
  next_profile 有 → init_ctx 接棒續跑
```

同一 run 的目標快照固定,三條 CMA 方向由 diff_want 的符號分區,數學上不互相抵銷:
CTA 只在 diff_want>0 開火,每成功 −S 趨零即停;輪尾結算/AVAIL_TO_CMA 只在
cma_gap>=S(⟹ diff_want<=−S)開火,每塊 +S、停在 (−S,0];diff_want 正轉負只能經
ready_budget 的 overshoot,而 CTA 不動 held+cma、new_page_need 對它不變,故
overshoot 上界恰為 cma 缺額。CMA_FREE 只吃 cma_excess>0 且讓路給仍可變現的
stage_in。各向單調趨向死區,同輪至多一向成立,無環。PREPARE 快照後新 flip 的塊
不屬本輪 cma_scan。已知有界 churn:case-3 中途 acquisition 轉順時,同 run 先 CTA
(付遷移)再結算回填(label+free,不同塊)——reserve 洗一輪;成本序 CHEAP→CTA
已把代價壓到最低,不值得為它破壞線性。

---

## 8. insmod / rmmod 序列

```
insmod:
  pool_size_max = min(ram − min(ram/2, system_reserve_mb), 表上限)
    (system_reserve_mb 預設 6144、下限 64,insmod-time 參數,§10:6G 裝得下重載
     Android 常駐集;temp-root/小 RAM 機種由 settings.prop 調低)
  讀 preflight 參數(load.sh 餵,§10):migrate_cma_val / pageblock_order_val /
    disable_kapi;解析 kapi、解出池頁所在 zone(CmaFree 讀取用);
    符號齊且值合法 → cma_params_state=PENDING,否則 UNAVAILABLE
    (preflight 只允許後續驗證,不授權 flip;capable = VERIFIED 的衍生查詢)
  走 RAM ranges(kapi.walk_ram,**兩趟:先數段數、按實數 kcalloc、再填**——無猜測
    上限可截斷;walk_ram 回傳總段數、超出 max 即代表沒填完。實測 OnePlus 15 有
    19 段且 13GB 主 bank 排在最後,曾被猜測的 16 段上限靜默截掉,池只蓋到 1.7GB
    低位碎段、prefill 全拒收。ranges 陣列 insmod 用完即 kfree;缺符號退
    min_low_pfn/max_pfn + pfn_valid 單段)
  → span_base 對齊 1GB、配 chunk;完整落在 range 內的 2MB 格 = EXT,其餘 NOT_USEABLE
  → 追加 NOT_USEABLE:**廠商 CMA area**(掃 pageblock migratetype==CMA——insmod 時
    我們還沒翻過任何塊,此刻的 MIGRATE_CMA 全是別人的;不讀 cma_areas[]:static、
    佈局逐版本變,不值)與 **ZONE_MOVABLE**(兩者同性質:GFP_KERNEL 拿不到、
    contig 隔離被擋、FOLL_LONGTERM 會被遷走)
  → 鎖初始化;clamp+對齊兩個 want(§4 同規則;UNAVAILABLE 且 with_cma≠0 →
    pr_warn + 清 0 純池)
  → **同步 prefill**:adjust_try(INSMOD) 後在 module_init 上下文反覆 adjust_round()
    到 finish(唯一允許連續跑的 run;load.sh/app 以「insmod 返回時池已建好」為前提)。
    cheap 整塊優先 → 池+儲備池一次建成(儲備池吃開機最乾淨的記憶體,由取法順序
    天然保證);拿不滿 A_FULL 補;再不滿 pr_warn + g_stop_reason。
    run 內 VERIFY_CMA 先驗;無安全窗 = PENDING 留給 USER 重試
  → 掛全部 hook(prefill 期間無 serve/released,晚掛不漏;儲備池的 order-9 free
    被 hook 看到但 state=CMA → MISS)
  → 套 unlock_cma lever(§9)、註冊 pinprobe

rmmod(全同步;順序是鐵律:先卸 hook → 停 worker → 交還參照):
  卸 hook(含 unlock_cma 的 gfp hook——先卸,下面 CMA 還原才不被 bypass 搶頁)
  release_run(0) + adjust_cancel() + cancel_work_sync ×2
  served_to_ext(ALL)(put 不可省:漏一頁 = 真洩漏)+ released_to_ext(ALL)
  cma→ext:3 pass × 100ms(每 pass 重探針;仍搶不回的塊**留 CMA 標籤續計**——
    label 與 freelist 一致,app 仍可用,只是拒 unmovable 到重開機,pr_err 記數;
    不用「只翻標籤」清場)
  cand_to_ext;avail_to_ext 批次迴圈;釋放 chunk 與 top[]
  (儲備池大時 rmmod 花幾秒到幾十秒,不是 bug)
```

---

## 9. unlock_cma(旁路元件)

廠商核心不會把一般 movable 配置放進 CMA(只認 __GFP_CMA)——沒有這塊,儲備池是
「監護著、沒人借得到」。作法:在配置路徑 flags 調整點掛 vendor hook,對單純 MOVABLE
請求放寬 ALLOC_CMA。

- **獨立旁路**:只用 kapi + pool 唯讀查詢(ready/cma/cma_capable、
  adjust_running),不動狀態機任何邊;單獨開關、單獨失效。
- **是賭注不是功能**:ALLOC_CMA 是 mm/internal.h 的 #define,無 BTF、preflight 驗
  不到、放寬無回傳。綁版本區間;驗證由使用者做(壓測看 lent);模組只報「掛上了」。
- 前置:儲備池已建好且採集靜止(prefill 已 finish 且**無任何 adjust run**)。
  判準是「這個 run 會不會碰 CMA 區」而不是「誰觸發的」:PRECISE stage 碰不到
  CMA,但 CTA/FULL 會,而**每個 profile 都帶會碰的 stage**(release 觸發的背景
  run 也有 CTA)——舊的「只擋 USER run」只是副作用剛好擋到一半(2026-08-28 改
  一刀切)。按 stage 分要從 lockless probe 讀 worker 的 racy 內部,不值;run 是
  單趟秒級,run 之間旁路全開,關機瞬間的短暫停借無感。
- **floor gate**:free CMA < cma_bypass_floor_mb(256)不放行(借光了自家 stage_in
  反而動彈不得)。
- 與 pool 的明文依賴:acquire 規定 GFP_KERNEL 系(§3.2),不帶 __GFP_MOVABLE 就不在
  放寬集合裡——哪天 acquire 改 movable gfp,unlock_cma 會讓 pool 撈自家儲備池
  (avail_in 擋下並記數,但那是最後防線不是授權)。
- lever 翻轉後 `kapi.drain_pages()`:6.6/6.12 的 static key 同時背著
  cma_has_pcplist,不 drain 會 strand CMA 頁在 pcp(實測)。key 在 rmmod 不回復
  (使用者政策,無安全前值)。

---

## 10. 對外 ABI:sysfs / 參數

三個外部消費者的解析行為固定,檔名/格式/錯誤碼是契約:管理 app(輪詢 refill_stat
逐行 key=value、讀寫兩個 want、按 acquire、比對 stop_reason 字串、統計讀取前寫
reconcile(一份 app 通吃兩代:對舊版模組是必要的同步對帳,對 v12 是 no-op)、
cma_usage 畫圖);load.sh(preflight 餵參數、多級 fallback 階梯);serve_test。

sysfs 讀取一律先取一次 `pool_stats_snapshot` / `pool_owner_stats_snapshot`,
不直讀欄位、不持鎖、單一輸出不混世代。

**檔案**(`/sys/module/gh_hugepage_reserve/parameters/`):

- `pool_want`(0600)/ `pool_want_with_cma`(0600):目標值;寫入規則與觸發見 §4。
  任何 run 進行中寫 → -EBUSY(故意:先 acquire=0、寫、重新 acquire,§4;
  acquire_active=1 即會被擋);
  with_cma:0=停用;UNAVAILABLE 且非 0 → -ENOSYS;PENDING 可寫(驗過才 flip)。
- `acquire`(0200):0=中斷;1=CONTIG_ANY;2=CONTIG_AT+EVICT_MEMCG(代號 A);
  3=CONTIG_AT+EVICT_ISOLATE(代號 B)。-ENODEV(insmod 未完)/-EBUSY(USER run 中)
  /-ENOSYS(缺符號:1 需 contig_pages;2 需 contig_range;3 另需
  folio_isolate_lru+reclaim_pages)。已達標回 0,reason="already at target"。
- `pool_avail` / `pool_cma` / `pool_avail_cma_able` / `pool_size_max`(0400):
  O(1) 查詢;avail_cma_able 於 !capable(含 PENDING)恆 0。
- `refill_stat`(0400):17 欄 key=value——state(idle/running,由 release run 導出)、
  pool_avail、pool_total(已證明容量,§5.1)、served(= served+released)、pool_want、
  total_served、total_refilled(= 五個 in_* 之和)、active_vms、
  acquire_active(= adjust worker **run != 0,任何 profile**:USER 按的、release
  觸發的背景 refill、SHRINK 都算——app 據此顯示「pool 正在調整」;它同時**就是**
  可寫訊號:=1 ⇔ 寫 `pool_want*`/`manual_refill` 會 -EBUSY ⇔ 旁路暫停借出,
  §4/§9 同一顆 bit)、acquire_mode / acquire_stop_reason(**只反映 USER run**——
  這兩個是使用者按的那顆 acquire 的模式與結束原因,背景 run 不碰)、refill_enable、
  free_reclaim(free hook 實際狀態)、pool_want_with_cma、pool_cma、
  pool_avail_cma_able、cma_pb_order(VERIFIED 才報,否則 -1)。
- `acquire_stop_reason` 字串集(app 逐字比對,固定):idle / acquiring /
  already at target / pool capacity full / cma headroom floor /
  cma flip failed (systemic) / stopped by user / reached target /
  reached target,with_cma / migration exhausted / cma sources exhausted /
  scanned all present memory / low-memory floor / evict-B unavailable /
  quality converged / cma sources stuck。
- `reclaim_debug`(0400):o9_seen/del_hit/del_miss/gate_drop/skip_unmovable/
  reject/orphan/purged/in_hook/in_sweep/in_cma/in_user/in_refill。
- `vm_owners`(0400):pid/vm_count/served/abandoned/comm 逐行。abandoned 語意
  (2026-08-28 起)= **pid 死亡時仍持有而被放手的頁數**;活著的 pid 恆 0——
  沒有 abandon-while-alive 概念,舊 app 的「vm_count==0 且 abandoned>0 = 疑似
  卡死」提示自然靜默(該組合不再出現)。stuck pin 的觀測改用:vm_count==0 而
  served>0 持續 = crosvm 收尾偏慢或卡住,頁仍在 SERVED 帳上。
- `served_summary`(0400):tracked/served/released/live/orphan/purged + per-owner
  行,讀時即時快照。per-owner 欄位名固定 **`pages=`**——舊 ABI 名,app 解析釘死;
  值是即時維護的 owner.served,只有標籤是歷史遺產。
- `purge_log`(0400,需 debug=1):放棄頁 pfn/refcount/現況——回答「池短了」和
  「手機丟了記憶體」的差別。
- `cma_usage`(0400,~1s 快取):儲備池佔用(free/anon/file MB、塊三態)。
- `reconcile`(0200):**舊版相容 no-op**——接受寫 1,什麼都不做。舊版把 per-owner
  統計與 ghost 清理放在同步的 reconcile pass 裡,舊 app 因此在每次統計讀取前
  (甚至每秒)寫它;v12 統計即時維護、放手走時戳,舊 app 想要的效果(下次讀到
  新數字)自動成立——**接受並忽略就是對舊語意最忠實的實作**,同時免疫舊 app
  輪詢造成的 state 抖動(armed 輪次壽命 ~1s,實測教訓 2026-08-28)。
  「現在跑一輪 release」= manual_release。
- `manual_release`(0200):寫 1 → release_run(1)。手動/診斷鈕,與 manual_refill
  對稱(催 release / 催 adjust);時戳判準使任何頻率無害、頻繁寫不會提早放手
  (§6)。合法用途:idle 時催收 sweep 失敗待 purge 的 released 殘頁。
  統計讀取前**不需要**它。
- `manual_refill`(0200):寫 1 → adjust_try(RELEASE);-ENODEV(refill_enable=0)
  /-EBUSY(任何 run 中——已有 run 在跑,催了不是靜默(撞 USER)就是白重啟
  (撞背景);sysfs 層自查後回)。
- `hook_enable` / `reclaim_enable`(0600):serve kretprobe / free tracepoint 開關;
  讀回**實際**掛載狀態。空池可開(與 insmod 的無條件掛載一致):serve hook 在
  avail==0 時由入口快濾閒置,晚開零損失、空轉只付探針稅——付不付是使用者政策,
  不設 gate。reclaim_enable=0 後歸還由 AS_PRECISE sweep / purge 承接(§6)。
- lever 三檔(§9):`moveable_to_cma_vender_already_allowed`(0400)/
  `moveable_to_cma_restrict_cma_redirect_disabled`(0600)/
  `moveable_to_cma_gfp_cma_hook`(0600)。

**模組參數**:preflight `disable_kapi` / `migrate_cma_val` / `pageblock_order_val`
(任一缺 → UNAVAILABLE;齊全只進 PENDING);`system_reserve_mb`(0400,預設 6144、
下限 64:§8 容量上限的系統保留,insmod-time,**下限**clamp 後讀回有效值——
`ram/2` 那道**上限**不寫回,故 8GB 機設 6144 讀回仍是 6144 而實際只留 4096);
`system_reserve_mb_default`(0400,init 算出:**本機**的預設保留量
`min(ram/2, 6144)`。設定值會蓋掉 system_reserve_mb,預設值只在這裡留存;
暴露的是裝置值而非內建常數,因為 app 要的門檻是「調低才警告」,而 8GB 機上
高於 4096 的設定全是 no-op。同一條 `gh_reserve_keep()` 算出,不會與生效值漂移);
`boot_acquire` / `boot_acquire_runs`(預設 1)/ `boot_acquire_wait`(預設 0)
(皆 **0600**:loader 政策的紀錄——存在 = 本包支援此 prop、值 = 目前設定的
mode/次數/秒數;模組三個都不據此行動,見 load.sh 契約。**可寫正是因為模組對它們
全盲**:寫入改變不了任何行為,而 app 需要一個「存檔後看得見」的落點——settings.prop
是 app 私有狀態,這三個檔才是使用者看得到的值。模組會據以行動的一律維持 0400
(system_reserve_mb 定的表只建一次,事後寫入只會說謊)。實際跑過的 mode 看
refill_stat 的 acquire_mode;EEXIST 重載後這三個是「最後被寫入的值」而非本次開機
所用);
floor:`cma_reservoir_floor_mb`(512)
/ `acquire_mem_floor_mb`(512)/ `cma_bypass_floor_mb`(256);`acquire_drop_slab`(1);
`debug`(0644);`sim_cma_order`(0400,測試:強制 S>1 簿記路徑在 S==1 裝置上可測,
relabel 仍逐真實 pageblock 做);`refill_delay_ms`(**接受即忽略**——載入腳本會餵,
拒絕會打斷 insmod 行、跌到 fallback 更低級);`refill_enable`(0600,預設 1:只 gate
RELEASE 的 A_CHEAP+A_FULL;free return、precise、CTA、EXPIRED 照常)。

**load.sh 契約**:階梯參數行含歷史名 `pool_target`(本模組**不得定義同名參數**——
一旦收下,「靠拒絕降級」的階梯機制反轉);settings.prop 鍵:`pool_want` /
`pool_want_with_cma` / `cma_movable_lever`(hook|flag → 兩個 lever 檔的 insmod 期望)/
`system_reserve_mb`(有設才傳:新增頂層階梯一級,舊 .ko 拒絕即自動落回原階梯)/
`cma_reservoir_floor_mb`(有設才傳,**掛在 v10 CMA 參數組**而非頂層一級:它是 v10
就有的參數,該跟 migrate_cma_val 那批同進同退。**必須走 insmod 而不是事後寫它的
0600 檔**——insmod 內的同步 prefill 已經在建儲備池(AVAIL_TO_CMA 無 stage gate,
儲備池吃開機最乾淨的記憶體),每次翻牌都問這個 floor;insmod 返回後才寫,晚了
一整個開機。能力偵測要注意:參數自 v10 就存在,**存在性證明不了本包的 load.sh
會傳它**——app 要靠重載後 readback 比對,或看 module.prop 的 versionCode)/
`boot_acquire`(0–3,預設 0:模組在位後 load.sh 對 `acquire` 檔寫入該 mode,
-ENOSYS 逐級降 3→2→1,失敗不致命——temp-root/碎片化開機的補拿)/
`boot_acquire_runs`(預設 1)/ `boot_acquire_wait`(秒,預設 0)。三者的
**政策都在 loader**(模組不據此行動);模組另定義三個同名 0400 參數作為**能力標記 +
readback**——檔案存在 = 本包的 load.sh 認識此 prop(app 能力偵測,承襲「參數存在
= 能力」慣例:app 測 v10 也是看 pool_want_with_cma 檔),值 = 本次開機 loader
餵入的設定(實際執行 mode 看 refill_stat 的 acquire_mode,被 -ENOSYS 降級時兩者
會不同);有設才餵,與 system_reserve_mb 同一頂層階梯級。機制(sysfs acquire)
已存在,模組不長 boot 政策;
它就是一個 USER run——app 進度照 refill_stat 顯示、`acquire=0` 可取消、進行中的
want 寫入照 §4 回 -EBUSY(改目標先 acquire=0);無「低於半目標才觸發」的 gate,
PREPARE 達標快查自會 O(1) 出場。

**開機按壓的三個維度**(temp-root 軟重啟是主場景:核心不重開、Android userspace
連同模組重載,而 userspace 倒下、GUI 未起的那一刻是模組能見到最乾淨的記憶體,
也是 temp root 唯一能上重壓的窗口):

- **按不按**——`boot_acquire`。條件是**模組在位**(`acquire` 檔存在)而非本次
  insmod 成功:軟重啟時 .ko 還在,insmod 回 EEXIST,把窗口賠給一個 EEXIST
  等於整個場景報廢。
- **按幾次**——`boot_acquire_runs`,前一 run 結束才按下一次。一個 run 是單向
  單趟(§7:drop_slab 一次 → cheap → full → sweep),游標後方被自己 evict 出來的
  窗只有**下一 run** 的 cheap 撿得到;重試屬於下一個 trigger 是設計,而開機路徑
  上本來沒有人按第二次。達標的 run 在 PREPARE 快查 O(1) 出場,多按一次不花錢。
- **等不等**——`boot_acquire_wait`(秒)。post-fs-data 是阻塞階段,**等**才是真的
  把 zygote 擋在後面讓 sweep 獨佔記憶體;不等(0)則 run 與開機賽跑,回到舊行為。
  預算有上限(Magisk 40s 殺逾時腳本),沒等完的 run 照樣在背景跑完。阻塞屬於
  開機路徑:post-fs-data.sh 以 `GH_BOOT=1` 呼叫 load.sh 才准等,app 的執行期
  「啟用」跑同一支腳本但永不阻塞(該按的照按,直接交給背景續按)。

三個都是 settings.prop 鍵 = 持久化在 /data,軟重啟自動重讀;預設
(0 / 1 / 0)就是本節上文描述的原行為。

---

## 11. 設計邊界與固定值

**不做**:單池+全池排序(排序修復 = ABA 寄生點;custody 讓屬性只在邊界事件變動);
持久塊節點層(per-page 表 + 即時導出使其冗餘);平面/稀疏偵測二選一(兩層定址
無條件用);CMA 候選精確全排序(occupied 本是 racy 估值,固定 0+八級 bucket);
為失敗開新路徑(§原則 4)。

**已知邊界**:cma→avail/ext 永久失敗(長期 pin)→ SKIP_THIS_SCAN + hp 上限 +
partial,塊留 CMA 續計;**crosvm 活著期間 stuck pin 一律不放手**——頁留 SERVED、
池就短著(held 含它,帳面不開洞、不觸發 refill 買替代),放手的唯一事件是 pid
死亡。這是刻意的:舊制 idle_since+10s 在「1→0 蓋戳→放手→0→1 重開」上有誤放手
/誤標 abandoned 的競態,事件驅動無此窗口;代價是 stuck 期間池短,由「重啟 crosvm
即恢復」承擔(§2,2026-08-28 定案)。pid-exit tracepoint 萬一 attach 失敗(不應
發生,core tracepoint)→ 死亡放手退化為 owner sweep 的 mm_users==0 偵測(表滿
兜底),正確性不變只是慢。

**固定值**:entry 20B(不做 union 壓縮);chunk 1GB;GRACE=10s(僅
slot.released_at 的 purge 時鐘);DEATH_GRACE_SEC=GH_RELEASE_ROUNDS−2=3s
(owner 死亡放手的寬限,時鐘單向、與 ROUNDS 配對,_Static_assert 擋 ROUNDS<2);
GH_RELEASE_ROUNDS=5(載具下限,同時決定 DEATH_GRACE);ADJUST_RUN_MAX=120000;
PRECISE_BATCH=16 / CHEAP_BATCH=64 / FULL_BATCH=16 / SWEEP_SCAN=256 / CTA_BATCH=8 /
FLIP_BATCH=8 / DROP_BATCH=8 / SHED_BATCH=32 / A_STRIDE=8;CMA verify delta 門檻
384 頁、CMA_VERIFY_MISS_MAX=8;S≥16 不支援(保住 atomic 掃描小常數上界)。

**實測依據(設計輸入,不是猜測)**:84% 的 2MB 窗有 hard straggler(→ white-kick
閘、缺口跳過);early-boot 是唯一拿滿時機(→ 同步 prefill + 整塊優先);每 VM
pcp-lag 漏 ~2 頁(→ 第 3 輪 drain);連續 CMA grab ~10s/8GB(→ CTA_BATCH);
sweep 不設 memory floor 會 RCU stall(→ acquire_mem_floor);dma_heap 的 unmovable
order-9 永不回 buddy(→ serve gfp 濾);unmovable 工作集 ~3.5G 進不了 CMA
(→ cma_reservoir_floor);6.6/6.12 lever 翻轉不 drain 會 strand pcp。

**實作紀律備忘**(實測換來,逐條核):jiffies-wrap 安全時戳;verify 的 label 寫讀回
驗失敗 → -EIO ⇒ UNAVAILABLE + with_cma=0 + pool_total clamp;每個提早停止都有具體
reason 字串的 pr_warn(沉默降級付過代價);cond_resched 批次 32~64;工具鏈地雷:
hweight8/strlen 變 libcall、snprintf 避用、5.10/5.15 的 -Wdeclaration-after-statement。

**CI**:Tier-0 mock harness(38 情境 + race harness)測狀態機/預算/驗證協定/
hook-less 迴路;Tier-1 QEMU 測掛載與真頁;真機測 atomic/鎖序/真實遷移。
