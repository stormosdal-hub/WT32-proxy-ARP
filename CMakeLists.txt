# WT32-ETH01 bring-up (Phase 2 / Mode 0): ESP-IDF v6.1 eth2ap example with WT32-ETH01 settings.
# Board configuration lives in sdkconfig.defaults; see README.md.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(wt32_eth2ap)
