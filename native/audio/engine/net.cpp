// SPDX-License-Identifier: GPL-3.0-only
#include "net.h"

#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace beamer::audio::net {

#if defined(_WIN32)
const Socket kInvalid = INVALID_SOCKET;
#else
const Socket kInvalid = -1;
#endif

std::string Addr::to_string() const {
  char b[32];
  snprintf(b, sizeof b, "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
           port);
  return b;
}

bool startup() {
#if defined(_WIN32)
  static bool ok = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

static sockaddr_in make_sa(uint32_t ip, uint16_t port) {
  sockaddr_in sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(ip);
  sa.sin_port = htons(port);
  return sa;
}

bool resolve_ipv4(const std::string& host, uint32_t* ip) {
  in_addr a;
  if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
    *ip = ntohl(a.s_addr);
    return true;
  }
  addrinfo hints;
  std::memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  *ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);
  return true;
}

static void set_nonblocking(Socket s) {
#if defined(_WIN32)
  u_long one = 1;
  ioctlsocket(s, FIONBIO, &one);
#else
  int fl = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, fl | O_NONBLOCK);
  fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

bool route_source(uint32_t remote_ip, uint32_t* local_ip) {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalid) return false;
  sockaddr_in sa = make_sa(remote_ip, 9);
  bool ok = connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
  if (ok) {
    sockaddr_in me;
    socklen_t len = sizeof me;
    ok = getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr != 0;
    if (ok) *local_ip = ntohl(me.sin_addr.s_addr);
  }
  close_socket(s);
  return ok;
}

static std::string last_error() {
#if defined(_WIN32)
  return "wsa error " + std::to_string(WSAGetLastError());
#else
  return strerror(errno);
#endif
}

Socket udp_open(std::string* err) {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalid) {
    *err = "socket: " + last_error();
    return kInvalid;
  }
  int buf = 1 << 20;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
#if defined(_WIN32)
  // A connected UDP socket otherwise reports ICMP port unreachable as WSAECONNRESET on recv.
  BOOL off = FALSE;
  DWORD ret = 0;
  WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#endif
#if defined(__APPLE__)
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  set_nonblocking(s);
  return s;
}

bool udp_bind(Socket s, uint32_t local_ip, uint16_t port, std::string* err) {
  sockaddr_in sa = make_sa(local_ip, port);
  if (bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    *err = "bind " + Addr{local_ip, port}.to_string() + ": " + last_error();
    return false;
  }
  return true;
}

uint16_t local_port(Socket s) {
  sockaddr_in me;
  socklen_t len = sizeof me;
  if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) != 0) return 0;
  return ntohs(me.sin_port);
}

bool udp_connect(Socket s, const Addr& remote, std::string* err) {
  sockaddr_in sa = make_sa(remote.ip, remote.port);
  if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    *err = "connect " + remote.to_string() + ": " + last_error();
    return false;
  }
  return true;
}

int udp_send(Socket s, const uint8_t* data, size_t len) {
#if defined(_WIN32)
  return send(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
#else
  return static_cast<int>(send(s, data, len, 0));
#endif
}

int udp_recv(Socket s, uint8_t* buf, size_t cap, Addr* from) {
  sockaddr_in sa;
  socklen_t len = sizeof sa;
  for (;;) {
#if defined(_WIN32)
    int r = recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                     reinterpret_cast<sockaddr*>(&sa), &len);
    if (r == SOCKET_ERROR) {
      int e = WSAGetLastError();
      if (e == WSAEWOULDBLOCK) return 0;
      if (e == WSAECONNRESET || e == WSAEMSGSIZE || e == WSAENETRESET) continue;
      return -1;
    }
#else
    ssize_t r = recvfrom(s, buf, cap, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      if (errno == EINTR || errno == ECONNREFUSED) continue;
      return -1;
    }
#endif
    if (r == 0) continue;  // empty datagram: ignore
    if (from) {
      from->ip = ntohl(sa.sin_addr.s_addr);
      from->port = ntohs(sa.sin_port);
    }
    return static_cast<int>(r);
  }
}

void close_socket(Socket s) {
  if (s == kInvalid) return;
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

bool poll_readable(const Socket* socks, size_t n, bool* readable, int timeout_ms) {
#if defined(_WIN32)
  thread_local std::vector<WSAPOLLFD> p;
  p.resize(n);
  for (size_t i = 0; i < n; ++i) {
    p[i].fd = socks[i];
    p[i].events = POLLRDNORM;
    p[i].revents = 0;
  }
  if (n == 0) {
    Sleep(static_cast<DWORD>(timeout_ms));
    return true;
  }
  int r = WSAPoll(p.data(), static_cast<ULONG>(n), timeout_ms);
  if (r < 0) return false;
  for (size_t i = 0; i < n; ++i) readable[i] = (p[i].revents & (POLLRDNORM | POLLERR | POLLHUP)) != 0;
#else
  thread_local std::vector<pollfd> p;
  p.resize(n);
  for (size_t i = 0; i < n; ++i) {
    p[i].fd = socks[i];
    p[i].events = POLLIN;
    p[i].revents = 0;
  }
  int r = poll(p.data(), static_cast<nfds_t>(n), timeout_ms);
  if (r < 0) return errno == EINTR;
  for (size_t i = 0; i < n; ++i) readable[i] = (p[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0;
#endif
  return true;
}

}  // namespace beamer::audio::net
