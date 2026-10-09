#ifdef SUPPORT_MS_EXTENSIONS
#pragma bss_seg(".app_at_dut.data.bss")
#pragma data_seg(".app_at_dut.data")
#pragma const_seg(".app_at_dut.text.const")
#pragma code_seg(".app_at_dut.text")
#endif

#include "system/includes.h"
#include "crc.h"
#include "app_config.h"
#include "app_at_dut.h"
#include "app_main.h"
#include "app_msg.h"
#include "init.h"
#include "bt_common.h"
#include "user_cfg.h"
#include "bt_event_func.h"
#include "btctrler_task.h"
#include "btstack/avctp_user.h"
#include "classic/hci_lmp.h"
#include "debug.h"
#include "smart_voice.h"
#include "audio_dac.h"
#include "wanson/uart/voice_uart.h"
#include "wanson/hz_asr/asr.h"
#include "wanson/hz_asr/readme_play.h"

#undef LOG_TAG
#define LOG_TAG "[AT_DUT]"

#if AT_DUT_DEBUG_LOG
#define AT_DBG(fmt, ...) do { printf("[AT-DBG] " fmt, ##__VA_ARGS__); } while (0)
#define AT_DBG_HEX(tag, buf, len) do { printf("[AT-DBG] %s: ", tag); put_buf(buf, len); printf("\n"); } while (0)
#else
#define AT_DBG(fmt, ...)
#define AT_DBG_HEX(tag, buf, len)
#endif

#if TCFG_AT_DUT_ENABLE

//=================================================================================//
//                            产测串口配置(PB4/PB5 共用)                            //
//=================================================================================//

// 软件版本号（照搬 V3 产测固件）
#define AT_DUT_SW_VERSION      "HZ_V1.0.0"

// 产测 MAC 存储 ID（VM 区自定义 ID；30 在 user_cfg_id.h 中未被占用）
// 说明：CFG_BT_MAC_ADDR 注册在 btif_table（BTIF 区域），应用内写入返回 -247(VM_ERR_INIT)，
//       故产测 MAC 改存 VM 区，开机时恢复覆盖 BTIF 值。
#define AT_DUT_MAC_CFG_ID      30

#define AT_DUT_CORE            "at_dut"
#define at_dut_post_msg(num, ...) os_taskq_post_msg(AT_DUT_CORE, num, __VA_ARGS__)

/*
 * 测试模式词表：结构与 asr_score.h 的 asr_score_t 一致(int + 31字节词条)。
 * 收到 AT+TEST=1 后切换，重启后回到默认词表（小金）。
 * 阈值沿用 V3 产测固件（asr_score.h）对小艾小艾/打开大灯的标定值：
 * 唤醒 -14500（空闲监听组），指令 -26000。模型已换成 V3 的 libmodel.a。
 */
#define AT_DUT_ASR_CMD_LEN     31
typedef struct {
    int score;
    char cmd[AT_DUT_ASR_CMD_LEN];
} at_dut_asr_score_t;

static const at_dut_asr_score_t at_dut_test_wake_score[] = {
    {-14500, "小艾小艾"},
};

static const at_dut_asr_score_t at_dut_test_cmd_score[] = {
    /* 与 V3 一致：唤醒词同时放进命令组——空闲态引擎在 commands group(1)，
     * 只有命令表里的词才会被接受。 */
    {-26000, "小艾小艾"},
    {-26000, "打开大灯"},
    {-26000, "开启大灯"},
    {-26000, "开大灯"},
};

/*
 * 测试模式标志：0 = 正式版，语音引擎读 Flash 里原版小金 grammar；
 * 1 = 测试模式，语音引擎读新增的小艾 grammar（libmodel.a 里已同时装入两份）。
 * 由 AT+TEST=1 置位；重启后 BSS 归零自动恢复正式版。
 */
unsigned char g_kws_test_mode;

/* 蓝牙起来重启智能语音后，再补一次测试词表，覆盖引擎异步初始化 */
static void at_dut_threshold_reapply(void *priv)
{
    (void)priv;
    AT_DBG("reapply test thresholds\n");
    wanson_threshold_update((void *)at_dut_test_wake_score, sizeof(at_dut_test_wake_score),
                            (void *)at_dut_test_cmd_score, sizeof(at_dut_test_cmd_score));
}

