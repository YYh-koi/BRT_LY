#include <stdio.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#ifndef __INLINE
#if defined(__ICCARM__)
#define __INLINE inline
#elif defined(__CC_ARM)
#define __INLINE __inline
#else
#define __INLINE inline
#endif
#endif

#include "uart.h"

#include "vb7012_voice_assistant.h"

#define VB7012_VOICE_ASSISTANT_UART_ID        UART_ID3
#define VB7012_VOICE_ASSISTANT_UART_BAUD      115200
#define VB7012_VOICE_ASSISTANT_READ_SIZE      32
#define VB7012_VOICE_ASSISTANT_TASK_STACK     (configMINIMAL_STACK_SIZE * 4)
#define VB7012_VOICE_ASSISTANT_TASK_PRIORITY  (configMAX_PRIORITIES / 3)

#define VB7012_FRAME_HEAD0    0x55
#define VB7012_FRAME_HEAD1    0xAA
#define VB7012_FRAME_TYPE     0x01

typedef enum {
    VB7012_PARSE_HEAD0 = 0,
    VB7012_PARSE_HEAD1,
    VB7012_PARSE_TYPE,
    VB7012_PARSE_CMD,
    VB7012_PARSE_CHECKSUM,
} vb7012_parse_state_t;

static UartPort_t *s_uart;
static TaskHandle_t s_task;
static vb7012_voice_assistant_callback_t s_callback;
static volatile int s_running;
static vb7012_parse_state_t s_parse_state;
static uint8_t s_command;

static void vb7012_voice_assistant_reset_parser(void)
{
    s_parse_state = VB7012_PARSE_HEAD0;
    s_command = 0;
}

static int vb7012_voice_assistant_command_to_event(uint8_t command,
    vb7012_voice_assistant_event_t *event)
{
    if (!event)
        return -1;

    switch (command) {
    case 0x00:
        *event = VB7012_VOICE_ASSISTANT_EVENT_WAKEUP;
        break;
    case 0x01:
        *event = VB7012_VOICE_ASSISTANT_EVENT_BYE;
        break;
    case 0x02:
        *event = VB7012_VOICE_ASSISTANT_EVENT_OPEN_ASSISTANT;
        break;
    case 0x03:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VOLUME_UP;
        break;
    case 0x04:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VOLUME_DOWN;
        break;
    case 0x05:
        *event = VB7012_VOICE_ASSISTANT_EVENT_PREV_TRACK;
        break;
    case 0x06:
        *event = VB7012_VOICE_ASSISTANT_EVENT_NEXT_TRACK;
        break;
    case 0x07:
        *event = VB7012_VOICE_ASSISTANT_EVENT_PLAY_MUSIC;
        break;
    case 0x08:
        *event = VB7012_VOICE_ASSISTANT_EVENT_PAUSE_MUSIC;
        break;
    case 0x09:
        *event = VB7012_VOICE_ASSISTANT_EVENT_TCS_OFF;
        break;
    case 0x0A:
        *event = VB7012_VOICE_ASSISTANT_EVENT_TCS_ON;
        break;
    case 0x0B:
        *event = VB7012_VOICE_ASSISTANT_EVENT_HDC_OFF;
        break;
    case 0x0C:
        *event = VB7012_VOICE_ASSISTANT_EVENT_HDC_ON;
        break;
    case 0x0D:
        *event = VB7012_VOICE_ASSISTANT_EVENT_ANTI_ROLLBACK_OFF;
        break;
    case 0x0E:
        *event = VB7012_VOICE_ASSISTANT_EVENT_ANTI_ROLLBACK_ON;
        break;
    case 0x0F:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_SELF_CHECK;
        break;
    case 0x10:
        *event = VB7012_VOICE_ASSISTANT_EVENT_REVERSE_ON;
        break;
    case 0x11:
        *event = VB7012_VOICE_ASSISTANT_EVENT_PUSH_MODE_ON;
        break;
    case 0x12:
        *event = VB7012_VOICE_ASSISTANT_EVENT_REVERSE_OR_PUSH_OFF;
        break;
    case 0x13:
        *event = VB7012_VOICE_ASSISTANT_EVENT_HEADLIGHT_ON;
        break;
    case 0x14:
        *event = VB7012_VOICE_ASSISTANT_EVENT_HEADLIGHT_OFF;
        break;
    case 0x15:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_ARM;
        break;
    case 0x16:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_DISARM;
        break;
    case 0x17:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_POWER_ON;
        break;
    case 0x18:
        *event = VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_POWER_OFF;
        break;
    case 0x19:
        *event = VB7012_VOICE_ASSISTANT_EVENT_SEAT_BUCKET_OPEN;
        break;
    default:
        return -1;
    }

    return 0;
}

