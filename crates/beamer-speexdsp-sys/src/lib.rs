//! speexdsp 1.2.1 as raw C bindings: the echo canceller, its preprocessor and the resampler.
//!
//! The C is vendored in `speexdsp-1.2.1/`, unmodified from xiph's release archive, under its own
//! BSD-3-Clause licence (`speexdsp-1.2.1/COPYING`), and `build.rs` compiles it with `cc`. This
//! crate binds what the media engine calls: one echo canceller per forwarded microphone, at
//! 48 kHz with 16-bit samples, and the preprocessor's residual echo suppression after it. The
//! frame, the filter length and the suppression levels live with the audio domain, not here.
//!
//! Every function here is `unsafe` to call unless it is marked `safe`: the caller keeps the
//! pointers valid and the lengths true, as speexdsp's own headers require.

#![no_std]

use core::ffi::{c_char, c_int, c_void};
use core::marker::{PhantomData, PhantomPinned};

/// An echo canceller's state, owned by speexdsp: created by [`speex_echo_state_init`], released
/// by [`speex_echo_state_destroy`], and only ever handled through a pointer.
#[repr(C)]
pub struct SpeexEchoState {
    _state: [u8; 0],
    _owned_by_c: PhantomData<(*mut u8, PhantomPinned)>,
}

/// A preprocessor's state, owned by speexdsp: created by [`speex_preprocess_state_init`],
/// released by [`speex_preprocess_state_destroy`], and only ever handled through a pointer.
#[repr(C)]
pub struct SpeexPreprocessState {
    _state: [u8; 0],
    _owned_by_c: PhantomData<(*mut u8, PhantomPinned)>,
}

/// A resampler's state, owned by speexdsp: created by [`speex_resampler_init`], released by
/// [`speex_resampler_destroy`], and only ever handled through a pointer.
#[repr(C)]
pub struct SpeexResamplerState {
    _state: [u8; 0],
    _owned_by_c: PhantomData<(*mut u8, PhantomPinned)>,
}

/// [`speex_echo_ctl`] request: sets the sampling rate, in samples per second, from an `i32`.
pub const SPEEX_ECHO_SET_SAMPLING_RATE: c_int = 24;
/// [`speex_echo_ctl`] request: writes the length of the echo path's impulse response, in
/// samples, to an `i32`.
pub const SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE: c_int = 27;
/// [`speex_echo_ctl`] request: writes the echo path's impulse response, as the filter estimates
/// it, to an array of `i32` as long as [`SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE`] says.
pub const SPEEX_ECHO_GET_IMPULSE_RESPONSE: c_int = 29;

/// [`speex_preprocess_ctl`] request: turns the denoiser on (1) or off (0), from an `i32`. The
/// residual echo suppression runs only while it is on.
pub const SPEEX_PREPROCESS_SET_DENOISE: c_int = 0;
/// [`speex_preprocess_ctl`] request: turns automatic gain control on (1) or off (0), from an
/// `i32`.
pub const SPEEX_PREPROCESS_SET_AGC: c_int = 2;
/// [`speex_preprocess_ctl`] request: turns dereverberation on (1) or off (0), from an `i32`.
pub const SPEEX_PREPROCESS_SET_DEREVERB: c_int = 8;
/// [`speex_preprocess_ctl`] request: sets the most the noise is attenuated, in dB (0 or
/// negative), from an `i32`.
pub const SPEEX_PREPROCESS_SET_NOISE_SUPPRESS: c_int = 18;
/// [`speex_preprocess_ctl`] request: sets the most the residual echo is attenuated, in dB (0 or
/// negative), from an `i32`.
pub const SPEEX_PREPROCESS_SET_ECHO_SUPPRESS: c_int = 20;
/// [`speex_preprocess_ctl`] request: sets the most the residual echo is attenuated while the
/// near end talks, in dB (0 or negative), from an `i32`.
pub const SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE: c_int = 22;
/// [`speex_preprocess_ctl`] request: gives the preprocessor the echo canceller whose residual
/// echo it suppresses, as a `*mut SpeexEchoState` passed as the pointer itself, or null for none.
pub const SPEEX_PREPROCESS_SET_ECHO_STATE: c_int = 24;

/// The result of a resampler call that succeeded. Every failure is positive.
pub const RESAMPLER_ERR_SUCCESS: c_int = 0;

