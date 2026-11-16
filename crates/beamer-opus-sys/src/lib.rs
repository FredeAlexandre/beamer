//! libopus 1.6.1, the codec of audio routes, as raw C bindings.
//!
//! The C is vendored in `opus-1.6.1/`, unmodified from xiph's release archive, under its own
//! BSD-3-Clause licence (`opus-1.6.1/COPYING`), and `build.rs` compiles it with `cc`. This crate
//! binds what the media engine calls: one encoder and one decoder per stream, at 48 kHz, with
//! float samples. The settings the encoder is given live with the audio domain, not here.
//!
//! Every function here is `unsafe` to call unless it is marked `safe`: the caller keeps the
//! pointers valid and the lengths true, as libopus's own documentation requires.

#![no_std]

use core::ffi::{c_char, c_int, c_uchar};
use core::marker::{PhantomData, PhantomPinned};

/// An Opus encoder's state, owned by libopus: created by [`opus_encoder_create`], released by
/// [`opus_encoder_destroy`], and only ever handled through a pointer.
#[repr(C)]
pub struct OpusEncoder {
    _state: [u8; 0],
    _owned_by_c: PhantomData<(*mut u8, PhantomPinned)>,
}

/// An Opus decoder's state, owned by libopus: created by [`opus_decoder_create`], released by
/// [`opus_decoder_destroy`], and only ever handled through a pointer.
#[repr(C)]
pub struct OpusDecoder {
    _state: [u8; 0],
    _owned_by_c: PhantomData<(*mut u8, PhantomPinned)>,
}

/// The result of a call that succeeded. Every failure is negative.
pub const OPUS_OK: c_int = 0;

/// The application tuned for speech: voice calls through the microphone.
pub const OPUS_APPLICATION_VOIP: c_int = 2048;
/// The application tuned for music and mixed content.
pub const OPUS_APPLICATION_AUDIO: c_int = 2049;
/// The application with the lowest delay, without the speech-only modes.
pub const OPUS_APPLICATION_RESTRICTED_LOWDELAY: c_int = 2051;

/// [`opus_encoder_ctl`] request: sets the bitrate, in bits per second, as an `i32`.
pub const OPUS_SET_BITRATE_REQUEST: c_int = 4002;
/// [`opus_encoder_ctl`] request: sets the encoder's complexity, 0 to 10, as an `i32`.
pub const OPUS_SET_COMPLEXITY_REQUEST: c_int = 4010;
/// [`opus_encoder_ctl`] request: turns in-band forward error correction on (1) or off (0), as an
/// `i32`.
pub const OPUS_SET_INBAND_FEC_REQUEST: c_int = 4012;
/// [`opus_encoder_ctl`] request: sets the packet loss the encoder expects, in percent, as an
/// `i32`.
pub const OPUS_SET_PACKET_LOSS_PERC_REQUEST: c_int = 4014;
/// [`opus_encoder_ctl`] request: turns discontinuous transmission on (1) or off (0), as an `i32`.
pub const OPUS_SET_DTX_REQUEST: c_int = 4016;

unsafe extern "C" {
    /// Creates an encoder for `fs` samples per second, `channels` interleaved channels (1 or 2)
    /// and one of the `OPUS_APPLICATION_*` applications. Writes [`OPUS_OK`] or the failure to
    /// `error`, and returns null on failure.
    pub fn opus_encoder_create(
        fs: i32,
        channels: c_int,
        application: c_int,
        error: *mut c_int,
    ) -> *mut OpusEncoder;

    /// Releases an encoder made by [`opus_encoder_create`].
    pub fn opus_encoder_destroy(st: *mut OpusEncoder);

    /// Applies one request to an encoder: an `OPUS_SET_*_REQUEST` followed by its `i32` value.
    /// Returns [`OPUS_OK`] or the failure.
    pub fn opus_encoder_ctl(st: *mut OpusEncoder, request: c_int, ...) -> c_int;

    /// Encodes one frame of `frame_size` samples per channel, interleaved floats in -1 to 1, into
    /// at most `max_data_bytes` bytes of `data`. Returns the packet's length in bytes, or the
    /// failure.
    pub fn opus_encode_float(
        st: *mut OpusEncoder,
        pcm: *const f32,
        frame_size: c_int,
        data: *mut c_uchar,
        max_data_bytes: i32,
    ) -> i32;

    /// Creates a decoder for `fs` samples per second and `channels` interleaved channels (1 or
    /// 2). Writes [`OPUS_OK`] or the failure to `error`, and returns null on failure.
    pub fn opus_decoder_create(fs: i32, channels: c_int, error: *mut c_int) -> *mut OpusDecoder;

    /// Releases a decoder made by [`opus_decoder_create`].
    pub fn opus_decoder_destroy(st: *mut OpusDecoder);

    /// Decodes into `pcm`, interleaved floats, at most `frame_size` samples per channel.
    ///
    /// With a packet and `decode_fec` 0, decodes that packet. With the packet that follows a lost
    /// one and `decode_fec` 1, recovers the lost one from its forward error correction. With a
    /// null `data` and a `len` of 0, conceals a lost packet. Returns the samples per channel
    /// written, or the failure.
    pub fn opus_decode_float(
        st: *mut OpusDecoder,
        data: *const c_uchar,
        len: i32,
        pcm: *mut f32,
        frame_size: c_int,
        decode_fec: c_int,
    ) -> c_int;

    /// Whether a packet carries forward error correction for the packet before it: 1 if it does,
    /// 0 if not, or the failure.
    pub fn opus_packet_has_lbrr(packet: *const c_uchar, len: i32) -> c_int;

    /// The failure's description, as a static, NUL-terminated string.
    pub safe fn opus_strerror(error: c_int) -> *const c_char;

    /// libopus's version, as a static, NUL-terminated string.
    pub safe fn opus_get_version_string() -> *const c_char;
}

