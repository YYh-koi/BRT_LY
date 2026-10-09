# 会话记忆：UART1 引脚配置 + romfile Data Abort 修复

> 日期：2026-09-01
> 工程：`amt630hv100-freertos_bc4661`（AMT630HV100 平台，FreeRTOS + AWTK，IAR 8.30.1）
> 目的：跨会话回忆，记录已完成改动、根因结论、关键地址与遗留问题。

## 一、本会话做完的两件事

### 1. 蓝牙串口从 UART0 改为 UART1，指定引脚映射
- 文件：[board.h](../ArkmicroFiles/libboard-amt630hv100/include/board.h#L215)
  - 启用 `#define UART1_FLOW_CTL`（硬件流控 CTS/RTS）
- 文件：[pinctrl.c](../ArkmicroFiles/libboard-amt630hv100/source/pinctrl.c#L169-L175)
  - UART1 引脚组：
    - 开流控（4 脚）：`RX=41, TX=40, CTS=46, RTS=47`
    - 关流控（2 脚）：`RX=41, TX=40`
- 引脚映射已对照核心板原理图 `ly a630 v1核心板.pdf` 验证一致。
- 打印/调试串口仍为 **UART0（IO38/IO39，115200）**，与本次改动无关。

### 2. Data Abort 根因定位 + romfile 防御性加固（本次核心）
- 崩溃现场：断点停在 `board_lowlevel.c` 的 `Abort_C_Handler` 的 `while(1);`，串口无输出。
- 经在 `Abort_C_Handler` 中把关键寄存器存入全局变量（见 `board_lowlevel.c` 的 `g_abort_*` 系列），读得：
  - `DFAR = 0x00100030`（访问未映射地址）
  - `DFSR = 0x05`（Translation fault - section）
  - `LR = 0x200C583E`（故障指令在 `strncmp`，`0x200C5836`）
  - `caller = 0x2010108c`（调用者是 `RomFileDirExist`）
  - `r0 = 0x00100030`（= DFAR，坏指针 `romheader->files[i].name`）；`r1` 为合法 RAM（AWTK 传入 name）；`r2 = 0x6`（=strlen）
- 根因链：AWTK `fs_os_dir_exist()` → `RomFileDirExist()` → 解引用未初始化的 `romheader`（NULL 或非法指针），`strncmp` 访问 `0x00100030` 触发 Data Abort。
- 更深层根因：`ReadRomFile()` 在 update file 中**未找到 ROMA 段**时直接 `return -1`，`romheader` 保持 `NULL`/悬空，但 AWTK 仍提前调用目录检查；旧函数无判空直接遍历 `romheader->filenum`。

## 二、本次代码改动（已落盘）

### romfile.c（[完整文件](../ArkmicroFiles/libboard-amt630hv100/source/romfile.c)）
1. 新增静态助手 `romheader_valid()`：判空 + RAM 地址范围校验（`< 0x20000000` 视为非法）；并带一次性诊断打印 `[ROMFILE DIAG]`（含当前指针值 + `__get_LR()` 调用者）。
   - 注：IAR 无 `__return_address`，必须用 `__get_LR()`。
2. `ReadRomFile()`：
   - magic 校验失败时 `romheader = NULL`（原来悬空）。
   - 新增 `filenum` 合理性校验（`==0 或 > 0x1000` 视为非法，置 NULL 返回 -1）。
3. `RomFileDirExist()`：开头 `if (!romheader_valid()) return 0;`。
4. `RomFileExist()`：开头 `if (!romheader_valid()) return 0;`。
5. `RomFileOpen()`：开头 `if (!romheader_valid()) return NULL;`（不触碰 mutex，避免 mutex 也未初始化时崩溃）。

### board_lowlevel.c（[文件](../ArkmicroFiles/libboard-amt630hv100/source/board_lowlevel.c)）
- `Abort_C_Handler` 增加崩溃现场保存：`g_abort_v1/v2/lr/dfsr/caller/sp/spsr/r0-r3/stack[]` 全局 volatile 变量。
- 前置要点：`r0-r3` 必须在函数最顶部第一条 asm 捕获；去掉局部变量 `=0` 初始化，否则 IAR 优化用 r0-r3 做临时寄存器覆写崩溃现场。
- 加了 `if (dfsr > 21) dfsr = 0;` 防 `abort_status` 数组越界。

## 三、关键符号/地址（来自 proj/AWTK/List/amt630hv100.map）
- `romheader` 静态变量：`0x20f46ab4`（4 字节）
- `romfileMutex`：`0x20f46ab8`；`romoffset`：`0x20f46abc`
- `romheader_valid`：`0x20100a7c`
- `ReadRomFile`：`0x20100ab0`；`RomFileOpen`：`0x20100c00`；`RomFileExist`：`0x20101008`；`RomFileDirExist`：`0x20101074`（0x98）
- 系统 RAM 从 `0x20000000` 起；堆 `ucHeap` 位于 `0x20f604e4`，大小 `0x2200000`（约 35MB）
- `romheader` 的 `.bss` 邻居为 lwIP 网络栈变量（raw.o/pxp.o/netif.o/mfcapi.o/mmcsd_core.o），无明显数组越界邻居。

## 四、验证结果（debug/log.txt）
- 修复后 R ebuild 正常启动，`Memory free: 26804992 bytes.` 每 10s 刷新，无 Data Abort。
- log 中出现：`[ROMFILE DIAG] romheader == NULL, call from 0x2010108c`（=`RomFileDirExist`），证明根因命中且防护生效。
- 同时暴露遗留问题：`Not found valid assets folder!` / `!!!Asset [name=home_page/home_page type=ui] not exist!!!`（见下）。

## 五、遗留问题（未处理，下次可接）
- **AWTK 资源缺失**：update file 中没有 ROMA 段（`Warning! Not found romfile in update file.`），导致 AWTK 启动找不到 `home_page` 等资源。
  - 属于 ROM 打包 / 资源路径问题，与本次 Data Abort 无关。
  - 用户选择先收尾，资源问题稍后单独处理。

## 六、调试经验沉淀（下次直接复用）
- IAR 优化会把局部变量 `<unavailable>`，必须用 `volatile` 全局变量保存断点现场。
- 捕获崩溃点通用寄存器要在函数第一条 asm 语句做，且局部变量不要 `=0` 初始化。
- `strncmp(s1,s2,n)` 参数映射：`r0=s1, r1=s2, r2=n`（AAPCS）。
- `.map` 地址含撇号分隔（如 `0x2010'108c`），Grep 时需用撇号格式。
- 本平台 RAM 基址 `0x20000000`，凡低于此的指针可判为非法（防御性校验可行）。
- 引脚号用 GPIO 编号（40/41/46/47）映射到 `amt630h_pin_map`，与原理图核对。
