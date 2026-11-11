// SPDX-License-Identifier: GPL-3.0-only
#include "flow.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include "streams.h"
#include "util.h"
#include "video_recv.h"
#include "video_send.h"

namespace beamer::audio {

namespace {
// The sub-millisecond rest of a wait (poll() takes whole ms): select() with a us timeout, so video timers
// fire within ~0.1-0.3 ms of their deadline instead of up to 1 ms late (camera §4.2). Few sockets only.
bool wait_readable_us(const net::Socket* socks, size_t n, bool* readable, int64_t us) {
  fd_set rd;
  FD_ZERO(&rd);
  int maxfd = 0;
  for (size_t i = 0; i < n; ++i) {
    FD_SET(socks[i], &rd);
#if !defined(_WIN32)
    maxfd = std::max(maxfd, int(socks[i]));
#endif
  }
  timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>(std::max<int64_t>(0, us));
  const int r = select(maxfd + 1, &rd, nullptr, nullptr, &tv);
  if (r < 0) return false;
  for (size_t i = 0; i < n; ++i) readable[i] = FD_ISSET(socks[i], &rd) != 0;
  return true;
}
}  // namespace

const char* flow_state_name(FlowState s) {
  switch (s) {
    case FlowState::Probing: return "probing";
    case FlowState::Up: return "up";
    case FlowState::Link: return "link";
    case FlowState::Closed: return "closed";
  }
  return "?";
}

Flow::Flow(const std::string& id, const std::string& peer, bool offerer, const uint8_t key[kKeyLen],
           int rr_ms, EventSink* events)
    : id_(id), peer_(peer), offerer_(offerer), rr_ms_(rr_ms), events_(events) {
  std::memcpy(key_, key, kKeyLen);
  // stream 0: bit 31 = 0 for the offerer's packets, 1 for the answerer's
  ctrl_sealer_.init(key_, offerer_ ? 0u : kDirBit);
  opened_ns_ = now_ns();
}

Flow::~Flow() {
  {
    std::lock_guard<std::mutex> lock(pacer_mu_);
    pacer_.reset();  // its thread sends through this flow
  }
  std::lock_guard<std::mutex> lock(sock_mu_);
  if (sock_ != net::kInvalid) net::close_socket(sock_);
  sock_ = net::kInvalid;
  secure_wipe(key_, sizeof key_);
}

bool Flow::open_socket(const std::string& host, int port, std::string* err) {
  net::startup();
  std::string e;
  if (port == 0 && !host.empty()) {
    // Peer host known, no port of our own requested (the side that must not listen, i.e. the
    // Mac): no explicit bind. A UDP socket connect()ed towards the peer gets an ephemeral port
    // on the routing interface, exactly like proto/transport/peer.py's UdpClient (Phase 1 V3:
    // no macOS firewall dialog). It points at the default answer port 24823 until flow_remote
    // re-connect()s it to the port the answer carries; probing starts only then.
    net::Addr a;
    if (!net::resolve_ipv4(host, &a.ip)) {
      *err = "bad host " + host;
      return false;
    }
    a.port = kDefaultAnswerPort;
    net::Socket s = net::udp_open(&e);
    if (s == net::kInvalid || !net::udp_connect(s, a, &e)) {
      if (s != net::kInvalid) net::close_socket(s);
      *err = e;
      return false;
    }
    std::lock_guard<std::mutex> lock(sock_mu_);
    sock_ = s;
    port_ = net::local_port(s);
    return true;
  }
  uint32_t local_ip = 0;  // no host (tests on 127.0.0.1 only): 0.0.0.0
  if (!host.empty()) {
    uint32_t rip;
    if (net::resolve_ipv4(host, &rip)) net::route_source(rip, &local_ip);
  }
  std::vector<int> ports;
  if (port > 0)
    for (int p = port; p < port + 8 && p <= 65535; ++p) ports.push_back(p);
  ports.push_back(0);
  for (int p : ports) {
    net::Socket s = net::udp_open(&e);
    if (s == net::kInvalid) break;
    if (net::udp_bind(s, local_ip, static_cast<uint16_t>(p), &e)) {
      std::lock_guard<std::mutex> lock(sock_mu_);
      sock_ = s;
      port_ = net::local_port(s);
      return true;
    }
    net::close_socket(s);
  }
  *err = e;
  return false;
}

void Flow::set_link_only() { link_only_ = true; }

bool Flow::set_remote(const std::string& host, int port, std::string* err) {
  if (link_only_) return true;
  net::Addr a;
  if (!net::resolve_ipv4(host, &a.ip) || port <= 0 || port > 65535) {
    *err = "bad remote " + host + ":" + std::to_string(port);
    return false;
  }
  a.port = static_cast<uint16_t>(port);
  std::lock_guard<std::mutex> lock(sock_mu_);
  if (sock_ == net::kInvalid) {
    *err = "flow has no socket";
    return false;
  }
  if (!net::udp_connect(sock_, a, err)) return false;
  remote_ = a;
  has_remote_.store(true);
  return true;
}

void Flow::start() {
  if (link_only_) {
    state_.store(static_cast<int>(FlowState::Link));
    link_mode_.store(true);
  }
  events_->emit(Json{{"ev", "flow_state"}, {"flow", id_}, {"state", flow_state_name(state())}, {"rtt_ms", nullptr}});
}

void Flow::link_in(std::vector<std::vector<uint8_t>>&& packets) {
  std::lock_guard<std::mutex> lock(link_mu_);
  for (auto& p : packets) {
    if (inbox_.size() >= 256) break;
    inbox_.push_back(std::move(p));
  }
}

void Flow::attach_recv(const std::shared_ptr<RecvStream>& s) {
  std::lock_guard<std::mutex> lock(streams_mu_);
  recv_[s->id()] = s;
}

void Flow::attach_send(const std::shared_ptr<SendStream>& s) {
  std::lock_guard<std::mutex> lock(streams_mu_);
  send_[s->id()] = s;
}

void Flow::attach_video_recv(const std::shared_ptr<VideoRecvStream>& s) {
  std::lock_guard<std::mutex> lock(streams_mu_);
  vrecv_[s->id()] = s;
}

void Flow::attach_video_send(const std::shared_ptr<VideoSendStream>& s) {
  pacer()->add_client(s->id(), std::weak_ptr<PacerClient>(std::static_pointer_cast<PacerClient>(s)));
  std::lock_guard<std::mutex> lock(streams_mu_);
  vsend_[s->id()] = s;
}

void Flow::detach(uint32_t id) {
  std::lock_guard<std::mutex> lock(streams_mu_);
  recv_.erase(id);
  send_.erase(id);
  vrecv_.erase(id);
  vsend_.erase(id);
}

Pacer* Flow::pacer() {
  std::lock_guard<std::mutex> lock(pacer_mu_);
  if (!pacer_) pacer_ = std::make_unique<Pacer>([this](const PacedPacket& p) { send_video(p); });
  return pacer_.get();
}

Pacer* Flow::pacer_if_any() {
  std::lock_guard<std::mutex> lock(pacer_mu_);
  return pacer_.get();
}

void Flow::emit_video_event(uint32_t stream, uint32_t frame, std::vector<std::string>&& p, bool end) {
  (void)stream;
  Json arr = Json::array();
  for (auto& x : p) arr.push_back(std::move(x));
  n_link_tx.fetch_add(arr.size(), std::memory_order_relaxed);
  n_video_link_events.fetch_add(1, std::memory_order_relaxed);
  events_->emit(Json{{"ev", "link_out"}, {"flow", id_}, {"p", arr}, {"video", true}, {"frame", frame}, {"end", end}});
}

void Flow::send_video(const PacedPacket& p) {
  if (state() == FlowState::Closed) return;
  // dev drops at send (the packets stay in the sender's history, so NACK can repair them)
  const int64_t now = now_ns();
  bool drop = false;
  if (dev_video_drop_until.load(std::memory_order_relaxed) > now) drop = true;
  int64_t n = dev_video_drop_pk.load(std::memory_order_relaxed);
  while (!drop && n > 0) {
    if (dev_video_drop_pk.compare_exchange_weak(n, n - 1)) drop = true;
  }
  const double loss = dev_video_loss.load(std::memory_order_relaxed);
  if (!drop && loss > 0) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double burst = dev_video_burst.load(std::memory_order_relaxed);
    if (burst <= 0) {
      drop = u(vrng_) < loss;
    } else {
      // Gilbert: mean burst `burst` packets, stationary loss `loss`
      const double r = 1.0 / std::max(1.0, burst), q = std::min(1.0, loss * r / std::max(1e-9, 1.0 - loss));
      gilbert_bad_ = gilbert_bad_ ? u(vrng_) >= r : u(vrng_) < q;
      drop = gilbert_bad_;
    }
  }
  if (drop) {
    n_video_dev_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  n_video_tx.fetch_add(1, std::memory_order_relaxed);
  if (link_mode()) {
    std::vector<std::string> out;
    uint32_t out_frame = 0;
    bool emit = false, end = false;
    std::vector<std::string> prev;
    uint32_t prev_frame = 0;
    {
      std::lock_guard<std::mutex> lock(vlink_mu_);
      VEvent& ev = vpending_[p.stream];
      if (!ev.p.empty() && ev.frame != p.frame) {  // one frame per event
        prev.swap(ev.p);
        prev_frame = ev.frame;
      }
      ev.frame = p.frame;
      ev.p.push_back(b64_encode(p.bytes->data(), p.bytes->size()));
      if (ev.p.size() >= 4 || p.end) {
        out.swap(ev.p);
        out_frame = ev.frame;
        emit = true;
        end = p.end;
      }
    }
    if (!prev.empty()) emit_video_event(p.stream, prev_frame, std::move(prev), false);
    if (emit) emit_video_event(p.stream, out_frame, std::move(out), end);
    return;
  }
  {
    // left link state: flush what a frame had collected (rare)
    std::vector<std::pair<uint32_t, VEvent>> rest;
    {
      std::lock_guard<std::mutex> lock(vlink_mu_);
      for (auto& kv : vpending_)
        if (!kv.second.p.empty()) rest.emplace_back(kv.first, std::move(kv.second));
      vpending_.clear();
    }
    for (auto& r : rest) emit_video_event(r.first, r.second.frame, std::move(r.second.p), false);
  }
  udp_send_raw(p.bytes->data(), p.bytes->size());
}

bool Flow::udp_send_raw(const uint8_t* pkt, size_t len) {
  if (dev_drop_udp.load(std::memory_order_relaxed) || !has_remote_.load()) return false;
  std::lock_guard<std::mutex> lock(sock_mu_);
  if (sock_ == net::kInvalid) return false;
  net::udp_send(sock_, pkt, len);
  last_udp_tx_.store(now_ns(), std::memory_order_relaxed);
  n_udp_tx.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void Flow::send_media(const uint8_t* pkt, size_t len) {
  if (state() == FlowState::Closed) return;
  if (link_mode()) {
    std::lock_guard<std::mutex> lock(link_mu_);
    if (outbox_.size() >= 64) {
      outbox_.pop_front();
      n_link_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    outbox_.emplace_back(pkt, pkt + len);
  }
  udp_send_raw(pkt, len);
}

void Flow::send_ctrl(uint8_t ptype, const uint8_t* payload, size_t len) {
  if (link_only_) return;
  uint8_t buf[kMaxPacket];
  Header h;
  h.ptype = ptype;
  h.stream = 0;
  size_t n = ctrl_sealer_.seal(h, payload, len, buf);
  udp_send_raw(buf, n);
}

void Flow::set_state(FlowState s) {
  FlowState old = state();
  if (old == s) return;
  state_.store(static_cast<int>(s));
  link_mode_.store(s == FlowState::Link);
  if (s == FlowState::Up) {
    std::lock_guard<std::mutex> lock(link_mu_);
    outbox_.clear();
  }
  double rtt = rtt_ms();
  Json ev{{"ev", "flow_state"}, {"flow", id_}, {"state", flow_state_name(s)}};
  if (s == FlowState::Up && rtt >= 0) ev["rtt_ms"] = std::round(rtt * 10) / 10;
  else ev["rtt_ms"] = nullptr;
  events_->emit(ev);
  BA_INFO("flow %s: %s -> %s", id_.c_str(), flow_state_name(old), flow_state_name(s));
}

void Flow::on_readable(int64_t) {
  uint8_t buf[2048];
  for (int i = 0; i < 256; ++i) {
    net::Socket s;
    {
      std::lock_guard<std::mutex> lock(sock_mu_);
      s = sock_;
    }
    if (s == net::kInvalid) return;
    int n = net::udp_recv(s, buf, sizeof buf, nullptr);
    if (n <= 0) return;
    if (dev_drop_udp.load(std::memory_order_relaxed)) continue;
    handle_packet(buf, static_cast<size_t>(n), now_ns(), false);  // arrival time per datagram
  }
}

void Flow::mark_rx(int64_t now, bool via_link) {
  if (via_link) {
    last_link_in_ = now;
    n_link_rx.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  n_udp_rx.fetch_add(1, std::memory_order_relaxed);
  if (!got_udp_ || now - last_udp_rx_ > timing.run_gap_ns) {
    udp_run_start_ = now;
    if (state() == FlowState::Link) {
      // UDP is back: answer at once and probe fast until up
      uint8_t hp[4] = {1, static_cast<uint8_t>(state()), 0, 0};
      send_ctrl(kHello, hp, sizeof hp);
      next_hello_ = now + timing.hello_fast_ns;
    }
  }
  got_udp_ = true;
  last_udp_rx_ = now;
  if (state() == FlowState::Probing) {
    set_state(FlowState::Up);
    next_hello_ = now + timing.hello_slow_ns;
    if (next_ping_ > now + 50'000'000) next_ping_ = now;
  }
}

void Flow::handle_packet(const uint8_t* data, size_t len, int64_t now, bool via_link) {
  Header h;
  if (!read_header(data, len, &h) || len < kMinPacket || len > kMaxPacket) {
    n_rejected.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (h.stream == 0) {
    if (h.ptype != kHello && h.ptype != kPing && h.ptype != kPong && h.ptype != kBye && h.ptype != kTimeMap) {
      n_rejected.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    uint8_t payload[kMaxPacket];
    size_t plen = 0;
    OpenResult r = open_packet(key_, offerer_ ? kDirBit : 0u, &ctrl_window_, data, len, &h, payload, &plen);
    if (r == OpenResult::Rejected) n_rejected.fetch_add(1, std::memory_order_relaxed);
    if (r == OpenResult::Replayed) n_replayed.fetch_add(1, std::memory_order_relaxed);
    if (r != OpenResult::Ok) return;
    mark_rx(now, via_link);
    handle_ctrl(h, payload, plen, now);
    return;
  }
  OpenResult r = OpenResult::Rejected;
  bool known = false;
  std::vector<std::vector<uint8_t>> fb;  // NACKs sealed while ingesting (network thread)
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    if (h.ptype == kMedia) {
      auto it = recv_.find(h.stream);
      if (it != recv_.end()) {
        known = true;
        r = it->second->ingest(data, len, now, via_link);
      } else {
        auto v = vrecv_.find(h.stream);
        if (v != vrecv_.end()) {
          known = true;
          r = v->second->ingest(data, len, now, via_link, &fb);
        }
      }
    } else if (h.ptype == kRR || h.ptype == kNack || h.ptype == kPli) {
      auto it = h.ptype == kRR ? send_.find(h.stream) : send_.end();
      if (it != send_.end()) {
        known = true;
        r = it->second->ingest_rr(data, len, now);
      } else {
        auto v = vsend_.find(h.stream);
        if (v != vsend_.end()) {
          known = true;
          r = v->second->ingest_feedback(data, len, now);
        }
      }
    }
  }
  for (auto& p : fb) send_feedback(p.data(), p.size());
  if (!known) {
    n_unknown.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (r == OpenResult::Rejected) n_rejected.fetch_add(1, std::memory_order_relaxed);
  if (r == OpenResult::Replayed) n_replayed.fetch_add(1, std::memory_order_relaxed);
  if (r == OpenResult::Ok) mark_rx(now, via_link);
}

void Flow::handle_ctrl(const Header& h, const uint8_t* p, size_t n, int64_t now) {
  switch (h.ptype) {
    case kHello:
      break;
    case kPing:
      if (n >= 8) {
        uint8_t out[24];
        std::memcpy(out, p, 8);
        put_u64(out + 8, static_cast<uint64_t>(now));
        put_u64(out + 16, static_cast<uint64_t>(now_ns()));
        send_ctrl(kPong, out, sizeof out);
      }
      break;
    case kPong:
      if (n >= 24) {
        int64_t t0 = static_cast<int64_t>(get_u64(p)), t1 = static_cast<int64_t>(get_u64(p + 8)),
                t2 = static_cast<int64_t>(get_u64(p + 16));
        int64_t rtt = (now - t0) - (t2 - t1);
        offset_.add(t0, t1, t2, now);  // clock offset, min-RTT of the last 10 (camera §4.5.3)
        if (rtt >= 0 && rtt < 10'000'000'000LL) {
          rtts_[n_rtts_ % 5] = static_cast<double>(rtt) * 1e-6;
          n_rtts_++;
          int k = std::min(n_rtts_, 5);
          double tmp[5];
          std::copy(rtts_, rtts_ + k, tmp);
          std::sort(tmp, tmp + k);
          rtt_ms_.store(k % 2 ? tmp[k / 2] : 0.5 * (tmp[k / 2 - 1] + tmp[k / 2]));
        }
      }
      break;
    case kTimeMap: {
      std::vector<TimeMapEntry> e;
      if (!read_time_map(p, n, &e)) break;
      n_time_map_rx.fetch_add(1, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(streams_mu_);
        for (auto& x : e) {
          auto v = vrecv_.find(x.stream);
          if (v != vrecv_.end()) v->second->on_time_map(x.ts, x.sender_ns);
          auto a = recv_.find(x.stream);
          if (a != recv_.end()) a->second->on_time_map(x.ts, x.sender_ns);
        }
      }
      if (dev_log_time_map.load()) {
        Json arr = Json::array();
        for (auto& x : e) arr.push_back(Json{{"stream", x.stream}, {"ts", x.ts}, {"sender_ns", x.sender_ns}});
        events_->emit(Json{{"ev", "time_map"}, {"flow", id_}, {"dir", "rx"}, {"t_ns", now}, {"entries", arr}});
      }
      break;
    }
    case kBye:
      // the peer closed its end of this flow: forget UDP liveness, keep probing
      got_udp_ = false;
      udp_run_start_ = 0;
      last_udp_rx_ = 0;
      opened_ns_ = now;
      if (state() == FlowState::Up) set_state(FlowState::Probing);
      next_hello_ = now + timing.hello_fast_ns;
      break;
    default:
      break;
  }
}

void Flow::send_rr(int64_t now) {
  uint8_t buf[kMaxPacket];
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    size_t active = 0;
    for (auto& kv : recv_)
      if (kv.second->media_recent(now)) active++;
    if (active == 0) return;
    size_t pick = rr_rr_++ % active, i = 0;
    for (auto& kv : recv_)
      if (kv.second->media_recent(now) && i++ == pick) {
        n = kv.second->make_rr(buf);
        break;
      }
  }
  if (n && udp_send_raw(buf, n)) n_rr_tx.fetch_add(1, std::memory_order_relaxed);
}

void Flow::flush_link_out() {
  std::vector<std::vector<uint8_t>> pk;
  {
    std::lock_guard<std::mutex> lock(link_mu_);
    if (outbox_.empty()) return;
    while (!outbox_.empty()) {
      pk.push_back(std::move(outbox_.front()));
      outbox_.pop_front();
    }
  }
  for (size_t i = 0; i < pk.size(); i += 4) {
    Json arr = Json::array();
    for (size_t k = i; k < pk.size() && k < i + 4; ++k) arr.push_back(b64_encode(pk[k]));
    n_link_tx.fetch_add(arr.size(), std::memory_order_relaxed);
    events_->emit(Json{{"ev", "link_out"}, {"flow", id_}, {"p", arr}});
  }
}

void Flow::tick(int64_t now) {
  if (state() == FlowState::Closed) return;
  // packets that came over the Beamer link
  {
    std::unique_lock<std::mutex> lock(link_mu_);
    if (!inbox_.empty()) {
      std::vector<std::vector<uint8_t>> in;
      in.swap(inbox_);
      lock.unlock();
      for (auto& p : in) handle_packet(p.data(), p.size(), now, true);
    }
  }

  if (!link_only_) {
    const bool remote = has_remote_.load();
    if (remote && remote_set_ns_ == 0) {
      remote_set_ns_ = now;
      next_hello_ = now;
      next_ping_ = now;
    }
    // state machine (§4.5)
    const bool peer_link = last_link_in_ != 0 && now - last_link_in_ < timing.peer_link_ns;
    const int64_t silent = got_udp_ ? now - last_udp_rx_ : now - std::max(opened_ns_, remote_set_ns_);
    FlowState st = state();
    if (st != FlowState::Link) {
      if (silent > timing.fallback_ns || peer_link) set_state(FlowState::Link);
    } else {
      bool udp_ok = got_udp_ && now - last_udp_rx_ < timing.run_gap_ns &&
                    now - udp_run_start_ >= timing.recover_ns;
      if (udp_ok && !peer_link) set_state(FlowState::Up);
    }
    st = state();
    if (remote) {
      if (now >= next_hello_) {
        uint8_t hp[4] = {1, static_cast<uint8_t>(st), 0, 0};
        send_ctrl(kHello, hp, sizeof hp);
        bool fast = (st == FlowState::Probing && !got_udp_) ||
                    (st == FlowState::Link && got_udp_ && now - last_udp_rx_ < timing.run_gap_ns);
        next_hello_ = now + (fast ? timing.hello_fast_ns : timing.hello_slow_ns);
      }
      if (got_udp_ && now >= next_ping_) {
        uint8_t pp[8];
        put_u64(pp, static_cast<uint64_t>(now_ns()));
        send_ctrl(kPing, pp, sizeof pp);
        next_ping_ = now + timing.ping_ns;
      }
      // receiver reports: while media arrives and this side sent nothing for rr_ms (W3)
      if (rr_ms_ > 0) {
        const int64_t iv = int64_t(rr_ms_) * 1'000'000;
        if (next_rr_ == 0) next_rr_ = now + iv;
        if (now >= next_rr_) {
          next_rr_ = (now - next_rr_ > iv) ? now + iv : next_rr_ + iv;
          if (now - last_udp_tx_.load(std::memory_order_relaxed) >= iv * 9 / 10) send_rr(now);
        }
      }
    }
  } else if (state() != FlowState::Link) {
    set_state(FlowState::Link);
  }
  video_tick(now);
  if (now >= next_flush_) {
    next_flush_ = now + timing.link_flush_ns;
    if (link_mode()) flush_link_out();
  }
}

void Flow::send_time_map(int64_t now) {
  std::vector<TimeMapEntry> e;
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    for (auto& kv : vsend_) {
      TimeMapEntry x;
      int64_t ns;
      if (kv.second->time_map_pair(&x.ts, &ns)) {
        x.stream = kv.first;
        x.sender_ns = static_cast<uint64_t>(ns);
        e.push_back(x);
      }
    }
    for (auto& kv : send_) {
      TimeMapEntry x;
      int64_t ns;
      if (kv.second->sr() && kv.second->time_map_pair(&x.ts, &ns)) {
        x.stream = kv.first;
        x.sender_ns = static_cast<uint64_t>(ns);
        e.push_back(x);
      }
    }
  }
  if (e.empty()) return;  // nothing to map: an old peer never sees a time map
  if (e.size() > kMaxTimeMapEntries) e.resize(kMaxTimeMapEntries);
  uint8_t payload[1 + kMaxTimeMapEntries * 16];
  const size_t n = write_time_map(e, payload);
  uint8_t buf[kMaxPacket];
  Header h;
  h.ptype = kTimeMap;
  h.stream = 0;
  const size_t len = ctrl_sealer_.seal(h, payload, n, buf);
  send_feedback(buf, len);  // UDP, plus the link outbox in link state
  n_time_map_tx.fetch_add(1, std::memory_order_relaxed);
  if (dev_log_time_map.load()) {
    Json arr = Json::array();
    for (auto& x : e) arr.push_back(Json{{"stream", x.stream}, {"ts", x.ts}, {"sender_ns", x.sender_ns}});
    events_->emit(Json{{"ev", "time_map"}, {"flow", id_}, {"dir", "tx"}, {"t_ns", now}, {"entries", arr}});
  }
}

void Flow::video_tick(int64_t now) {
  // receiver side: NACK retries, PLIs posted by the video/backend threads, the 50 ms video RR
  std::vector<std::shared_ptr<VideoRecvStream>> vr;
  bool covered_new = false;
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    for (auto& kv : vrecv_) vr.push_back(kv.second);
    // time map: every 1 s, and in the first tick after a covered stream sent its first media packet and again
    // 20 ms later (§4.2)
    auto first = [&](uint32_t id, int64_t first_tx) {
      if (!first_tx) return;
      TmState& t = tm_state_[id];
      if (!t.first_sent) {
        t.first_sent = true;
        t.second_at = now + 20'000'000;
        covered_new = true;
      } else if (t.second_at && now >= t.second_at) {
        t.second_at = 0;
        covered_new = true;
      }
    };
    for (auto& kv : vsend_) first(kv.first, kv.second->first_tx_ns());
    for (auto& kv : send_)
      if (kv.second->sr()) first(kv.first, kv.second->first_tx_ns());
    for (auto it = tm_state_.begin(); it != tm_state_.end();)
      it = (vsend_.count(it->first) || send_.count(it->first)) ? std::next(it) : tm_state_.erase(it);
  }
  const double rtt = rtt_ms();
  std::vector<std::vector<uint8_t>> out;
  for (auto& v : vr) {
    out.clear();
    v->net_tick(now, rtt, &out);
    for (auto& p : out) send_feedback(p.data(), p.size());
  }
  if (next_tm_ == 0) next_tm_ = now + 1'000'000'000;
  if (covered_new || now >= next_tm_) {
    if (now >= next_tm_) next_tm_ = now + 1'000'000'000;
    send_time_map(now);
  }
}

int64_t Flow::next_deadline(int64_t now) const {
  int64_t vd = now + 10'000'000;  // video timers (NACK retry, video RR, time map, PLI repeats)
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    for (auto& kv : vrecv_) {
      const int64_t t = kv.second->next_net_deadline();
      if (t) vd = std::min(vd, t);
    }
    for (auto& kv : tm_state_)
      if (kv.second.second_at) vd = std::min(vd, kv.second.second_at);
    for (auto& kv : vsend_)
      if (kv.second->first_tx_ns() && !tm_state_.count(kv.first)) vd = now;
    if (!vsend_.empty() || !vrecv_.empty()) vd = std::min(vd, next_tm_ ? next_tm_ : now);
  }
  // link mode (queues to flush, link_in to process) and the very start: the 2 ms granularity
  if (link_only_ || link_mode() || remote_set_ns_ == 0) return std::min(vd, now + 2'000'000);
  int64_t d = std::min(next_hello_, vd);
  if (got_udp_) d = std::min(d, next_ping_);
  if (rr_ms_ > 0 && next_rr_ != 0) d = std::min(d, next_rr_);
  // fallback check: when the 2 s of UDP silence would expire
  const int64_t since = got_udp_ ? last_udp_rx_ : std::max(opened_ns_, remote_set_ns_);
  d = std::min(d, since + timing.fallback_ns + 1'000'000);
  {
    std::lock_guard<std::mutex> lock(link_mu_);
    if (!inbox_.empty()) return now;
  }
  return d;
}

void Flow::close_now() {
  if (state() != FlowState::Closed && !link_only_ && has_remote_.load()) {
    uint8_t z[1] = {0};
    for (int i = 0; i < 3; ++i) send_ctrl(kBye, z, 0);
  }
  {
    std::lock_guard<std::mutex> lock(sock_mu_);
    if (sock_ != net::kInvalid) net::close_socket(sock_);
    sock_ = net::kInvalid;
  }
  state_.store(static_cast<int>(FlowState::Closed));
  link_mode_.store(false);
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    recv_.clear();
    send_.clear();
    vrecv_.clear();
    vsend_.clear();
  }
}

void Flow::mark_closed() {
  std::lock_guard<std::mutex> lock(closed_mu_);
  closed_ = true;
  closed_cv_.notify_all();
}

bool Flow::wait_closed(int ms) {
  std::unique_lock<std::mutex> lock(closed_mu_);
  return closed_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return closed_; });
}

