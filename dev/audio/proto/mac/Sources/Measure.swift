// measure props | buffers | tap-latency | loop-latency | drift
import AudioToolbox
import CoreAudio
import Foundation

func measureUsage() -> Never {
    let u = """
    beamer-audio measure props          [--in UID] [--out UID]
    beamer-audio measure buffers        [--in UID] [--out UID] [--sizes 128,256,480]
    beamer-audio measure tap-latency    [--seconds 10] [--mute-local] [--clocked] [--amp 0.25] [--buffer-frames N] [--prefill-ms 10]
    beamer-audio measure loop-latency   [--seconds 10] [--in UID] [--out UID] [--amp 0.1] [--buffer-frames N]
    beamer-audio measure drift          [--seconds 300] [--in UID] [--out UID]
    """
    FileHandle.standardError.write((u + "\n").data(using: .utf8)!)
    exit(2)
}

func cmdMeasure(_ args: Args) -> Never {
    installSignalHandlers()
    verboseCleanup = false
    guard args.command.count >= 2 else { measureUsage() }
    switch args.command[1] {
    case "props": measureProps(args)
    case "buffers": measureBuffers(args)
    case "tap-latency": measureClickLatency(args, viaTap: true)
    case "loop-latency": measureClickLatency(args, viaTap: false)
    case "drift": measureDrift(args)
    default: measureUsage()
    }
    finish(0)
}

private func inOut(_ args: Args) -> (AudioObjectID, AudioObjectID) {
    do {
        let i = try selectDevice(args.options["in"], scope: scopeIn, defaultSel: kAudioHardwarePropertyDefaultInputDevice, allowName: true)
        let o = try selectDevice(args.options["out"], scope: scopeOut, defaultSel: kAudioHardwarePropertyDefaultOutputDevice, allowName: true)
        return (i, o)
    } catch { fail("\(error)") }
}

private func ms(_ hostDelta: Int64) -> Double { (Double(hostDelta) / hostTicksPerSecond() * 1e3 * 1000).rounded() / 1000 }

// MARK: props

func measureProps(_ args: Args) {
    let (i, o) = inOut(args)
    var r: [String: Any] = ["event": "props"]
    var ii = deviceInfo(i); ii["input"] = latencyInfo(i, scopeIn)
    var oo = deviceInfo(o); oo["output"] = latencyInfo(o, scopeOut)
    r["input_device"] = ii
    r["output_device"] = oo
    print(jsonString(r))
}

// MARK: buffers

func measureBuffers(_ args: Args) {
    let (i, o) = inOut(args)
    let sizes = (args.options["sizes"] ?? "128,256,480").split(separator: ",").compactMap { Int($0) }
    for (dev, isInput) in [(i, true), (o, false)] {
        let original = Int(getValue(dev, paddr(kAudioDevicePropertyBufferFrameSize), UInt32(0)) ?? 0)
        for n in sizes {
            let io: IODevice
            do { io = try IODevice(device: dev, isInput: isInput, ringFrames: 48000) } catch { fail("\(error)") }
            bio_set_silent(io.ctx, 1)
            let granted = io.setBufferFrames(n)
            do { try io.start() } catch { fail("\(error)") }
            let t0 = wallSeconds()
            var drained = [Float](repeating: 0, count: 8192 * io.channels)
            while wallSeconds() - t0 < 1.0 {
                io.wait(ms: 50)
                if isInput { _ = drained.withUnsafeMutableBufferPointer { bring_read(io.ring, $0.baseAddress!, $0.count) } }
            }
            io.stop()
            let s = io.stats
            let span = hostToSeconds(s.last_now_time - s.first_now_time)
            print(jsonString(["event": "buffer_trial", "device": deviceName(dev), "direction": isInput ? "input" : "output",
                              "requested": n, "granted": granted, "callback_frames_min": Int(s.min_buffer_frames),
                              "callback_frames_max": Int(s.max_buffer_frames), "callbacks": Int(s.callbacks),
                              "callback_period_ms": s.callbacks > 1 ? (span / Double(s.callbacks - 1) * 1e5).rounded() / 100 : 0,
                              "latency": latencyInfo(dev, isInput ? scopeIn : scopeOut)]))
            bio_destroy(io.ctx)
        }
        setValue(dev, paddr(kAudioDevicePropertyBufferFrameSize), UInt32(original))
    }
}

