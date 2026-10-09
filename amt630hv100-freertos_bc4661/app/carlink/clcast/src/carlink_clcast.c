#include "os_adapt.h"
#include <FreeRTOS_POSIX.h>
#include <task.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include "board.h"
#include "carlink_clcast.h"
#include "carlink_clcast_ble.h"
#include "carlink_common.h"
#include "carlink_video.h"
#include "clcast_ble_first_connection.h"

#ifndef DEV_NAME_PREFIX
#define DEV_NAME_PREFIX   "AP630_CARLINK"
#endif

#if CARLINK_CLCAST
#include "clcast_sink.h"
#include "clcast_protocol.h"
#include "clcast_kv.h"
#include "clcast_utils.h"

#define printf_func   printf

#ifdef AWTK
extern void set_qr_text_buf(const char *str);
#endif

/* ========== 全局状态 ========== */
static bool mInited = false;
static bool g_clcast_disable = false;
static clcast_sink_server *g_clcast_server = NULL;
static clcast_sink_session_delegate g_clcast_delegate;
static void *g_clcast_video_handle = NULL;
static struct ICalinkEventCallbacks gCarlinkClcastEventCB;

#define CLCAST_PERF_LOG_INTERVAL_FRAMES 30U

/* CLCast 视频显示策略：解码与显示解耦（异步显示）。
 * 接收线程把每帧交给 h264_video_player_submit_async 全量解码，
 * H.264 参考链完整，画面不会花屏；独立显示任务只挂最新已解码帧，
 * 来不及显示的旧帧在缓冲池耗尽时自动丢弃——不阻塞接收、不破坏
 * 参考链，也没有整 GOP 丢弃导致的画面停顿顿挫。 */
struct clcast_video_perf_stats
{
	uint32_t total_frames;
	uint32_t window_frames;
	uint32_t window_bytes;
	uint32_t window_start_us;
	uint32_t rx_window_frames;      /* 窗口内实际到达帧数（含丢弃帧） */
	uint32_t last_rx_us;
	uint32_t max_gap_us;
	uint32_t max_proc_us;
	uint32_t last_proc_us;
	uint32_t dropped_frames;
	uint32_t last_displayed;
	uint64_t window_start_pts;
};

static struct clcast_video_perf_stats g_clcast_video_perf;

/* ========== 前向声明 ========== */
static void onEventClcast(void* ctx, const struct carlink_event *ev);

static void clcast_release_video(void)
{
	void *video_handle;

	taskENTER_CRITICAL();
	video_handle = g_clcast_video_handle;
	g_clcast_video_handle = NULL;
	taskEXIT_CRITICAL();

	if (video_handle != NULL) {
		h264_video_player_uninit_async(video_handle);
		set_carlink_video_info(LCD_WIDTH, LCD_HEIGHT, 30);
		set_carlink_display_info(0, 0, LCD_WIDTH, LCD_HEIGHT);
	}
}

static int clcast_generate_qr_text(char *qr_buf, size_t buf_size)
{
	int length;

	if (qr_buf == NULL || buf_size == 0) {
		return -1;
	}
	qr_buf[0] = '\0';

	{
		const char *ble_mac_source = carlink_get_ble_mac();
		char ble_mac[18];
		size_t index;

		if (CLCAST_APP_QR_TEMPLATE[0] == '\0' ||
			CLCAST_APP_DOWNLOAD_LINK[0] == '\0' ||
			CLCAST_PROJECT_CHANNEL[0] == '\0') {
			printf_func("clcast: BLE QR configuration missing "
				"(template/app link/channel)\n");
			return -1;
		}
		if (!clcast_ble_format_mac(ble_mac_source, ble_mac)) {
			printf_func("clcast: BLE QR MAC unavailable\n");
			return -1;
		}
		for (index = 0; ble_mac[index] != '\0'; index++) {
			if (ble_mac[index] >= 'a' && ble_mac[index] <= 'f') {
				ble_mac[index] = (char)(ble_mac[index] - 'a' + 'A');
			}
		}
		length = snprintf(qr_buf, buf_size, CLCAST_APP_QR_TEMPLATE,
			CLCAST_APP_DOWNLOAD_LINK, CLCAST_PROJECT_CHANNEL, ble_mac);
		if (length <= 0 || (size_t)length >= buf_size) {
			qr_buf[0] = '\0';
			printf_func("clcast: BLE QR output is invalid or truncated\n");
			return -1;
		}
		printf_func("clcast: BLE QR ready (ble=%s channel=%s)\n",
			ble_mac, CLCAST_PROJECT_CHANNEL);
	}
	return 0;
}

