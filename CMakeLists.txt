cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Firmware version — bump when publishing a release to firmware/manifest.json
# (the "Check for updates" button compares against it).
set(PROJECT_VER "1.1.2")
project(voxrelay)
