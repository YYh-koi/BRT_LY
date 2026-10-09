#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "carlink_clcast.h"
#include "carlink_common.h"
#include "carlink_clcast_ble.h"
#include "carlink_video.h"
#include "clcast_ble_adapter.h"
#include "clcast_ble_first_connection.h"

#if CARLINK_CLCAST

#define CLCAST_BLE_EVENT_QUEUE_DEPTH 4
#define CLCAST_BLE_HANDLE_RETRY_MS   500
#define CLCAST_BLE_TASK_POLL_MS      20

static struct clcast_ble_adapter g_clcast_ble_adapter;
static struct clcast_ble_first_connection g_clcast_ble_first_connection;
static QueueHandle_t g_clcast_ble_event_queue = NULL;
static StaticQueue_t g_clcast_ble_event_queue_buffer;
static uint8_t g_clcast_ble_event_queue_storage[
    CLCAST_BLE_EVENT_QUEUE_DEPTH * sizeof(struct clcast_ble_driver_event)];
static TaskHandle_t g_clcast_ble_task_handle = NULL;
static volatile bool g_clcast_ble_queue_overflow = false;
static unsigned int g_clcast_ble_queue_overflow_count = 0;
static unsigned int g_clcast_ble_invalid_event_count = 0;
static unsigned int g_clcast_ble_param_error_count = 0;

static int clcast_ble_send_command(
    void *ctx, const char *command, size_t length)
{
    (void)ctx;
    return carlink_clcast_ble_driver_send_at(command, length);
}

static int clcast_ble_send_frame(
    void *ctx, const uint8_t *frame, size_t length)
{
    return clcast_ble_adapter_notify(
        (struct clcast_ble_adapter *)ctx, frame, length);
}

static int clcast_ble_prepare_first_connection(void)
{
    struct clcast_ble_first_connection_params params;
    const char *bt_mac = carlink_get_ble_mac();
    const char *bt_name = carlink_get_bt_name();
    const char *wifi_mac = carlink_get_p2p_go_mac();
    const char *wifi_ssid = carlink_get_wifi_ssid();
    const char *wifi_password = carlink_get_wifi_passwd();
    char b0[18];
    char b3[24];
    char b6[18];
    char b9[16];
    char ba[8];
    char ip[4];
    int length;

    if (bt_mac == NULL || bt_name == NULL || wifi_mac == NULL ||
        wifi_ssid == NULL || wifi_ssid[0] == '\0' ||
        wifi_password == NULL || wifi_password[0] == '\0' ||
        !clcast_ble_format_mac(bt_mac, b0) ||
        !clcast_ble_format_mac(wifi_mac, b6)) {
        return -1;
    }
    strcpy(b0, "d4:d7:25:57:27:83");

    length = snprintf(b3, sizeof(b3), "%dx%d",
        CLCAST_VIDEO_WIDTH, CLCAST_VIDEO_HEIGHT);
    if (length <= 0 || (size_t)length >= sizeof(b3)) {
        return -1;
    }
    carlink_get_ap_ip_addr(ip);
    length = snprintf(b9, sizeof(b9), "%u.%u.%u.%u",
        (unsigned int)(uint8_t)ip[0], (unsigned int)(uint8_t)ip[1],
        (unsigned int)(uint8_t)ip[2], (unsigned int)(uint8_t)ip[3]);
    if (length <= 0 || (size_t)length >= sizeof(b9)) {
        return -1;
    }
    length = snprintf(ba, sizeof(ba), "%u",
        (unsigned int)CLCAST_CONTROL_PORT);
    if (length <= 0 || (size_t)length >= sizeof(ba)) {
        return -1;
    }

    memset(&params, 0, sizeof(params));
    params.values[0] = b0;
    params.values[1] = bt_name;
    params.values[2] = "1";
    params.values[3] = b3;
    params.values[4] = "30";
    params.values[5] = "0";
    params.values[6] = b6;
    params.values[7] = wifi_ssid;
    params.values[8] = wifi_password;
    params.values[9] = b9;
    params.values[10] = ba;
    params.values[11] = "0";
    params.frame_interval_ms = CLCAST_BLE_FRAME_INTERVAL_MS;
    return clcast_ble_first_connection_set_params(
        &g_clcast_ble_first_connection, &params);
}

static void clcast_ble_driver_event(
    void *ctx, const struct clcast_ble_driver_event *event)
{
    (void)ctx;
    if (g_clcast_ble_event_queue == NULL || event == NULL ||
        xQueueSend(g_clcast_ble_event_queue, event, 0) != pdPASS) {
        g_clcast_ble_queue_overflow = true;
    }
}