static int clcast_update_qr(void)
{
	char qr_text[200] = {0};
	int result = clcast_generate_qr_text(qr_text, sizeof(qr_text));

	if (result != 0) {
		qr_text[0] = '\0';
	}
#ifdef AWTK
	set_qr_text_buf(qr_text);
#endif
	return result;
}

/* ========== Delegate 回调: 会话生命周期 ========== */
static void clcast_session_create(clcast_sink_session *session)
{
	(void)session;
	printf_func("clcast: session create\n");
}

static void clcast_session_connected(clcast_sink_session *session)
{
	printf_func("clcast: session connected, device=%s, mac=%s\n",
		session->device_name, session->mac_address);
}

static void clcast_session_disconnected(clcast_sink_session *session)
{
	printf_func("clcast: session disconnected, device=%s\n",
		session->device_name);
	clcast_release_video();
}

static void clcast_session_destroy(clcast_sink_session *session)
{
	(void)session;
	clcast_release_video();
	printf_func("clcast: session destroy\n");
}

/* ========== Delegate 回调: getprop (车机能力声明) ========== */
static void clcast_session_getprop(clcast_sink_session *session,
	const char *key, clcast_kv_encoder_t *kv_encoder)
{
	(void)session;

	if (strcmp(key, CLCAST_KEY_DEVICE_NAME) == 0) {
		clcast_kv_put_str(kv_encoder, CLCAST_KEY_DEVICE_NAME,
			DEV_NAME_PREFIX);
	} else if (strcmp(key, CLCAST_KEY_DEVICE_MACADDR) == 0) {
		const char *wifi_mac = carlink_get_p2p_go_mac();
		clcast_kv_put_str(kv_encoder, CLCAST_KEY_DEVICE_MACADDR,
			wifi_mac ? wifi_mac : "00:00:00:00:00:00");
	} else if (strcmp(key, CLCAST_KEY_SCREEN_WIDTH) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_SCREEN_WIDTH,
			(uint32_t)CLCAST_VIDEO_WIDTH);
	} else if (strcmp(key, CLCAST_KEY_SCREEN_HEIGHT) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_SCREEN_HEIGHT,
			(uint32_t)CLCAST_VIDEO_HEIGHT);
	} else if (strcmp(key, CLCAST_KEY_SCREEN_DENSITY) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_SCREEN_DENSITY, 160);
	} else if (strcmp(key, CLCAST_KEY_VIDEO_MAX_FPS) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_VIDEO_MAX_FPS, 30);
	} else if (strcmp(key, CLCAST_KEY_AUDIO_MAIN_FORMAT) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_AUDIO_MAIN_FORMAT,
			STREAM_AUDIO_FORMAT_PCM_48KHZ_16BIT_STEREO |
			STREAM_AUDIO_FORMAT_PCM_48KHZ_16BIT_MONO);
	} else if (strcmp(key, CLCAST_KEY_AUDIO_AUX_FORMAT) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_AUDIO_AUX_FORMAT,
			STREAM_AUDIO_FORMAT_PCM_16KHZ_16BIT_MONO);
	} else if (strcmp(key, CLCAST_KEY_VIDEO_MAIN_FORMAT) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_VIDEO_MAIN_FORMAT,
			STREAM_VIDEO_FORMAT_H264);
	} else if (strcmp(key, CLCAST_KEY_VIDEO_MAP_FORMAT) == 0) {
		clcast_kv_put_u32(kv_encoder, CLCAST_KEY_VIDEO_MAP_FORMAT,
			STREAM_VIDEO_FORMAT_H264);
	}
}

/* CLCast video callbacks and diagnostics. */
static bool clcast_is_video_stream(CLCAST_STREAM_TYPE streamType)
{
	return streamType == STREAM_TYPE_VIDEO_MAIN ||
		streamType == STREAM_TYPE_VIDEO_MAP;
}

static void clcast_video_perf_reset(void)
{
	memset(&g_clcast_video_perf, 0, sizeof(g_clcast_video_perf));
}