// ------------------------------------------------------------------ Waker

Waker::~Waker() {
  if (sock_ != net::kInvalid) net::close_socket(sock_);
}

bool Waker::open() {
  if (sock_ != net::kInvalid) return true;
  net::startup();
  std::string e;
  net::Socket s = net::udp_open(&e);
  if (s == net::kInvalid) return false;
  net::Addr self;
  self.ip = 0x7F000001;  // 127.0.0.1
  if (!net::udp_bind(s, self.ip, 0, &e) || !(self.port = net::local_port(s)) || !net::udp_connect(s, self, &e)) {
    net::close_socket(s);
    return false;
  }
  sock_ = s;
  return true;
}

void Waker::wake() {
  if (sock_ == net::kInvalid || pending_.exchange(true)) return;
  const uint8_t b = 1;
  net::udp_send(sock_, &b, 1);
}

void Waker::drain() {
  pending_.store(false);
  uint8_t buf[64];
  for (int i = 0; i < 64 && net::udp_recv(sock_, buf, sizeof buf, nullptr) > 0; ++i) {
  }
}

// ------------------------------------------------------------------ NetLoop

void NetLoop::start() {
  if (running_) return;
  if (!waker_.open()) BA_WARN("net loop: no waker socket (video timers fall back to polling)");
  running_ = true;
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void NetLoop::stop() {
  if (!running_) return;
  stop_.store(true);
  thread_.join();
  running_ = false;
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& f : flows_) {
    f->close_now();
    f->mark_closed();
  }
  flows_.clear();
}

