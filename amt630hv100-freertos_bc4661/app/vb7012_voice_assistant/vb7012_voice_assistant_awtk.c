#include "awtk.h"
#include <stdio.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "base/timer.h"
#include "widgets/label.h"
#include "widgets/view.h"

#include "board.h"
#include "vb7012_voice_assistant.h"
#include "vb7012_voice_assistant_awtk.h"

#ifdef VB7012_VOICE_ASSISTANT_ENABLE

#define VB7012_VOICE_ASSISTANT_AWTK_EVENT_QUEUE_SIZE  8

typedef struct {
    const char *text;
    unsigned int count;
} vb7012_voice_assistant_awtk_event_msg_t;

static widget_t *s_vb7012_voice_assistant_window;
static widget_t *s_vb7012_voice_assistant_label;
static volatile unsigned int s_vb7012_voice_assistant_event_count;
static QueueHandle_t s_vb7012_voice_assistant_event_queue;
static int s_vb7012_voice_assistant_started;
static int s_vb7012_voice_assistant_timer_started;

static const char *vb7012_voice_assistant_awtk_event_to_text(
    vb7012_voice_assistant_event_t event)
{
    switch (event) {
    case VB7012_VOICE_ASSISTANT_EVENT_WAKEUP:
        return "VB7012 Voice Assistant: WAKEUP";
    case VB7012_VOICE_ASSISTANT_EVENT_BYE:
        return "VB7012 Voice Assistant: BYE";
    case VB7012_VOICE_ASSISTANT_EVENT_OPEN_ASSISTANT:
        return "VB7012 Voice Assistant: OPEN_ASSISTANT";
    case VB7012_VOICE_ASSISTANT_EVENT_VOLUME_UP:
        return "VB7012 Voice Assistant: VOLUME_UP";
    case VB7012_VOICE_ASSISTANT_EVENT_VOLUME_DOWN:
        return "VB7012 Voice Assistant: VOLUME_DOWN";
    case VB7012_VOICE_ASSISTANT_EVENT_PREV_TRACK:
        return "VB7012 Voice Assistant: PREV_TRACK";
    case VB7012_VOICE_ASSISTANT_EVENT_NEXT_TRACK:
        return "VB7012 Voice Assistant: NEXT_TRACK";
    case VB7012_VOICE_ASSISTANT_EVENT_PLAY_MUSIC:
        return "VB7012 Voice Assistant: PLAY_MUSIC";
    case VB7012_VOICE_ASSISTANT_EVENT_PAUSE_MUSIC:
        return "VB7012 Voice Assistant: PAUSE_MUSIC";
    case VB7012_VOICE_ASSISTANT_EVENT_TCS_OFF:
        return "VB7012 Voice Assistant: TCS_OFF";
    case VB7012_VOICE_ASSISTANT_EVENT_TCS_ON:
        return "VB7012 Voice Assistant: TCS_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_HDC_OFF:
        return "VB7012 Voice Assistant: HDC_OFF";
    case VB7012_VOICE_ASSISTANT_EVENT_HDC_ON:
        return "VB7012 Voice Assistant: HDC_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_ANTI_ROLLBACK_OFF:
        return "VB7012 Voice Assistant: ANTI_ROLLBACK_OFF";
    case VB7012_VOICE_ASSISTANT_EVENT_ANTI_ROLLBACK_ON:
        return "VB7012 Voice Assistant: ANTI_ROLLBACK_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_SELF_CHECK:
        return "VB7012 Voice Assistant: VEHICLE_SELF_CHECK";
    case VB7012_VOICE_ASSISTANT_EVENT_REVERSE_ON:
        return "VB7012 Voice Assistant: REVERSE_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_PUSH_MODE_ON:
        return "VB7012 Voice Assistant: PUSH_MODE_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_REVERSE_OR_PUSH_OFF:
        return "VB7012 Voice Assistant: REVERSE_OR_PUSH_OFF";
    case VB7012_VOICE_ASSISTANT_EVENT_HEADLIGHT_ON:
        return "VB7012 Voice Assistant: HEADLIGHT_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_HEADLIGHT_OFF:
        return "VB7012 Voice Assistant: HEADLIGHT_OFF";
    case VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_ARM:
        return "VB7012 Voice Assistant: VEHICLE_ARM";
    case VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_DISARM:
        return "VB7012 Voice Assistant: VEHICLE_DISARM";
    case VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_POWER_ON:
        return "VB7012 Voice Assistant: VEHICLE_POWER_ON";
    case VB7012_VOICE_ASSISTANT_EVENT_VEHICLE_POWER_OFF:
        return "VB7012 Voice Assistant: VEHICLE_POWER_OFF";
    case VB7012_VOICE_ASSISTANT_EVENT_SEAT_BUCKET_OPEN:
        return "VB7012 Voice Assistant: SEAT_BUCKET_OPEN";
    default:
        return "VB7012 Voice Assistant: UNKNOWN";
    }
}

