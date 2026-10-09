#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "carlink_video.h"
#include "mfcapi.h"
#include "jpegdecapi.h"
#include "lcd.h"
#include "pxp.h"
#include "cp15/cp15.h"
#include "timer.h"
#include "task.h"
#include "semphr.h"
#include "os_adapt.h"
#include "ff_stdio.h"
#if !DISABLE_CARLINK_H264_FRAME_BUF
static List_t								frame_free_list;
static List_t								frame_ready_list;
static video_frame_s						h264_frame_fifos[H264_FRAME_FIFO_COUNT];
static char*								h264_buf = NULL;
static SemaphoreHandle_t					frame_list_mutex = NULL;
static TaskHandle_t							dec_h264_handle;
static QueueHandle_t						h264_frame_queue;
static uint8_t								g_init_flag = 0;
static uint8_t								g_video_start = 0;
static int g_disp_x = 0, g_disp_y = 0, g_disp_width = LCD_WIDTH, g_disp_height = LCD_HEIGHT;
static int g_video_width = LCD_WIDTH, g_video_height = LCD_HEIGHT, g_video_fps = 30;
static int g_hide_carlink_flag = 0;

static int g_active_video_x = 0, g_active_video_y = 0;

void set_carlink_active_video_info(int x, int y)//for android auto
{
	g_active_video_x = x;
	g_active_video_y = y;
}


void set_carlink_video_info(int w, int h, int fps)
{
	g_video_width	= w;
	g_video_height	= h;
	g_video_fps		= fps;
}

int get_carlink_video_width()
{
	return g_video_width;
}

int get_carlink_video_height()
{
	return g_video_height;
}

int get_carlink_video_fps()
{
	return g_video_fps;
}

void set_carlink_display_info(int x, int y, int w, int h)
{
	g_disp_x		= x;
	g_disp_y		= y;
	g_disp_width	= w;
	g_disp_height	= h;
}

static void h264_frame_list_reset()
{
	video_frame_s *buffer;
	int i;

	buffer = (video_frame_s *)h264_frame_fifos;
	vListInitialise(&frame_free_list);
	vListInitialise(&frame_ready_list);
	for(i = 0; i < H264_FRAME_FIFO_COUNT; i++) {
		buffer->buf = (char *)(((unsigned int)h264_buf + i * H264_FRAME_BUF_SIZE));
		buffer->cur = buffer->buf;
		buffer->frame_id = i;
		buffer->len = 0;
		vListInitialiseItem(&buffer->entry);
		vListInsertEnd(&frame_free_list, &buffer->entry);
		listSET_LIST_ITEM_OWNER(&buffer->entry, buffer);
		buffer++;
	}
}

static int h264_frame_buf_init(void)
{
	h264_buf = pvPortMalloc(H264_FRAME_FIFO_COUNT * H264_FRAME_BUF_SIZE );
	if (h264_buf == NULL) {
		printf("malloc jpeg buf failed\r\n");
		return -1;
	}
	h264_frame_list_reset();

	return 0;
}

void h264_dec_ctx_init()
{
	g_video_start = 0;
	xQueueSend(h264_frame_queue, NULL, 0);
}

void set_carlink_display_state(int on)
{
	g_hide_carlink_flag = on;
	xQueueSend(h264_frame_queue, NULL, 0);
}


void set_carlink_display_state_irq(int on)
{
	g_hide_carlink_flag = on;

	xQueueSendFromISR(h264_frame_queue, NULL, 0);
}

video_frame_s* get_h264_frame_buf()
{
	video_frame_s* frame = NULL;
	ListItem_t* item = NULL;
	xSemaphoreTake(frame_list_mutex, portMAX_DELAY);
	if (g_video_start && !listLIST_IS_EMPTY(&frame_free_list)) {
		frame = list_first_entry(&frame_free_list);
		item = listGET_HEAD_ENTRY(&frame_free_list);
		uxListRemove(item);
	}
	xSemaphoreGive(frame_list_mutex);
	return frame;
}


void notify_h264_frame_ready(video_frame_s** frame)
{
	//ListItem_t* item = NULL;
	video_frame_s * tmp = *frame;
	xSemaphoreTake(frame_list_mutex, portMAX_DELAY);
	if (NULL != tmp) {
		vListInsertEnd(&frame_ready_list, &tmp->entry);
	}
	xSemaphoreGive(frame_list_mutex);

	xQueueSend(h264_frame_queue, NULL, 0);
}

void set_h264_frame_free(video_frame_s* frame)
{
	xSemaphoreTake(frame_list_mutex, portMAX_DELAY);
	if (g_video_start) {
		frame->cur = frame->buf;
		frame->len = 0;
		vListInsertEnd(&frame_free_list, &frame->entry);
	}
	xSemaphoreGive(frame_list_mutex);
}

