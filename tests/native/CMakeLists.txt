# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(led_patterns_workqueue)
target_sources(app PRIVATE ${LED_NATIVE_SOURCE})
target_include_directories(app PRIVATE ../../include ../runtime ../../src)
target_compile_options(app PRIVATE -Wno-unused-function)
