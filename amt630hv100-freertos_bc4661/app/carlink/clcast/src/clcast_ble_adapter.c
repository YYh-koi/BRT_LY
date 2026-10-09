#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clcast_ble_adapter.h"

#define CLCAST_BLE_NOTIFY_COMMAND_SIZE \
    (CLCAST_BLE_MAX_DATA_SIZE * 2 + 40)

struct handle_query
{
    uint16_t uuid;
    const char *command;
};

static const struct handle_query handle_queries[] = {
    {CLCAST_BLE_UUID_FFE1, "AT+GATTHANDLE=FFE1\r\n"},
    {CLCAST_BLE_UUID_FFE2, "AT+GATTHANDLE=FFE2\r\n"},
    {CLCAST_BLE_UUID_FFE3, "AT+GATTHANDLE=FFE3\r\n"}
};

static uint16_t *get_handle_slot(
    struct clcast_ble_adapter *adapter, uint16_t uuid)
{
    switch (uuid) {
    case CLCAST_BLE_UUID_FFE1:
        return &adapter->handles.ffe1;
    case CLCAST_BLE_UUID_FFE2:
        return &adapter->handles.ffe2;
    case CLCAST_BLE_UUID_FFE3:
        return &adapter->handles.ffe3;
    default:
        return NULL;
    }
}

static bool all_handles_ready(const struct clcast_ble_adapter *adapter)
{
    return adapter->handles.ffe1 != 0 && adapter->handles.ffe2 != 0 &&
        adapter->handles.ffe3 != 0;
}

static int send_command(struct clcast_ble_adapter *adapter,
    const char *command, size_t length)
{
    if (adapter->io.send_command == NULL) {
        return -1;
    }
    return adapter->io.send_command(adapter->io.ctx, command, length);
}

static int send_missing_handle_queries(struct clcast_ble_adapter *adapter)
{
    size_t index;
    bool missing = false;

    if (adapter->query_round >= CLCAST_BLE_HANDLE_QUERY_MAX_ROUNDS) {
        return -1;
    }
    adapter->query_round++;
    for (index = 0; index < sizeof(handle_queries) / sizeof(handle_queries[0]);
        index++) {
        uint16_t *slot = get_handle_slot(adapter, handle_queries[index].uuid);

        if (slot != NULL && *slot == 0) {
            const char *command = handle_queries[index].command;

            missing = true;
            if (send_command(adapter, command, strlen(command)) < 0) {
                return -1;
            }
        }
    }
    if (!missing) {
        adapter->query_round--;
    }
    return 0;
}

static void update_ready_state(struct clcast_ble_adapter *adapter)
{
    static const char clear_read_cache[] = "AT+GATTREADSET=\r\n";

    if (!all_handles_ready(adapter)) {
        adapter->ready = false;
        return;
    }
    if (!adapter->read_cache_cleared) {
        if (send_command(adapter, clear_read_cache,
            sizeof(clear_read_cache) - 1) < 0) {
            adapter->ready = false;
            return;
        }
        adapter->read_cache_cleared = true;
    }
    adapter->ready = true;
}

void clcast_ble_adapter_init(struct clcast_ble_adapter *adapter,
    const struct clcast_ble_adapter_io *io)
{
    if (adapter == NULL) {
        return;
    }
    memset(adapter, 0, sizeof(*adapter));
    if (io != NULL) {
        adapter->io = *io;
    }
}

void clcast_ble_adapter_invalidate_handles(struct clcast_ble_adapter *adapter)
{
    if (adapter == NULL) {
        return;
    }
    memset(&adapter->handles, 0, sizeof(adapter->handles));
    adapter->query_round = 0;
    adapter->notify_enabled = false;
    adapter->read_cache_cleared = false;
    adapter->ready = false;
}

void clcast_ble_adapter_abort_exchange(struct clcast_ble_adapter *adapter)
{
    if (adapter == NULL) {
        return;
    }
    adapter->notify_enabled = false;
}

int clcast_ble_adapter_start(struct clcast_ble_adapter *adapter)
{
    if (adapter == NULL) {
        return -1;
    }
    clcast_ble_adapter_invalidate_handles(adapter);
    return send_missing_handle_queries(adapter);
}

int clcast_ble_adapter_retry_missing_handles(
    struct clcast_ble_adapter *adapter)
{
    if (adapter == NULL) {
        return -1;
    }
    if (all_handles_ready(adapter)) {
        if (adapter->ready) {
            return 0;
        }
        if (adapter->query_round >= CLCAST_BLE_HANDLE_QUERY_MAX_ROUNDS) {
            return -1;
        }
        adapter->query_round++;
        update_ready_state(adapter);
        return adapter->ready ? 0 : -1;
    }
    return send_missing_handle_queries(adapter);
}

void clcast_ble_adapter_handle_event(struct clcast_ble_adapter *adapter,
    const struct clcast_ble_driver_event *event)
{
    uint16_t *slot;

    if (adapter == NULL || event == NULL) {
        return;
    }
    switch (event->type) {
    case CLCAST_BLE_DRIVER_EVENT_HANDLE:
        slot = get_handle_slot(adapter, event->value.handle.uuid);
        if (slot != NULL) {
            *slot = event->value.handle.handle;
            update_ready_state(adapter);
        }
        break;
    case CLCAST_BLE_DRIVER_EVENT_DATA:
        if (event->value.data.length <= CLCAST_BLE_MAX_DATA_SIZE &&
            adapter->io.on_uplink != NULL) {
            adapter->io.on_uplink(adapter->io.ctx,
                event->value.data.bytes, event->value.data.length);
        }
        break;
    case CLCAST_BLE_DRIVER_EVENT_NOTIFY_STATE:
        adapter->notify_enabled = event->value.notify_enabled;
        break;
    case CLCAST_BLE_DRIVER_EVENT_CONNECTED:
        adapter->connected = true;
        break;
    case CLCAST_BLE_DRIVER_EVENT_DISCONNECTED:
        adapter->connected = false;
        adapter->notify_enabled = false;
        break;
    case CLCAST_BLE_DRIVER_EVENT_RESTARTED:
        adapter->connected = false;
        clcast_ble_adapter_start(adapter);
        break;
    default:
        break;
    }
}

bool clcast_ble_adapter_is_ready(const struct clcast_ble_adapter *adapter)
{
    return adapter != NULL && adapter->ready;
}

int clcast_ble_adapter_notify(struct clcast_ble_adapter *adapter,
    const uint8_t *data, size_t length)
{
    static const char prefix[] = "AT+GATTDATASEND=";
    static const char hex[] = "0123456789ABCDEF";
    char command[CLCAST_BLE_NOTIFY_COMMAND_SIZE];
    int prefix_length;
    size_t index;
    size_t command_length;

    if (adapter == NULL || !adapter->ready || !adapter->connected ||
        !adapter->notify_enabled || length > CLCAST_BLE_MAX_DATA_SIZE ||
        (length != 0 && data == NULL)) {
        return -1;
    }
    prefix_length = snprintf(command, sizeof(command), "%s%04X,", prefix,
        adapter->handles.ffe3);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(command)) {
        return -1;
    }
    command_length = (size_t)prefix_length;
    for (index = 0; index < length; index++) {
        command[command_length++] = hex[(data[index] >> 4) & 0x0f];
        command[command_length++] = hex[data[index] & 0x0f];
    }
    command[command_length++] = '\r';
    command[command_length++] = '\n';
    return send_command(adapter, command, command_length);
}
