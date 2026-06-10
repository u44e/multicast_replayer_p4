/* ── Stream Manager ──
 * Two passes over the PCAP: count packets per multicast stream, then
 * allocate exact-size index arrays (offset/len/timestamp) and fill them.
 * Replay then reads any packet with a single fseek+fread.
 */
#include "stream_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "stream";

#define ETH_HLEN 14

static uint32_t rd_u32_be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd_u16_be(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }

/* Parse Ethernet/IPv4/UDP headers; fill desc if it's a multicast UDP packet. */
static int classify_packet(const uint8_t *buf, uint32_t len, stream_desc_t *desc)
{
    if (len < ETH_HLEN + 20 + 8) return -1;
    if (rd_u16_be(buf + 12) != 0x0800) return -1;       /* not IPv4 */

    const uint8_t *ip = buf + ETH_HLEN;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0f) * 4);
    if (ihl < 20 || len < (uint32_t)ETH_HLEN + ihl + 8) return -1;
    if (ip[9] != 17) return -1;                          /* not UDP */

    uint32_t dst_ip = rd_u32_be(ip + 16);
    if ((dst_ip & 0xf0000000u) != 0xe0000000u) return -1; /* not 224.0.0.0/4 */

    const uint8_t *udp = ip + ihl;
    desc->src_ip   = rd_u32_be(ip + 12);
    desc->src_port = rd_u16_be(udp);
    desc->dst_ip   = dst_ip;
    desc->dst_port = rd_u16_be(udp + 2);
    return 0;
}

static int find_stream(stream_mgr_t *mgr, const stream_desc_t *d)
{
    for (uint32_t j = 0; j < mgr->count; j++) {
        const stream_desc_t *s = &mgr->streams[j].desc;
        if (s->src_ip == d->src_ip && s->src_port == d->src_port &&
            s->dst_ip == d->dst_ip && s->dst_port == d->dst_port)
            return (int)j;
    }
    return -1;
}

int stream_mgr_load(stream_mgr_t *mgr, const char *pcap_path)
{
    stream_mgr_free(mgr);

    if (pcap_open(&mgr->pcap, pcap_path) != 0) {
        ESP_LOGE(TAG, "cannot open %s", pcap_path);
        return -1;
    }

    /* Pass 1: discover streams and per-stream packet counts */
    pcap_rec_t rec;
    uint8_t hdr[64];   /* enough for eth(14) + ip(max 60 we never read past 20+ihl) */
    uint32_t total = 0;
    int r;
    while ((r = pcap_next(&mgr->pcap, &rec)) == 1) {
        uint32_t want = rec.incl_len < sizeof(hdr) ? rec.incl_len : sizeof(hdr);
        if (pcap_read_at(&mgr->pcap, rec.data_off, hdr, want) != want) break;

        stream_desc_t d;
        if (classify_packet(hdr, want, &d) != 0) continue;

        int idx = find_stream(mgr, &d);
        if (idx < 0) {
            if (mgr->count >= MAX_STREAMS) continue;
            idx = (int)mgr->count++;
            mgr->streams[idx].desc = d;
        }
        mgr->streams[idx].desc.pkt_count++;
        if (++total >= MAX_INDEXED_PACKETS) {
            mgr->truncated = true;
            break;
        }
    }
    if (r < 0)
        ESP_LOGW(TAG, "pcap scan stopped at corrupt record");

    /* Allocate exact-size indexes */
    for (uint32_t j = 0; j < mgr->count; j++) {
        stream_t *st = &mgr->streams[j];
        uint32_t n = st->desc.pkt_count;
        st->pkt_off = malloc(n * sizeof(uint32_t));
        st->pkt_len = malloc(n * sizeof(uint16_t));
        st->pkt_ts  = malloc(n * sizeof(uint32_t));
        if (!st->pkt_off || !st->pkt_len || !st->pkt_ts) {
            ESP_LOGE(TAG, "out of memory indexing %lu packets",
                     (unsigned long)total);
            stream_mgr_free(mgr);
            return -2;
        }
        st->desc.pkt_count = 0;   /* refilled in pass 2 */
    }

    /* Pass 2: fill indexes */
    mgr->pcap.next_off = 24;
    uint64_t first_ts[MAX_STREAMS] = { 0 };
    uint32_t filled = 0;
    while (filled < total && pcap_next(&mgr->pcap, &rec) == 1) {
        uint32_t want = rec.incl_len < sizeof(hdr) ? rec.incl_len : sizeof(hdr);
        if (pcap_read_at(&mgr->pcap, rec.data_off, hdr, want) != want) break;

        stream_desc_t d;
        if (classify_packet(hdr, want, &d) != 0) continue;
        int idx = find_stream(mgr, &d);
        if (idx < 0) continue;

        stream_t *st = &mgr->streams[idx];
        uint32_t k = st->desc.pkt_count++;
        if (k == 0) first_ts[idx] = rec.ts_us;
        st->pkt_off[k] = rec.data_off;
        st->pkt_len[k] = (uint16_t)(rec.incl_len > 0xFFFF ? 0xFFFF : rec.incl_len);
        uint64_t dt = rec.ts_us >= first_ts[idx] ? rec.ts_us - first_ts[idx] : 0;
        st->pkt_ts[k] = (uint32_t)(dt > 0xFFFFFFFFu ? 0xFFFFFFFFu : dt);
        st->desc.duration_us = dt;
        filled++;
    }

    mgr->loaded = true;
    ESP_LOGI(TAG, "loaded %lu streams, %lu packets%s",
             (unsigned long)mgr->count, (unsigned long)filled,
             mgr->truncated ? " (index limit reached)" : "");
    return (int)mgr->count;
}

stream_t *stream_mgr_get(stream_mgr_t *mgr, uint32_t idx)
{
    if (!mgr->loaded || idx >= mgr->count) return NULL;
    return &mgr->streams[idx];
}

uint32_t stream_read_packet(stream_mgr_t *mgr, uint32_t stream_idx,
                            uint32_t pkt_idx, uint8_t *buf, uint32_t bufsz)
{
    stream_t *st = stream_mgr_get(mgr, stream_idx);
    if (!st || pkt_idx >= st->desc.pkt_count) return 0;
    uint32_t len = st->pkt_len[pkt_idx];
    if (len > bufsz) return 0;
    return pcap_read_at(&mgr->pcap, st->pkt_off[pkt_idx], buf, len);
}

void stream_mgr_free(stream_mgr_t *mgr)
{
    for (uint32_t j = 0; j < MAX_STREAMS; j++) {
        free(mgr->streams[j].pkt_off);
        free(mgr->streams[j].pkt_len);
        free(mgr->streams[j].pkt_ts);
    }
    pcap_close(&mgr->pcap);
    memset(mgr, 0, sizeof(*mgr));
}
