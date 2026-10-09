#ifdef SUPPORT_MS_EXTENSIONS
#pragma bss_seg(".app_at_dut.data.bss")
#pragma data_seg(".app_at_dut.data")
#pragma const_seg(".app_at_dut.text.const")
#pragma code_seg(".app_at_dut.text")
#endif

#include "system/includes.h"
#include "uart.h"
#include "crc.h"
#include "app_config.h"
#include "app_main.h"
#include "init.h"
#include "bt_common.h"
#include "user_cfg.h"
#include "bt_event_func.h"
#include "btctrler_task.h"
#include "classic/hci_lmp.h"
#include "debug.h"

#undef LOG_TAG
#define LOG_TAG "[AT_DUT]"

//=================================================================================//
//                            产测串口配置(UART2)                                  //
//=================================================================================//
#define AT_DUT_UART_NUM        2               // UART2
#define AT_DUT_UART_TX_PIN     IO_PORTB_04     // PB4
#define AT_DUT_UART_RX_PIN     IO_PORTB_05     // PB5
#define AT_DUT_UART_BAUD       115200

// 软件版本号
#define AT_DUT_SW_VERSION      "HZ_V1.0.0"

// 产测 MAC 存储 ID(VM 区自定义 ID，30 在 user_cfg_id.h 中未被占用)
// 说明：CFG_BT_MAC_ADDR 注册在 btif_table(BTIF 区域)，应用内写入返回 -247(VM_ERR_INIT)，
//       故产测 MAC 改存 VM 区，开机时恢复覆盖 BTIF 值。
#define AT_DUT_MAC_CFG_ID      30

//=================================================================================//
//                                   内部变量                                       //
//=================================================================================//
static int at_dut_uart = -1;
static u8 at_dut_rx_buf[64];
static u8 *at_dut_rx_dma_buf;
static u8 at_dut_tx_buf[64];                 // 发送缓冲(static，DMA 安全)
static u8 at_dut_line_buf[64];               // 行缓冲
static u16 at_dut_line_len;
static u32 at_dut_last_rx_time;              // 最近一次收到数据的时间

//=================================================================================//
//                                TRIM 值(频偏)                                     //
//  存储格式与蓝牙库一致：CFG_BT_FRE_OFFSET(110) 存 6 字节：crc(2)+data(4)，        //
//  data[0..1] 为 s16 频偏值。蓝牙库启动时 syscfg_read(110) 校验 CRC 后应用。      //
//=================================================================================//
struct at_dut_trim_item {
    u16 crc;
    u8  data[4];
} _GNU_PACKED_;

/* 读 TRIM：有效返回频偏值，无效(未初始化/CRC错)返回 0 */
static s16 at_dut_trim_read(void)
{
    struct at_dut_trim_item item;

    if (syscfg_read(CFG_BT_FRE_OFFSET, (u8 *)&item, sizeof(item)) == sizeof(item)) {
        if (item.crc == CRC16((u8 *)&item.data, 4)) {
            s16 v;

            memcpy(&v, &item.data[0], 2);
            return v;
        }
    }
    return 0;
}

/* 写 TRIM：组装 crc(2)+data(4) 结构写入 CFG_BT_FRE_OFFSET */
static int at_dut_trim_write(s16 trim)
{
    struct at_dut_trim_item item;

    memset(&item, 0, sizeof(item));
    memcpy(&item.data[0], &trim, 2);
    item.crc = CRC16((u8 *)&item.data, 4);

    if (syscfg_write(CFG_BT_FRE_OFFSET, (u8 *)&item, sizeof(item)) != sizeof(item)) {
        return -1;
    }
    return 0;
}

//=================================================================================//
//                                   回复发送                                       //
//=================================================================================//
static void at_dut_send(const char *s)
{
    if (at_dut_uart >= 0) {
        u16 len = strlen(s);

        if (len > sizeof(at_dut_tx_buf)) {
            len = sizeof(at_dut_tx_buf);
        }
        /* 拷贝到 static RAM 缓冲再发：栈/const 段数据在 DMA 发送完成前可能失效 */
        memcpy(at_dut_tx_buf, s, len);
        uart_send_blocking(at_dut_uart, at_dut_tx_buf, len, 200);
    }
}

