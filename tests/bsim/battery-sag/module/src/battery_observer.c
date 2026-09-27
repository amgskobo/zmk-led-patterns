/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Logs the battery levels ZMK itself reports: this half's, which battery.c
 * also writes to BAS, and on the central the peripheral's, read from the
 * peripheral's BAS over the split link. ZMK raises both only on a change.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_REGISTER(battery_observer, LOG_LEVEL_INF);

static int battery_observer(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *own = as_zmk_battery_state_changed(eh);

    if (own != NULL) {
        LOG_INF("battery: reports %u%%", own->state_of_charge);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    const struct zmk_peripheral_battery_state_changed *peer =
        as_zmk_peripheral_battery_state_changed(eh);

    if (peer != NULL) {
        LOG_INF("battery: peripheral %u reports %u%% over BLE", peer->source,
                peer->state_of_charge);
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_observer, battery_observer);
ZMK_SUBSCRIPTION(battery_observer, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(battery_observer, zmk_peripheral_battery_state_changed);
#endif