static void clcast_video_start(clcast_sink_session *session,
	CLCAST_STREAM_TYPE streamType, uint32_t format)
{
	(void)session;
	printf_func("clcast: video_start type=%d, format=0x%x\n",
		(int)streamType, (unsigned int)format);

	if (!clcast_is_video_stream(streamType)) {
		return;
	}
	if (!STREAM_FORMAT_HAS_MASK(format, STREAM_VIDEO_FORMAT_H264)) {
		printf_func("clcast: unsupported video format=0x%x\n",
			(unsigned int)format);
		return;
	}
	clcast_video_perf_reset();
	clcast_release_video();
	set_carlink_video_info(CLCAST_VIDEO_WIDTH, CLCAST_VIDEO_HEIGHT, 30);
	set_carlink_display_info(
		(LCD_WIDTH - CLCAST_VIDEO_WIDTH) / 2,
		(LCD_HEIGHT - CLCAST_VIDEO_HEIGHT) / 2,
		CLCAST_VIDEO_WIDTH, CLCAST_VIDEO_HEIGHT);
	printf_func("[CLCAST-PERF] start video=%ux%u interval_frames=%u\n",
		(unsigned int)CLCAST_VIDEO_WIDTH,
		(unsigned int)CLCAST_VIDEO_HEIGHT,
		(unsigned int)CLCAST_PERF_LOG_INTERVAL_FRAMES);
	g_clcast_video_handle = h264_video_player_init_async();
	if (g_clcast_video_handle == NULL) {
		printf_func("clcast: H.264 player init failed\n");
	} else {
		printf_func("clcast: H.264 async video stream ready\n");
	}
}

static void clcast_video_frame(clcast_sink_session *session,
	CLCAST_STREAM_TYPE streamType,
	const uint8_t *frame, uint32_t len, uint64_t pts)
{
	uint32_t rx_us;
	uint32_t gap_us = 0;
	uint32_t proc_start_us;
	uint32_t proc_us;
	uint32_t window_us;
	uint32_t rx_fps_x10;
	uint32_t shw_fps_x10;
	uint32_t kbps;
	uint32_t pts_span;
	int proc_ret;

	(void)session;
	if (!clcast_is_video_stream(streamType)) {
		return;
	}
	if (g_clcast_video_handle == NULL) {
		printf_func("clcast: H.264 frame received before video_start\n");
		return;
	}

	rx_us = get_timer(0);
	g_clcast_video_perf.rx_window_frames++;
	if (g_clcast_video_perf.last_rx_us != 0U) {
		gap_us = rx_us - g_clcast_video_perf.last_rx_us;
		if (gap_us > g_clcast_video_perf.max_gap_us) {
			g_clcast_video_perf.max_gap_us = gap_us;
		}
	}
	g_clcast_video_perf.last_rx_us = rx_us;

	if (g_clcast_video_perf.window_frames == 0U) {
		g_clcast_video_perf.window_start_us = rx_us;
		g_clcast_video_perf.window_start_pts = pts;
	}
	g_clcast_video_perf.window_bytes += len;

	proc_start_us = get_timer(0);
	proc_ret = h264_video_player_submit_async(g_clcast_video_handle,
		(const char *)frame, (int)len);
	proc_us = get_timer(0) - proc_start_us;
	if (proc_us > g_clcast_video_perf.max_proc_us) {
		g_clcast_video_perf.max_proc_us = proc_us;
	}
	g_clcast_video_perf.last_proc_us = proc_us;

	if (proc_ret != 0) {
		/* 显示缓冲池耗尽（来不及处理）或解码失败：丢弃该帧，不阻塞接收 */
		g_clcast_video_perf.dropped_frames++;
		return;
	}
	g_clcast_video_perf.total_frames++;
	g_clcast_video_perf.window_frames++;

	if (g_clcast_video_perf.window_frames >=
		CLCAST_PERF_LOG_INTERVAL_FRAMES) {
		uint32_t disp_now;
		uint32_t disp_delta;

		window_us = get_timer(0) - g_clcast_video_perf.window_start_us;
		rx_fps_x10 = window_us == 0U ? 0U :
			(uint32_t)(((uint64_t)g_clcast_video_perf.rx_window_frames *
			10000000ULL) / window_us);
		/* shw 为真实挂屏帧率（显示任务实际挂屏数增量） */
		disp_now = h264_video_player_get_displayed(g_clcast_video_handle);
		disp_delta = disp_now - g_clcast_video_perf.last_displayed;
		g_clcast_video_perf.last_displayed = disp_now;
		shw_fps_x10 = window_us == 0U ? 0U :
			(uint32_t)(((uint64_t)disp_delta *
			10000000ULL) / window_us);
		kbps = window_us == 0U ? 0U :
			(uint32_t)(((uint64_t)g_clcast_video_perf.window_bytes *
			8000ULL) / window_us);
		pts_span = (uint32_t)(pts -
			g_clcast_video_perf.window_start_pts);
		printf_func("[CLCAST-PERF] frames=%u rx=%u shw=%u kbps=%u "
			"max_gap_ms=%u max_proc_ms=%u avg_proc_ms=%u "
			"pts_span=%u drop=%u\n",
			(unsigned int)g_clcast_video_perf.total_frames,
			(unsigned int)rx_fps_x10, (unsigned int)shw_fps_x10,
			(unsigned int)kbps,
			(unsigned int)(g_clcast_video_perf.max_gap_us / 1000U),
			(unsigned int)(g_clcast_video_perf.max_proc_us / 1000U),
			(unsigned int)g_clcast_video_perf.last_proc_us / 1000U,
			(unsigned int)pts_span,
			(unsigned int)h264_video_player_get_dropped(
				g_clcast_video_handle));
		g_clcast_video_perf.window_frames = 0U;
		g_clcast_video_perf.window_bytes = 0U;
		g_clcast_video_perf.rx_window_frames = 0U;
		g_clcast_video_perf.max_gap_us = 0U;
		g_clcast_video_perf.max_proc_us = 0U;
	}
}

