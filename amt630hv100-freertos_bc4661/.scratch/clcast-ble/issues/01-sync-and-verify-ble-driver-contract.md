# 同步并验证 CLCast BLE 驱动契约

Status: in-progress - build verified, target validation pending

## Implementation status (2026-07-10)

The application-side integration is now present:

- `CARLINK_CLCAST` selects `BLE_CLCAST_CONNECTION` in the active Barrot wrapper.
- The shared Bluetooth callback parses `+GATTHANDLE`, `+CLCASTDATA`,
  `+CLCASTCCCD`, connection state, and driver restart into typed events.
- The CLCast adapter queries FFE1, FFE2, and FFE3 at runtime with
  bounded retries, clears FFE1, gates notifications on connection and FFE3 subscription
  readiness, and sends through the queried FFE3 handle.
- A fixed-capacity nonblocking event queue separates the Bluetooth callback
  from CLCast processing.
- A host-side behavioral test entry covers parsing, handle readiness, CCCD
  gating, notification output, reconnect, restart, retry, and uplink copying.

The active product links `lib/bt/brt_bt/lib/brlink.a`. The archive was replaced
on 2026-07-10 (SHA-256
`67BB4656B7E0AB501B1467EEEBE379253F1F034F0F3E5A8ACF1D206673490862`). It
contains the CLCast uplink event, handle/read/send AT command handlers, CCCD
event, service registration, and FFE1/FFE2/FFE3/CCCD runtime handle symbols.
The matching companion archive `brlink_stack.a` is now linked after `brlink.a`.
The IAR 8.1 `AWTK` configuration was rebuilt successfully on 2026-07-10 with
zero errors and five pre-existing warnings. The resulting image is
`proj/AWTK/Exe/amt630hv100.bin`. Target/phone validation remains pending.

The 17:00 target run confirms the updated application queries only FFE1/FFE2/
FFE3, obtains `0007/0009/000B`, enters ready state, and parses the full
`+GATTSTAT=3,FF,<MAC>` connection event. The updated driver archive contains
`+CLCASTCCCD=%d`, but its service-added callback still records the CLCast 2902
descriptor as handle zero. Phone writes to the actual handle `000C` reach the
stack with values `0001/0000/0001`, then fail with `handle is null!`; no
`+CLCASTCCCD` event reaches the application. Driver-side FFE3 CCCD handle
resolution remains the current blocker.

The 17:13 target run with `brlink.a` SHA-256
`87E1C9EAB899526D26E426410013F6E919150ED241370E6EF66F287C0BD20FA0`
resolves that blocker. Phone writes of `0001/0000/0001` to FFE3 CCCD handle
`000C` produce `+CLCASTCCCD=1/0/1`, and the application updates its notify
gate to `1/0/1`. No `+CLCASTDATA` uplink or `AT+GATTDATASEND` downlink appears
in this capture, so bidirectional payload validation remains pending.

The appended capture completes FFE2 uplink validation: the phone writes ASCII
`1234` (`31 32 33 34`), the driver reports `+CLCASTDATA=31323334`, and the
application receives `uplink len=4 data=31323334`. FFE3 notification downlink
validation remains pending.

The 17:20 target run completes FFE3 downlink validation. The application sends
`AT+GATTDATASEND=000B,31323334`; the HCI trace contains an ATT notification on
handle `000B` with payload `31 32 33 34`, and the phone application displays
`1234`. The temporary automatic smoke-send trigger was removed after this
verification.

### BLE discovery fix (2026-07-10)

Target logs showed that the Bluetooth stack started, connection type 2 was
selected, and the BR/EDR name was visible, but no BLE advertiser was visible.
The reference firmware and Barrot AT specification confirmed that
`AT+PAIR=3` explicitly enters combined BR/EDR and BLE discoverable mode. The
current firmware only wrote legacy advertising data after `+LEADDR` and never
sent the discoverability command. The `+LEADDR` flow now sends `AT+SSP=1`,
`AT+PIN=0000`, and `AT+PAIR=3` after setting advertising data. The binary
advertising-address command was also changed from an unsafe `%s` conversion to
bounded fixed-length assembly. The updated AWTK image builds successfully;
phone BLE scan confirmation is pending.

## Parent

`.scratch/clcast-ble/PRD.md`

## What to build

将已经交付的 CLCast BLE 驱动版本完整同步到当前固件，并打通一条可验证的上层路径：公共蓝牙适配器把驱动 AT 文本转换成类型化 CLCast 事件，CLCast 模块能够查询并缓存三个特征运行时 handle、接收手机写入的原始字节，并使用查询到的 Notify handle 向手机发送测试字节。该切片只证明驱动契约和上层边界，不实现 B0～BA 业务。

## Acceptance criteria

- [x] CLCast 构建能够选择已交付的 CLCast BLE profile，并实际链接匹配版本的驱动源码、头文件和库。
- [x] FFE1、FFE2、FFE3 handle 均通过运行时命令查询，不存在硬编码值；应用不查询全局 2902 handle。
- [ ] 服务未就绪返回零 handle 时进行有界重试，全部非零后进入 ready 状态。
- [x] 手机向 FFE2 写入任意测试字节时，上层收到内容和长度完全一致的类型化数据事件。
- [x] 上层使用运行时 FFE3 handle 发送任意测试字节时，手机收到内容完全一致的 Notify。
- [x] 手机启用/关闭 FFE3 Notify 时，上层分别收到 `+CLCASTCCCD=1/0` 并更新发送门控。
- [ ] 非法 hex、奇数长度、超长输入和事件队列满不会越界或阻塞蓝牙回调，并产生受限诊断。
- [ ] 普通 BLE 断开会清除连接和订阅状态；驱动重启会使 handle 失效并触发重新查询。
- [ ] BLE 状态机测试入口可以注入驱动事件并捕获上层发送命令。
- [ ] EC/EY 配置仍保持原有 GATT 数据路由和构建行为。

## Blocked by

None - can start immediately. The delivered driver package is an external input required by this issue.
