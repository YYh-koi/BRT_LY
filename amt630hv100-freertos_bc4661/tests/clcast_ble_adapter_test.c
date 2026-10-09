#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "carlink_clcast_ble_at.h"
#include "clcast_ble_adapter.h"
#include "clcast_ble_first_connection.h"

#define CAPTURED_COMMAND_COUNT 64
#define CAPTURED_COMMAND_SIZE  1100
#define CAPTURED_FRAME_COUNT   16

struct test_io {
    char commands[CAPTURED_COMMAND_COUNT][CAPTURED_COMMAND_SIZE];
    size_t command_count;
    uint8_t uplink[CLCAST_BLE_MAX_DATA_SIZE];
    size_t uplink_length;
};

struct frame_capture {
    uint8_t frames[CAPTURED_FRAME_COUNT]
        [CLCAST_BLE_FIRST_CONNECTION_MAX_FRAME_SIZE];
    size_t lengths[CAPTURED_FRAME_COUNT];
    unsigned int send_count;
    int send_result;
};

static int capture_frame(void *ctx, const uint8_t *frame, size_t length)
{
    struct frame_capture *capture = (struct frame_capture *)ctx;
    unsigned int frame_index = capture->send_count;

    assert(frame_index < CAPTURED_FRAME_COUNT);
    assert(length <= sizeof(capture->frames[frame_index]));
    memcpy(capture->frames[frame_index], frame, length);
    capture->lengths[frame_index] = length;
    capture->send_count++;
    return capture->send_result;
}

static void set_test_first_connection_params(
    struct clcast_ble_first_connection *connection, uint32_t interval_ms)
{
    static const char *values[] = {
        "dc:0d:10:23:f2:35",
        "AP630_CARLINK_F235",
        "1",
        "1024x600",
        "30",
        "0",
        "2c:cc:7a:74:42:7b",
        "AP630_CARLINK_F235",
        "88888888",
        "192.168.13.1",
        "9000",
        "0"
    };
    struct clcast_ble_first_connection_params params;
    size_t index;

    memset(&params, 0, sizeof(params));
    for (index = 0; index <
        CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT; index++) {
        params.values[index] = values[index];
    }
    params.frame_interval_ms = interval_ms;
    assert(clcast_ble_first_connection_set_params(
        connection, &params) == 0);
}

static void assert_parameter_frame(const struct frame_capture *capture,
    unsigned int index, uint8_t type, const char *value)
{
    const uint8_t *frame = capture->frames[index];
    size_t length = capture->lengths[index];
    size_t data_length = strlen(value);
    uint16_t crc;
    uint16_t wire_crc;

    assert(length == data_length + 6);
    assert(frame[0] == 0xff);
    assert(frame[1] == 0xfe);
    assert(frame[2] == data_length + 3);
    assert(frame[3] == type);
    assert(memcmp(&frame[4], value, data_length) == 0);
    crc = clcast_ble_crc16_modbus(frame, length - 2);
    wire_crc = (uint16_t)(frame[length - 2] |
        ((uint16_t)frame[length - 1] << 8));
    assert(crc == wire_crc);
}

static int capture_command(void *ctx, const char *command, size_t length)
{
    struct test_io *io = (struct test_io *)ctx;

    assert(io->command_count < CAPTURED_COMMAND_COUNT);
    assert(length < CAPTURED_COMMAND_SIZE);
    memcpy(io->commands[io->command_count], command, length);
    io->commands[io->command_count][length] = '\0';
    io->command_count++;
    return 0;
}

static void capture_uplink(void *ctx, const uint8_t *data, size_t length)
{
    struct test_io *io = (struct test_io *)ctx;

    assert(length <= sizeof(io->uplink));
    if (length != 0) {
        memcpy(io->uplink, data, length);
    }
    io->uplink_length = length;
}

static struct clcast_ble_driver_event handle_event(uint16_t uuid, uint16_t handle)
{
    struct clcast_ble_driver_event event;

    memset(&event, 0, sizeof(event));
    event.type = CLCAST_BLE_DRIVER_EVENT_HANDLE;
    event.value.handle.uuid = uuid;
    event.value.handle.handle = handle;
    return event;
}

