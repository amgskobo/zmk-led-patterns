/* SPDX-License-Identifier: MIT
 * Scheduled presentation controller fixture. See tests/README.md for the
 * prior receipt-phase assertion replacement map.
 */
#define CONFIG_ZMK_SPLIT 1
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_ZMK_BLE 1
#include "led_stubs.h"
#include "scheduled_render_stubs.h"

static void set_presentation(const struct led_pattern_state *state, bool replace_brightness,
                             uint8_t brightness);

/* DRIVER_FUNCTIONS */

#include "scheduled_render_checks.h"
int main(void) {
    check_scheduled_controller();
    puts("led pattern (scheduled split central): PASS");
    return 0;
}
