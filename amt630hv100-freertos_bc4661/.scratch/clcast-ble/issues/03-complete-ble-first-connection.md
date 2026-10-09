# 完成 BLE 首连参数集和二维码

Status: in-progress - QR/subscription work; real APP skips MTU and receives truncated long fields

The temporary BLE-name-only QR validation is complete and
`CLCAST_QR_BT_NAME_TEST` is now disabled. The configured QR payload is:

```text
https://bucket.wealthmid.com/cycling/map/map.apk?SN=TYW001&MAC=<BLE_MAC>
```

`BLE_MAC` is populated at runtime in uppercase colon-separated form.

## Implementation status (2026-07-10)

- The first-connection state machine now sends B0 through BB in ascending
  order, one frame per configurable interval, using CRC16/MODBUS. BB carries
  the UTF-8 string `0` as the parameter synchronization terminator.
- B0/B1 use the runtime BT MAC and the exact name sent to the Bluetooth
  module. B6 through BA use the current Wi-Fi MAC, P2P SSID/password, receiver
  IP, and TCP port.
- MAC values are normalized to lowercase colon-separated strings.
- A transport loss resets the sequence; reconnect plus FFE3 resubscription
  starts again from B0. Immediate send failure latches the current exchange.
- Unknown uplink remains bounded diagnostic-only data and has no business
  effect. Queue overflow aborts the current BLE exchange.
- The legacy QR containing Wi-Fi credentials was removed. The BLE bootstrap
  QR now requires `CLCAST_APP_QR_TEMPLATE`, `CLCAST_APP_DOWNLOAD_LINK`, and
  `CLCAST_PROJECT_CHANNEL`; missing configuration clears the QR and logs a
  diagnostic. The final APP template is still required externally.
- `CLCAST_DEVELOPMENT_ACTIVATION_BYPASS` is explicitly enabled for the current
  CLCast board configuration. Startup logs the policy; disabling it blocks the
  casting server because the production activation protocol is not available.
- Host tests were updated for field order, values, CRC, pacing, MAC formatting,
  failure, and reconnect. Per user request, command-line build/tests were not run.
- Target log at 17:54 exposed an initialization race: the phone connected about
  two seconds before the CLCast BLE application callback was registered, so
  `+GATTSTAT=3` was missed while the later `+CLCASTCCCD=1` was received. The
  transport correctly stayed gated and sent no fields. The common Bluetooth
  layer now caches connected/CCCD state even without a registered consumer;
  CLCast reads that state immediately after registering its callback.
- Target log at 18:00 verified recovery with `initial connected=1 notify=0`,
  then queued B0 through BA in order after `+CLCASTCCCD=1`. Frame values, CRC,
  runtime FFE3 handle, and pacing were correct at the AT boundary.
- HCI proved that the negotiated ATT MTU remained 23. Notifications therefore
  carried at most 20 value bytes: B0/B6 (23-byte frames) lost 3 bytes and B1/B7
  (24-byte frames) lost 4 bytes, including their CRC tails. The driver accepted
  the original commands without reporting truncation. Issue 03 cannot pass
  until the driver negotiates MTU >= 27 and rejects oversize notifications.
- Follow-up target log at 18:30 verified the stack's client-initiated MTU
  exchange path. The phone requested `0x0205` (517), the receiver responded
  with 517, and FFE3 was subscribed afterward. B0/B6 then appeared as complete
  23-byte values and B1/B7 as complete 24-byte values, including their CRC
  tails. All B0-BA raw notifications are now verified at the HCI boundary.
- The remaining transport hardening item is that the driver still silently
  truncates oversized sends when the phone has not negotiated a larger MTU.
  Final CLCast APP validation must confirm that it exchanges MTU before CCCD.
- The 2026-07-13 11:04 real APP trace confirmed this is an active integration
  failure, not only a theoretical risk: the APP enabled FFE3 without exchanging
  MTU, and B0/B1/B6/B7 were truncated at 20 value bytes. The APP subsequently
  did not start P2P and disconnected. MTU negotiation or driver-side oversize
  rejection remains required before real APP BLE bootstrap can pass.

## Next target validation

Build and flash from IAR, connect with the same BLE test client, and enable
FFE3 Notify. The firmware log should show `field B0 sent` through `field BB
sent`, followed by `parameter set queued`, with approximately 100 ms between
frames. Based on the latest target log, the expected current values are:

If the phone reconnects before CLCast initialization finishes, the log should
also show `initial connected=1 notify=0` (or `notify=1` if subscription already
completed). This confirms the early-connection state was recovered.

| Field | Expected value |
| --- | --- |
| B0 | `2c:cc:7a:74:42:7c` |
| B1 | `AP630_CARLINK_427C` |
| B2 | `1` |
| B3 | `1024x600` |
| B4 | `30` |
| B5 | `0` (H.264) |
| B6 | `2c:cc:7a:74:42:7b` |
| B7 | `AP630_CARLINK_427C` |
| B8 | `88888888` |
| B9 | `192.168.13.1` |
| BA | `9000` |
| BB | `0` |

The application log redacts notification payloads because B8 contains the P2P
password. Driver/HCI debug may still expose raw packets and should be disabled
after integration. For this checkpoint, only raw BLE receipt, order, timing,
and CRC need validation; real CLCast APP behavior is deferred until the full
BLE/P2P/MJPEG path is implemented.

## Parent

`.scratch/clcast-ble/PRD.md`

## What to build

把单字段示踪扩展成完整 CLCast BLE 首连。接收端显示可配置的 APP/BLE 引导二维码，在手机订阅 Notify 后依次发送 B0～BA，并向手机提供当前 P2P 连接参数。手机未知上行数据被安全观测但不触发业务行为；开发构建可通过显式配置绕过尚未提供的激活协议。

## Acceptance criteria

- [ ] 二维码只包含 APP 链接/标识、BLE MAC 和项目渠道，不包含 SSID、密码、IP 或 TCP 端口。
- [ ] 二维码字段模板、下载链接和渠道均由项目配置提供，缺少正式模板时有明确诊断。
- [ ] B0 使用 BT MAC 生成，并采用小写冒号格式。
- [ ] B1 与设备实际广播的 `AP630_CARLINK_<后缀>` 名称完全一致。
- [ ] B2=`1`、B3=`<实际宽度>x<实际高度>`、B4=`30`、B5=`1`，均为不带标签和单位的 UTF-8 字符串。
- [ ] B6 使用小写冒号格式的 Wi-Fi/P2P MAC；B7/B8 使用当前 P2P 组网 SSID和密码；B9=`192.168.13.1`；BA=`9000`。
- [ ] B0～BA 按类型升序逐帧发送，每帧均通过 CRC16/MODBUS 验证，发送节拍可配置。
- [ ] 普通手机重连可复用有效 handle，但会在重新订阅后重新发送完整参数集。
- [ ] 未知手机上行数据经过长度和队列边界检查，仅记录受限摘要和计数，不回显、不改变会话状态。
- [ ] 激活绕过只能由显式开发配置启用，启动日志明确打印当前激活策略。
- [ ] 未开启绕过且尚未激活时，不进入正式投屏流程。
- [ ] 自动测试覆盖完整字段值、顺序、MAC 格式、参数重发、未知上行和错误恢复。

## Blocked by

- `.scratch/clcast-ble/issues/02-send-one-b4-parameter-frame.md`
