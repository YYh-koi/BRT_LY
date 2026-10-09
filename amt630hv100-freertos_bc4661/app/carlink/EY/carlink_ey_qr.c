#include "carlink_ey_qr.h"

#include <stddef.h>

static int append_char(char *buffer, size_t buffer_size, size_t *offset,
    char value)
{
    if (*offset >= buffer_size - 1) {
        return -1;
    }
    buffer[*offset] = value;
    (*offset)++;
    return 0;
}

static int append_text(char *buffer, size_t buffer_size, size_t *offset,
    const char *text)
{
    while (*text != '\0') {
        if (append_char(buffer, buffer_size, offset, *text++) != 0) {
            return -1;
        }
    }
    return 0;
}

static int is_unreserved(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') || value == '-' || value == '.' ||
        value == '_' || value == '~';
}

static int append_encoded(char *buffer, size_t buffer_size, size_t *offset,
    const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned char value;

    while (*text != '\0') {
        value = (unsigned char)*text++;
        if (is_unreserved(value)) {
            if (append_char(buffer, buffer_size, offset, (char)value) != 0) {
                return -1;
            }
        } else {
            if (append_char(buffer, buffer_size, offset, '%') != 0 ||
                append_char(buffer, buffer_size, offset,
                    hex[value >> 4]) != 0 ||
                append_char(buffer, buffer_size, offset,
                    hex[value & 0x0f]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

const char *carlink_ey_qr_direct_device_mac(void)
{
    return CARLINK_EY_QR_DIRECT_DEVICE_MAC;
}

int carlink_ey_build_qr_url(char *buffer, size_t buffer_size,
    const char *device_mac, const char *ssid, const char *password,
    const char *p2p_name)
{
    size_t offset = 0;

    if (buffer != NULL && buffer_size != 0) {
        buffer[0] = '\0';
    }
    if (buffer == NULL || buffer_size == 0 || device_mac == NULL ||
        ssid == NULL || password == NULL || p2p_name == NULL ||
        *device_mac == '\0' || *ssid == '\0' || *password == '\0' ||
        *p2p_name == '\0') {
        return -1;
    }

    if (append_text(buffer, buffer_size, &offset,
            "https://appstore.eryanet.com?productid=f03aad9ac00ec5df"
            "&device=EY_") != 0 ||
        append_encoded(buffer, buffer_size, &offset, device_mac) != 0 ||
        append_text(buffer, buffer_size, &offset, "&ssid=") != 0 ||
        append_encoded(buffer, buffer_size, &offset, ssid) != 0 ||
        append_text(buffer, buffer_size, &offset, "&pwd=") != 0 ||
        append_encoded(buffer, buffer_size, &offset, password) != 0 ||
        append_text(buffer, buffer_size, &offset, "&name=") != 0 ||
        append_encoded(buffer, buffer_size, &offset, p2p_name) != 0 ||
        append_text(buffer, buffer_size, &offset, "&mac=") != 0 ||
        append_encoded(buffer, buffer_size, &offset, device_mac) != 0 ||
        append_text(buffer, buffer_size, &offset,
            "&IP=192.168.13.1&port1=11111&port2=11113"
            "&key=EYMOTOR12FCB99FD224DB2B22F5A7703&w=1024&h=600") != 0) {
        buffer[0] = '\0';
        return -1;
    }
    buffer[offset] = '\0';
    return 0;
}
