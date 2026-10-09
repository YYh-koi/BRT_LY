# Repository Guidelines

## Project Structure & Module Organization
This is an embedded FreeRTOS firmware tree for the AMT630HV100 platform. Top-level startup and RTOS glue live in `main.c`, `FreeRTOSConfig.h`, `FreeRTOS_tick_config.c`, `os_adapt.*`, and `cstartup_with_FreeRTOS_vectors.s`. Application code and UI demos are under `app/`, including AWTK/LVGL entry points and product-specific assets such as `app/*/image` and font folders. `FreeRTOS/` and `FreeRTOS-Plus/` contain kernel and middleware sources. Third-party and platform libraries live in `lib/` (`awtk`, `LittlevGL`, `lwip`, `wlan`, `wolfssl`, etc.). IAR project files and linker settings are in `proj/`; Wi-Fi variant projects are in `proj/wifi_project/`. Treat `proj/*/Obj`, `proj/*/List`, `.dep`, `.pbi`, `.map`, and similar build outputs as generated artifacts.

## Build, Test, and Development Commands
Open `proj/amt630hv100.eww` in IAR Embedded Workbench for the main workspace. Useful CLI examples, when `iarbuild` is on `PATH`:

```powershell
iarbuild proj\amt630hv100.eww -batch amt630hv100-awtk
iarbuild proj\amt630hv100.eww -batch amt630hv100-lvgl-DDR32X16
iarbuild proj\wifi_project\wifi_atbm6165.ewp -build library
```

Use the AWTK batch for `app/main_awtk.c` changes and the LVGL batch for `app/main_lvgl.c` changes. Rebuild the matching Wi-Fi project after changes under `lib/wlan`, `atbm_rtos_wifi6/`, or `proj/wifi_project/`.

## Coding Style & Naming Conventions
Follow the existing C style: 4-space indentation, K&R braces, lowercase filenames, and `snake_case` functions and variables. Keep compile-time feature switches as uppercase macros (`WIFI_SUPPORT`, `OTA_UPDATE_SUPPORT`). Prefer local headers beside their implementation and avoid broad changes to vendored libraries unless required.

## Testing Guidelines
No standalone automated test runner is present in this checkout. Validate by building the affected IAR configuration, flashing to target hardware, and smoke-testing the changed path. Use `Blinky_Demo/main_blinky.c` for board bring-up checks and `demo/Common` for FreeRTOS demo references. Document hardware, build configuration, and observed result in the change notes.

## Commit & Pull Request Guidelines
This checkout does not expose git history, so use concise imperative commits such as `fix wifi reconnect timeout` or `add lvgl startup guard`. Pull requests should describe the target configuration, touched modules, build result, hardware test result, and any asset or generated-file changes. Link related issues when available.

## Security & Configuration Tips
Do not commit device credentials, customer network settings, private keys, or local IAR paths. Keep board-specific configuration in existing config headers or project settings, and call out any required flashing or provisioning steps.

## Project memory
项目技术上下文见 `project_memory.md`，新对话开始时请先读取该文件以恢复上下文。

## Agent skills

### Issue tracker

Issues and PRDs use local Markdown files under `.scratch/`; external pull requests are not a triage surface. See `docs/agents/issue-tracker.md`.

### Triage labels

Use the default labels `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Use the single-context layout with `CONTEXT.md` at the repository root and architectural decisions under `docs/adr/`. See `docs/agents/domain.md`.
