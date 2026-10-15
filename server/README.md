# Relay server

`strikers_relay.cpp` is the matchmaking and relay server for online matches. One file, no
dependencies, Windows, Linux and macOS. It listens on **UDP port 7200** by default.

```sh
strikers_relay [port]
```

What it does:

1. A game sends `create`: the relay makes a 4-digit room code and sends it back.
2. Another game sends `join` with that code: the relay tells both games about each other.
3. From then on it forwards each game's packets to the other one.

Rooms close when the host leaves, or after 30 minutes without traffic. Nothing is written to disk.

`strikers_relay.py` is an earlier version of the same server in plain Python 3. The C++ one is
the one tested with the current game; use the Python one only where compiling is not an option:
`python3 strikers_relay.py 7200`.

`HOST-SERVER.bat` starts the relay and a [Playit.gg](https://playit.gg) tunnel on Windows; see
[docs/HOSTING-A-SERVER.md](../docs/HOSTING-A-SERVER.md).

## Running it as a service on Linux

```ini
# /etc/systemd/system/strikers-relay.service
[Unit]
Description=Super Mario Strikers relay
After=network-online.target

[Service]
ExecStart=/usr/local/bin/strikers_relay 7200
Restart=always
DynamicUser=yes

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now strikers-relay
```
