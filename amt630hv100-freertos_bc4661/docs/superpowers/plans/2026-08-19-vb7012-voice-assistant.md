# VB7012 Voice Assistant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the FeiBao VB7018 UART voice-event demo into the current AWTK firmware as directly compiled, consistently renamed VB7012 source.

**Architecture:** A core FreeRTOS/UART module parses frames and dispatches one registered callback. An AWTK adapter moves callback events through a queue to a UI timer, while board and startup guards reserve UART3 by suppressing the CLI when the feature is enabled.

**Tech Stack:** C, FreeRTOS tasks/queues, AMT630HV100 UART driver, AWTK, IAR Embedded Workbench XML projects, PowerShell verification.

---

## File map

- Create `app/vb7012_voice_assistant/vb7012_voice_assistant.c`: UART lifecycle, receive task, parser, command mapping, callback dispatch.
- Create `app/vb7012_voice_assistant/vb7012_voice_assistant.h`: event enum and public lifecycle/callback API.
- Create `app/vb7012_voice_assistant/vb7012_voice_assistant_awtk.c`: AWTK queue, timer, label, and startup integration.
- Create `app/vb7012_voice_assistant/vb7012_voice_assistant_awtk.h`: AWTK initialization API.
- Create `app/vb7012_voice_assistant/vb7012_voice_assistant_demo.c`: console-only event-name demo callback.
- Create `app/vb7012_voice_assistant/vb7012_voice_assistant_demo.h`: console demo entry point.
- Modify `ArkmicroFiles/libboard-amt630hv100/include/board.h`: default-on feature macro.
- Modify `app/main_awtk.c`: do not start the UART3 CLI when the feature owns UART3.
- Modify `lib/awtk/awtk-examples/Meter.v2-Demo/src/window_main.c`: initialize the AWTK adapter on the home page.
- Modify `proj/amt630hv100.ewp`: add direct source references, include paths, and LVGL/VG exclusions.

Implementation files are intentionally left uncommitted because `board.h`, `main_awtk.c`, and `amt630hv100.ewp` already contain user changes. Only the plan document is committed independently.

### Task 1: Establish the structural red check

**Files:**
- Test: repository structure and `proj/amt630hv100.ewp`

- [ ] **Step 1: Run the pre-implementation file check**

```powershell
$root = 'D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat'
$files = @(
  'vb7012_voice_assistant.c',
  'vb7012_voice_assistant.h',
  'vb7012_voice_assistant_awtk.c',
  'vb7012_voice_assistant_awtk.h',
  'vb7012_voice_assistant_demo.c',
  'vb7012_voice_assistant_demo.h'
)
$missing = $files | Where-Object { -not (Test-Path "$root\app\vb7012_voice_assistant\$_") }
if ($missing.Count -eq 0) { throw 'Expected structural check to fail before implementation' }
$missing
```

Expected: all six target filenames are printed, proving the check is red for the missing feature.

### Task 2: Add the directly compiled VB7012 source module

**Files:**
- Create: `app/vb7012_voice_assistant/vb7012_voice_assistant.c`
- Create: `app/vb7012_voice_assistant/vb7012_voice_assistant.h`
- Create: `app/vb7012_voice_assistant/vb7012_voice_assistant_awtk.c`
- Create: `app/vb7012_voice_assistant/vb7012_voice_assistant_awtk.h`
- Create: `app/vb7012_voice_assistant/vb7012_voice_assistant_demo.c`
- Create: `app/vb7012_voice_assistant/vb7012_voice_assistant_demo.h`

- [ ] **Step 1: Add the six source files from the approved reference**

Use the corresponding files under:

```text
D:\Project\Outside\FeiBao\amt630hV100-sdk-beta-bc4661\amt630hv100-freertos\app\vb7018_voice
```

Apply these exact identifier replacements to the copied source content:

