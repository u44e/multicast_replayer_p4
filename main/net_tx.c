/* ── Network Transmitter — UDP socket over lwIP ── */
#include "net_tx.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

#define ETH_HLEN 14
#define SEND_BUF_MAX 1500

static int s_fd = -1;

void net_tx_init(void)
{
    s_fd = -1;
}

static int ensure_socket(void)
{
    if (s_fd >= 0) return 0;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return -1;
    }
    uint8_t ttl = NET_TX_TTL;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
        ESP_LOGW(TAG, "IP_MULTICAST_TTL: errno %d", errno);
    s_fd = fd;
    return 0;
}

int net_tx_send(const uint8_t *frame, uint32_t frame_len,
                uint32_t dst_ip, uint16_t dst_port,
                bool embed_seq, uint16_t seq)
{
    if (ensure_socket() != 0) return -1;

    /* Extract the UDP payload from the captured frame */
    if (frame_len < ETH_HLEN + 20 + 8) return -2;
    const uint8_t *ip = frame + ETH_HLEN;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0f) * 4);
    if (ihl < 20 || frame_len < (uint32_t)ETH_HLEN + ihl + 8) return -2;

    const uint8_t *udp = ip + ihl;
    uint16_t udp_len = (uint16_t)((udp[4] << 8) | udp[5]);
    if (udp_len < 8) return -2;
    uint32_t payload_len = (uint32_t)udp_len - 8;
    if (ETH_HLEN + ihl + 8 + payload_len > frame_len)
        payload_len = frame_len - ETH_HLEN - ihl - 8;  /* truncated capture */
    const uint8_t *payload = udp + 8;

    uint8_t buf[SEND_BUF_MAX];
    const uint8_t *send_ptr = payload;
    uint32_t send_len = payload_len;
    if (embed_seq) {
        if (payload_len + 2 > sizeof(buf)) return -2;
        buf[0] = (uint8_t)(seq >> 8);
        buf[1] = (uint8_t)(seq & 0xff);
        memcpy(buf + 2, payload, payload_len);
        send_ptr = buf;
        send_len = payload_len + 2;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(dst_port),
        .sin_addr.s_addr = htonl(dst_ip),
    };
    int ret = sendto(s_fd, send_ptr, send_len, 0,
                     (struct sockaddr *)&dest, sizeof(dest));
    if (ret < 0) {
        ESP_LOGW(TAG, "sendto: errno %d", errno);
        return -3;
    }
    return 0;
}

void net_tx_shutdown(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}
