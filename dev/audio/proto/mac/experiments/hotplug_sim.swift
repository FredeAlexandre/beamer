// Build: swiftc -O experiments/hotplug_sim.swift -o .build/hotplug_sim ; run while `beamer-audio watch` runs.
// Simulates hot-plug for testing `beamer-audio watch`: creates a public aggregate device
// (wrapping the built-in mic), briefly makes it the default input, restores, destroys it.
import CoreAudio
import Foundation
let sys = AudioObjectID(kAudioObjectSystemObject)
func getDefIn() -> AudioObjectID {
    var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultInputDevice, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var d = AudioObjectID(0); var sz = UInt32(4); AudioObjectGetPropertyData(sys, &a, 0, nil, &sz, &d); return d
}
func setDefIn(_ d: AudioObjectID) -> OSStatus {
    var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultInputDevice, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var v = d; return AudioObjectSetPropertyData(sys, &a, 0, nil, 4, &v)
}
let original = getDefIn()
let dict: [String: Any] = [kAudioAggregateDeviceNameKey: "beamer-audio hotplug test", kAudioAggregateDeviceUIDKey: "net.beamer.audio.hotplugtest",
    kAudioAggregateDeviceSubDeviceListKey: [[kAudioSubDeviceUIDKey: "BuiltInMicrophoneDevice"]], kAudioAggregateDeviceMainSubDeviceKey: "BuiltInMicrophoneDevice"]
var agg = AudioObjectID(0)
print("create", AudioHardwareCreateAggregateDevice(dict as CFDictionary, &agg), agg)
sleep(1)
if CommandLine.arguments.contains("--default") {
    print("set default", setDefIn(agg)); sleep(1)
    print("restore", setDefIn(original)); sleep(1)
}
print("destroy", AudioHardwareDestroyAggregateDevice(agg))
sleep(1)
print("default input restored:", getDefIn() == original, original)