```text
vb7018_voice                 -> vb7012_voice_assistant
VB7018_VOICE                 -> VB7012_VOICE_ASSISTANT
VB7018                       -> VB7012
vb7018                       -> vb7012
"VB7018:                    -> "VB7012 Voice Assistant:
[VB7018]                     -> [VB7012_VOICE_ASSISTANT]
[VB7018_AWTK]                -> [VB7012_VOICE_ASSISTANT_AWTK]
```

The public header must expose these exact signatures:

```c
int vb7012_voice_assistant_init(void);
int vb7012_voice_assistant_register_callback(vb7012_voice_assistant_callback_t callback);
int vb7012_voice_assistant_start(void);
void vb7012_voice_assistant_stop(void);
```

Keep UART3, 115200 baud, 32-byte reads, the five-state parser, checksum rule, 26 command mappings, queue capacity 8, and 100 ms AWTK timer unchanged.

- [ ] **Step 2: Run the file check to green**

```powershell
$root = 'D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat'
$files = @(
  'vb7012_voice_assistant.c',
  'vb7012_voice_assistant.h',
  'vb7012_voice_assistant_awtk.c',
  'vb7012_voice_assistant_awtk.h',
  'vb7012_voice_assistant_demo.c',
  'vb7012_voice_assistant_demo.h'
)
$missing = $files | Where-Object { -not (Test-Path "$root\app\vb7012_voice_assistant\$_") }
if ($missing.Count -ne 0) { throw "Missing: $($missing -join ', ')" }
```

Expected: exit code 0 and no output.

- [ ] **Step 3: Check renamed source consistency**

```powershell
rg -n 'vb7018|VB7018' app/vb7012_voice_assistant
```

Expected: exit code 1 with no matches.

### Task 3: Reserve UART3 and initialize the AWTK adapter

**Files:**
- Modify: `ArkmicroFiles/libboard-amt630hv100/include/board.h:213`
- Modify: `app/main_awtk.c:930`
- Modify: `lib/awtk/awtk-examples/Meter.v2-Demo/src/window_main.c:1`
- Modify: `lib/awtk/awtk-examples/Meter.v2-Demo/src/window_main.c:220`

- [ ] **Step 1: Add the feature switch beside UART configuration**

```c
/***** VB7012 voice assistant configuration *****/
#define VB7012_VOICE_ASSISTANT_ENABLE
/*************************************************/
```

Place it after the UART configuration block without changing the existing commented `UART1_FLOW_CTL` line or Carlink settings.

- [ ] **Step 2: Guard UART3 CLI startup in `main_awtk`**

```c
#if !defined(VB7012_VOICE_ASSISTANT_ENABLE) || (UART_MCU_PORT != 3)
    vUARTCommandConsoleStart((configMINIMAL_STACK_SIZE * 3), tskIDLE_PRIORITY);
    vRegisterSampleCLICommands();
#endif
```

Wrap only the existing CLI start and command-registration statements.

- [ ] **Step 3: Include and initialize the AWTK adapter**

At the top of `window_main.c`, add:

```c
#include "board.h"
#ifdef VB7012_VOICE_ASSISTANT_ENABLE
#include "vb7012_voice_assistant_awtk.h"
#endif
```

After existing home-page timers are installed, add:

```c
#ifdef VB7012_VOICE_ASSISTANT_ENABLE
    vb7012_voice_assistant_awtk_init(win);
#endif
```

- [ ] **Step 4: Verify the integration markers**

```powershell
rg -n 'VB7012_VOICE_ASSISTANT_ENABLE|vb7012_voice_assistant_awtk_init' `
  ArkmicroFiles/libboard-amt630hv100/include/board.h `
  app/main_awtk.c `
  lib/awtk/awtk-examples/Meter.v2-Demo/src/window_main.c
