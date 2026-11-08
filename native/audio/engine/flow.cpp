// SPDX-License-Identifier: GPL-3.0-only
#include "flow.h"

#include <algorithm>
#include <cstring>

#include "streams.h"
#include "util.h"

namespace beamer::audio {

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

void Flow::detach(uint32_t id) {
  std::lock_guard<std::mutex> lock(streams_mu_);
  recv_.erase(id);
  send_.erase(id);
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
    if (h.ptype != kHello && h.ptype != kPing && h.ptype != kPong && h.ptype != kBye) {
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
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    if (h.ptype == kMedia) {
      auto it = recv_.find(h.stream);
      if (it != recv_.end()) {
        known = true;
        r = it->second->ingest(data, len, now, via_link);
      }
    } else if (h.ptype == kRR) {
      auto it = send_.find(h.stream);
      if (it != send_.end()) {
        known = true;
        r = it->second->ingest_rr(data, len, now);
      }
    }
  }
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
  if (now >= next_flush_) {
    next_flush_ = now + timing.link_flush_ns;
    if (link_mode()) flush_link_out();
  }
}

int64_t Flow::next_deadline(int64_t now) const {
  // link mode (queues to flush, link_in to process) and the very start: the 2 ms granularity
  if (link_only_ || link_mode() || remote_set_ns_ == 0) return now + 2'000'000;
  int64_t d = std::min(next_hello_, now + 10'000'000);
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

// ------------------------------------------------------------------ NetLoop

void NetLoop::start() {
  if (running_) return;
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
    for (auto& f : flows) {
      if (f->sock() != net::kInvalid && socks.size() < 256) {
        socks.push_back(f->sock());
        owners.push_back(f.get());
      }
    }
    for (size_t i = 0; i < socks.size(); ++i) readable[i] = false;
    // sleep until the earliest flow timer (<= 10 ms; 2 ms in link mode); packets wake us earlier
    int64_t t = now_ns(), wake = t + 10'000'000;
    for (auto& f : flows) wake = std::min(wake, f->next_deadline(t));
    int timeout_ms = static_cast<int>(std::max<int64_t>(0, (wake - t + 999'999) / 1'000'000));
    net::poll_readable(socks.data(), socks.size(), readable.get(), timeout_ms);
    const int64_t now = now_ns();
    for (size_t i = 0; i < socks.size(); ++i)
      if (readable[i]) owners[i]->on_readable(now);
    for (auto& f : flows) f->tick(now);
  }
}

}  // namespace beamer::audio
