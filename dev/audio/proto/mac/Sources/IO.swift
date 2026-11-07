// IOProc wrapper, format conversion (channel map + AudioConverter resampling) and process taps.
import AudioToolbox
import CoreAudio
import Foundation

// MARK: - IODevice

/// One AudioDeviceIOProc on one device, backed by the C real-time context.
final class IODevice {
    let device: AudioObjectID
    let isInput: Bool
    let channels: Int
    let rate: Double
    let ctx: OpaquePointer
    private var procID: AudioDeviceIOProcID?
    private var running = false

    init(device: AudioObjectID, isInput: Bool, ringFrames: Int, prefillFrames: Int = 0, logRecords: Int = 0) throws {
        self.device = device
        self.isInput = isInput
        channels = channelCount(device, isInput ? scopeIn : scopeOut)
        if channels == 0 { throw CAError(what: "device \(deviceName(device)) has no \(isInput ? "input" : "output") channels", status: -1) }
        rate = nominalRate(device)
        for s in streams(device, isInput ? scopeIn : scopeOut) {
            if let f = getValue(s, paddr(kAudioStreamPropertyVirtualFormat), AudioStreamBasicDescription()),
               f.mFormatID != kAudioFormatLinearPCM || f.mFormatFlags & kAudioFormatFlagIsFloat == 0 || f.mBitsPerChannel != 32 {
                throw CAError(what: "stream format is not float32 (\(asbdDict(f)))", status: -1)
            }
        }
        ctx = bio_create(isInput ? 1 : 0, Int32(channels), ringFrames, prefillFrames, logRecords)
    }

    /// Request an IO buffer size; returns what the HAL granted.
    @discardableResult
    func setBufferFrames(_ n: Int) -> Int {
        if n > 0 { setValue(device, paddr(kAudioDevicePropertyBufferFrameSize), UInt32(n)) }
        return Int(getValue(device, paddr(kAudioDevicePropertyBufferFrameSize), UInt32(0)) ?? 0)
    }

    func start() throws {
        var pid: AudioDeviceIOProcID?
        try check(AudioDeviceCreateIOProcID(device, bio_proc(ctx), UnsafeMutableRawPointer(ctx), &pid), "AudioDeviceCreateIOProcID")
        procID = pid
        try check(AudioDeviceStart(device, pid), "AudioDeviceStart")
        running = true
    }

    func stop() {
        guard let pid = procID else { return }
        if running { AudioDeviceStop(device, pid); running = false }
        AudioDeviceDestroyIOProcID(device, pid)
        procID = nil
    }

    var ring: OpaquePointer { bio_ring(ctx) }
    var stats: BIOStats { var s = BIOStats(); bio_stats(ctx, &s); return s }
    func wait(ms: Int) { _ = bio_wait(ctx, Int32(ms)) }

    func popRecords() -> [BTimeRec] {
        guard let r = bio_records(ctx) else { return [] }
        var out: [BTimeRec] = []
        var rec = BTimeRec()
        while brec_pop(r, &rec) != 0 { out.append(rec) }
        return out
    }
}

// MARK: - Channel mapping

/// Interleaved channel conversion. Down to mono = average; mono up = duplicate; otherwise
/// the first min(src, dst) channels are kept and extra outputs are silent.
func mapChannels(_ src: UnsafePointer<Float>, frames: Int, from sc: Int, to dc: Int, into dst: inout [Float]) {
    let need = frames * dc
    if dst.count < need { dst = [Float](repeating: 0, count: need) }
    dst.withUnsafeMutableBufferPointer { d in
        if sc == dc {
            d.baseAddress!.update(from: src, count: need)
        } else if dc == 1 {
            let inv = 1 / Float(sc)
            for f in 0..<frames {
                var acc: Float = 0
                for c in 0..<sc { acc += src[f * sc + c] }
                d[f] = acc * inv
            }
        } else if sc == 1 {
            for f in 0..<frames { for c in 0..<dc { d[f * dc + c] = src[f] } }
        } else {
            for f in 0..<frames {
                for c in 0..<dc { d[f * dc + c] = c < sc ? src[f * sc + c] : 0 }
            }
        }
    }
}

// MARK: - Resampler (AudioConverter, push style)