static void clcast_ble_task(void *param)
{
    struct clcast_ble_driver_event event;
    TickType_t last_handle_retry;

    (void)param;
    if (clcast_ble_adapter_start(&g_clcast_ble_adapter) != 0) {
        printf("clcast ble: initial handle query failed\r\n");
    }
    last_handle_retry = xTaskGetTickCount();

    for (;;) {
        if (xQueueReceive(g_clcast_ble_event_queue, &event,
            pdMS_TO_TICKS(CLCAST_BLE_TASK_POLL_MS)) == pdPASS) {
            if (event.type == CLCAST_BLE_DRIVER_EVENT_INVALID) {
                if (g_clcast_ble_invalid_event_count < 8) {
                    g_clcast_ble_invalid_event_count++;
                    printf("clcast ble: invalid AT event (%u)\r\n",
                        g_clcast_ble_invalid_event_count);
                }
            } else {
                clcast_ble_adapter_handle_event(&g_clcast_ble_adapter, &event);
            }
        }

        {
            TickType_t now_ticks = xTaskGetTickCount();

            if (!clcast_ble_adapter_is_ready(&g_clcast_ble_adapter) &&
                g_clcast_ble_adapter.query_round <
                CLCAST_BLE_HANDLE_QUERY_MAX_ROUNDS &&
                now_ticks - last_handle_retry >=
                pdMS_TO_TICKS(CLCAST_BLE_HANDLE_RETRY_MS)) {
                last_handle_retry = now_ticks;
                if (clcast_ble_adapter_retry_missing_handles(
                    &g_clcast_ble_adapter) != 0) {
                    printf("clcast ble: handle query retry failed\r\n");
                }
            }
        }

        if (g_clcast_ble_queue_overflow) {
            g_clcast_ble_queue_overflow = false;
            clcast_ble_adapter_abort_exchange(&g_clcast_ble_adapter);
            if (g_clcast_ble_queue_overflow_count < 8) {
                g_clcast_ble_queue_overflow_count++;
                printf("clcast ble: event queue overflow (%u)\r\n",
                    g_clcast_ble_queue_overflow_count);
            }
        }

        {
            bool transport_ready = g_clcast_ble_adapter.ready &&
                g_clcast_ble_adapter.connected &&
                g_clcast_ble_adapter.notify_enabled;
            uint32_t now_ms = (uint32_t)(xTaskGetTickCount() *
                portTICK_PERIOD_MS);
            int first_connection_result;

            if (transport_ready &&
                !clcast_ble_first_connection_has_params(
                    &g_clcast_ble_first_connection) &&
                clcast_ble_prepare_first_connection() != 0) {
                if (g_clcast_ble_param_error_count < 8) {
                    g_clcast_ble_param_error_count++;
                    printf("clcast ble: parameters not ready (%u)\r\n",
                        g_clcast_ble_param_error_count);
                }
            }
            first_connection_result = clcast_ble_first_connection_process(
                &g_clcast_ble_first_connection, transport_ready, now_ms);

            if (first_connection_result > 0) {
                if (g_clcast_ble_first_connection.completed) {
                    printf("clcast ble: parameter set sent\r\n");
                }
            } else if (first_connection_result < 0) {
                printf("clcast ble: parameter frame send failed\r\n");
            }
        }
    }
}

int carlink_clcast_ble_init(void)
{
    struct clcast_ble_adapter_io io;
    struct clcast_ble_first_connection_io first_connection_io;
    struct clcast_ble_driver_event initial_event;
    bool initially_connected;
    bool initially_notify_enabled;

    if (g_clcast_ble_task_handle != NULL) {
        return 0;
    }
    g_clcast_ble_event_queue = xQueueCreateStatic(
        CLCAST_BLE_EVENT_QUEUE_DEPTH,
        sizeof(struct clcast_ble_driver_event),
        g_clcast_ble_event_queue_storage,
        &g_clcast_ble_event_queue_buffer);
    if (g_clcast_ble_event_queue == NULL) {
        return -1;
    }

    memset(&io, 0, sizeof(io));
    io.send_command = clcast_ble_send_command;
    clcast_ble_adapter_init(&g_clcast_ble_adapter, &io);
    memset(&first_connection_io, 0, sizeof(first_connection_io));
    first_connection_io.ctx = &g_clcast_ble_adapter;
    first_connection_io.send_frame = clcast_ble_send_frame;
    clcast_ble_first_connection_init(&g_clcast_ble_first_connection,
        &first_connection_io);
    carlink_clcast_ble_register_driver_event_cb(NULL,
        clcast_ble_driver_event);
    carlink_clcast_ble_get_driver_state(&initially_connected,
        &initially_notify_enabled);
    memset(&initial_event, 0, sizeof(initial_event));
    if (initially_connected) {
        initial_event.type = CLCAST_BLE_DRIVER_EVENT_CONNECTED;
        clcast_ble_adapter_handle_event(&g_clcast_ble_adapter,
            &initial_event);
    }
    if (initially_notify_enabled) {
        initial_event.type = CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE;
        initial_event.value.notify_enabled = true;
        clcast_ble_adapter_handle_event(&g_clcast_ble_adapter,
            &initial_event);
    }

    if (xTaskCreate(clcast_ble_task, "clcast_ble", 2048, NULL,
        configMAX_PRIORITIES / 4, &g_clcast_ble_task_handle) != pdPASS) {
        carlink_clcast_ble_register_driver_event_cb(NULL, NULL);
        g_clcast_ble_event_queue = NULL;
        return -1;
    }
    return 0;
}

bool carlink_clcast_ble_is_ready(void)
{
    return clcast_ble_adapter_is_ready(&g_clcast_ble_adapter);
}

#else

int carlink_clcast_ble_init(void)
{
    return 0;
}

bool carlink_clcast_ble_is_ready(void)
{
    return false;
}

#endif
