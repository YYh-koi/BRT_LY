#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "carlink_clcast_ble_at.h"

#define CLCAST_BLE_MAX_AT_LINE (CLCAST_BLE_MAX_DATA_SIZE * 2 + 32)

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int get_line_length(const char *text, size_t *length)
{
    size_t text_length = 0;

    while (text_length <= CLCAST_BLE_MAX_AT_LINE && text[text_length] != '\0') {
        text_length++;
    }
    if (text_length > CLCAST_BLE_MAX_AT_LINE) {
        return -1;
    }
    while (text_length != 0 &&
        (text[text_length - 1] == '\r' || text[text_length - 1] == '\n')) {
        text_length--;
    }
    *length = text_length;
    return 0;
}

static int parse_u16_hex(const char *text, size_t length, uint16_t *value)
{
    size_t index;
    uint16_t result = 0;

    if (length == 0 || length > 4) {
        return -1;
    }
    for (index = 0; index < length; index++) {
        int digit = hex_value(text[index]);

        if (digit < 0) {
            return -1;
        }
        result = (uint16_t)((result << 4) | (uint16_t)digit);
    }
    *value = result;
    return 0;
}

static int parse_handle_event(const char *text, size_t length,
    struct clcast_ble_driver_event *event)
{
    static const char prefix[] = "+GATTHANDLE=";
    const size_t prefix_length = sizeof(prefix) - 1;
    size_t handle_length;

    if (length < prefix_length ||
        memcmp(text, prefix, prefix_length) != 0) {
        return CLCAST_BLE_AT_IGNORED;
    }
    if (length < prefix_length + 6 || text[prefix_length + 4] != ',') {
        return CLCAST_BLE_AT_INVALID;
    }
    handle_length = length - prefix_length - 5;
    if (parse_u16_hex(text + prefix_length, 4,
        &event->value.handle.uuid) != 0 ||
        parse_u16_hex(text + prefix_length + 5, handle_length,
        &event->value.handle.handle) != 0) {
        return CLCAST_BLE_AT_INVALID;
    }
    event->type = CLCAST_BLE_DRIVER_EVENT_HANDLE;
    return CLCAST_BLE_AT_EVENT;
}

static int parse_data_event(const char *text, size_t length,
    struct clcast_ble_driver_event *event)
{
    static const char prefix[] = "+CLCASTDATA=";
    const size_t prefix_length = sizeof(prefix) - 1;
    size_t hex_length;
    size_t index;

    if (length < prefix_length ||
        memcmp(text, prefix, prefix_length) != 0) {
        return CLCAST_BLE_AT_IGNORED;
    }
    hex_length = length - prefix_length;
    if ((hex_length & 1U) != 0 ||
        hex_length > CLCAST_BLE_MAX_DATA_SIZE * 2U) {
        return CLCAST_BLE_AT_INVALID;
    }
    for (index = 0; index < hex_length / 2U; index++) {
        int high = hex_value(text[prefix_length + index * 2U]);
        int low = hex_value(text[prefix_length + index * 2U + 1U]);

        if (high < 0 || low < 0) {
            return CLCAST_BLE_AT_INVALID;
        }
        event->value.data.bytes[index] = (uint8_t)((high << 4) | low);
    }
    event->value.data.length = (uint16_t)(hex_length / 2U);
    event->type = CLCAST_BLE_DRIVER_EVENT_DATA;
    return CLCAST_BLE_AT_EVENT;
}

static int parse_notify_state_event(const char *text, size_t length,
    struct clcast_ble_driver_event *event)
{
    static const char prefix[] = "+CLCASTCCCD=";
    const size_t prefix_length = sizeof(prefix) - 1;

    if (length < prefix_length ||
        memcmp(text, prefix, prefix_length) != 0) {
        return CLCAST_BLE_AT_IGNORED;
    }
    if (length != prefix_length + 1 ||
        (text[prefix_length] != '0' && text[prefix_length] != '1')) {
        return CLCAST_BLE_AT_INVALID;
    }
    event->value.notify_enabled = text[prefix_length] == '1';
    event->type = CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE;
    return CLCAST_BLE_AT_EVENT;
}

int carlink_clcast_ble_parse_at(
    const char *text, struct clcast_ble_driver_event *event)
{
    size_t length;
    int result;

    if (text == NULL || event == NULL || get_line_length(text, &length) != 0) {
        return CLCAST_BLE_AT_INVALID;
    }
    memset(event, 0, sizeof(*event));

    result = parse_handle_event(text, length, event);
    if (result != CLCAST_BLE_AT_IGNORED) {
        return result;
    }
    result = parse_data_event(text, length, event);
    if (result != CLCAST_BLE_AT_IGNORED) {
        return result;
    }
    result = parse_notify_state_event(text, length, event);
    if (result != CLCAST_BLE_AT_IGNORED) {
        return result;
    }
    if (length >= 11 && memcmp(text, "+GATTSTAT=3", 11) == 0 &&
        (length == 11 || text[11] == ',')) {
        event->type = CLCAST_BLE_DRIVER_EVENT_CONNECTED;
        return CLCAST_BLE_AT_EVENT;
    }
    if (length >= 11 && memcmp(text, "+GATTSTAT=1", 11) == 0 &&
        (length == 11 || text[11] == ',')) {
        event->type = CLCAST_BLE_DRIVER_EVENT_DISCONNECTED;
        return CLCAST_BLE_AT_EVENT;
    }
    if (length >= 4 && memcmp(text, "+VER", 4) == 0) {
        event->type = CLCAST_BLE_DRIVER_EVENT_RESTARTED;
        return CLCAST_BLE_AT_EVENT;
    }
    return CLCAST_BLE_AT_IGNORED;
}