func floatASBD(rate: Double, channels: Int) -> AudioStreamBasicDescription {
    AudioStreamBasicDescription(mSampleRate: rate, mFormatID: kAudioFormatLinearPCM,
                                mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                mBytesPerPacket: UInt32(4 * channels), mFramesPerPacket: 1,
                                mBytesPerFrame: UInt32(4 * channels), mChannelsPerFrame: UInt32(channels),
                                mBitsPerChannel: 32, mReserved: 0)
}

private let kNoMoreInput: OSStatus = 0x6E6F_6D72  // 'nomr'

final class Resampler {
    let channels: Int
    let ratio: Double
    private var conv: AudioConverterRef?
    fileprivate var input: UnsafeMutablePointer<Float>
    fileprivate var inputCapacity: Int
    fileprivate var inputFrames = 0
    private var output: [Float] = []

    init(from inRate: Double, to outRate: Double, channels: Int) throws {
        self.channels = channels
        ratio = outRate / inRate
        var inF = floatASBD(rate: inRate, channels: channels)
        var outF = floatASBD(rate: outRate, channels: channels)
        try check(AudioConverterNew(&inF, &outF, &conv), "AudioConverterNew")
        var q = UInt32(kAudioConverterQuality_Medium)
        AudioConverterSetProperty(conv!, kAudioConverterSampleRateConverterQuality, 4, &q)
        inputCapacity = 4096 * channels
        input = .allocate(capacity: inputCapacity)
    }

    deinit {
        if let c = conv { AudioConverterDispose(c) }
        input.deallocate()
    }

    /// Feed `frames` interleaved frames, get back whatever output the converter can produce now.
    func process(_ src: UnsafePointer<Float>, frames: Int) -> ArraySlice<Float> {
        if frames * channels > inputCapacity {
            input.deallocate()
            inputCapacity = frames * channels
            input = .allocate(capacity: inputCapacity)
        }
        input.update(from: src, count: frames * channels)
        inputFrames = frames
        let cap = Int(Double(frames) * ratio) + 256
        if output.count < cap * channels { output = [Float](repeating: 0, count: cap * channels) }
        var produced = 0
        while true {
            var packets = UInt32(cap - produced)
            if packets == 0 { break }
            let status: OSStatus = output.withUnsafeMutableBytes { ob in
                var abl = AudioBufferList(mNumberBuffers: 1, mBuffers: AudioBuffer(
                    mNumberChannels: UInt32(channels), mDataByteSize: UInt32(Int(packets) * 4 * channels),
                    mData: ob.baseAddress! + produced * 4 * channels))
                return AudioConverterFillComplexBuffer(conv!, resamplerInput, Unmanaged.passUnretained(self).toOpaque(),
                                                       &packets, &abl, nil)
            }
            produced += Int(packets)
            if status == kNoMoreInput || packets == 0 || status != noErr { break }
        }
        return output[0..<(produced * channels)]
    }
}

private let resamplerInput: AudioConverterComplexInputDataProc = { _, ioPackets, ioData, _, user in
    let r = Unmanaged<Resampler>.fromOpaque(user!).takeUnretainedValue()
    if r.inputFrames == 0 {
        ioPackets.pointee = 0
        return kNoMoreInput
    }
    ioData.pointee.mNumberBuffers = 1
    ioData.pointee.mBuffers.mNumberChannels = UInt32(r.channels)
    ioData.pointee.mBuffers.mData = UnsafeMutableRawPointer(r.input)
    ioData.pointee.mBuffers.mDataByteSize = UInt32(r.inputFrames * 4 * r.channels)
    ioPackets.pointee = UInt32(r.inputFrames)
    r.inputFrames = 0
    return noErr
}

/// Channel map + optional resample, from device format to the requested stream format.
final class Converter {
    let inCh: Int, outCh: Int
    let resampler: Resampler?
    private var mapped: [Float] = []

    init(inRate: Double, inCh: Int, outRate: Double, outCh: Int) throws {
        self.inCh = inCh
        self.outCh = outCh
        resampler = abs(inRate - outRate) > 0.01 ? try Resampler(from: inRate, to: outRate, channels: outCh) : nil
    }

    /// Returns interleaved output samples; the closure must not keep the pointer.
    func convert(_ src: UnsafePointer<Float>, frames: Int, _ body: (UnsafeBufferPointer<Float>) -> Void) {
        if frames == 0 { return }
        mapChannels(src, frames: frames, from: inCh, to: outCh, into: &mapped)
        mapped.withUnsafeBufferPointer { m in
            if let r = resampler {
                let out = r.process(m.baseAddress!, frames: frames)
                out.withUnsafeBufferPointer { body($0) }
            } else {
                body(UnsafeBufferPointer(rebasing: m[0..<(frames * outCh)]))
            }
        }
    }
}

