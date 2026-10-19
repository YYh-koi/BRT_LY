# 项目记忆 project_memory.md

> 用途：跨会话恢复上下文。打开本文件即可接续上次工作。
> 最近更新：2026-10-16（新增亿联骑行 EC 投屏问题排查 + H4/H5、BLE 澄清）

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
- 产品 = 车载互联（Carlink）投屏盒子。当前产品配置见 `ArkmicroFiles/libboard-amt630hv100/include/board.h`：`CARLINK_EC=1`（EY/CP/AA/CLCAST 全 0，互斥）、`BT_STACK_BARROT=1`、`WIFI_CHIP_GT=1`、`USE_LWIP=1`。（注：本仓库最早记录为 CLCAST，现已切到 EC，2026-10-13 核实。）
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

## 十、WiFi 热点/连接问题修复（2026-10-13）

### 0. 构建速查（IarBuild CLI）
- 命令：`& "D:\Tools\IAR\common\bin\IarBuild.exe" "<ewp>" -build <配置> [-log errors]`（退出码 0 = 成功，看 `Total number of errors: 0`）。
- **WiFi 库工程**：`proj\rtos_wifi6_630hv100.ewp`，配置 **Debug**。注意：**只有 Debug 配置的 `IarchiveOutput` 有效**（= `..\lib\wlan\atbm_wifi6\wlan_lib\wifi_gt.a`），Release 的为 `###Unitialized###` → 库只由 Debug 配置产出。
- **固件工程**：`proj\amt630hv100.ewp`，配置名是 **AWTK / LVGL / VG_ONLLY / AWTK_BW3583 / AWTK_BW2232 / AWTK_BW8201**（**没有** Debug/Release）。本产品 UI=AWTK，用 `AWTK`。
- **构建顺序**：先编库(Debug) → 再编固件(AWTK) 重新链接。固件通过 `..\lib\wlan\atbm_wifi6\wlan_lib\wifi_gt.a` 链接库。
- SDK 源为共享目录 `d:\project_ly\atbm_rtos_wifi6`，ewp 以 `$PROJ_DIR$\..\..\atbm_rtos_wifi6\...` 相对引用。
- 产物：`proj\AWTK\Exe\amt630hv100.out`（ELF）/ `amt630hv100.bin`（Post-Build 生成，可烧录）。

### 1. 问题一：手机 WiFi 列表看不到盒子热点
- 根因：新版 `atbm_wifi_p2p_user_config()`（`atbm_rtos_wifi6\hal\atbm_wifi_driver_api.c`）用 `>= CONFIG_MAX_NAME_LEN(32)` 做长度校验，App 传入的设备名 `DIRECT-AP630_LUYUAN_NEW888_p2p_C3BE`（35 字符）被拒 → `g_user_p2p_config` 为空 → 退回随机名/随机信道(ch11)启动 P2P 组，手机不作为普通 AP 显示。
- 修复：校验改为按目标缓冲区实际大小比较：
  `device_name_len >= sizeof(((struct user_p2p_config *)0)->device_name) || ssid_len >= sizeof(...ssid) || pwd_len >= sizeof(...pwd)`

### 2. 问题二：热点出现了但手机连不上（本次核心）
- 现象：手机完成 WPA2 四次握手（log `hostapd_handshake_over`），但拿不到 IP，约 18s 后 DEAUTH 断开。
- **根因（两处叠加，均在 WiFi 库工程的 Debug 配置里）**：
  1. `CCDefines` 定义了 `ATBM_TCPIP_STACK=ATBM_TCPIP_STACK_FREERTOS_TCP`（ewp L225）→ `Iwip_Init()` 选 `freertos_tcp_opt`，其 `net_enable = atbm_freertos_tcp_enable`（空壳，只置标志位，**不启 DHCP 服务器**）。
  2. `dhcps.c` 带 `<excluded><configuration>Debug</configuration></excluded>`（ewp L2412-2417）→ Debug 下根本不编译。即使第 1 项切回 LWIP，也会 `Error[Li005]: no definition for "dhcps_init"`。
