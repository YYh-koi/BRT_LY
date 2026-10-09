/******************************************************************************
 *  @file      voice_app.c
 *  @author    Tangp
 *  @date      29 January 2021
 *  @version   V1.0
 *  @copyright Shanghai Wanson Elec Technology Co., Ltd. All rights reserved.
 ******************************************************************************/

#include "wanson/voice_config.h"
#ifdef TCFG_QW_DEMO_ENABLE
#include "smart_voice/wanson/aliyun/qw_config.h"
#endif

#undef TAG
#define TAG "WANSON_ASR"

#if (TCFG_WANSON_DEMO_ENABLE)

#include "asr.h"
#include "wanson/hz_asr/asr_score.h"
// #include "fst_graph.h"
// #include "fst_words.h"
#if CONFIG_ANC_ENABLE
#include "audio_anc.h"
#endif /*CONFIG_ANC_ENABLE*/

#include "wanson/uart/voice_uart.h"
#include "app_at_dut.h"

#if CONFIG_VAD_PLATFORM_SUPPORT_EN
extern unsigned char vad_wake_flag;
#endif

extern u32 tone_play_start_tick2;  // 记录开始时的系统tick

#if (TCFG_THIRD_PARTY_PROTOCOLS_SEL == RCSP_MODE_EN) && TCFG_WANSON_AUDIO_OPUS_ENABLE
static void app_ai_open()
{
    // FFF7F100 开启App AI对话
    unsigned char data[4] = {0xFF, 0xF7, 0xF1, 0x00};
    rcsp_user_cmd_send(data, 4);
}
static void app_ai_close()
{
    // FFF7F101 关闭App AI对话
    unsigned char data[4] = {0xFF, 0xF7, 0xF1, 0x01};
    rcsp_user_cmd_send(data, 4);
}

// FFF7F500 打断AI对话
static void app_ai_play_interrupt()
{
    unsigned char data[4] = {0xFF, 0xF7, 0xF5, 0x00};
    rcsp_user_cmd_send(data, 4);
}

void app_cmd_recieve_no_respone(u8 *data, u16 len)
{
    switch (data[len - 1]) {
    case 02:
        WANSON_LOG_I("app:a2dp start");
        set_wakeup_status(0); // Reset wake-up status
        break;
    case 03:
        WANSON_LOG_I("app:a2dp stop");
        break;
    default:
        break;
    }
}

#endif
// FFF7F102 app下发状态 进入AI对话状态
// FFF7F103 app下发状态 退出AI对话状态

// Wake-up timeout duration in milliseconds
#define VOICE_WAKEUP_TIMEOUT 10 // Timeout duration
#define ASR_CMD_LIST_LEN 150    // Length for ASR commands
#define WANSON_ASR_PCM_DEBUG_ENABLE 1

// Global flags and timers
static unsigned char data_update_flag;
static unsigned char wakeup_flag = 0;                         // Indicates if the system is in wake-up state
static unsigned int asr_wakeup_tick_timer = 0;                // Timer for wake-up duration
static unsigned int asr_wakeup_timers = VOICE_WAKEUP_TIMEOUT; // Wake-up timer value
static u16 asr_timer;

#if (TCFG_WANSON_MODEL_GROUP_ENABLE)
typedef enum {
    ASR_MODEL_CN = 0,
    ASR_MODEL_EN,
} ASR_MODEL_INDEX;

void wanson_asr_mode_chage(ASR_MODEL_INDEX index)
{
    local_irq_disable();
    // 释放当前模型资源
    Wanson_ASR_Release();

    int ret = wanson_group_index_set(index);
    if (ret == OS_NO_ERR) {
        g_printf("wanson grop succ switch to (%d) (0-CN, 1-EN)!!!\n", index);
    }

    // 设置好分组后重新初始化
    Wanson_ASR_Init();
    Wanson_ASR_Reset(1);
    local_irq_enable();
}
#endif

///-----------------------
// 0 1 2
// 播放/唤醒/退出唤醒
static unsigned char threshold_index = 2;

int app_kws_threshold_index_get()
{
    return threshold_index;
}

void app_kws_threshold_timer_update(unsigned char value)
{
#if TCFG_AT_DUT_ENABLE
    /* 测试模式词表由 AT+TEST=1 统一管理，禁止其他逻辑把它冲回去 */
    if (at_dut_test_mode_get()) {
        return;
    }
#endif

    // 中文模型参数
    switch (value) {
    case 0:
    case 1:
        /* 播放、唤醒状态使用完整命令词表 */
        if (wanson_threshold_update(&wake_score, sizeof(wake_score), &cmd_score, sizeof(cmd_score)) < 0) {
            printf("wanson_threshold_update Failed!");
        }
        threshold_index = value;
        break;
    case 2:
        /* 退出唤醒 */
        if (wanson_threshold_update(&wake_score, sizeof(wake_score), &wake_score, sizeof(wake_score)) < 0) {
            printf("wanson_threshold_update Failed!");
        }
        threshold_index = value;
        break;

    default:
        break;
    }
}

int app_kws_group_set(unsigned char value)
{
    if (wanson_kws_group_index_get() != value) {
        Wanson_ASR_Reset(value);
    }
    return 0;
}

