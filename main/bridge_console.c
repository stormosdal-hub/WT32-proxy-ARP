/*
 * Serial console (UART0, the same port as the log): status and settings without the setup page.
 * Settings are saved to NVS and take effect after 'restart'.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "bridge.h"
#include "bridge_config.h"
#include "bridge_frames.h"

static const char *TAG = "console";

static int save_and_report(const bridge_config_t *cfg)
{
    esp_err_t err = bridge_config_save(cfg);
    if (err != ESP_OK) {
        printf("Saving failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Saved. Type 'restart' to apply.\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    bridge_status_t st;
    bridge_get_status(&st);
    bridge_config_t cfg;
    bridge_config_load(&cfg);

    printf("Ethernet:   %s\n", st.eth_link_up ? "link up" : "link down");
    if (st.wifi_connected) {
        printf("Wi-Fi:      connected to \"%s\" (" MACSTR "), channel %u, rssi %d dBm\n",
               cfg.sta_ssid, MAC2STR(st.bssid), st.channel, st.rssi);
    } else {
        printf("Wi-Fi:      not connected (network \"%s\")\n", cfg.sta_ssid);
    }
    if (st.plc_mac_known) {
        printf("PLC:        " MACSTR "%s\n", MAC2STR(st.plc_mac), st.plc_seen ? "" : " (cached, not seen since link-up)");
    } else {
        printf("PLC:        not found yet\n");
    }
    if (st.plc_ip != 0) {
        printf("PLC IP:     " IPV4_FMT "\n", IPV4_ARGS(st.plc_ip));
    } else {
        printf("PLC IP:     learning\n");
    }
    printf("Eth->Wi-Fi: %" PRIu32 " frames (queue full %" PRIu32 ", tx fail %" PRIu32 ")\n",
           st.eth2wifi_fwd, st.eth2wifi_queue_full, st.eth2wifi_tx_fail);
    printf("Wi-Fi->Eth: %" PRIu32 " frames (tx fail %" PRIu32 ")\n", st.wifi2eth_fwd, st.wifi2eth_tx_fail);
    printf("Reconnects: %" PRIu32 "\n", st.reconnects);
    printf("PROFINET IO on Wi-Fi: %d device(s), %" PRIu32 " frames from the PLC to them\n",
           st.pnio_devices, st.eth2wifi_pnio);
    printf("Setup AP:   %s", st.setup_ap_active ? "open" : "closed");
    if (st.setup_ap_active) {
        printf(" (%d client%s)", st.setup_ap_clients, st.setup_ap_clients == 1 ? "" : "s");
    }
    printf("\n");
    if (!cfg.nat_enabled) {
        printf("Other devices: not bridged ('nat on' to enable)\n");
    } else if (!st.nat_active) {
        printf("Other devices: enabled, but off until the PLC IP is set and the bridge restarts\n");
    } else {
        printf("Other devices: %d\n", st.nat_count);
        for (int i = 0; i < st.nat_count; i++) {
            const nat_device_t *d = &st.nat_devices[i];
            printf("  " IPV4_FMT "  " MACSTR "  to Wi-Fi %" PRIu32 ", from Wi-Fi %" PRIu32 ", ",
                   IPV4_ARGS(d->ip), MAC2STR(d->mac), d->to_wifi, d->to_eth);
            if (d->idle_s < 0) {
                printf("not seen since boot\n");
            } else {
                printf("last frame %" PRId32 " s ago\n", d->idle_s);
            }
        }
    }
    return 0;
}

static int cmd_nat(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        printf("Usage: nat <on | off>   (bridge other IPv4 devices on the switch too)\n");
        return 1;
    }
    bridge_config_t cfg;
    bridge_config_load(&cfg);
    cfg.nat_enabled = strcmp(argv[1], "on") == 0;
    if (cfg.nat_enabled && cfg.plc_ip == 0) {
        printf("Note: this needs the PLC IP ('plcip <a.b.c.d>') to tell the PLC apart from the other devices.\n");
    }
    return save_and_report(&cfg);
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("Usage: wifi <ssid> [password]   (quote names with spaces: wifi \"My Net\" secret123)\n");
        return 1;
    }
    const char *password = argc == 3 ? argv[2] : "";
    const char *problem = bridge_config_check_ssid(argv[1]);
    if (!problem) {
        problem = bridge_config_check_sta_password(password);
    }
    if (problem) {
        printf("%s\n", problem);
        return 1;
    }
    bridge_config_t cfg;
    bridge_config_load(&cfg);
    strlcpy(cfg.sta_ssid, argv[1], sizeof(cfg.sta_ssid));
    strlcpy(cfg.sta_password, password, sizeof(cfg.sta_password));
    return save_and_report(&cfg);
}

static int cmd_plcip(int argc, char **argv)
{
    uint32_t ip = 0;
    if (argc != 2 || (strcmp(argv[1], "auto") != 0 && !ipv4_parse(argv[1], &ip))) {
        printf("Usage: plcip <a.b.c.d | auto>\n");
        return 1;
    }
    bridge_config_t cfg;
    bridge_config_load(&cfg);
    cfg.plc_ip = ip;
    return save_and_report(&cfg);
}

static int cmd_setup(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        printf("Usage: setup <on | off>\n");
        return 1;
    }
    bridge_request_setup_ap(strcmp(argv[1], "on") == 0);
    return 0;
}

static int cmd_setuppass(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: setuppass <password>\n");
        return 1;
    }
    const char *problem = bridge_config_check_setup_password(argv[1]);
    if (problem) {
        printf("%s\n", problem);
        return 1;
    }
    bridge_config_t cfg;
    bridge_config_load(&cfg);
    strlcpy(cfg.setup_password, argv[1], sizeof(cfg.setup_password));
    return save_and_report(&cfg);
}

static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    bridge_restart_soon();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    esp_err_t err = bridge_config_factory_reset();
    if (err != ESP_OK) {
        printf("Factory reset failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Saved settings and cached PLC MAC erased. Restarting with the build defaults...\n");
    bridge_restart_soon();
    return 0;
}

void bridge_console_start(void)
{
    static const esp_console_cmd_t commands[] = {
        { .command = "status", .help = "Show links, PLC, frame counters and setup AP state", .func = cmd_status },
        { .command = "wifi", .help = "Set the Wi-Fi network to join", .hint = "<ssid> [password]", .func = cmd_wifi },
        { .command = "plcip", .help = "Set the PLC's IP address, or 'auto' to learn it", .hint = "<a.b.c.d | auto>", .func = cmd_plcip },
        { .command = "nat", .help = "Also bridge other IPv4 devices on the switch", .hint = "<on | off>", .func = cmd_nat },
        { .command = "setup", .help = "Open or close the setup access point", .hint = "<on | off>", .func = cmd_setup },
        { .command = "setuppass", .help = "Set the setup access point password (8-63 characters)", .hint = "<password>", .func = cmd_setuppass },
        { .command = "restart", .help = "Restart the bridge (applies saved settings)", .func = cmd_restart },
        { .command = "factory-reset", .help = "Erase saved settings and the cached PLC MAC, then restart", .func = cmd_factory_reset },
    };

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "bridge>";
    repl_config.task_stack_size = 6144;         // 'status' holds the device table on the stack
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Console unavailable: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
