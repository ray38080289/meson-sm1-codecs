"""Append netconsole UDP packets to a file. Usage: python3 netcon_listen.py log [port]"""
import socket, sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", int(sys.argv[2]) if len(sys.argv) > 2 else 6666))
with open(sys.argv[1], "ab", buffering=0) as f:
    while True:
        f.write(s.recv(4096))
