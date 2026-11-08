// SPDX-License-Identifier: GPL-3.0-only
// Windows backend: on-demand detection for the virtual mic (PHASE2 §6.3). An app records "CABLE Output" when it has an
// audio session on that capture endpoint in state Active: IAudioSessionManager2 + IAudioSessionNotification for new
// sessions, IAudioSessionEvents::OnStateChanged per session. This process's own sessions never count.
#include "backend_win.h"

#include <algorithm>

namespace beamer::audio::win {

namespace {
struct Session;
}

struct SessionWatcher::Impl : public IAudioSessionNotification {
    std::atomic<ULONG> ref{1};
    SessionWatcher* owner = nullptr;
    std::mutex m;
    ComPtr<IAudioSessionManager2> mgr;
    std::string id;
    std::vector<Session*> sessions;
    bool registered = false;

    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IAudioSessionNotification)) { *p = static_cast<IAudioSessionNotification*>(this); AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --ref; if (!n) delete this; return n; }
    STDMETHODIMP OnSessionCreated(IAudioSessionControl* s) override;

    void add(IAudioSessionControl* s);
    void recompute();
    void clear();
};

namespace {
struct Session : public IAudioSessionEvents {
    std::atomic<ULONG> ref{1};
    SessionWatcher::Impl* impl = nullptr;  // strong ref (AddRef'd), dropped in Impl::clear()
    ComPtr<IAudioSessionControl2> ctl;
    DWORD pid = 0;
    std::atomic<int> state{AudioSessionStateInactive};
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IAudioSessionEvents)) { *p = static_cast<IAudioSessionEvents*>(this); AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --ref; if (!n) delete this; return n; }
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState s) override {
        state = (int)s;
        notify();
        return S_OK;
    }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
        state = (int)AudioSessionStateExpired;
        notify();
        return S_OK;
    }
    void notify();
    ~Session();
};
}  // namespace

// Holds a strong ref on the impl only while a callback is being processed.
struct SessionWatcherAccess {
    static void changed(SessionWatcher::Impl* impl) { impl->recompute(); }
};

Session::~Session() {
    if (impl) impl->Release();
}

void Session::notify() {
    if (impl) SessionWatcherAccess::changed(impl);  // impl stays alive: this session holds a ref until clear()
}

STDMETHODIMP SessionWatcher::Impl::OnSessionCreated(IAudioSessionControl* s) {
    add(s);
    recompute();
    return S_OK;
}

void SessionWatcher::Impl::add(IAudioSessionControl* s) {
    auto* x = new Session();
    if (FAILED(s->QueryInterface(IID_PPV_ARGS(&x->ctl)))) { x->Release(); return; }
    AddRef();
    x->impl = this;
    x->ctl->GetProcessId(&x->pid);
    // Register first, then read the state, so a transition between the two is not lost.
    if (FAILED(x->ctl->RegisterAudioSessionNotification(x))) { x->impl = nullptr; Release(); x->Release(); return; }
    AudioSessionState st = AudioSessionStateInactive;
    if (SUCCEEDED(x->ctl->GetState(&st))) x->state = (int)st;
    std::lock_guard<std::mutex> g(m);
    sessions.push_back(x);
}

void SessionWatcher::Impl::recompute() {
    bool use = false;
    int active = 0;
    SessionWatcher* o;
    {
        std::lock_guard<std::mutex> g(m);
        o = owner;
        const DWORD self = GetCurrentProcessId();
        for (auto* s : sessions)
            if (s->state.load() == AudioSessionStateActive && s->pid != self) { use = true; active++; }
    }
    if (!o) return;
    bool was = o->in_use_.exchange(use);
    if (was != use) {
        BLOG_D("virtual mic %s (%d active recorder session(s))", use ? "in use" : "idle", active);
        if (o->on_change_) o->on_change_(use);
    }
}

void SessionWatcher::Impl::clear() {
    std::vector<Session*> old;
    {
        std::lock_guard<std::mutex> g(m);
        old.swap(sessions);
    }
    if (mgr && registered) mgr->UnregisterSessionNotification(this);
    registered = false;
    for (auto* s : old) {
        s->ctl->UnregisterAudioSessionNotification(s);
        s->Release();  // its ref on this impl is dropped when the session object itself goes (see ~Session)
    }
    mgr.Reset();
    id.clear();
}

SessionWatcher::SessionWatcher(std::function<void(bool)> on_change) : on_change_(std::move(on_change)) {}

SessionWatcher::~SessionWatcher() { detach(); }

bool SessionWatcher::attach(IMMDevice* capture_endpoint, const std::string& id) {
    detach();
    auto* impl = new Impl();
    impl->owner = this;
    HRESULT hr = capture_endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &impl->mgr);
    if (FAILED(hr)) {
        BLOG_W("virtual mic demand: IAudioSessionManager2 failed %s", hr_text(hr).c_str());
        impl->Release();
        return false;
    }
    impl->id = id;
    if (SUCCEEDED(impl->mgr->RegisterSessionNotification(impl))) impl->registered = true;
    // Enumerating is also what starts the delivery of OnSessionCreated.
    ComPtr<IAudioSessionEnumerator> en;
    if (SUCCEEDED(impl->mgr->GetSessionEnumerator(&en))) {
        int n = 0;
        en->GetCount(&n);
        for (int i = 0; i < n; i++) {
            ComPtr<IAudioSessionControl> s;
            if (SUCCEEDED(en->GetSession(i, &s))) impl->add(s.Get());
        }
    }
    impl_.reset(impl);
    impl->recompute();
    return impl->registered;
}

void SessionWatcher::detach() {
    if (!impl_) return;
    Impl* impl = impl_.release();
    {
        std::lock_guard<std::mutex> g(impl->m);
        impl->owner = nullptr;
    }
    impl->clear();
    impl->Release();
    if (in_use_.exchange(false) && on_change_) on_change_(false);
}

std::string SessionWatcher::attached_id() const { return impl_ ? impl_->id : std::string(); }

int SessionWatcher::active_sessions() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> g(impl_->m);
    int n = 0;
    for (auto* s : impl_->sessions) n += s->state.load() == AudioSessionStateActive && s->pid != GetCurrentProcessId();
    return n;
}

}  // namespace beamer::audio::win
