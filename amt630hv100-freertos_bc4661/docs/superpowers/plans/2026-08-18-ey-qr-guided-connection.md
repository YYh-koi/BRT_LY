# EY QR-Guided Connection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the EY connection URL to the existing QR-code UI while preserving the current BLE handshake.

**Architecture:** A new dependency-free formatter owns the EY URL layout and validates all fields before formatting. `carlink_bt_wifi.c` uses it after Wi-Fi P2P credentials are ready; AWTK builds publish the result to the established QR text buffer, while BLE GATT configuration continues unchanged.

**Tech Stack:** C99-compatible embedded C, FreeRTOS firmware, AWTK, IAR Embedded Workbench.

---

### Task 1: Add a testable EY QR-payload formatter

**Files:**
- Create: `app/carlink/EY/carlink_ey_qr.h`
- Create: `app/carlink/EY/carlink_ey_qr.c`
- Create: `tests/carlink_ey_qr_test.c`

- [ ] **Step 1: Write the failing host test**

```c
assert(carlink_ey_build_qr_url(url, sizeof(url), "58C587F4293D",
    "AP630_CARLINK_293D", "88888888", "DIRECT-AP630_CARLINK_p2p_293D") == 0);
assert(strstr(url, "device=EY_58C587F4293D") != NULL);
assert(strstr(url, "port1=11111&port2=11113") != NULL);
assert(carlink_ey_build_qr_url(url, sizeof(url), "", "ssid", "pwd", "name") < 0);
assert(carlink_ey_build_qr_url(url, 16, "58C587F4293D", "ssid", "pwd", "name") < 0);
```

- [ ] **Step 2: Run the test and confirm the expected red state**

Run: `gcc -std=c99 -Wall -Wextra -Iapp/carlink/EY tests/carlink_ey_qr_test.c app/carlink/EY/carlink_ey_qr.c -o tests/carlink_ey_qr_test.exe`

Expected: compilation fails because `carlink_ey_qr.h` and its formatter do not yet exist.

- [ ] **Step 3: Implement the smallest formatter API**

```c
int carlink_ey_build_qr_url(char *buffer, size_t buffer_size,
    const char *device_mac, const char *ssid, const char *password,
    const char *p2p_name);
```

The implementation returns `0` only when all fields are non-empty and the full
existing EY URL fits. It otherwise writes an empty string when possible and
returns `-1`.

- [ ] **Step 4: Re-run the host test and confirm green**

Run: `gcc -std=c99 -Wall -Wextra -Iapp/carlink/EY tests/carlink_ey_qr_test.c app/carlink/EY/carlink_ey_qr.c -o tests/carlink_ey_qr_test.exe; .\tests\carlink_ey_qr_test.exe`

Expected: `carlink_ey_qr_test: PASS`.

### Task 2: Publish EY QR data from the Wi-Fi startup path

**Files:**
- Modify: `app/carlink/common/carlink_bt_wifi.c:48-57, 319-336`
- Modify: `proj/amt630hv100.ewp:7391-7399`

- [ ] **Step 1: Add the failing integration assertion**

Extend `tests/carlink_ey_qr_test.c` with the exact values passed by the EY
Wi-Fi startup path and assert the payload contains `productid`, SSID, password,
P2P name, device MAC, both ports, key, width, and height.

- [ ] **Step 2: Run the test and confirm the new assertion fails before wiring**

Run: `gcc -std=c99 -Wall -Wextra -Iapp/carlink/EY tests/carlink_ey_qr_test.c app/carlink/EY/carlink_ey_qr.c -o tests/carlink_ey_qr_test.exe; .\tests\carlink_ey_qr_test.exe`

Expected: failure until the formatter emits the complete production URL.

- [ ] **Step 3: Wire the formatter without changing BLE behavior**

Include `carlink_ey_qr.h` for `CARLINK_EY` builds. Under `AWTK`, declare
`set_qr_text_buf()` and publish a 256-byte formatted payload after
`carlink_reset_wifi_ap_info()`. On formatter failure, clear the buffer and log
an EY QR error. Leave the `AT+GATTSET=AAA5...` construction and
`brtbt_send_at_command()` call unchanged. Add `carlink_ey_qr.c` to the IAR
`carlink` group in `proj/amt630hv100.ewp`.

- [ ] **Step 4: Re-run the host test**

Run: `gcc -std=c99 -Wall -Wextra -Iapp/carlink/EY tests/carlink_ey_qr_test.c app/carlink/EY/carlink_ey_qr.c -o tests/carlink_ey_qr_test.exe; .\tests\carlink_ey_qr_test.exe`

Expected: `carlink_ey_qr_test: PASS`.

- [ ] **Step 5: Build and perform target smoke test**

Run: `iarbuild proj\amt630hv100.eww -batch amt630hv100-awtk`

Expected: the AWTK firmware builds; after flashing, the idle EY page exposes a
scannable QR code and the EY app connects through its normal BLE/P2P handshake.

### Task 3: Review the scoped change

**Files:**
- Modify: `app/carlink/EY/carlink_ey_qr.h`
- Modify: `app/carlink/EY/carlink_ey_qr.c`
- Modify: `app/carlink/common/carlink_bt_wifi.c`
- Modify: `proj/amt630hv100.ewp`
- Modify: `tests/carlink_ey_qr_test.c`

- [ ] **Step 1: Inspect the diff**

Run: `git diff -- app/carlink/EY/carlink_ey_qr.h app/carlink/EY/carlink_ey_qr.c app/carlink/common/carlink_bt_wifi.c proj/amt630hv100.ewp tests/carlink_ey_qr_test.c`

Expected: only the QR formatter, its unit test, the QR-buffer publication, and
the IAR source entry are changed; no BLE command changes are present.

- [ ] **Step 2: Preserve the user’s existing worktree state**

Do not create a commit in this already-dirty worktree. Report the exact files
changed and the unavailable verification, if no C/IAR toolchain is installed.
