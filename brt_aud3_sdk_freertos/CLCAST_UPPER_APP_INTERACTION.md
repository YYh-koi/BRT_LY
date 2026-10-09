# CLCast BLE Upper App Interaction Guide

This document describes the information the upper application needs after
enabling the CLCast BLE driver path.

## Scope

- This layer only provides BLE GATT service registration, handle query, data
  uplink event reporting, read-value cache setting, and notification sending.
- CLCast business protocol parsing remains in the upper application.
- CLCast phone uplink data uses a dedicated AT event and does not reuse
  `+GATTDATA`.

## BLE Service

When `CARLINK_CLCAST` is enabled, the BLE connection type is set to
`BLE_CLCAST_CONNECTION`, and the driver registers the following GATT service:

| Item | UUID | Property | Direction |
| --- | --- | --- | --- |
| Service | `0000FFE0-0000-1000-8000-C9EFDEFDEF9F` | Primary service | - |
| Read characteristic | `0000FFE1-0000-1000-8000-C9EFDEFDEF9F` | Read | Phone reads device |
| Write characteristic | `0000FFE2-0000-1000-8000-C9EFDEFDEF9F` | Write / Write without response | Phone writes to device |
| Notify characteristic | `0000FFE3-0000-1000-8000-C9EFDEFDEF9F` | Notify | Device notifies phone |
| CCCD | `00002902-0000-1000-8000-C9EFDEFDEF9F` | Descriptor | Phone enables notify |

## AT Commands For Upper App

### Query CLCast GATT Handle

Use this after GATT service registration has completed.

```text
AT+GATTHANDLE=<uuid16>
```

Supported `<uuid16>` values:

| Query | Meaning |
| --- | --- |
| `FFE1` | Read characteristic value handle |
| `FFE2` | Write characteristic value handle |
| `FFE3` | Notify characteristic value handle |

Response:

```text
+GATTHANDLE=<uuid16>,<handle>
```

Example:

```text
AT+GATTHANDLE=FFE3
+GATTHANDLE=FFE3,0012
```

If the service has not been added yet, `<handle>` may be `0000`. The upper
application should query again after the GATT service-added flow finishes.

### Set FFE1 Read Value

Use this to set the cached data returned when the phone reads `FFE1`.

```text
AT+GATTREADSET=<hex>
```

Rules:

- `<hex>` is uppercase or lowercase hexadecimal text.
- The hex string length must be even.
- Maximum payload length is 512 bytes.
- Empty payload is allowed and clears the cached read value.

Responses:

```text
OK
```

or

```text
ERROR
```

Example:

```text
AT+GATTREADSET=01020304
OK
```

### Send Data To Phone

CLCast downlink reuses the existing GATT notification command.

```text
AT+GATTDATASEND=<handle>,<hex>
```

Use the handle returned by:

```text
AT+GATTHANDLE=FFE3
```

Example:

```text
AT+GATTHANDLE=FFE3
+GATTHANDLE=FFE3,0012

AT+GATTDATASEND=0012,01020304
OK
```

The phone should enable the `FFE3` CCCD before the upper application sends
notification data. The upper application should not query, save, or derive the
CCCD descriptor handle.

## AT Events To Upper App

### Phone Writes CLCast Data

When the phone writes to `FFE2`, the driver reports:

```text
+CLCASTDATA=<hex>
```

Example:

```text
+CLCASTDATA=01020304
```

Notes:

- The payload is raw bytes converted to hex text.
- Empty phone writes are reported as `+CLCASTDATA=`.
- CLCast data is intentionally separated from the legacy `+GATTDATA=<hex>`
  event so upper-layer routing can distinguish CLCast from EC/EY or generic
  GATT traffic.

### CLCast CCCD Status Event

When the phone writes the CCCD that belongs to `FFE3`, the driver reports the
dedicated CLCast notify status:

```text
+CLCASTCCCD=<status>
```

Status values:

| Status | Meaning |
| --- | --- |
| `1` | Phone enabled `FFE3` notify |
| `0` | Phone disabled `FFE3` notify |

The driver obtains the CLCast CCCD handle from the `FFE3` attribute mapping
instead of using a descriptor service-added event handle. On the current local
attribute table, `FFE3` value handle is `000B`, so the CCCD handle is `000C`.
The CCCD payload is parsed as a two-byte little-endian value:

| CCCD payload | Event |
| --- | --- |
| `0100` | `+CLCASTCCCD=1` |
| `0000` | `+CLCASTCCCD=0` |

Examples:

```text
+CLCASTCCCD=1
+CLCASTCCCD=0
```

Non-CLCast descriptor writes still use the existing event:

```text
+CCCDEN:<handle>
```

## Recommended Upper App Flow

1. Enable the CLCast build path with `CARLINK_CLCAST`.
2. Wait for BLE/GATT initialization to finish.
3. Query handles:

```text
AT+GATTHANDLE=FFE1
AT+GATTHANDLE=FFE2
AT+GATTHANDLE=FFE3
```

4. Set the read cache if the phone may read `FFE1`:

```text
AT+GATTREADSET=<hex>
```

5. Wait for the phone to enable notify on `FFE3`:

```text
+CLCASTCCCD=1
```

6. Receive phone uplink data from:

```text
+CLCASTDATA=<hex>
```

7. Send device downlink data through:

```text
AT+GATTDATASEND=<FFE3_handle>,<hex>
```

## Compatibility Notes

- EC/EY behavior remains on the existing path.
- Generic GATT write reporting still uses `+GATTDATA=<hex>`.
- CLCast write reporting uses only `+CLCASTDATA=<hex>`.
- CLCast CCCD writes use `+CLCASTCCCD=<status>` instead of the generic
  `+CCCDEN:<handle>` event.
- CLCast notification sending does not add a new AT command; it reuses
  `AT+GATTDATASEND`.
- No send-complete event is added for CLCast in this change.
