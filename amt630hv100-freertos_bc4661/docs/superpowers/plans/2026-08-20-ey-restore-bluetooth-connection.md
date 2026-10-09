# EY Restore Bluetooth Connection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make pure EY builds use the original Bluetooth bootstrap path by default while retaining the QR-direct implementation.

**Architecture:** Change only the local connection-mode compile-time default in `carlink_bt_wifi.c`. Existing `#if !CARLINK_EY_QR_DIRECT` branches already contain the required callback registration, Bluetooth profile selection, Bluetooth enable command, MAC acquisition, and `AAA5` payload flow, so no protocol logic is added or removed.

**Tech Stack:** C, C preprocessor, PowerShell verification, existing EY QR C unit test.

---

### Task 1: Restore the EY Bluetooth default

**Files:**
- Modify: `app/carlink/common/carlink_bt_wifi.c:64-68`
- Verify: `tests/carlink_ey_qr_test.c`

- [ ] **Step 1: Run a failing source-level mode check**

Run:

```powershell
$p = 'app/carlink/common/carlink_bt_wifi.c'
$text = Get-Content -LiteralPath $p -Raw
if ($text -notmatch '(?m)^#define CARLINK_EY_QR_DIRECT 0$') { throw 'EY Bluetooth mode is not the default' }
```

Expected: FAIL with `EY Bluetooth mode is not the default` because the pure EY branch currently defines the macro as `1`.

- [ ] **Step 2: Change the pure EY mode default**

Replace:

```c
#if CARLINK_EY && !CARLINK_EC && !CARLINK_CP && !CARLINK_AA && !CARLINK_CLCAST
#define CARLINK_EY_QR_DIRECT 1
#else
#define CARLINK_EY_QR_DIRECT 0
#endif
```

with:

```c
#if CARLINK_EY && !CARLINK_EC && !CARLINK_CP && !CARLINK_AA && !CARLINK_CLCAST
#define CARLINK_EY_QR_DIRECT 0
#else
#define CARLINK_EY_QR_DIRECT 0
#endif
```

This deliberately keeps the existing conditional structure so the QR-direct mode can be restored later with a one-line change.

- [ ] **Step 3: Re-run the mode check**

Run the PowerShell command from Step 1.

Expected: PASS with exit code `0` and no output.

- [ ] **Step 4: Verify the retained Bluetooth and QR branches**

Run:

```powershell
$p = 'app/carlink/common/carlink_bt_wifi.c'
$text = Get-Content -LiteralPath $p -Raw
$required = @('bt_register_event_cb(NULL, carlink_bt_callback)', 'bt_set_support_carplay_android_auto()', 'carlink_bt_open_nolock()', 'AT+GATTSET=AAA5', 'carlink_ey_build_qr_url')
foreach ($token in $required) { if (-not $text.Contains($token)) { throw "Missing retained path: $token" } }
```

Expected: PASS with exit code `0` and no output, proving the Bluetooth bootstrap remains present and QR code support was not deleted.

- [ ] **Step 5: Run the existing QR helper test if a C compiler is available**

Run:

```powershell
$cc = Get-Command gcc -ErrorAction SilentlyContinue
if ($cc) {
    gcc -std=c99 -Wall -Wextra -Iapp/carlink/EY tests/carlink_ey_qr_test.c app/carlink/EY/carlink_ey_qr.c -o tests/carlink_ey_qr_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & tests/carlink_ey_qr_test.exe
    exit $LASTEXITCODE
} else {
    Write-Output 'SKIP: gcc is unavailable'
}
```

Expected: test executable exits `0`, or the command reports `SKIP: gcc is unavailable`.

- [ ] **Step 6: Check the focused diff**

Run:

```powershell
git diff --check -- app/carlink/common/carlink_bt_wifi.c
git diff -- app/carlink/common/carlink_bt_wifi.c
```

Expected: no whitespace errors; the new change is limited to `CARLINK_EY_QR_DIRECT` changing from `1` to `0` while pre-existing user changes remain intact.

- [ ] **Step 7: Commit only the focused source change**

```powershell
git add -- app/carlink/common/carlink_bt_wifi.c
git commit -m "fix: restore EY Bluetooth connection flow" -- app/carlink/common/carlink_bt_wifi.c
```

Expected: one commit containing the connection-mode default change together with any already-present modifications in that same tracked file. Because the file is already modified by the user, inspect the staged diff before committing; if unrelated edits cannot be separated safely, leave the source change uncommitted and report that condition.