- 双 TCP/IP 栈机制：`h\atbm_wifi_driver_api.h` 定义 `ATBM_TCPIP_STACK_LWIP=1` / `ATBM_TCPIP_STACK_FREERTOS_TCP=2`，默认 LWIP。
  - LWIP：`os\freertos\atbm_os_skbuf.c` 的 `lwip_tcp_opt` → `atbm_lwip_enable()`（打印 `init_udhcpd` + 设 netif ip 192.168.13.1 + `dhcps_init(dev->nif)`）。
  - FREERTOS_TCP：`freertos_tcp_opt` → `atbm_freertos_tcp_enable()` 空壳，无 DHCP；需 App 注册 `atbm_wifi_register_tcpip_rx_callback` 自供协议栈（本项目从未接线）。
- App 侧：`board.h` 的 `USE_LWIP=1`（CARLINK_EC 命中）→ `iot_wifi_atbm6165.c` 的 `start_p2p()` 里 `#if !USE_LWIP` 分支不生效，即 App 自身**不**启动 DHCP，完全依赖 SDK lwip → 所以库的栈选型必须为 LWIP。
- **修复（改 `proj\rtos_wifi6_630hv100.ewp`）**：
  - L225：`ATBM_TCPIP_STACK_FREERTOS_TCP` → `ATBM_TCPIP_STACK_LWIP`
  - L2412-2417：删除 `dhcps.c` 的 Debug 排除项
  - 与 Release 配置、旧版正常工程、App 的 USE_LWIP=1 全部对齐。
- 验证：库 Debug 编译 0 错误（`dhcps.c` 已进入编译列表）；固件 `AWTK` 链接 0 错误，产出 `amt630hv100.bin`。
- 实测 log（`debug\log_test.txt`）关键行：`init_udhcpd`(L287) → `DHCP assign ip = 192.168.13.100`(L404) → REQUEST/ACK → `save_client_addr`(L419)，手机正常拿到 IP，问题解决。

### 3. 前序已落盘的文件改动（配套）
- `lib\wlan\atbm_wifi6\atbm_api.h`：补 `ATBM_WIFI_SCAN_AP_NOT_FOUND_EVENT = 12`。
- `lib\wlan\atbm_wifi6\atbm_wifi_driver_api.h`：补 `atbm_tcpip_rx_callback_t` typedef、`atbm_wifi_register_tcpip_rx_callback`、`atbm_wifi_is_sta_connected` 原型（与新版库 ABI 对齐）。

### 4. 排错经验（避免误判）
- **日志级别**：`atbm_debug.h` 里 `WIFI_DEBUG_LEVEL=0`（全关），仅 `WIFI_ALWAYS`(BIT20) 恒打印。故 `start to beacon`（WIFI_WPA 级）**不打印属正常**，不代表失败；`init_udhcpd`（WIFI_ALWAYS）**必打印**，其缺失即 = DHCP 服务器未启动（本次判据）。
- `maxstacnt = 2 vs 8`：新版 `ATBMWIFI__MAX_STA_IN_AP_MODE`=2，旧版=8，属版本差异非 bug。
- 判据对比：正常版 `debug\ly_log.txt` 有 `init_udhcpd` + DHCP 分配 192.168.13.x；异常版此前全篇无任何 dhcp/udhcp/192.168 行。

### 5. 参照工程（已验证正常的旧版）
- `D:\project_ly\BRT_LY-master\amt630hv100-freertos_bc4661`：其 `rtos_wifi6_630hv100.ewp` **无** `ATBM_TCPIP_STACK` 定义（默认 LWIP）、`dhcps.c` **无**排除 → 正常工作。对比排查时以此为基准。

