/* ── Classic PCAP file reader ── */
#include "pcap_reader.h"

#include <string.h>

#define PCAP_MAGIC      0xa1b2c3d4u
#define PCAP_MAGIC_SWAP 0xd4c3b2a1u
#define PCAP_GHDR_LEN   24
#define PCAP_PHDR_LEN   16
/* Anything bigger than this in incl_len means a corrupt file */
#define PCAP_SANE_LEN   (256 * 1024)

static uint32_t rd_u32(const uint8_t *p, bool swapped)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return swapped ? __builtin_bswap32(v) : v;
}

int pcap_open(pcap_file_t *p, const char *path)
{
    memset(p, 0, sizeof(*p));
    p->f = fopen(path, "rb");
    if (!p->f) return -1;

    uint8_t gh[PCAP_GHDR_LEN];
    if (fread(gh, 1, sizeof(gh), p->f) != sizeof(gh)) {
        pcap_close(p);
        return -2;
    }

    uint32_t magic = rd_u32(gh, false);
    if (magic == PCAP_MAGIC) {
        p->swapped = false;
    } else if (magic == PCAP_MAGIC_SWAP) {
        p->swapped = true;
    } else {
        pcap_close(p);
        return -3;
    }

    p->next_off = PCAP_GHDR_LEN;
    return 0;
}

int pcap_next(pcap_file_t *p, pcap_rec_t *rec)
{
    if (!p->f) return -1;
    uint8_t ph[PCAP_PHDR_LEN];
    if (fseek(p->f, (long)p->next_off, SEEK_SET) != 0) return -1;
    size_t n = fread(ph, 1, sizeof(ph), p->f);
    if (n == 0) return 0;
    if (n < sizeof(ph)) return feof(p->f) ? 0 : -1;

    uint32_t ts_sec  = rd_u32(ph + 0, p->swapped);
    uint32_t ts_usec = rd_u32(ph + 4, p->swapped);
    uint32_t incl    = rd_u32(ph + 8, p->swapped);
    if (incl == 0 || incl > PCAP_SANE_LEN) return -1;

    rec->data_off = p->next_off + PCAP_PHDR_LEN;
    rec->incl_len = incl;
    rec->ts_us    = (uint64_t)ts_sec * 1000000ULL + ts_usec;
    p->next_off   = rec->data_off + incl;
    return 1;
}

uint32_t pcap_read_at(pcap_file_t *p, uint32_t off, uint8_t *buf, uint32_t maxlen)
{
    if (!p->f) return 0;
    if (fseek(p->f, (long)off, SEEK_SET) != 0) return 0;
    return (uint32_t)fread(buf, 1, maxlen, p->f);
}

void pcap_close(pcap_file_t *p)
{
    if (p->f) {
        fclose(p->f);
        p->f = NULL;
    }
}