static void test_at_parser(void)
{
    struct clcast_ble_driver_event event;
    char oversized[CLCAST_BLE_MAX_DATA_SIZE * 2 + 32];

    assert(carlink_clcast_ble_parse_at(
        "+GATTHANDLE=FFE3,0012\r\n", &event) == CLCAST_BLE_AT_EVENT);
    assert(event.type == CLCAST_BLE_DRIVER_EVENT_HANDLE);
    assert(event.value.handle.uuid == CLCAST_BLE_UUID_FFE3);
    assert(event.value.handle.handle == 0x0012);

    assert(carlink_clcast_ble_parse_at(
        "+CLCASTDATA=0102aB\r\n", &event) == CLCAST_BLE_AT_EVENT);
    assert(event.type == CLCAST_BLE_DRIVER_EVENT_DATA);
    assert(event.value.data.length == 3);
    assert(event.value.data.bytes[0] == 0x01);
    assert(event.value.data.bytes[1] == 0x02);
    assert(event.value.data.bytes[2] == 0xab);

    assert(carlink_clcast_ble_parse_at(
        "+CLCASTDATA=\r\n", &event) == CLCAST_BLE_AT_EVENT);
    assert(event.value.data.length == 0);
    assert(carlink_clcast_ble_parse_at(
        "+CLCASTDATA=123", &event) == CLCAST_BLE_AT_INVALID);
    assert(carlink_clcast_ble_parse_at(
        "+CLCASTDATA=12XZ", &event) == CLCAST_BLE_AT_INVALID);

    memset(oversized, '0', sizeof(oversized));
    memcpy(oversized, "+CLCASTDATA=", 12);
    oversized[12 + CLCAST_BLE_MAX_DATA_SIZE * 2 + 2] = '\0';
    assert(carlink_clcast_ble_parse_at(
        oversized, &event) == CLCAST_BLE_AT_INVALID);

    assert(carlink_clcast_ble_parse_at(
        "+CLCASTCCCD=1\r\n", &event) == CLCAST_BLE_AT_EVENT);
    assert(event.type == CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE);
    assert(event.value.notify_enabled);
    assert(carlink_clcast_ble_parse_at(
        "+CLCASTCCCD=0\r\n", &event) == CLCAST_BLE_AT_EVENT);
    assert(!event.value.notify_enabled);
    assert(carlink_clcast_ble_parse_at(
        "+CLCASTCCCD=2\r\n", &event) == CLCAST_BLE_AT_INVALID);

    assert(carlink_clcast_ble_parse_at(
        "+GATTSTAT=3,FF,5533FA6AEC66\r\n",
        &event) == CLCAST_BLE_AT_EVENT);
    assert(event.type == CLCAST_BLE_DRIVER_EVENT_CONNECTED);
    assert(carlink_clcast_ble_parse_at(
        "+GATTSTAT=1,FF,5533FA6AEC66\r\n",
        &event) == CLCAST_BLE_AT_EVENT);
    assert(event.type == CLCAST_BLE_DRIVER_EVENT_DISCONNECTED);
    assert(carlink_clcast_ble_parse_at(
        "+GATTSTAT=30", &event) == CLCAST_BLE_AT_IGNORED);
}

static void test_handle_query_and_notify(void)
{
    struct test_io test_io;
    struct clcast_ble_adapter adapter;
    struct clcast_ble_adapter_io io;
    struct clcast_ble_driver_event event;
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

    memset(&test_io, 0, sizeof(test_io));
    io.ctx = &test_io;
    io.send_command = capture_command;
    io.on_uplink = capture_uplink;
    clcast_ble_adapter_init(&adapter, &io);

    assert(clcast_ble_adapter_start(&adapter) == 0);
    assert(test_io.command_count == 3);
    assert(strcmp(test_io.commands[0], "AT+GATTHANDLE=FFE1\r\n") == 0);
    assert(strcmp(test_io.commands[1], "AT+GATTHANDLE=FFE2\r\n") == 0);
    assert(strcmp(test_io.commands[2], "AT+GATTHANDLE=FFE3\r\n") == 0);

    event = handle_event(CLCAST_BLE_UUID_FFE1, 0x0010);
    clcast_ble_adapter_handle_event(&adapter, &event);
    event = handle_event(CLCAST_BLE_UUID_FFE2, 0x0011);
    clcast_ble_adapter_handle_event(&adapter, &event);
    event = handle_event(CLCAST_BLE_UUID_FFE3, 0x0012);
    clcast_ble_adapter_handle_event(&adapter, &event);

    assert(clcast_ble_adapter_is_ready(&adapter));
    assert(test_io.command_count == 4);
    assert(strcmp(test_io.commands[3], "AT+GATTREADSET=\r\n") == 0);
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) < 0);

    memset(&event, 0, sizeof(event));
    event.type = CLCAST_BLE_DRIVER_EVENT_CONNECTED;
    clcast_ble_adapter_handle_event(&adapter, &event);
    event.type = CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE;
    event.value.notify_enabled = false;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) < 0);

    event.value.notify_enabled = true;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) == 0);
    assert(strcmp(test_io.commands[4],
        "AT+GATTDATASEND=0012,01020304\r\n") == 0);

    event.value.notify_enabled = false;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) < 0);

    event.type = CLCAST_BLE_DRIVER_EVENT_DISCONNECTED;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_is_ready(&adapter));
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) < 0);
}

