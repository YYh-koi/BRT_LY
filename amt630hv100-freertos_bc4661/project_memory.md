# 项目记忆 project_memory.md

> 用途：跨会话恢复上下文。打开本文件即可接续上次工作。
> 最近更新：2026-09-03（补充三大仓库业务架构速览）

## 一、项目概览
- 平台：AMT630HV100，嵌入式 FreeRTOS 固件，AWTK 作为应用 UI，IAR 8.30.1 编译。
- 工作区：`proj/amt630hv100.eww`；AWTK 批处理 `amt630hv100-awtk`，LVGL 批处理 `amt630hv100-lvgl-DDR32X16`。
- 打印/调试串口：**UART0（IO38/IO39，115200）**。

## 二、本会话已完成改动

### 1. 蓝牙串口改为 UART1 + 指定引脚
- 文件：`ArkmicroFiles/libboard-amt630hv100/include/board.h`（第215行）：启用 `#define UART1_FLOW_CTL`
- 文件：`ArkmicroFiles/libboard-amt630hv100/source/pinctrl.c`（第169-175行）：
  - 开流控（4 脚）：`RX=41, TX=40, CTS=46, RTS=47`
  - 关流控（2 脚）：`RX=41, TX=40`
- 已对照核心板原理图 `ly a630 v1核心板.pdf` 验证一致。

### 2. Data Abort 定位与 romfile 防御性加固（核心）
- 崩溃现象：断点停在 `board_lowlevel.c` 的 `Abort_C_Handler` 的 `while(1);`，串口无输出。
- 现场读取（经全局 volatile 变量保存）：DFAR=`0x00100030`、DFSR=`0x05`(Translation fault-section)、LR=`0x200C583E`(strncmp)、caller=`0x2010108c`(RomFileDirExist)、r0=`0x00100030`(=DFAR 坏指针)、r1=合法RAM、r2=`0x6`(=strlen)。
- 根因链：AWTK `fs_os_dir_exist()` → `RomFileDirExist()` → 解引用未初始化的 `romheader`（NULL/非法）→ `strncmp` 访问 `0x00100030` 触发 Data Abort。
- 深层根因：`ReadRomFile()` 在 update file 中未找到 ROMA 段时直接 `return -1`，`romheader` 保持 NULL/悬空，但 AWTK 仍提前调用目录检查；旧函数无判空直接遍历 `romheader->filenum`。

## 三、改动文件清单（已落盘）
- `ArkmicroFiles/libboard-amt630hv100/source/romfile.c`：
  - 新增静态助手 `romheader_valid()`：判空 + RAM 范围校验（`< 0x20000000` 判非法），带一次性诊断打印 `[ROMFILE DIAG]`（含指针值 + `__get_LR()` 调用者）。注：IAR 无 `__return_address`，用 `__get_LR()`。
  - `ReadRomFile()`：magic 失败处 `romheader = NULL`；新增 `filenum` 合理性校验（`==0 或 >0x1000` 判非法）。
  - `RomFileDirExist()` / `RomFileExist()`：开头 `if (!romheader_valid()) return 0;`
  - `RomFileOpen()`：开头 `if (!romheader_valid()) return NULL;`
- `ArkmicroFiles/libboard-amt630hv100/source/board_lowlevel.c`：
  - `Abort_C_Handler` 增加全局 volatile 现场保存：`g_abort_v1/v2/lr/dfsr/caller/sp/spsr/r0-r3/stack[]`。
  - 关键技巧：r0-r3 必须在函数最顶部第一条 asm 捕获；局部变量勿 `=0` 初始化（IAR 优化会覆写崩溃现场）。加 `if (dfsr > 21) dfsr = 0;` 防数组越界。

