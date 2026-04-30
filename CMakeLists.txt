# Top-level CMakeLists.txt for the DEFCON SG 1 badge DOOM port.
#
#   target  : ESP32-C6
#   panel   : ST7789 240x320, addressed as 320x240 landscape
#   buttons : 4 (A=GPIO23, B=15, C=10, D=9, all active-low)
#   storage : 8 MB flash, ~6 MB SPIFFS partition for DOOM1.WAD
#
# This is doomgeneric (https://github.com/ozkl/doomgeneric) wrapped as an
# ESP-IDF project, with the SDL platform layer replaced by a minimal
# ESP32-C6 backend (LCD blit + button → doomkey mapping).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dcsgonefw-doom)
# No SPIFFS — the WAD lives in a raw 'wad' partition (see partitions.csv)
# and is mmap'd at runtime via esp_partition_mmap. Flash it with
# tools/flash_wad.sh after the firmware is flashed.