static void h264_decode_proc(void *pvParameters)
{
	MFCHandle *handle = NULL;
	DWLLinearMem_t inBuf;
	OutFrameBuffer outBuf = {0};
	int ret = -1, i;
	uint32_t align_width, align_height;
	uint32_t yaddr, uaddr, vaddr, dstaddr;
	video_frame_s* frame = NULL;
	uint32_t ts = get_timer(0), show_ts = 0, video_ts = 0;
	int32_t delta_ts = 0;;
	int carlink_lcd_take = 0;
	ListItem_t* item = NULL;
	//FF_FILE *fp = ff_fopen("/usb/ey.h264", "w+");

	handle = mfc_init(RAW_STRM_TYPE_H264_NOREORDER);
	if(!handle) {
		printf("%s, mfc_init failed.\r\n", __func__);
		goto exit;
	}
	g_video_start = 1;
	while(1) {
	 if (xQueueReceive(h264_frame_queue, NULL, portMAX_DELAY) != pdPASS) {
		printf("%s xQueueReceive err!\r\n", __func__);
		continue;
		}
		if (g_video_start == 0) {
			printf("dec uninit\r\n");
			if (carlink_lcd_take) {
				ark_lcd_osd_enable(LCD_VIDEO_LAYER, 0);
				ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
				vVideoDisplayBufGive();
				ark_lcd_osd_enable(LCD_UI_LAYER, 1);
				ark_lcd_set_osd_sync(LCD_UI_LAYER);
				carlink_lcd_take = 0;
			}
			mfc_uninit(handle);
			xSemaphoreTake(frame_list_mutex, portMAX_DELAY);
			h264_frame_list_reset();
			xSemaphoreGive(frame_list_mutex);
			xQueueReset(h264_frame_queue);
			handle = mfc_init(RAW_STRM_TYPE_H264_NOREORDER);
			if(!handle) {
				printf("%s, mfc_reinit failed.\r\n", __func__);
				continue;
			}
			g_video_start = 1;
			continue;
		}

		if (g_hide_carlink_flag == 0) {
			ark_lcd_osd_enable(LCD_VIDEO_LAYER, 0);
			ark_lcd_osd_enable(LCD_UI_LAYER, 1);
			ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
			ark_lcd_set_osd_sync(LCD_UI_LAYER);
		}
		xSemaphoreTake(frame_list_mutex, portMAX_DELAY);
		if (!listLIST_IS_EMPTY(&frame_ready_list)) {
			frame = list_first_entry(&frame_ready_list);
			item = listGET_HEAD_ENTRY(&frame_ready_list);
			uxListRemove(item);
		} else {
			xSemaphoreGive(frame_list_mutex);
			continue;
		}
		xSemaphoreGive(frame_list_mutex);

		memset(&outBuf, 0, sizeof(OutFrameBuffer));
		inBuf.busAddress		= (u32)frame->cur;
		inBuf.virtualAddress	= (u32 *)inBuf.busAddress;
		inBuf.size				= frame->len;
		CP15_clean_dcache_for_dma(inBuf.busAddress, inBuf.busAddress + inBuf.size);
		ts = get_timer(0);
		//printf("start dec...\r\n");
		ret = mfc_decode(handle, &inBuf, &outBuf);
		//ret = 0;
		//if (NULL != fp)
		//	ret = ff_fwrite((void*)inBuf.virtualAddress, 1, inBuf.size, fp);

		xSemaphoreTake(frame_list_mutex, portMAX_DELAY);
		frame->cur = frame->buf;
		frame->len = 0;
		vListInsertEnd(&frame_free_list, &frame->entry);
		xSemaphoreGive(frame_list_mutex);

		if (ret < 0) {
			printf("h264 decode fail.\n");
			continue;
		}//printf("dec num:%d use:%d ms\r\n", outBuf.num, (get_timer(0) - ts) / 1000);
		//continue;

		for(i = 0; i < outBuf.num; i++) {
			int active_offset = 0;
			align_width = outBuf.frameWidth;
			align_height = outBuf.frameHeight;

			if(!(align_width && align_height))
				continue;
			active_offset = g_active_video_y * align_width;
			yaddr = outBuf.buffer[i].yBusAddress + active_offset + g_active_video_x;
			uaddr = outBuf.buffer[i].yBusAddress + align_width * align_height + active_offset / 2 + g_active_video_x;
			vaddr = 0;//yaddr + align_width * align_height * 5/4;

			if (carlink_lcd_take == 0) {
				carlink_lcd_take = 1;
				xVideoDisplayBufTake(portMAX_DELAY);
			}
			if (carlink_lcd_take) {
				LcdOsdInfo info = {0};

				if (0 == g_hide_carlink_flag)
					continue;

				show_ts = get_timer(0);

				info.format = LCD_OSD_FORAMT_Y_UV420;	//set default format.
				if ((info.format == LCD_OSD_FORAMT_RGB565) || \
					(g_disp_width != g_video_width) || (g_disp_height != g_video_height)) {
					uint32_t pxp_width, pxp_height;
					int pxp_out_fmt = PXP_OUT_FMT_YUV2P420;

					if(info.format == LCD_OSD_FORAMT_RGB565)
						pxp_out_fmt = PXP_OUT_FMT_RGB565;

					//align_width = ((outBuf.frameWidth + 0xf) & (~0xf));
					if (outBuf.frameWidth != g_video_width)
						align_width = g_video_width;
					else
						align_width = outBuf.frameWidth ;
					if (outBuf.frameHeight != g_video_height)
						align_height = g_video_height;
					else
						align_height = outBuf.frameHeight;

					pxp_width = g_disp_width;
					pxp_height = g_disp_height;
					dstaddr = ulVideoDisplayBufGet();

					pxp_scaler_rotate(yaddr, uaddr, vaddr, PXP_SRC_FMT_YUV2P420, align_width, align_height,
						dstaddr, dstaddr + pxp_width * pxp_height, pxp_out_fmt, pxp_width, pxp_height, LCD_ROTATE_ANGLE);
					info.yaddr = dstaddr;
					info.uaddr = dstaddr + pxp_width * pxp_height;
					info.vaddr = 0;
				} else {
					info.yaddr = yaddr;
					info.uaddr = uaddr;
					info.vaddr = vaddr;
				}
				info.x = g_disp_x;
				info.y = g_disp_y;
				info.width = g_disp_width;
				info.height = g_disp_height;
				if (g_active_video_x != 0)
					info.stride = outBuf.frameWidth;

				if (!ark_lcd_get_osd_info_atomic_isactive(LCD_VIDEO_LAYER)) {
					ark_lcd_wait_for_vsync();
				}

				ark_lcd_set_osd_info_atomic(LCD_VIDEO_LAYER, &info);
				ark_lcd_osd_enable(LCD_VIDEO_LAYER, 1);
				ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
				ark_lcd_osd_enable(LCD_UI_LAYER, 1);
				ark_lcd_set_osd_sync(LCD_UI_LAYER);
				vVideoDisplayBufRender(dstaddr);
				if (outBuf.num > 1) {// for apple h264
					video_ts = (1000 / g_video_fps);
					show_ts = (get_timer(0) - show_ts) / 1000;
					delta_ts =  ((int32_t)video_ts - (int32_t)show_ts) -3;
					//printf("delay %d ms\r\n", delta_ts);
					if (delta_ts > 0)
						vTaskDelay(pdMS_TO_TICKS(delta_ts));
				}
			}
		}
		printf("dec num:%d use:%d ms\r\n", outBuf.num, (get_timer(0) - ts) / 1000);
		//printf("all use:%d ms dec:%ld ms all ts:%ld ms\r\n",  (get_timer(0) - ts) / 1000, dec_ts, (get_timer(0) - all_ts) / 1000);
		//all_ts = get_timer(0);
	}

exit:
	if (NULL != handle)
		mfc_uninit(handle);

	vTaskDelete(NULL);
}

