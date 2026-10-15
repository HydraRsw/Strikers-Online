# Hosting a relay server

The relay is what lets two games find each other: the game that creates a room asks the relay for a
code, the other game asks to join that code, and the relay passes their packets back and forth. It
is tiny (no install, a few MB of memory) and one relay can serve many rooms at once.

Players never need to open ports on their routers: only the relay must be reachable, and a free
[Playit.gg](https://playit.gg) tunnel does that for you.

## On a Windows PC (the easy way)

You need the release zip (it contains `strikers_relay.exe` and `HOST-SERVER.bat`).

1. Download **playit** for Windows from <https://playit.gg/download> and put `playit.exe` in the
   game folder, next to `HOST-SERVER.bat`.
2. Double-click **HOST-SERVER.bat**. Two windows open: the relay ("listening on UDP port 7200") and
   Playit.
3. **First time only:** the Playit window shows a `https://playit.gg/claim/...` link. Open it, create
   a free account, and add a tunnel:
   - Tunnel type: **Custom (UDP)**
   - Local port: **7200**
4. Playit shows your public address, for example `name.ply.gg:12345`. That is your server address.
   It stays the same from now on.
5. Give that address to everyone who will play. They put it on the `server =` line of their
   `strikers.ini`. Put it in yours too.

Keep both windows open while people play. Close them when you are done.

Any player, you included, can now create rooms. The PC running the relay does not have to play:
it only has to be on.

> When you create a room on the same PC that runs the relay, the game uses the relay directly
> instead of going out through Playit and back.

## Without Playit (port forwarding)

If you can forward a port on your router, forward **UDP 7200** to the PC running
`strikers_relay.exe`, and give players `your-public-ip:7200`. A dynamic DNS name helps if your IP
changes.

## On a Linux server or VPS

The relay builds with any C++ compiler and has no dependencies:

```sh
g++ -O2 -std=c++17 -o strikers_relay server/strikers_relay.cpp
./strikers_relay 7200
```

Open UDP port 7200 in the firewall and give players `your-server:7200`. To keep it running, a
systemd unit is enough; see [server/README.md](../server/README.md).

## What the relay sees

Only the game's own packets: room requests, and the button presses and game-state checks the two
games exchange. It keeps nothing on disk. Rooms close when the host leaves or after 30 minutes
without traffic.
