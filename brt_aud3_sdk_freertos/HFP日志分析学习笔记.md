# HFP 通话日志分层分析方法（学习笔记）

> 目的：以后拿到一份 HFP 相关 log（`COM8_*.txt` 之类），能快速判断"某 AT 事件为什么上报 / 为什么没上报"。
> 适用范围：本项目 brt_aud3_sdk_freertos HFP 固件（开了 `CONFIG_HF_SUPPORT_3_WAY_CALL`）。
> 注意：文中源码行号以当前工作区为准，代码改动后行号会变，请以函数名+log 关键字为准。

---

## 0. 关键结论速记（不想看全文就看这）

1. **`+HFPMTPX` 与 `+HFPSTAT` 是两套不同的上报格式**，不是同一件事的不同写法：
   - `+HFPSTAT=N[,号码]`：单方"主状态机"视角（连接状态 2/3，通话 4/5/6）。
   - `+HFPMTPX=index\xffstate\xff号码`：多方 3-way 通话视角，state 用 0xff 分隔。
2. **固件开着 3-way 宏时，CLCC（event=28）通道的通话只会报 `+HFPMTPX`**，不会出 `+HFPSTAT=4/5/6`。
3. **`+HFPSTAT=4/5/6` 何时出**：
   - AG 直接发呼叫事件（`+CIEV`/`+CLIP`，协议栈上报 event=8/7/6）→ `hlp_hf_dial_ind_ex` → 出 4/5/6；
   - 或固件在 3-way `change_state` 新建第一路节点时**补发** `EV_HF_OUTGOING/INCOMMINGCALL/ONGOING`（就是 L402-412 的三行补丁）。
4. **补丁为什么加在 internal_hf.c（业务层）而不是 evt_parse.c（格式层）**：是否该覆盖主状态是业务判定（index==1 && 状态机在 CONNECTED），evt_parse 只有 `p_call`，没有主状态上下文，且第 2/3 路不能误覆盖。
5. **判断底层事件来没来，就看 `hlp_hf_event_cbk state=%d, event=%d`**；它来了但没出字符串，原因在它之后的内部分支/过滤里。

---

## 1. 上报链路（5 层）——最重要

一条底层数据变成应用层可见的 AT 字符串，要穿过 5 层。**每层用不同日志关键字识别。**

```
① HCI/RFCOMM 收 AG 的 AT 原文        bse_stack - [HCI][*Recv] ... 2B 43 4C 43 ...(=+CLC)
② 协议栈解析→回调 app               bse_hfp - hlp_hf_event_cbk state=%d, event=%d   ←【入口日志】
③ 业务层状态/事件转换                bse_hfp - you set %d / not event hfp stat %d
                                     / hlp_hf_3_way_call_change_state / event_hf_indication
④ manager 层转发                    bse_mgr - bt_hf_cbk, event = %d
⑤ 拼字符串给应用层【应用层唯一天花板】 at_event - event_buffer:+XXX=...
                                     后面 fsc_bt_callback_ec / kkkkk / [CARLINK-BT] 是应用层消费
```

### 各层实例对照（客户 COM8 log）

| 层 | 关键字 | 实例行 | 说明 |
|---|---|---|---|
| ① | `bse_stack - [HCI][*Recv]` | L10587 附近 `LCC: 1,0,4,...` | AG 回的 +CLCC 原文 |
| ② | `hlp_hf_event_cbk state=5, event=28` | L10584 | event=28 = CURRENT_CALLS_IND（CLCC 结果） |
| ③ | `hlp_hf_3_way_call_change_state, 1, 4, 号码` | L10612 | 解析出 idx=1,state=4 |
| ④ | `bt_hf_cbk, event = 24` | L10614 | event=24 = EV_HF_3_WAY_CALL_NEW |
| ⑤ | `event_buffer:+HFPMTPX=1\xff4\xff号码` | L10618 | 应用层实际收到的字符串 |

**分析方法要诀：**
- 应用层"收到没有" = ⑤ 有没有 `event_buffer:+XXX`。
- 底层"事件来没来" = ② 有没有 `hlp_hf_event_cbk`（L1345，每个堆栈事件必打）。
- ② 有、⑤ 无 → 原因一定在 ③④⑤ 之间的判断/过滤/改道，重点查 ③。

