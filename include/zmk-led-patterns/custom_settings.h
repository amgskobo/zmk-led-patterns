/*
 * SPDX-License-Identifier: MIT
 *
 * The custom-settings namespace this module publishes under.
 *
 * A setting is dropped with -ENOENT unless its subsystem is registered, and a
 * client groups the list it renders by subsystem, so this is also the heading
 * a person reads.
 *
 * It is short because it is spent, not read. The stored settings name is
 * "custom_settings/<subsystem>/<key>" against Zephyr's 64-byte
 * SETTINGS_MAX_NAME_LEN, so every character here is a character taken from the
 * key. See ASSERT_KEY_FITS below.
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

/*
 * The identifier is spelled once, as a token.
 *
 * custom-settings takes the subsystem as a string in every setting it
 * registers, while ZMK's ZMK_RPC_CUSTOM_SUBSYSTEM takes it as a token and
 * stringifies it to get the registered id. Spelling it twice is a silent
 * failure if the two ever disagree: the settings define fine, the subsystem
 * registers fine under the other name, and every setting is then dropped with
 * -ENOENT because no subsystem of its name is registered. So the token is the
 * definition and the string is derived from it, and the registration passes
 * the token through a wrapper so that it expands before being stringified.
 *
 * The prefix is an author namespace. Subsystem ids are a flat global space
 * shared by every module a firmware happens to load, so a bare "led" would be
 * the obvious thing for two of them to pick; prefixing keeps one author's
 * headings together in a client's list as well.
 */
#define ZMK_LED_PATTERNS_SUBSYSTEM_TOKEN amgskobo__led
#define ZMK_LED_PATTERNS_SUBSYSTEM STRINGIFY(ZMK_LED_PATTERNS_SUBSYSTEM_TOKEN)

/*
 * A setting key is "led." and then the field:
 *
 *     led.usb_pattern
 *     led.ble_brightness
 *
 * A fixed prefix rather than the owning node's devicetree name, which is what
 * the processor modules in this family use. They have to: a board routes
 * several of each and the node name is the only thing telling two instances
 * apart. This module controls one LED -- the one behind the zmk,backlight
 * chosen node, which devicetree allows exactly one of -- so there is nothing
 * to tell apart, and a fixed prefix buys two things instead. It reads as what
 * it is in a client's list, and it survives the behavior node being renamed in
 * a keymap, which a node-derived key would not: the stored values would simply
 * stop being found.
 */
#define ZMK_LED_PATTERNS_SETTING_KEY(field) "led." field

/*
 * The name a setting is stored under, which is longer than its key.
 *
 * custom-settings prefixes its own subtree and the subsystem before saving:
 * setting_storage_name() builds "custom_settings/<subsystem>/<key>" into a
 * SETTINGS_MAX_NAME_LEN buffer and returns -ENAMETOOLONG if it does not fit.
 * "custom_settings" is private to that module, so it is spelled out here; if
 * it ever changes, this over-estimates or under-estimates the budget and the
 * assert below is the thing to fix.
 */
#define ZMK_LED_PATTERNS_STORAGE_NAME(field)                                                       \
    "custom_settings/" ZMK_LED_PATTERNS_SUBSYSTEM "/" ZMK_LED_PATTERNS_SETTING_KEY(field)

/*
 * Fail at build time when a field cannot fit a settings key.
 *
 * There are two limits and they are not the same one. custom-settings refuses
 * a key over CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN (48) at build time, which
 * is the Studio RPC protobuf's limit on the key alone. Zephyr's settings
 * subsystem separately refuses a *stored name* over SETTINGS_MAX_NAME_LEN
 * (64), which covers the subtree and the subsystem too -- and it refuses it at
 * runtime, inside the save path, long after the value has been accepted over
 * RPC and applied to the hardware. The setting reads back correctly for as
 * long as the keyboard stays powered and is simply gone after a reboot.
 *
 * Nothing in custom-settings checks the two together, so the second limit is
 * checked here.
 *
 * "custom_settings/amgskobo__led/led." is 34 characters, which leaves a field
 * 29 -- the longest is "usb_brightness" at 14.
 */
#define ZMK_LED_PATTERNS_ASSERT_KEY_FITS(longest_field)                                            \
    BUILD_ASSERT(sizeof(ZMK_LED_PATTERNS_SETTING_KEY(longest_field)) <=                            \
                     CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                                       \
                 "setting key \"" ZMK_LED_PATTERNS_SETTING_KEY(longest_field) "\" is too long");   \
    BUILD_ASSERT(sizeof(ZMK_LED_PATTERNS_STORAGE_NAME(longest_field)) <= SETTINGS_MAX_NAME_LEN,    \
                 "setting key \"" ZMK_LED_PATTERNS_SETTING_KEY(                                    \
                     longest_field) "\" is too long to store under");
