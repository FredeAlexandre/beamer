namespace beamer::audio {
constexpr int kRate = 48000;                      // the engine works in 48 kHz float32 only
enum class Form { Unknown, Microphone, Speakers, Headphones, Headset };
enum class Role { Normal, VirtualMic, VirtualMicFeed };  // blocked devices are never listed
struct DeviceInfo {
  std::string key, name, manufacturer, transport;  // transport: "builtin","usb","bluetooth","virtual","hdmi","other"
  bool input = false, output = false;
  int channels = 0; double nominal_rate = 0; Form form = Form::Unknown; Role role = Role::Normal;
};
struct DeviceSnapshot {
  std::vector<DeviceInfo> devices;
  std::string default_input, default_output;      // keys, "" = none
  std::string vmic_kind;                          // "beamer" | "vbcable" | ""
  bool vmic_present = false, vmic_in_use = false; // in_use = some other process records the virtual mic
};
struct CaptureSink {   // called on the backend's IO thread: no locks, no allocation, no syscalls
  virtual void on_capture(const float* interleaved, int frames, int channels, uint64_t host_ns) = 0;
  virtual ~CaptureSink() = default;
};
struct RenderSource {  // called on the backend's IO thread; must fill exactly `frames`
  virtual void on_render(float* interleaved, int frames, int channels, uint64_t host_ns) = 0;
  virtual ~RenderSource() = default;
};
enum class CaptureKind { Mic, System, Reference };  // Reference = AEC reference of an output device
struct CaptureSpec { CaptureKind kind; std::string device;  // "" = default (followed on change)
                     int channels; bool mute_local = false; std::vector<int64_t> exclude_pids; };
struct RenderSpec  { std::string device;  // "" = default output (followed), or the key
                     bool virtual_mic = false; int channels; };
struct StreamHandle { virtual ~StreamHandle() = default; virtual std::string device_in_use() const = 0; };
struct BackendListener {
  virtual void devices_changed() = 0;                      // debounced 300 ms by the backend
  virtual void stream_lost(StreamHandle*, const std::string& why) = 0;  // device gone/invalidated
  virtual ~BackendListener() = default;
};
class Backend {
 public:
  virtual ~Backend() = default;
  virtual void set_listener(BackendListener*) = 0;
  virtual DeviceSnapshot snapshot() = 0;
  virtual std::unique_ptr<StreamHandle> open_capture(const CaptureSpec&, CaptureSink*, std::string* err) = 0;
  virtual std::unique_ptr<StreamHandle> open_render(const RenderSpec&, RenderSource*, std::string* err) = 0;
  virtual int restore_journal() = 0;                       // Windows endpoint mutes; Mac returns 0
  virtual std::vector<std::string> features() = 0;         // §3.3 feature words
  virtual std::string permission(const std::string& what) = 0;  // "mic" -> granted|denied|undetermined|unknown
};
std::unique_ptr<Backend> make_null_backend(const std::string& options);  // engine-provided, for tests
int run_helper(Backend& backend, int argc, char** argv);                 // engine-provided main loop
}