// Set the wake-up timer value
void set_wakeup_time(unsigned int value)
{
    asr_wakeup_timers = value;
}

// Get the current wake-up status
int get_wakeup_status()
{
    return wakeup_flag;
}

// Set the wake-up status
void set_wakeup_status(unsigned char value)
{
    wakeup_flag = value;
}

// Timer loop for ASR, called every 100 ms
static void asr_timer_loop(void *p)
{
    static int cnt = 0;

    cnt++;
    if (cnt >= 10) { // If count reaches 10, increment timer by 1 second
        cnt = 0;
        if (wakeup_flag) {
            asr_wakeup_tick_timer++; // Increment timer if in wake-up state
        } else {
            asr_wakeup_tick_timer = 0; // Reset timer if not in wake-up state
        }
        // mem_stats();
        // WANSON_LOG_D("bt_a2dp_status_check: (%d)\n", bt_a2dp_status_check());
    }
}

// Clear the ASR timer
void asr_timer_cnt_clear(void)
{
    asr_wakeup_tick_timer = 0; // Reset the wake-up tick timer
}

// Initialize the ASR timer
static void asr_timer_init(void)
{
    asr_wakeup_tick_timer = 0;                     // Initialize timer
    wakeup_flag = 0;                               // Reset wake-up flag
    if (!asr_timer) {
        asr_timer = sys_s_hi_timer_add(NULL, asr_timer_loop, 100);
    }
}

static void asr_timer_deinit(void)
{
    if (asr_timer) {
        sys_s_hi_timer_del(asr_timer);
        asr_timer = 0;
    }
    asr_wakeup_tick_timer = 0;
    wakeup_flag = 0;
}

// Check if the ASR has timed out
static int asr_is_timeout(unsigned int timeOut)
{
    return (asr_wakeup_tick_timer > timeOut) ? 1 : 0; // Return 1 if timed out
}

static void app_timerout_init()
{
    static u8 qw_cnt = 0;

    WANSON_LOG_I("\r\n wakedown...\r\n\r\n"); // Print wake down message

#ifdef TCFG_QW_DEMO_ENABLE
    if (dummy_recoder_is_work()) { // 千问正处于listening状态
        qw_cnt++;
        if (qw_cnt > 1) {
            qw_active_stop();
            goto _wake_exit;
        }
        asr_timer_cnt_clear();
        set_wakeup_status(1);

        g_printf("It's listening -> reset wakeup_tick!!!\r\n");
        return;
    }
#endif

_wake_exit:
    qw_cnt = 0;

    set_wakeup_status(0); // Reset wake-up status

#if CONFIG_VAD_PLATFORM_SUPPORT_EN
    if (!dev_music_play_status_get()) {
        // vad_wake_flag = 0;
        smart_voice_mcu_mic_change(VOICE_VAD_MIC);
    }
    // 修改时间: 2025-11-19
    // 避免 voice_music.c-timer_kws_status_loop 切换分组冲突（一般情况下应该不会复现，因为在播放的情况下退出唤醒后切换分组不回去判断vad_wake_flag）
    vad_wake_flag = 0;
#endif
}

static int asr_wakeup_tone_play_end_callback(void *priv, enum stream_event event)
{
    if (event == STREAM_EVENT_STOP) {
        WANSON_LOG_I("asr_wakeup_tone_play_end_callback\r\n");
    }
    return 0;
}

//==================================================================================
/**
 * @brief 请求listen
 *
 */
static void qw_listen_start()
{
#ifdef TCFG_QW_DEMO_ENABLE
    if (qw_server_is_connected()) {
        OS_SEM mic_sem;
        os_sem_create(&mic_sem, 0);
        if (OS_NO_ERR == qw_core_post_msg(2, QW_RECORD_START, &mic_sem)) { // 请求开启对话
            os_sem_pend(&mic_sem, 10);
        }
        os_sem_del(&mic_sem, 0);
    }
#endif
}

/**
 * @brief 停止qw 活动
 *
 */
void qw_active_stop()
{
#ifdef TCFG_QW_DEMO_ENABLE
    if (qw_server_is_connected()) {
        OS_SEM sem;
        os_sem_create(&sem, 0);
        qw_core_post_msg(2, QW_STOP_ALL, NULL); // 停止大模型语音识别和opus解码
        os_sem_pend(&sem, 10);
        os_sem_del(&sem, 0);
    }

#endif
}

static bool qw_multi_continue; // 是否开启qw多轮对话

bool voice_qw_multi_get()
{
    return qw_multi_continue ? true : false;
}

void voice_qw_multi_set(bool val)
{
    qw_multi_continue = val ? 1 : 0;
}

int siri_status_check()
{
    return (app_var.siri_stu == 1 || app_var.siri_stu == 2) ? 1 : 0;
}


static u8 siri_closing_flag = 0;
static void clear_siri_flag(void *priv)
{
    siri_closing_flag = 0;
}

u8 get_siri_closing(void)
{
    return siri_closing_flag;
}