int carlink_ey_video_init()
{
	int ret = -1;

	if (g_init_flag ==1) {
		return 0;
	}
	g_init_flag = 1;
	ret = h264_frame_buf_init();
	frame_list_mutex = xSemaphoreCreateMutex();
	//h264_frame_queue = xQueueCreate(H264_FRAME_FIFO_COUNT, sizeof(uint32_t));
	h264_frame_queue = xQueueCreate(1, 0);
	ret = xTaskCreate(h264_decode_proc, "h264_decode_proc", 2048 * 4, NULL, configMAX_PRIORITIES - 3, &dec_h264_handle);
	return ret;
}

#else
static int g_disp_x = 0, g_disp_y = 0, g_disp_width = LCD_WIDTH, g_disp_height = LCD_HEIGHT;
static int g_video_width = LCD_WIDTH, g_video_height = LCD_HEIGHT, g_video_fps = 30;
static int g_hide_carlink_flag = 0;
//static int g_carlink_take_video = 0;
static int g_active_video_x = 0, g_active_video_y = 0;

void set_carlink_active_video_info(int x, int y)//for android auto
{
	g_active_video_x = x;
	g_active_video_y = y;
}


void set_carlink_video_info(int w, int h, int fps)
{
	g_video_width	= w;
	g_video_height	= h;
	g_video_fps		= fps;
}

int get_carlink_video_width()
{
	return g_video_width;
}

int get_carlink_video_height()
{
	return g_video_height;
}

int get_carlink_video_fps()
{
	return g_video_fps;
}

void set_carlink_display_info(int x, int y, int w, int h)
{
	g_disp_x		= x;
	g_disp_y		= y;
	g_disp_width	= w;
	g_disp_height	= h;
}


void h264_dec_ctx_init()
{
}

void set_carlink_display_state(int on)
{
	g_hide_carlink_flag = on;

	if (!on) {
		ark_lcd_osd_enable(LCD_OSD0, 0);
		ark_lcd_set_osd_sync(LCD_OSD0);
		ark_lcd_osd_enable(LCD_OSD1, 0);
		ark_lcd_set_osd_sync(LCD_OSD1);
	}
}


void set_carlink_display_state_irq(int on)
{
	set_carlink_display_state(on);
}


void* h264_video_player_init()
{
	MFCHandle *handle = NULL;
	
	handle = mfc_init(RAW_STRM_TYPE_H264_NOREORDER);
	if(!handle) {
		printf("%s, mfc_init failed.\r\n", __func__);
		return NULL;
	}

	g_hide_carlink_flag = 1;

	return (void*)handle;
}

void h264_video_player_uninit(void* h264_Handle)
{
	MFCHandle *handle = (MFCHandle*)h264_Handle;
			
	if (NULL != handle) {
		mfc_uninit(handle);
	}
	g_hide_carlink_flag = 0;
	ark_lcd_osd_enable(LCD_VIDEO_LAYER, 0);
	ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
	ark_lcd_osd_enable(LCD_UI_LAYER, 1);
	ark_lcd_set_osd_sync(LCD_UI_LAYER);
}

