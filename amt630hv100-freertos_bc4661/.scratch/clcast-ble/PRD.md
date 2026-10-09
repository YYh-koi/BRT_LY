# CLCast BLE First Connection and MJPEG Map Casting

Status: ready-for-agent

## Problem Statement

The firmware has a partial CLCast receiver integration for Wi-Fi/TCP session handling and H.264 display, but it does not implement the phone application's BLE first-connection contract. The current Bluetooth integration does not consume the delivered CLCast GATT interface, the QR code exposes Wi-Fi credentials instead of bootstrapping the phone application and BLE connection, and the active video capability is H.264 even though the target CLCast product uses MJPEG. As a result, the receiver cannot complete the documented scan, BLE parameter exchange, P2P connection, and map-casting workflow with the target phone application.

## Solution

Add a dedicated CLCast BLE application adapter above the delivered Bluetooth driver interface. The shared Bluetooth layer will translate driver AT text into typed CLCast events, while the CLCast adapter will manage handles, CCCD readiness, first-connection parameter frames, and bounded uplink handling. Replace the existing Wi-Fi credential QR with the phone application's BLE bootstrap QR. Keep the existing P2P and CLCast TCP session architecture, but advertise MJPEG consistently and display incoming JPEG frames through a common MJPEG player shaped like the existing EC video player integration.

The first milestone delivers BLE first connection, P2P parameter exchange, and MJPEG map casting. Activation can be bypassed only by an explicit development configuration until the application team supplies the activation protocol. Simple-mode TBT remains out of scope.

## User Stories

1. As a driver, I want to scan the receiver QR code with the CLCast phone application so that I can identify and connect to the correct receiver.
2. As a driver, I want the phone application to connect over BLE before Wi-Fi so that network credentials do not need to be exposed in the QR code.
3. As a driver, I want the receiver name shown by the phone to match the advertised Bluetooth name so that I can recognize the device.
4. As a driver, I want the phone to receive the receiver identity and casting capabilities so that it can select a compatible casting workflow.
5. As a driver, I want the phone to receive the P2P connection parameters so that it can establish the Wi-Fi video connection automatically.
6. As a driver, I want map casting to continue if BLE briefly disconnects after Wi-Fi casting has started so that navigation is not interrupted unnecessarily.
7. As a driver, I want the receiver to display the phone's MJPEG map stream so that I can view navigation on the in-vehicle display.
8. As a driver, I want casting to stop cleanly when the Wi-Fi/TCP casting session ends so that the native receiver UI returns predictably.
9. As an integrator, I want CLCast BLE data to use a dedicated upper-layer event so that it cannot be confused with EC/EY or generic GATT traffic.
10. As an integrator, I want GATT handles to be queried at runtime so that driver service layout changes do not require hard-coded handle updates.
11. As an integrator, I want handle queries to retry when the service is not ready so that Bluetooth initialization timing does not make startup intermittent.
12. As an integrator, I want handle values cached across ordinary phone reconnects so that reconnecting does not add unnecessary AT exchanges.
13. As an integrator, I want handles invalidated after Bluetooth driver restart or handle-related failures so that stale values are not reused.
14. As an integrator, I want the driver to report the FFE3 subscription state directly so that other services' 2902 descriptors cannot enable CLCast notifications.
15. As an integrator, I want BLE disconnect to clear connection and subscription state so that a later connection performs a fresh parameter exchange.
16. As an integrator, I want first-connection frames sent in a paced sequence so that the Bluetooth driver's unknown transmit queue depth is not overrun.
17. As an integrator, I want oversized or malformed BLE uplink data rejected safely so that invalid phone traffic cannot corrupt memory.
18. As an integrator, I want unknown phone uplink messages recorded in a bounded diagnostic form so that the missing activation and TBT contracts can be learned during integration.
19. As an integrator, I want unknown phone uplink messages to have no business effect so that guessed protocol behavior cannot alter a casting session.
20. As an integrator, I want queue overflow to fail the current first-connection exchange without blocking the Bluetooth callback so that Bluetooth service remains responsive.
21. As an integrator, I want the CLCast BLE protocol isolated from the shared Carlink event queue so that EC and EY behavior remains unchanged.
22. As an integrator, I want B0 and B6 MAC addresses rendered in lowercase colon-separated form so that they match the application contract examples.
23. As an integrator, I want the B0 device UUID derived from the Bluetooth MAC so that the first implementation has a deterministic device identity without a new provisioning flow.
24. As an integrator, I want B1 to match the existing AP630 Carlink Bluetooth name so that advertising and first-connection data remain consistent.
25. As an integrator, I want B2 to advertise map mode only so that the phone cannot select the unimplemented TBT simple mode.
26. As an integrator, I want B3 to use the configured casting dimensions so that the phone and receiver agree on the maximum MJPEG frame size.
27. As an integrator, I want B4 to advertise 30 fps so that the phone does not select an unsupported frame rate.
28. As an integrator, I want B5 to advertise MJPEG only so that the target phone application sends the format implemented by this product.
29. As an integrator, I want B6 through BA to describe the active P2P MAC, SSID, password, receiver IP, and TCP port so that the phone can establish the existing network session.
30. As an integrator, I want B0 through BA protected by CRC16/MODBUS so that the phone can detect corrupted parameter frames.
31. As an integrator, I want the documented CRC sample to remain a permanent test vector so that CRC parameters and byte order cannot drift.
32. As an integrator, I want FFE1 cleared in the first milestone so that the receiver does not invent an undefined read-value protocol.
33. As an integrator, I want all first-connection parameter frames sent through FFE3 notifications so that the phone receives them through one defined path.
34. As an integrator, I want the bootstrap QR template configurable so that the final application download link and channel format can be supplied without redesigning the connection flow.
35. As an integrator, I want an explicit development activation bypass so that BLE, P2P, and MJPEG can be tested before the activation response protocol is available.
36. As a release owner, I want startup logs to state whether activation enforcement is enabled so that a development bypass cannot be mistaken for production behavior.
37. As a release owner, I want production acceptance to require the final activation protocol so that an engineering bypass is not shipped accidentally.
38. As a video integrator, I want CLCast video callbacks to use start, frame, and stop operations shaped like EC so that protocol code does not own decoder or display details.
39. As a video integrator, I want each CLCast JPEG frame copied into a fixed aligned input buffer so that decoder address, cache, and lifetime requirements do not depend on the CLCast library callback buffer.
40. As a video integrator, I want oversized JPEG frames rejected before copying so that malformed or unexpected input cannot overflow memory.
41. As a video integrator, I want JPEG decoding performed by the platform hardware path so that map casting has adequate performance.
42. As a video integrator, I want YUV420 and YUV422 JPEG output supported so that the implementation matches the proven platform JPEG display path.
43. As a video integrator, I want unsupported JPEG color formats rejected with diagnostics so that failures are visible and safe.
44. As a video integrator, I want JPEG dimensions checked against the B3/session dimensions so that buffer sizing and declared capability remain consistent.
45. As a video integrator, I want smaller JPEG frames scaled to the configured display area so that valid lower-resolution input still renders correctly.
46. As a video integrator, I want the native UI restored when the CLCast stream stops so that display ownership is returned cleanly.
47. As a maintainer, I want existing H.264 code preserved for other product configurations so that the MJPEG change does not remove a previously supported decoder path.
48. As a maintainer, I want EC and EY Bluetooth and video paths unchanged so that adding CLCast does not regress existing products.
49. As a tester, I want the BLE state machine exercised without Bluetooth hardware so that handle, CCCD, frame order, CRC, disconnect, and reconnect behavior can be repeated automatically.
50. As a tester, I want a known JPEG frame decoded on target hardware so that the decoder, PXP, and LCD path are verified together.
51. As a tester, I want a real phone application smoke test covering QR scan through MJPEG display so that undocumented application assumptions are detected before release.

