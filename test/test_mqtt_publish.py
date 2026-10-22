#!/usr/bin/env python3
"""Minimal MQTT 3.1.1 QoS0 publisher for lljs_ctrl testing; stdlib only."""
import argparse
import socket
import struct


def enc_len(n):
    out = bytearray()
    while True:
        d = n % 128
        n //= 128
        if n:
            d |= 0x80
        out.append(d)
        if not n:
            return bytes(out)


def utf8(s):
    b = s.encode()
    return struct.pack('!H', len(b)) + b


def packet_connect(client_id):
    payload = utf8('MQTT') + bytes([4, 2]) + struct.pack('!H', 60) + utf8(client_id)
    return bytes([0x10]) + enc_len(len(payload)) + payload


def packet_publish(topic, payload):
    body = utf8(topic) + payload.encode()
    return bytes([0x30]) + enc_len(len(body)) + body


def read_connack(sock):
    h = sock.recv(2)
    if len(h) != 2 or h[0] != 0x20 or h[1] != 2:
        raise RuntimeError('invalid CONNACK')
    data = sock.recv(2)
    if len(data) != 2 or data[1] != 0:
        raise RuntimeError('MQTT CONNACK refused')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host')
    ap.add_argument('port', type=int)
    ap.add_argument('topic')
    ap.add_argument('payload')
    ap.add_argument('--client-id', default='lljs_test_pub')
    args = ap.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as s:
        s.sendall(packet_connect(args.client_id))
        read_connack(s)
        s.sendall(packet_publish(args.topic, args.payload))
        print('MQTT PUB sent: topic=%s payload=%s' % (args.topic, args.payload))
        s.sendall(b'\xc0\x00')


if __name__ == '__main__':
    main()