### 6. 遗留/待办（用户未确认）
- 是否需要把新版两处差异回退对齐旧版：`p2p_get_available_ifpriv()` 的 `if_id 0`→`if_id 1`；`ATBMWIFI__MAX_STA_IN_AP_MODE` 由 2 改回 8。
- `bt_manager.c` 崩溃防护；补回 SDK 的 4 行 `bt_disconnect_avrcp_if_hf_hid_lost()`（蓝牙音乐相关，非本次 WiFi 问题）。
- 是否彻底统一 `lib` 与 `api` 两套头文件。

## 十一、H4/H5 与 BLE 澄清（2026-10-16）

### 1. 当前走 H5（TI 三线协议），不是 H4
- 传输层选择：`lib/bt/brt_bt/brt_bt.c`（与 `brt_aud3_sdk_freertos/brt_bt/brt_bt.c` 同源）按芯片能力选择；实际运行日志前缀为 **`hci_h5 -`**，且 SDK 内 `sdk/auto_app/h5/`（`hci_h5.c`/`tl_h5.c`）被编入 → **确认走 H5**。
- H5 需要固件 BT 芯片支持：`atbm_rtos_wifi6/hal/sdio/firmware_bt_h5_3m.h` 存在（另有 `firmware_bt_h4.h`）——选 H5 时加载 H5 固件；H4 与 H5 **靠固件+链路配置共同决定**，不是"无关"。
- 传输初始化见 `brt_aud3_sdk_freertos/sdk/auto_app/transport/transport.c`（L203-212 附近）。

### 2. EC 业务下 BLE 广播默认未开（用户 ly_log 扫不到 BLE 广播）
- 若要让 EC/主流程也能被 BLE 扫到，需放开广播开关：`app/carlink/common/carlink_bt_wifi.c`（约 L731）或取消 `sdk/auto_at/cmd_process_ex.c`（约 L431）注释。
- 结论：**当前关 BLE 属配置行为，非 bug**；是否开启取决于业务是否需要 BLE 引导（CLCast 需要，EC 走 QR/AP 则不需要）。

## 十二、亿联骑行（EC / EasyRide）业务速览（2026-10-16）

### 1. 业务定位与开关
- EC = **亿联骑行 EasyRide（EasyConn）**投屏；`board.h` 中 `CARLINK_EC=1`（与 EY/CP/AA/CLCAST 互斥）。
- App 包名：国内 `net.easyconn.carman.easyride`（flavor 50）；海外 `net.easyconn.easyride.wws`（flavor 51）。
- EC SDK 为**预编译库无源码**：`app/carlink/EC/lib/ECTiny.a`（版本 `ECS裸SDK_VERSION "1.0.10"`，见 `ECTiny.h`），旧版留档 `ECTiny_old.a`。
- 源码：`app/carlink/EC/src/carlink_ec.c`（新版）、`app/carlink/EC-orig/src/carlink_ec.c`（旧版备份）；头文件 `app/carlink/EC/include/{ECTiny.h,ECTypes.h}`。

### 2. EC 引擎初始化链路（`carlink_ec.c`）
```
EC_createECConfig() → EC_setBaseConfig(cfg, uuid, "V0.0.1", "B:/")
→ registerECCallback() → EC_bindAccessFile(&accessFile)
→ EC_initialize(cfg, cb) → EC_setLogInfo(ERROR, STD, NULL, SDK|APP)
→ EC_setMirrorConfig(&mirrorCfg) → EC_setVideoPlayer(...) → EC_start()
→ EC_generateQRCodeUrl(&qr_info)
```
- 线程：`xTaskCreate(taskInitCarlink, "initThread", 2048*4, NULL, configMAX_PRIORITIES/4, NULL)`。
- 二维码字段（L486-556 附近）：
  - `qr_info.action = EC_QR_ACTION_WIFI_P2P_MODE | EC_QR_ACTION_WIFI_AP_MODE_CUSTOMIZED` = **9**
  - `ssid/pwd` 取 `carlink_get_wifi_ssid()/carlink_get_wifi_passwd()`，`auth="WPA-PSK"`，`name` 取 `carlink_get_wifi_p2p_name()`，`uuid="250002025122400000003"`。