static void vb7012_voice_assistant_dispatch(uint8_t command)
{
    vb7012_voice_assistant_event_t event;
    vb7012_voice_assistant_callback_t callback;

    if (vb7012_voice_assistant_command_to_event(command, &event) != 0) {
        printf("[VB7012_VOICE_ASSISTANT] unknown command:0x%02x\r\n", command);
        return;
    }

    callback = s_callback;
    printf("[VB7012_VOICE_ASSISTANT] dispatch cmd=0x%02x event=%d callback=0x%08x\r\n",
        command, event, (unsigned int)callback);
    if (callback)
        callback(event);
}

static void vb7012_voice_assistant_parse_byte(uint8_t data)
{
    switch (s_parse_state) {
    case VB7012_PARSE_HEAD0:
        if (data == VB7012_FRAME_HEAD0)
            s_parse_state = VB7012_PARSE_HEAD1;
        break;
    case VB7012_PARSE_HEAD1:
        if (data == VB7012_FRAME_HEAD1)
            s_parse_state = VB7012_PARSE_TYPE;
        else if (data != VB7012_FRAME_HEAD0)
            vb7012_voice_assistant_reset_parser();
        break;
    case VB7012_PARSE_TYPE:
        if (data == VB7012_FRAME_TYPE) {
            s_parse_state = VB7012_PARSE_CMD;
        } else {
            vb7012_voice_assistant_reset_parser();
            if (data == VB7012_FRAME_HEAD0)
                s_parse_state = VB7012_PARSE_HEAD1;
        }
        break;
    case VB7012_PARSE_CMD:
        s_command = data;
        s_parse_state = VB7012_PARSE_CHECKSUM;
        break;
    case VB7012_PARSE_CHECKSUM:
        if (data == (uint8_t)(VB7012_FRAME_TYPE + s_command))
            vb7012_voice_assistant_dispatch(s_command);
        else
            printf("[VB7012_VOICE_ASSISTANT] checksum error cmd=0x%02x checksum=0x%02x expected=0x%02x\r\n",
                s_command, data, (uint8_t)(VB7012_FRAME_TYPE + s_command));
        vb7012_voice_assistant_reset_parser();
        break;
    default:
        vb7012_voice_assistant_reset_parser();
        break;
    }
}

static void vb7012_voice_assistant_task(void *param)
{
    uint8_t rxbuf[VB7012_VOICE_ASSISTANT_READ_SIZE];
    int len;
    int i;

    (void)param;
    vb7012_voice_assistant_reset_parser();
    printf("[VB7012_VOICE_ASSISTANT] rx task start\r\n");

    while (s_running) {
        len = iUartRead(s_uart, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(20));
        if (len > 0) {
            printf("[VB7012_VOICE_ASSISTANT] read len=%d data:", len);
            for (i = 0; i < len; i++)
                printf(" %02x", rxbuf[i]);
            printf("\r\n");
        }
        for (i = 0; i < len; i++)
            vb7012_voice_assistant_parse_byte(rxbuf[i]);
    }

    printf("[VB7012_VOICE_ASSISTANT] rx task stop\r\n");
    vUartClose(s_uart);
    s_uart = NULL;
    s_task = NULL;
    vTaskDelete(NULL);
}

int vb7012_voice_assistant_init(void)
{
    if (s_uart)
        return 0;

    s_uart = xUartOpen(VB7012_VOICE_ASSISTANT_UART_ID);
    if (!s_uart) {
        printf("[VB7012_VOICE_ASSISTANT] open uart%d fail\r\n",
            VB7012_VOICE_ASSISTANT_UART_ID);
        return -1;
    }

    vUartInit(s_uart, VB7012_VOICE_ASSISTANT_UART_BAUD, 0);
    printf("[VB7012_VOICE_ASSISTANT] uart%d init baud=%d 8N1 flow=off\r\n",
        VB7012_VOICE_ASSISTANT_UART_ID, VB7012_VOICE_ASSISTANT_UART_BAUD);
    vb7012_voice_assistant_reset_parser();
    return 0;
}

int vb7012_voice_assistant_register_callback(vb7012_voice_assistant_callback_t callback)
{
    s_callback = callback;
    printf("[VB7012_VOICE_ASSISTANT] register callback=0x%08x\r\n",
        (unsigned int)callback);
    return 0;
}

int vb7012_voice_assistant_start(void)
{
    if (s_task)
        return 0;

    if (vb7012_voice_assistant_init() != 0)
        return -1;

    s_running = 1;
    if (xTaskCreate(vb7012_voice_assistant_task, "vb7012_voice_assistant",
        VB7012_VOICE_ASSISTANT_TASK_STACK, NULL,
        VB7012_VOICE_ASSISTANT_TASK_PRIORITY, &s_task) != pdPASS) {
        printf("[VB7012_VOICE_ASSISTANT] create rx task fail\r\n");
        s_running = 0;
        vUartClose(s_uart);
        s_uart = NULL;
        return -1;
    }

    printf("[VB7012_VOICE_ASSISTANT] start ok task=0x%08x\r\n",
        (unsigned int)s_task);
    return 0;
}

void vb7012_voice_assistant_stop(void)
{
    s_running = 0;
}
