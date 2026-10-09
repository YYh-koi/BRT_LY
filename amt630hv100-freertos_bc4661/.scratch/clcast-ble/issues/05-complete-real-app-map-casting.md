# 完成真实 APP 地图投屏闭环

Status: in-progress - real APP subscribes without MTU; long BLE fields are truncated

## 2026-07-13 real APP trace

The real APP connects successfully, discovers the complete CLCast FFE0 service
(`FFE1=0007`, `FFE2=0009`, `FFE3=000B`, FFE3 CCCD=`000C`), and negotiates ATT
MTU 517. It then remains idle without writing `0001` to CCCD handle `000C`.
Consequently the driver never reports `+CLCASTCCCD=1`, the receiver correctly
does not send B0-BA, and no P2P or TCP 9000 session starts. One connection was
terminated by the phone with HCI reason `0x13`, followed by an automatic
reconnect that stopped at the same point. APP-side logs or a known-good receiver
trace are required to identify the subscription gate (activation/binding,
backend SN/MAC validation, or an undocumented GATT prerequisite).

A later 11:04 target run passed the subscription gate: the APP wrote
`000C=0001`, the driver reported `+CLCASTCCCD=1`, and the receiver queued B0
through BA. However, this connection did not perform an ATT MTU exchange.
The stack therefore retained MTU 23 and truncated B0/B1/B6/B7 notifications to
20 value bytes. HCI packets for those fields are 32 bytes instead of the 35/36
bytes required for the complete frames, so their tails and CRC are missing.
The APP did not join the receiver AP or open TCP 9000 and then disconnected
with HCI reason `0x13`. The next integration requirement is explicit: the APP
must negotiate MTU at least 27 before enabling FFE3, or the driver must initiate
an equivalent exchange and reject notifications larger than `MTU - 3` rather
than silently truncating them.

APP source review then identified why the MTU-first run did not subscribe:
`getDescriptor()` queried `00002902-0000-1000-8000-C9EFDEFDEF9F`, while the
target HCI discovery response exposes CCCD as standard 16-bit UUID `0x2902`.
Android expands that descriptor to
`00002902-0000-1000-8000-00805F9B34FB`; the custom-base lookup returns null.
The APP must use the standard UUID, call `setCharacteristicNotification()`
before writing, and treat `onDescriptorWrite(GATT_SUCCESS)` rather than the
boolean queue result as subscription completion.

## Parent

`.scratch/clcast-ble/PRD.md`

## What to build

将 BLE 首连和 MJPEG 显示合并为真实手机 APP 可验证的地图投屏旅程：扫码、连接 BLE、接收参数、连接 P2P、建立现有 CLCast TCP 会话、显示 MJPEG，并正确处理 BLE 丢失、Wi-Fi 会话结束和 UI 恢复。该切片同时完成目标配置构建与 EC/EY 回归验证。

## Acceptance criteria

- [ ] 使用最终二维码模板时，真实手机 APP 能扫描并连接正确的 BLE 设备。
- [ ] 四个 handle 查询成功，手机订阅 CLCast Notify，并接收全部 B0～BA 参数帧且 CRC 校验通过。
- [ ] 手机使用 B6～BA 成功连接现有 P2P 网络并访问接收端 TCP 9000。
- [ ] 现有 CLCast 会话协商出的格式为 MJPEG，接收端连续显示地图视频。
- [ ] MJPEG 分辨率、帧率、内存占用和同步解码耗时满足目标硬件烟雾测试要求。
- [ ] Wi-Fi/TCP 会话活跃时断开 BLE 不会停止视频；BLE 状态被清理，后续重连可重新交换参数。
- [ ] Wi-Fi/TCP 会话结束或视频停止后恢复本机 UI，相关播放器资源被释放。
- [ ] 未知手机上行不会破坏地图投屏；开发激活绕过状态在日志中清晰可见。
- [ ] 使用目标 IAR AWTK 配置完成构建，记录构建结果、硬件型号、手机 APP 版本和实测现象。
- [ ] EC 和 EY 对应配置完成构建或最低限度烟雾回归，原有蓝牙和 H.264 路径不受影响。
- [ ] 未确认的驱动/APP 契约项均有最终结论或明确阻塞记录，不以猜测作为量产验收依据。

## Blocked by

- `.scratch/clcast-ble/issues/03-complete-ble-first-connection.md`
- `.scratch/clcast-ble/issues/04-display-one-clcast-mjpeg-frame.md`
