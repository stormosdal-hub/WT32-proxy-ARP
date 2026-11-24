# WT32-ETH01 PLC bridge, ESP-IDF v6.1. Board and mode settings live in sdkconfig.defaults;
# passwords in sdkconfig.secrets (not in git, see sdkconfig.secrets.example). See README.md.
cmake_minimum_required(VERSION 3.22)

# Read the passwords after the defaults, so they win. An explicit -D SDKCONFIG_DEFAULTS=... still
# takes precedence.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND NOT DEFINED ENV{SDKCONFIG_DEFAULTS}
        AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.secrets")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(wt32_eth2ap)
