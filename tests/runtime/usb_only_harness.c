/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * behavior_led_pattern.c built with no split and no BLE: there is never a
 * host to advertise for, so the advertising blink is never shown.
 */
#include "led_stubs.h"

/* DRIVER_FUNCTIONS */

int main(void) {
    advertising_indicator = true;
    selected_transport = ZMK_TRANSPORT_NONE;
    assert(!should_show_advertising_blink());
    puts("led pattern (USB only): PASS");
    return 0;
}
