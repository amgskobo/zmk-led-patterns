/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * behavior_led_pattern_custom_settings.c built without USB: the BLE set is
 * always the live one, and there is no Studio serial port to go quiet for.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#define IS_ENABLED(option) option

struct transport_settings { const char *name; };
static const struct transport_settings usb_transport = {"usb"};
static const struct transport_settings ble_transport = {"ble"};

/* DRIVER_FUNCTIONS */

int main(void) {
    assert(active_transport() == &ble_transport && usb_transport.name != NULL);
    assert(!usb_studio_port_closed());
    puts("led settings bridge (BLE only): PASS");
    return 0;
}
