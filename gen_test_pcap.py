#!/usr/bin/env python3
"""Generate a test pcap file for Cardputer-Adv ESP32 multicast replayer."""
import os
import struct
import socket

def write_pcap(filename, packets):
    gh = struct.pack('>IHHiIII',
                     0xa1b2c3d4, 2, 0, 0, 0, 1500, 1)
    with open(filename, 'wb') as f:
        f.write(gh)
        for ts, pkt in packets:
            sec = int(ts)
            usec = int(round((ts - sec) * 1_000_000))
            ph = struct.pack('>IIII', sec, usec, len(pkt), len(pkt))
            f.write(ph)
            f.write(pkt)

def make_udp_packet(src_ip, src_port, dst_ip, dst_port, payload):
    eth = bytes.fromhex('01005e0000010000000000010800')
    total_len = 20 + 8 + len(payload)
    ip_hdr = bytearray(20)
    ip_hdr[0] = 0x45
    struct.pack_into('>H', ip_hdr, 2, total_len)
    ip_hdr[8] = 0x40
    ip_hdr[9] = 0x11
    struct.pack_into('>I', ip_hdr, 12, int.from_bytes(socket.inet_aton(src_ip), 'big'))
    struct.pack_into('>I', ip_hdr, 16, int.from_bytes(socket.inet_aton(dst_ip), 'big'))
    cksum = 0
    for i in range(0, 20, 2):
        word = (ip_hdr[i] << 8) | ip_hdr[i+1]
        cksum = (cksum + word) & 0xffff
    cksum = (~cksum) & 0xffff
    struct.pack_into('>H', ip_hdr, 10, cksum)
    udp_len = 8 + len(payload)
    udp_hdr = bytearray(8)
    struct.pack_into('>HHHH', udp_hdr, 0, src_port, dst_port, udp_len, 0)
    pkt = eth + ip_hdr + udp_hdr + payload
    if len(pkt) < 64:
        pkt += b'\x00' * (64 - len(pkt))
    return pkt

def main():
    packets = []
    ts = 1000.0
    for stream_idx in range(10):
        dst_ip = f'239.255.{stream_idx + 1}.1'
        dst_port = 5000 + stream_idx
        src_ip = f'192.168.1.{10 + stream_idx}'
        src_port = 4000 + stream_idx
        for i in range(3):
            payload = struct.pack('>H', i) + f'STRM{stream_idx}_DATA_{i}'.encode()
            pkt = make_udp_packet(src_ip, src_port, dst_ip, dst_port, payload)
            packets.append((ts + stream_idx * 2.0 + i * 0.1, pkt))
    packets.sort(key=lambda x: x[0])
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_multicast.pcap')
    write_pcap(out, packets)
    print(f"Generated {len(packets)} packets in {out}")

if __name__ == '__main__':
    main()
