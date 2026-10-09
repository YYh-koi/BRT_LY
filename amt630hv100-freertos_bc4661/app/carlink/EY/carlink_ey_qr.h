#ifndef CARLINK_EY_QR_H
#define CARLINK_EY_QR_H

#include <stddef.h>

#define CARLINK_EY_QR_DIRECT_DEVICE_MAC "58C587F4293D"

const char *carlink_ey_qr_direct_device_mac(void);

int carlink_ey_build_qr_url(char *buffer, size_t buffer_size,
    const char *device_mac, const char *ssid, const char *password,
    const char *p2p_name);

#endif
