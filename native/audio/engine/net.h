// SPDX-License-Identifier: GPL-3.0-only
// Minimal portable IPv4 UDP socket layer (BSD sockets / Winsock 2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace beamer::audio::net {

#if defined(_WIN32)
using Socket = uintptr_t;
#else
using Socket = int;
#endif
extern const Socket kInvalid;

struct Addr {
  uint32_t ip = 0;  // host order
  uint16_t port = 0;
  bool valid() const { return ip != 0; }
  std::string to_string() const;
};

bool startup();  // WSAStartup on Windows; no-op elsewhere
bool resolve_ipv4(const std::string& host, uint32_t* ip);  // literal or name, host order
// Local address of the interface that routes to `remote` (UDP connect + getsockname, no packet).
bool route_source(uint32_t remote_ip, uint32_t* local_ip);

Socket udp_open(std::string* err);  // non-blocking, 1 MiB buffers, no Windows UDP connreset
bool udp_bind(Socket s, uint32_t local_ip, uint16_t port, std::string* err);
uint16_t local_port(Socket s);
bool udp_connect(Socket s, const Addr& remote, std::string* err);
// Returns bytes sent or -1 (errors other than "would block" are ignored by callers).
int udp_send(Socket s, const uint8_t* data, size_t len);
// Returns bytes, 0 when nothing is pending, -1 on a hard error. from may be null.
int udp_recv(Socket s, uint8_t* buf, size_t cap, Addr* from);
void close_socket(Socket s);

// poll(2)/WSAPoll over sockets; readable[i] set for each socket with data. Returns false on error.
bool poll_readable(const Socket* socks, size_t n, bool* readable, int timeout_ms);

}  // namespace beamer::audio::net