// MARK: - stdout / stdin

/// Write all bytes to fd 1. Returns false on EPIPE (reader went away) or another error.
func writeAllStdout(_ p: UnsafeRawBufferPointer) -> Bool {
    var off = 0
    while off < p.count {
        let r = write(1, p.baseAddress! + off, p.count - off)
        if r < 0 {
            if errno == EINTR { continue }
            if errno == EPIPE { elog(["event": "epipe"]) } else { elog(["event": "error", "message": "stdout write errno \(errno)"]) }
            return false
        }
        off += r
    }
    return true
}

// MARK: - Process tap + private aggregate

struct SystemTap {
    let tapID: AudioObjectID
    let aggregateID: AudioObjectID
    let tapUID: String
    let format: AudioStreamBasicDescription
    let clockDeviceUID: String?
    let excluded: [[String: Any]]   // {pid, object} for each --exclude-pid resolved at creation
    let excludePending: [Int]       // PIDs with no Core Audio process object yet (added when they appear)
}

/// Core Audio process object of a PID, or 0. A process only has one once it is a HAL client
/// (it has made a Core Audio call), so a player that has just been spawned may not have one yet.
func processObject(pid: pid_t) -> AudioObjectID {
    var a = paddr(kAudioHardwarePropertyTranslatePIDToProcessObject)
    var p = pid
    var obj = AudioObjectID(0)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let st = AudioObjectGetPropertyData(systemObject, &a, UInt32(MemoryLayout<pid_t>.size), &p, &size, &obj)
    return st == noErr ? obj : 0
}

func ownProcessObject() -> AudioObjectID { processObject(pid: getpid()) }

/// Keeps a tap's exclusion list in step with PIDs that had no process object when the tap was
/// created: on every change of the system's process list, resolve them again and, if any appeared,
/// rewrite the tap's description (kAudioTapPropertyDescription is settable on a live tap).
@available(macOS 14.2, *)
final class LateExclusions {
    let tapID: AudioObjectID
    let desc: CATapDescription
    var pending: [pid_t]
    let queue = DispatchQueue(label: "beamer-audio.exclusions")
    init(tapID: AudioObjectID, desc: CATapDescription, pending: [pid_t]) {
        self.tapID = tapID; self.desc = desc; self.pending = pending
        var a = paddr(kAudioHardwarePropertyProcessObjectList)
        AudioObjectAddPropertyListenerBlock(systemObject, &a, queue) { [weak self] _, _ in self?.refresh() }
        queue.async { self.refresh() }  // a PID may have appeared between creation and now
    }
    func refresh() {
        var found: [[String: Any]] = []
        var objs = desc.processes
        pending = pending.filter { pid in
            let o = processObject(pid: pid)
            if o == 0 { return true }
            if !objs.contains(o) { objs.append(o) }
            found.append(["pid": Int(pid), "object": Int(o)])
            return false
        }
        guard !found.isEmpty else { return }
        desc.processes = objs
        var a = paddr(kAudioTapPropertyDescription)
        var d: CATapDescription = desc
        let st = withUnsafeMutablePointer(to: &d) {
            AudioObjectSetPropertyData(tapID, &a, 0, nil, UInt32(MemoryLayout<CATapDescription>.size), $0)
        }
        elog(["event": "exclude_added", "added": found, "status": Int(st), "still_pending": pending.map { Int($0) }])
    }
}
private var lateExclusions: AnyObject?

enum TapMute: String { case unmuted, mutedWhenTapped = "muted_when_tapped", muted }

