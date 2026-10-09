#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "carlink_ey_qr.h"

static void test_builds_complete_url(void)
{
    char url[512];
    const char *expected =
        "https://appstore.eryanet.com?productid=f03aad9ac00ec5df"
        "&device=EY_A1B2C3D4E5F6&ssid=OfficeWifi&pwd=secret123"
        "&name=EY_AP&mac=A1B2C3D4E5F6&IP=192.168.13.1&port1=11111"
        "&port2=11113&key=EYMOTOR12FCB99FD224DB2B22F5A7703&w=1024&h=600";

    assert(carlink_ey_build_qr_url(url, sizeof(url), "A1B2C3D4E5F6",
        "OfficeWifi", "secret123", "EY_AP") == 0);
    assert(strcmp(url, expected) == 0);
}

static void test_rejects_empty_parameters(void)
{
    char url[512];

    strcpy(url, "sentinel");
    assert(carlink_ey_build_qr_url(url, sizeof(url), "", "OfficeWifi",
        "secret123", "EY_AP") == -1);
    assert(url[0] == '\0');
    strcpy(url, "sentinel");
    assert(carlink_ey_build_qr_url(url, sizeof(url), "A1B2C3D4E5F6", "",
        "secret123", "EY_AP") == -1);
    assert(url[0] == '\0');
    strcpy(url, "sentinel");
    assert(carlink_ey_build_qr_url(url, sizeof(url), "A1B2C3D4E5F6",
        "OfficeWifi", "", "EY_AP") == -1);
    assert(url[0] == '\0');
    strcpy(url, "sentinel");
    assert(carlink_ey_build_qr_url(url, sizeof(url), "A1B2C3D4E5F6",
        "OfficeWifi", "secret123", "") == -1);
    assert(url[0] == '\0');
}

static void test_percent_encodes_reserved_characters(void)
{
    char url[512];
    const char *expected =
        "https://appstore.eryanet.com?productid=f03aad9ac00ec5df"
        "&device=EY_A%26B%3DC%23D%25E%2BF&ssid=Office%20WiFi%26Guest"
        "&pwd=p%40ss%3Dword%23x&name=EY%20AP%2B%26"
        "&mac=A%26B%3DC%23D%25E%2BF&IP=192.168.13.1&port1=11111"
        "&port2=11113&key=EYMOTOR12FCB99FD224DB2B22F5A7703&w=1024&h=600";

    assert(carlink_ey_build_qr_url(url, sizeof(url), "A&B=C#D%E+F",
        "Office WiFi&Guest", "p@ss=word#x", "EY AP+&") == 0);
    assert(strcmp(url, expected) == 0);
}

static void test_rejects_truncation(void)
{
    char url[16];

    strcpy(url, "sentinel");
    assert(carlink_ey_build_qr_url(url, sizeof(url), "A1B2C3D4E5F6",
        "OfficeWifi", "secret123", "EY_AP") == -1);
    assert(url[0] == '\0');
}

static void test_uses_the_ey_qr_direct_device_identity(void)
{
    assert(strcmp(carlink_ey_qr_direct_device_mac(),
        "58C587F4293D") == 0);
}

int main(void)
{
    test_builds_complete_url();
    test_rejects_empty_parameters();
    test_percent_encodes_reserved_characters();
    test_rejects_truncation();
    test_uses_the_ey_qr_direct_device_identity();
    puts("carlink_ey_qr_test: PASS");
    return 0;
}
