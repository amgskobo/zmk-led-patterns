# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_TEST_BATTERY_DIVIDER)
  target_sources(app PRIVATE src/test_battery_divider.c src/battery_observer.c)
endif()