//=================================================================================//
//                                   内部变量                                       //
//=================================================================================//
static u8 at_dut_test_mode;                  // 测试模式标志：AT+TEST=1 置位，重启复位
static u8 at_dut_test_ok_pending;            // AT+TEST 已受理，等蓝牙就绪后回 OK
static u16 at_dut_test_ok_timer;             // AT+TEST 回 OK 兜底定时器

static u8 at_dut_line_buf[64];               // AT 行缓冲（RX 中断只写，AT 任务锁中断读走）
static u16 at_dut_line_len;
static volatile u8 at_dut_line_ready;
static u32 at_dut_last_rx_jiffies;           // 最近一次收到字节的时间

//=================================================================================//
//                                TRIM 值(频偏)                                     //
//  存储格式与蓝牙库一致：CFG_BT_FRE_OFFSET(110) 存 6 字节：crc(2)+data(4)；
//  data[0..1] 为 s16 频偏值。蓝牙库启动时 syscfg_read(110) 校验 CRC 后应用。
//=================================================================================//
struct at_dut_trim_item {
    u16 crc;
    u8  data[4];
} _GNU_PACKED_;

/* 读 TRIM：有效返回频偏值，无效(未初始化/CRC错)返回 0 */
static s16 at_dut_trim_read(void)
{
    struct at_dut_trim_item item;
    int ret;

    ret = syscfg_read(CFG_BT_FRE_OFFSET, (u8 *)&item, sizeof(item));
    AT_DBG("trim read ret=%d\n", ret);
    if (ret == sizeof(item)) {
        if (item.crc == CRC16((u8 *)&item.data, 4)) {
            s16 v;

            memcpy(&v, &item.data[0], 2);
            AT_DBG("trim read crc ok, value=%d\n", v);
            return v;
        }
        AT_DBG("trim read crc fail\n");
    }
    return 0;
}

/* 写 TRIM：组装 crc(2)+data(4) 结构写入 CFG_BT_FRE_OFFSET */
static int at_dut_trim_write(s16 trim)
{
    struct at_dut_trim_item item;
    int ret;

    memset(&item, 0, sizeof(item));
    memcpy(&item.data[0], &trim, 2);
    item.crc = CRC16((u8 *)&item.data, 4);

    ret = syscfg_write(CFG_BT_FRE_OFFSET, (u8 *)&item, sizeof(item));
    AT_DBG("trim write ret=%d\n", ret);
    if (ret != sizeof(item)) {
        return -1;
    }
    return 0;
}

//=================================================================================//
//                                   回复发送                                       //
//=================================================================================//
/* AT 回复统一走语音串口发送队列，与二进制语音帧排队，避免互相打断 */
void at_dut_send(const char *s)
{
    u16 len = strlen(s);

    AT_DBG_HEX("at_dut_send", (u8 *)s, len);

    if (len) {
        wanson_uart2_send_data((unsigned char *)s, len);
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

/* 解析纯十进制数字，成功返回 0 */
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
        AT_DBG("dtm: __ble_dut_ops is null\n");
        AT_DUT_REPLY_ERROR();
        return;
    }
    if (!dtm_priv) {
        dtm_priv = __ble_dut_ops->init();
        if (!dtm_priv) {
            AT_DBG("dtm: init fail\n");
            AT_DUT_REPLY_ERROR();
            return;
        }
        AT_DBG("dtm: init ok priv=%p\n", dtm_priv);
    }
    if (__ble_dut_ops->ioctrl(BLE_DUT_SET_TX_MODE, &param) != 0) {
        AT_DBG("dtm: set tx mode fail\n");
        AT_DUT_REPLY_ERROR();
        return;
    }
    AT_DBG("dtm: enter 2402MHz TX ok\n");
    AT_DUT_REPLY_OK();
}

/* AT+TEST 回 OK 兜底：蓝牙初始化异常时避免产线一直等待 */
static void at_dut_test_ok_timeout(void *priv)
{
    (void)priv;
    AT_DBG("test ok fallback timeout, pending=%u\n", at_dut_test_ok_pending);
    at_dut_test_ok_timer = 0;
    if (at_dut_test_ok_pending) {
        at_dut_test_ok_pending = 0;
        AT_DUT_REPLY_OK();
    }
}