```

Expected: the macro definition, CLI guard, include guard, and initialization call are all listed.

### Task 4: Add direct source compilation to the IAR project

**Files:**
- Modify: `proj/amt630hv100.ewp`

- [ ] **Step 1: Add the include directory to all four AWTK configurations**

Add this state once to each AWTK-family `CCIncludePath2` option:

```xml
<state>$PROJ_DIR$\..\app\vb7012_voice_assistant</state>
```

The affected configurations are `AWTK`, `AWTK_BW3583`, `AWTK_BW2232`, and `AWTK_BW8201`. Do not add it to `LVGL` or `VG_ONLLY`.

- [ ] **Step 2: Add a directly compiled project group**

Insert this group under the application groups:

```xml
<group>
    <name>vb7012_voice_assistant</name>
    <excluded>
        <configuration>LVGL</configuration>
        <configuration>VG_ONLLY</configuration>
    </excluded>
    <file><name>$PROJ_DIR$\..\app\vb7012_voice_assistant\vb7012_voice_assistant.c</name></file>
    <file><name>$PROJ_DIR$\..\app\vb7012_voice_assistant\vb7012_voice_assistant.h</name></file>
    <file><name>$PROJ_DIR$\..\app\vb7012_voice_assistant\vb7012_voice_assistant_awtk.c</name></file>
    <file><name>$PROJ_DIR$\..\app\vb7012_voice_assistant\vb7012_voice_assistant_awtk.h</name></file>
    <file><name>$PROJ_DIR$\..\app\vb7012_voice_assistant\vb7012_voice_assistant_demo.c</name></file>
    <file><name>$PROJ_DIR$\..\app\vb7012_voice_assistant\vb7012_voice_assistant_demo.h</name></file>
</group>
```

- [ ] **Step 3: Validate the XML and link model**

```powershell
[xml](Get-Content -Raw proj/amt630hv100.ewp) | Out-Null
if (rg -n 'vb7012_voice_assistant\.a|lib\\vb7012_voice_assistant' proj/amt630hv100.ewp) {
  throw 'Voice assistant library link found'
}
```

Expected: exit code 0; no voice-assistant library path is found.

### Task 5: Verify the complete port

**Files:**
- Verify: all changed files above

- [ ] **Step 1: Run whitespace and scope checks**

```powershell
git diff --check
git status --short
git diff -- app/vb7012_voice_assistant ArkmicroFiles/libboard-amt630hv100/include/board.h app/main_awtk.c lib/awtk/awtk-examples/Meter.v2-Demo/src/window_main.c proj/amt630hv100.ewp
```

Expected: `git diff --check` exits 0; the scoped diff contains only the approved port plus pre-existing user hunks in overlapping files.

- [ ] **Step 2: Verify naming, direct compilation, and configuration scope**

```powershell
$root = 'D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat'
if (rg -n 'vb7018_voice|VB7018_VOICE' "$root\app\vb7012_voice_assistant" "$root\proj\amt630hv100.ewp") {
  throw 'Legacy VB7018 identifier found in target integration'
}
if (-not (rg -n 'app\\vb7012_voice_assistant\\vb7012_voice_assistant\.c' "$root\proj\amt630hv100.ewp")) {
  throw 'Direct core source reference missing'
}
if (Get-ChildItem "$root\lib" -Recurse -Filter 'vb7012_voice_assistant.a' -ErrorAction SilentlyContinue) {
  throw 'Forbidden voice assistant library found'
}
```

Expected: exit code 0.

- [ ] **Step 3: Build the selected AWTK configuration**

```powershell
& 'D:\IDE\IAR\common\bin\IarBuild.exe' `
  'D:\Project\Outside\Letter\amt630hv100-freertos_bc4661_for_clcat\proj\amt630hv100.ewp' `
  -build AWTK -log all
```

Expected: IAR exits 0 with zero compile or link errors.

- [ ] **Step 4: Record hardware-only checks for handoff**

```text
1. UART3 CLI is absent when VB7012_VOICE_ASSISTANT_ENABLE is defined.
2. 55 AA 01 00 01 displays the WAKEUP event.
3. A bad checksum frame is ignored and the next valid frame parses.
4. An unknown command is logged without a UI event.
5. More than eight rapid events preserve recent UI updates without blocking.
```

Expected: report these as pending unless physical hardware is available and exercised.