int h264_video_player_proc(void* h264_Handle, const char *h264_buf, int h264_buf_len)
{
	MFCHandle *handle = (MFCHandle*)h264_Handle;
	DWLLinearMem_t inBuf;
	OutFrameBuffer outBuf = {0};
	int ret = -1, i;
	uint32_t align_width, align_height;
	uint32_t yaddr, uaddr, vaddr, dstaddr = 0;
	uint32_t ts = get_timer(0), show_ts = 0, video_ts = 0;
	int32_t delta_ts = 0;

	if (NULL == h264_Handle) {
		return -1;
	}

	inBuf.busAddress		= (u32)h264_buf;
	inBuf.virtualAddress	= (u32 *)h264_buf;
	inBuf.size				= h264_buf_len;
	CP15_clean_dcache_for_dma(inBuf.busAddress, inBuf.busAddress + inBuf.size);
	ts = get_timer(0);
	ret = mfc_decode(handle, &inBuf, &outBuf);
	if (ret != 0) {
		printf("mfc_decode err.\n\n");
		return -1;
	}

	if (0 == g_hide_carlink_flag)
		return 0;

	/*if (xVideoDisplayBufTake(pdMS_TO_TICKS(50)) != pdPASS) {
		printf("xVideoDisplayBufTake failed!\r\n");
		return -1;
	}*/
	
	for(i = 0; i < outBuf.num; i++) {
		int active_offset = 0;
		align_width = outBuf.frameWidth;
		align_height = outBuf.frameHeight;

		if(!(align_width && align_height))
			continue;
		active_offset = g_active_video_y * align_width;
		yaddr = outBuf.buffer[i].yBusAddress + active_offset + g_active_video_x;
		uaddr = outBuf.buffer[i].yBusAddress + align_width * align_height + active_offset / 2 + g_active_video_x;
		vaddr = 0;

		LcdOsdInfo info = {0};
		show_ts = get_timer(0);

		info.format = LCD_OSD_FORAMT_Y_UV420;	//set default format.
		if ((info.format == LCD_OSD_FORAMT_RGB565) || \
			(g_disp_width != g_video_width) || (g_disp_height != g_video_height)) {
			uint32_t pxp_width, pxp_height;
			int pxp_out_fmt = PXP_OUT_FMT_YUV2P420;

			if(info.format == LCD_OSD_FORAMT_RGB565)
				pxp_out_fmt = PXP_OUT_FMT_RGB565;

			//align_width = ((outBuf.frameWidth + 0xf) & (~0xf));
			if (outBuf.frameWidth != g_video_width)
				align_width = g_video_width;
			else
				align_width = outBuf.frameWidth ;
			if (outBuf.frameHeight != g_video_height)
				align_height = g_video_height;
			else
				align_height = outBuf.frameHeight;

			pxp_width = g_disp_width;
			pxp_height = g_disp_height;
			dstaddr = ulVideoDisplayBufGet();

			pxp_scaler_rotate(yaddr, uaddr, vaddr, PXP_SRC_FMT_YUV2P420, align_width, align_height,
				dstaddr, dstaddr + pxp_width * pxp_height, pxp_out_fmt, pxp_width, pxp_height, LCD_ROTATE_ANGLE);
			info.yaddr = dstaddr;
			info.uaddr = dstaddr + pxp_width * pxp_height;
			info.vaddr = 0;
		} else {
			info.yaddr = yaddr;
			info.uaddr = uaddr;
			info.vaddr = vaddr;
		}
		info.x = g_disp_x;
		info.y = g_disp_y;
		info.width = g_disp_width;
		info.height = g_disp_height;
		if (g_active_video_x != 0)
			info.stride = outBuf.frameWidth;

		if (!ark_lcd_get_osd_info_atomic_isactive(LCD_VIDEO_LAYER)) {
			ark_lcd_wait_for_vsync();
		}

		ark_lcd_set_osd_info_atomic(LCD_VIDEO_LAYER, &info);
		ark_lcd_osd_enable(LCD_VIDEO_LAYER, 1);
		ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
		ark_lcd_osd_enable(LCD_UI_LAYER, 0);
		ark_lcd_set_osd_sync(LCD_UI_LAYER);
		if (dstaddr != 0)
			vVideoDisplayBufRender(dstaddr);
		if (outBuf.num > 1) {// for apple h264
			video_ts = (1000 / g_video_fps);
			show_ts = (get_timer(0) - show_ts) / 1000;
			delta_ts =  ((int32_t)video_ts - (int32_t)show_ts) -3;
			//printf("delay %d ms\r\n", delta_ts);
			if (delta_ts > 0)
				vTaskDelay(pdMS_TO_TICKS(delta_ts));
		}
	}
	//vVideoDisplayBufGive();
	return 0;
}

/* ============ CLCast 异步显示：解码与显示解耦 ============
 * 同步路径（h264_video_player_proc）在 sink 接收线程内解码+显示，
 * 高帧率下接收线程被显示拖住，且丢帧只能整 GOP 丢造成画面顿挫。
 * 异步路径把解码（接收线程）与显示（独立任务，vsync 挂屏）解耦：
 * - 所有帧都解码，H.264 参考链完整，画面不会花；
 * - 显示任务只挂最新已解码帧，来不及显示的旧帧自动丢弃，
 *   既防积压又不破坏参考链，无停顿跳变；
 * - 挂屏在 vsync 点，无撕裂。 */
#define CLCAST_DISP_BUF_NUM    3U
#define CLCAST_DISP_PERIOD_MS  16U