static void test_retry_restart_and_uplink(void)
{
    struct test_io test_io;
    struct clcast_ble_adapter adapter;
    struct clcast_ble_adapter_io io;
    struct clcast_ble_driver_event event;
    unsigned int retry;

    memset(&test_io, 0, sizeof(test_io));
    io.ctx = &test_io;
    io.send_command = capture_command;
    io.on_uplink = capture_uplink;
    clcast_ble_adapter_init(&adapter, &io);

    assert(clcast_ble_adapter_start(&adapter) == 0);
    event = handle_event(CLCAST_BLE_UUID_FFE1, 0);
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_retry_missing_handles(&adapter) == 0);
    assert(strcmp(test_io.commands[3], "AT+GATTHANDLE=FFE1\r\n") == 0);

    for (retry = 2; retry < CLCAST_BLE_HANDLE_QUERY_MAX_ROUNDS; retry++) {
        assert(clcast_ble_adapter_retry_missing_handles(&adapter) == 0);
    }
    assert(clcast_ble_adapter_retry_missing_handles(&adapter) < 0);

    memset(&event, 0, sizeof(event));
    event.type = CLCAST_BLE_DRIVER_EVENT_DATA;
    event.value.data.length = 3;
    event.value.data.bytes[0] = 0xaa;
    event.value.data.bytes[1] = 0xbb;
    event.value.data.bytes[2] = 0xcc;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(test_io.uplink_length == 3);
    assert(memcmp(test_io.uplink, event.value.data.bytes, 3) == 0);

    event.type = CLCAST_BLE_DRIVER_EVENT_RESTARTED;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(!clcast_ble_adapter_is_ready(&adapter));
    assert(adapter.query_round == 1);
}

static void test_notify_state_before_connection(void)
{
    struct test_io test_io;
    struct clcast_ble_adapter adapter;
    struct clcast_ble_adapter_io io;
    struct clcast_ble_driver_event event;
    const uint8_t payload[] = {0x5a};

    memset(&test_io, 0, sizeof(test_io));
    io.ctx = &test_io;
    io.send_command = capture_command;
    io.on_uplink = capture_uplink;
    clcast_ble_adapter_init(&adapter, &io);
    assert(clcast_ble_adapter_start(&adapter) == 0);

    event = handle_event(CLCAST_BLE_UUID_FFE1, 0x0010);
    clcast_ble_adapter_handle_event(&adapter, &event);
    event = handle_event(CLCAST_BLE_UUID_FFE2, 0x0011);
    clcast_ble_adapter_handle_event(&adapter, &event);
    event = handle_event(CLCAST_BLE_UUID_FFE3, 0x0012);
    clcast_ble_adapter_handle_event(&adapter, &event);

    memset(&event, 0, sizeof(event));
    event.type = CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE;
    event.value.notify_enabled = true;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) < 0);

    event.type = CLCAST_BLE_DRIVER_EVENT_CONNECTED;
    clcast_ble_adapter_handle_event(&adapter, &event);
    assert(clcast_ble_adapter_is_ready(&adapter));
    assert(clcast_ble_adapter_notify(&adapter, payload, sizeof(payload)) == 0);
    assert(strcmp(test_io.commands[4],
        "AT+GATTDATASEND=0012,5A\r\n") == 0);
}