/// Global (or per-device) process tap wrapped in a private aggregate device. Registers cleanup.
/// `excludePIDs`: other processes to leave out of the tap (e.g. Beamer's own player, so received
/// audio is not captured and sent back). Each is waited for up to `excludeWait` seconds; one still
/// without a process object after that is added later by `LateExclusions`.
func makeSystemTap(deviceUID tapDeviceUID: String?, mute: TapMute, includeSelf: Bool, clockSubdevice: Bool = false,
                   excludePIDs: [pid_t] = [], excludeWait: Double = 5) throws -> SystemTap {
    if #unavailable(macOS 14.2) { throw CAError(what: "process taps need macOS 14.2+", status: -1) }
    var exclude: [AudioObjectID] = []
    if !includeSelf {
        let me = ownProcessObject()
        if me != 0 { exclude.append(me) }
    }
    var excluded: [[String: Any]] = []
    var pending = excludePIDs
    let waitUntil = wallSeconds() + excludeWait
    while true {
        pending = pending.filter { pid in
            let o = processObject(pid: pid)
            if o == 0 { return true }
            if !exclude.contains(o) { exclude.append(o) }
            excluded.append(["pid": Int(pid), "object": Int(o)])
            return false
        }
        if pending.isEmpty || wallSeconds() >= waitUntil { break }
        usleep(20_000)
    }
    let desc: CATapDescription
    let clockUID: String
    if let uid = tapDeviceUID {
        desc = CATapDescription(excludingProcesses: exclude, deviceUID: uid, stream: 0)
        clockUID = uid
    } else {
        desc = CATapDescription(stereoGlobalTapButExcludeProcesses: exclude)
        let out = defaultDevice(kAudioHardwarePropertyDefaultSystemOutputDevice)
        clockUID = deviceUID(out)
    }
    desc.name = "beamer-audio tap"
    desc.uuid = UUID()
    desc.isPrivate = true
    switch mute {
    case .unmuted: desc.muteBehavior = .unmuted
    case .mutedWhenTapped: desc.muteBehavior = .mutedWhenTapped
    case .muted: desc.muteBehavior = .muted
    }
    var tapID = AudioObjectID(0)
    try check(AudioHardwareCreateProcessTap(desc, &tapID), "AudioHardwareCreateProcessTap")
    Cleanup.shared.add("destroy_tap") { AudioHardwareDestroyProcessTap(tapID) }
    if !pending.isEmpty, #available(macOS 14.2, *) {
        lateExclusions = LateExclusions(tapID: tapID, desc: desc, pending: pending)
    }
    let tapUID = getString(tapID, paddr(kAudioTapPropertyUID)) ?? desc.uuid.uuidString
    let format = getValue(tapID, paddr(kAudioTapPropertyFormat), AudioStreamBasicDescription()) ?? AudioStreamBasicDescription()

    let aggUID = "net.beamer.audio.tap." + UUID().uuidString
    var dict: [String: Any] = [
        kAudioAggregateDeviceNameKey: "beamer-audio system tap",
        kAudioAggregateDeviceUIDKey: aggUID,
        kAudioAggregateDeviceIsPrivateKey: true,
        kAudioAggregateDeviceIsStackedKey: false,
        // tapautostart=true makes the aggregate wait for the first tapped audio before its IOProc
        // runs at all (measured: zero callbacks while the system is silent). We want a continuous
        // stream (silence included) from the start, so keep it off.
        kAudioAggregateDeviceTapAutoStartKey: false,
        kAudioAggregateDeviceTapListKey: [[kAudioSubTapDriftCompensationKey: true, kAudioSubTapUIDKey: tapUID]],
    ]
    // Default: tap-only aggregate. Adding the output device as main sub-device clocks the
    // aggregate from the hardware, but our IOProc then keeps that device running even when the
    // system is silent (measured on the MacBook Air speakers: coreaudiod ~11 % CPU vs ~0.4 %).
    if clockSubdevice {
        dict[kAudioAggregateDeviceMainSubDeviceKey] = clockUID
        dict[kAudioAggregateDeviceSubDeviceListKey] = [[kAudioSubDeviceUIDKey: clockUID]]
    }
    var aggID = AudioObjectID(0)
    try check(AudioHardwareCreateAggregateDevice(dict as CFDictionary, &aggID), "AudioHardwareCreateAggregateDevice")
    Cleanup.shared.add("destroy_aggregate") { AudioHardwareDestroyAggregateDevice(aggID) }
    // The tap's input stream appears asynchronously on some systems; wait briefly for it.
    for _ in 0..<50 where channelCount(aggID, scopeIn) == 0 { usleep(10_000) }
    return SystemTap(tapID: tapID, aggregateID: aggID, tapUID: tapUID, format: format, clockDeviceUID: clockSubdevice ? clockUID : nil,
                     excluded: excluded, excludePending: pending.map { Int($0) })
}