extern u8 a2dp_preempted_addr[6];
static void app_siri_close()
{
#if TCFG_APP_BT_EN
    if (bt_get_connect_status() >= BT_STATUS_CONNECTING) {
        printf("-------->>>>>>>siri_status_check==%d\n", siri_status_check());
        if (siri_status_check()) {

            qw_active_stop();  // 停止云端对话

            // 2. 发送停止播放给手机
            #if (TCFG_THIRD_PARTY_PROTOCOLS_SEL == RCSP_MODE_EN)
                app_ai_play_interrupt();  // 打断APP AI对话
            #endif

            // 3. 关闭Siri
            bt_cmd_prepare(USER_CTRL_HFP_GET_SIRI_CLOSE, 0, NULL);

            bt_cmd_prepare(USER_CTRL_AVCTP_OPID_STOP, 0, NULL);
            bt_cmd_prepare(USER_CTRL_A2DP_CMD_SUSPEND, 0, NULL);
           
            siri_closing_flag = 1;
            sys_timeout_add(NULL, clear_siri_flag, 800);


        
        }
    }
#endif
}

static void siri_close_check_loop()
{
#if TCFG_APP_BT_EN
    g_printf("siri_close_check_loop\n");

    if (siri_status_check()) {
        sys_s_hi_timerout_add(NULL, siri_close_check_loop, 1000);
    } else {
        lmp_private_esco_suspend_resume(4);
        bt_key_call_siri(); // 触发Siri助手
    }
#endif
}

static void app_siri_open()
{
#if TCFG_APP_BT_EN
    if (bt_get_connect_status() < BT_STATUS_CONNECTING) {
        voice_tone_play(IDEX_TONE_NUM_PLEASE); // 提示用户未连接蓝牙
        return;
    }
    set_wakeup_status(0);      // 设置唤醒状态
    asr_timer_cnt_clear();     // 清除计时器
    last_music_status_clear(); // 清除最后一次的音乐播放状态

    if (bt_get_connect_status() >= BT_STATUS_CONNECTING) {
        if (siri_status_check()) {
            sys_s_hi_timerout_add(NULL, siri_close_check_loop, 1000);
        } else {
            lmp_private_esco_suspend_resume(4);
            bt_key_call_siri(); // 触发Siri助手
            g_printf("app_siri_open\n");
        }
    }
#endif
}

//==================================================================================
static void app_wakeup_init()
{
    WANSON_LOG_I("wakeup...\r\n\r\n"); // Print wake-up message

    //增加判断siri状态是否已关闭
#if TCFG_APP_BT_EN
    if(bt_get_connect_status() >= BT_STATUS_CONNECTING)
    {
        do{
            if(siri_status_check())
            {
                os_time_dly(1);
                putchar('a');
            }
            else
            {
                break;
            }
        } while(1);
    }

    // bt_cmd_prepare(USER_CTRL_HFP_GET_SIRI_CLOSE, 0, NULL);
#if CONFIG_VAD_PLATFORM_SUPPORT_EN
    vad_wake_flag = 0;
    smart_voice_mcu_mic_change(VOICE_MCU_MIC);
#endif
    set_wakeup_status(1);  // Set wake-up status
    asr_timer_cnt_clear(); // Clear wake-up timer

    WANSON_LOG_D("bt_a2dp_status_check %d", bt_a2dp_status_check());
    if (bt_a2dp_status_check()) { // 如果有音乐播放需要先停止，避免音箱语音识别
        app_send_message(APP_MSG_MUSIC_PAUSE, 0);
    }
#endif

    // extern const struct tone_files *get_tone_files();
    // int ret = play_tone_file_callback(get_tone_files()->asr_wakeup, NULL, asr_wakeup_tone_play_end_callback);
    // if (ret != 0) {
    //     WANSON_LOG_E("power on tone play err!!!");
    // }

#if (TCFG_THIRD_PARTY_PROTOCOLS_SEL == RCSP_MODE_EN) && TCFG_WANSON_AUDIO_OPUS_ENABLE
    // if (!wanson_opus_enc_record_status_get()) {
    WANSON_LOG_D("wanson_opus_enc_record_status_get is false");

#ifdef TCFG_QW_DEMO_ENABLE
    extern bool voice_rcsp_connect_get();
    if (voice_rcsp_connect_get()) {
        app_ai_open(); // 语音唤醒，开启app录音
    } else {
        qw_listen_start();
    }
    voice_tone_play(IDEX_TONE_NUM_WAKEUP);
#else
    app_ai_open(); // 语音唤醒，开启app录音
#endif
    // } else {
    //     WANSON_LOG_D("wanson_opus_enc_record_status_get is true");
    // }
#endif
}

// Initialize the ASR application
void wanson_app_asr_init()
{
    app_audio_set_volume(APP_AUDIO_STATE_WTONE, 16, 1);
    asr_timer_init();       // Initialize the ASR timer
    dev_music_timer_init(); // Initialize the music timer

    wanson_uart2_init();
}

void wanson_app_asr_deinit(void)
{
    dev_music_timer_deinit();
    asr_timer_deinit();
    wanson_uart2_deinit();
}

