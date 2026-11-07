// list, watch, capture-mic, capture-system, play.
import AudioToolbox
import CoreAudio
import Foundation

// MARK: - list

func cmdList(_ args: Args) {
    let devs = allDevices()
    if args.has("json") {
        var o = defaultsInfo()
        o["devices"] = devs.map { deviceInfo($0, verbose: args.has("verbose")) }
        print(jsonString(o))
        return
    }
    let defIn = defaultDevice(kAudioHardwarePropertyDefaultInputDevice)
    let defOut = defaultDevice(kAudioHardwarePropertyDefaultOutputDevice)
    let defSys = defaultDevice(kAudioHardwarePropertyDefaultSystemOutputDevice)
    for d in devs {
        var marks: [String] = []
        if d == defIn { marks.append("default-input") }
        if d == defOut { marks.append("default-output") }
        if d == defSys { marks.append("default-system-output") }
        let transport = transportName(getValue(d, paddr(kAudioDevicePropertyTransportType), UInt32(0)) ?? 0)
        func pad(_ s: String, _ n: Int) -> String { s.padding(toLength: max(n, s.count), withPad: " ", startingAt: 0) }
        print(pad(deviceName(d), 34) + " in=" + pad("\(channelCount(d, scopeIn))", 3) + "out=" + pad("\(channelCount(d, scopeOut))", 3)
              + pad(String(format: "%.0f Hz", nominalRate(d)), 9) + " " + pad(transport, 10) + " uid=" + deviceUID(d)
              + (marks.isEmpty ? "" : " [" + marks.joined(separator: ",") + "]"))
    }
}

// MARK: - watch

private var watchKnown: [AudioObjectID: [String: Any]] = [:]
private var rateListeners: Set<AudioObjectID> = []

private func emit(_ o: [String: Any]) {
    var o = o
    o["t"] = round(wallSeconds() * 1000) / 1000
    print(jsonString(o))
    fflush(stdout)
}

private func watchRate(_ d: AudioObjectID) {
    guard !rateListeners.contains(d) else { return }
    rateListeners.insert(d)
    var a = paddr(kAudioDevicePropertyNominalSampleRate)
    AudioObjectAddPropertyListenerBlock(d, &a, DispatchQueue.main) { _, _ in
        guard watchKnown[d] != nil else { return }
        emit(["event": "rate_changed", "uid": deviceUID(d), "name": deviceName(d), "nominal_rate": nominalRate(d)])
    }
}

func cmdWatch(_ args: Args) -> Never {
    for d in allDevices() { watchKnown[d] = deviceInfo(d); watchRate(d) }
    var snap = defaultsInfo()
    snap["event"] = "snapshot"
    snap["devices"] = allDevices().map { watchKnown[$0]! }
    emit(snap)

    var devAddr = paddr(kAudioHardwarePropertyDevices)
    AudioObjectAddPropertyListenerBlock(systemObject, &devAddr, DispatchQueue.main) { _, _ in
        let now = Set(allDevices())
        let old = Set(watchKnown.keys)
        let added = now.subtracting(old).sorted()
        let removed = old.subtracting(now).sorted()
        var addedInfo: [[String: Any]] = []
        for d in added { let i = deviceInfo(d); watchKnown[d] = i; addedInfo.append(i); watchRate(d) }
        var removedInfo: [[String: Any]] = []
        for d in removed {
            let i = watchKnown.removeValue(forKey: d)!
            removedInfo.append(["uid": i["uid"]!, "name": i["name"]!, "id": Int(d)])
        }
        emit(["event": "devices_changed", "added": addedInfo, "removed": removedInfo, "count": now.count])
    }
    let defaults: [(AudioObjectPropertySelector, String)] = [
        (kAudioHardwarePropertyDefaultInputDevice, "default_input_changed"),
        (kAudioHardwarePropertyDefaultOutputDevice, "default_output_changed"),
        (kAudioHardwarePropertyDefaultSystemOutputDevice, "default_system_output_changed"),
    ]
    for (sel, name) in defaults {
        var a = paddr(sel)
        AudioObjectAddPropertyListenerBlock(systemObject, &a, DispatchQueue.main) { _, _ in
            let d = defaultDevice(sel)
            emit(["event": name, "device": d == 0 ? NSNull() : ["uid": deviceUID(d), "name": deviceName(d), "id": Int(d)]])
        }
    }
    verboseCleanup = false
    installSignalHandlers()
    dispatchMain()
}