/* 进入测试模式：切测试词表 -> 拉起蓝牙(等就绪后回 OK) */
static void at_dut_enter_test_mode(void)
{
    AT_DBG("AT+TEST=1 enter test mode, current=%u\n", at_dut_test_mode);
    if (at_dut_test_mode) {
        return;
    }
    at_dut_test_mode = 1;
    /* 让语音引擎在下一次初始化时使用小艾 grammar（正式版用小金） */
    g_kws_test_mode = 1;
    /* 测试模式开启识别提示音（普通模式保持静音） */
    tone_play_switch_set(1);

    /* 唤醒词切到小艾小艾，命令词切到打开大灯；重启后由默认初始化恢复小金词表 */
    int thr_ret = wanson_threshold_update((void *)at_dut_test_wake_score, sizeof(at_dut_test_wake_score),
                                          (void *)at_dut_test_cmd_score, sizeof(at_dut_test_cmd_score));
    AT_DBG("threshold update ret=%d\n", thr_ret);

    /* 蓝牙按 V3 水平拉起：走标准 BT 模式入口完成协议栈初始化。
     * V3 是开机默认时钟(96M)下初始化蓝牙；V4 此时离线语音已把主频锁到 192M，
     * 先释放主频锁回到默认时钟再进蓝牙，避免控制器初始化异常。 */
    at_dut_test_ok_pending = 1;
    if (!at_dut_test_ok_timer) {
        at_dut_test_ok_timer = sys_timeout_add(NULL, at_dut_test_ok_timeout, 5000);
        AT_DBG("ok fallback timer add=%d\n", at_dut_test_ok_timer);
    }
    /* 运行很久的语音/音频流状态会让首次拉起 btstack 异常复位（开机直接进蓝牙则正常），
     * 先关闭智能语音回到开机时的干净状态，蓝牙起来后再重启语音。 */
    audio_smart_voice_detect_close();
    AT_DBG("smart voice closed before bt init\n");
    app_send_message(APP_MSG_GOTO_MODE, APP_MODE_BT);
    AT_DBG("post GOTO_MODE BT done, wait bt init ok\n");
}

/* 蓝牙初始化完成：给 AT+TEST 补回 OK */
void at_dut_bt_init_ok_hook(void)
{
    AT_DBG("bt init ok hook, pending=%u\n", at_dut_test_ok_pending);
    if (!at_dut_test_ok_pending) {
        return;
    }
    at_dut_test_ok_pending = 0;
    if (at_dut_test_ok_timer) {
        sys_timeout_del(at_dut_test_ok_timer);
        at_dut_test_ok_timer = 0;
    }
    /* 测试模式下禁止无连接自动关机，避免长测站被 180s 自动关机打断 */
    extern void sys_auto_shut_down_disable(void);
    sys_auto_shut_down_disable();
    /* 蓝牙就绪：重启智能语音，并恢复测试词表（小艾小艾/打开大灯） */
    audio_smart_voice_detect_start();
    at_dut_threshold_reapply(NULL);
    sys_timeout_add(NULL, at_dut_threshold_reapply, 2000);
    AT_DBG("smart voice restarted, test thresholds reapplied\n");
    /* 测试模式打开 DAC（语音测试期间需要音频输出通路工作） */
    int dac_ret = audio_dac_try_power_on(&dac_hdl);
    AT_DBG("dac power on ret=%d\n", dac_ret);
    /* 智能语音重启会异步重建语音串口任务，等串口就绪后再回 OK，避免回复被丢掉 */
    int wait = 0;
    while (wanson_uart2_handle_get() < 0 && wait++ < 500) {
        os_time_dly(1);
    }
    AT_DBG("wait voice uart up: wait=%d hd=%d\n", wait, wanson_uart2_handle_get());
    AT_DBG("bt ready, reply OK to AT+TEST\n");
    AT_DUT_REPLY_OK();
}