/// A round trip through the vendored C with the media engine's two settings, the microphone's
/// and system audio's.
#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use core::f64::consts::PI;
    use core::ffi::CStr;
    use core::ptr;
    use std::vec;
    use std::vec::Vec;

    const OPUS_GET_BITRATE_REQUEST: c_int = 4003;
    const OPUS_GET_COMPLEXITY_REQUEST: c_int = 4011;
    const OPUS_GET_INBAND_FEC_REQUEST: c_int = 4013;
    const OPUS_GET_PACKET_LOSS_PERC_REQUEST: c_int = 4015;
    const OPUS_GET_DTX_REQUEST: c_int = 4017;
    const OPUS_BAD_ARG: c_int = -1;

    const RATE: i32 = 48_000;
    /// 10 ms at 48 kHz, the engine's frame for both settings.
    const FRAME: usize = 480;
    const FRAMES: usize = 100;

    struct Setting {
        application: c_int,
        channels: usize,
        bitrate: i32,
        complexity: i32,
        fec: bool,
    }

    /// The engine's microphone setting: speech, mono, 32 kb/s, complexity 5, FEC for 10 % loss.
    const MIC: Setting = Setting {
        application: OPUS_APPLICATION_VOIP,
        channels: 1,
        bitrate: 32_000,
        complexity: 5,
        fec: true,
    };

    /// The engine's system audio setting: music, stereo, 96 kb/s, complexity 9, no FEC.
    const SYSTEM: Setting = Setting {
        application: OPUS_APPLICATION_AUDIO,
        channels: 2,
        bitrate: 96_000,
        complexity: 9,
        fec: false,
    };

    fn get(enc: *mut OpusEncoder, request: c_int) -> i32 {
        let mut value: i32 = -1;
        let status = unsafe { opus_encoder_ctl(enc, request, &mut value as *mut i32) };
        assert_eq!(status, OPUS_OK, "get request {request}");
        value
    }

    fn set(enc: *mut OpusEncoder, request: c_int, value: i32) {
        let status = unsafe { opus_encoder_ctl(enc, request, value) };
        assert_eq!(status, OPUS_OK, "set request {request} to {value}");
    }

    /// A voiced stand-in, as the engine's own codec test used: 12 harmonics of 180 Hz under a
    /// 3 Hz envelope, with 220 Hz on the second channel.
    fn voice(channels: usize, samples: usize) -> Vec<f32> {
        let mut pcm = Vec::with_capacity(samples * channels);
        for n in 0..samples {
            let t = n as f64 / f64::from(RATE);
            let envelope = 0.55 + 0.45 * (2.0 * PI * 3.0 * t).sin();
            for c in 0..channels {
                let f0 = [180.0, 220.0][c];
                let v: f64 = (1..=12)
                    .map(|k| (2.0 * PI * f64::from(k) * f0 * t).sin() / f64::from(k))
                    .sum();
                pcm.push((0.12 * envelope * v) as f32);
            }
        }
        pcm
    }

    /// The best normalised correlation of `b` against `a`, over delays up to `max_delay` frames
    /// of `channels` samples.
    fn best_correlation(a: &[f32], b: &[f32], channels: usize, max_delay: usize) -> f32 {
        (0..=max_delay)
            .map(|delay| {
                let b = &b[delay * channels..];
                let n = a.len().min(b.len());
                let (mut ab, mut aa, mut bb) = (0.0f64, 0.0f64, 0.0f64);
                for i in 0..n {
                    ab += f64::from(a[i]) * f64::from(b[i]);
                    aa += f64::from(a[i]) * f64::from(a[i]);
                    bb += f64::from(b[i]) * f64::from(b[i]);
                }
                (ab / (aa.sqrt() * bb.sqrt())) as f32
            })
            .fold(f32::MIN, f32::max)
    }

    fn round_trip(setting: &Setting) {
        let channels = setting.channels as c_int;
        let mut error = OPUS_BAD_ARG;
        let enc = unsafe { opus_encoder_create(RATE, channels, setting.application, &mut error) };
        assert_eq!(error, OPUS_OK);
        assert!(!enc.is_null());
        let mut error = OPUS_BAD_ARG;
        let dec = unsafe { opus_decoder_create(RATE, channels, &mut error) };
        assert_eq!(error, OPUS_OK);
        assert!(!dec.is_null());

        set(enc, OPUS_SET_BITRATE_REQUEST, setting.bitrate);
        set(enc, OPUS_SET_COMPLEXITY_REQUEST, setting.complexity);
        set(enc, OPUS_SET_INBAND_FEC_REQUEST, i32::from(setting.fec));
        set(
            enc,
            OPUS_SET_PACKET_LOSS_PERC_REQUEST,
            if setting.fec { 10 } else { 0 },
        );
        set(enc, OPUS_SET_DTX_REQUEST, 0);
        assert_eq!(get(enc, OPUS_GET_BITRATE_REQUEST), setting.bitrate);
        assert_eq!(get(enc, OPUS_GET_COMPLEXITY_REQUEST), setting.complexity);
        assert_eq!(
            get(enc, OPUS_GET_INBAND_FEC_REQUEST),
            i32::from(setting.fec)
        );
        assert_eq!(
            get(enc, OPUS_GET_PACKET_LOSS_PERC_REQUEST),
            if setting.fec { 10 } else { 0 }
        );
        assert_eq!(get(enc, OPUS_GET_DTX_REQUEST), 0);

        let input = voice(setting.channels, FRAME * FRAMES);
        let mut packets = Vec::new();
        for frame in input.chunks(FRAME * setting.channels) {
            let mut packet = vec![0u8; 1500];
            let len = unsafe {
                opus_encode_float(
                    enc,
                    frame.as_ptr(),
                    FRAME as c_int,
                    packet.as_mut_ptr(),
                    1500,
                )
            };
            assert!(len > 0, "encode failed: {len}");
            packet.truncate(len as usize);
            packets.push(packet);
        }

        // The bitrate holds: on average within 25 % of the setting.
        let bytes: usize = packets.iter().map(Vec::len).sum();
        let bitrate = bytes as f32 * 8.0 / (FRAMES as f32 * 0.010);
        assert!(
            (bitrate - setting.bitrate as f32).abs() < 0.25 * setting.bitrate as f32,
            "{bitrate} b/s against {}",
            setting.bitrate
        );

        // FEC is carried in most packets when it is on, and in none when it is off.
        let lbrr = packets
            .iter()
            .filter(|p| unsafe { opus_packet_has_lbrr(p.as_ptr(), p.len() as i32) } == 1)
            .count();
        if setting.fec {
            assert!(lbrr > FRAMES / 2, "FEC in {lbrr} of {FRAMES} packets");
        } else {
            assert_eq!(lbrr, 0);
        }

        let mut output = Vec::new();
        let mut pcm = vec![0f32; FRAME * setting.channels];
        for packet in &packets {
            let decoded = unsafe {
                opus_decode_float(
                    dec,
                    packet.as_ptr(),
                    packet.len() as i32,
                    pcm.as_mut_ptr(),
                    FRAME as c_int,
                    0,
                )
            };
            assert_eq!(decoded, FRAME as c_int);
            output.extend_from_slice(&pcm);
        }

        // What comes out is what went in, a few milliseconds later. The first 20 ms are skipped
        // while the codec settles.
        let settle = 2 * FRAME * setting.channels;
        let correlation =
            best_correlation(&input[settle..], &output[settle..], setting.channels, FRAME);
        assert!(correlation > 0.9, "correlation {correlation}");

        // A lost packet is concealed, and recovered from the next one's FEC.
        let concealed =
            unsafe { opus_decode_float(dec, ptr::null(), 0, pcm.as_mut_ptr(), FRAME as c_int, 0) };
        assert_eq!(concealed, FRAME as c_int);
        let next = &packets[FRAMES - 1];
        let recovered = unsafe {
            opus_decode_float(
                dec,
                next.as_ptr(),
                next.len() as i32,
                pcm.as_mut_ptr(),
                FRAME as c_int,
                1,
            )
        };
        assert_eq!(recovered, FRAME as c_int);

        unsafe {
            opus_encoder_destroy(enc);
            opus_decoder_destroy(dec);
        }
    }

    #[test]
    fn the_microphone_setting_round_trips() {
        round_trip(&MIC);
    }

    #[test]
    fn the_system_audio_setting_round_trips() {
        round_trip(&SYSTEM);
    }

    #[test]
    fn the_vendored_version_is_1_6_1() {
        let version = unsafe { CStr::from_ptr(opus_get_version_string()) };
        assert_eq!(version.to_str(), Ok("libopus 1.6.1"));
    }

    #[test]
    fn a_failure_is_described() {
        let text = unsafe { CStr::from_ptr(opus_strerror(OPUS_BAD_ARG)) };
        assert_eq!(text.to_str(), Ok("invalid argument"));
    }

    #[test]
    fn a_bad_channel_count_is_refused() {
        let mut error = OPUS_OK;
        let enc = unsafe { opus_encoder_create(RATE, 3, OPUS_APPLICATION_VOIP, &mut error) };
        assert!(enc.is_null());
        assert_eq!(error, OPUS_BAD_ARG);
    }
}