---

## 2. 判断决策树（某状态没上报时）

```
某 AT 状态没上报（例如 +HFPSTAT=4/5）
│
├─ ① 搜 log：有没有对应 hlp_hf_event_cbk state=?, event=?   ← ②层入口
│     （想看来电/去电：找 event=7/8/6 直接事件，或 event=28 + 上面 +CLCC 原文）
│     │
│     ├─ 没有 → 底层没产生该事件
│     │     再往下看 ①层有没有收到对应 AT（+CLCC/+CIEV/+RING）
│     │     都没有 → AG 侧没发或时序问题（不是本机 bug）
│     │
│     └─ 有 → 事件到了 app，在 ③④⑤ 排查"为什么没转成字符串"：
│           a. set_status 被拦？→ "not event hfp stat %d"（status>=READY 一律拦截）
│           b. 走了别的上报通道？→ 3-way 路径只出 +HFPMTPX（改道）
│           c. 分支被 hf_num 空/非空跳过？→ L930 delay-500 / L1099 空号判断
│           d. #ifdef 宏分支不同？→ CONFIG_HF_SUPPORT_3_WAY_CALL
└─ 结论：是"事件没产生"还是"产生了但被中间层吞掉/改道"
```

---

## 3. +HFPSTAT 与 +HFPMTPX 的区别（evt_parse.c）

### 3.1 两套通道对照

| | `+HFPSTAT` 通道 | `+HFPMTPX` 通道 |
|---|---|---|
| 触发 BT_EV | OUTGOING / INCOMMINGCALL / ONGOING；另有连接状态 CONNECTING/CONNECTED/DISCONNECTED | 3_WAY_CALL_NEW / STATE_IND / DONE |
| param | `(char*)param` 裸号码 | `hf_3_way_call_t*`（index/state/number） |
| 格式 | `+HFPSTAT=N,号码` 逗号分隔，**状态刻在指令号** | `+HFPMTPX=index\xffstate\xff号码`，0xff 分隔，**状态是字段** |
| state 含义 | 4=去电 5=来电 6=通话中（指令号本身） | 0=active 1=hold 2=outgoing拨号 3=warning 4=incoming来电 5=waiting；DONE 硬编码 6=结束 |
| 代码位置 | evt_parse.c L450-506 | evt_parse.c L569-594 |

> 注意注释残留：L495/500/505 里被注释的旧代码，说明单通话早期也用 `+HFPMTPX` 格式，后来改成 `+HFPSTAT`。

### 3.2 +HFPSTAT 实际是两个不同子通道（关键澄清）

| 子通道 | 事件 | 形态 | 何时触发 |
|---|---|---|---|
| 连接状态 | EV_HF_STAT→BT_EV_PROFILE_HFPSTAT（evt_parse L1612） | `+HFPSTAT=%d` **无号码** | 状态机 <READY 时 set_status 才发；连接事件 CONNECTING→2、CONNECTED→3（evt_parse L458/L486） |
| 通话状态 | EV_HF_OUTGOING/INCOMMINGCALL/ONGOING（evt_parse L492-506） | `+HFPSTAT=4/5/6,号码` | 呼叫事件产生时 |

**最容易踩的坑**：`set_status` 对 `status >= ST_HF_READY(2)` 一律 `return`（只打 "not event hfp stat"），所以 `set_status(ST_HF_OUTGOING)` **不会**出 `+HFPSTAT=4`。出 `+HFPSTAT=4` 的是紧邻的 `event_hf_indication(EV_HF_OUTGOING, 号码)` 那一行。

### 3.3 set_status 源码语义（internal_hf.c L790-811）

```c
void hlp_hf_set_status(UINT8 status) {
    HF_STATE = status;
    int even_stat = status;
    if(status > ST_HF_INIT) even_stat = status - 1;
    if(get_g_bt_opened_status() && even_stat < ST_HF_OUTGOING) {
        if(status >= ST_HF_READY) {          // status 2..11 全被拦
            bse_log_api(TAG, "not event hfp stat %d", status);
            return;
        }
        event_hf_indication(EV_HF_STAT, even_stat);   // 只 status 0/1 能走到
    }
}
```
→ `set_status` 只负责**改内部状态机 + 上报低层连接状态**，通话状态上报另走 EV 事件，别混。

