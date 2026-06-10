/* ── Multicast Replayer for ESP32-P4 (wired Ethernet) ──
 *
 * Loads multicast UDP streams from a PCAP file on the microSD card
 * (SDMMC, 4-bit) and retransmits them over Ethernet, paced by the
 * original capture timestamps. Controlled over the USB-Serial-JTAG
 * console (esp_console REPL):
 *
 *   load /sdcard/capture.pcap     load & index a PCAP file
 *   list                          show streams
 *   start <idx>|all               start replaying
 *   stop                          stop all
 *   opts [--port N] [--seq 0|1]   output options
 *   stat                          stats + link state
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "argtable3/argtable3.h"
#include "driver/sdmmc_host.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#include "eth_mgr.h"
#include "stream_mgr.h"
#include "net_tx.h"
#include "replay.h"

static const char *TAG = "app";

#define SD_MOUNT_POINT "/sdcard"

static stream_mgr_t g_mgr;
static bool g_sd_ok;
static uint16_t g_out_port;
static bool g_embed_seq;

/* ── SD card (SDMMC, on-chip LDO power on the P4 EV board) ── */
static bool sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

#if CONFIG_MCR_SD_LDO_CHAN >= 0
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CONFIG_MCR_SD_LDO_CHAN,
    };
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) == ESP_OK)
        host.pwr_ctrl_handle = pwr;
    else
        ESP_LOGW(TAG, "sd ldo init failed, trying without");
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.cmd = CONFIG_MCR_SD_PIN_CMD;
    slot.clk = CONFIG_MCR_SD_PIN_CLK;
    slot.d0  = CONFIG_MCR_SD_PIN_D0;
#if CONFIG_MCR_SD_BUS_WIDTH_4
    slot.d1  = CONFIG_MCR_SD_PIN_D1;
    slot.d2  = CONFIG_MCR_SD_PIN_D2;
    slot.d3  = CONFIG_MCR_SD_PIN_D3;
    slot.width = 4;
#else
    slot.width = 1;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd mount: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "sd mounted (%lluMB)",
             ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20);
    return true;
}

/* ── Console commands ── */
static struct {
    struct arg_str *path;
    struct arg_end *end;
} s_load_args;

static int cmd_load(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_load_args);
    if (n != 0) {
        arg_print_errors(stderr, s_load_args.end, argv[0]);
        return 1;
    }
    replay_stop_all();
    int rc = stream_mgr_load(&g_mgr, s_load_args.path->sval[0]);
    if (rc < 0) {
        printf("load failed (%d)\n", rc);
        return 1;
    }
    printf("%d multicast stream(s) indexed%s\n", rc,
           g_mgr.truncated ? " (index limit reached)" : "");
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (g_mgr.count == 0) {
        printf("no streams loaded (use: load <path>)\n");
        return 0;
    }
    for (uint32_t i = 0; i < g_mgr.count; i++) {
        stream_t *st = stream_mgr_get(&g_mgr, i);
        if (!st) continue;
        uint32_t ip = st->desc.dst_ip;
        replay_stats_t rs;
        replay_get_stats(i, &rs);
        printf("[%2lu] %lu.%lu.%lu.%lu:%u  %lu pkts  %.1fs  %s  sent=%lu err=%lu\n",
               (unsigned long)i,
               (unsigned long)(ip >> 24) & 0xff, (unsigned long)(ip >> 16) & 0xff,
               (unsigned long)(ip >> 8) & 0xff, (unsigned long)ip & 0xff,
               st->desc.dst_port,
               (unsigned long)st->desc.pkt_count,
               (double)st->desc.duration_us / 1e6,
               replay_is_running(i) ? "RUN " : "stop",
               (unsigned long)rs.packets_sent, (unsigned long)rs.packets_error);
    }
    return 0;
}

static struct {
    struct arg_str *what;
    struct arg_end *end;
} s_start_args;