static void test_parameter_set_waits_for_transport_and_is_paced(void)
{
    static const char *expected_values[] = {
        "dc:0d:10:23:f2:35",
        "AP630_CARLINK_F235",
        "1",
        "1024x600",
        "30",
        "0",
        "2c:cc:7a:74:42:7b",
        "AP630_CARLINK_F235",
        "88888888",
        "192.168.13.1",
        "9000",
        "0"
    };
    struct clcast_ble_first_connection connection;
    struct clcast_ble_first_connection_io io;
    struct frame_capture capture;
    unsigned int index;

    memset(&capture, 0, sizeof(capture));
    io.ctx = &capture;
    io.send_frame = capture_frame;
    clcast_ble_first_connection_init(&connection, &io);
    set_test_first_connection_params(&connection, 100);

    assert(clcast_ble_first_connection_process(
        &connection, false, 0) == 0);
    assert(capture.send_count == 0);
    for (index = 0; index <
        CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT; index++) {
        uint32_t send_time = index * 100U;

        assert(clcast_ble_first_connection_process(
            &connection, true, send_time) == 1);
        assert(capture.send_count == index + 1);
        assert_parameter_frame(&capture, index,
            (uint8_t)(CLCAST_BLE_FIELD_B0 + index),
            expected_values[index]);
        if (index + 1 < CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT) {
            assert(clcast_ble_first_connection_process(
                &connection, true, send_time + 99U) == 0);
        }
    }
    assert(connection.completed);
    {
        static const uint8_t expected_video_format[] = {
            0xff, 0xfe, 0x04, 0xb5, 0x30, 0x36, 0xb9
        };
        unsigned int video_format_index =
            CLCAST_BLE_FIELD_B5 - CLCAST_BLE_FIELD_B0;

        assert(capture.lengths[video_format_index] ==
            sizeof(expected_video_format));
        assert(memcmp(capture.frames[video_format_index],
            expected_video_format, sizeof(expected_video_format)) == 0);
    }
    {
        static const uint8_t expected_sync_end[] = {
            0xff, 0xfe, 0x04, 0xbb, 0x30, 0x32, 0xd9
        };
        unsigned int sync_end_index =
            CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT - 1;

        assert(capture.lengths[sync_end_index] ==
            sizeof(expected_sync_end));
        assert(memcmp(capture.frames[sync_end_index], expected_sync_end,
            sizeof(expected_sync_end)) == 0);
    }
    assert(clcast_ble_first_connection_process(
        &connection, true, 2000) == 0);

    assert(clcast_ble_first_connection_process(
        &connection, false, 2001) == 0);
    assert(clcast_ble_first_connection_process(
        &connection, true, 2002) == 1);
    assert(capture.send_count ==
        CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT + 1);
    assert_parameter_frame(&capture,
        CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT,
        CLCAST_BLE_FIELD_B0, expected_values[0]);
}

static void test_crc16_modbus_documented_vector(void)
{
    static const uint8_t input[] = {
        0xff, 0xfe, 0x05, 0xb4, 0x32, 0x34
    };

    assert(clcast_ble_crc16_modbus(input, sizeof(input)) == 0x9d69);
}

static void test_mac_formatting(void)
{
    char formatted[18];

    assert(clcast_ble_format_mac("DC0D1023F235", formatted));
    assert(strcmp(formatted, "dc:0d:10:23:f2:35") == 0);
    assert(clcast_ble_format_mac("2C:CC:7A:74:42:7B", formatted));
    assert(strcmp(formatted, "2c:cc:7a:74:42:7b") == 0);
    assert(!clcast_ble_format_mac("DC0D1023F23", formatted));
    assert(!clcast_ble_format_mac("DC0D1023F23Z", formatted));
}

static void test_parameter_send_failure_stops_until_transport_resets(void)
{
    struct clcast_ble_first_connection connection;
    struct clcast_ble_first_connection_io io;
    struct frame_capture capture;

    memset(&capture, 0, sizeof(capture));
    capture.send_result = -1;
    io.ctx = &capture;
    io.send_frame = capture_frame;
    clcast_ble_first_connection_init(&connection, &io);
    set_test_first_connection_params(&connection, 100);

    assert(clcast_ble_first_connection_process(
        &connection, true, 0) < 0);
    assert(capture.send_count == 1);
    capture.send_result = 0;
    assert(clcast_ble_first_connection_process(
        &connection, true, 100) == 0);
    assert(capture.send_count == 1);

    assert(clcast_ble_first_connection_process(
        &connection, false, 101) == 0);
    assert(clcast_ble_first_connection_process(
        &connection, true, 102) == 1);
    assert(capture.send_count == 2);
}

int main(void)
{
    test_at_parser();
    test_handle_query_and_notify();
    test_retry_restart_and_uplink();
    test_notify_state_before_connection();
    test_parameter_set_waits_for_transport_and_is_paced();
    test_crc16_modbus_documented_vector();
    test_mac_formatting();
    test_parameter_send_failure_stops_until_transport_resets();
    printf("clcast_ble_adapter_test: PASS\n");
    return 0;
}