## Implementation Decisions

- The delivered driver interface is the fixed lower-layer contract: the driver owns CLCast GATT registration, runtime handle query, FFE1 read cache, FFE2 uplink reporting through the dedicated CLCast event, FFE3 notification sending, and CCCD write reporting.
- Driver delivery must be synchronized into the firmware before integration; headers, source, and linked prebuilt libraries must come from the same version.
- The shared Bluetooth adapter parses AT text and emits typed CLCast events. It does not parse B0 through BA, activation, TBT, QR, or casting business fields.
- The CLCast BLE adapter owns its payload copies and a fixed bounded event queue. It does not allocate heap memory per event.
- Enqueue operations are nonblocking. Oversized input or queue overflow aborts the current first-connection exchange and records diagnostics instead of silently dropping protocol data.
- GATT handles for FFE1, FFE2, and FFE3 are queried after Bluetooth/GATT initialization. Zero handles are retried with bounded timing. Ordinary phone reconnects reuse valid cached handles; driver restart and handle errors invalidate them.
- The driver owns the FFE3 CCCD association and reports `+CLCASTCCCD=1/0`. Notification readiness follows this event and is cleared on BLE disconnect. The application never queries a global 2902 handle.
- FFE1 is cleared in the first milestone. First-connection parameters are sent only through paced FFE3 notifications using the runtime FFE3 handle.
- The sender does not wait for a send-complete event because the delivered interface does not provide one. Packet pacing is configurable; immediate command failure stops the current exchange.
- Parameter frames use header bytes FF FE, one-byte length, one-byte field type, UTF-8 field data without a terminator, and a two-byte CRC.
- CRC is CRC16/MODBUS with polynomial 0xA001, initial value 0xFFFF, reflected processing, xorout 0x0000, and little-endian wire bytes.
- The receiver sends B0 through BA in ascending type order after connection, valid handles, and notification readiness are all present. A new BLE connection may repeat the complete sequence.
- B0 is the Bluetooth MAC rendered as a lowercase colon-separated string. Changing the Bluetooth module changes the device UUID and therefore the activation/binding identity.
- B1 is the actual advertised AP630 Carlink Bluetooth name.
- B2 is the UTF-8 string `1`, advertising map mode only.
- B3 is the configured casting width and height rendered as `<width>x<height>`.
- B4 is the UTF-8 string `30`.
- B5 is the UTF-8 string `1`, advertising MJPEG.
- B6 is the Wi-Fi/P2P MAC in lowercase colon-separated form.
- B7 is the active P2P group SSID, B8 is its password, B9 is `192.168.13.1`, and BA is `9000`.
- The connection QR is a BLE bootstrap QR containing the phone application link/identity, receiver BLE MAC, and project channel. It does not contain Wi-Fi credentials. Exact field syntax is configuration pending the final application template.
- Unknown CLCast uplink bytes are length-checked, copied safely, and logged only as bounded diagnostics. They do not trigger replies or business state changes in the first milestone.
- Activation enforcement is controlled by an explicit product configuration. The engineering build may bypass activation to test the first milestone; production completion remains blocked on the real activation protocol.
- BLE is a bootstrap transport for map mode. Once the Wi-Fi/TCP casting session is active, BLE disconnect clears BLE state but does not stop MJPEG casting.
- The existing CLCast TCP session protocol and prebuilt session library remain in place. The application capability callbacks are changed from H.264 to MJPEG for this product.
- The common video layer exposes an MJPEG player with init, process, and uninit operations matching the EC player shape. CLCast callbacks call it synchronously.
- The MJPEG player owns one fixed aligned compressed-frame buffer and one bounded YUV output buffer. Each callback frame is length-checked, copied, cache-cleaned, and decoded through the platform JPEG hardware API.
- The first MJPEG implementation accepts YUV420 and YUV422 output only, scales through the platform pixel processor when required, and displays through the LCD video layer.
- B3/session dimensions are the maximum accepted JPEG dimensions. Smaller frames may be scaled; larger frames and unsupported color formats are rejected.
- Existing H.264 functions remain available for other product configurations, but the current CLCast build does not advertise H.264.

