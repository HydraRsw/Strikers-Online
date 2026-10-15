/*
 * transport.h — UDP transport layer for rollback netcode.
 *
 * Uses SDL3's cross-platform socket API for portability across
 * Windows, Linux, and macOS.  This is a thin non-blocking UDP wrapper.
 */

#ifndef PORT_NETPLAY_TRANSPORT_H
#define PORT_NETPLAY_TRANSPORT_H

#include "types.h"
#include <cstddef>

#ifdef __cplusplus

/* Opaque socket handle. */
struct NetplaySocket;

/*
 * Create a UDP socket bound to the given local port.
 * Returns NULL on failure.
 */
NetplaySocket* NetplaySocketCreate(u16 localPort);

/*
 * Destroy a socket created by NetplaySocketCreate.
 */
void NetplaySocketDestroy(NetplaySocket* sock);

/*
 * Set the remote peer address.  All subsequent sends go here.
 * addr is "ip:port" (e.g. "192.168.1.42:7100").
 * Returns true on success.
 */
bool NetplaySocketSetPeer(NetplaySocket* sock, const char* addr);

/*
 * Send a packet to the peer.  Non-blocking.
 * Returns the number of bytes sent, or -1 on error.
 */
int NetplaySocketSend(NetplaySocket* sock, const void* data, size_t len);

/*
 * Receive a packet (non-blocking).
 * Returns the number of bytes received, 0 if nothing available, or -1 on error.
 * If fromAddr is not NULL, it receives the sender's "ip:port" (null-terminated,
 * up to fromAddrSize bytes including the terminator).
 */
int NetplaySocketRecv(NetplaySocket* sock, void* buf, size_t bufSize,
                      char* fromAddr, size_t fromAddrSize);

/*
 * Get the current time in microseconds (monotonic).
 * Used for RTT measurement and timeout detection.
 */
u64 NetplayGetTimeUs();

/*
 * Detect the local Tailscale or LAN IPv4 address (e.g. "100.96.226.15").
 * Returns "127.0.0.1" if no network interface is found.
 */
const char* NetplayGetLocalIP();

#endif /* __cplusplus */

#endif /* PORT_NETPLAY_TRANSPORT_H */
