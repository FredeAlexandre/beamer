// beamer-audio: Phase-1 audio prototype for Beamer (macOS side).
// Raw f32le interleaved audio on stdout/stdin, diagnostics as JSON lines on stderr.
import Foundation

let usage = """
usage:
  beamer-audio list [--json] [--verbose]
  beamer-audio watch
  beamer-audio capture-mic    [--device UID|NAME] [--rate 48000] [--channels 1] [--buffer-frames N] [--stats S]
  beamer-audio capture-system [--device UID] [--mute-local] [--rate 48000] [--channels 2] [--buffer-frames N] [--stats S]
                              [--clocked] [--include-self] [--muted-always]
  beamer-audio play           [--device UID|NAME] [--rate 48000] [--channels 2] [--buffer-frames N]
                              [--buffer-ms 80] [--prefill-ms 20] [--stats S]
  beamer-audio measure props|buffers|tap-latency|loop-latency|drift [...]
"""

_ = startHost  // pin the time origin before anything else (globals are lazy)
let args = Args(Array(CommandLine.arguments.dropFirst()))
switch args.command.first {
case "list": cmdList(args)
case "watch": cmdWatch(args)
case "capture-mic": cmdCaptureMic(args)
case "capture-system": cmdCaptureSystem(args)
case "play": cmdPlay(args)
case "measure": cmdMeasure(args)
default:
    FileHandle.standardError.write((usage + "\n").data(using: .utf8)!)
    exit(2)
}
