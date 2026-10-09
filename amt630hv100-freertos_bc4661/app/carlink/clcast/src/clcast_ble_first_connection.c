#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "clcast_ble_first_connection.h"

#define CLCAST_BLE_FRAME_HEADER_1 0xff
#define CLCAST_BLE_FRAME_HEADER_2 0xfe

static int hex_to_lower(int value)
{
    if (value >= '0' && value <= '9') {
        return value;
    }
    if (value >= 'a' && value <= 'f') {
        return value;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 'a';
    }
    return -1;
}

uint16_t clcast_ble_crc16_modbus(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned int bit;

        crc ^= data[index];
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 1U) != 0) {
                crc = (uint16_t)((crc >> 1) ^ 0xa001U);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool clcast_ble_format_mac(const char *source, char output[18])
{
    size_t source_index = 0;
    size_t digit_index = 0;

    if (source == NULL || output == NULL) {
        return false;
    }
    while (source[source_index] != '\0') {
        int digit;

        if (source[source_index] == ':' || source[source_index] == '-') {
            source_index++;
            continue;
        }
        digit = hex_to_lower((unsigned char)source[source_index++]);
        if (digit < 0 || digit_index >= 12) {
            return false;
        }
        output[digit_index + digit_index / 2] = (char)digit;
        digit_index++;
        if ((digit_index & 1U) == 0 && digit_index < 12) {
            output[digit_index + digit_index / 2 - 1] = ':';
        }
    }
    if (digit_index != 12) {
        return false;
    }
    output[17] = '\0';
    return true;
}

static size_t encode_parameter_frame(uint8_t type, const char *value,
    uint8_t *frame, size_t capacity)
{
    size_t data_length;
    size_t crc_offset;
    uint16_t crc;

    if (value == NULL || frame == NULL) {
        return 0;
    }
    data_length = strlen(value);
    if (data_length > CLCAST_BLE_FIRST_CONNECTION_MAX_DATA_SIZE ||
        capacity < data_length + 6) {
        return 0;
    }
    frame[0] = CLCAST_BLE_FRAME_HEADER_1;
    frame[1] = CLCAST_BLE_FRAME_HEADER_2;
    frame[2] = (uint8_t)(data_length + 3);
    frame[3] = type;
    if (data_length != 0) {
        memcpy(&frame[4], value, data_length);
    }
    crc_offset = data_length + 4;
    crc = clcast_ble_crc16_modbus(frame, crc_offset);
    frame[crc_offset] = (uint8_t)(crc & 0xffU);
    frame[crc_offset + 1] = (uint8_t)((crc >> 8) & 0xffU);
    return data_length + 6;
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void reset_exchange(struct clcast_ble_first_connection *connection)
{
    connection->next_send_ms = 0;
    connection->next_field = 0;
    connection->last_sent_type = 0;
    connection->active = false;
    connection->completed = false;
    connection->failed = false;
}

void clcast_ble_first_connection_init(
    struct clcast_ble_first_connection *connection,
    const struct clcast_ble_first_connection_io *io)
{
    if (connection == NULL) {
        return;
    }
    memset(connection, 0, sizeof(*connection));
    if (io != NULL) {
        connection->io = *io;
    }
}

int clcast_ble_first_connection_set_params(
    struct clcast_ble_first_connection *connection,
    const struct clcast_ble_first_connection_params *params)
{
    size_t index;

    if (connection == NULL || params == NULL ||
        params->frame_interval_ms == 0) {
        return -1;
    }
    for (index = 0; index < CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT;
        index++) {
        size_t length;

        if (params->values[index] == NULL) {
            return -1;
        }
        length = strlen(params->values[index]);
        if (length == 0 ||
            length > CLCAST_BLE_FIRST_CONNECTION_MAX_DATA_SIZE) {
            return -1;
        }
    }
    for (index = 0; index < CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT;
        index++) {
        strcpy(connection->values[index], params->values[index]);
    }
    connection->frame_interval_ms = params->frame_interval_ms;
    connection->params_ready = true;
    reset_exchange(connection);
    return 0;
}

bool clcast_ble_first_connection_has_params(
    const struct clcast_ble_first_connection *connection)
{
    return connection != NULL && connection->params_ready;
}

int clcast_ble_first_connection_process(
    struct clcast_ble_first_connection *connection,
    bool transport_ready, uint32_t now_ms)
{
    uint8_t frame[CLCAST_BLE_FIRST_CONNECTION_MAX_FRAME_SIZE];
    uint8_t type;
    size_t length;

    if (connection == NULL) {
        return -1;
    }
    if (!transport_ready) {
        reset_exchange(connection);
        return 0;
    }
    if (!connection->params_ready || connection->completed ||
        connection->failed) {
        return 0;
    }
    if (!connection->active) {
        connection->active = true;
        connection->next_send_ms = now_ms;
    }
    if (!time_reached(now_ms, connection->next_send_ms)) {
        return 0;
    }

    type = (uint8_t)(CLCAST_BLE_FIELD_B0 + connection->next_field);
    length = encode_parameter_frame(type,
        connection->values[connection->next_field], frame, sizeof(frame));
    if (length == 0 || connection->io.send_frame == NULL ||
        connection->io.send_frame(connection->io.ctx, frame, length) < 0) {
        connection->failed = true;
        return -1;
    }
    connection->last_sent_type = type;
    connection->next_field++;
    if (connection->next_field >=
        CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT) {
        connection->completed = true;
    } else {
        connection->next_send_ms = now_ms + connection->frame_interval_ms;
    }
    return 1;
}