---

## 4. hf_num 的作用（决定走 delay 还是直接报）

### 4.1 填充点（拿到号码）
| 位置 | 场景 |
|---|---|
| internal_hf.c ~L1026-1029 `current_calls_ind` | 每次 event=28（+CLCC 结果）**一进来就填** |
| ~L996-1000 `dial_num_ind` | CLIP 来电带号 |
| ~L941-964 `dial_ind_ex` switch | 兜底填 `"000000000"`（实际是死代码，见下） |

### 4.2 清空点（变回空）
| 位置 | 场景 |
|---|---|
| ~L977 `hlp_hf_dial_ind` | 发起新拨号时清空并查 CLCC |
| ~L1095 `current_calls_ind` | 状态变化时清空 |
| ~L906 `hlp_hf_standby` | 通话结束回 standby |

### 4.3 为空时的行为（dial_ind_ex L930-936）
```c
if(s_hf_user_data->hf_num[0] == '\0') {
    // 号码还没就绪 → 挂 500ms 定时器重查 AT+CLCC，直接 return
    if(!s_ft_hf_get_calls)
        s_ft_hf_get_calls = FsmAddTimer(NULL, 500, 0, NULL, NULL, hlp_hf_get_currnet_calls_timer);
    return;   // ← 不打印 "event is"，不进 switch
}
bse_log_api(TAG, "hlp_hf_dial_ind_ex event is %d", event);   // 只有号码非空才到这句
switch(event) { ... }   // 号码非空才走 switch → 出 +HFPSTAT=4/5/6
```

**结论**：
- 号码空（如被叫 RING 先到、号码晚到）→ delay-500 → 500ms 后查 CLCC（event=28）→ 走 3-way 通道 → 报 `+HFPMTPX`。
- 号码非空（如 AG 先给 CIEV/CLIP 且号码已在 hf_num）→ 直接进 switch → 报 `+HFPSTAT=4/5/6`。

### 4.4 死代码：L941-944 的 "000000000" 兜底
只要过了 L930 守卫，`hf_num[0]!='\0'` ⇒ `strlen>0` ⇒ L941 `if(strlen<=0)` **永假** → L941-944（以及 L950-953、L959-962 两处）是**不可达死代码**。它是 L930 delay 守卫加入前"空号占位"的历史遗留，现已被 delay-500 兜底取代，可删不影响行为。

---

## 5. change_state 详解（internal_hf.c L354-419）

```c
p_node = NewListFind(call_list, &idx, 1);   // 按 index 找节点
if (p_node != NULL) {   // ① 老节点：该 index 已存在（之前 NEW 过）
    p_node->state 变化 → STATE_IND / state==0 走 active 特殊处理
} else {                // ② 新节点：该 index 第一次出现
    新建节点入链表 → EV_HF_3_WAY_CALL_NEW → +HFPMTPX
    [L402-412 补丁] index==1 && 状态机==CONNECTED → set_status + EV_HF_xxx → +HFPSTAT=4/5/6
}
```

**重要澄清**：
- `p_node != NULL` 判断的是"**这一路（index）之前有没有被记录过**"，**与号码是否为空无关**。号码空时照样创建节点走对应分支。
- 同一 idx 的 CLCC 第二次来（节点还在链表中）→ 走①更新状态；通话结束节点被清 → 再来就走②新建（客户 log 两段 event=28 都是②就是这原因）。

### 为什么补丁加 L402-412 而不是 evt_parse L569-594
1. **分层职责**：evt_parse 是纯翻译层（一个 case→一条 AT），internal_hf 是业务状态机层（决定"发生了什么事件"）。
2. **判据只有业务层有**：`hlp_hf_get_status()==ST_HF_CONNECTED`、`p_node->index==1`，evt_parse 手里只有 `p_call`，没有主状态上下文。
3. **3-way 事件 ≠ 单方主状态**：将来第 2/3 路（index=2，如 call waiting）也会触发 `EV_HF_3_WAY_CALL_NEW`，不能覆盖 +HFPSTAT 主状态——"只有 index==1 的第一路才推进主状态机"必须在业务层判断。
4. **单一事件源**：补丁与 L399（NEW 上报）同处一个业务点，两个视角（多方 HFPMTPX / 单方 HFPSTAT）从同一事实发出，好排查。