static void clcast_video_stop(clcast_sink_session *session,
	CLCAST_STREAM_TYPE streamType)
{
	(void)session;
	printf_func("clcast: video_stop type=%d\n", (int)streamType);
	if (clcast_is_video_stream(streamType)) {
		printf_func("[CLCAST-PERF] stop total_frames=%u drop=%u\n",
			(unsigned int)g_clcast_video_perf.total_frames,
			(unsigned int)g_clcast_video_perf.dropped_frames);
		clcast_release_video();
	}
}

/* ========== Delegate 回调: 音频 ========== */
static void clcast_audio_start(clcast_sink_session *session,
	CLCAST_STREAM_TYPE streamType,
	int sampleRate, int format, int channel)
{
	(void)session;
	printf_func("clcast: audio_start type=%d, rate=%d, fmt=%d, ch=%d\n",
		(int)streamType, sampleRate, format, channel);
	/* TODO: 初始化 I2S/DAC 音频输出 */
}

static void clcast_audio_frame(clcast_sink_session *session,
	CLCAST_STREAM_TYPE streamType,
	const uint8_t *frame, uint32_t len, uint64_t pts)
{
	(void)session;
	(void)streamType;
	(void)frame;
	(void)len;
	(void)pts;
	/* TODO: PCM 数据送入音频播放缓冲 */
}

static void clcast_audio_stop(clcast_sink_session *session,
	CLCAST_STREAM_TYPE streamType)
{
	(void)session;
	printf_func("clcast: audio_stop type=%d\n", (int)streamType);
	/* TODO: 停止音频播放 */
}

/* ========== CLCast Server 运行任务 ========== */
static void clcast_server_task(void *param)
{
	clcast_sink_server *server = (clcast_sink_server *)param;

	printf_func("clcast: server runloop start on port %d\n",
		CLCAST_CONTROL_PORT);

	clcast_sink_runloop(server);

	printf_func("clcast: server runloop exit\n");
	vTaskDelete(NULL);
}