// MARK: - capture (shared pump)

/// Pump the capture ring to stdout until EPIPE or signal. Emits periodic stats on stderr.
func runCapturePump(_ io: IODevice, outRate: Double, outCh: Int, statsEvery: Double) -> Never {
    let conv: Converter
    do { conv = try Converter(inRate: io.rate, inCh: io.channels, outRate: outRate, outCh: outCh) } catch { fail("\(error)") }
    let chunkFrames = 4096
    var buf = [Float](repeating: 0, count: chunkFrames * io.channels)
    var sumSq = 0.0, peak: Float = 0, nSamples = 0
    var lastStats = wallSeconds()
    var lastOverruns: UInt64 = 0
    var outFrames: UInt64 = 0
    var sawFirst = false
    exitExtras = {
        let s = io.stats
        return ["frames_out": Int(outFrames), "overruns": Int(s.overruns), "device_callbacks": Int(s.callbacks)]
    }
    while true {
        io.wait(ms: 100)
        while true {
            let avail = bring_readable(io.ring) / io.channels
            if avail == 0 { break }
            let n = min(avail, chunkFrames)
            let got = buf.withUnsafeMutableBufferPointer { bring_read(io.ring, $0.baseAddress!, n * io.channels) } / io.channels
            var ok = true
            buf.withUnsafeBufferPointer { b in
                conv.convert(b.baseAddress!, frames: got) { out in
                    for x in out { sumSq += Double(x * x); peak = max(peak, abs(x)) }
                    nSamples += out.count
                    outFrames += UInt64(out.count / outCh)
                    ok = writeAllStdout(UnsafeRawBufferPointer(out))
                }
            }
            if !ok { finish(0) }
        }
        let st = io.stats
        if !sawFirst, st.callbacks > 0 {
            sawFirst = true
            elog(["event": "first_callback", "buffer_frames": Int(st.last_buffer_frames)])
        }
        if st.overruns != lastOverruns {
            elog(["event": "overrun", "count": Int(st.overruns), "new": Int(st.overruns - lastOverruns)])
            lastOverruns = st.overruns
        }
        let t = wallSeconds()
        if statsEvery > 0, t - lastStats >= statsEvery {
            let rms = nSamples > 0 ? sqrt(sumSq / Double(nSamples)) : 0
            elog(["event": "stats", "frames_out": Int(outFrames), "rms_dbfs": dbfs(rms), "peak_dbfs": dbfs(Double(peak)),
                  "callbacks": Int(st.callbacks), "buffer_frames": Int(st.last_buffer_frames),
                  "overruns": Int(st.overruns), "rate_scalar": st.last_rate_scalar])
            sumSq = 0; peak = 0; nSamples = 0; lastStats = t
        }
    }
}

func startCapture(_ io: IODevice, extra: [String: Any], args: Args, outRate: Double, outCh: Int) -> Never {
    let granted = io.setBufferFrames(args.int("buffer-frames", 0))
    do { try io.start() } catch { fail("\(error)") }
    Cleanup.shared.add("stop_ioproc") { io.stop() }
    var o: [String: Any] = ["event": "start", "device_rate": io.rate, "device_channels": io.channels,
                            "out_rate": outRate, "out_channels": outCh, "buffer_frames": granted,
                            "resampling": abs(io.rate - outRate) > 0.01]
    o.merge(extra) { a, _ in a }
    elog(o)
    runCapturePump(io, outRate: outRate, outCh: outCh, statsEvery: args.double("stats", 5))
}