static void vb7012_voice_assistant_awtk_callback(vb7012_voice_assistant_event_t event)
{
    vb7012_voice_assistant_awtk_event_msg_t msg;
    vb7012_voice_assistant_awtk_event_msg_t drop_msg;

    s_vb7012_voice_assistant_event_count++;
    msg.text = vb7012_voice_assistant_awtk_event_to_text(event);
    msg.count = s_vb7012_voice_assistant_event_count;

    if (!s_vb7012_voice_assistant_event_queue)
        return;

    if (xQueueSend(s_vb7012_voice_assistant_event_queue, &msg, 0) != pdPASS) {
        printf("[VB7012_VOICE_ASSISTANT_AWTK] queue full, drop oldest before enqueue #%u %s\r\n",
            msg.count, msg.text);
        xQueueReceive(s_vb7012_voice_assistant_event_queue, &drop_msg, 0);
        xQueueSend(s_vb7012_voice_assistant_event_queue, &msg, 0);
    }

    printf("[VB7012_VOICE_ASSISTANT_AWTK] callback enqueue #%u %s waiting=%u\r\n",
        msg.count, msg.text,
        (unsigned int)uxQueueMessagesWaiting(s_vb7012_voice_assistant_event_queue));
}

static ret_t vb7012_voice_assistant_awtk_timer(const timer_info_t *info)
{
    vb7012_voice_assistant_awtk_event_msg_t msg;
    char show_text[80];

    (void)info;

    if (s_vb7012_voice_assistant_label && s_vb7012_voice_assistant_event_queue &&
        xQueueReceive(s_vb7012_voice_assistant_event_queue, &msg, 0) == pdPASS) {
        if (msg.text) {
            tk_snprintf(show_text, sizeof(show_text), "#%u %s", msg.count, msg.text);
            widget_set_text_utf8(s_vb7012_voice_assistant_label, show_text);
            printf("[VB7012_VOICE_ASSISTANT_AWTK] ui update %s waiting=%u\r\n",
                show_text,
                (unsigned int)uxQueueMessagesWaiting(s_vb7012_voice_assistant_event_queue));
        }
    }

    return RET_REPEAT;
}

void vb7012_voice_assistant_awtk_init(widget_t *parent)
{
    if (!parent)
        return;

    if (!s_vb7012_voice_assistant_window) {
        s_vb7012_voice_assistant_window = view_create(parent, 10, 10, 700, 86);
        if (s_vb7012_voice_assistant_window) {
            widget_set_style_color(s_vb7012_voice_assistant_window,
                "normal:bg_color", 0xff000000);
            widget_set_style_color(s_vb7012_voice_assistant_window,
                "normal:border_color", 0xffffffff);
            widget_set_style_str(s_vb7012_voice_assistant_window,
                "normal:border", "all");
        }
    }

    if (!s_vb7012_voice_assistant_label) {
        s_vb7012_voice_assistant_label = label_create(
            s_vb7012_voice_assistant_window ? s_vb7012_voice_assistant_window : parent,
            12, 8, 676, 70);
        if (s_vb7012_voice_assistant_label) {
            widget_set_style_int(s_vb7012_voice_assistant_label,
                "normal:font_size", 32);
            widget_set_style_color(s_vb7012_voice_assistant_label,
                "normal:text_color", 0xff0000ff);
            widget_set_style_str(s_vb7012_voice_assistant_label,
                "normal:text_align_h", "center");
            widget_set_style_str(s_vb7012_voice_assistant_label,
                "normal:text_align_v", "middle");
            widget_set_text_utf8(s_vb7012_voice_assistant_label,
                "VB7012 Voice Assistant: waiting");
        }
    }

    if (!s_vb7012_voice_assistant_event_queue) {
        s_vb7012_voice_assistant_event_queue = xQueueCreate(
            VB7012_VOICE_ASSISTANT_AWTK_EVENT_QUEUE_SIZE,
            sizeof(vb7012_voice_assistant_awtk_event_msg_t));
    }
    printf("[VB7012_VOICE_ASSISTANT_AWTK] init parent=0x%08x label=0x%08x queue=0x%08x\r\n",
        (unsigned int)parent, (unsigned int)s_vb7012_voice_assistant_label,
        (unsigned int)s_vb7012_voice_assistant_event_queue);

    if (!s_vb7012_voice_assistant_event_queue) {
        if (s_vb7012_voice_assistant_label) {
            widget_set_text_utf8(s_vb7012_voice_assistant_label,
                "VB7012 Voice Assistant: queue create failed");
        }
        printf("[VB7012_VOICE_ASSISTANT_AWTK] queue create failed\r\n");
        return;
    }

    if (!s_vb7012_voice_assistant_started) {
        vb7012_voice_assistant_register_callback(vb7012_voice_assistant_awtk_callback);
        if (vb7012_voice_assistant_start() != 0) {
            vb7012_voice_assistant_awtk_event_msg_t msg;

            s_vb7012_voice_assistant_event_count++;
            msg.text = "VB7012 Voice Assistant: start failed";
            msg.count = s_vb7012_voice_assistant_event_count;
            if (s_vb7012_voice_assistant_event_queue)
                xQueueSend(s_vb7012_voice_assistant_event_queue, &msg, 0);
            printf("[VB7012_VOICE_ASSISTANT_AWTK] voice start failed\r\n");
        } else {
            printf("[VB7012_VOICE_ASSISTANT_AWTK] voice start ok\r\n");
            s_vb7012_voice_assistant_started = 1;
        }
    }

    if (!s_vb7012_voice_assistant_timer_started) {
        timer_add(vb7012_voice_assistant_awtk_timer, NULL, 100);
        s_vb7012_voice_assistant_timer_started = 1;
        printf("[VB7012_VOICE_ASSISTANT_AWTK] timer start\r\n");
    }
}

#endif
