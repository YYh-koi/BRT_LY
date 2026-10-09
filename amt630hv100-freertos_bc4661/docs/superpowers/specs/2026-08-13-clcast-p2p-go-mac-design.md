# CLCast P2P GO MAC Design

## Goal

Make CLCast advertise the ATBM P2P GO/AP interface MAC, rather than the VIF0
P2P Device Address.

## Design

ATBM creates `wlan0` as VIF0 and `p2p0` as VIF1. The driver assigns VIF1 its
own MAC by toggling the local-administered bit of VIF0. CLCast needs the MAC
that is active on `p2p0` (VIF1).

Keep `carlink_get_wifi_mac()` unchanged because it is a shared generic helper.
Add a CLCast-specific helper that reads VIF1 through
`atbm_wifi_get_mac_address_vif(1, ...)`, formats the returned six bytes as a
colon-delimited string, and returns `NULL` on a read failure. Use the new helper
only for CLCast BLE parameter B6 and the CLCast session property
`device_mac_address`.

## Failure handling

If VIF1 cannot be read, CLCast must not silently substitute the VIF0 Device
Address. BLE parameter preparation will fail through its existing `NULL` check;
the session property retains its existing all-zero fallback.

## Verification

Add a host-side unit test around an extracted formatter/reader seam or compile
the helper with a stubbed VIF reader. Verify that the B6 source is the VIF1
value `16:d7:25:57:27:80`, VIF0 is not requested, and read failures return
`NULL`. Then rebuild the IAR firmware and capture serial output showing the
P2P GO MAC and `clcast ble: parameter set sent`.