static void at_dut_cmd_handle(u8 *cmd, u16 len)
{
    AT_DBG("cmd_handle len=%u test_mode=%u, raw:", len, at_dut_test_mode);
    AT_DBG_HEX("cmd", cmd, len);

    /* 只处理 AT+ 开头且以 \r\n 结束的行 */
    if (len < 3 || memcmp(cmd, "AT+", 3) != 0) {
        AT_DBG("not AT+ line, ignore\n");
        return;
    }

    /* AT+TEST=1 : 进入测试模式（唯一在测试模式外生效的指令），共 9 个字符 */
    if (len == 9 && memcmp(cmd, "AT+TEST=1", 9) == 0) {
        AT_DBG("match AT+TEST=1\n");
        if (at_dut_test_mode) {
            AT_DBG("already in test mode, reply OK\n");
            AT_DUT_REPLY_OK();
        } else {
            at_dut_enter_test_mode();
        }
        return;
    }

    /* 未进测试模式：其余产测指令一律不生效，静默 */
    if (!at_dut_test_mode) {
        AT_DBG("not in test mode, drop cmd\n");
        return;
    }

    /* AT+GVER? : 查询软件版本，回复 +GVER=XXX */
    if (len == 8 && memcmp(cmd, "AT+GVER?", 8) == 0) {
        char reply[32];

        AT_DBG("match AT+GVER?\n");
        sprintf(reply, "+GVER=%s\r\n", AT_DUT_SW_VERSION);
        at_dut_send(reply);
        return;
    }

    /* AT+MAC? : 查询 MAC 地址，回复 +MAC=XX:XX:XX:XX:XX:XX */
    if (len == 7 && memcmp(cmd, "AT+MAC?", 7) == 0) {
        const u8 *mac = bt_get_mac_addr();
        char reply[32];

        AT_DBG("match AT+MAC?\n");
        sprintf(reply, "+MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        at_dut_send(reply);
        return;
    }

    /* AT+TRIM? : 读 TRIM 值(频偏)，回复 +TRIM=X */
    if (len == 8 && memcmp(cmd, "AT+TRIM?", 8) == 0) {
        char reply[16];
        s16 trim = at_dut_trim_read();

        AT_DBG("match AT+TRIM?, trim=%d\n", trim);
        sprintf(reply, "+TRIM=%d\r\n", trim);
        at_dut_send(reply);
        return;
    }

    /* AT+TRIM=+1 / AT+TRIM=-1 : TRIM 值微调 */
    if (len == 10 && memcmp(cmd, "AT+TRIM=", 8) == 0 &&
        (cmd[8] == '+' || cmd[8] == '-') && cmd[9] == '1') {
        s16 trim = at_dut_trim_read();

        AT_DBG("match AT+TRIM=±1, old=%d dir=%c\n", trim, cmd[8]);
        trim += (cmd[8] == '+') ? 1 : -1;
        if (at_dut_trim_write(trim) != 0) {
            AT_DBG("trim write fail\n");
            AT_DUT_REPLY_ERROR();
            return;
        }
        AT_DBG("trim write ok, new=%d\n", trim);
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+TRIM=<signed kHz> : 直接写 TRIM 绝对值（单位 kHz）。
     * 符号约定（实测确认）：TRIM 值与实际频率反方向——
     * 想让实际频率升高 X kHz 就写 -X，想让实际频率降低 X kHz 就写 +X。
     * 例：实测 2401.475（比 2402 低 525kHz）→ AT+TRIM=-525。
     * 注意：写完后重启（或下次蓝牙初始化）才生效。 */
    if (len > 9 && memcmp(cmd, "AT+TRIM=", 8) == 0) {
        int val;
        int neg = 0;
        u16 i = 8;

        if (cmd[i] == '+' || cmd[i] == '-') {
            neg = (cmd[i] == '-');
            i++;
        }
        if (i >= len || at_dut_parse_dec(cmd + i, len - i, &val) != 0) {
            AT_DBG("trim abs parse fail\n");
            AT_DUT_REPLY_ERROR();
            return;
        }
        if (neg) {
            val = -val;
        }
        if (val < -32768 || val > 32767) {
            AT_DBG("trim abs out of range: %d\n", val);
            AT_DUT_REPLY_ERROR();
            return;
        }
        if (at_dut_trim_write((s16)val) != 0) {
            AT_DBG("trim abs write fail\n");
            AT_DUT_REPLY_ERROR();
            return;
        }
        AT_DBG("trim abs write ok, new=%d\n", val);
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+FREQ=XXXX : BDR 定频(2402/2441/2480MHz) */
    if (len > 8 && memcmp(cmd, "AT+FREQ=", 8) == 0) {
        int freq;

        AT_DBG("match AT+FREQ=\n");
        if (at_dut_parse_dec(cmd + 8, len - 8, &freq) != 0) {
            AT_DBG("freq parse fail\n");
            AT_DUT_REPLY_ERROR();
            return;
        }
        if (freq < 2402 || freq > 2480) {
            AT_DBG("freq out of range: %d\n", freq);
            AT_DUT_REPLY_ERROR();
            return;
        }
#if 0
        /* 诊断：临时屏蔽定频前“停基带”步骤（bredr_set_dut_enble + 关扫描/连接/广播 + 300ms），
         * 收到 AT+FREQ 直接调 bt_fix_fre_api，验证 +1.425MHz 偏移是否由 DUT 模式/停基带引入。 */
        bredr_set_dut_enble(1, 1);
        bt_cmd_prepare(USER_CTRL_WRITE_SCAN_ENABLE, 0, NULL);
        bt_cmd_prepare(USER_CTRL_WRITE_CONN_ENABLE, 0, NULL);
        bt_ble_adv_enable(0);
        u32 fix_fre_wait = jiffies;
        while (jiffies_offset_to_msec(fix_fre_wait, jiffies) < 300) {
            os_time_dly(1);
        }
#endif

        AT_DBG("fix freq %dMHz\n", freq);
        /* 按 BR 标准映射 freq-2402（0/39/78），三个频点都能出信号；
         * 实测输出比标称高约 1.4~1.5MHz（库频点表固定偏置，待定位）。 */
        bt_fix_fre_api(freq - 2402);
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+DTM=1 : 进入 DTM 测试模式，BLE 2402MHz TX */
#if TCFG_USER_BLE_ENABLE
    if (len == 8 && memcmp(cmd, "AT+DTM=1", 8) == 0) {
        AT_DBG("match AT+DTM=1\n");
        at_dut_cmd_dtm();
        return;
    }
#endif

    /* AT+DUT=1 : 进入 DUT 测试模式 */
    if (len == 8 && memcmp(cmd, "AT+DUT=1", 8) == 0) {
        AT_DBG("match AT+DUT=1\n");
        bt_bredr_enter_dut_mode(1, 1);
        AT_DUT_REPLY_OK();
        return;
    }

    /* AT+OTA=1 : 进入升级（V4 无按键，用串口指令代替按键组合） */
    if (len == 8 && memcmp(cmd, "AT+OTA=1", 8) == 0) {
        extern void go_mask_usb_updata(void);

        AT_DBG("match AT+OTA=1, flush then reboot to upgrade\n");
        AT_DUT_REPLY_OK();
        wanson_uart2_flush();
        go_mask_usb_updata();   /* 设置升级标志并复位，不复返 */
        return;
    }

    /* AT+RST=1 : 系统复位。定频模式不可恢复、TRIM 写入需重启生效，
     * 产线流程在定频/写 TRIM 后发此指令复位，重启后自动回到正式模式。 */
    if (len == 8 && memcmp(cmd, "AT+RST=1", 8) == 0) {
        AT_DBG("match AT+RST=1, system reset\n");
        AT_DUT_REPLY_OK();
        wanson_uart2_flush();
        system_reset(RESET_FLAG_RESERVE);
        return;
    }

    /* AT+MAC=XXXXXXXXXXXX : 写 MAC 地址(12位十六进制) */
    if (len == 19 && memcmp(cmd, "AT+MAC=", 7) == 0) {
        u8 mac[6];
        char reply[32];
        int i, h, l;

        AT_DBG("match AT+MAC=\n");
        for (i = 0; i < 6; i++) {
            h = at_dut_hex_val(cmd[7 + i * 2]);
            l = at_dut_hex_val(cmd[7 + i * 2 + 1]);
            if (h < 0 || l < 0) {
                AT_DBG("mac hex parse fail at %d\n", i);
                AT_DUT_REPLY_ERROR();
                return;
            }
            mac[i] = (u8)((h << 4) | l);
        }

        /* 存到 VM 区自定义 ID(可写)，替代 BTIF 写入 */
        if (syscfg_write(AT_DUT_MAC_CFG_ID, mac, 6) != 6) {
            AT_DBG("mac vm write fail\n");
            AT_DUT_REPLY_ERROR();
            return;
        }
        /* 更新内存中的当前地址并通知蓝牙控制器(立即生效) */
        bt_update_mac_addr(mac);
        lmp_hci_write_local_address(mac);

        AT_DBG_HEX("mac written", mac, 6);
        sprintf(reply, "+MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        at_dut_send(reply);
        return;
    }

    AT_DBG("unknown cmd, reply ERROR\n");
    AT_DUT_REPLY_ERROR();
}

//=================================================================================//
//                              RX 分流与 AT 任务                                   //
//=================================================================================//
/*
 * 中断上下文只负责组行，随后唤醒 AT 任务处理；
 * AT 任务上下文再做指令执行(flash 写、蓝牙拉起、阈值切换都不能在中断里做)。
 */
void at_dut_rx_byte(u8 c)
{
    at_dut_last_rx_jiffies = jiffies;

    AT_DBG("rx_byte=0x%02x '%c' line_len=%u ready=%u\n",
           c, (c >= 0x20 && c < 0x7f) ? c : '.', at_dut_line_len, at_dut_line_ready);

    if (c == '\r' || c == '\n') {
        if (at_dut_line_len > 0 && !at_dut_line_ready) {
            AT_DBG("line end, post to at task, len=%u\n", at_dut_line_len);
            at_dut_line_ready = 1;
            at_dut_post_msg(1, 0);
        }
        return;
    }

    if (at_dut_line_ready) {
        AT_DBG("prev line not consumed, drop byte\n");
        return; /* 上一行还未被 AT 任务取走，丢弃新字节避免覆盖 */
    }
    if (at_dut_line_len == 0) {
        if (c != 'A') {
            AT_DBG("not 'A', ignore\n");
            return; /* 只收集 AT 开头的行，忽略二进制帧 */
        }
        at_dut_line_buf[at_dut_line_len++] = c;
        return;
    }
    if (at_dut_line_len < sizeof(at_dut_line_buf) - 1) {
        at_dut_line_buf[at_dut_line_len++] = c;
    } else {
        AT_DBG("line overflow, reset\n");
        at_dut_line_len = 0; /* 行过长，丢弃重新开始 */
    }
}

void at_dut_rx_flush(void)
{
    AT_DBG("rx_flush line_len=%u ready=%u\n", at_dut_line_len, at_dut_line_ready);
    if (at_dut_line_len > 0 && !at_dut_line_ready) {
        at_dut_line_ready = 1;
        at_dut_post_msg(1, 0);
    }
}

/* 兼容不带 \r\n 结尾的指令：停止接收超过 100ms 就把攒下的字节当作一行处理 */
void at_dut_rx_idle_tick(void)
{
    if (at_dut_line_len > 0 && !at_dut_line_ready &&
        jiffies_offset_to_msec(at_dut_last_rx_jiffies, jiffies) > 100) {
        AT_DBG("idle 100ms, flush partial line\n");
        at_dut_rx_flush();
    }
}

/* 开机恢复产测 MAC：从 VM 自定义 ID 读，覆盖 BTIF 值，让蓝牙广播用产测 MAC */
static void at_dut_mac_restore(void)
{
    u8 mac[6];
    u8 ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int ret = syscfg_read(AT_DUT_MAC_CFG_ID, mac, 6);

    AT_DBG("mac restore read ret=%d\n", ret);
    if (ret == 6 &&
        memcmp(mac, ff, 6) != 0) {
        bt_update_mac_addr(mac);
        lmp_hci_write_local_address(mac);
        AT_DBG_HEX("mac restored", mac, 6);
    }
}

u8 at_dut_test_mode_get(void)
{
    return at_dut_test_mode;
}

static void at_dut_task(void *priv)
{
    int msg[8];
    u8 line[sizeof(at_dut_line_buf)];
    u16 len;
    int pend_ret;

    AT_DBG("at_dut_task start\n");
    at_dut_mac_restore();

    while (1) {
        pend_ret = os_taskq_pend(NULL, msg, ARRAY_SIZE(msg));
        AT_DBG("taskq pend ret=%d line_ready=%u\n", pend_ret, at_dut_line_ready);

        if (!at_dut_line_ready) {
            continue;
        }

        local_irq_disable();
        len = at_dut_line_len;
        memcpy(line, at_dut_line_buf, len);
        at_dut_line_len = 0;
        at_dut_line_ready = 0;
        local_irq_enable();

        line[len] = 0;
        AT_DBG("task copy line len=%u: ", len);
        AT_DBG_HEX("line", line, len);
        at_dut_cmd_handle(line, len);
    }
}

//=================================================================================//
//                                    注册                                          //
//=================================================================================//
static int app_at_dut_init(void)
{
    AT_DBG("app_at_dut_init, create task '%s'\n", AT_DUT_CORE);
    /* 普通模式默认关闭识别提示音，保持 V4 原有行为 */
    tone_play_switch_set(0);
    int err = os_task_create(at_dut_task, NULL, 2, 1024, 32, AT_DUT_CORE);
    AT_DBG("os_task_create ret=%x\n", err);
    if (err != OS_NO_ERR) {
        log_error("at dut task create fail: %x\n", err);
        return err;
    }
    return 0;
}
late_initcall(app_at_dut_init);

#endif /* TCFG_AT_DUT_ENABLE */
