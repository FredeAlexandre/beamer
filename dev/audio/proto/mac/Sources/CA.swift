// Core Audio property helpers, device inventory, JSON output, cleanup and process stats.
import AudioToolbox
import CoreAudio
import Foundation

let systemObject = AudioObjectID(kAudioObjectSystemObject)

struct CAError: Error, CustomStringConvertible {
    let what: String
    let status: OSStatus
    var description: String { "\(what) failed: \(status) (\(fourCC(UInt32(bitPattern: status))))" }
}

func check(_ status: OSStatus, _ what: String) throws {
    if status != noErr { throw CAError(what: what, status: status) }
}

func fourCC(_ v: UInt32) -> String {
    let bytes = [UInt8((v >> 24) & 0xff), UInt8((v >> 16) & 0xff), UInt8((v >> 8) & 0xff), UInt8(v & 0xff)]
    if bytes.allSatisfy({ $0 >= 32 && $0 < 127 }) { return String(bytes: bytes, encoding: .ascii) ?? "\(v)" }
    return "\(Int32(bitPattern: v))"
}

func paddr(_ sel: AudioObjectPropertySelector,
           _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
           _ element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: element)
}

func getValue<T>(_ obj: AudioObjectID, _ address: AudioObjectPropertyAddress, _ initial: T) -> T? {
    var a = address
    var v = initial
    var size = UInt32(MemoryLayout<T>.size)
    let st = withUnsafeMutablePointer(to: &v) { AudioObjectGetPropertyData(obj, &a, 0, nil, &size, $0) }
    return st == noErr ? v : nil
}

@discardableResult
func setValue<T>(_ obj: AudioObjectID, _ address: AudioObjectPropertyAddress, _ value: T) -> OSStatus {
    var a = address
    var v = value
    return withUnsafeMutablePointer(to: &v) {
        AudioObjectSetPropertyData(obj, &a, 0, nil, UInt32(MemoryLayout<T>.size), $0)
    }
}

func getString(_ obj: AudioObjectID, _ address: AudioObjectPropertyAddress) -> String? {
    var a = address
    var size = UInt32(MemoryLayout<CFString?>.size)
    var result: Unmanaged<CFString>? = nil
    let st = withUnsafeMutablePointer(to: &result) { AudioObjectGetPropertyData(obj, &a, 0, nil, &size, $0) }
    guard st == noErr, let s = result else { return nil }
    return s.takeRetainedValue() as String
}

func getArray<T>(_ obj: AudioObjectID, _ address: AudioObjectPropertyAddress, _ zero: T) -> [T] {
    var a = address
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(obj, &a, 0, nil, &size) == noErr, size > 0 else { return [] }
    var arr = [T](repeating: zero, count: Int(size) / MemoryLayout<T>.stride)
    let st = arr.withUnsafeMutableBytes { AudioObjectGetPropertyData(obj, &a, 0, nil, &size, $0.baseAddress!) }
    guard st == noErr else { return [] }
    return Array(arr.prefix(Int(size) / MemoryLayout<T>.stride))
}

func hasProperty(_ obj: AudioObjectID, _ address: AudioObjectPropertyAddress) -> Bool {
    var a = address
    return AudioObjectHasProperty(obj, &a)
}

// MARK: - Devices

let scopeIn = kAudioObjectPropertyScopeInput
let scopeOut = kAudioObjectPropertyScopeOutput

func allDevices() -> [AudioObjectID] { getArray(systemObject, paddr(kAudioHardwarePropertyDevices), AudioObjectID(0)) }

func defaultDevice(_ sel: AudioObjectPropertySelector) -> AudioObjectID {
    getValue(systemObject, paddr(sel), AudioObjectID(0)) ?? 0
}

func deviceUID(_ d: AudioObjectID) -> String { getString(d, paddr(kAudioDevicePropertyDeviceUID)) ?? "" }
func deviceName(_ d: AudioObjectID) -> String { getString(d, paddr(kAudioObjectPropertyName)) ?? "" }
func nominalRate(_ d: AudioObjectID) -> Double { getValue(d, paddr(kAudioDevicePropertyNominalSampleRate), Float64(0)) ?? 0 }

func channelCount(_ d: AudioObjectID, _ scope: AudioObjectPropertyScope) -> Int {
    var a = paddr(kAudioDevicePropertyStreamConfiguration, scope)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(d, &a, 0, nil, &size) == noErr, size > 0 else { return 0 }
    let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
    defer { raw.deallocate() }
    guard AudioObjectGetPropertyData(d, &a, 0, nil, &size, raw) == noErr else { return 0 }
    let abl = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
    return abl.reduce(0) { $0 + Int($1.mNumberChannels) }
}