---

## 6. 两份真实 log 复盘（最佳教材）

### 6.1 客户 log（COM8_202692133757.txt）—— 全程只有 +HFPSTAT=2/3 与 +HFPMTPX
- `+HFPSTAT=2`（L1259）：`bt_hf_cbk event=3`（=EV_HF_CONNECTING）→ evt_parse L458。
- `+HFPSTAT=3`（L1813/L11069）：`bt_hf_cbk event=5`（=EV_HF_CONNECTED）→ evt_parse L486。
- 来电（第一段 event=28）：CLCC `dir:1 status:4` → change_state 新建 → `+HFPMTPX=1\xff4\xff号码`，`you set 7`（=INCOMING），**无 +HFPSTAT=5**。
- 去电/拨号（第二段 event=28）：CLCC `dir:0 status:2` → change_state 新建 → `+HFPMTPX=1\xff2\xff号码`，`you set 6` + `not event hfp stat 6`，**无 +HFPSTAT=4**。
- 挂断：`bt_hf_cbk event=26`（=3_WAY_CALL_DONE）→ `+HFPMTPX=1\xff6\xff号码`（DONE 硬编码 6），然后 `you set 5` 回 CONNECTED → `+HFPSTAT=3`。
- 全篇 **grep 不到 event=6/7/8** → 客户环境从未走直接事件通道，号码全靠模块 500ms 定时器查 CLCC。

### 6.2 用户自测 log（log.txt）—— 出了 +HFPSTAT=4/6
```
L1690-1691  手机发来 +CIEV: 2,2        ← AG 主动报"开始去电"（不是 CLCC！）
L1696  hlp_hf_event_cbk state=5, event=8   ← HFP_EV_OUTGOINGCALL_IND 直接事件
L1697  hlp_hf_dial_ind_ex event is 8       ← 直接事件通道
L1701  bt_hf_cbk event = 7                 ← = EV_HF_OUTGOING
L1705  +HFPSTAT=4,13267216950              ← 出 4！
```
L2315 的 `+HFPSTAT=6,号码` 同理，是接通后的 event=6（ONGOINGCALL_IND）。

### 6.3 差异根源一句话
| | 用户手机 log.txt | 客户 COM8 |
|---|---|---|
| AG 怎么报通话 | 主动发 `+CIEV` 直接事件 | 不发直接事件，等模块查 +CLCC |
| 底层事件 | event=8 / event=6（直接） | 只有 event=28（CLCC） |
| 应用层结果 | **+HFPSTAT=4/6** | **+HFPMTPX** |

> 启示：如果客户现场手机不发 CIEV 直接事件，即使加了补丁，来电/去电仍只会看到 +HFPMTPX（除非补丁让 CLCC 通道也补发 +HFPSTAT，见 L402-412）。

---

## 7. 速查表：想看某个上报，该搜什么

| 想看应用层收到 | ②层搜（hlp_hf_event_cbk） | ①层/③层确认 |
|---|---|---|
| +HFPSTAT=2/3 | 连接成功事件 | `bt_hf_cbk event=3/5` → 转串 |
| +HFPSTAT=4(去电) | event=8（OUTGOINGCALL_IND）或 event=28 | CLCC `dir:0,status:2/3`；3-way 版默认只出 +HFPMTPX |
| +HFPSTAT=5(来电) | event=7（INCOMINGCALL_IND）/CLIP 或 event=28 | CLCC `dir:1,status:4/5`；3-way 版默认只出 +HFPMTPX |
| +HFPSTAT=6(通话中) | event=6 或 event=28 | CLCC `status:0` |
| +HFPMTPX | event=28 | `hlp_hf_3_way_call_change_state` 出现 |

