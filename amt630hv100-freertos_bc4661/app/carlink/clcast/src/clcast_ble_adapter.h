#ifndef __CLCAST_BLE_ADAPTER_H
#define __CLCAST_BLE_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "carlink_clcast_ble_at.h"

#define CLCAST_BLE_HANDLE_QUERY_MAX_ROUNDS 10

typedef int (*clcast_ble_send_command_fn)(
    void *ctx, const char *command, size_t length);
typedef void (*clcast_ble_uplink_fn)(
    void *ctx, const uint8_t *data, size_t length);

struct clcast_ble_adapter_io
{
    void *ctx;
    clcast_ble_send_command_fn send_command;
    clcast_ble_uplink_fn on_uplink;
};

struct clcast_ble_handles
{
    uint16_t ffe1;
    uint16_t ffe2;
    uint16_t ffe3;
};

struct clcast_ble_adapter
{
    struct clcast_ble_adapter_io io;
    struct clcast_ble_handles handles;
    unsigned int query_round;
    bool connected;
    bool notify_enabled;
    bool read_cache_cleared;
    bool ready;
};

void clcast_ble_adapter_init(struct clcast_ble_adapter *adapter,
    const struct clcast_ble_adapter_io *io);
int clcast_ble_adapter_start(struct clcast_ble_adapter *adapter);
int clcast_ble_adapter_retry_missing_handles(
    struct clcast_ble_adapter *adapter);
void clcast_ble_adapter_handle_event(struct clcast_ble_adapter *adapter,
    const struct clcast_ble_driver_event *event);
void clcast_ble_adapter_invalidate_handles(struct clcast_ble_adapter *adapter);
void clcast_ble_adapter_abort_exchange(struct clcast_ble_adapter *adapter);
bool clcast_ble_adapter_is_ready(const struct clcast_ble_adapter *adapter);
int clcast_ble_adapter_notify(struct clcast_ble_adapter *adapter,
    const uint8_t *data, size_t length);

#endif
