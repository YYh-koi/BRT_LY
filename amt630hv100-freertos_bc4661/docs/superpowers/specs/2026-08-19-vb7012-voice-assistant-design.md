# VB7012 Voice Assistant 源码移植设计

## 目标

将 FeiBao 工程 `app/vb7018_voice` 的语音命令接收、协议解析、事件回调和 AWTK 演示提示功能移植到当前 AMT630HV100 FreeRTOS 工程。目标组件统一命名为 `vb7012_voice_assistant`，并以源码形式直接编入主 IAR 工程，不创建或链接语音静态库。

## 范围

本次移植包括：

- UART3、115200、8N1、无流控的语音模块通信。
- `55 AA 01 CMD CHECKSUM` 协议解析。
- `0x00` 至 `0x19` 的 26 个语音命令到事件的映射。
- 单一事件回调注册接口以及接收任务的启动、停止接口。
- FreeRTOS 队列到 AWTK UI 定时器的跨任务事件传递。
- AWTK 首页上的语音事件提示区域。
- IAR 主工程中的直接源码编译、头文件路径和配置排除规则。
- 启用语音助手时对 UART3 FreeRTOS CLI 的互斥处理。

本次不包括：

- 将音量、切歌、灯光或车辆控制事件接入实际业务动作。
- LVGL 或 VG 界面集成。
- 创建 `vb7012_voice_assistant.a` 或独立语音库工程。
- 修改协议字段、命令编号或事件语义。

## 文件和命名

在 `app/vb7012_voice_assistant/` 下放置重命名后的源码：

- `vb7012_voice_assistant.c`
- `vb7012_voice_assistant.h`
- `vb7012_voice_assistant_awtk.c`
- `vb7012_voice_assistant_awtk.h`
- `vb7012_voice_assistant_demo.c`
- `vb7012_voice_assistant_demo.h`

目录、文件、头文件保护宏、类型、函数、静态变量、FreeRTOS 任务名、日志标签和 UI 文字统一使用 `vb7012_voice_assistant`、`VB7012_VOICE_ASSISTANT` 或用户可见的 `VB7012 Voice Assistant`。目标工程内不得新增 `vb7018_voice` 标识。

## 架构

### 核心层

`vb7012_voice_assistant.c` 负责 UART3 生命周期、FreeRTOS 接收任务、逐字节状态机、命令到事件映射和回调分发。公开接口保持源功能的形态：初始化、注册回调、启动和停止。初始化与启动为幂等操作；停止请求由接收任务观察，任务退出时关闭 UART。

### AWTK 适配层

`vb7012_voice_assistant_awtk.c` 注册核心事件回调。回调只把事件描述和递增序号写入容量为 8 的 FreeRTOS 队列，不直接操作 AWTK 控件。AWTK 定时器每 100 ms 从队列读取一条消息并更新提示标签。队列满时先丢弃最旧消息，再写入最新消息。

AWTK 首页创建完成后调用适配层初始化函数。适配层负责创建提示区域、队列、定时器并启动核心接收任务，重复调用不会重复创建资源。

### Demo 层

`vb7012_voice_assistant_demo.c` 保留源工程的非 UI 回调演示入口，输出事件名称，便于后续板级调试。本次 AWTK 产品入口不调用该 demo，避免覆盖 AWTK 注册的单一回调。

## 数据流

1. UART3 接收任务以最多 32 字节为一批读取数据。
2. 每个字节送入帧解析状态机。
3. 状态机依次识别 `0x55`、`0xAA`、类型 `0x01`、命令和校验字节。
4. 校验规则为 `CHECKSUM == (uint8_t)(0x01 + CMD)`。
5. 有效命令映射为对应的 `vb7012_voice_assistant_event_t`。
6. 核心层调用已注册回调。
7. AWTK 回调把事件写入队列。
8. AWTK 定时器在 UI 上下文中更新提示文字。

## 错误处理

- UART 打开失败或接收任务创建失败时，启动接口返回失败并记录日志。
- 校验失败时记录命令、实收校验值和期望校验值，随后重置解析器。
- 未知命令记录日志且不触发回调。
- 解析到非法类型或失配帧头时重置状态；若当前字节是新的 `0x55`，立即作为下一帧开头继续同步。
- AWTK 队列创建失败或核心启动失败时记录日志，并在可用的提示标签上显示启动失败。
- 队列满时丢弃最旧事件，优先保留最新语音事件。

## 配置和构建集成

`ArkmicroFiles/libboard-amt630hv100/include/board.h` 增加并默认启用 `VB7012_VOICE_ASSISTANT_ENABLE`。

`app/main_awtk.c` 在以下条件同时成立时不启动 FreeRTOS CLI：

- 已定义 `VB7012_VOICE_ASSISTANT_ENABLE`；
- `UART_MCU_PORT == 3`。

这使 UART3 由语音助手独占。现有 UART1 流控设置以及其他本地配置保持不变。

`lib/awtk/awtk-examples/Meter.v2-Demo/src/window_main.c` 在功能宏启用时包含 AWTK 适配层头文件，并在首页控件创建完成后初始化语音助手。

`proj/amt630hv100.ewp` 直接引用六个源码/头文件，并加入 `app/vb7012_voice_assistant` 头文件搜索路径。该模块仅参加 AWTK、AWTK_BW3583、AWTK_BW2232 和 AWTK_BW8201 配置；LVGL 与 VG_ONLLY 配置排除该模块。链接器附加库列表中不增加任何 VB7012 语音库。

## 验收和验证

### 自动检查

- 在移植前运行结构检查并确认因目标文件和工程引用不存在而失败。
- 移植后确认六个目标文件存在且名称正确。
- 确认目标工程中没有 `vb7018_voice` 残留。
- 确认不存在 `vb7012_voice_assistant.a`，链接器也没有相关库引用。
- 确认 IAR 工程直接引用目标源码、包含头文件路径，并对 LVGL/VG 配置设置排除规则。
- 使用 PowerShell XML 解析检查 `proj/amt630hv100.ewp` 结构有效。
- 使用 `D:\IDE\IAR\common\bin\IarBuild.exe` 完整构建 `amt630hv100/AWTK`，要求退出码为 0。

### 硬件冒烟测试

- 启动 AWTK 固件后确认 UART3 CLI 不再启动。
- 发送有效帧，确认 UI 显示对应 VB7012 事件。
- 连续发送超过队列容量的事件，确认界面继续显示最新事件且系统不阻塞。
- 发送错误校验帧，确认不更新事件，随后发送有效帧仍能正常解析。
- 发送未知命令，确认不触发事件且后续有效帧正常工作。

由于该功能依赖目标 UART3、FreeRTOS 调度和实际 AWTK UI，本次经确认不新增可运行的主机端单元测试；自动验收使用结构红/绿检查、IAR 全量构建和工程结构检查，运行行为由硬件冒烟测试覆盖。

## 兼容性和变更约束

- 保留工作区现有未提交修改，仅对相关文件做增量编辑。
- 不修改 LVGL 运行路径。
- 不改变现有 Carlink 功能和二维码逻辑。
- 不修改语音协议命令编号及事件语义。
- 不复制源工程的 `vb7018_voice.a`，也不新增独立库依赖。