typedef struct {
	MFCHandle        *handle;
	uint32_t          buf_addr[CLCAST_DISP_BUF_NUM];
	volatile uint8_t  buf_state[CLCAST_DISP_BUF_NUM]; /* 0 空闲 1 就绪 2 显示中 */
	TaskHandle_t      task;
	volatile int      running;
	uint32_t          pxp_w;
	uint32_t          pxp_h;
	uint32_t          displayed;   /* 实际挂屏帧数 */
	uint32_t          dropped;     /* 真实丢弃帧数（覆盖旧就绪帧/缓冲耗尽） */
} clcast_disp_ctx_t;

static void clcast_display_task(void *param)
{
	clcast_disp_ctx_t *ctx = (clcast_disp_ctx_t *)param;
	LcdOsdInfo info = {0};
	TickType_t next_wake;
	int prev_idx = -1;
	int idx;
	int i;

	info.format = LCD_OSD_FORAMT_Y_UV420;
	info.x = g_disp_x;
	info.y = g_disp_y;
	info.width = g_disp_width;
	info.height = g_disp_height;
	next_wake = xTaskGetTickCount();

	while (ctx->running) {
		/* 固定节拍驱动：16ms 节拍（上限约 60fps），比 33ms 节拍把
		 * 帧到屏的相位延迟从平均 ~16ms 压到 ~8ms；节拍本身固定，
		 * 显示节奏稳定，不随解码波动。 */
		vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(CLCAST_DISP_PERIOD_MS));
		if (!ctx->running)
			break;

		/* 上一块显示缓冲：等硬件锁存完成（isactive==1 表示上一拍
		 * 写入的 OSD 配置已由 vsync 锁存切换走）才释放，避免 16ms
		 * 节拍下 PXP 复用先于硬件锁存造成短暂错帧。 */
		if (ark_lcd_get_osd_info_atomic_isactive(LCD_VIDEO_LAYER)) {
			taskENTER_CRITICAL();
			if (prev_idx >= 0 && ctx->buf_state[prev_idx] == 2U) {
				ctx->buf_state[prev_idx] = 0U;
				prev_idx = -1;
			}
			taskEXIT_CRITICAL();
		}

		/* 取最新一块就绪帧（扫描到最后一块 state==1，跳过旧帧） */
		idx = -1;
		taskENTER_CRITICAL();
		for (i = 0; i < CLCAST_DISP_BUF_NUM; i++) {
			if (ctx->buf_state[i] == 1U)
				idx = i;
		}
		if (idx >= 0)
			ctx->buf_state[idx] = 2U;
		taskEXIT_CRITICAL();

		if (g_hide_carlink_flag == 0) {
			ark_lcd_osd_enable(LCD_VIDEO_LAYER, 0);
			ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
			ark_lcd_osd_enable(LCD_UI_LAYER, 1);
			ark_lcd_set_osd_sync(LCD_UI_LAYER);
			taskENTER_CRITICAL();
			if (idx >= 0)
				ctx->buf_state[idx] = 0U;
			if (prev_idx >= 0 && ctx->buf_state[prev_idx] == 2U)
				ctx->buf_state[prev_idx] = 0U;
			prev_idx = -1;
			taskEXIT_CRITICAL();
			continue;
		}

		if (idx < 0)
			continue; /* 无新帧：保持当前画面 */

		/* 挂最新帧。撕裂由硬件 LCD_OSD_COEF_SYNC 锁存保证，
		 * 无需软件等 vsync（等待反而增加显示延迟）。 */
		info.yaddr = ctx->buf_addr[idx];
		info.uaddr = ctx->buf_addr[idx] + ctx->pxp_w * ctx->pxp_h;
		info.vaddr = 0;
		ark_lcd_set_osd_info_atomic(LCD_VIDEO_LAYER, &info);
		ark_lcd_osd_enable(LCD_VIDEO_LAYER, 1);
		ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
		ark_lcd_osd_enable(LCD_UI_LAYER, 0);
		ark_lcd_set_osd_sync(LCD_UI_LAYER);
		taskENTER_CRITICAL();
		ctx->displayed++;
		/* 上一块显示缓冲在下一拍由 isactive 门控释放 */
		prev_idx = idx;
		taskEXIT_CRITICAL();
	}
	ctx->task = NULL;
	vTaskDelete(NULL);
}

void* h264_video_player_init_async(void)
{
	clcast_disp_ctx_t *ctx;
	uint32_t buf_size;
	int i;

	ctx = (clcast_disp_ctx_t *)pvPortMalloc(sizeof(clcast_disp_ctx_t));
	if (ctx == NULL)
		return NULL;
	memset(ctx, 0, sizeof(clcast_disp_ctx_t));

	ctx->handle = mfc_init(RAW_STRM_TYPE_H264_NOREORDER);
	if (ctx->handle == NULL) {
		printf("%s, mfc_init failed.\r\n", __func__);
		vPortFree(ctx);
		return NULL;
	}
	ctx->pxp_w = (uint32_t)g_disp_width;
	ctx->pxp_h = (uint32_t)g_disp_height;
	buf_size = ctx->pxp_w * ctx->pxp_h * 3U / 2U;
	for (i = 0; i < CLCAST_DISP_BUF_NUM; i++) {
		ctx->buf_addr[i] = (uint32_t)pvPortMalloc(buf_size);
		if (ctx->buf_addr[i] == 0U) {
			printf("%s, display buf malloc failed.\r\n", __func__);
			h264_video_player_uninit_async(ctx);
			return NULL;
		}
	}
	ctx->running = 1;
	if (xTaskCreate(clcast_display_task, "clcast_disp", 2048, ctx,
		configMAX_PRIORITIES - 4, &ctx->task) != pdPASS) {
		printf("%s, task create failed.\r\n", __func__);
		h264_video_player_uninit_async(ctx);
		return NULL;
	}

	g_hide_carlink_flag = 1;
	return (void *)ctx;
}