// MARK: click latency (tap or acoustic loop)

private struct Click { var ringFrame: UInt64; var pushHost: UInt64; var presentHost: UInt64 = 0; var renderHost: UInt64 = 0 }
private struct Detection { var frame: UInt64; var captureHost: UInt64 = 0; var deliverHost: UInt64 = 0; var peak: Float }

private func median(_ a: [Double]) -> Double {
    guard !a.isEmpty else { return .nan }
    let s = a.sorted()
    return s.count % 2 == 1 ? s[s.count / 2] : (s[s.count / 2 - 1] + s[s.count / 2]) / 2
}

private func summary(_ a: [Double]) -> [String: Any] {
    guard !a.isEmpty else { return ["n": 0] }
    let r = { (x: Double) in (x * 100).rounded() / 100 }
    return ["n": a.count, "median_ms": r(median(a)), "min_ms": r(a.min()!), "max_ms": r(a.max()!)]
}

/// Plays a click every 500 ms through our own output IOProc (ring + prefill, same path as `play`)
/// and detects it in a capture IOProc: a process tap that includes this process (viaTap), or the
/// microphone (acoustic loop). Timestamps come from the HAL (AudioTimeStamp of each buffer) and
/// from mach_absolute_time at the callbacks.
func measureClickLatency(_ args: Args, viaTap: Bool) {
    let (inDev, outDev) = inOut(args)
    let seconds = args.double("seconds", 10)
    let amp = Float(args.double("amp", viaTap ? 0.25 : 0.1))
    let bufFrames = args.int("buffer-frames", 0)
    let prefillMs = args.double("prefill-ms", 10)

    let capDev: AudioObjectID
    var tapInfo: [String: Any] = [:]
    if viaTap {
        let mute: TapMute = args.has("mute-local") ? .mutedWhenTapped : .unmuted
        do {
            let t = try makeSystemTap(deviceUID: deviceUID(outDev), mute: mute, includeSelf: true, clockSubdevice: args.has("clocked"))
            capDev = t.aggregateID
            tapInfo = ["mute": mute.rawValue, "tap_format": asbdDict(t.format)]
        } catch { fail("\(error)") }
    } else {
        capDev = inDev
    }
    let out: IODevice, cap: IODevice
    do {
        out = try IODevice(device: outDev, isInput: false, ringFrames: Int(nominalRate(outDev) * 0.1),
                           prefillFrames: Int(nominalRate(outDev) * prefillMs / 1000), logRecords: 8192)
        cap = try IODevice(device: capDev, isInput: true, ringFrames: Int(nominalRate(capDev)), logRecords: 8192)
    } catch { fail("\(error)") }
    let outGranted = out.setBufferFrames(bufFrames)
    let capGranted = cap.setBufferFrames(bufFrames)
    do { try cap.start(); try out.start() } catch { fail("\(error)") }
    Cleanup.shared.add("stop_out") { out.stop() }
    Cleanup.shared.add("stop_cap") { cap.stop() }

    let outRate = out.rate, capRate = cap.rate
    let ticks = hostTicksPerSecond()
    let period = Int(outRate * 0.5)
    let burst = Int(outRate * 0.002)
    var clicks: [Click] = []
    var outRecs: [BTimeRec] = []
    var capRecs: [BTimeRec] = []
    var detections: [Detection] = []

    // Feeder: keep the output ring topped up (paced by the device), a click every `period` frames.
    let prefillSamples = max(Int(outRate * prefillMs / 1000), 64) * out.channels
    let feeder = Thread {
        var frame: UInt64 = 0
        let ch = out.channels
        var chunk = [Float](repeating: 0, count: 64 * ch)
        let end = UInt64(outRate * (seconds + 0.5))
        while frame < end {
            // Keep the ring at the prefill level, like a live stream would (not full).
            while bring_readable(out.ring) >= prefillSamples || bring_writable(out.ring) < chunk.count { out.wait(ms: 2) }
            let f0 = frame
            for f in 0..<64 {
                let k = Int((f0 + UInt64(f)) % UInt64(period))
                // 3 kHz burst starting at a peak, first click 250 ms in
                let v: Float = (f0 + UInt64(f) >= UInt64(outRate * 0.25) && k < burst) ? amp * cos(2 * .pi * 3000 * Float(k) / Float(outRate)) : 0
                for c in 0..<ch { chunk[f * ch + c] = v }
            }
            let isClick = (f0 >= UInt64(outRate * 0.25)) && (f0 % UInt64(period)) < 64
            let push = nowHost()
            chunk.withUnsafeBufferPointer { _ = bring_write(out.ring, $0.baseAddress!, $0.count) }
            if isClick {
                let start = f0 - (f0 % UInt64(period))
                DispatchQueue.main.async { clicks.append(Click(ringFrame: start, pushHost: push)) }
            }
            frame += 64
        }
        bio_set_draining(out.ctx, 1)
    }
    feeder.start()

    // Collect the capture (channel average) on this thread, analyse afterwards.
    let cch = cap.channels
    var buf = [Float](repeating: 0, count: 8192 * cch)
    var mono: [Float] = []
    mono.reserveCapacity(Int(capRate * (seconds + 1.5)))
    let t0 = wallSeconds()
    while wallSeconds() - t0 < seconds + 1.0 {
        cap.wait(ms: 20)
        outRecs += out.popRecords()
        capRecs += cap.popRecords()
        while true {
            let n = buf.withUnsafeMutableBufferPointer { bring_read(cap.ring, $0.baseAddress!, $0.count) } / cch
            if n == 0 { break }
            for f in 0..<n {
                var x: Float = 0
                for c in 0..<cch { x += buf[f * cch + c] }
                mono.append(x / Float(cch))
            }
        }
        RunLoop.main.run(until: Date())  // pick up click registrations queued by the feeder
    }
    // Detection: 3 kHz envelope (sliding quadrature sum over the burst length), robust against
    // room noise. Onset = first sample where the envelope reaches half of the local peak, minus
    // half a window (the envelope of a delayed burst ramps over exactly one window).
    let W = Int(capRate * 0.002)
    let w0 = 2 * Double.pi * 3000 / capRate
    var cs = [Double](repeating: 0, count: mono.count + 1), sn = cs
    for i in mono.indices {
        cs[i + 1] = cs[i] + Double(mono[i]) * cos(w0 * Double(i))
        sn[i + 1] = sn[i] + Double(mono[i]) * sin(w0 * Double(i))
    }
    var env = [Double](repeating: 0, count: mono.count)
    for i in W..<max(W, mono.count) {
        let c = cs[i + 1] - cs[i + 1 - W], q = sn[i + 1] - sn[i + 1 - W]
        env[i] = 2 * sqrt(c * c + q * q) / Double(W)
    }
    let sortedEnv = env.sorted()
    let medianEnv = sortedEnv.isEmpty ? 0 : sortedEnv[sortedEnv.count / 2]
    let maxPeakSeen = sortedEnv.last ?? 0
    let threshold = max(medianEnv * 10, maxPeakSeen * 0.25, 1e-5)
    elog(["event": "envelope", "median": medianEnv, "max": maxPeakSeen, "threshold": threshold])
    var i = W
    let refractory = Int(capRate * 0.3), look = Int(capRate * 0.01)
    while i < env.count {
        if env[i] >= threshold {
            let end = min(env.count, i + look)
            var am = i
            for k in i..<end where env[k] > env[am] { am = k }
            var m = am
            while m > W && env[m - 1] >= env[am] * 0.5 { m -= 1 }
            let onset = max(0, m - W / 2)
            detections.append(Detection(frame: UInt64(onset), peak: Float(env[am])))
            i = am + refractory
        } else {
            i += 1
        }
    }
    // Drain remaining main-queue appends.
    RunLoop.main.run(until: Date().addingTimeInterval(0.05))
    outRecs += out.popRecords()
    capRecs += cap.popRecords()

    // Map click ring frames -> output presentation / render times.
    for i in clicks.indices {
        if let r = outRecs.first(where: { $0.frames > 0 && clicks[i].ringFrame >= $0.index && clicks[i].ringFrame < $0.index + UInt64($0.frames) }) {
            let off = Double(clicks[i].ringFrame - r.index)
            clicks[i].presentHost = r.host_time + UInt64(off / outRate * ticks)
            clicks[i].renderHost = r.now_time
        }
    }
    for i in detections.indices {
        if let r = capRecs.first(where: { $0.flags == 0 && detections[i].frame >= $0.index && detections[i].frame < $0.index + UInt64($0.frames) }) {
            let off = Double(detections[i].frame - r.index)
            detections[i].captureHost = r.host_time + UInt64(off / capRate * ticks)
            detections[i].deliverHost = r.now_time
        }
    }
    // Pair: the latest click pushed before the detection was delivered, within one period.
    var tsLat: [Double] = [], renderLat: [Double] = [], pushLat: [Double] = []
    var renderToPresent: [Double] = [], captureToDeliver: [Double] = []
    var pairs: [[String: Any]] = []
    for d in detections where d.deliverHost != 0 {
        guard let c = clicks.last(where: { $0.presentHost != 0 && $0.pushHost < d.deliverHost }) else { continue }
        let p = ms(Int64(d.deliverHost) - Int64(c.pushHost))
        if p > 500 || p < 0 { continue }
        let ts = ms(Int64(d.captureHost) - Int64(c.presentHost))
        let rl = ms(Int64(d.deliverHost) - Int64(c.renderHost))
        tsLat.append(ts); renderLat.append(rl); pushLat.append(p)
        renderToPresent.append(ms(Int64(c.presentHost) - Int64(c.renderHost)))
        captureToDeliver.append(ms(Int64(d.deliverHost) - Int64(d.captureHost)))
        pairs.append(["timestamp_ms": ts, "render_to_deliver_ms": rl, "push_to_deliver_ms": p, "peak": Double(d.peak)])
    }
    let outLat = latencyInfo(outDev, scopeOut)
    let inLat = viaTap ? latencyInfo(capDev, scopeIn) : latencyInfo(inDev, scopeIn)
    var r: [String: Any] = [
        "event": viaTap ? "tap_latency" : "loop_latency",
        "out_device": deviceName(outDev), "capture_device": viaTap ? "process tap (aggregate)" : deviceName(inDev),
        "out_buffer_frames": outGranted, "capture_buffer_frames": capGranted, "prefill_ms": prefillMs,
        "clicks_sent": clicks.count, "detections": detections.count, "paired": pairs.count, "amp": Double(amp),
        "threshold": threshold, "max_envelope": maxPeakSeen,
        "timestamp_latency": summary(tsLat), "render_to_deliver": summary(renderLat), "push_to_deliver": summary(pushLat),
        "out_render_to_present_ts": summary(renderToPresent), "in_capture_ts_to_deliver": summary(captureToDeliver),
        "out_latency_props": outLat, "in_latency_props": inLat,
        "out_underruns": Int(out.stats.underruns), "capture_overruns": Int(cap.stats.overruns),
    ]
    r.merge(tapInfo) { a, _ in a }
    if args.has("verbose") { r["pairs"] = pairs }
    print(jsonString(r))
    fflush(stdout)
}