void NetLoop::add(const std::shared_ptr<Flow>& f) {
  f->set_waker(&waker_);
  std::lock_guard<std::mutex> lock(mu_);
  flows_.push_back(f);
  gen_++;
}

void NetLoop::close(const std::shared_ptr<Flow>& f) {
  if (!running_) {
    f->close_now();
    f->mark_closed();
    std::lock_guard<std::mutex> lock(mu_);
    flows_.erase(std::remove(flows_.begin(), flows_.end(), f), flows_.end());
    gen_++;
    return;
  }
  f->request_close();
  if (!f->wait_closed(1000)) BA_WARN("flow %s: close timed out", f->id().c_str());
}

void NetLoop::run() {
  set_thread_name("beamer-audio-net");
  set_thread_interactive();
  enable_fine_timers();
  std::vector<std::shared_ptr<Flow>> flows;
  std::vector<net::Socket> socks;
  std::vector<Flow*> owners;
  std::unique_ptr<bool[]> readable(new bool[256]);
  uint64_t seen_gen = ~uint64_t(0);
  socks.reserve(256);
  owners.reserve(256);
  while (!stop_.load()) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto it = flows_.begin(); it != flows_.end();) {
        if ((*it)->close_requested()) {
          (*it)->close_now();
          (*it)->mark_closed();
          it = flows_.erase(it);
          gen_++;
        } else {
          ++it;
        }
      }
      if (seen_gen != gen_) {  // copy (refcounts, allocation) only when the set changed
        flows = flows_;
        seen_gen = gen_;
      }
    }
    if (flows.empty()) {
      sleep_ns(20'000'000);
      continue;
    }
    socks.clear();
    owners.clear();
    const bool have_waker = waker_.sock() != net::kInvalid;
    if (have_waker) {
      socks.push_back(waker_.sock());
      owners.push_back(nullptr);
    }
    for (auto& f : flows) {
      if (f->sock() != net::kInvalid && socks.size() < 256) {
        socks.push_back(f->sock());
        owners.push_back(f.get());
      }
    }
    for (size_t i = 0; i < socks.size(); ++i) readable[i] = false;
    // sleep until the earliest flow timer (<= 10 ms; 2 ms in link mode; video timers); packets and the
    // waker end the wait earlier
    int64_t t = now_ns(), wake = t + 10'000'000;
    for (auto& f : flows) wake = std::min(wake, f->next_deadline(t));
    // whole ms with poll (rounded down: it may return up to 1 ms early), the sub-ms rest with select()
    const int64_t rem = wake - t;
#if defined(_WIN32)
    const bool fine = rem > 0 && rem < 1'000'000 && socks.size() < FD_SETSIZE;
#else
    bool fine = rem > 0 && rem < 1'000'000;
    for (auto s : socks) fine = fine && s >= 0 && s < FD_SETSIZE;
#endif
    if (fine) wait_readable_us(socks.data(), socks.size(), readable.get(), rem / 1000);
    else net::poll_readable(socks.data(), socks.size(), readable.get(), static_cast<int>(std::max<int64_t>(0, rem / 1'000'000)));
    const int64_t now = now_ns();
    for (size_t i = 0; i < socks.size(); ++i) {
      if (!readable[i]) continue;
      if (owners[i]) owners[i]->on_readable(now);
      else waker_.drain();
    }
    for (auto& f : flows) f->tick(now);
  }
}

}  // namespace beamer::audio