/// The resampler's lowest quality, and its fastest.
pub const SPEEX_RESAMPLER_QUALITY_MIN: c_int = 0;
/// The resampler's quality meant for voice calls.
pub const SPEEX_RESAMPLER_QUALITY_VOIP: c_int = 3;
/// The resampler's default quality.
pub const SPEEX_RESAMPLER_QUALITY_DEFAULT: c_int = 4;
/// The resampler's quality meant for desktop audio.
pub const SPEEX_RESAMPLER_QUALITY_DESKTOP: c_int = 5;
/// The resampler's highest quality, and its slowest.
pub const SPEEX_RESAMPLER_QUALITY_MAX: c_int = 10;

unsafe extern "C" {
    /// Creates an echo canceller for frames of `frame_size` samples and an echo path up to
    /// `filter_length` samples long. Returns null on failure.
    pub fn speex_echo_state_init(frame_size: c_int, filter_length: c_int) -> *mut SpeexEchoState;

    /// Releases an echo canceller made by [`speex_echo_state_init`].
    pub fn speex_echo_state_destroy(st: *mut SpeexEchoState);

    /// Cancels the echo of one frame: `rec` is what the microphone captured, `play` what the
    /// speakers played, aligned so that its echo lags it, and `out` receives the microphone
    /// without the echo. Each holds one frame.
    pub fn speex_echo_cancellation(
        st: *mut SpeexEchoState,
        rec: *const i16,
        play: *const i16,
        out: *mut i16,
    );

    /// Forgets the echo path the canceller has learnt, as if it were new.
    pub fn speex_echo_state_reset(st: *mut SpeexEchoState);

    /// Applies one `SPEEX_ECHO_*` request to an echo canceller, reading or writing through `ptr`.
    /// Returns 0, or -1 for an unknown request.
    pub fn speex_echo_ctl(st: *mut SpeexEchoState, request: c_int, ptr: *mut c_void) -> c_int;

    /// Creates a preprocessor for frames of `frame_size` samples at `sampling_rate` samples per
    /// second. Returns null on failure.
    pub fn speex_preprocess_state_init(
        frame_size: c_int,
        sampling_rate: c_int,
    ) -> *mut SpeexPreprocessState;

    /// Releases a preprocessor made by [`speex_preprocess_state_init`].
    pub fn speex_preprocess_state_destroy(st: *mut SpeexPreprocessState);

    /// Processes one frame of `x` in place. Returns whether the frame holds speech, which means
    /// something only while voice activity detection is on.
    pub fn speex_preprocess_run(st: *mut SpeexPreprocessState, x: *mut i16) -> c_int;

    /// Applies one `SPEEX_PREPROCESS_*` request to a preprocessor, reading or writing through
    /// `ptr`. Returns 0, or -1 for an unknown request.
    pub fn speex_preprocess_ctl(
        st: *mut SpeexPreprocessState,
        request: c_int,
        ptr: *mut c_void,
    ) -> c_int;

    /// Creates a resampler for `nb_channels` channels from `in_rate` to `out_rate` samples per
    /// second, at a quality from [`SPEEX_RESAMPLER_QUALITY_MIN`] to
    /// [`SPEEX_RESAMPLER_QUALITY_MAX`]. Writes [`RESAMPLER_ERR_SUCCESS`] or the failure to `err`,
    /// and returns null on failure.
    pub fn speex_resampler_init(
        nb_channels: u32,
        in_rate: u32,
        out_rate: u32,
        quality: c_int,
        err: *mut c_int,
    ) -> *mut SpeexResamplerState;

    /// Releases a resampler made by [`speex_resampler_init`].
    pub fn speex_resampler_destroy(st: *mut SpeexResamplerState);

    /// Resamples interleaved floats: reads up to `*in_len` samples per channel from `input`,
    /// writes up to `*out_len` to `output`, then sets each length to the samples per channel it
    /// read and wrote. Returns [`RESAMPLER_ERR_SUCCESS`] or the failure.
    pub fn speex_resampler_process_interleaved_float(
        st: *mut SpeexResamplerState,
        input: *const f32,
        in_len: *mut u32,
        output: *mut f32,
        out_len: *mut u32,
    ) -> c_int;

    /// Changes the ratio of input to output samples to `ratio_num / ratio_den`, which needn't
    /// equal `in_rate / out_rate`: a clock running a few ppm apart is followed this way. Returns
    /// [`RESAMPLER_ERR_SUCCESS`] or the failure.
    pub fn speex_resampler_set_rate_frac(
        st: *mut SpeexResamplerState,
        ratio_num: u32,
        ratio_den: u32,
        in_rate: u32,
        out_rate: u32,
    ) -> c_int;

    /// Forgets the samples a resampler holds, as if it were new. Returns
    /// [`RESAMPLER_ERR_SUCCESS`] or the failure.
    pub fn speex_resampler_reset_mem(st: *mut SpeexResamplerState) -> c_int;

    /// The resampler failure's description, as a static, NUL-terminated string.
    pub safe fn speex_resampler_strerror(err: c_int) -> *const c_char;
}