### 相关枚举速记
- HFP 直接事件（hfp_ui.h）：6=ONGOINGCALL_IND、7=INCOMINGCALL_IND、8=OUTGOINGCALL_IND、28=CURRENT_CALLS_IND（CLCC 结果）、26=ATCMD_RESULT。
- 内部状态（auto_hf.h ST_HF_*）：0=UNINIT 1=INIT 2=READY 3=DISCONNECTING 4=CONNECTING 5=CONNECTED 6=OUTGOING 7=INCOMING 8=ONGOING 9=HELD 10=HUNGUPING 11=END。
- 内部事件（auto_hf.h EV_HF_*）：3=CONNECTING 4=DISCONNECTED 5=CONNECTED 7=OUTGOING 8=INCOMMINGCALL 10=ONGOING 24=3_WAY_CALL_NEW 25=3_WAY_CALL_STATE_IND 26=3_WAY_CALL_DONE 37=STAT。
- bt_hf_cbk 里 event 号 == EV_HF_* 号；EV_HF_STAT→BT_EV_PROFILE_HFPSTAT(+HFPSTAT=%d 无号码)。

---

## 8. 涉及源码关键位置清单

| 文件 | 位置 | 作用 |
|---|---|---|
| sdk/auto_app/internal_hf.c | L90-109 | event_hf_indication：统一事件出口 |
| | L354-419 | hlp_hf_3_way_call_change_state：CLCC 通话状态机（新/老节点） |
| | **L401-413** | **补丁：第一路 new node 补发 EV_HF_OUTGOING/ONGOING/INCOMMINGCALL** |
| | L790-811 | set_status：改状态机 + "not event hfp stat" 过滤 |
| | L813-822 | get_status：读状态 |
| | L930-936 | dial_ind_ex：hf_num 空 → delay-500 兜底 |
| | L941-964 | dial_ind_ex：号码非空 → switch 直接发 +HFPSTAT（其中 941-944/950-953/959-962 为死代码） |
| | L1017-1119 | hlp_hf_current_calls_ind：event=28 总入口（hf_num 提前填充→L1099 补发机制失效） |
| | L1339-1345 | hlp_hf_event_cbk：②层入口日志 |
| sdk/auto_at/evt_parse.c | L450-506 | +HFPSTAT 连接/通话事件 → 字符串 |
| | L569-594 | +HFPMTPX 3-way 事件 → 字符串 |
| | L1612-1614 | BT_EV_PROFILE_HFPSTAT → +HFPSTAT=%d（无号码） |
| sdk/auto_app/bt_manager.c | L1741-1742 | EV_HF_STAT → BT_EV_PROFILE_HFPSTAT |
| sdk/auto_app/auto_hf.h | L25-83 | ST_HF_* / EV_HF_* 枚举 |

---

## 9. 复盘自测题

1. 为什么客户 log 只有 +HFPSTAT=2/3，却出现了 +HFPMTPX=1\xff4 / 1\xff2 / 1\xff6？
   - 答：3-way 宏开启 → 所有 CLCC 通话走 change_state 只出 +HFPMTPX；2/3 是连接状态通道与通话无关；4/2/6 是 HFPMTPX 的 state 字段（来电/去电/结束），编号体系与 HFPSTAT 不同。
2. `set_status(ST_HF_OUTGOING)` 会不会上报？
   - 答：不会。status>=READY(2) 被 "not event hfp stat" 拦截；出 +HFPSTAT=4 的是下一行 event_hf_indication(EV_HF_OUTGOING,...)。
3. L930-936 与 L941-944 什么关系？
   - 答：L930 空号直接 return；能到 L941 说明号码非空 ⇒ L941 永假 = 死代码。
4. change_state 的 if/else 分什么？
   - 答：按 index 查链表：老节点更新 / 新节点创建，与号码是否为空无关。
5. 为什么补丁不放 evt_parse.c？
   - 答：缺主状态上下文 + 第 2/3 路会误报 + 违反格式层单一职责。

---

## 10. 下次拿到新 log 的动手流程

1. 先全局 grep `event_buffer:+` 汇总应用层收到了哪些事件 → 对照 AT PDF 期望清单找缺口。
2. 缺口事件 → grep 对应 `hlp_hf_event_cbk ... event=N` 判断底层是否产生。
3. 产生了但没上报 → 顺着 ③④ 层看：`you set`/`not event`/`change_state`/`bt_hf_cbk event=`，定位被哪层吞掉或改道。
4. 底层都没产生 → 去 ① 层看 HCI/RFCOMM 里有没有对应 +CLCC/+CIEV/+CLIP 原文，判定是 AG 侧问题还是时序问题。