- 镜像配置：`touchMode=2`、`type=EC_VIDEO_TYPE_H264`、`quality=4MB`、`capScreenMode=0x08`。
- **新版 `carlink_ec.c` 未调用**：`EC_bindWIFIDevice` / `EC_setNetInterfaceInfo` / `EC_notifyWifiStateChanged` / `EC_setCommonConfig`（旧版 `EC-orig` L722-723 有被注释的 `EC_bindWIFIDevice(EC_TRANSPORT_*_WIFI, ...)`、L1006 `EC_bindBTDevice`、L1022-1045 `EC_notifyWifiStateChanged`）→ **WiFi/BT 传输绑定可能缺失，待确认**。
- 回调注册（`registerECCallback`，L239-242 / 实体 L176-210）：`onECConnectStatus`、`onMirrorStatus`、`onECStatusMessage`、`onPhoneAppInfo`、`onLicenseAuthSuccess/Fail`、`onRequestBuildNet`、`onRequestBuildNetCancel`、`onPhoneBuildNetFinish`、`onPhoneAPInfo`。

## 十三、EC 枚举/错误码/日志速查（源码 `ECTypes.h`/`ECTiny.h`）

### 1. 错误码（ECTypes.h L24-32）
`EC_OK=0`｜`EC_ERR_INVAL_OP=-1`｜`EC_ERR_INVAL_PARAM=-2`｜**`EC_ERR_OP_FAIL=-3`**｜`EC_ERR_LICENSE_AUTH_FAIL=-4`｜`EC_ERR_APP_NOT_STARTED=-5`｜`EC_ERR_APP_AUTH_FAIL=-6`｜`EC_ERR_APP_AUTH_PENDING=-7`｜`EC_ERR_PERMISSION_DENIED=-8`

### 2. `ECConnectedStatus`（连接状态回调 `onECConnectStatus:status=N`，L35-45）
1 DEVICE_ATTACHED｜2 DEATTACHED｜3 CONNECTING｜4 CONNECT_FAILED｜5 CONNECT_SUCCEED｜6 DISCONNECTED｜7 APP_EXIT｜**8 INTERRUPTED_BY_APP**

### 3. `ECConnectedType`（`type=N`，L48-60）
0 ANDROID_USB｜**1 ANDROID_WIFI**｜2 IOS_USB_EAP｜3 IOS_USB_MUX｜4 IOS_USB_AIRPLAY｜**5 IOS_WIFI_APP**｜6 IOS_WIFI_AIRPLAY｜7 IOS_USB_LIGHTNING

### 4. `ECMirrorStatus`（`onMirrorStatus`，L62-69）
0x1000(4096) AUTH_PENDING｜**0x1001(4097) STARTED**｜0x1002 FAILED｜0x1003 STOPPED

### 5. `ECStatusMessage`（`onECStatusMessage:status=N`，L78-122）
0 USER_NOT_AUTH_DEBUG｜1 APP_NOT_RUNNING｜2 APP_FOREGROUND(废弃)｜3 FOREGROUND_SAME｜4 FOREGROUND_SPLIT｜5 FOREGROUND_SAME(bg)｜6 BACKGROUND_SPLIT｜7 SCREENLOCKED｜8 UNSCREENLOCKED｜**9 NAVI_STARTED**｜**10 NAVI_STOPPED**｜11 SWITCH_AOA_FAIL｜16/17 ACQUIRE_BLUETOOTH_A2DP｜18 SWITCH_TO_SYSTEM_MAIN_PAGE｜19 LAUNCH_PHONE_APP｜31 TALKIE_NON_LOGIN｜33 HU_BT_NOT_MATCH｜36/37 ENABLE/DISABLE_ACCESSIBILITY｜38/39 START/STOP_DLNA_SERVICE