int h264_video_player_submit_async(void *h264_Handle,
	const char *h264_buf, int h264_buf_len)
{
	clcast_disp_ctx_t *ctx = (clcast_disp_ctx_t *)h264_Handle;
	DWLLinearMem_t inBuf;
	OutFrameBuffer outBuf = {0};
	int ret;
	int i;
	int idx = -1;
	uint32_t align_width, align_height;
	uint32_t yaddr, uaddr, vaddr;

	if (ctx == NULL)
		return -1;

	/* 取一块可写缓冲：优先空闲块，其次覆盖就绪旧帧（丢旧保新）。
	 * 所有缓冲都在显示中/占用中则返回失败，由调用方丢弃该帧（不阻塞接收）。 */
	taskENTER_CRITICAL();
	for (i = 0; i < CLCAST_DISP_BUF_NUM; i++) {
		if (ctx->buf_state[i] == 0U) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		for (i = 0; i < CLCAST_DISP_BUF_NUM; i++) {
			if (ctx->buf_state[i] == 1U) {
				idx = i;
				break;
			}
		}
		if (idx >= 0)
			ctx->dropped++; /* 覆盖旧就绪帧：该帧来不及显示，真实丢弃 */
	}
	if (idx >= 0)
		ctx->buf_state[idx] = 3U; /* 占用中（解码/缩放） */
	taskEXIT_CRITICAL();
	if (idx < 0) {
		ctx->dropped++; /* 缓冲耗尽：来不及处理，丢弃该帧 */
		return -1;
	}

	inBuf.busAddress = (u32)h264_buf;
	inBuf.virtualAddress = (u32 *)h264_buf;
	inBuf.size = (u32)h264_buf_len;
	CP15_clean_dcache_for_dma(inBuf.busAddress, inBuf.busAddress + inBuf.size);
	ret = mfc_decode(ctx->handle, &inBuf, &outBuf);
	if (ret != 0) {
		taskENTER_CRITICAL();
		ctx->buf_state[idx] = 0U;
		taskEXIT_CRITICAL();
		return -1;
	}

	for (i = 0; i < outBuf.num; i++) {
		int active_offset;

		align_width = outBuf.frameWidth;
		align_height = outBuf.frameHeight;
		if (!(align_width && align_height))
			continue;
		active_offset = g_active_video_y * align_width;
		yaddr = outBuf.buffer[i].yBusAddress + active_offset + g_active_video_x;
		uaddr = outBuf.buffer[i].yBusAddress +
			align_width * align_height + active_offset / 2 + g_active_video_x;
		vaddr = 0;
		if (outBuf.frameWidth != g_video_width)
			align_width = g_video_width;
		else
			align_width = outBuf.frameWidth;
		if (outBuf.frameHeight != g_video_height)
			align_height = g_video_height;
		else
			align_height = outBuf.frameHeight;
		pxp_scaler_rotate(yaddr, uaddr, vaddr, PXP_SRC_FMT_YUV2P420,
			align_width, align_height,
			ctx->buf_addr[idx], ctx->buf_addr[idx] + ctx->pxp_w * ctx->pxp_h,
			PXP_OUT_FMT_YUV2P420, ctx->pxp_w, ctx->pxp_h, LCD_ROTATE_ANGLE);
	}

	taskENTER_CRITICAL();
	ctx->buf_state[idx] = 1U; /* 就绪待显示 */
	taskEXIT_CRITICAL();
	return 0;
}

unsigned int h264_video_player_get_dropped(void *h264_Handle)
{
	clcast_disp_ctx_t *ctx = (clcast_disp_ctx_t *)h264_Handle;

	if (ctx == NULL)
		return 0;
	return ctx->dropped;
}

unsigned int h264_video_player_get_displayed(void *h264_Handle)
{
	clcast_disp_ctx_t *ctx = (clcast_disp_ctx_t *)h264_Handle;

	if (ctx == NULL)
		return 0;
	return ctx->displayed;
}

void h264_video_player_uninit_async(void *h264_Handle)
{
	clcast_disp_ctx_t *ctx = (clcast_disp_ctx_t *)h264_Handle;
	int i;

	if (ctx == NULL)
		return;

	ctx->running = 0;
	while (ctx->task != NULL)
		vTaskDelay(pdMS_TO_TICKS(2));

	if (ctx->handle != NULL)
		mfc_uninit(ctx->handle);
	ark_lcd_osd_enable(LCD_VIDEO_LAYER, 0);
	ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
	ark_lcd_osd_enable(LCD_UI_LAYER, 1);
	ark_lcd_set_osd_sync(LCD_UI_LAYER);
	g_hide_carlink_flag = 0;

	for (i = 0; i < CLCAST_DISP_BUF_NUM; i++) {
		if (ctx->buf_addr[i] != 0U)
			vPortFree((void *)ctx->buf_addr[i]);
	}
	vPortFree(ctx);
}

