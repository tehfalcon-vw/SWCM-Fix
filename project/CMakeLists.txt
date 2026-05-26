cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stm32_can_sniff_inject)

target_sources(app PRIVATE src/main.c)
