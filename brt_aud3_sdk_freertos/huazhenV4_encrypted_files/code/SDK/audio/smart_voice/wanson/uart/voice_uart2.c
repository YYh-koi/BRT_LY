
#include "debug.h"
#include "system/includes.h"
#include "uart.h"

#include "voice_uart.h"
#include "wanson/voice_config.h"
#include "app_at_dut.h"

#undef TAG
#define TAG "WANSON_UART2"

#if AT_DUT_DEBUG_LOG
#define UART2_DBG(fmt, ...) do { printf("[UART2-DBG] " fmt, ##__VA_ARGS__); } while (0)
#else
#define UART2_DBG(fmt, ...)
#endif

#if (TCFG_WANSON_AUDIO_OPUS_ENABLE)
#include "wanson/opus/opus_demo.h"
#endif

enum {
	UART_MSG_TX_DATA = 0,
	UART_MSG_RX_COMMAND,
};

static int uart_hd = -1;
static u8 uart_task_started;
static void *uart_rx_dma_buffer;
#define WANSON_UART2_CORE "wanson_uart2"
#define wanson_uart2_post_msg(num, ...) os_taskq_post_msg(WANSON_UART2_CORE, num, __VA_ARGS__)

static unsigned char tx_buffer[128];
static unsigned char rx_buffer[128];


#define VOICE_UART_FRAME_LEN	6
#define VOICE_UART_TX_DIRECTION	0x01
#define VOICE_UART_RX_DIRECTION	0x02

struct voice_uart_rx_parser {
	u8 frame[VOICE_UART_FRAME_LEN];
	u8 len;
};

static struct voice_uart_rx_parser rx_parser;

static void voice_uart2_rx_resync(void)
{
	for (u8 i = 1; i < VOICE_UART_FRAME_LEN; i++) {
		if (rx_parser.frame[i] != 0xAA) {
			continue;
		}
		if (i + 1 < VOICE_UART_FRAME_LEN && rx_parser.frame[i + 1] != 0x55) {
			continue;
		}
		rx_parser.len = VOICE_UART_FRAME_LEN - i;
		memmove(rx_parser.frame, &rx_parser.frame[i], rx_parser.len);
		return;
	}
	rx_parser.len = 0;
}

/*
 * 串口发送统一由识别结果映射层调用。保留旧接口是为了兼容提示音模块，
 * 避免提示音播放再次发送同一条语音命令。
 */
void tone_event_uart_send(unsigned int tone_index)
{
	(void)tone_index;
}

int wanson_uart2_send_command(unsigned char command)
{
	unsigned char frame[VOICE_UART_FRAME_LEN] = {
		0xAA,
		0x55,
		VOICE_UART_TX_DIRECTION,
		command,
		(unsigned char)(VOICE_UART_TX_DIRECTION + command),
		0xED,
	};

	WANSON_LOG_I("voice command 0x%02x TX:", command);
	printf_buf(frame, sizeof(frame));
	return wanson_uart2_send_data(frame, sizeof(frame));
}


static void voice_uart2_rx_byte(u8 data)
{
	if (rx_parser.len == 0) {
		if (data == 0xAA) {
			rx_parser.frame[rx_parser.len++] = data;
		}
		return;
	}

	if (rx_parser.len == 1) {
		if (data == 0x55) {
			rx_parser.frame[rx_parser.len++] = data;
		} else if (data != 0xAA) {
			rx_parser.len = 0;
		}
		return;
	}

	rx_parser.frame[rx_parser.len++] = data;
	if (rx_parser.len < VOICE_UART_FRAME_LEN) {
		return;
	}

	u8 valid = rx_parser.frame[2] == VOICE_UART_RX_DIRECTION &&
		   rx_parser.frame[3] <= VOICE_UART_CMD_MAX &&
		   rx_parser.frame[4] == (u8)(rx_parser.frame[2] + rx_parser.frame[3]) &&
		   rx_parser.frame[5] == 0xED;
	if (valid) {
		if (wanson_uart2_post_msg(2, UART_MSG_RX_COMMAND, rx_parser.frame[3]) != OS_NO_ERR) {
			WANSON_LOG_E("rx command queue full");
		}
		rx_parser.len = 0;
	} else {
		voice_uart2_rx_resync();
	}
}

