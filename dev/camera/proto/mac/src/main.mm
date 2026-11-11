// beamer-camera (Phase-1 prototype): command line, shared helpers.
#import "common.h"
#include <sys/resource.h>
#include <mach/mach.h>
#include <cstdarg>
#include <cmath>
#include <csignal>

static mach_timebase_info_data_t g_tb;

uint64_t host_to_ns(uint64_t h) {
  if (!g_tb.denom) mach_timebase_info(&g_tb);
  return (uint64_t)((__uint128_t)h * g_tb.numer / g_tb.denom);
}
uint64_t ns_to_host(uint64_t ns) {
  if (!g_tb.denom) mach_timebase_info(&g_tb);
  return (uint64_t)((__uint128_t)ns * g_tb.denom / g_tb.numer);
}
uint64_t now_ns() { return host_to_ns(mach_absolute_time()); }
double cmtime_to_host_ns(CMTime t) {
  // AVCaptureSession stamps with the host time clock: value/timescale seconds of mach time.
  return CMTimeGetSeconds(t) * 1e9;
}

static std::mutex g_err_mu;
void emit(NSDictionary *d) {
  NSError *e = nil;
  NSData *j = [NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingSortedKeys error:&e];
  std::lock_guard<std::mutex> lk(g_err_mu);
  if (j) {
    fwrite(j.bytes, 1, j.length, stderr);
    fputc('\n', stderr);
  } else {
    fprintf(stderr, "error: json: %s\n", e.localizedDescription.UTF8String);
  }
  fflush(stderr);
}
void infof(const char *fmt, ...) {
  std::lock_guard<std::mutex> lk(g_err_mu);
  va_list ap;
  va_start(ap, fmt);
  fputs("info: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  fflush(stderr);
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  size_t i = (size_t)std::ceil(p / 100.0 * v.size());
  if (i < 1) i = 1;
  if (i > v.size()) i = v.size();
  return v[i - 1];
}
double mean(const std::vector<double> &v) {
  if (v.empty()) return -1;
  double s = 0;
  for (double x : v) s += x;
  return s / v.size();
}
double cpu_seconds() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
double rss_mb() {
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return -1;
  return info.resident_size / 1048576.0;
}

static void usage() {
  fprintf(stderr,
"beamer-camera (Phase-1 prototype)\n"
"  list [--json]                      cameras, formats, video-effect state, permission\n"
"  auth [--request] [--auth-timeout S] camera permission status (--request asks, never waits longer)\n"
"  encoders                           VideoToolbox H.264 encoders and supported properties\n"
"  run [options]                      capture (camera|synthetic) -> H.264 Annex-B + trace\n"
"    --source synthetic|camera  --device UID|NAME  --size 1280x720  --fps 30  --seconds 10\n"
"    --bitrate 2500000  --limit F (DataRateLimits = F x bitrate per 1 s)  --out FILE|-  --trace FILE\n"
"    --profile chigh|high|main|cbaseline|baseline  --no-lowlatency  --no-realtime  --speed\n"
"    --preset vc|hs|bal|hq  --keyint N  --force-key-every N  --force-key-at a,b,c\n"
"    --ltr  --ltr-refresh-at a,b  --ltr-ack-delay N  --max-qp N  --verify  --verify-drop a,b  --verify-from N  --no-encode  --fast  --noise N\n"
"    --pin-fps  --cycles N  --gap S  --format-index I  --pixfmt 420v|420f|yuvs|2vuy|BGRA  --hw  --effects  --stats S  --tag T\n"
"    --encoder-id ID  --prop Key=Value (any VT session property, repeatable; true/false/number/string)\n");
}

static std::vector<int> parse_list(const char *s) {
  std::vector<int> v;
  for (NSString *p in [@(s) componentsSeparatedByString:@","])
    if (p.length) v.push_back(p.intValue);
  return v;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1];
    Opts o;
    bool json = false, request = false;
    for (int i = 2; i < argc; i++) {
      std::string a = argv[i];
      auto next = [&]() -> const char * {
        if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
        return argv[++i];
      };
      if (a == "--json") json = true;
      else if (a == "--request") request = true;
      else if (a == "--source") o.source = next();
      else if (a == "--device") o.device = next();
      else if (a == "--size") { if (sscanf(next(), "%dx%d", &o.width, &o.height) != 2) { usage(); return 2; } }
      else if (a == "--fps") o.fps = atof(next());
      else if (a == "--seconds") o.seconds = atof(next());
      else if (a == "--bitrate") o.bitrate = atoll(next());
      else if (a == "--limit") o.limit_factor = atof(next());
      else if (a == "--out") o.out = next();
      else if (a == "--trace") o.trace = next();
      else if (a == "--profile") o.profile = next();
      else if (a == "--no-lowlatency") o.lowlatency = false;
      else if (a == "--no-realtime") o.realtime = false;
      else if (a == "--speed") o.speed = true;
      else if (a == "--preset") o.preset = next();
      else if (a == "--keyint") o.keyint = atoi(next());
      else if (a == "--force-key-every") o.force_key_every = atoi(next());
      else if (a == "--force-key-at") o.force_key_at = parse_list(next());
      else if (a == "--ltr") o.ltr = true;
      else if (a == "--ltr-ack-delay") o.ltr_ack_delay = atoi(next());
      else if (a == "--ltr-refresh-at") o.ltr_refresh_at = parse_list(next());
      else if (a == "--max-qp") o.max_qp = atoi(next());
      else if (a == "--verify") o.verify = true;
      else if (a == "--verify-drop") { o.verify = true; o.verify_drop = parse_list(next()); }
      else if (a == "--verify-from") { o.verify = true; o.verify_from = atoi(next()); }
      else if (a == "--no-encode") o.encode = false;
      else if (a == "--fast") o.fast = true;
      else if (a == "--noise") o.noise = atoi(next());
      else if (a == "--pin-fps") o.pin_fps = true;
      else if (a == "--format-index") o.format_index = atoi(next());
      else if (a == "--pixfmt") o.pixfmt = next();
      else if (a == "--auth-timeout") o.auth_timeout = atof(next());
      else if (a == "--stats") o.stats_every = atof(next());
      else if (a == "--hw") o.hw_required = true;
      else if (a == "--effects") o.effects_probe = true;
      else if (a == "--tag") o.tag = next();
      else if (a == "--encoder-id") o.encoder_id = next();
      else if (a == "--cycles") o.cycles = atoi(next());
      else if (a == "--gap") o.gap = atof(next());
      else if (a == "--prop") {
        std::string kv = next();
        size_t eq = kv.find('=');
        if (eq == std::string::npos) { usage(); return 2; }
        o.props.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
      }
      else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (cmd == "list") return cmd_list(o, json);
    if (cmd == "auth") return cmd_auth(o, request);
    if (cmd == "encoders") return cmd_encoders();
    if (cmd == "run") return o.source == "camera" ? cmd_run_camera(o) : cmd_run_synthetic(o);
    usage();
    return 2;
  }
}
