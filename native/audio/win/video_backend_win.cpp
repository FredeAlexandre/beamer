// SPDX-License-Identifier: GPL-3.0-only
// VideoBackend for Windows (camera PHASE2 §2.4, §4.6, CW4): a virtual-camera sink, no capture.
//
// The sink (one at a time) decodes on its own decoder thread: decode() only queues the AU, so the engine's presenter
// (present/hold/show_placeholder, which write the heartbeat) never waits on the GPU. On lebig a hardware MFT call
// sometimes blocked 0.4-1.7 s under outside GPU load; on the presenter that stopped the heartbeat and the DLL showed
// its built-in image. The decoder thread runs ProcessInput, drains, reads back into one of 12 NV12 pool pictures at
// the display size (§4.6.1) and calls on_decoded; on_decoded / on_sink_event therefore come from that thread (the
// engine posts them to its mailbox, video_recv.h). The MFT and its D3D11 device are created on the backend's worker
// thread: the first decoder at open_sink, a reopen after 3 errors in 10 s, software after a device loss, and the
// hardware retry 30 s later. An IDR arriving while a decoder is being made is kept (with the AUs behind it, <= 1 s)
// and decoded when it is ready; other AUs without a decoder are reported on_decoded(ok=false). A new decoder reports
// "decoder_hw" / "decoder_sw" and is fed nothing before an IDR. The hardware retry is seamless: software keeps
// decoding until the hardware decoder gets its IDR.
#include "video_backend_win.h"

#include <mferror.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>

