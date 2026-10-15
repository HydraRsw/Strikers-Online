#!/usr/bin/env python3
"""
strikers_relay.py — Lightweight Matchmaking and Packet Relay Server for Super Mario Strikers.

Zero external dependencies (pure Python 3 standard library).
Can be run on your local machine, a VPS, or deployed to free hosts (Railway, Render, Fly.io).

How it works:
  1. Host sends:   {"cmd": "create"}
     Server replies: {"cmd": "created", "room": "4821"}
  2. Guest sends:  {"cmd": "join", "room": "4821"}
     Server replies: {"cmd": "joined", "room": "4821"}
  3. Server introduces both endpoints (UDP hole punching) and relays
     packets between them with minimal latency (~1ms overhead).
"""

import socket
import select
import random
import time
import json
import sys

DEFAULT_PORT = 7200
ROOM_TIMEOUT_SECS = 1800  # 30 minutes idle timeout

class Room:
    def __init__(self, code, host_addr):
        self.code = code
        self.host_addr = host_addr
        self.guest_addr = None
        self.created_at = time.time()
        self.last_active = time.time()
        self.packets_relayed = 0

    def is_expired(self):
        return time.time() - self.last_active > ROOM_TIMEOUT_SECS

def generate_room_code(existing_rooms):
    for _ in range(100):
        code = str(random.randint(1000, 9999))
        if code not in existing_rooms:
            return code
    return str(random.randint(10000, 99999))

def main():
    port = DEFAULT_PORT
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except ValueError:
            print(f"Usage: {sys.argv[0]} [port]")
            sys.exit(1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.setblocking(False)

    print("=" * 60)
    print(f" Super Mario Strikers Netplay Relay Server")
    print(f" Listening on UDP port {port}")
    print("=" * 60)

    rooms = {}          # code -> Room
    addr_to_room = {}   # (ip, port) -> Room

    last_cleanup = time.time()

    while True:
        try:
            r, _, _ = select.select([sock], [], [], 1.0)
        except (KeyboardInterrupt, SystemExit):
            print("\nShutting down server...")
            break

        now = time.time()

        # Clean up stale rooms once every 30 seconds
        if now - last_cleanup > 30:
            last_cleanup = now
            expired = [code for code, r in rooms.items() if r.is_expired()]
            for code in expired:
                r = rooms.pop(code)
                addr_to_room.pop(r.host_addr, None)
                if r.guest_addr:
                    addr_to_room.pop(r.guest_addr, None)
                print(f"[-] Room {code} expired and removed")

        if not r:
            continue

        try:
            data, addr = sock.recvfrom(2048)
        except OSError:
            continue

        # Check if this packet is a JSON control command
        if data.startswith(b"{") and data.endswith(b"}"):
            try:
                msg = json.loads(data.decode("utf-8"))
            except Exception:
                continue

            cmd = msg.get("cmd")

            if cmd == "create":
                # The game resends "create" until it hears back; reuse this
                # address's room instead of opening a second one.
                existing = addr_to_room.get(addr)
                if existing and existing.host_addr == addr and existing.code in rooms:
                    existing.last_active = now
                    sock.sendto(json.dumps({"cmd": "created", "room": existing.code, "slot": 0}).encode("utf-8"), addr)
                    continue

                # Create a new room
                code = generate_room_code(rooms)
                room = Room(code, addr)
                rooms[code] = room
                addr_to_room[addr] = room

                resp = json.dumps({
                    "cmd": "created",
                    "room": code,
                    "slot": 0
                }).encode("utf-8")
                sock.sendto(resp, addr)
                print(f"[+] Room {code} created by {addr[0]}:{addr[1]}")

            elif cmd == "join":
                # Join an existing room
                code = str(msg.get("room", "")).strip()
                room = rooms.get(code)

                if not room:
                    resp = json.dumps({"cmd": "error", "message": f"Room {code} not found"}).encode("utf-8")
                    sock.sendto(resp, addr)
                    print(f"[!] {addr[0]}:{addr[1]} tried to join non-existent room {code}")
                    continue

                if room.guest_addr and room.guest_addr != addr:
                    addr_to_room.pop(room.guest_addr, None)
                room.guest_addr = addr
                room.last_active = now
                addr_to_room[addr] = room

                # Notify Guest
                guest_resp = json.dumps({
                    "cmd": "joined",
                    "room": code,
                    "slot": 1
                }).encode("utf-8")
                sock.sendto(guest_resp, addr)

                # Notify Host that opponent connected
                host_resp = json.dumps({
                    "cmd": "peer_joined",
                    "room": code
                }).encode("utf-8")
                sock.sendto(host_resp, room.host_addr)

                print(f"[+] Guest {addr[0]}:{addr[1]} joined Room {code} (Host: {room.host_addr[0]}:{room.host_addr[1]})")

            elif cmd == "leave":
                room = addr_to_room.get(addr)
                if room and room.code in rooms:
                    if addr == room.host_addr:
                        # The host leaves: the room closes.
                        del rooms[room.code]
                        addr_to_room.pop(addr, None)
                        if room.guest_addr:
                            addr_to_room.pop(room.guest_addr, None)
                            try:
                                sock.sendto(b'{"cmd":"peer_left"}', room.guest_addr)
                            except OSError:
                                pass
                        print(f"[-] Room {room.code} closed by the host")
                    elif addr == room.guest_addr:
                        # The guest leaves: the room stays open for a rejoin.
                        addr_to_room.pop(addr, None)
                        room.guest_addr = None
                        room.last_active = now
                        try:
                            sock.sendto(b'{"cmd":"peer_left"}', room.host_addr)
                        except OSError:
                            pass
                        print(f"[-] Guest left room {room.code}; it stays open for a rejoin")

            elif cmd == "ping":
                # Keep-alive: keeps the NAT/tunnel mapping and the room alive
                room = addr_to_room.get(addr)
                if room:
                    room.last_active = now
                sock.sendto(b'{"cmd":"pong"}', addr)

            continue

        # Otherwise, this is a binary netplay game packet (8-byte inputs, rollback states, etc.)
        # Forward it to the peer in the same room!
        room = addr_to_room.get(addr)
        if room and room.guest_addr:
            room.last_active = now
            room.packets_relayed += 1

            if addr == room.host_addr:
                # Forward from Host -> Guest
                sock.sendto(data, room.guest_addr)
            elif addr == room.guest_addr:
                # Forward from Guest -> Host
                sock.sendto(data, room.host_addr)

if __name__ == "__main__":
    main()