int carlink_ey_video_init()
{
	return 0;
}

#endif

#define CARLINK_MJPEG_BUFFER_ALIGNMENT 32U
#define CARLINK_MJPEG_ALIGN_16(value) (((value) + 15U) & ~15U)
#define CARLINK_MJPEG_MAX_FRAME_SIZE ((size_t)LCD_WIDTH * LCD_HEIGHT)
#define CARLINK_MJPEG_YUV_BUFFER_SIZE \
	((size_t)CARLINK_MJPEG_ALIGN_16(LCD_WIDTH) * \
	CARLINK_MJPEG_ALIGN_16(LCD_HEIGHT) * 2U)
#define CARLINK_MJPEG_DISPLAY_WAIT_MS 50
#define CARLINK_MJPEG_DIAGNOSTIC_LIMIT 8U

struct carlink_mjpeg_player
{
	MFCHandle *mfc_handle;
	uint8_t *jpeg_allocation;
	uint8_t *jpeg_buffer;
	uint8_t *yuv_allocation;
	uint8_t *yuv_buffer;
	size_t jpeg_capacity;
	size_t yuv_capacity;
	uint16_t max_width;
	uint16_t max_height;
	unsigned int frame_count;
	unsigned int error_count;
};

static uint8_t *carlink_mjpeg_align_buffer(uint8_t *buffer)
{
	uintptr_t address = (uintptr_t)buffer;

	address = (address + CARLINK_MJPEG_BUFFER_ALIGNMENT - 1U) &
		~((uintptr_t)CARLINK_MJPEG_BUFFER_ALIGNMENT - 1U);
	return (uint8_t *)address;
}

static void carlink_mjpeg_log_error(struct carlink_mjpeg_player *player,
	const char *reason, int value)
{
	if (player->error_count >= CARLINK_MJPEG_DIAGNOSTIC_LIMIT) {
		return;
	}
	player->error_count++;
	printf("carlink mjpeg: %s value=%d count=%u\r\n", reason, value,
		player->error_count);
}

void* mjpeg_video_player_init(void)
{
	struct carlink_mjpeg_player *player;
	int video_width = get_carlink_video_width();
	int video_height = get_carlink_video_height();

	if (video_width <= 0 || video_height <= 0 ||
		video_width > LCD_WIDTH || video_height > LCD_HEIGHT ||
		g_disp_width <= 0 || g_disp_height <= 0 ||
		g_disp_x < 0 || g_disp_y < 0 ||
		g_disp_x + g_disp_width > LCD_WIDTH ||
		g_disp_y + g_disp_height > LCD_HEIGHT) {
		printf("carlink mjpeg: invalid video/display dimensions\r\n");
		return NULL;
	}

	player = (struct carlink_mjpeg_player *)pvPortMalloc(sizeof(*player));
	if (player == NULL) {
		printf("carlink mjpeg: player allocation failed\r\n");
		return NULL;
	}
	memset(player, 0, sizeof(*player));
	player->jpeg_capacity = CARLINK_MJPEG_MAX_FRAME_SIZE;
	player->yuv_capacity = CARLINK_MJPEG_YUV_BUFFER_SIZE;
	player->max_width = (uint16_t)video_width;
	player->max_height = (uint16_t)video_height;

	player->jpeg_allocation = (uint8_t *)pvPortMalloc(
		player->jpeg_capacity + CARLINK_MJPEG_BUFFER_ALIGNMENT - 1U);
	player->yuv_allocation = (uint8_t *)pvPortMalloc(
		player->yuv_capacity + CARLINK_MJPEG_BUFFER_ALIGNMENT - 1U);
	if (player->jpeg_allocation == NULL || player->yuv_allocation == NULL) {
		printf("carlink mjpeg: frame buffer allocation failed\r\n");
		mjpeg_video_player_uninit(player);
		return NULL;
	}
	player->jpeg_buffer = carlink_mjpeg_align_buffer(
		player->jpeg_allocation);
	player->yuv_buffer = carlink_mjpeg_align_buffer(
		player->yuv_allocation);
	player->mfc_handle = mfc_init(RAW_STRM_TYPE_JPEG);
	if (player->mfc_handle == NULL) {
		printf("carlink mjpeg: mfc_init failed\r\n");
		mjpeg_video_player_uninit(player);
		return NULL;
	}
	printf("carlink mjpeg: init max=%ux%u jpeg=%u yuv=%u\r\n",
		(unsigned int)player->max_width,
		(unsigned int)player->max_height,
		(unsigned int)player->jpeg_capacity,
		(unsigned int)player->yuv_capacity);
	return player;
}

void mjpeg_video_player_uninit(void* mjpeg_handle)
{
	struct carlink_mjpeg_player *player =
		(struct carlink_mjpeg_player *)mjpeg_handle;

	if (player != NULL) {
		if (player->mfc_handle != NULL) {
			mfc_uninit(player->mfc_handle);
		}
		if (player->jpeg_allocation != NULL) {
			vPortFree(player->jpeg_allocation);
		}
		if (player->yuv_allocation != NULL) {
			vPortFree(player->yuv_allocation);
		}
		vPortFree(player);
	}
	ark_lcd_osd_enable(LCD_VIDEO_LAYER, 0);
	ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
	ark_lcd_osd_enable(LCD_UI_LAYER, 1);
	ark_lcd_set_osd_sync(LCD_UI_LAYER);
}

