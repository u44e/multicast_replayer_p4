/* ── Replay engine: paced multicast retransmission ── */
#ifndef REPLAY_H
#define REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "stream_mgr.h"

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_error;
    uint32_t loops;          /* completed passes over the stream */
} replay_stats_t;

/** Start the replay task. mgr must stay valid; guard reloads with
 *  replay_stop_all() first. */
void replay_init(stream_mgr_t *mgr);

/** Output options applied to subsequently started streams. */
void replay_set_options(uint16_t out_port_override, bool embed_seq);

void replay_start(uint32_t stream_idx);
void replay_start_all(void);

/** Stop all streams and block until the task is idle (so the caller may
 *  free/reload the stream manager). */
void replay_stop_all(void);

bool replay_is_running(uint32_t stream_idx);
bool replay_any_running(void);

/** Aggregate packets/sec over the last second, for the status line. */
uint32_t replay_current_pps(void);

void replay_get_stats(uint32_t stream_idx, replay_stats_t *out);

#endif /* REPLAY_H */