// Initialize the Wanson KWS algorithm
int wanson_kws_algorithm_init()
{
#if (TCFG_WANSON_MODEL_GROUP_ENABLE)
    wanson_group_para_init();             // Initialize model group parameters
    wanson_group_index_set(ASR_MODEL_CN); // Set group index to 0 cn
#endif

    if (Wanson_ASR_Init() < 0) {
        WANSON_LOG_I("Wanson_ASR_Init Failed!\n"); // Print failure message
        return -1;                                 // Return error
    }
    Wanson_ASR_Reset();                    // Reset ASR
    WANSON_LOG_I("Wanson_ASR_Init OK!\n"); // Print success message

    // 初始阈值设置
    app_kws_threshold_timer_update(2);
    wanson_score_display_set(1);
    return 0; // Return success
}

// Deinitialize the Wanson KWS algorithm
void wanson_kws_algorithm_deinit()
{
    Wanson_ASR_Release(); // Release ASR resources
}

// Process the application logic
int wanson_app_proc()
{
    if (wakeup_flag && asr_is_timeout(asr_wakeup_timers)) { // Check for wake-up timeout
        app_timerout_init();                                // Reset wake-up status
    }
    asr_voice_readme_play();

    return 0; // Indicate successful processing
}

// Enumeration for ASR command list items
typedef enum {
    ASR_WAKEUP = 0,
    ASR_BYEBYE,

    ASR_VOICE_ASS,
    ASR_ANSWER,
    ASR_HUNG,

    ASR_PRE,
    ASR_NEXT,
    ASR_PLAY,
    ASR_PAUSE,
    ASR_VOL_UP,
    ASR_VOL_DOWN,
    ASR_SWITCH_MODE_CN,
    ASR_SWITCH_MODE_EN,

    ASR_TAKE_PHOTO,
    ASR_VIDEO_RECORD,
    ASR_VIDEO_RECORD_STOP,
    ASR_VISUAL_RECOGNITION,
    ASR_PHOTO_RECOGNITION,
    ASR_WHATS_THIS,

    ASR_01,
    ASR_02,
    ASR_03,
    ASR_04,
    ASR_05,
    ASR_06,
    ASR_07,
    ASR_08,
    ASR_09,
    ASR_10,
    ASR_11,
    ASR_12,
    ASR_13,
    ASR_14,
    ASR_15,
    ASR_16,
    ASR_17,


    ASR_RESPONSE_RESULT_INDEX
} ASR_COMD_LIST_ITEM;

// Define ASR command list
const uint8_t asr_cmd_list[ASR_RESPONSE_RESULT_INDEX][ASR_CMD_LIST_LEN] = {
    /*00*/ {"你好小金|小金小金"},          // Wake-up command
    /*01*/ {"小金再见"},                    // Exit wake-up command
    /*00*/ {"打开助手|hey assistant"},    // Wake-up command
    /*00*/ {"接听电话|answer the call"},  // Wake-up command
    /*00*/ {"挂断电话|hang up the call"}, // Wake-up command

    /*00*/ {"上一首|上一曲|previous song"}, // Previous track command
    /*00*/ {"下一首|下一曲|换一首|next song"}, // Next track command

    /*03*/ {"继续播放"},
    /*05*/ {"暂停播放|暂停音乐|停止播放|stop playing|pause music"},

    /*00*/ {"增大音量|volume up"},
    /*00*/ {"减小音量|volume down"},
    /*00*/ {"switch to chinese"},
    /*00*/ {"切换到英文"},

    /*00*/ {"开始拍照|打开拍照|take a photo"},
    /*00*/ {"开始录像|打开录像|video recording"},
    /*00*/ {"停止录像|关闭录像|stop video recording"},
    /*00*/ {"视觉识别|visual recognization"},
    /*00*/ {"拍照识别|photo recognition"},
    /*00*/ {"我面前的是什么|whats this"},
    
    /*00*/ {"关闭 t c s|关闭提色额思|关 t c s|关提色额思"},
    /*00*/ {"打开 t c s|打开提色额思|开 t c s|开提色额思|开启提色额思|开启 t c s"},
    /*00*/ {"关闭陡坡缓降|关陡坡缓降"},
    /*00*/ {"打开陡坡缓降|开启陡坡缓降|开陡坡缓降"},
    /*00*/ {"关闭防溜坡|关防溜坡"},
    /*00*/ {"打开防溜坡|开启防溜坡|开防溜坡"},
    /*00*/ {"车辆自检"},
    /*00*/ {"进入倒车|打开倒车|开启倒车"},
    /*00*/ {"进入推车|打开推车|开启推车"},
    /*00*/ {"退出倒车|退出推车|关闭倒车|关闭推车"},
    /*00*/ {"打开大灯|开启大灯|开大灯"},
    /*00*/ {"关闭大灯|关大灯"},
    /*00*/ {"车辆设防|车设防"},
    /*00*/ {"车辆解防|车解防"},
    /*00*/ {"车辆上电|车上电"},
    /*00*/ {"车辆下电|车下电"},
    /*00*/ {"打开座桶|开启座桶|开座桶"},
   



};

struct asr_uart_command {
	const char *phrases;
	u8 command;
};