// MARK: - capture-mic

func cmdCaptureMic(_ args: Args) -> Never {
    installSignalHandlers()
    let dev: AudioObjectID
    do {
        dev = try selectDevice(args.options["device"], scope: scopeIn, defaultSel: kAudioHardwarePropertyDefaultInputDevice, allowName: true)
    } catch { fail("\(error)") }
    let outRate = args.double("rate", 48000), outCh = args.int("channels", 1)
    let io: IODevice
    do { io = try IODevice(device: dev, isInput: true, ringFrames: Int(nominalRate(dev) * 0.5)) } catch { fail("\(error)") }
    startCapture(io, extra: ["mode": "capture-mic", "uid": deviceUID(dev), "name": deviceName(dev),
                             "input": latencyInfo(dev, scopeIn)], args: args, outRate: outRate, outCh: outCh)
}

// MARK: - capture-system

func cmdCaptureSystem(_ args: Args) -> Never {
    installSignalHandlers()
    let mute: TapMute = args.has("muted-always") ? .muted : (args.has("mute-local") ? .mutedWhenTapped : .unmuted)
    var excludePIDs: [pid_t] = []
    for v in args.all("exclude-pid") {
        guard let p = pid_t(v), p > 0 else { fail("--exclude-pid needs a process id, got \(v)", code: 2) }
        if kill(p, 0) != 0 && errno == ESRCH { fail("--exclude-pid: no process \(p)", code: 2) }
        excludePIDs.append(p)
    }
    let tap: SystemTap
    do { tap = try makeSystemTap(deviceUID: args.options["device"], mute: mute, includeSelf: args.has("include-self"),
                                 clockSubdevice: args.has("clocked"), excludePIDs: excludePIDs,
                                 excludeWait: args.double("exclude-wait", 5)) } catch { fail("\(error)") }
    let outRate = args.double("rate", 48000), outCh = args.int("channels", 2)
    let io: IODevice
    do { io = try IODevice(device: tap.aggregateID, isInput: true, ringFrames: Int(max(nominalRate(tap.aggregateID), 48000) * 0.5)) }
    catch { fail("\(error)") }
    startCapture(io, extra: ["mode": "capture-system", "mute": mute.rawValue, "tap_uid": tap.tapUID,
                             "tap_format": asbdDict(tap.format), "clock_device": tap.clockDeviceUID ?? NSNull(),
                             "aggregate_id": Int(tap.aggregateID), "excluded": tap.excluded,
                             "exclude_pending": tap.excludePending], args: args, outRate: outRate, outCh: outCh)
}

// MARK: - play

