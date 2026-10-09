# CLCast P2P GO MAC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CLCast advertise the ATBM P2P GO/AP interface MAC from VIF1 instead of the VIF0 P2P Device Address.

**Architecture:** Preserve `carlink_get_wifi_mac()` as the generic VIF0 accessor. Add a CLCast-specific accessor in the Carlink Wi-Fi layer that reads the ATBM `p2p0` VIF1 MAC. Use that accessor in the two CLCast protocol paths that expose the device MAC.

**Tech Stack:** C, FreeRTOS, ATBM Wi-Fi driver, IAR Embedded Workbench.

---

### Task 1: Expose and test the VIF-specific read seam

**Files:**
- Modify: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\lib\wlan\atbm_wifi6\atbm_wifi_driver_api.h:390-399`
- Create: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\tests\clcast_p2p_go_mac_test.c`

- [ ] **Step 1: Write the failing test**

Create a host test that stubs `atbm_wifi_get_mac_address_vif()`, records the
requested VIF, and expects `carlink_get_p2p_go_mac()` to return
`"16:d7:25:57:27:80"` after the stub supplies bytes
`{0x16, 0xd7, 0x25, 0x57, 0x27, 0x80}`. Add a second assertion that a negative
driver return produces `NULL`.

```c
assert(strcmp(carlink_get_p2p_go_mac(), "16:d7:25:57:27:80") == 0);
assert(requested_vif == 1);
driver_return = -1;
assert(carlink_get_p2p_go_mac() == NULL);
```

- [ ] **Step 2: Run the test to verify it fails**

Run the host test build command used by the project test harness. Expected
result: compilation fails because `carlink_get_p2p_go_mac` does not exist.

- [ ] **Step 3: Expose the existing driver function declaration**

Add the existing implementation signature to the ATBM driver header so the
Carlink layer can read a specific VIF without declaring a private prototype:

```c
atbm_int32 atbm_wifi_get_mac_address_vif(atbm_uint8 if_id,
    unsigned char *addr);
```

- [ ] **Step 4: Re-run the test**

Run the same host test command. Expected result: it still fails because the
Carlink-specific accessor is not implemented.

### Task 2: Add the CLCast-specific P2P GO MAC accessor

**Files:**
- Modify: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\app\carlink\common\carlink_common.h:90-105`
- Modify: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\app\carlink\common\carlink_bt_wifi.c:144-148`
- Test: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\tests\clcast_p2p_go_mac_test.c`

- [ ] **Step 1: Declare the helper**

Add this public Carlink declaration next to `carlink_get_wifi_mac()`:

```c
const char *carlink_get_p2p_go_mac(void);
```

- [ ] **Step 2: Implement the helper**

Use a dedicated static 32-byte string buffer. On `WIFI_CHIP_GT`, call
`atbm_wifi_get_mac_address_vif(1, mac)`, format success as lower-case
`%02x:%02x:%02x:%02x:%02x:%02x`, log the result, and return the buffer. On a
negative return or unsupported Wi-Fi chipset, log the failure and return
`NULL`. Do not call `WIFI_GetMAC()` as fallback.

- [ ] **Step 3: Run the unit test to verify it passes**

Run the host test command. Expected result: the test passes, requests VIF1,
returns the GO MAC, and returns `NULL` after a driver read failure.

### Task 3: Route CLCast protocol fields to the GO MAC

**Files:**
- Modify: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\app\carlink\clcast\src\carlink_clcast_ble.c:54-72`
- Modify: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\app\carlink\clcast\src\carlink_clcast.c:168-171`
- Test: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\tests\clcast_p2p_go_mac_test.c`

- [ ] **Step 1: Update B6 preparation**

Replace the B6 source with `carlink_get_p2p_go_mac()`. Keep the existing
`NULL` and MAC-format validation so the parameter exchange is rejected instead
of sending the VIF0 address when VIF1 is unavailable.

- [ ] **Step 2: Update the session property**

Replace the `CLCAST_KEY_DEVICE_MACADDR` source with
`carlink_get_p2p_go_mac()`. Keep the existing all-zero fallback only for a
read failure.

- [ ] **Step 3: Verify source routing**

Run `rg -n "carlink_get_wifi_mac\\(|carlink_get_p2p_go_mac\\("` over the two
CLCast files. Expected result: both CLCast MAC call sites use the GO helper.

### Task 4: Build and on-target verification

**Files:**
- Verify: `D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\proj\amt630hv100.ewp`

- [ ] **Step 1: Rebuild the CLCast firmware configuration in IAR**

Build the active `amt630hv100.ewp` configuration with `CARLINK_CLCAST=1`.
Expected result: no compile or linker errors.

- [ ] **Step 2: Capture a first-connect serial log**

Connect iOS, enable the CLCast BLE notification, and capture the P2P
connection sequence. Expected log includes a `p2p0` MAC of
`16:d7:25:57:27:80`, `clcast ble: parameter set sent`, and then
`clcast: session connected`.

- [ ] **Step 3: Run the existing CLCast host test**

Compile and run `tests\clcast_ble_adapter_test.c` with its CLCast sources.
Expected result: `clcast_ble_adapter_test: PASS`.
