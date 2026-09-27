# Copyright (c) 2025-2026 amgskobo
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_LED_PATTERNS)
  zephyr_include_directories(include)
  # One behavior per setting. Each file compiles to nothing when its node is
  # not in the devicetree; the pattern file also holds the LED controller.
  target_sources(app PRIVATE
    src/behavior_led_pattern.c
    src/behavior_led_brightness.c
    src/behavior_led_speed.c
  )
endif()

if(CONFIG_ZMK_LED_PATTERNS_BATTERY_ADC_OFFSET)
  target_sources(app PRIVATE src/battery_adc_offset.c)
endif()

if(CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS)
  # Also on the app target: it includes zmk/studio/custom.h, which pulls ZMK's
  # nanopb-generated proto/zmk/custom.pb.h, and that header is only on the app
  # target's include path.
  target_sources(app PRIVATE src/behavior_led_pattern_custom_settings.c)
endif()
