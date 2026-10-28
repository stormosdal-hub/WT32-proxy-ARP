# Mode 0 builds the unmodified ESP-IDF eth2ap example; Mode A builds the station bridge with
# its setup page and console.
if(CONFIG_BRIDGE_MODE_SOFTAP)
    set(srcs "ethernet_example_main.c")
    set(embed "")
else()
    set(srcs "bridge_main.c" "bridge_frames.c" "bridge_config.c" "bridge_web.c" "bridge_console.c")
    set(embed "web/index.html")
endif()

# Requirements can't depend on Kconfig, so both modes list everything; unused code isn't linked.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES ${embed}
                       PRIV_REQUIRES esp_wifi esp_eth esp_event esp_timer esp_netif nvs_flash
                                     esp_http_server app_update esp_app_format console)
