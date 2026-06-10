/* ── Classic PCAP file reader ── */
#ifndef PCAP_READER_H
#define PCAP_READER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE    *f;
    bool     swapped;      /* file endianness != host */
    uint32_t next_off;     /* offset of next record header (for pcap_next) */
} pcap_file_t;

typedef struct {
    uint32_t data_off;     /* file offset of packet data */
    uint32_t incl_len;     /* captured length */
    uint64_t ts_us;        /* timestamp in microseconds */
} pcap_rec_t;

/** Open a PCAP file and validate the global header. 0 on success. */
int pcap_open(pcap_file_t *p, const char *path);

/** Sequentially read the next record header. 1 = ok, 0 = EOF, -1 = corrupt. */
int pcap_next(pcap_file_t *p, pcap_rec_t *rec);

/** Read up to maxlen bytes of packet data at a known offset. Returns bytes read. */
uint32_t pcap_read_at(pcap_file_t *p, uint32_t off, uint8_t *buf, uint32_t maxlen);

void pcap_close(pcap_file_t *p);

#endif /* PCAP_READER_H */