### 6. 能力位 `fw_paramCarDesc`（日志 L317 附近一行打印全部）
- `supportConnect`（L214-227）：ADB=0x01｜AOA=0x02｜**WIFIDIRECT=0x08**｜EAP=0x20｜MUX=0x40｜LIGHTNING=0x80｜**IPHONE_WIFI=0x100**｜**ANDROID_WIFI=0x200**｜USB_AIRPLAY=0x400｜WIFI_AIRPLAY=0x800
- `supportFunction`（L229-246）：INPUT=0x01｜SPEECH_WAKE=0x02｜VR_TEXT=0x04｜BT_MUSIC=0x08｜BT_AUTO_PAIR=0x10｜HU_HOTSPOT=0x20｜GPS=0x40｜**WIFI_QR_CONNECT=0x80**｜BT_CALL_BY_HU=0x100｜DLNA=0x200｜PHONE_CONTROL_CAR=0x400｜MEDIA_TRANSPORT_VIA_EC=0x800｜ANDROID_PARALLEL_WORLD=0x1000｜BLE_CONNECT=0x2000
- 日志实测：**`supportConnect:8`（仅 WIFIDIRECT，未含 IPHONE_WIFI/ANDROID_WIFI）、`supportFunction:128`（0x80=WIFI_QR_CONNECT）** → 即"扫码直连"能力已开，但未显式声明 iOS/安卓 WiFi 直连位。**待确认是否应补 `0x100|0x200`。**

### 7. `ECQrAction`（L1327-1339）与 `ECQRInfo`（L1341-1351）
- action：DEFAULT=0x00｜WIFI_AP_MODE_CUSTOMIZED=0x01｜WIFI_AP_MODE_ACCESS_INTERNET=0x02｜WIFI_STATION_MODE=0x04｜**WIFI_P2P_MODE=0x08**｜USB_ANDROID=0x10｜USB_IPHONE=0x20｜BT=0x40｜BLE=0x80 → 日志实测 **action=9（0x08|0x01 = P2P + AP customized）**
- `ECQRInfo` 字段：`ssid[32]`/`pwd[16]`/`auth[16]`/`mac[32]`/`name[32]`/`bm[32]`(车机 BT MAC)/`action`

### 8. `ECTransportType`（L388-401）
ANDROID_USB_ADB=0｜ANDROID_USB_AOA=1｜ANDROID_WIFI=2｜IOS_USB_EAP=3｜IOS_USB_MUX=4｜IOS_USB_AIRPLAY=5｜**IOS_WIFI_APP=6**｜IOS_WIFI_AIRPLAY=7｜IOS_USB_LIGHTNING=8

### 9. `ECFlavor`（L1159-1177）
AE 0x00-0x04｜GWM_EC=0x10｜XIANDOU_ORA=0x11｜XIANDOU_HAVAL=0x12｜FORD=0x13｜YLTP=0x15｜MOTOFUN_CHINA=0x30｜MOTOFUN_OVERSEA=0x31｜**MOTOFUN_MINI_CHINA=0x32(50)**｜**MOTOFUN_MINI_OVERSEA=0x33(51)**｜GUANGYANG_EC=0x34 → 用户机 flavor:50，客户机 flavor:51。

### 10. 成功/失败日志签名
- 成功链路：`onECConnectStatus:status=1` → `status=3`(CONNECTING) → `onLicenseAuthSuccess` → `onMirrorStatus 4096` → `status=4097` + `start VideoPlayer,width=..,height=..`。
- **iOS WiFi 硬失败**：`recvCMD: ECP_PXC_CMD_ERR cmdType:80000020|data:null` → `sendClientInfo: rly {cmdType: 0x80000020, cmdLength: 16}` → `start_C2PService: start: sendClientInfo failed` → `pxc_start: failed to start C2PService` → `ectiny_imp_openTransport openTransport fail,ret=-3`（无限重试）。
- 软失败重试（可恢复）：同签名但 `ret=-7`（APP_AUTH_PENDING）→ 重试后成功。

