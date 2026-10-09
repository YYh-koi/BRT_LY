#ifndef __CARLINK_CLCAST_BLE_AT_H
#define __CARLINK_CLCAST_BLE_AT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLCAST_BLE_MAX_DATA_SIZE 512

#define CLCAST_BLE_UUID_FFE1 0xFFE1
#define CLCAST_BLE_UUID_FFE2 0xFFE2
#define CLCAST_BLE_UUID_FFE3 0xFFE3

enum clcast_ble_driver_event_type
{
    CLCAST_BLE_DRIVER_EVENT_NONE = 0,
    CLCAST_BLE_DRIVER_EVENT_HANDLE,
    CLCAST_BLE_DRIVER_EVENT_DATA,
    CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE,
    CLCAST_BLE_DRIVER_EVENT_CONNECTED,
    CLCAST_BLE_DRIVER_EVENT_DISCONNECTED,
    CLCAST_BLE_DRIVER_EVENT_RESTARTED,
    CLCAST_BLE_DRIVER_EVENT_INVALID
};

struct clcast_ble_driver_event
{
    enum clcast_ble_driver_event_type type;
    union {
        struct {
            uint16_t uuid;
            uint16_t handle;
        } handle;
        struct {
            uint16_t length;
            uint8_t bytes[CLCAST_BLE_MAX_DATA_SIZE];
        } data;
        bool notify_enabled;
    } value;
};

enum clcast_ble_at_parse_result
{
    CLCAST_BLE_AT_INVALID = -1,
    CLCAST_BLE_AT_IGNORED = 0,
    CLCAST_BLE_AT_EVENT = 1
};

int carlink_clcast_ble_parse_at(
    const char *text, struct clcast_ble_driver_event *event);

#endif