func cmdPlay(_ args: Args) -> Never {
    installSignalHandlers()
    let dev: AudioObjectID
    do {
        dev = try selectDevice(args.options["device"], scope: scopeOut, defaultSel: kAudioHardwarePropertyDefaultOutputDevice, allowName: true)
    } catch { fail("\(error)") }
    let inRate = args.double("rate", 48000), inCh = args.int("channels", 2)
    let devRate = nominalRate(dev)
    let ringMs = args.double("buffer-ms", 80), prefillMs = args.double("prefill-ms", 20)
    let io: IODevice
    do {
        io = try IODevice(device: dev, isInput: false, ringFrames: Int(devRate * ringMs / 1000),
                          prefillFrames: Int(devRate * prefillMs / 1000))
    } catch { fail("\(error)") }
    let conv: Converter
    do { conv = try Converter(inRate: inRate, inCh: inCh, outRate: devRate, outCh: io.channels) } catch { fail("\(error)") }
    let granted = io.setBufferFrames(args.int("buffer-frames", 0))
    do { try io.start() } catch { fail("\(error)") }
    Cleanup.shared.add("stop_ioproc") { io.stop() }
    elog(["event": "start", "mode": "play", "uid": deviceUID(dev), "name": deviceName(dev), "device_rate": devRate,
          "device_channels": io.channels, "in_rate": inRate, "in_channels": inCh, "buffer_frames": granted,
          "ring_ms": ringMs, "prefill_ms": prefillMs, "resampling": abs(devRate - inRate) > 0.01,
          "output": latencyInfo(dev, scopeOut)])

    // Monitor: underruns promptly, stats periodically.
    let statsEvery = args.double("stats", 5)
    let timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "beamer-audio.monitor"))
    var lastUnder: UInt64 = 0
    var lastStats = wallSeconds()
    timer.schedule(deadline: .now() + 0.1, repeating: 0.1)
    timer.setEventHandler {
        let s = io.stats
        if s.underruns != lastUnder {
            elog(["event": "underrun", "count": Int(s.underruns), "new": Int(s.underruns - lastUnder),
                  "silence_frames_total": Int(s.underrun_frames)])
            lastUnder = s.underruns
        }
        let t = wallSeconds()
        if statsEvery > 0, t - lastStats >= statsEvery {
            elog(["event": "stats", "frames_played": Int(s.ring_frames), "callbacks": Int(s.callbacks),
                  "buffer_frames": Int(s.last_buffer_frames), "underruns": Int(s.underruns),
                  "ring_fill_ms": Double(bring_readable(io.ring) / io.channels) / devRate * 1000])
            lastStats = t
        }
    }
    timer.resume()
    exitExtras = {
        let s = io.stats
        return ["frames_played": Int(s.ring_frames), "underruns": Int(s.underruns),
                "silence_frames_total": Int(s.underrun_frames), "device_callbacks": Int(s.callbacks)]
    }

    let frameBytes = 4 * inCh
    var raw = [UInt8](repeating: 0, count: 4096 * frameBytes)
    var have = 0
    while true {
        let r = raw.withUnsafeMutableBytes { read(0, $0.baseAddress! + have, $0.count - have) }
        if r < 0 { if errno == EINTR { continue }; fail("stdin read errno \(errno)") }
        if r == 0 { break }
        have += r
        let frames = have / frameBytes
        if frames == 0 { continue }
        raw.withUnsafeBytes { rb in
            let fp = rb.baseAddress!.assumingMemoryBound(to: Float.self)
            conv.convert(fp, frames: frames) { out in
                var off = 0
                while off < out.count {
                    let w = bring_write(io.ring, out.baseAddress! + off, out.count - off)
                    off += w
                    if off < out.count { io.wait(ms: 20) }  // ring full: wait for the device to consume
                }
            }
        }
        let used = frames * frameBytes
        if have > used { raw.withUnsafeMutableBytes { $0.baseAddress!.copyMemory(from: $0.baseAddress! + used, byteCount: have - used) } }
        have -= used
    }
    // EOF: play out what is buffered, then stop.
    bio_set_draining(io.ctx, 1)
    while bring_readable(io.ring) > 0 { io.wait(ms: 50) }
    // The last buffer is still on its way to the speaker (one IO buffer + safety offset +
    // device/stream latency); give it time before stopping the IOProc.
    let o = latencyInfo(dev, scopeOut)
    let tailFrames = (o["buffer_frames"] as? Int ?? 512) + (o["safety_offset_frames"] as? Int ?? 0)
        + (o["device_latency_frames"] as? Int ?? 0) + ((o["stream_latency_frames"] as? [Int])?.first ?? 0)
    usleep(useconds_t(Double(tailFrames) / devRate * 1e6) + 20_000)
    let s = io.stats
    if s.underruns != lastUnder {
        elog(["event": "underrun", "count": Int(s.underruns), "new": Int(s.underruns - lastUnder),
              "silence_frames_total": Int(s.underrun_frames)])
    }
    timer.cancel()
    finish(0)
}