## 十四、EC 问题排查记录（2026-10-16）

### 1. 苹果手机连不上（日志 `debug/ly_log.txt`，用户机，已闭环）
- 现象：iPhone 扫码后连不上；同机安卓正常。
- 分析：
  1. 网络层正常：`STA added 5e:0b:ef:ce:34:04`（iOS 随机化 MAC）→ DHCP `192.168.13.101` → **排除 ATBM WiFi 驱动**；`ReOrder:timeout` 为良性。
  2. EC 识别为 `type=5`(IOS_WIFI_APP)、`status=1`(ATTACHED)。
  3. 失败固定为 `ECP_PXC_CMD_ERR cmdType:80000020` → `-3`(OP_FAIL)，无限重试（L1129-1212 循环）。
  4. 同机安卓走 `type=1`→`status=5`→license→mirror→VideoPlayer 640x368 成功 → 车机主链路正常。
- **关键差异**：用户安卓 / 客户 iPhone 用 `cmdType: 0x70000020`（-7 pending，重试后成功）；**用户 iPhone 用 `0x80000020` 被手机回 `ECP_PXC_CMD_ERR`** → 硬失败 `-3`。
- **结论**：**非蓝牙/WiFi 库问题**，是 iPhone 端 App 与车机 EC SDK 的 **C2P 协议不匹配**，需 App / EC SDK（`ECTiny.a` 1.0.10）侧解决；建议核对 App 版本/包名、联系 EC SDK 提供方；并核对 `supportConnect:8` 是否需补 `IPHONE_WIFI(0x100)`。

### 2. 二维码连接流程澄清（已闭环）
- QR 内含 ssid/pwd/auth，且车机开了 `WIFI_QR_CONNECT(0x80)` → **设计上是 App 扫码后自行完成 WiFi 连接**（不强制用户先手动连仪表热点）。
- 能否自动取决于 App 权限：**iOS 必须申请 `NEHotspotConfiguration`（Hotspot Configuration entitlement）**，无权限则须先在系统 WiFi 列表手动连仪表热点再扫码；Android 一般可用 `WifiNetworkSpecifier` 自动连。
- 用户日志走 **AP 模式**（非 P2P）：未触发 `onRequestBuildNet`/`onPhoneAPInfo` 等协商建网回调 → 印证 AP 直连。
- 结论：扫码只是替用户"连 WiFi"，**与之前 iPhone 失败点（WiFi 已连后的 C2P）无关**。

### 3. 手机端"清除互联配对"后卡死（日志 `debug/SOC 6_2026-10-16_10_46_06.txt`，**已定位根因**）
- 文件：21183 行 / 1491461 字节；窗口 `[2026/10/16 10:46:08]` → `[2026-10-16 14:47:18:913] stop VideoPlayer`（约 4 小时）。日志跨多次重启，不是单次会话。
- **更正（重要）**：§14.3 原结论"无 assert"错误。实际存在真实断言崩溃——用 `crash` 关键词（而非 assert/panic）grep 到：
  **L16856 `[2026-10-16 14:39:59:694] crash E:\...\lib\hx170dec\src\dwl_freertos.c : 611`**，随后 2ms 即 `ARK SoC Booter V 2.00` 重启。该 `crash` 由 `main.c` 的 `vAssertCalled()`（`configASSERT` 钩子）打印，打印后 `taskENTER_CRITICAL(); while(ul==0) portNOP();` 死循环 → 整机冻结。