#define AT_DUT_REPLY_OK()      at_dut_send("\r\nOK\r\n")
#define AT_DUT_REPLY_ERROR()   at_dut_send("\r\nERROR\r\n")

//=================================================================================//
//                                  指令解析                                       //
//=================================================================================//
/* 十六进制字符转数值，非法字符返回 -1 */
static int at_dut_hex_val(u8 c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* 解析纯十进制数字，成功返回 0，*val 为结果 */
static int at_dut_parse_dec(u8 *s, u16 len, int *val)
{
    int v = 0;
    u16 i;

    if (len == 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    *val = v;
    return 0;
}

/* 进入 BLE DTM 测试模式：2402MHz TX 测试包 */
static void at_dut_cmd_dtm(void)
{
    static void *dtm_priv = NULL;
    struct ble_dut_tx_param_t param = {
        .ch_index    = 0,       /* 2402MHz */
        .payload_len = 37,
        .payload_type = 0,      /* PRBS9 */
        .phy_type    = 1,       /* LE 1M */
    };

    if (!__ble_dut_ops) {
        AT_DUT_REPLY_ERROR();
        return;
    }
    if (!dtm_priv) {
        dtm_priv = __ble_dut_ops->init();
        if (!dtm_priv) {
            AT_DUT_REPLY_ERROR();
            return;
        }
    }
    if (__ble_dut_ops->ioctrl(BLE_DUT_SET_TX_MODE, &param) != 0) {
        AT_DUT_REPLY_ERROR();
        return;
    }
    AT_DUT_REPLY_OK();
}

static void at_dut_cmd_handle(u8 *cmd, u16 len)
{
    /* AT+GVER? : 查询软件版本，回复 +GVER=XXX (AT+GVER? 共 8 个字符) */
    if (len == 8 && memcmp(cmd, "AT+GVER?", 8) == 0) {
        char reply[32];

        sprintf(reply, "+GVER=%s\r\n", AT_DUT_SW_VERSION);
        at_dut_send(reply);
        return;
    }

    /* AT+MAC? : 查询 MAC 地址，回复 +MAC=XX:XX:XX:XX:XX:XX (AT+MAC? 共 7 个字符) */
    if (len == 7 && memcmp(cmd, "AT+MAC?", 7) == 0) {
        const u8 *mac = bt_get_mac_addr();
        char reply[32];

        sprintf(reply, "+MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        at_dut_send(reply);
        return;
    }

    /* AT+TRIM? : 读 TRIM 值(频偏)，回复 +TRIM=X (AT+TRIM? 共 8 个字符) */
    if (len == 8 && memcmp(cmd, "AT+TRIM?", 8) == 0) {
        char reply[16];
        s16 trim = at_dut_trim_read();

        sprintf(reply, "+TRIM=%d\r\n", trim);
        at_dut_send(reply);
        return;
    }

    /* AT+TRIM=+1 / AT+TRIM=-1 : TRIM 值微调 (各 10 个字符，符号位在 cmd[8]) */
    if (len == 10 && memcmp(cmd, "AT+TRIM=", 8) == 0 &&
        (cmd[8] == '+' || cmd[8] == '-') && cmd[9] == '1') {
        s16 trim = at_dut_trim_read();

        trim += (cmd[8] == '+') ? 1 : -1;
        if (at_dut_trim_write(trim) != 0) {
            AT_DUT_REPLY_ERROR();
            return;
        }
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+FREQ=XXXX : BDR 定频(2402/2441/2480MHz)，调用 bt_fix_fre_api(信道号=频率-2402) */
    if (len > 8 && memcmp(cmd, "AT+FREQ=", 8) == 0) {
        int freq;

        if (at_dut_parse_dec(cmd + 8, len - 8, &freq) != 0) {
            AT_DUT_REPLY_ERROR();
            return;
        }
        if (freq < 2402 || freq > 2480) {
            AT_DUT_REPLY_ERROR();
            return;
        }
        bt_fix_fre_api(freq - 2402);    /* 0=2402MHz, 39=2441MHz, 78=2480MHz */
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+DTM=1 : 进入 DTM 测试模式(8 个字符)，BLE 2402MHz TX */
    if (len == 8 && memcmp(cmd, "AT+DTM=1", 8) == 0) {
        at_dut_cmd_dtm();
        return;
    }

    /* AT+DUT=1 : 进入 DUT 测试模式(8 个字符)，调用 bt_bredr_enter_dut_mode(1,1) */
    if (len == 8 && memcmp(cmd, "AT+DUT=1", 8) == 0) {
        bt_bredr_enter_dut_mode(1, 1);
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+OTA=1 : 进入升级(代替 K2/K4 按键)，先回 OK 再复位进 USB 升级模式 */
    if (len == 8 && memcmp(cmd, "AT+OTA=1", 8) == 0) {
        extern void go_mask_usb_updata(void);

        /* 与按键流程一致：要求 USB 5V 在线，否则进不了升级模式 */
        // if (!get_ldo5v_online_hw()) {
        //     AT_DUT_REPLY_ERROR();
        //     return;
        // }
        AT_DUT_REPLY_OK();
        if (at_dut_uart >= 0) {
            uart_wait_tx_idle(at_dut_uart, 200);
        }
        go_mask_usb_updata();   /* 设置升级标志并复位，不复返 */
        return;
    }

    /* AT+MAC=XXXXXXXXXXXX : 写 MAC 地址(12位十六进制)，回复 +MAC=XX:XX:XX:XX:XX:XX
     * (AT+MAC= 共 7 个字符 + 12 个 hex = 19 字符) */
    if (len == 19 && memcmp(cmd, "AT+MAC=", 7) == 0) {
        u8 mac[6];
        char reply[32];
        int i, h, l;

        for (i = 0; i < 6; i++) {
            h = at_dut_hex_val(cmd[7 + i * 2]);
            l = at_dut_hex_val(cmd[7 + i * 2 + 1]);
            if (h < 0 || l < 0) {
                AT_DUT_REPLY_ERROR();
                return;
            }
            mac[i] = (u8)((h << 4) | l);
        }

        /* 存到 VM 区自定义 ID(可写)，替换 BTIF 写入 */
        {
            int ret = syscfg_write(AT_DUT_MAC_CFG_ID, mac, 6);

            if (ret != 6) {
                char dbg[32];

                sprintf(dbg, "[ERR]vm write=%d\r\n", ret);
                at_dut_send(dbg);
                AT_DUT_REPLY_ERROR();
                return;
            }
        }
        /* 更新内存中的当前地址并通知蓝牙控制器(立即生效) */
        bt_update_mac_addr(mac);
        lmp_hci_write_local_address(mac);

        sprintf(reply, "+MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        at_dut_send(reply);
        return;
    }

    AT_DUT_REPLY_ERROR();
}

/* 收到数据，逐字节入行缓冲，遇 \r 或 \n 触发一行解析 */
static void at_dut_rx_process(u8 *data, u16 len)
{
    u16 i;

    for (i = 0; i < len; i++) {
        if (data[i] == '\r' || data[i] == '\n') {
            if (at_dut_line_len > 0) {
                at_dut_cmd_handle(at_dut_line_buf, at_dut_line_len);
                at_dut_line_len = 0;
            }
        } else if (at_dut_line_len < (sizeof(at_dut_line_buf) - 1)) {
            at_dut_line_buf[at_dut_line_len++] = data[i];
        } else {
            /* 行过长，丢弃重新开始 */
            at_dut_line_len = 0;
        }
    }
}

//=================================================================================//
//                              UART 初始化与任务                                   //
//=================================================================================//
/* DMA 溢出保护 */
static void at_dut_uart_irq(uart_dev uart_num, enum uart_event event)
{
    if (event & UART_EVENT_RX_FIFO_OVF) {
        uart_dma_rx_reset(uart_num);
    }
}

static int at_dut_uart_init(void)
{
    const struct uart_config config = {
        .baud_rate = AT_DUT_UART_BAUD,
        .tx_pin = AT_DUT_UART_TX_PIN,
        .rx_pin = AT_DUT_UART_RX_PIN,
        .parity = UART_PARITY_DISABLE,
        .tx_wait_mutex = 0,
    };

    at_dut_uart = uart_init(AT_DUT_UART_NUM, &config);
    if (at_dut_uart < 0) {
        printf("[AT_DUT] uart(%d) init fail: %d\n", AT_DUT_UART_NUM, at_dut_uart);
        return -1;
    }
    printf("[AT_DUT] uart(%d) init ok, tx=PB4 rx=PB5 baud=%d\n", at_dut_uart, AT_DUT_UART_BAUD);

    at_dut_rx_dma_buf = dma_malloc(256);
    if (!at_dut_rx_dma_buf) {
        printf("[AT_DUT] rx dma buf alloc fail\n");
        return -1;
    }

    struct uart_dma_config dma = {
        .rx_timeout_thresh = 3 * 10000000 / AT_DUT_UART_BAUD,   // 3字节时间
        .frame_size = 8,
        .event_mask = UART_EVENT_RX_DATA | UART_EVENT_RX_TIMEOUT |
                      UART_EVENT_RX_FIFO_OVF | UART_EVENT_TX_DONE,
        .irq_priority = 3,
        .irq_callback = at_dut_uart_irq,
        .rx_cbuffer = at_dut_rx_dma_buf,
        .rx_cbuffer_size = 256,
    };

    if (uart_dma_init(at_dut_uart, &dma) < 0) {
        printf("[AT_DUT] uart(%d) dma init fail\n", at_dut_uart);
        return -1;
    }
    printf("[AT_DUT] uart(%d) dma init ok\n", at_dut_uart);
    return 0;
}

/* 开机恢复产测 MAC：从 VM 自定义 ID 读，覆盖 BTIF 值，让蓝牙广播用产测 MAC */
static void at_dut_mac_restore(void)
{
    u8 mac[6];
    u8 ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    if (syscfg_read(AT_DUT_MAC_CFG_ID, mac, 6) == 6 &&
        memcmp(mac, ff, 6) != 0) {
        bt_update_mac_addr(mac);
        lmp_hci_write_local_address(mac);
        printf("[AT_DUT] mac restore:");
        put_buf(mac, 6);
    }
}

static void at_dut_task(void *priv)
{
    at_dut_mac_restore();
    at_dut_uart_init();

    while (1) {
        u16 len = uart_recv_bytes(at_dut_uart, at_dut_rx_buf, sizeof(at_dut_rx_buf));
        if (len > 0) {
            at_dut_rx_process(at_dut_rx_buf, len);
            at_dut_last_rx_time = jiffies;
        } else {
            /* 兼容指令不带 \r\n 的情况：停止接收 100ms 后把攒下的数据当作一行处理 */
            if (at_dut_line_len > 0 &&
                jiffies_offset_to_msec(at_dut_last_rx_time, jiffies) > 100) {
                at_dut_cmd_handle(at_dut_line_buf, at_dut_line_len);
                at_dut_line_len = 0;
            }
        }
        os_time_dly(1);
    }
}

//=================================================================================//
//                                    注册                                          //
//=================================================================================//
static int app_at_dut_init(void)
{
    int err = os_task_create(at_dut_task, NULL, 2, 1024, 32, "at_dut");
    if (err != OS_NO_ERR) {
        log_error("at dut task create fail: %x\n", err);
        return err;
    }
    return 0;
}
late_initcall(app_at_dut_init);