func transportName(_ t: UInt32) -> String {
    switch t {
    case kAudioDeviceTransportTypeBuiltIn: return "builtin"
    case kAudioDeviceTransportTypeAggregate: return "aggregate"
    case kAudioDeviceTransportTypeVirtual: return "virtual"
    case kAudioDeviceTransportTypeUSB: return "usb"
    case kAudioDeviceTransportTypeBluetooth: return "bluetooth"
    case kAudioDeviceTransportTypeBluetoothLE: return "bluetooth_le"
    case kAudioDeviceTransportTypeHDMI: return "hdmi"
    case kAudioDeviceTransportTypeDisplayPort: return "displayport"
    case kAudioDeviceTransportTypeAirPlay: return "airplay"
    case kAudioDeviceTransportTypeAVB: return "avb"
    case kAudioDeviceTransportTypeThunderbolt: return "thunderbolt"
    case kAudioDeviceTransportTypeFireWire: return "firewire"
    case kAudioDeviceTransportTypePCI: return "pci"
    case kAudioDeviceTransportTypeContinuityCaptureWired, kAudioDeviceTransportTypeContinuityCaptureWireless:
        return "continuity"
    case kAudioDeviceTransportTypeUnknown: return "unknown"
    default: return fourCC(t)
    }
}

func streams(_ d: AudioObjectID, _ scope: AudioObjectPropertyScope) -> [AudioObjectID] {
    getArray(d, paddr(kAudioDevicePropertyStreams, scope), AudioObjectID(0))
}

func asbdDict(_ f: AudioStreamBasicDescription) -> [String: Any] {
    let flags = f.mFormatFlags
    return [
        "rate": f.mSampleRate, "format": fourCC(f.mFormatID), "channels": Int(f.mChannelsPerFrame),
        "bits": Int(f.mBitsPerChannel), "float": flags & kAudioFormatFlagIsFloat != 0,
        "interleaved": flags & kAudioFormatFlagIsNonInterleaved == 0,
    ]
}

func latencyInfo(_ d: AudioObjectID, _ scope: AudioObjectPropertyScope) -> [String: Any] {
    var r: [String: Any] = [:]
    r["device_latency_frames"] = Int(getValue(d, paddr(kAudioDevicePropertyLatency, scope), UInt32(0)) ?? 0)
    r["safety_offset_frames"] = Int(getValue(d, paddr(kAudioDevicePropertySafetyOffset, scope), UInt32(0)) ?? 0)
    r["buffer_frames"] = Int(getValue(d, paddr(kAudioDevicePropertyBufferFrameSize), UInt32(0)) ?? 0)
    if let range = getValue(d, paddr(kAudioDevicePropertyBufferFrameSizeRange), AudioValueRange()) {
        r["buffer_frames_range"] = [range.mMinimum, range.mMaximum]
    }
    r["stream_latency_frames"] = streams(d, scope).map {
        Int(getValue($0, paddr(kAudioStreamPropertyLatency), UInt32(0)) ?? 0)
    }
    r["virtual_formats"] = streams(d, scope).compactMap {
        getValue($0, paddr(kAudioStreamPropertyVirtualFormat), AudioStreamBasicDescription()).map(asbdDict)
    }
    return r
}

func deviceInfo(_ d: AudioObjectID, verbose: Bool = false) -> [String: Any] {
    let transport = getValue(d, paddr(kAudioDevicePropertyTransportType), UInt32(0)) ?? 0
    let ins = channelCount(d, scopeIn)
    let outs = channelCount(d, scopeOut)
    var r: [String: Any] = [
        "id": Int(d), "uid": deviceUID(d), "name": deviceName(d),
        "manufacturer": getString(d, paddr(kAudioObjectPropertyManufacturer)) ?? "",
        "transport": transportName(transport), "nominal_rate": nominalRate(d),
        "input_channels": ins, "output_channels": outs,
        "is_default_input": d == defaultDevice(kAudioHardwarePropertyDefaultInputDevice),
        "is_default_output": d == defaultDevice(kAudioHardwarePropertyDefaultOutputDevice),
        "is_default_system_output": d == defaultDevice(kAudioHardwarePropertyDefaultSystemOutputDevice),
        "alive": (getValue(d, paddr(kAudioDevicePropertyDeviceIsAlive), UInt32(0)) ?? 0) != 0,
    ]
    if let rates = Optional(getArray(d, paddr(kAudioDevicePropertyAvailableNominalSampleRates), AudioValueRange())),
       !rates.isEmpty {
        r["available_rates"] = rates.map { $0.mMinimum == $0.mMaximum ? [$0.mMinimum] : [$0.mMinimum, $0.mMaximum] }
    }
    r["clock_domain"] = Int(getValue(d, paddr(kAudioDevicePropertyClockDomain), UInt32(0)) ?? 0)
    if verbose {
        if ins > 0 { r["input"] = latencyInfo(d, scopeIn) }
        if outs > 0 { r["output"] = latencyInfo(d, scopeOut) }
    }
    return r
}