/*
 * 协议表是命令字的唯一来源，交付的 asr_score.h 是训练词的唯一来源。
 * 本表只负责两者之间的映射；asr_cmd_list 仅保留本机蓝牙动作。
 * 多个训练词表达同一意图时共用一个命令字，未分配命令字的词不入表。
 */
static const struct asr_uart_command asr_uart_command_table[] = {
	{"你好小金|小金小金", VOICE_UART_CMD_WAKEUP},
	{"小金再见", VOICE_UART_CMD_BYEBYE},
	{"停止播报", VOICE_UART_CMD_STOP_REPORT},
	{"上一首", VOICE_UART_CMD_PREV},
	{"下一首|换一首", VOICE_UART_CMD_NEXT},
	{"暂停播放|暂停音乐|停止播放", VOICE_UART_CMD_PAUSE},
	{"继续播放", VOICE_UART_CMD_PLAY},
	{"单曲循环", 0x07},
	{"顺序播放", 0x08},
	{"随机播放", 0x09},
	{"上一台", 0x0A},
	{"下一台", 0x0B},
	{"打开调频|打开调幅|打开收音机", 0x0C},
	{"关闭收音机", 0x0D},
	{"打开音乐", 0x0E},
	{"关闭音乐", 0x0F},
	{"打开监控", 0x10},
	{"关闭监控", 0x11},
	{"打开蓝牙", 0x12},
	{"关闭蓝牙", 0x13},
	{"关闭屏幕", 0x14},
	{"返回桌面", 0x15},
	{"调高屏幕亮度", 0x16},
	{"调低屏幕亮度", 0x17},
	{"最大屏幕亮度", 0x18},
	{"最小屏幕亮度", 0x19},
	{"调高仪表亮度", 0x1A},
	{"调低仪表亮度", 0x1B},
	{"最大仪表亮度", 0x1C},
	{"最小仪表亮度", 0x1D},
	{"打开白色主题", 0x1E},
	{"打开黑色主题", 0x1F},
	{"关闭声音", 0x20},
	{"打开声音", 0x21},
	{"增大音量", VOICE_UART_CMD_VOL_UP},
	{"减小音量", VOICE_UART_CMD_VOL_DOWN},
	{"打开空调", 0x24},
	{"关闭空调", 0x25},
	{"调高温度", 0x26},
	{"调低温度", 0x27},
	{"最高温度", 0x28},
	{"最低温度", 0x29},
	{"制冷模式", 0x2A},
	{"加热模式", 0x2B},
	{"内循环", 0x2C},
	{"外循环", 0x2D},
	{"调高风速", 0x2E},
	{"调低风速", 0x2F},
	{"最大风速", 0x30},
	{"最小风速", 0x31},
	{"打开新风", 0x32},
	{"关闭新风", 0x33},
	{"打开净化", 0x34},
	{"关闭净化", 0x35},
	{"打开司机区暖风", 0x36},
	{"打开乘客区暖风", 0x37},
	{"暖风高速", 0x38},
	{"暖风低速", 0x39},
	{"打开电加热器", 0x3A},
	{"关闭电加热器", 0x3B},
	{"打开柴油加热器", 0x3C},
	{"关闭柴油加热器", 0x3D},
	{"打开氛围灯", 0x3E},
	{"关闭氛围灯", 0x3F},
	{"打开乘客阅读灯", 0x40},
	{"关闭乘客阅读灯", 0x41},
	{"打开后雾灯", 0x42},
	{"关闭后雾灯", 0x43},
	{"打开前雾灯", 0x44},
	{"关闭前雾灯", 0x45},
	{"打开自动大灯", 0x46},
	{"关闭自动大灯", 0x47},
	{"打开司机顶灯", 0x48},
	{"关闭司机顶灯", 0x49},
	{"打开过道灯", 0x4A},
	{"关闭过道灯", 0x4B},
	{"打开车外氛围灯", 0x4C},
	{"关闭车外氛围灯", 0x4D},
	{"打开前面罩氛围灯", 0x4E},
	{"关闭前面罩氛围灯", 0x4F},
	{"打开安全带报警", 0x50},
	{"关闭安全带报警", 0x51},
	{"打开智能排水加热", 0x52},
	{"关闭智能排水加热", 0x53},
	{"打开自动宣传视频", 0x54},
	{"关闭自动宣传视频", 0x55},
	{"打开前挡风扇", 0x56},
	{"关闭前挡风扇", 0x57},
	{"打开化冰", 0x58},
	{"打开自动雨刮", 0x59},
	{"关闭自动雨刮", 0x5A},
	{"打开行李舱灯", 0x5B},
	{"关闭行李舱灯", 0x5C},
	{"打开司机风扇", 0x5D},
	{"关闭司机风扇", 0x5E},
};

int compare_substring(const char *strA, const char *strB)
{
    const char delim[] = ",.!|";
    char *strACopy = strdup(strA);
    char *token = strtok(strACopy, delim);
    while (token != NULL) {
        if (strcmp(token, strB) == 0) {
            free(strACopy);
            return 1;
        }
        token = strtok(NULL, delim);
    }

    free(strACopy);
    return 0;
}

