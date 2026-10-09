# EY QR-Guided Connection Design

## Goal

Allow the supported EY mobile app to start wireless projection by scanning a
QR code shown on the head unit, while retaining the existing BLE handshake.

## Scope

The firmware will create the QR payload from the exact EY connection URL
already emitted by `carlink_start_wlan()`.  Once the Bluetooth MAC and Wi-Fi
P2P credentials have been initialized, the payload is stored through the
existing AWTK `set_qr_text_buf()` interface.  The existing UI is responsible
for rendering that buffer as a QR code.

The existing `AT+GATTSET=AAA5...` command remains unchanged.  The mobile app
uses the QR payload to discover the device and then completes the established
BLE/P2P connection and authentication sequence.

## Design

Introduce a small EY QR-payload formatter with no platform dependencies.  It
accepts the device MAC, Wi-Fi SSID, password, P2P name, and output buffer; it
formats the current `https://appstore.eryanet.com` URL with the existing
product ID, IP address, ports, key, and display size.  It returns failure for
invalid inputs or truncated output.

`carlink_start_wlan()` will call the formatter after its connection parameters
are ready.  For AWTK builds it will publish a successfully formatted URL to
`set_qr_text_buf()`.  On formatting failure it will clear the buffer, preventing
a stale QR code from being displayed.  Non-AWTK builds retain their current
behavior and do not reference the UI callback.

## Error Handling

- Empty required connection fields or a too-small output buffer produce no QR
  payload.
- A formatting failure clears the AWTK QR buffer and logs a diagnostic.
- The existing BLE GATT parameter transmission is still attempted independently
  of QR-payload generation, preserving the current connection fallback.

## Verification

A standalone host test will verify a complete URL, invalid required input, and
output truncation.  The production IAR AWTK configuration must be rebuilt, then
the target must show a scannable QR code and complete an EY app connection.