func defaultsInfo() -> [String: Any] {
    func brief(_ d: AudioObjectID) -> Any { d == 0 ? NSNull() : ["uid": deviceUID(d), "name": deviceName(d)] }
    return [
        "default_input": brief(defaultDevice(kAudioHardwarePropertyDefaultInputDevice)),
        "default_output": brief(defaultDevice(kAudioHardwarePropertyDefaultOutputDevice)),
        "default_system_output": brief(defaultDevice(kAudioHardwarePropertyDefaultSystemOutputDevice)),
    ]
}

enum DeviceSelectError: Error, CustomStringConvertible {
    case notFound(String), ambiguous(String, [String])
    var description: String {
        switch self {
        case .notFound(let q): return "no matching device for '\(q)'"
        case .ambiguous(let q, let names): return "'\(q)' matches several devices: \(names.joined(separator: ", "))"
        }
    }
}

/// Resolve a device by exact UID, then by case-insensitive name substring, among devices that
/// have channels in `scope`. nil query = the given default device.
func selectDevice(_ query: String?, scope: AudioObjectPropertyScope, defaultSel: AudioObjectPropertySelector,
                  allowName: Bool) throws -> AudioObjectID {
    guard let q = query, !q.isEmpty else {
        let d = defaultDevice(defaultSel)
        if d == 0 { throw DeviceSelectError.notFound("default") }
        return d
    }
    let candidates = allDevices().filter { channelCount($0, scope) > 0 }
    if let d = candidates.first(where: { deviceUID($0) == q }) { return d }
    if allowName {
        let m = candidates.filter { deviceName($0).localizedCaseInsensitiveContains(q) }
        if m.count == 1 { return m[0] }
        if m.count > 1 { throw DeviceSelectError.ambiguous(q, m.map(deviceName)) }
    }
    throw DeviceSelectError.notFound(q)
}

// MARK: - JSON + logging

/// Minimal JSON encoder (sorted keys, shortest round-trip doubles; JSONSerialization prints
/// 0.061 as 0.060999999999999999).
func jsonString(_ obj: Any) -> String {
    var out = ""
    func str(_ s: String) {
        out += "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default:
                if u.value < 0x20 { out += String(format: "\\u%04x", u.value) } else { out.unicodeScalars.append(u) }
            }
        }
        out += "\""
    }
    func enc(_ v: Any) {
        switch v {
        case let d as [String: Any]:
            out += "{"
            for (i, k) in d.keys.sorted().enumerated() {
                if i > 0 { out += "," }
                str(k); out += ":"; enc(d[k]!)
            }
            out += "}"
        case let a as [Any]:
            out += "["
            for (i, x) in a.enumerated() { if i > 0 { out += "," }; enc(x) }
            out += "]"
        case let s as String: str(s)
        case let b as Bool: out += b ? "true" : "false"
        case let i as Int: out += String(i)
        case let d as Double:
            if !d.isFinite { out += "null" } else if d == d.rounded(), abs(d) < 1e15 { out += String(Int64(d)) } else { out += "\(d)" }
        case let f as Float: enc(Double(f))
        case is NSNull: out += "null"
        default: str("\(v)")
        }
    }
    enc(obj)
    return out
}

let stderrLock = NSLock()
func elog(_ obj: [String: Any]) {
    var o = obj
    o["t"] = round(wallSeconds() * 1000) / 1000
    let line = jsonString(o) + "\n"
    stderrLock.lock()
    line.withCString { p in _ = fputs(p, stderr) }
    fflush(stderr)
    stderrLock.unlock()
}

func fail(_ msg: String, code: Int32 = 1) -> Never {
    elog(["event": "error", "message": msg])
    finish(code)
}

// MARK: - Host time

let timebase: mach_timebase_info_data_t = {
    var tb = mach_timebase_info_data_t()
    mach_timebase_info(&tb)
    return tb
}()