static int asr_uart_command_find(const char *result_dat)
{
	for (u16 i = 0; i < ARRAY_SIZE(asr_uart_command_table); i++) {
		if (compare_substring(asr_uart_command_table[i].phrases, result_dat)) {
			return asr_uart_command_table[i].command;
		}
	}
	return -1;
}

/*
 * MCU下行命令是独立控制，不是语音上报的ACK。
 * 语音识别路径立即发送0x01方向帧，确认音由TCFG_WANSON_ASR_TONE_ENABLE控制。
 * MCU下行只触发语音模组本地已有的动作，不再回发串口。
 */
void voice_mcu_command_handle(unsigned char command)
{
	switch (command) {
	case VOICE_UART_CMD_WAKEUP:
		app_wakeup_init();
		break;
	case VOICE_UART_CMD_BYEBYE:
		voice_tone_play(IDEX_TONE_TONE_BYEBYE);
		break;
	case VOICE_UART_CMD_STOP_REPORT:
		tone_player_stop();
		break;
	case VOICE_UART_CMD_PREV:
		voice_tone_play(IDEX_TONE_NUM_PREV);
		break;
	case VOICE_UART_CMD_NEXT:
		voice_tone_play(IDEX_TONE_NUM_NEXT);
		break;
	case VOICE_UART_CMD_PAUSE:
		voice_tone_play(IDEX_TONE_NUM_PAUSE);
		break;
	case VOICE_UART_CMD_PLAY:
		voice_tone_play(IDEX_TONE_NUM_PLAY);
		break;
	case VOICE_UART_CMD_VOL_UP:
		voice_tone_play(IDEX_TONE_NUM_VOL_UP);
		break;
	case VOICE_UART_CMD_VOL_DOWN:
		voice_tone_play(IDEX_TONE_NUM_VOL_DOWN);
		break;
	default:
		WANSON_LOG_I("rx command 0x%02x has no local action", command);
		break;
	}
}

