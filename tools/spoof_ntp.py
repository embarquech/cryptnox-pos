#!/usr/bin/env python3
"""Hostile NTP server: answers every request with a shifted date.

The real test for item 2/3 — unlike forcing the clock in firmware, this
exercises the actual SNTP client path with an attacker-controlled server.

    python spoof_ntp.py --days -200      # back-date (must be REFUSED by floor)
    python spoof_ntp.py --minutes 30     # forward skew (must be caught by Date)

Needs UDP/123, so run as Administrator. Point the device at it by
temporarily editing the ESP_SNTP_SERVER_LIST in main/net.cpp to this
machine's LAN IP.
"""
import argparse
import socket
import struct
import time

NTP_EPOCH_DELTA = 2208988800  # seconds between 1900-01-01 and 1970-01-01


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--days', type=float, default=0.0)
    ap.add_argument('--minutes', type=float, default=0.0)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=123)
    args = ap.parse_args()

    offset = (args.days * 86400.0) + (args.minutes * 60.0)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((args.bind, args.port))
    print(f'spoof NTP on {args.bind}:{args.port}, offset {offset:+.0f}s '
          f'-> claiming {time.ctime(time.time() + offset)}')

    while True:
        data, addr = s.recvfrom(512)
        if len(data) < 48:
            continue

        lie = time.time() + offset
        ntp_secs = int(lie) + NTP_EPOCH_DELTA
        ntp_frac = int((lie - int(lie)) * (2 ** 32))

        # LI=0 VN=4 Mode=4 (server), stratum 1, poll 3, precision -20
        pkt = struct.pack('!BBBbIIIQQQ',
                          (0 << 6) | (4 << 3) | 4, 1, 3, -20,
                          0, 0, 0x4C4F434C,          # root delay/disp, refid
                          (ntp_secs << 32) | ntp_frac,   # reference
                          struct.unpack('!Q', data[40:48])[0],  # originate
                          (ntp_secs << 32) | ntp_frac)   # receive
        pkt += struct.pack('!Q', (ntp_secs << 32) | ntp_frac)   # transmit

        s.sendto(pkt, addr)
        print(f'  lied to {addr[0]}: {time.ctime(lie)}')


if __name__ == '__main__':
    main()
