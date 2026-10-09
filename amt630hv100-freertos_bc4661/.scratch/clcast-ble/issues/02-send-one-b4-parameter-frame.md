# 发送一个可校验的 B4 参数帧

Status: complete - phone received the raw B4 frame; APP parsing is deferred to issue 05

## Implementation status (2026-07-10)

- Added `clcast_ble_first_connection.[ch]` as the business state machine above
  the generic BLE transport adapter.
- The state machine emits B4=`30` as
  `FF FE 05 B4 33 30 69 CE` using CRC16/MODBUS and little-endian CRC bytes.
- The documented `FF FE 05 B4 32 34 -> 69 9D` vector remains a standalone
  CRC example for the payload string `24`; it is not the configured B4 value.
- B4 is sent once only when handles are ready, BLE is connected, and
  `+CLCASTCCCD=1` has enabled FFE3 notifications.
- Immediate send failure latches the current exchange; transport loss resets
  the state so reconnect and resubscribe can send B4 again.
- The existing host test entry now covers the golden frame, pre-ready gating,
  single-send behavior, send failure, and successful reconnect resend.
- The new source file is included in `proj/amt630hv100.ewp`.
- The IAR target build completed successfully. Target logs verified that the
  frame was sent only after `+CLCASTCCCD=1`, through runtime FFE3 handle
  `000B`, and reached HCI as `FF FE 05 B4 33 30 69 CE`.
- The user confirmed that the phone received the frame. The real CLCast APP
  workflow will be tested after the complete B0-BA and MJPEG path is ready.
- Per user request, no command-line build or host test execution was performed.
  Phone-side CLCast parsing/behavior confirmation remains pending.

## Parent

`.scratch/clcast-ble/PRD.md`

## What to build

在已验证的 BLE 通道上完成第一个业务示踪切片。手机连接并启用 FFE3 Notify 后，接收端清空 FFE1，通过 FFE3 发送 B4=`30` 参数帧。参数帧使用规定帧头、长度、UTF-8 数据和 CRC16/MODBUS，并通过可配置节拍发送，不依赖不存在的逐包发送完成事件。

## Acceptance criteria

- [x] 只有连接有效、FFE1/FFE2/FFE3 handle ready 且收到 `+CLCASTCCCD=1` 后才允许发送。
- [x] 初始化时清空 FFE1 Read 缓存，不在 FFE1 返回未定义业务数据。
- [x] B4 帧的数据内容是 UTF-8 字符串 `30`，长度字段覆盖类型、数据和两字节 CRC。
- [x] CRC16/MODBUS 参数和低字节在前的序列化方式符合 PRD。
- [x] 固定测试向量 `FF FE 05 B4 32 34` 生成 CRC 字节 `69 9D`。
- [x] 手机从 FFE3 收到原始 B4 帧，并可按同一 CRC 规则校验；CLCast APP 解析延期到整体联调。
- [x] 发送函数使用运行时 FFE3 handle，并允许配置包间隔。
- [x] 立即发送失败会终止本轮参数交换并记录错误，不会继续假定发送成功。
- [x] BLE 断开后禁止继续发送；重连并重新订阅后能够再次发送 B4。
- [x] 状态机自动测试覆盖发送门控、CRC、断开和重连（测试代码已加入，待用户侧执行）。

## Blocked by

- `.scratch/clcast-ble/issues/01-sync-and-verify-ble-driver-contract.md`