// Handle recognized ASR commands
void asr_deal(const char *result_dat)
{
    // 【计时开始】记录当前系统tick（单位：10ms或1ms，取决于config）
    tone_play_start_tick2 = timer_get_ms();  // 获取毫秒级时间戳

#if TCFG_AT_DUT_ENABLE
    if (at_dut_test_mode_get()) {
        /* 测试模式：唤醒词为小艾小艾，只回 ASCII +WAKE=1（测试模式内停发二进制帧） */
        if (compare_substring("小艾小艾", result_dat)) {
            at_dut_send("+WAKE=1\r\n");
            app_wakeup_init();
            /* 测试模式播放唤醒提示音，验证 DAC/喇叭输出 */
            voice_tone_play(IDEX_TONE_NUM_WAKEUP);
            return;
        }
        /* 测试模式：打开大灯 回复 +LIGHT=1（无二进制映射，按约定只回 ASCII） */
        if (compare_substring("打开大灯|开启大灯|开大灯", result_dat)) {
            at_dut_send("+LIGHT=1\r\n");
            /* 播放提示音，验证 DAC/喇叭输出；
             * 按 V3 演示版口径：打开大灯用 11.mp3（关闭大灯在 V3 演示版用 12.mp3） */
            voice_tone_play(IDEX_TONE_TONE_11);
            return;
        }
        /* 测试模式下其余识别结果不上送 */
        return;
    }
#endif

#if (TCFG_USER_TWS_ENABLE)
    if (bt_tws_is_master() == false) {
        WANSON_LOG_D("slave return");
        return;
    }
#endif

	int uart_command = asr_uart_command_find(result_dat);
	if (uart_command >= 0) {
		if (uart_command != VOICE_UART_CMD_WAKEUP && !get_wakeup_status()) {
			return;
		}
		wanson_uart2_send_command((u8)uart_command);
	}

	int32_t item = 0;
    for (; item < ASR_RESPONSE_RESULT_INDEX; item++) {
        if (compare_substring((const char *)&asr_cmd_list[item], result_dat)) {
            break;
        }
    }
    if (item >= ASR_RESPONSE_RESULT_INDEX)
        return;

    if (item > ASR_HUNG) {
        if (!get_wakeup_status()) {
            return;
        }
    }
    asr_timer_cnt_clear(); // Clear wake-up timer

    

#if (TCFG_THIRD_PARTY_PROTOCOLS_SEL == RCSP_MODE_EN) && TCFG_WANSON_AUDIO_OPUS_ENABLE
    if (wanson_opus_enc_record_status_get()) { // opus编码打开的情况就任务app录音是开启的
#ifdef TCFG_QW_DEMO_ENABLE
        extern bool voice_rcsp_connect_get();
        if (voice_rcsp_connect_get()) {
            WANSON_LOG_D("app ai play close");
            app_ai_play_interrupt(); // 有离线识别结果，关闭app结果
        }
#else
        WANSON_LOG_D("app ai play close");
        app_ai_play_interrupt(); // 有离线识别结果，关闭app结果
#endif
    }
#endif

#ifdef TCFG_QW_DEMO_ENABLE
    if (item > ASR_HUNG) {
        qw_active_stop();
    }
#endif

    app_siri_close(); // siri 打开后, 识别了离线指令则需要关闭siri

    switch (item) {
    case ASR_WAKEUP:
        app_wakeup_init(); // Initialize wake-up status
        break;
    case ASR_BYEBYE:
        voice_tone_play(IDEX_TONE_TONE_BYEBYE);
        // qw_active_stop(); // Initialize wake-up status
        // tone_event_uart_send(UART_SEND_BYEBYE);
        break;
    default:
        switch (item) {
        case ASR_VOICE_ASS:
            // app_send_message(APP_MSG_OPEN_SIRI, 0);

            app_siri_open();
            tone_event_uart_send(UART_SEND_SIRI);
            break;
        case ASR_ANSWER:
            if (bt_phone_status_check()) {
                app_send_message(APP_MSG_CALL_ANSWER, 0);
            }
            break;
        case ASR_HUNG:
            // if (bt_phone_status_check())
            {
                app_send_message(APP_MSG_CALL_HANGUP, 0);
            }
            break;

        case ASR_PRE:
            voice_tone_play(IDEX_TONE_NUM_PREV);
            // app_send_message(APP_MSG_MUSIC_PREV, 0);
            // set_wakeup_status(0); // 控制播放后退出离线语音唤醒状态
            break;
        case ASR_NEXT:
            voice_tone_play(IDEX_TONE_NUM_NEXT);
            // app_send_message(APP_MSG_MUSIC_NEXT, 0);
            // set_wakeup_status(0); // 控制播放后退出离线语音唤醒状态
            break;

        case ASR_PLAY:
            voice_tone_play(IDEX_TONE_NUM_PLAY);

            // int ret = bt_a2dp_status_check();
            // g_printf("a2dp status: %d\n", ret);
            // ret = bt_get_connect_status();
            // g_printf("bt music status: %d\n", ret);

            // if (!bt_a2dp_status_check() || (bt_get_connect_status() != BT_STATUS_PLAYING_MUSIC)) {
            //     putchar('P');
            //     app_send_message(APP_MSG_MUSIC_PP, 0);
            //     set_wakeup_status(0); // 控制播放后退出离线语音唤醒状态
            // }
            break;

        case ASR_PAUSE:
            voice_tone_play(IDEX_TONE_NUM_PAUSE);
            // if (bt_a2dp_status_check()) {
            //     app_send_message(APP_MSG_MUSIC_PAUSE, 0);
            // }
            break;
        case ASR_VOL_UP:
            voice_tone_play(IDEX_TONE_NUM_VOL_UP);
            // app_send_message(APP_MSG_VOL_UP, 0);
            break;
        case ASR_VOL_DOWN:
            voice_tone_play(IDEX_TONE_NUM_VOL_DOWN);
            // app_send_message(APP_MSG_VOL_DOWN, 0);
            break;

#if (TCFG_WANSON_MODEL_GROUP_ENABLE)
        case ASR_SWITCH_MODE_CN:
            // wanson_asr_mode_chage(ASR_MODEL_CN);
            break;

        case ASR_SWITCH_MODE_EN:
            // wanson_asr_mode_chage(ASR_MODEL_EN);
            break;
#endif
        case ASR_TAKE_PHOTO:
            break;
        case ASR_VIDEO_RECORD:
            break;
        case ASR_VIDEO_RECORD_STOP:
            break;
        case ASR_VISUAL_RECOGNITION:
            break;
        case ASR_PHOTO_RECOGNITION:
            break;
        case ASR_WHATS_THIS:
            break;
        
        case ASR_01:
            voice_tone_play(IDEX_TONE_TONE_01);
            break;
        case ASR_02:
            voice_tone_play(IDEX_TONE_TONE_02);
            break;
        case ASR_03:
            voice_tone_play(IDEX_TONE_TONE_03);
            break;
        case ASR_04:
            voice_tone_play(IDEX_TONE_TONE_04);
            break;
        case ASR_05:
            voice_tone_play(IDEX_TONE_TONE_05);
            break;
        case ASR_06:
            voice_tone_play(IDEX_TONE_TONE_06);
            break;
        case ASR_07:
            voice_tone_play(IDEX_TONE_TONE_07);
            break;
        case ASR_08:
            voice_tone_play(IDEX_TONE_TONE_08);
            break;
        case ASR_09:
            voice_tone_play(IDEX_TONE_TONE_09);
            break;
        case ASR_10:
            voice_tone_play(IDEX_TONE_TONE_10);
            break;
        case ASR_11:
            voice_tone_play(IDEX_TONE_TONE_11);
            break;
        case ASR_12:
            voice_tone_play(IDEX_TONE_TONE_12);
            break;
        case ASR_13:
            voice_tone_play(IDEX_TONE_TONE_13);
            break;
        case ASR_14:
            voice_tone_play(IDEX_TONE_TONE_14);
            break;
        case ASR_15:
            voice_tone_play(IDEX_TONE_TONE_15);
            break;
        case ASR_16:
            voice_tone_play(IDEX_TONE_TONE_16);
            break;
        case ASR_17:
            voice_tone_play(IDEX_TONE_TONE_17);
            break;

        default:
            break;
        }
        break; // No valid command
    }
}
#if WANSON_ASR_PCM_DEBUG_ENABLE
static void wanson_asr_pcm_debug(const s16 *pcm, int len, int recog_called, int rs, int score)
{
	static u32 last_print_ms;
	static u32 frames;
	static u32 recog_calls;
	static u32 samples;
	static u64 abs_sum;
	static s64 sample_sum;
	static s32 min_sample = 32767;
	static s32 max_sample = -32768;
	static u32 peak;
	static int last_rs;
	static int last_score;
	u32 now = timer_get_ms();
	int sample_count = len / sizeof(s16);

	for (int i = 0; i < sample_count; i++) {
		s32 sample = pcm[i];
		u32 abs_sample = sample < 0 ? -sample : sample;

		if (sample < min_sample) {
			min_sample = sample;
		}
		if (sample > max_sample) {
			max_sample = sample;
		}
		if (abs_sample > peak) {
			peak = abs_sample;
		}
		abs_sum += abs_sample;
		sample_sum += sample;
	}

	frames++;
	samples += sample_count;
	if (recog_called) {
		recog_calls++;
		last_rs = rs;
		last_score = score;
	}
	if (!last_print_ms) {
		last_print_ms = now;
	}

	if (now - last_print_ms >= 1000 && samples) {
		printf("[DEBUG-ASR-PCM] frames:%u recog:%u samples:%u len:%d min:%d max:%d peak:%u avg_abs:%u dc:%d last_rs:%d last_score:%d\n",
		       frames, recog_calls, samples, len, min_sample, max_sample, peak,
		       (u32)(abs_sum / samples), (s32)(sample_sum / samples), last_rs, last_score);
		last_print_ms = now;
		frames = 0;
		recog_calls = 0;
		samples = 0;
		abs_sum = 0;
		sample_sum = 0;
		min_sample = 32767;
		max_sample = -32768;
		peak = 0;
	}
}
#endif