// MARK: drift

/// Runs the input and output devices side by side (output plays silence) and compares the
/// sample clocks against the host clock: per-device rate from (sample time, host time)
/// regression and from frames delivered vs callback host time.
func measureDrift(_ args: Args) {
    let (inDev, outDev) = inOut(args)
    let seconds = args.double("seconds", 300)
    let cap: IODevice, out: IODevice
    do {
        cap = try IODevice(device: inDev, isInput: true, ringFrames: 48000)
        out = try IODevice(device: outDev, isInput: false, ringFrames: 4800)
    } catch { fail("\(error)") }
    bio_set_silent(out.ctx, 1)
    do { try cap.start(); try out.start() } catch { fail("\(error)") }
    Cleanup.shared.add("stop_out") { out.stop() }
    Cleanup.shared.add("stop_cap") { cap.stop() }

    struct Sample { var host: Double; var sampleTime: Double; var now: Double; var frames: Double }
    var ins: [Sample] = [], outs: [Sample] = []
    var drain = [Float](repeating: 0, count: 48000)
    let t0 = wallSeconds()
    var lastReport = t0
    // Skip the first second (start-up transients).
    while wallSeconds() - t0 < seconds + 1 {
        cap.wait(ms: 100)
        _ = drain.withUnsafeMutableBufferPointer { bring_read(cap.ring, $0.baseAddress!, $0.count) }
        let t = wallSeconds()
        if t - t0 < 1 { continue }
        let a = cap.stats, b = out.stats
        ins.append(Sample(host: hostToSeconds(a.last_host_time), sampleTime: a.last_sample_time,
                          now: hostToSeconds(a.last_now_time), frames: Double(a.frames)))
        outs.append(Sample(host: hostToSeconds(b.last_host_time), sampleTime: b.last_sample_time,
                           now: hostToSeconds(b.last_now_time), frames: Double(b.frames)))
        if t - lastReport >= 30 {
            elog(["event": "drift_progress", "elapsed_s": Int(t - t0)])
            lastReport = t
        }
    }
    func slope(_ xs: [Double], _ ys: [Double]) -> Double {
        let n = Double(xs.count)
        let mx = xs.reduce(0, +) / n, my = ys.reduce(0, +) / n
        var sxy = 0.0, sxx = 0.0
        for i in xs.indices { sxy += (xs[i] - mx) * (ys[i] - my); sxx += (xs[i] - mx) * (xs[i] - mx) }
        return sxy / sxx
    }
    let inTsRate = slope(ins.map { $0.host - ins[0].host }, ins.map { $0.sampleTime - ins[0].sampleTime })
    let outTsRate = slope(outs.map { $0.host - outs[0].host }, outs.map { $0.sampleTime - outs[0].sampleTime })
    let inFrRate = slope(ins.map { $0.now - ins[0].now }, ins.map { $0.frames - ins[0].frames })
    let outFrRate = slope(outs.map { $0.now - outs[0].now }, outs.map { $0.frames - outs[0].frames })
    let span = (ins.last!.now - ins[0].now)
    let inFramesTotal = ins.last!.frames - ins[0].frames
    let outFramesTotal = outs.last!.frames - outs[0].frames
    let r3 = { (x: Double) in (x * 1000).rounded() / 1000 }
    print(jsonString([
        "event": "drift", "seconds": r3(span), "in_device": deviceName(inDev), "out_device": deviceName(outDev),
        "in_clock_domain": Int(getValue(inDev, paddr(kAudioDevicePropertyClockDomain), UInt32(0)) ?? 0),
        "out_clock_domain": Int(getValue(outDev, paddr(kAudioDevicePropertyClockDomain), UInt32(0)) ?? 0),
        "in_nominal": nominalRate(inDev), "out_nominal": nominalRate(outDev),
        "in_rate_from_timestamps": r3(inTsRate), "out_rate_from_timestamps": r3(outTsRate),
        "in_rate_from_frames": r3(inFrRate), "out_rate_from_frames": r3(outFrRate),
        "in_ppm_vs_host": r3((inTsRate / nominalRate(inDev) - 1) * 1e6),
        "out_ppm_vs_host": r3((outTsRate / nominalRate(outDev) - 1) * 1e6),
        "in_vs_out_ppm_timestamps": r3((inTsRate / outTsRate - 1) * 1e6),
        "in_vs_out_ppm_frames": r3((inFrRate / outFrRate - 1) * 1e6),
        "frames_in": Int(inFramesTotal), "frames_out": Int(outFramesTotal),
        "frames_in_minus_out": Int(inFramesTotal - outFramesTotal), "samples": ins.count,
    ]))
    fflush(stdout)
}
