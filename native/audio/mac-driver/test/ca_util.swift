// SPDX-License-Identifier: GPL-3.0-only
// Small Core Audio helper for live_check.py (user process, no root):
//   ca_util rate UID            -> prints the device's nominal rate
//   ca_util set-rate UID RATE   -> sets it (the plug-in then moves both of its devices)
//   ca_util visible UID         -> 1 if the device is in kAudioHardwarePropertyDevices, else 0
//   ca_util id UID              -> AudioObjectID from TranslateUIDToDevice (0 = unknown)
//   ca_util volume UID [SCALAR] -> input volume as apps see it (kAudioDevicePropertyVolumeScalar),
//                                  prints "<scalar> settable=<0|1>"; sets it when SCALAR is given
//   ca_util mute UID [0|1]      -> input mute as apps see it (kAudioDevicePropertyMute), same output
import CoreAudio
import Foundation

let system = AudioObjectID(kAudioObjectSystemObject)

func addr(_ s: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: s, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
}

func translate(_ uid: String) -> AudioObjectID {
    var a = addr(kAudioHardwarePropertyTranslateUIDToDevice)
    var cf = uid as CFString
    var dev = AudioObjectID(0)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let st = withUnsafeMutablePointer(to: &cf) {
        AudioObjectGetPropertyData(system, &a, UInt32(MemoryLayout<CFString>.size), $0, &size, &dev)
    }
    return st == noErr ? dev : 0
}

func die(_ msg: String) -> Never { FileHandle.standardError.write((msg + "\n").data(using: .utf8)!); exit(1) }

let args = CommandLine.arguments
guard args.count >= 3 else { die("usage: ca_util rate|set-rate|visible|id UID [RATE]") }
let dev = translate(args[2])
switch args[1] {
case "id":
    print(dev)
case "rate":
    guard dev != 0 else { die("unknown device \(args[2])") }
    var a = addr(kAudioDevicePropertyNominalSampleRate)
    var rate = Float64(0); var size = UInt32(8)
    let st = AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &rate)
    guard st == noErr else { die("get rate failed: \(st)") }
    print(rate)
case "set-rate":
    guard dev != 0, args.count == 4, var rate = Float64(args[3]) else { die("usage: ca_util set-rate UID RATE") }
    var a = addr(kAudioDevicePropertyNominalSampleRate)
    let st = AudioObjectSetPropertyData(dev, &a, 0, nil, 8, &rate)
    guard st == noErr else { die("set rate failed: \(st)") }
    print("ok")
case "visible":
    var a = addr(kAudioHardwarePropertyDevices)
    var size = UInt32(0)
    AudioObjectGetPropertyDataSize(system, &a, 0, nil, &size)
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / 4)
    AudioObjectGetPropertyData(system, &a, 0, nil, &size, &ids)
    print(dev != 0 && ids.contains(dev) ? 1 : 0)
case "volume", "mute":
    guard dev != 0 else { die("unknown device \(args[2])") }
    let isVolume = args[1] == "volume"
    var a = AudioObjectPropertyAddress(mSelector: isVolume ? kAudioDevicePropertyVolumeScalar : kAudioDevicePropertyMute,
                                       mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
    guard AudioObjectHasProperty(dev, &a) else { die("device has no input \(args[1])") }
    if args.count == 4 {
        let st: OSStatus
        if isVolume { var v = Float32(args[3]) ?? -1; st = AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &v) }
        else { var v = UInt32(args[3]) ?? 9; st = AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &v) }
        guard st == noErr else { die("set \(args[1]) failed: \(st)") }
        usleep(100_000)
    }
    var settable: DarwinBoolean = false
    AudioObjectIsPropertySettable(dev, &a, &settable)
    var size = UInt32(4)
    if isVolume { var v = Float32(-1); AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &v); print("\(v) settable=\(settable.boolValue ? 1 : 0)") }
    else { var v = UInt32(9); AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &v); print("\(v) settable=\(settable.boolValue ? 1 : 0)") }
default:
    die("unknown command \(args[1])")
}
