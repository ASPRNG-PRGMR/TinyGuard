# TinyGuard Monitor — ESP-IDF project root
# Requires ESP-IDF v5.x
# idf.py set-target esp32
# idf.py build && idf.py flash monitor

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinyguard_monitor)