## 四、关键符号/地址（proj/AWTK/List/amt630hv100.map）
- `romheader` 静态变量：`0x20f46ab4`；`romfileMutex`：`0x20f46ab8`；`romoffset`：`0x20f46abc`
- `romheader_valid`：`0x20100a7c`；`ReadRomFile`：`0x20100ab0`；`RomFileOpen`：`0x20100c00`；`RomFileExist`：`0x20101008`；`RomFileDirExist`：`0x20101074`
- 系统 RAM 基址 `0x20000000`；堆 `ucHeap` 位于 `0x20f604e4`，大小 `0x2200000`
- `.map` 地址带撇号分隔（如 `0x2010'108c`），Grep 时用撇号格式。

## 五、验证结果（debug/log.txt）
- 修复后 Rebuild 正常启动，`Memory free: 26804992 bytes.` 每 10s 刷新，无 Data Abort。
- log 中 `[ROMFILE DIAG] romheader == NULL, call from 0x2010108c` 命中根因且防护生效。

## 六、遗留问题（未处理）
- **AWTK 资源缺失**：update file 无 ROMA 段（`Warning! Not found romfile in update file.`），AWTK 启动报 `Not found valid assets folder!` / `!!!Asset [name=home_page/home_page type=ui] not exist!!!`。属 ROM 打包/资源路径问题，与本次崩溃无关，用户选择先收尾、稍后单独处理。

## 七、调试经验沉淀
- IAR 优化致局部变量 `<unavailable>`，须用 volatile 全局变量保存断点现场。
- 捕获崩溃点通用寄存器要在函数第一条 asm 语句做，局部变量勿 `=0` 初始化。
- `strncmp(s1,s2,n)` 参数映射（AAPCS）：`r0=s1, r1=s2, r2=n`。
- 平台 RAM 基址 `0x20000000`，低于此的指针可判为非法（防御性校验可行）。
- GPIO 编号（40/41/46/47）映射到 `amt630h_pin_map`，与原理图核对。

## 八、三大仓库业务架构速览（2026-09-03）

### 业务定位
- 产品 = 车载互联（Carlink）投屏盒子。当前产品配置见 `ArkmicroFiles/libboard-amt630hv100/include/board.h`：`CARLINK_CLCAST=1`（EY/EC/CP/AA 全 0，互斥）、`BT_STACK_BARROT=1`、`WIFI_CHIP_GT=1`、`USE_LWIP=1`。
- 硬件：BT 走 UART1（TX=IO40/RX=IO41，流控 46/47），BT 复位 IO94，WiFi 复位 IO95；调试串口 UART0（115200）。

### 1. brt_aud3_sdk_freertos —— 蓝牙协议栈（Barrot）
- 运行在 630HV100 上，经 UART(H5/HCI) 驱动外挂 BT 模块；提供 BLE GATT/A2DP/AVRCP/HFP 等 profile，业务以 AT 命令+事件文本上抛。
- 应用侧固定契约 = `brt_bt/brtbt_interface.h`：`brt_bt_main()` 起 task → `brlinkd_stack_init/run()`；上行发 AT `brtbt_send_at_command()`；下行事件 `brtbt_register_event_cb()`（如 `+GATTHANDLE/+CLCASTCCCD/+GATTDATA`）。
- 主工程侧胶水 = `lib/bt/brt_bt/brt_bt.c`（与 SDK 内 `brt_bt/brt_bt.c` 同源），其中按 `CARLINK_CLCAST/CARLINK_EC` 宏选 BLE 连接类型（CLCast=2）。
- 配套文档：`CLCAST_UPPER_APP_INTERACTION.md`（FFE0 服务 FFE1 读/FFE2 写/FFE3 通知 + AT 查询契约）。
- 注意：SDK 里 `app_task.c/app_ble.c`+br1601x 是"模块自带 SDK"示例，与跑在主机侧的 brlink 是两套东西，勿混淆。

### 2. atbm_rtos_wifi6 —— WiFi6 模块（GT/atbm）
- 提供驱动/HAL+协议栈，对外暴露 `atbm_api`/`atbm_wifi_driver_api`；app 侧包装为 AWS 风格 `lib/wlan/iot_wifi.h`，适配实现 `lib/wlan/atbm_wifi6/iot_wifi_atbm6165.c`。
- 业务上跑 P2P GO/AP（IP 192.168.13.1，TCP 端口 9000），承载 CLCast 视频 TCP 会话。