/*
 * 串口接收参考 V3 产测串口的做法：中断里不读数据，只在任务里轮询
 * uart_recv_bytes。中断只处理 DMA 溢出复位。
 */
static void demo_uart_irq_func(uart_dev uart_num, enum uart_event event)
{
    UART2_DBG("irq event=0x%x\n", event);
    if (event & UART_EVENT_RX_FIFO_OVF) {
        UART2_DBG("RX_FIFO_OVF, reset dma rx\n");
        uart_dma_rx_reset(uart_num);
    }
}

static uart_dev dmeo_uart_init(void)
{
    UART2_DBG("dmeo_uart_init start, baud=115200 tx=PB4 rx=PB5\n");

    const struct uart_config config = {
        // .baud_rate = 2000000,
        // .baud_rate = 921600,
           .baud_rate = 115200,
		.tx_pin = IO_PORTB_04,
		.rx_pin = IO_PORTB_05,
        .parity = UART_PARITY_DISABLE,
        .tx_wait_mutex = 0, // 1:不支持中断调用,互斥,0:支持中断,不互斥
    };

    const int uart_rx_dma_buffer_size = 768;
    uart_rx_dma_buffer = dma_malloc(uart_rx_dma_buffer_size);
    if (!uart_rx_dma_buffer) {
        WANSON_LOG_E("uart rx dma buffer alloc failed");
        return -1;
    }
    void *uart_rx_ptr = uart_rx_dma_buffer;
    struct uart_dma_config dma = {
        .rx_timeout_thresh = 3 * 10000000 / config.baud_rate, // 单位:us,公式：3*10000000/baud(ot:3个byte时间)
        .frame_size = 8,                                      // 接收32字节就触发中断
        .event_mask = UART_EVENT_RX_DATA | UART_EVENT_RX_FIFO_OVF | UART_EVENT_TX_DONE | UART_EVENT_RX_TIMEOUT,
        .irq_priority = 3,
        .irq_callback = demo_uart_irq_func, // 在中断里面调用
        .rx_cbuffer = uart_rx_ptr,          // 使用支持dma的内存
        .rx_cbuffer_size = uart_rx_dma_buffer_size,
    };

    int r; 
    uart_dev uart_id = -1;
    int ut = uart_init(uart_id, &config);
    UART2_DBG("uart_init ret=%d\n", ut);
    if (ut < 0) {

        WANSON_LOG_E("uart(%d) init error\n", ut);
    } else {
        WANSON_LOG_I("uart(%d) init ok\n", ut);
    }
    r = uart_dma_init(ut, &dma);
    UART2_DBG("uart_dma_init ret=%d\n", r);
    if (r < 0) {
        WANSON_LOG_E("uart(%d) dma init error\n", ut);
    } else {
        WANSON_LOG_I("uart(%d) dma init ok\n", ut);
    }
    uart_dump();

    return ut;
} 
/// UART task for handling UART events

