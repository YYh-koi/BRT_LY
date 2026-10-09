#ifndef __CLCAST_BLE_FIRST_CONNECTION_H
#define __CLCAST_BLE_FIRST_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT       12
#define CLCAST_BLE_FIRST_CONNECTION_MAX_DATA_SIZE     64
#define CLCAST_BLE_FIRST_CONNECTION_MAX_FRAME_SIZE    \
    (CLCAST_BLE_FIRST_CONNECTION_MAX_DATA_SIZE + 6)

#define CLCAST_BLE_FIELD_B0 0xb0
#define CLCAST_BLE_FIELD_B5 0xb5
#define CLCAST_BLE_FIELD_BA 0xba
#define CLCAST_BLE_FIELD_BB 0xbb

typedef int (*clcast_ble_first_connection_send_fn)(
    void *ctx, const uint8_t *frame, size_t length);

struct clcast_ble_first_connection_io
{
    void *ctx;
    clcast_ble_first_connection_send_fn send_frame;
};

struct clcast_ble_first_connection_params
{
    const char *values[CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT];
    uint32_t frame_interval_ms;
};

struct clcast_ble_first_connection
{
    struct clcast_ble_first_connection_io io;
    char values[CLCAST_BLE_FIRST_CONNECTION_FIELD_COUNT]
        [CLCAST_BLE_FIRST_CONNECTION_MAX_DATA_SIZE + 1];
    uint32_t frame_interval_ms;
    uint32_t next_send_ms;
    uint8_t next_field;
    uint8_t last_sent_type;
    bool params_ready;
    bool active;
    bool completed;
    bool failed;
};

uint16_t clcast_ble_crc16_modbus(const uint8_t *data, size_t length);
bool clcast_ble_format_mac(const char *source, char output[18]);

void clcast_ble_first_connection_init(
    struct clcast_ble_first_connection *connection,
    const struct clcast_ble_first_connection_io *io);
int clcast_ble_first_connection_set_params(
    struct clcast_ble_first_connection *connection,
    const struct clcast_ble_first_connection_params *params);
bool clcast_ble_first_connection_has_params(
    const struct clcast_ble_first_connection *connection);

/* Returns 1 after sending a field, 0 when idle, and -1 on send failure. */
int clcast_ble_first_connection_process(
    struct clcast_ble_first_connection *connection,
    bool transport_ready, uint32_t now_ms);

#endif