### 3. amt630hv100-freertos_bc4661 —— 应用主工程（业务层）
- 入口 `app/main_awtk.c`（AWTK UI），按 CARLINK_* 宏选业务并 `set_carlink_display/video_info`。
- 业务调用链：`carlink_clcast_init()` → `app/carlink/common/carlink_bt_wifi.c` 的 `carlink_common_init()`+`carlink_bt_wifi_init()`（同启 BT task 与 WiFi/lwip，注册驱动事件，把 BT AT 文本解析为类型化事件，解析在 `carlink_clcast_ble_at.c`）。
- CLCast 主流程（详见 `.scratch/clcast-ble/PRD.md`）：BLE bootstrap 二维码 → 手机扫码 → BLE 连接 → BT 栈注册 0xFFE0 服务，FFE3 notify 按序发 B0~BA 参数帧（B0 MAC/B1 名/B2 仅地图/B3 分辨率/B5 MJPEG/B6~BA P2P 参数）→ 手机连 P2P → TCP 9000 建会话 → MJPEG 投屏（平台 JPEG 硬解/缩放/显示）。BLE 断开不影响已建立投屏。
- 备选业务/方案（当前不用）：EY 亿联、EC、CP(CarPlay)、AA(AndroidAuto)；fsc_bt(blueware) 备选 BT 栈；RTL8821CS 等备选 WiFi。

## 九、brlink(Barrot) log 速查表（2026-09-03，依据 brt_aud3 sdk 源码 + 1.txt 真实 log）

### 0. 怎么读
- 日志行三段：`[时间]收←◆模块TAG - 正文`。时间戳/方向是串口工具加的，固件只打印 `TAG - 正文`。
- 找源码：grep `#define TAG` 即可定位打印出处（brt_trace.h 定义 bse_log_api/info/...）。
- 前缀→职能：bse_stack=栈引擎/HCI收发/GAP内部回调（源码不可见）｜bse_le_scan=LE周期扫描｜hci_h5=H5链路｜bse_gap=GAP连接｜bse_hfp=HFP状态机｜bse_mgr=管理器(设备库/profile回调汇总)｜hlp_manager=设备名库｜auto_gatt=GATT｜at_event=BT事件→AT文本出口｜[CARLINK-BT]=主应用收到的业务事件。
- **周期噪点（非异常）**：`bse_le_scan scan_complete,cap:BREDR`、`le_scan_device_info_updated`、`GAP Internal callback 0052/0056/0060` 周期性出现是例行扫描，别误判。
- 事件编号三套体系别混：①bse_hfp hlp_hf_event_cbk 的 event=HFP_EV(1~72)；②bse_mgr bt_hf_cbk 的 event=EV_HF(0~39)；③at_event bt_event_indicate 的 BT_EV 码=PROFILE_ID<<8+索引。

### 1. HFP 状态机编号（bse_hfp - hlp_hf_set_status/get_status，ST_HF，auto_hf.h）
0 UNINIT｜1 INIT｜2 READY｜3 DISCONNECTING｜4 CONNECTING｜5 CONNECTED｜6 OUTGOING｜7 INCOMING｜8 ONGOING｜9 HELD｜10 HUNGUPING｜11 END
- 看 log 小技巧：set 会打 `you set N`；get 只在 N<8 时打 `you get N`（internal_hf.c ~L790/813）。
- 上层换算：`+HFPSTAT = set值 - 2`（log 实证：set 4→HFPSTAT=2 连接中；set 5→HFPSTAT=3 已连接）。