int mjpeg_video_player_proc(void* mjpeg_handle,
	const uint8_t *jpeg_frame, size_t jpeg_length)
{
	struct carlink_mjpeg_player *player =
		(struct carlink_mjpeg_player *)mjpeg_handle;
	JpegHeaderInfo jpeg_info = {0};
	LcdOsdInfo display_info = {0};
	uint32_t display_address;
	int pxp_format;
	const char *format_name;
	int ret = -1;

	if (player == NULL) {
		return -1;
	}
	if (jpeg_frame == NULL || jpeg_length < 4 ||
		jpeg_length > player->jpeg_capacity) {
		carlink_mjpeg_log_error(player, "invalid frame length",
			(int)jpeg_length);
		return -1;
	}
	if (jpeg_frame[0] != 0xff || jpeg_frame[1] != 0xd8 ||
		jpeg_frame[jpeg_length - 2] != 0xff ||
		jpeg_frame[jpeg_length - 1] != 0xd9) {
		carlink_mjpeg_log_error(player, "invalid JPEG markers", 0);
		return -1;
	}

	memcpy(player->jpeg_buffer, jpeg_frame, jpeg_length);
	CP15_clean_dcache_for_dma((uint32_t)player->jpeg_buffer,
		(uint32_t)player->jpeg_buffer + (uint32_t)jpeg_length);
	if (xVideoDisplayBufTake(pdMS_TO_TICKS(
		CARLINK_MJPEG_DISPLAY_WAIT_MS)) != pdPASS) {
		carlink_mjpeg_log_error(player, "display buffer timeout", 0);
		return -1;
	}

	jpeg_info.handle = player->mfc_handle;
	jpeg_info.jpg_addr = (uint32_t)player->jpeg_buffer;
	jpeg_info.jpg_size = (uint32_t)jpeg_length;
	jpeg_info.dec_addry = (uint32_t)player->yuv_buffer;
	jpeg_info.dec_size = (uint32_t)player->yuv_capacity;
	ret = mfc_jpegdec(&jpeg_info);
	if (ret != 0) {
		carlink_mjpeg_log_error(player, "decode failed", ret);
		goto done;
	}
	if (jpeg_info.jpg_width == 0 || jpeg_info.jpg_height == 0 ||
		jpeg_info.jpg_width > player->max_width ||
		jpeg_info.jpg_height > player->max_height) {
		carlink_mjpeg_log_error(player, "unsupported dimensions",
			((int)jpeg_info.jpg_width << 16) | jpeg_info.jpg_height);
		ret = -1;
		goto done;
	}
	if (jpeg_info.dec_format == JPEGDEC_YCbCr420_SEMIPLANAR) {
		pxp_format = PXP_SRC_FMT_YUV2P420;
		format_name = "YUV420";
	} else if (jpeg_info.dec_format == JPEGDEC_YCbCr422_SEMIPLANAR) {
		pxp_format = PXP_SRC_FMT_YUV2P422;
		format_name = "YUV422";
	} else {
		carlink_mjpeg_log_error(player, "unsupported YUV format",
			(int)jpeg_info.dec_format);
		ret = -1;
		goto done;
	}

	display_address = ulVideoDisplayBufGet();
	if (display_address == 0) {
		carlink_mjpeg_log_error(player, "invalid display buffer", 0);
		ret = -1;
		goto done;
	}
	ret = pxp_scaler_rotate(jpeg_info.dec_addry, jpeg_info.dec_addru,
		jpeg_info.dec_addrv, pxp_format,
		jpeg_info.dec_width, jpeg_info.dec_height,
		display_address, 0, PXP_OUT_FMT_RGB565,
		g_disp_width, g_disp_height, LCD_ROTATE_ANGLE);
	if (ret != 0) {
		carlink_mjpeg_log_error(player, "PXP failed", ret);
		goto done;
	}

	display_info.x = g_disp_x;
	display_info.y = g_disp_y;
	display_info.width = g_disp_width;
	display_info.height = g_disp_height;
	display_info.format = LCD_OSD_FORAMT_RGB565;
	display_info.yaddr = display_address;
	if (!ark_lcd_get_osd_info_atomic_isactive(LCD_VIDEO_LAYER)) {
		ark_lcd_wait_for_vsync();
	}
	ark_lcd_set_osd_info_atomic(LCD_VIDEO_LAYER, &display_info);
	ark_lcd_osd_enable(LCD_VIDEO_LAYER, 1);
	ark_lcd_set_osd_sync(LCD_VIDEO_LAYER);
	ark_lcd_osd_enable(LCD_UI_LAYER, 0);
	ark_lcd_set_osd_sync(LCD_UI_LAYER);
	vVideoDisplayBufRender(display_address);
	player->frame_count++;
	if (player->frame_count <= CARLINK_MJPEG_DIAGNOSTIC_LIMIT) {
		printf("carlink mjpeg: frame=%u jpeg=%ux%u decode=%ux%u "
			"format=%s display=%dx%d\r\n",
			player->frame_count,
			(unsigned int)jpeg_info.jpg_width,
			(unsigned int)jpeg_info.jpg_height,
			(unsigned int)jpeg_info.dec_width,
			(unsigned int)jpeg_info.dec_height, format_name,
			g_disp_width, g_disp_height);
	}
	ret = 0;

done:
	vVideoDisplayBufGive();
	return ret;
}