/// A round trip through the vendored C with the media engine's echo canceller setting, and a
/// resampling of a tone.
#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use core::f64::consts::PI;
    use core::ffi::CStr;
    use std::vec;
    use std::vec::Vec;

    const SPEEX_ECHO_GET_SAMPLING_RATE: c_int = 25;
    const SPEEX_ECHO_GET_FRAME_SIZE: c_int = 3;
    const SPEEX_PREPROCESS_GET_DENOISE: c_int = 1;
    const SPEEX_PREPROCESS_GET_AGC: c_int = 3;
    const SPEEX_PREPROCESS_GET_DEREVERB: c_int = 9;
    const SPEEX_PREPROCESS_GET_NOISE_SUPPRESS: c_int = 19;
    const SPEEX_PREPROCESS_GET_ECHO_SUPPRESS: c_int = 21;
    const SPEEX_PREPROCESS_GET_ECHO_SUPPRESS_ACTIVE: c_int = 23;
    const RESAMPLER_ERR_INVALID_ARG: c_int = 3;

    const RATE: c_int = 48_000;
    /// 10 ms at 48 kHz, the engine's echo canceller frame.
    const FRAME: usize = 480;
    /// 100 ms at 48 kHz, the engine's echo canceller filter.
    const TAIL: c_int = 4800;
    /// 44 ms at 48 kHz: the echo's delay, as the engine's own echo test used.
    const DELAY: usize = 2112;

    /// Deterministic white noise at about -20 dBFS, in 16-bit samples.
    fn noise(samples: usize, seed: u32) -> Vec<i16> {
        let mut state = seed;
        (0..samples)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                ((state >> 16) as i16) / 10
            })
            .collect()
    }

    fn energy(x: &[i16]) -> f64 {
        x.iter().map(|&s| f64::from(s) * f64::from(s)).sum()
    }

    fn echo_ctl(st: *mut SpeexEchoState, request: c_int, value: &mut i32) {
        let status = unsafe { speex_echo_ctl(st, request, (value as *mut i32).cast()) };
        assert_eq!(status, 0, "echo request {request}");
    }

    fn preprocess_set(st: *mut SpeexPreprocessState, request: c_int, mut value: i32) {
        let status = unsafe { speex_preprocess_ctl(st, request, (&mut value as *mut i32).cast()) };
        assert_eq!(status, 0, "preprocess request {request}");
    }

    fn preprocess_get(st: *mut SpeexPreprocessState, request: c_int) -> i32 {
        let mut value = i32::MIN;
        let status = unsafe { speex_preprocess_ctl(st, request, (&mut value as *mut i32).cast()) };
        assert_eq!(status, 0, "preprocess request {request}");
        value
    }

    #[test]
    fn an_echo_is_cancelled_and_its_residual_suppressed() {
        let echo = unsafe { speex_echo_state_init(FRAME as c_int, TAIL) };
        assert!(!echo.is_null());
        let mut rate = RATE;
        echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &mut rate);
        let mut value = 0;
        echo_ctl(echo, SPEEX_ECHO_GET_SAMPLING_RATE, &mut value);
        assert_eq!(value, RATE);
        echo_ctl(echo, SPEEX_ECHO_GET_FRAME_SIZE, &mut value);
        assert_eq!(value, FRAME as i32);
        let mut ir_size = 0;
        echo_ctl(echo, SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE, &mut ir_size);
        assert_eq!(ir_size, TAIL);

        // The engine's residual echo suppression: the denoiser on with a 0 dB noise floor, no
        // AGC or dereverb, -40 dB of residual echo and -15 dB while the near end talks.
        let pre = unsafe { speex_preprocess_state_init(FRAME as c_int, RATE) };
        assert!(!pre.is_null());
        preprocess_set(pre, SPEEX_PREPROCESS_SET_DENOISE, 1);
        preprocess_set(pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, 0);
        preprocess_set(pre, SPEEX_PREPROCESS_SET_AGC, 0);
        preprocess_set(pre, SPEEX_PREPROCESS_SET_DEREVERB, 0);
        preprocess_set(pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, -40);
        preprocess_set(pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, -15);
        assert_eq!(preprocess_get(pre, SPEEX_PREPROCESS_GET_DENOISE), 1);
        assert_eq!(preprocess_get(pre, SPEEX_PREPROCESS_GET_NOISE_SUPPRESS), 0);
        assert_eq!(preprocess_get(pre, SPEEX_PREPROCESS_GET_AGC), 0);
        assert_eq!(preprocess_get(pre, SPEEX_PREPROCESS_GET_DEREVERB), 0);
        assert_eq!(preprocess_get(pre, SPEEX_PREPROCESS_GET_ECHO_SUPPRESS), -40);
        assert_eq!(
            preprocess_get(pre, SPEEX_PREPROCESS_GET_ECHO_SUPPRESS_ACTIVE),
            -15
        );
        let status =
            unsafe { speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_ECHO_STATE, echo.cast()) };
        assert_eq!(status, 0);

        // 8 s of far end, heard by the microphone 44 ms later at -10 dB.
        let frames = 800;
        let play = noise(FRAME * frames, 1);
        let mut rec = vec![0i16; play.len()];
        for i in DELAY..rec.len() {
            rec[i] = (f64::from(play[i - DELAY]) * 0.316) as i16;
        }
        let mut cancelled = vec![0i16; rec.len()];
        let mut suppressed = vec![0i16; rec.len()];
        for k in 0..frames {
            let at = k * FRAME..(k + 1) * FRAME;
            unsafe {
                speex_echo_cancellation(
                    echo,
                    rec[at.clone()].as_ptr(),
                    play[at.clone()].as_ptr(),
                    cancelled[at.clone()].as_mut_ptr(),
                );
            }
            suppressed[at.clone()].copy_from_slice(&cancelled[at.clone()]);
            unsafe { speex_preprocess_run(pre, suppressed[at].as_mut_ptr()) };
        }

        // Over the last 3 s the canceller removes at least 20 dB of echo, as the engine's own
        // test asked, and the suppressor removes more.
        let last = rec.len() - 3 * RATE as usize..;
        let erle = 10.0 * (energy(&rec[last.clone()]) / energy(&cancelled[last.clone()])).log10();
        assert!(erle >= 20.0, "the canceller removed {erle:.1} dB");
        let total = 10.0 * (energy(&rec[last.clone()]) / energy(&suppressed[last])).log10();
        assert!(
            total > erle,
            "{total:.1} dB with suppression, {erle:.1} without"
        );

        // The echo path the filter learnt peaks at the echo's delay.
        let mut ir = vec![0i32; ir_size as usize];
        let status = unsafe {
            speex_echo_ctl(
                echo,
                SPEEX_ECHO_GET_IMPULSE_RESPONSE,
                ir.as_mut_ptr().cast(),
            )
        };
        assert_eq!(status, 0);
        let peak = (0..ir.len()).max_by_key(|&i| ir[i].unsigned_abs()).unwrap();
        assert!(peak.abs_diff(DELAY) <= 2, "peak at {peak}, echo at {DELAY}");

        // After a reset the echo comes through again.
        unsafe { speex_echo_state_reset(echo) };
        let at = FRAME * (frames - 1)..;
        let mut out = vec![0i16; FRAME];
        unsafe {
            speex_echo_cancellation(
                echo,
                rec[at.clone()].as_ptr(),
                play[at.clone()].as_ptr(),
                out.as_mut_ptr(),
            );
        }
        assert!(energy(&out) > 0.5 * energy(&rec[at]));

        unsafe {
            speex_preprocess_state_destroy(pre);
            speex_echo_state_destroy(echo);
        }
    }

    fn resampler(channels: u32, from: u32, to: u32) -> *mut SpeexResamplerState {
        let mut err = RESAMPLER_ERR_INVALID_ARG;
        let st = unsafe {
            speex_resampler_init(
                channels,
                from,
                to,
                SPEEX_RESAMPLER_QUALITY_DESKTOP,
                &mut err,
            )
        };
        assert_eq!(err, RESAMPLER_ERR_SUCCESS);
        assert!(!st.is_null());
        st
    }

    /// Feeds `input` (interleaved, `channels` wide) in 10 ms blocks; returns the output and the
    /// input samples per channel it consumed.
    fn resample(
        st: *mut SpeexResamplerState,
        channels: usize,
        block: usize,
        input: &[f32],
    ) -> (Vec<f32>, usize) {
        let mut output = Vec::new();
        let mut consumed = 0;
        let mut buffer = vec![0f32; 4 * block * channels];
        for chunk in input.chunks(block * channels) {
            let mut at = 0;
            while at < chunk.len() {
                let mut in_len = ((chunk.len() - at) / channels) as u32;
                let mut out_len = (buffer.len() / channels) as u32;
                let status = unsafe {
                    speex_resampler_process_interleaved_float(
                        st,
                        chunk[at..].as_ptr(),
                        &mut in_len,
                        buffer.as_mut_ptr(),
                        &mut out_len,
                    )
                };
                assert_eq!(status, RESAMPLER_ERR_SUCCESS);
                at += in_len as usize * channels;
                consumed += in_len as usize;
                output.extend_from_slice(&buffer[..out_len as usize * channels]);
            }
        }
        (output, consumed)
    }

    /// Two tones, one per channel, interleaved.
    fn tone(rate: f64, hz: [f64; 2], samples: usize) -> Vec<f32> {
        (0..samples)
            .flat_map(|n| {
                let t = n as f64 / rate;
                hz.map(|f| (0.5 * (2.0 * PI * f * t).sin()) as f32)
            })
            .collect()
    }

    #[test]
    fn a_tone_keeps_its_pitch_through_the_resampler() {
        let st = resampler(2, 48_000, 44_100);
        let input = tone(48_000.0, [440.0, 1000.0], 48_000);
        let (output, consumed) = resample(st, 2, 480, &input);
        assert_eq!(consumed, 48_000);
        let frames = output.len() / 2;
        assert!(frames.abs_diff(44_100) < 200, "{frames} frames out");

        // Past its latency, the output is the same tones sampled at 44.1 kHz, a few samples
        // later.
        let reference = tone(44_100.0, [440.0, 1000.0], frames);
        let skip = 2 * 200;
        let best = (0..100)
            .map(|delay| {
                let out = &output[skip + 2 * delay..];
                let reference = &reference[skip..];
                let n = out.len().min(reference.len());
                let (mut ab, mut aa, mut bb) = (0.0f64, 0.0f64, 0.0f64);
                for i in 0..n {
                    let (a, b) = (f64::from(reference[i]), f64::from(out[i]));
                    ab += a * b;
                    aa += a * a;
                    bb += b * b;
                }
                ab / (aa.sqrt() * bb.sqrt())
            })
            .fold(f64::MIN, f64::max);
        assert!(best > 0.99, "correlation {best}");

        unsafe { speex_resampler_destroy(st) };
    }

    #[test]
    fn a_fractional_ratio_follows_a_clock_100_ppm_apart() {
        let input = vec![0.25f32; 48_000 * 10];
        let even = resampler(1, 48_000, 48_000);
        let (even_out, _) = resample(even, 1, 480, &input);
        let fast = resampler(1, 48_000, 48_000);
        let status =
            unsafe { speex_resampler_set_rate_frac(fast, 1_000_100, 1_000_000, 48_000, 48_000) };
        assert_eq!(status, RESAMPLER_ERR_SUCCESS);
        let (fast_out, consumed) = resample(fast, 1, 480, &input);
        assert_eq!(consumed, input.len());
        // 10 s read 100 ppm fast come out 48 samples shorter than at an even ratio.
        let short = even_out.len() as i64 - fast_out.len() as i64;
        assert!((short - 48).abs() <= 2, "{short} samples short");

        assert_eq!(
            unsafe { speex_resampler_reset_mem(fast) },
            RESAMPLER_ERR_SUCCESS
        );
        unsafe {
            speex_resampler_destroy(even);
            speex_resampler_destroy(fast);
        }
    }

    #[test]
    fn a_bad_resampler_is_refused_and_described() {
        let mut err = RESAMPLER_ERR_SUCCESS;
        let st = unsafe { speex_resampler_init(0, 48_000, 44_100, 3, &mut err) };
        assert!(st.is_null());
        assert_eq!(err, RESAMPLER_ERR_INVALID_ARG);
        let text = unsafe { CStr::from_ptr(speex_resampler_strerror(err)) };
        assert_eq!(text.to_str(), Ok("Invalid argument."));
    }
}