### 2. EV_HF 事件编号（bse_mgr - bt_hf_cbk, event=N，EV_HF，auto_hf.h，从0起）
0 ENTER_PAIRING_MODE｜1 EXIT_PAIRING_MODE｜2 EXIT_SCAN_MODE｜3 CONNECTING｜4 DISCONNECTED｜5 CONNECTED｜6 CONN_CFM｜7 OUTGOING｜8 INCOMMINGCALL｜9 STANDBY｜10 ONGOING｜11 AG_DEVICE_NAME｜12 AG_DEVICE_VENDOR_NAME｜13 AG_DEVICE_MODEL｜14 AG_OS_VERSION_IND｜15 SCO_CONNECTED｜16 SCO_DISCONNECTED｜17 RETURN_STATUS｜18 CONNECT_FAIL｜19 RINGTONE｜20 NETWORK_OPERATOR_IND｜21 SIGNAL_STRENGTH_IND｜22 BATTERY_CHARGE_IND｜23 SUBSCRIBER_NUMBER_IND｜24 3_WAY_CALL_NEW｜25 3_WAY_CALL_STATE_IND｜26 3_WAY_CALL_DONE｜27 MIC_GAIN｜28 RINGTONE_TERMINATED｜29 ROAMING_STATE_IND｜30 NETWORK_STATE_IND｜31 VOICE_RECOGN_START｜32 VOICE_RECOGN_STOP｜33 EXIT_CFG｜34 DEV_INFO｜35 RINGTONE_ON｜36 RINGTONE_OFF｜37 STAT｜38 OK｜39 SPK_GAIN
- 1.txt 实测对应：event 21/22/23/29/30=信号/电量/本机号码/漫游/网络状态例行上报（param 为值）；19/28/35/36=铃声相关。

### 3. 底层 HFP 事件编号（bse_hfp - hlp_hf_event_cbk, event=N，HFP_EV，hfp_ui.h，显式从1起连续）
SLC/SCO/呼叫状态：1 SLC_ESTABLISHED｜2 SLC_RELEASED｜3 AUDIO_CONN_ESTABLISHED｜4 AUDIO_CONN_RELEASED｜5 STANDBY｜6 ONGOINGCALL｜7 INCOMINGCALL｜8 OUTGOINGCALL｜9 CALLHELD｜10 CALL_WAITING｜11 TBUSY
通用指示：12 GEN_INBAND_RINGTONE｜13 TERMINATE_RINGTONE｜14 VOICE_RECOGN_ON｜15 VOICE_RECOGN_OFF｜16 NETWORK_AVAILABLE｜17 NETWORK_UNAVAILABLE｜18 ROAMING_RESET｜19 ROAMING_ACTIVE｜20 SIGNAL_STRENGTH｜21 BATTERY｜22 CHLDHOLD_ON｜23 CHLDHOLD_OFF｜24 MICVOL｜25 SPKVOL
命令结果/来电信息：26 ATCMD_RESULT(OK/ERROR/CME/超时)｜27 CLIP(号码)｜28 CURRENT_CALLS(+CLCC)｜29 NETWORK_OPERATOR(+COPS)｜30 SUBSCRIBER_NUMBER(+CNUM)｜31 VOICETAG_NUM(+BINP)
HF侧请求(AG场景)：32 CURRENT_CALLS_REQ｜33 OPERATOR_FORMAT_REQ｜34 OPERATOR_REQ｜35 SUBSCRIBER_REQ｜36 VOICETAG_REQ｜37 INDICATOR_VAL_REQ｜38 DIAL｜39 MEM_DIAL｜40 LASTNUM_REDIAL｜41 MANUFACTURER_REQ(+CGMI)｜42 MODEL_REQ(+CGMM)｜43 NREC_DISABLE｜44 DTMF(+VTS)｜45 ANSWER｜46 CANCEL｜47 HOLD(+BTRH=0)
三方CHLD：48 CHLD=0｜49 CHLD=1｜50 CHLD=2｜51 CHLD=3｜52 CHLD=4
扩展：53 EXTEND_CMD｜54 UPPER_DECIDE_SCO｜55 SPP_ESTABLISHED｜56 HF_MANUFACTURERID｜57 HF_MODELID｜58 RINGING(+RING 只放铃音)｜59 TRANSPARENT_CIEV｜60 SERVER_START_CFM｜61 GET_AGFEATURES_DONE｜62 CHECKSCO_DONE｜63 CONNECT_CFM(RFCOMM)｜64 CONNECT_IND(RFCOMM来电)｜65 DISCONNECT_COMPLETE(带原因)｜66 THREEWAY_ERROR｜67 BTRH_QUERY_REQ｜68 HF_VERSION｜69 CODEC_CHANGED｜70 AUDIO_CONN_CODEC｜71 AUDIO_TX_PKT_LEN｜72 AUDIO_CONNECT_FAILURE

