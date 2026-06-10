/* ── Stream Manager: index of multicast UDP streams in a PCAP file ── */
#ifndef STREAM_MGR_H
#define STREAM_MGR_H

#include <stdbool.h>
#include <stdint.h>

#include "pcap_reader.h"

#define MAX_STREAMS 32
/* Total packets indexed across all streams; 10 bytes of RAM per packet.
 * The index arrays are large, so with CONFIG_SPIRAM_USE_MALLOC they are
 * allocated from PSRAM (200k packets = ~2MB). */
#define MAX_INDEXED_PACKETS 200000

typedef struct {
    uint32_t src_ip;      /* host byte order (224.0.0.1 -> 0xE0000001) */
    uint16_t src_port;
    uint32_t dst_ip;      /* host byte order */
    uint16_t dst_port;
    uint32_t pkt_count;
    uint64_t duration_us; /* capture time from first to last packet */
} stream_desc_t;

typedef struct {
    stream_desc_t desc;
    uint32_t *pkt_off;    /* file offset of each packet's data */
    uint16_t *pkt_len;    /* captured length of each packet */
    uint32_t *pkt_ts;     /* timestamp, microseconds from stream start */
} stream_t;

typedef struct {
    stream_t    streams[MAX_STREAMS];
    uint32_t    count;
    pcap_file_t pcap;     /* kept open while loaded, for replay reads */
    bool        loaded;
    bool        truncated; /* hit MAX_INDEXED_PACKETS while indexing */
} stream_mgr_t;

/** Load a PCAP file and build per-stream packet indexes.
 *  Returns stream count, or negative on error. */
int stream_mgr_load(stream_mgr_t *mgr, const char *pcap_path);

/** Get stream by index, or NULL. */
stream_t *stream_mgr_get(stream_mgr_t *mgr, uint32_t idx);

/** Read raw packet (Ethernet frame) pkt_idx of a stream into buf.
 *  Returns bytes read (0 on error). */
uint32_t stream_read_packet(stream_mgr_t *mgr, uint32_t stream_idx,
                            uint32_t pkt_idx, uint8_t *buf, uint32_t bufsz);

/** Free indexes and close the PCAP file. */
void stream_mgr_free(stream_mgr_t *mgr);

#endif /* STREAM_MGR_H */
