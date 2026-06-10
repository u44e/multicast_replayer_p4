/* ── Network Transmitter ── */
#ifndef NET_TX_H
#define NET_TX_H

#include <stdbool.h>
#include <stdint.h>

#define NET_TX_TTL 64

/** Initialise the transmitter (socket is created lazily, once Wi-Fi is up). */
void net_tx_init(void);

/**
 * Send the UDP payload of a captured packet as a fresh multicast datagram.
 * @param frame     raw Ethernet frame from the PCAP (Ethernet+IP+UDP)
 * @param frame_len length of the frame
 * @param dst_ip    destination IP, host byte order
 * @param dst_port  destination UDP port
 * @param embed_seq if true, prepend a 2-byte big-endian sequence number
 *                  (changes the payload — leave off for real video streams)
 * @param seq       sequence number used when embed_seq is true
 * @return 0 on success, negative on error
 */
int net_tx_send(const uint8_t *frame, uint32_t frame_len,
                uint32_t dst_ip, uint16_t dst_port,
                bool embed_seq, uint16_t seq);

void net_tx_shutdown(void);

#endif /* NET_TX_H */