/* ========== CLCast 核心初始化 ========== */
static void* initClcastTiny(void* param)
{
	(void)param;

	if (mInited) {
		printf_func("clcast: already inited\n");
		return NULL;
	}

	printf_func("clcast: -------- init begin ----------\n");

	/* 1. 填充 delegate 回调 */
	memset(&g_clcast_delegate, 0, sizeof(g_clcast_delegate));
	g_clcast_delegate.create       = clcast_session_create;
	g_clcast_delegate.connected    = clcast_session_connected;
	g_clcast_delegate.disconnected = clcast_session_disconnected;
	g_clcast_delegate.destory      = clcast_session_destroy;
	g_clcast_delegate.getprop      = clcast_session_getprop;
	g_clcast_delegate.audio_start  = clcast_audio_start;
	g_clcast_delegate.audio_frame  = clcast_audio_frame;
	g_clcast_delegate.audio_stop   = clcast_audio_stop;
	g_clcast_delegate.video_start  = clcast_video_start;
	g_clcast_delegate.video_frame  = clcast_video_frame;
	g_clcast_delegate.video_stop   = clcast_video_stop;

	/* 2. 初始化网络 */
	printf_func("clcast: net_init\n");
	clcast_net_init();

	/* 3. 启动 Sink 服务器 */
	printf_func("clcast: sink_start port=%d\n", CLCAST_CONTROL_PORT);
	g_clcast_server = clcast_sink_start(CLCAST_CONTROL_PORT, g_clcast_delegate);
	if (g_clcast_server == NULL) {
		printf_func("clcast: ERROR: sink_start failed!\n");
		return NULL;
	}

	/* 4. 在独立任务中运行事件循环 */
	xTaskCreate(clcast_server_task, "clcast_srv", 2048 * 4,
		g_clcast_server, configMAX_PRIORITIES / 4, NULL);

	mInited = true;

	printf_func("clcast: -------- init end ----------\n");
	return NULL;
}

/* ========== 初始化线程 ========== */
static void clcastThread(void)
{
	int ret;

	ret = carlink_common_init();
	printf_func("clcast: common_init ret=%d\n", ret);
	gCarlinkClcastEventCB.onEvent = onEventClcast;
	gCarlinkClcastEventCB.rfcomm_data_read = NULL;
	carlink_register_event_callbacks(&gCarlinkClcastEventCB);

	ret = carlink_bt_wifi_init();
	printf_func("clcast: bt_wifi_init ret=%d\n", ret);
	if (ret == 0) {
		ret = carlink_clcast_ble_init();
		printf_func("clcast: ble_init ret=%d\n", ret);
	}
	clcast_update_qr();

	/* 注册事件回调 */
	initClcastTiny(NULL);
}

static void taskInitClcast(void* param)
{
	(void)param;
	clcastThread();
	vTaskDelete(NULL);
}

/* ========== 公共 API ========== */
int carlink_clcast_init(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf_func("clcast: ------ init start ------\n");

	/* ★ 复用 common 的视频解码管线（HX170DEC + 帧缓冲 + 解码任务） */
	carlink_ey_video_init();

	/* 在线程中初始化，避免阻塞 UI 启动 */
	xTaskCreate(taskInitClcast, "initThread", 2048 * 4, NULL,
		configMAX_PRIORITIES / 4, NULL);

	printf_func("clcast: ------ init dispatched ------\n");
	return 0;
}

void carlink_clcast_enable(int enable)
{
	if (enable) {
		if (g_clcast_disable) {
			/* TODO: 重新启动 CLCast server */
		}
		g_clcast_disable = false;
	} else {
		if (!g_clcast_disable) {
			if (g_clcast_server) {
				clcast_sink_stop(g_clcast_server);
				g_clcast_server = NULL;
				mInited = false;
			}
		}
		g_clcast_disable = true;
	}
}

/* ========== Carlink 事件回调 ========== */
static void onEventClcast(void* ctx, const struct carlink_event *ev)
{
	(void)ctx;

	if (ev == NULL) return;

	switch (ev->type) {
	case CARLINK_EVENT_BT_IDENTITY_READY:
		printf_func("clcast: event BT_IDENTITY_READY\n");
		clcast_update_qr();
		break;
	case CARLINK_EVENT_INIT_DONE:
		printf_func("clcast: event INIT_DONE\n");
		break;
	case CARLINK_EVENT_WIFI_CONNECT:
		printf_func("clcast: event WIFI_CONNECT\n");
		break;
	case CARLINK_EVENT_WIFI_DISCONNECT:
		printf_func("clcast: event WIFI_DISCONNECT\n");
		break;
	case CARLINK_EVENT_MSG_SESSION_CONNECT:
		printf_func("clcast: event SESSION_CONNECT\n");
		break;
	case CARLINK_EVENT_MSG_SESSION_STOP:
		printf_func("clcast: event SESSION_STOP\n");
		break;
	default:
		break;
	}
}

#else  /* !CARLINK_CLCAST */

int carlink_clcast_init(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

void carlink_clcast_enable(int enable)
{
	(void)enable;
}

#endif /* CARLINK_CLCAST */
