/* ── Replay engine ──
 * One FreeRTOS task paces all running streams using the PCAP timestamps:
 * each stream's packet k is due at base_us + pkt_ts[k]; the task always
 * services the earliest-due packet and sleeps until then.
 */
#include "replay.h"
#include "net_tx.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "replay";

/* Pause between the last packet of a pass and restarting the stream */
#define LOOP_GAP_US 10000
/* If a capture has zero/garbage timestamps, pace at this interval instead */
#define FALLBACK_INTERVAL_US 1000

typedef struct {
    volatile bool running;
    uint32_t pkt_pos;
    uint16_t seq;
    uint64_t base_us;        /* wall time of packet 0 of the current pass */
    replay_stats_t stats;
} ctx_t;

static stream_mgr_t *s_mgr;
static ctx_t s_ctx[MAX_STREAMS];
static uint16_t s_out_port;
static bool s_embed_seq;
static volatile bool s_task_busy;
static TaskHandle_t s_task;

/* pps measurement */
static volatile uint32_t s_pps;
static uint32_t s_pps_count;
static uint64_t s_pps_window_us;

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static uint64_t pkt_due(const stream_t *st, const ctx_t *c)
{
    uint32_t ts = st->pkt_ts[c->pkt_pos];
    if (st->desc.duration_us == 0)
        return c->base_us + (uint64_t)c->pkt_pos * FALLBACK_INTERVAL_US;
    return c->base_us + ts;
}

static void send_one(uint32_t idx)
{
    ctx_t *c = &s_ctx[idx];
    stream_t *st = stream_mgr_get(s_mgr, idx);
    if (!st || st->desc.pkt_count == 0) {
        c->running = false;
        return;
    }

    uint8_t buf[1600];
    uint32_t len = stream_read_packet(s_mgr, idx, c->pkt_pos, buf, sizeof(buf));
    if (len > 0) {
        uint16_t port = s_out_port ? s_out_port : st->desc.dst_port;
        int ret = net_tx_send(buf, len, st->desc.dst_ip, port,
                              s_embed_seq, c->seq);
        if (ret == 0) c->stats.packets_sent++;
        else c->stats.packets_error++;
        c->seq++;
    } else {
        c->stats.packets_error++;
    }

    s_pps_count++;

    if (++c->pkt_pos >= st->desc.pkt_count) {
        c->pkt_pos = 0;
        c->stats.loops++;
        c->base_us = now_us() + LOOP_GAP_US;
    }
}

static void replay_task(void *arg)
{
    (void)arg;
    while (1) {
        /* Find the earliest-due running stream */
        int best = -1;
        uint64_t best_due = 0;
        for (uint32_t i = 0; i < MAX_STREAMS; i++) {
            if (!s_ctx[i].running) continue;
            stream_t *st = stream_mgr_get(s_mgr, i);
            if (!st) { s_ctx[i].running = false; continue; }
            uint64_t due = pkt_due(st, &s_ctx[i]);
            if (best < 0 || due < best_due) { best = (int)i; best_due = due; }
        }

        uint64_t now = now_us();
        if (now - s_pps_window_us >= 1000000ULL) {
            s_pps = s_pps_count;
            s_pps_count = 0;
            s_pps_window_us = now;
        }

        if (best < 0) {
            s_task_busy = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_task_busy = true;

        if (best_due > now) {
            uint64_t wait_us = best_due - now;
            /* sleep in chunks so a stop request is noticed quickly */
            if (wait_us > 50000) wait_us = 50000;
            if (wait_us >= 1000) {
                vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
                continue;   /* re-evaluate after sleeping */
            }
            /* sub-millisecond: busy-wait for precision */
            while (now_us() < best_due) { }
        }

        if (s_ctx[best].running)
            send_one((uint32_t)best);
    }
}

void replay_init(stream_mgr_t *mgr)
{
    s_mgr = mgr;
    memset(s_ctx, 0, sizeof(s_ctx));
    net_tx_init();
    xTaskCreatePinnedToCore(replay_task, "replay", 6144, NULL, 10, &s_task, 1);
}

void replay_set_options(uint16_t out_port_override, bool embed_seq)
{
    s_out_port = out_port_override;
    s_embed_seq = embed_seq;
}

static void start_idx(uint32_t i)
{
    ctx_t *c = &s_ctx[i];
    if (c->running) return;
    c->pkt_pos = 0;
    c->seq = 0;
    memset(&c->stats, 0, sizeof(c->stats));
    c->base_us = now_us();
    c->running = true;
}

void replay_start(uint32_t stream_idx)
{
    if (stream_idx >= MAX_STREAMS) return;
    start_idx(stream_idx);
}

void replay_start_all(void)
{
    if (!s_mgr) return;
    for (uint32_t i = 0; i < s_mgr->count && i < MAX_STREAMS; i++)
        start_idx(i);
}

void replay_stop_all(void)
{
    for (uint32_t i = 0; i < MAX_STREAMS; i++)
        s_ctx[i].running = false;
    /* Wait for the task to go idle before the caller touches s_mgr */
    for (int t = 0; t < 50 && s_task_busy; t++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task_busy)
        ESP_LOGW(TAG, "task still busy after stop");
    s_pps = 0;
}

bool replay_is_running(uint32_t stream_idx)
{
    return stream_idx < MAX_STREAMS && s_ctx[stream_idx].running;
}

bool replay_any_running(void)
{
    for (uint32_t i = 0; i < MAX_STREAMS; i++)
        if (s_ctx[i].running) return true;
    return false;
}

uint32_t replay_current_pps(void)
{
    return s_pps;
}

void replay_get_stats(uint32_t stream_idx, replay_stats_t *out)
{
    if (stream_idx >= MAX_STREAMS) { memset(out, 0, sizeof(*out)); return; }
    *out = s_ctx[stream_idx].stats;
}