- **心跳证据（决定性）**：idle hook 每 10s 打印 `Memory free:`。崩溃前心跳正常（`14:23:25/35/45/55`），**`14:23:56.366 stop VideoPlayer` 之后心跳彻底消失，静默 16 分钟**，直到 `14:39:59` 才出现 `crash` 行并重启 → 说明 `stop VideoPlayer` 之后系统已整体冻结（idle 都跑不到）。
- **同一模式反复出现（每次都在 `stop VideoPlayer` 之后）**：
  - `11:11:32.272 stop VideoPlayer` → 静默约 4 分钟 → `11:15:31 ARK SoC Booter`（重启）
  - `11:28:11.248 stop VideoPlayer` → 数秒 → `11:28:15.004 ARK SoC Booter`
  - `14:23:56.366 stop VideoPlayer` → 静默 16 分钟 → `14:39:59 crash dwl_freertos.c:611` → 重启
  - `14:47:18.913 stop VideoPlayer`（日志结束，之后无输出）
- **根因链（use-after-free 竞态）**：
  1. 手机端"清除互联配对" → EC 会话被中断（`onECConnectStatus:status=8`=INTERRUPTED_BY_APP）或断开（`status=2`）→ EC SDK 回调解绑，调用 `VideoPlayer->stop()` = `vp_stop()`。
  2. `vp_stop()`（`carlink_ec.c:273-284`）→ `h264_video_player_uninit(gECVideoHandle)` → `mfc_uninit()`（`mfcapi.c:429-454`）= `H264DecRelease(decInst)` **释放 HX170 解码器及其 dwl 实例** + `vPortFree(handle)`，再把 `gECVideoHandle=NULL`。
  3. 与此同时 EC 视频数据线程仍在 `vp_play()` → `h264_video_player_proc(gECVideoHandle,…)` → `mfc_decode()` 中（`carlink_video.c:459-479`）。`vp_play` 的 `if (gECVideoHandle)` 检查与 `h264_video_player_proc()` 调用之间存在 **TOCTOU 窗口**：`vp_stop` 可在其中释放 handle。
  4. 释放后仍在用的 dwl 实例，其 `numCores` 从已释放内存读到 0 → `DWLWriteReg`（`dwl_freertos.c:611`）`configASSERT(coreID < dec_dwl->numCores)` 失败 → `crash` → `vAssertCalled` 关中断死循环 → **整机冻结/重启**。
  5. 崩溃前一行 `-E- wait_event_interruptible dec error 0`（L16855，`HX170DEC_IOCX_DEC_WAIT` 1000ms 超时）是同一次竞态的伴生现象：解码器被并发释放/复位，解码 IRQ 永不来。
- **关键前提**：`carlink_video.h:9 DISABLE_CARLINK_H264_FRAME_BUF=1` → 实际走的是 `h264_video_player_init/uninit/proc`（`carlink_video.c` `#else` 分支，CLCcast 同步解码），**不是** L290 的 `vp_play` `get_retry` 帧缓冲自旋分支（该分支为死代码）。因此修复应针对 `h264_video_player_*` 路径。
- **修复方向（待定稿）**：
  - 给 `vp_start/vp_stop/vp_play` 加互斥（或引用计数）：`vp_stop` 必须先等 in-flight 的 `h264_video_player_proc` 返回，再 `mfc_uninit` 并置空 handle；`vp_play` 在锁内取 handle。
  - 或在 EC 断连时先停/解绑视频数据流（`EC_setVideoPlayer(NULL)`/停止 EC 数据线程）再 `mfc_uninit`，避免释放与解码并发。
  - 至少：`vp_stop` 先置 `gECVideoHandle=NULL` 再 `mfc_uninit`，缩小窗口（但不能根除，仍需加锁）。
