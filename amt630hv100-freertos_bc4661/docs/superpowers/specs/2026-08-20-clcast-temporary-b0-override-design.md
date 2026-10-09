# CLCast Temporary B0 Override Design

## Goal

Temporarily send `d4:d7:25:57:27:82` as the CLCast B0 device identifier so the phone treats the test firmware as a different business-level device identity.

## Scope

- Change only B0 construction in `app/carlink/clcast/src/carlink_clcast_ble.c`.
- Keep the actual BLE address, advertising data, GATT service and characteristic UUIDs, and QR-code MAC unchanged.
- Continue to obtain the real BLE address so all existing readiness and parameter validation remains intact.
- Do not change B1 through BB.

## Data Flow

`clcast_ble_prepare_first_connection()` will validate and format the real BLE address as it does today, then replace the local B0 value with `d4:d7:25:57:27:82` before assigning `params.values[0]`. The existing frame encoder will calculate a new CRC16/MODBUS value for the changed B0 payload automatically.

The QR code will continue to show the real BLE address because it independently calls `carlink_get_ble_mac()`.

## Expected Behavior

- BLE scanning, connection, MTU exchange, FFE3 subscription, and notification transport remain unchanged.
- The phone receives B0 value `d4:d7:25:57:27:82`.
- The QR code continues to contain the real BLE MAC, currently `D4:D7:25:57:27:81`.
- Any phone-side activation, binding, or cached identity keyed by B0 may treat this as a new device; this is the purpose of the temporary test.

## Verification

- Add or update a focused test to confirm the B0 frame contains `d4:d7:25:57:27:82` and has a valid recalculated CRC.
- Run the focused CLCast BLE tests.
- Inspect the source diff to confirm no advertising, QR, or FFE0-FFE3 definitions changed.
- On hardware, confirm the first B0 notification contains the new value while the QR code and BLE connection behavior remain unchanged.

## Reversion

Remove the local B0 override and use the formatted value returned by `carlink_get_ble_mac()` again.