static void wanson_uart_task(void *priv)
{

	int msg[16];
	int res;
	int rx_len;
	u32 last_hb_jiffies;

	uart_hd = dmeo_uart_init();
	UART2_DBG("wanson uart task start, uart_hd=%d\n", uart_hd);
	last_hb_jiffies = jiffies;
 
	while (1) {
		/* 非阻塞处理队列消息，避免任务阻塞导致串口接收饿死 */
		res = os_taskq_accept(ARRAY_SIZE(msg), msg);
		if (res == OS_TASKQ) {
			switch (msg[1]) {
			case UART_MSG_RX_COMMAND:
				WANSON_LOG_I("uart[%d] rx command: 0x%02x", uart_hd, msg[2]);
				voice_mcu_command_handle((u8)msg[2]);
				break;
			case UART_MSG_TX_DATA:
				WANSON_LOG_I("uart[%d] tx data, len=%d", uart_hd, msg[3]);
				printf_buf((unsigned char *)msg[2], msg[3]);

				// 实际发送数据
				if (uart_hd >= 0) {
					int ret = uart_send_blocking(uart_hd, (unsigned char *)msg[2], msg[3], 100);
					if (ret < 0) {
						WANSON_LOG_E("uart send failed: %d", ret);
					} else {
						WANSON_LOG_I("uart send success: %d bytes", ret);
					}
				}
				dma_free((void *)msg[2]);
				break;
			default:
				break;
			}
		}

		/* 任务轮询接收（V3 产测串口同款），字节同时喂语音帧解析和 AT 行解析 */
		if (uart_hd >= 0) {
			rx_len = uart_recv_bytes(uart_hd, rx_buffer, sizeof(rx_buffer));
			if (rx_len > 0) {
				UART2_DBG("task poll rx len=%d\n", rx_len);
				for (int i = 0; i < rx_len; i++) {
					voice_uart2_rx_byte(rx_buffer[i]);
#if TCFG_AT_DUT_ENABLE
					at_dut_rx_byte(rx_buffer[i]);
#endif
				}
			} else {
				if (jiffies_offset_to_msec(last_hb_jiffies, jiffies) > 5000) {
					last_hb_jiffies = jiffies;
					UART2_DBG("poll heartbeat: task alive, rx idle\n");
				}
				os_time_dly(1);
			}
#if TCFG_AT_DUT_ENABLE
			at_dut_rx_idle_tick();
#endif
		} else {
			os_time_dly(1);
		}
	}
}
//===============================================================================
/// Initialize the UART task for Wanson
int wanson_uart2_init()
{
	if (uart_task_started) {
		return OS_NO_ERR;
	}

	int err = os_task_create(wanson_uart_task, NULL, 2, 512, 32, WANSON_UART2_CORE);
	if (err != OS_NO_ERR) {
		WANSON_LOG_E("creat fail %x\n", err);
	} else {
		uart_task_started = 1;
	}
	return err;
}

/// Deinitialize the UART for Wanson
int wanson_uart2_deinit()
{
	int err = OS_NO_ERR;

	if (!uart_task_started) {
		return err;
	}

	if (uart_hd >= 0) {
		err = uart_deinit(uart_hd);
		if (err != OS_NO_ERR) {
			WANSON_LOG_E("%s: uart_deinit fail %x\n", __func__, err);
		}
		uart_hd = -1;
	}

	int task_err = os_task_del(WANSON_UART2_CORE);
	if (task_err != OS_NO_ERR && task_err != OS_TASK_NOT_EXIST) {
		WANSON_LOG_E("%s: os_task_del fail %x\n", __func__, task_err);
		if (err == OS_NO_ERR) {
			err = task_err;
		}
	}
	uart_task_started = 0;

	if (uart_rx_dma_buffer) {
		dma_free(uart_rx_dma_buffer);
		uart_rx_dma_buffer = NULL;
	}
	return err;
}

// Send data over UART for Wanson
// Returns the number of bytes sent
int wanson_uart2_send_data(unsigned char *data, unsigned short len)
{
	if (uart_hd < 0 || !data || !len) {
		UART2_DBG("send_data reject: uart_hd=%d data=%p len=%u\n", uart_hd, data, len);
		return 0;
	}

	unsigned char *tx_data = dma_malloc(len);
	if (!tx_data) {
		WANSON_LOG_E("uart tx dma buffer alloc failed");
		UART2_DBG("send_data dma_malloc fail len=%u\n", len);
		return 0;
	}
	memcpy(tx_data, data, len);

	int post_ret = wanson_uart2_post_msg(3, UART_MSG_TX_DATA, tx_data, len);
	UART2_DBG("send_data post ret=%x len=%u\n", post_ret, len);
	if (post_ret == OS_NO_ERR) {
		return len;
	}

	dma_free(tx_data);
	return 0;
}

/*
 * 等待已入队的发送数据发完（AT+OTA 复位进升级前调用，
 * 避免 OK 回复还停留在发送队列里就被复位丢掉）。
 */
void wanson_uart2_flush(void)
{
	int wait = 0;

	UART2_DBG("flush start uart_hd=%d\n", uart_hd);
	if (uart_hd < 0) {
		return;
	}
	os_time_dly(1); /* 让 wanson 任务取出队列中的待发数据并启动发送 */
	while (!uart_is_send_complete(uart_hd) && wait++ < 200) {
		os_time_dly(1);
	}
	UART2_DBG("flush done wait=%d\n", wait);
}

/* 返回当前语音串口句柄：任务未起来或已退出时返回 -1 */
int wanson_uart2_handle_get(void)
{
	return uart_hd;
}