namespace beamer::audio {

namespace win {

namespace {
int64_t ms_to_qpc(int ms) { return int64_t(ms) * qpc_freq() / 1000; }
bool newer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }  // frame-number serial order
constexpr size_t kPoolSize = 12;  // (D 150 + hold 150 ms) / 33 ms + 2 (§4.6.1)
}  // namespace

// ------------------------------------------------------------------ backend
WinVideoBackend::WinVideoBackend(const WinVideoOptions& o) : opt_(o), vcam_(std::make_unique<Vcam>(o)) {}

WinVideoBackend::~WinVideoBackend() {
    vcam_->stop();
    {
        std::lock_guard<std::mutex> g(jmu_);
        jquit_ = true;
    }
    jcv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void WinVideoBackend::set_listener(VideoBackendListener* l) {
    std::lock_guard<std::mutex> g(mu_);
    if (!l) {
        vcam_->detach_listener();  // the helper is stopping
        return;
    }
    if (started_) return;
    started_ = true;
    vcam_->start(l);  // the helper is starting (`ready`): open the objects, create the camera when installed
}

std::unique_ptr<VideoSinkHandle> WinVideoBackend::open_sink(VideoSinkListener* l, std::string* err) {
    std::lock_guard<std::mutex> g(mu_);
    if (sink_) {
        if (err) *err = "internal: sink open";
        return nullptr;
    }
    if (!mf_startup()) {
        if (err) *err = "unsupported: Media Foundation is not available";
        return nullptr;
    }
    auto s = std::make_unique<WinVideoSink>(this, l);
    sink_ = s.get();
    vcam_->sink_opened();
    return s;
}

void WinVideoBackend::sink_gone(WinVideoSink* s) {
    std::lock_guard<std::mutex> g(mu_);
    if (sink_ != s) return;
    sink_ = nullptr;
    vcam_->sink_closed();
}

void WinVideoBackend::post_job(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> g(jmu_);
        jobs_.push_back(std::move(job));
        if (!worker_.joinable()) worker_ = std::thread([this] { worker(); });
    }
    jcv_.notify_one();
}

void WinVideoBackend::worker() {
    SetThreadDescription(GetCurrentThread(), L"video-decoder-worker");
    ComScope com;
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> g(jmu_);
            jcv_.wait(g, [&] { return jquit_ || !jobs_.empty(); });
            if (jquit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

// ------------------------------------------------------------------ sink
WinVideoSink::WinVideoSink(WinVideoBackend* b, VideoSinkListener* l) : b_(b), l_(l), sh_(std::make_shared<Shared>()) {
    request_decoder(b_->options().decoder != "sw", "open");
    dth_ = std::thread([this] { run_decoder(); });
}

void WinVideoSink::run_decoder() {
    SetThreadDescription(GetCurrentThread(), L"video-decoder");
    ComScope com;
    for (;;) {
        Job j;
        bool have = false;
        {
            std::unique_lock<std::mutex> g(qmu_);
            qcv_.wait_for(g, std::chrono::milliseconds(100), [&] { return qquit_ || !q_.empty(); });
            if (qquit_) return;
            if (!q_.empty()) {
                j = std::move(q_.front());
                q_.pop_front();
                have = true;
            }
        }
        adopt_pending();
        maybe_retry_hw(qpc_now());
        if (have) process(j);
    }
}

WinVideoSink::~WinVideoSink() {
    {
        std::lock_guard<std::mutex> g(qmu_);
        qquit_ = true;
    }
    qcv_.notify_all();
    if (dth_.joinable()) dth_.join();
    {
        std::lock_guard<std::mutex> g(sh_->mu);
        sh_->dead = true;
        sh_->pending.reset();
    }
    b_->sink_gone(this);
}

void WinVideoSink::request_decoder(bool hw, const char* why) {
    {
        std::lock_guard<std::mutex> g(sh_->mu);
        if (sh_->running) return;
        sh_->running = true;
    }
    const bool fallback = b_->options().decoder == "auto";
    std::string reason = why;
    BLOG_I("video sink: creating %s decoder (%s)", hw ? "hardware" : "software", why);
    b_->post_job([sh = sh_, hw, fallback, reason] {
        const auto t0 = std::chrono::steady_clock::now();
        std::string info, info2;
        HRESULT hr = S_OK;
        auto d = H264Decoder::create(hw, &info, &hr);
        // Software fallback when there is no D3D11 video device (§4.6.1), except for the hardware retry, which
        // keeps the software decoder that is already running.
        if (!d && hw && fallback && reason != "hardware retry") d = H264Decoder::create(false, &info2, &hr);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> g(sh->mu);
        sh->running = false;
        if (sh->dead) return;
        if (d) {
            sh->info = d->info() + " (" + reason + ", " + std::to_string(int(ms + 0.5)) + " ms" +
                       (info2.empty() && hw == d->hw() ? "" : "; hardware failed: " + info) + ")";
            sh->pending = std::move(d);
        } else {
            sh->fail = info + (info2.empty() ? "" : "; " + info2);
        }
    });
}

void WinVideoSink::set_kind() {
    kind_ = dec_ ? (dec_->hw() ? 1 : 2) : 0;
}

void WinVideoSink::adopt_pending() {
    std::unique_ptr<H264Decoder> d;
    std::string info, fail;
    {
        std::lock_guard<std::mutex> g(sh_->mu);
        if (!sh_->pending && sh_->fail.empty()) return;
        d = std::move(sh_->pending);
        info.swap(sh_->info);
        fail.swap(sh_->fail);
    }
    const int64_t now = qpc_now();
    if (!d) {
        BLOG_W("video sink: decoder creation failed: %s", fail.c_str());
        flush_backlog(false);
        if (sw_after_loss_ && dec_) {
            hw_retry_at_ = now + ms_to_qpc(b_->options().hw_retry_ms);  // still no GPU: try again later
        } else if (!dec_) {
            retry_create_at_ = now + ms_to_qpc(2000);
            {
                std::lock_guard<std::mutex> g(cmu_);
                c_.decode_errors++;
            }
        }
        return;
    }
    if (d->hw() && dec_ && !dec_->hw() && sw_after_loss_) {
        // Back to hardware after a device loss: keep software running until the hardware decoder gets an IDR.
        standby_hw_ = std::move(d);
        BLOG_I("video sink: hardware decoder ready, waiting for an IDR: %s", info.c_str());
        l_->on_sink_event("decoder_hw", info);
        return;
    }
    dec_ = std::move(d);
    awaiting_idr_ = true;
    rejected_size_.clear();
    set_kind();
    BLOG_I("video sink: decoder ready: %s", info.c_str());
    l_->on_sink_event(dec_->hw() ? "decoder_hw" : "decoder_sw", info);
    flush_backlog(true);
}

void WinVideoSink::flush_backlog(bool decode) {
    std::vector<Held> b;
    b.swap(backlog_);
    for (auto& h : b) {
        if (decode && dec_) decode_one(h.au.data(), h.au.size(), h.frame, h.keep, au_has_idr(h.au.data(), h.au.size()));
        else decoded(h.frame, false, 0);
    }
}

void WinVideoSink::maybe_retry_hw(int64_t now) {
    if (!dec_ && retry_create_at_ && now >= retry_create_at_) {
        retry_create_at_ = 0;
        request_decoder(b_->options().decoder != "sw", "retry");
    }
    if (sw_after_loss_ && !standby_hw_ && hw_retry_at_ && now >= hw_retry_at_) {
        hw_retry_at_ = 0;
        request_decoder(true, "hardware retry");
    }
}

WinVideoSink::Pic* WinVideoSink::pic_for_new(uint32_t frame, int w, int h) {
    std::lock_guard<std::mutex> pg(pmu_);
    Pic* got = nullptr;
    if (pool_.size() != kPoolSize || pool_w_ != w || pool_h_ != h) {
        pool_.assign(kPoolSize, Pic());
        for (auto& p : pool_) p.nv12.resize(size_t(w) * h * 3 / 2);
        pool_w_ = w;
        pool_h_ = h;
    }
    for (auto& p : pool_)
        if (p.used && p.frame == frame) p.used = false;  // re-decode of the same number replaces it
    for (auto& p : pool_)
        if (!p.used && !got) got = &p;
    if (!got) {
        // Pool exhausted: the oldest unpresented kept picture is dropped, never shown (counted skipped).
        got = &pool_[0];
        for (auto& p : pool_)
            if (newer(got->frame, p.frame)) got = &p;
        got->used = false;
        std::lock_guard<std::mutex> g(cmu_);
        c_.skipped++;
    }
    got->busy = true;  // being written outside the lock; the presenter only reads `used` pictures
    return got;
}

void WinVideoSink::decoded(uint32_t frame, bool ok, double ms) {
    l_->on_decoded(frame, ok, ms);
}

void WinVideoSink::on_error(uint32_t frame, HRESULT hr, double ms) {
    const int64_t now = qpc_now();
    {
        std::lock_guard<std::mutex> g(cmu_);
        c_.decode_errors++;
    }
    errors_.push_back(now);
    while (!errors_.empty() && now - errors_.front() > ms_to_qpc(10000)) errors_.pop_front();
    decoded(frame, false, ms);
    if (errors_.size() < 3 || !dec_) return;
    // 3 decoder errors within 10 s: reopen (§4.5.6); software if the GPU path keeps failing.
    errors_.clear();
    bool hw = dec_->hw();
    if (hw && last_hw_reopen_ && now - last_hw_reopen_ < ms_to_qpc(30000)) hw = false;
    if (hw) last_hw_reopen_ = now;
    BLOG_W("video sink: 3 decoder errors in 10 s (last %s): reopening (%s)", hr_text(hr).c_str(), hw ? "hardware" : "software");
    {
        std::lock_guard<std::mutex> g(cmu_);
        c_.reopens++;
    }
    dec_.reset();
    set_kind();
    if (!hw && b_->options().decoder != "sw") {
        sw_after_loss_ = true;
        hw_retry_at_ = now + ms_to_qpc(b_->options().hw_retry_ms);
    }
    request_decoder(hw, "3 errors");
}

void WinVideoSink::decode(const uint8_t* au, size_t len, uint32_t frame, bool keep) {
    std::vector<uint32_t> dropped;
    {
        std::lock_guard<std::mutex> g(qmu_);
        // A decoder stuck for 10 s (300 AUs): the oldest queued AUs are given up (the engine sees decoder errors).
        while (q_.size() >= 300) {
            dropped.push_back(q_.front().frame);
            q_.pop_front();
        }
        q_.push_back(Job{std::vector<uint8_t>(au, au + len), frame, keep});
    }
    qcv_.notify_one();
    for (uint32_t f : dropped) decoded(f, false, 0);
}

void WinVideoSink::process(const Job& j) {
    const uint8_t* au = j.au.data();
    const size_t len = j.au.size();
    const uint32_t frame = j.frame;
    const bool keep = j.keep;
    const bool idr = au_has_idr(au, len);
    if (standby_hw_ && idr) {
        dec_ = std::move(standby_hw_);
        sw_after_loss_ = false;
        awaiting_idr_ = true;
        set_kind();
        BLOG_I("video sink: back to the hardware decoder at frame %u", frame);
    }
    if (fake_remove_.exchange(false) && dec_ && dec_->hw()) dec_->simulate_device_removed();
    if (!dec_) {
        bool making;
        {
            std::lock_guard<std::mutex> g(sh_->mu);
            making = sh_->running || sh_->pending;
        }
        if (making && (idr || !backlog_.empty()) && backlog_.size() < 30) {
            if (idr) flush_backlog(false);
            backlog_.push_back(Held{std::vector<uint8_t>(au, au + len), frame, keep});
            std::lock_guard<std::mutex> g(cmu_);
            c_.backlogged++;
            return;  // on_decoded comes when the decoder is ready
        }
    }
    decode_one(au, len, frame, keep, idr);
}

void WinVideoSink::decode_one(const uint8_t* au, size_t len, uint32_t frame, bool keep, bool idr) {
    const int64_t now = qpc_now();
    if (!dec_ || (awaiting_idr_ && !idr)) {
        // No decoder (being made, nothing to keep yet) or a new decoder before its IDR: nothing to decode against.
        {
            std::lock_guard<std::mutex> g(cmu_);
            c_.discarded++;
        }
        decoded(frame, false, 0);
        return;
    }
    awaiting_idr_ = false;
    Pic* target = nullptr;
    DecodeOut r = dec_->decode(au, len, frame, [&](int w, int h) -> uint8_t* {
        if (!keep) return nullptr;
        target = pic_for_new(frame, w, h);
        return target->nv12.data();
    });
    {
        std::lock_guard<std::mutex> g(cmu_);
        c_.w = r.w;
        c_.h = r.h;
        c_.coded_h = r.coded_h;
    }
    if (target) {
        std::lock_guard<std::mutex> pg(pmu_);
        target->busy = false;
        target->used = r.result == DecodeResult::kOk;
        target->frame = frame;
    }
    switch (r.result) {
        case DecodeResult::kOk:
            rejected_size_.clear();
            {
                std::lock_guard<std::mutex> g(cmu_);
                c_.decoded++;
            }
            decoded(frame, true, r.ms);
            return;
        case DecodeResult::kSizeRejected: {
            const std::string sz = std::to_string(r.w) + "x" + std::to_string(r.h);
            {
                std::lock_guard<std::mutex> g(cmu_);
                c_.size_rejected++;
            }
            // Once per new output type and once per IDR at that size: the engine counts 3 within 10 s (§4.5.6).
            if (idr || r.size_changed || rejected_size_ != sz) l_->on_sink_event("size_rejected", sz);
            rejected_size_ = sz;
            decoded(frame, false, r.ms);
            return;
        }
        case DecodeResult::kDeviceLost: {
            {
                std::lock_guard<std::mutex> g(cmu_);
                c_.device_lost++;
            }
            BLOG_W("video sink: decoder device lost (%s): software now, hardware again in %d ms", hr_text(r.hr).c_str(),
                   b_->options().hw_retry_ms);
            l_->on_sink_event("device_lost", hr_text(r.hr));
            dec_.reset();
            standby_hw_.reset();
            set_kind();
            sw_after_loss_ = true;
            hw_retry_at_ = now + ms_to_qpc(b_->options().hw_retry_ms);
            request_decoder(false, "device lost");
            decoded(frame, false, r.ms);
            return;
        }
        case DecodeResult::kNoOutput:
        case DecodeResult::kError:
            on_error(frame, r.result == DecodeResult::kNoOutput ? MF_E_TRANSFORM_NEED_MORE_INPUT : r.hr, r.ms);
            return;
    }
}

void WinVideoSink::present(uint32_t frame) {
    std::lock_guard<std::mutex> pg(pmu_);
    Pic* pic = nullptr;
    for (auto& p : pool_)
        if (p.used && p.frame == frame) pic = &p;
    if (!pic) {
        b_->vcamref().sink_hold();  // revoked or never kept: keep showing what is shown
        return;
    }
    b_->vcamref().sink_present(pic->nv12.data(), pool_w_, pool_h_);
    // Never backwards (§4.5.4): this picture and every older one are done.
    for (auto& p : pool_)
        if (p.used && !newer(p.frame, frame)) p.used = false;
    std::lock_guard<std::mutex> g(cmu_);
    c_.presented++;
}

void WinVideoSink::hold() { b_->vcamref().sink_hold(); }

void WinVideoSink::drop(uint32_t frame) {
    std::lock_guard<std::mutex> pg(pmu_);
    for (auto& p : pool_)
        if (p.used && p.frame == frame) p.used = false;
}

void WinVideoSink::show_placeholder(const std::string& state) {
    b_->vcamref().sink_placeholder(state);
}

void WinVideoSink::simulate_device_removed() { fake_remove_ = true; }

std::string WinVideoSink::decoder_kind() {
    int k = kind_.load();
    return k == 1 ? "hw" : k == 2 ? "sw" : "";
}

SinkCounters WinVideoSink::counters() {
    std::lock_guard<std::mutex> g(cmu_);
    return c_;
}

}  // namespace win

// ------------------------------------------------------------------ public surface
WinVideoOptions win_video_options_from_args() {
    WinVideoOptions o;
    int n = 0;
    std::string decoder;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; i++) {
            std::string a = win::utf8(w[i]);
            if (a == "--dev") o.dev = true;
            else if (a.rfind("--video-decoder=", 0) == 0) decoder = a.substr(16);
        }
        LocalFree(w);
    }
    if (o.dev && (decoder == "sw" || decoder == "hw" || decoder == "auto")) o.decoder = decoder;
    return o;
}

std::unique_ptr<VideoBackend> make_win_video_backend(const WinVideoOptions& options) {
    return std::make_unique<win::WinVideoBackend>(options);
}

}  // namespace beamer::audio