- **尾部现场时序（L21000-21183，重启后新会话）**：
  - `14:46:51` UI 切 navigation_page1/home_page，`Memory free: 16027456`
  - `14:46:52.549` `onMirrorStatus 4096→4097` + `start VideoPlayer 1024x480`
  - `14:46:56.342` `recv_ socket(192.168.13.100:10922) closed, mSock=4, errno=107` + `recvFrame: failed to recv frame data`
  - `14:46:56.458` `onECConnectStatus:status=2,type=5`（DEATTACHED）+ `onRecv_P2CService recv CmdBase failed` + `stop VideoPlayer`
  - `14:46:58.723` **`onECConnectStatus:status=8,type=5`（INTERRUPTED_BY_APP，即"清除配对"语义）**
  - `14:46:58.868` `onPhoneAppInfo` → `14:46:58.994` **`status=5,type=5`（又重连成功）**
  - `14:47:00.525` `onMirrorStatus 4097` + `start VideoPlayer 1024x480`（重新投屏）
  - `14:47:01.435` `onECStatusMessage:status=4`；`Memory free: 12642944`（16MB→12.6MB）
  - `14:47:02.444` / `14:47:13.608` `[BT_UART] state change request: BT_STATE_ENABLED -> BT_STATE_DISABLING_A2DP` + `changed=0`（**A2DP 未能真正关闭**）
  - `14:47:11.631` `onECStatusMessage:status=9`（NAVI_STARTED）→ `14:47:15.239` `status=10`（NAVI_STOPPED）
  - **`14:47:18.913` `stop VideoPlayer`（日志结束，之后无输出 → 与前述冻结模式一致）**
- 其他记录：10:58:08 重启是 UI 主动复位（`window reset_popup open` + `reset_count:1..5` + `send_mcu_cmd_arg cmd:b0`），与本卡死无关。内存 16MB→12.6MB 波动疑似视频缓冲占用，未见持续单调泄漏。

## 十五、三份日志文件对照（EC 排查用）
| 文件 | 机器/flavor | 包名 | 关键结论 |
|---|---|---|---|
| `debug/ly_log.txt`（2026-10-16） | 用户机 flavor 50 | `net.easyconn.carman.easyride` | 安卓 `type=1` 成功；**iPhone `type=5` 失败**（`cmdType:80000020`→`-3`） |
| `debug/SOC 6_2026-10-16_10_4.txt` | 客户机 flavor 51 | `net.easyconn.easyride.wws` | **iOS `type=5` 成功**（全篇 37 处 `cmdType:0x70000020`，无 `ECP_PXC_CMD_ERR`/`-3`） |
| `debug/SOC 6_2026-10-16_10_46_06.txt` | 客户机 | — | "清除互联配对"卡死现场（见十四.3，进行中） |

## 十六、EC 相关遗留待办（2026-10-16）
1. **【高】确认"清除互联配对"卡死根因**（十四.3），区分 UI 卡死 vs 系统僵死，定位是否为 `status=8` 后 `stop VideoPlayer`/网络回收竞态。
2. **【高】确认 WiFi transport 对外绑定**：新版 `carlink_ec.c` 未调 `EC_bindWIFIDevice`（旧版 `EC-orig` L722-723 有注释行）——是否需补回。
3. **【中】`supportConnect:8` 是否应含 `0x100|0x200`**（IPHONE_WIFI|ANDROID_WIFI）。
4. **【中】** `flavor 50 vs 51` 由固件 `fw_updateFlavor`/`opReserve` 打包还是协商决定。
5. **【中】解释"去掉 A2DP 后 log 里 A2DP 仍跑"**：日志反复 `BT_STATE_DISABLING_A2DP` + `changed=0`，给出正确关闭方式。
6. **【中】** CLCAST/EC 主流程中 BLE 广播由谁何时开启（`carlink_bt_wifi.c#L731` / `cmd_process_ex.c#L431`）。
7. **【低】** 给客户补 `atbm_wifi_driver_api.h` L395-396 声明 / 重打整包 SDK。
8. **【低】** 确认 WiFi `Pe256` 修复是否仍在；`wlan_lib/wifi_gt.a` 缺失需补回/重建。
9. **【低】** A2DP 崩溃（`0xffffffc6` Data Abort）是否继续排查。
10. **【低】** 辨析 P2P GO 实际使用 VIF0 还是 VIF1。