static int cmd_start(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_start_args);
    if (n != 0) {
        arg_print_errors(stderr, s_start_args.end, argv[0]);
        return 1;
    }
    if (g_mgr.count == 0) {
        printf("no streams loaded\n");
        return 1;
    }
    if (!eth_mgr_connected())
        printf("warning: ethernet not connected yet\n");

    replay_set_options(g_out_port, g_embed_seq);
    const char *w = s_start_args.what->sval[0];
    if (strcmp(w, "all") == 0) {
        replay_start_all();
        printf("started all %lu streams\n", (unsigned long)g_mgr.count);
    } else {
        uint32_t idx = (uint32_t)atoi(w);
        if (idx >= g_mgr.count) {
            printf("no such stream: %s\n", w);
            return 1;
        }
        replay_start(idx);
        printf("started stream %lu\n", (unsigned long)idx);
    }
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    replay_stop_all();
    printf("stopped\n");
    return 0;
}

static struct {
    struct arg_int *port;
    struct arg_int *seq;
    struct arg_end *end;
} s_opts_args;

static int cmd_opts(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_opts_args);
    if (n != 0) {
        arg_print_errors(stderr, s_opts_args.end, argv[0]);
        return 1;
    }
    if (s_opts_args.port->count)
        g_out_port = (uint16_t)s_opts_args.port->ival[0];
    if (s_opts_args.seq->count)
        g_embed_seq = s_opts_args.seq->ival[0] != 0;
    replay_set_options(g_out_port, g_embed_seq);
    printf("port_override=%u embed_seq=%d (applies to next start)\n",
           g_out_port, g_embed_seq);
    return 0;
}

static int cmd_stat(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("eth: %s  ip: %s\n",
           eth_mgr_connected() ? "up" : "down", eth_mgr_ip_str());
    printf("sd: %s   heap: %luK  psram: %luK  tx: %lu pps\n",
           g_sd_ok ? "ok" : "not mounted",
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned long)replay_current_pps());
    return 0;
}

static void register_commands(void)
{
    s_load_args.path = arg_str1(NULL, NULL, "<path>", "PCAP file on /sdcard");
    s_load_args.end  = arg_end(2);
    const esp_console_cmd_t load_cmd = {
        .command = "load", .help = "Load & index a PCAP file",
        .func = cmd_load, .argtable = &s_load_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&load_cmd));

    const esp_console_cmd_t list_cmd = {
        .command = "list", .help = "List indexed streams", .func = cmd_list,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&list_cmd));

    s_start_args.what = arg_str1(NULL, NULL, "<idx>|all", "stream to start");
    s_start_args.end  = arg_end(2);
    const esp_console_cmd_t start_cmd = {
        .command = "start", .help = "Start replaying",
        .func = cmd_start, .argtable = &s_start_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command = "stop", .help = "Stop all streams", .func = cmd_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    s_opts_args.port = arg_int0("p", "port", "<n>", "override destination port (0=original)");
    s_opts_args.seq  = arg_int0("s", "seq", "<0|1>", "prepend 2-byte sequence number");
    s_opts_args.end  = arg_end(4);
    const esp_console_cmd_t opts_cmd = {
        .command = "opts", .help = "Set output options",
        .func = cmd_opts, .argtable = &s_opts_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&opts_cmd));

    const esp_console_cmd_t stat_cmd = {
        .command = "stat", .help = "Show link/heap/tx status", .func = cmd_stat,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stat_cmd));
}

/* ── Entry point ── */
void app_main(void)
{
    ESP_LOGI(TAG, "multicast replayer (ESP32-P4, Ethernet)");

    if (eth_mgr_start() != ESP_OK)
        ESP_LOGE(TAG, "ethernet init failed (check PHY wiring/menuconfig)");

    g_sd_ok = sd_mount();

    memset(&g_mgr, 0, sizeof(g_mgr));
    replay_init(&g_mgr);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "mcr>";
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    register_commands();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