## Testing Decisions

- The highest repeatable test seam is the CLCast BLE state machine. Tests feed typed driver events and capture outbound driver commands or notification payloads without requiring Bluetooth hardware.
- State-machine tests cover zero-handle retry, complete handle readiness, FFE3 subscription state, notification gating, B0 through BA order, field values, reconnect behavior, BLE disconnect behavior, malformed hex, oversized uplink data, and queue overflow recovery.
- The complete frame encoder is tested through the state-machine output rather than only through internal helper functions.
- A permanent golden test verifies that bytes `FF FE 05 B4 32 34` produce CRC bytes `69 9D`.
- MAC formatting and configured B2 through B5 values are covered by externally visible frame-output tests.
- The MJPEG hardware path is verified on the target board with known valid YUV420 and YUV422 JPEG images, an oversized image, and an unsupported color-format image.
- Target validation records decoder return values, detected dimensions and format, scaling/display result, native UI restoration, and memory allocation failures.
- Full acceptance uses the matching IAR AWTK configuration, target hardware, and the real CLCast phone application to validate QR scan, BLE connection, handle query, parameter notifications, P2P connection, TCP session, MJPEG display, BLE loss during casting, and session teardown.
- EC and EY configurations receive build and smoke-test regression coverage because they share Bluetooth and video modules.

## Out of Scope

- Phone application implementation.
- Changes to the delivered Bluetooth GATT service or AT command contract, except integration fixes required to synchronize the delivered version.
- Production activation and user binding behavior until the phone uplink protocol is supplied.
- Simple-mode TBT parsing and display.
- Audio playback.
- Touch and key reverse control.
- Runtime support for both H.264 and MJPEG in the same CLCast product configuration.
- Replacing the existing CLCast TCP session protocol or prebuilt session library.
- General refactoring of EC, EY, CarPlay, or Android Auto Bluetooth paths.

## Further Notes

- The current workspace did not contain the delivered CLCast Bluetooth driver symbols or AT interface implementation when inspected. Driver synchronization is a hard prerequisite for end-to-end integration.
- The final QR template, project channel, BLE MTU and maximum payload, service-ready signal, CCCD disable behavior, notification command queue depth, and exact meaning of a successful send response require confirmation from the driver or application teams.
- The current application document does not define activation, binding, disconnect-reason, or TBT uplink messages. Unknown uplink diagnostics are intended to support discovery, not to replace an authoritative protocol.
- The application document's B0 through BA strings are treated as machine-readable values without labels or units; real-phone integration must confirm this assumption.
- The engineering activation bypass must be visibly logged and reviewed before any production build.