// Recognize speech using the ASR algorithm
int wanson_algorithm_recog(void *data, int len)
{
    const char *text;
    float score = 0.0f;
    int rs = 0;


    wdt_clear();

    if(tone_player_runing())
    {
        //如果正在播放提示音就返回
#if WANSON_ASR_PCM_DEBUG_ENABLE
		wanson_asr_pcm_debug(data, len, 0, 0, 0);
#endif
        return 0;
    }

    rs = Wanson_ASR_Recog(data, len, &text, &score); // 识别计算函数，有识别结果rs返回1
#if WANSON_ASR_PCM_DEBUG_ENABLE
	wanson_asr_pcm_debug(data, len, 1, rs, (int)score);
#endif
    if (rs == 1) {
        WANSON_LOG_I("ASR Result: %s", text); // Print the recognized text
        asr_deal(text);                       // Handle the recognized command
    }
    return 0; // Return success
}

int voice_qw_toolcall_deal(u16 evt)
{
    g_printf("=============>>%s: %d\n", __func__, evt);
    printf("----------------------------千问处理\n");

    switch (evt)
    {
    #if 1
    case QW_OP_ASS_OPEN:
        app_siri_open();
        break;
    case QW_OP_CALL_ANSWER:
        if (bt_phone_status_check()) {
            app_send_message(APP_MSG_CALL_ANSWER, 0);
        }
        break;
    case QW_OP_CALL_HANGUP:
        app_send_message(APP_MSG_CALL_HANGUP, 0);
        break;
    case QW_OP_VOL_UP:
        app_send_message(APP_MSG_VOL_UP, 0);
        break;
    case QW_OP_VOL_DOWN:
        app_send_message(APP_MSG_VOL_DOWN, 0);
        break;
    case QW_OP_VOL_MAX:

        break;
    case QW_OP_VOL_MID:

        break;
    case QW_OP_VOL_MIN:

        break;
    case QW_OP_MUSIC_PREV:
        app_send_message(APP_MSG_MUSIC_PREV, 0);
        set_wakeup_status(0);
        break;
    case QW_OP_MUSIC_NEXT:
        app_send_message(APP_MSG_MUSIC_NEXT, 0);
        set_wakeup_status(0);
        break;
    case QW_OP_MUSIC_PLAY:
        if (!bt_a2dp_status_check()) {
            app_send_message(APP_MSG_MUSIC_PP, 0);
            set_wakeup_status(0); // 控制播放后退出离线语音唤醒状态
        }
        break;
    case QW_OP_MUSIC_PAUSE:
        if (bt_a2dp_status_check()) {
            app_send_message(APP_MSG_MUSIC_PAUSE, 0);
        }
        break;
    case QW_OP_MODE_NS:
        voice_qw_multi_set(1);
        voice_tone_play(IDEX_TONE_NET_NS);
        break;
    case QW_OP_MODE_NOR:
        voice_qw_multi_set(1);
        voice_tone_play(IDEX_TONE_NET_NOR);
        break;
    case QW_OP_MODE_TRANS:
        voice_qw_multi_set(1);
        voice_tone_play(IDEX_TONE_NET_TRANS);
        break;
#endif
    default:
        break;
    }
}

#endif // End of TCFG_SMART_VOICE_ENABLE