### 4. 上层 AT 业务事件（at_event 输出，主应用 [CARLINK-BT] 收到的最终事件）
- `bt_event_indicate(码)512`：码=BT_EV（PROFILE_ID<<8+事件索引），512=事件缓冲长度。例：264=0x108=GAP BOND_STATE_CHANGED；515=0x203=HFP CONNECTING。
- +HFPSTAT 值（evt_parse.c L450-509）：1=已断开｜2=连接中(带bd)｜3=已连接｜4=去电,号码｜5=来电,号码｜6=通话中,号码；STANDBY(通话结束/拒接)上抛串 `IF`。
- +HFPAUDIO：1=SCO音频建立｜0=SCO释放（EV_HF_SCO_CONNECTED/DISCONNECTED）。
- +HFPRING=1/0（RINGTONE_ON/OFF）；RINGTONE(19)另有 evt_income_ringtone 串。
- +HFPSIG/信号｜+HFPBATT/电量｜+HFPROAM/网络漫游状态｜+HFPNUM/本机号码(23)｜+HFPNET/运营商(20)｜+HFPIBR/带内铃声终止(28)｜+HFPVR=1/0 语音识别启停｜+HFPMTPX=index 0xff state 0xff number（三方呼叫：state 0 active/1 hold/2 outgoing/3 warning/4 incoming/5 waiting/6 done）。
- 配对：`+PAIRED=状态,MAC,名字`（状态 0=已配对 BONDED，1=未配对；BT_EV_BOND_STATE_CHANGED）。
- A2DP：`hlp_a2dp_set_status(N)`→even=N-1→`+A2DPSTAT=N-1`（log：set 4 已连接→A2DPSTAT=3）。AVRCPSTAT=2/3 在 A2DP 连接建立后出现（控制通道状态）。+PLAYSTAT：1=playing｜2=paused。

### 5. 常见流程 log 特征（行号引 1.txt）
**周期扫描**：bse_le_scan device_info_updated/scan_complete + GAP Internal callback 0052/0056（L9-11）。
**新设备连接(配对)起点**：`bse_stack - GAP Internal callback 0003` → `bse_gap - remote device[MAC],COD[...] request connection.` → `hlp_gap_conn_req_cbk link_type=0001`(L13-17) → HCI `[Send] 01 09 04 07 MAC 00` 建ACL(L18-19)→ `Command complete 0062` → `hlp_mgr_gap_cbk` → `+PAIRED=...`(L48-59)。
**HFP 连接建立**：auto_gatt gatt_connection_cbk → hlp_gap_conn_event_cbk:1,2,00 → set 4(CONNECTING) → bt_hf_cbk 3 → HFPSTAT=2 → set 5 → bt_hf_cbk 5 → HFPSTAT=3 已连接（L71-81），其后 bt_hf_cbk 21/22/29/30 例行上报(L560-597)。
**A2DP 播放**：porting_a2dp_event_cbk 若干(open/start 等) → hlp_a2dp_set_status(4) → +A2DPSTAT=3 → AVRCPSTAT 2/3（L1465-1501）。
**HFP 通话**：来电=INCOMMINGCALL(EV_HF 8)→HFPSTAT=5,号码 + RINGTONE/铃声；接听→EV_HF 15 SCO_CONNECTED→+HFPAUDIO=1；挂断→+HFPAUDIO=0→STANDBY(IF)。
**断链**：关注 bt_hf_cbk event=4(DISCONNECTED)→+HFPSTAT=1，及底层 HFP_EV 2/65（SLC_RELEASED/DISCONNECT_COMPLETE）与 hlp_hf_set_status 回退。