func hostToSeconds(_ t: UInt64) -> Double { Double(t) * Double(timebase.numer) / Double(timebase.denom) / 1e9 }
func hostTicksPerSecond() -> Double { 1e9 * Double(timebase.denom) / Double(timebase.numer) }
func nowHost() -> UInt64 { mach_absolute_time() }

let startHost = mach_absolute_time()
func wallSeconds() -> Double { let s = startHost; return hostToSeconds(mach_absolute_time() &- s) }

func dbfs(_ x: Double) -> Double { x > 1e-10 ? (20 * log10(x) * 10).rounded() / 10 : -200 }

// MARK: - Cleanup, signals, exit stats

final class Cleanup {
    static let shared = Cleanup()
    private let lock = NSLock()
    private var actions: [(String, () -> Void)] = []
    private var done = false

    func add(_ name: String, _ action: @escaping () -> Void) {
        lock.lock(); actions.append((name, action)); lock.unlock()
    }

    /// Runs the actions once (LIFO). Returns true for the caller that actually ran them.
    func run() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if done { return false }
        done = true
        for (name, a) in actions.reversed() {
            a()
            if verboseCleanup { elog(["event": "cleanup", "step": name]) }
        }
        actions.removeAll()
        return true
    }
}

var verboseCleanup = true
var exitExtras: () -> [String: Any] = { [:] }

func cpuStats() -> [String: Any] {
    var ru = rusage()
    getrusage(RUSAGE_SELF, &ru)
    let user = Double(ru.ru_utime.tv_sec) + Double(ru.ru_utime.tv_usec) / 1e6
    let sys = Double(ru.ru_stime.tv_sec) + Double(ru.ru_stime.tv_usec) / 1e6
    let wall = wallSeconds()
    return ["cpu_user_s": (user * 1000).rounded() / 1000, "cpu_sys_s": (sys * 1000).rounded() / 1000,
            "wall_s": (wall * 1000).rounded() / 1000,
            "cpu_pct": wall > 0 ? ((user + sys) / wall * 1000).rounded() / 10 : 0,
            "max_rss_mb": Double(ru.ru_maxrss) / 1048576]
}

func finish(_ code: Int32) -> Never {
    fflush(stdout)
    if Cleanup.shared.run() {
        var o: [String: Any] = ["event": "exit", "code": Int(code)]
        o.merge(cpuStats()) { a, _ in a }
        o.merge(exitExtras()) { a, _ in a }
        elog(o)
        exit(code)
    }
    // Another thread is already finishing; let it call exit().
    while true { pause() }
}

private var signalSources: [DispatchSourceSignal] = []
private let signalQueue = DispatchQueue(label: "beamer-audio.signals")

func installSignalHandlers() {
    signal(SIGPIPE, SIG_IGN)  // stdout EPIPE comes back from write() instead
    for sig in [SIGINT, SIGTERM, SIGHUP] {
        signal(sig, SIG_IGN)
        let src = DispatchSource.makeSignalSource(signal: sig, queue: signalQueue)
        src.setEventHandler {
            elog(["event": "signal", "signal": Int(sig)])
            finish(0)
        }
        src.resume()
        signalSources.append(src)
    }
}

// MARK: - Arguments

struct Args {
    var command: [String] = []
    var options: [String: String] = [:]
    var multi: [String: [String]] = [:]  // every value of a repeated option, in order
    var flags: Set<String> = []
    static let boolFlags: Set<String> = ["json", "mute-local", "include-self", "muted-always", "verbose", "clocked"]

    init(_ argv: [String]) {
        var i = 0
        while i < argv.count {
            let a = argv[i]
            if a.hasPrefix("--") {
                var key = String(a.dropFirst(2))
                if let eq = key.firstIndex(of: "=") {
                    options[String(key[..<eq])] = String(key[key.index(after: eq)...])
                    multi[String(key[..<eq]), default: []].append(String(key[key.index(after: eq)...]))
                    i += 1
                    continue
                }
                if Args.boolFlags.contains(key) { flags.insert(key); i += 1; continue }
                if i + 1 < argv.count {
                    options[key] = argv[i + 1]
                    multi[key, default: []].append(argv[i + 1])
                    i += 2
                } else { key = ""; i += 1 }
            } else {
                command.append(a)
                i += 1
            }
        }
    }

    func double(_ k: String, _ d: Double) -> Double { options[k].flatMap(Double.init) ?? d }
    func int(_ k: String, _ d: Int) -> Int { options[k].flatMap(Int.init) ?? d }
    func has(_ k: String) -> Bool { flags.contains(k) }
    func all(_ k: String) -> [String] { multi[k] ?? [] }
}
