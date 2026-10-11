# Low-latency system audio

The audio path sends the logged-in Mac's system sound to the paired Windows computer. It does not capture a microphone or forward the Windows microphone. Both endpoints must include the audio implementation; older hosts and viewers retain their existing video/input behavior.

System audio and playback diagnostics are included in `v0.3.0`; historical `v0.2.1` packages do not contain audio. Update both endpoints before testing these features.

## Capture and packets

ScreenCaptureKit captures 48 kHz stereo audio on a separate serial queue, excluding the host application's own audio. Float32 planar/interleaved samples become saturated signed PCM16; non-finite input becomes silence. Each packet contains 240 stereo frames (5 ms), a 32-byte `RemoteAudioHeader`, and 960 bytes of little-endian PCM. One 992-byte packet fits a QUIC datagram without video-style fragmentation. Raw audio uses 1.536 Mbps before headers and encryption. The LAN path deliberately avoids a codec's lookahead and processing cost; it is not a bandwidth-saving WAN audio mode.

A paired viewer requests `{"type":"audio-start","version":1}` on CONTROL. The host reports availability with `audio-status`; Windows starts playback after a compatible `running` status. CONTROL messages and media datagrams can arrive in different orders. Transport permission resets on every session, so an old viewer never receives unrecognized audio datagrams, including a fast reconnect that the native status timer misses. `audio-stop` revokes the subscription. No audio is forwarded before pairing.

Audio never enters the input or video poll queue. One media datagram receiver routes audio into a separate eight-packet queue. Capacity accommodates source bursts: a 1024-frame ScreenCaptureKit callback can produce five packets together. Packets still expire after 20 ms of actual queue waiting; the eight slots do not extend this deadline. The sender prioritizes audio between video fragments and preserves video's explicit IDR recovery. On a static desktop it drains remaining sound before waiting for another notification. Quinn's media send buffer is bounded to 4 KiB. Audio problems degrade sound while the desktop remains connected.

## Playback

Windows uses an independent MMCSS/WASAPI event-driven worker. It requests a supported low-latency shared engine period with `IAudioClient3`, and falls back when the output device cannot provide it. It respects the device's actual sample rate, channel layout and float/PCM format. A short adaptive jitter buffer absorbs arrival variation; continuous rate correction handles independent capture/playback clocks. Large obsolete backlogs are discarded with a transition, rather than replayed at increasing delay. Lost audio fades toward silence and resumes with a fade; severe starvation reprimes the buffer. These measures reduce clicks, but cannot reconstruct missing music or guarantee perfect sound on a congested network.

The adaptive source-buffer target ranges from 10 to 30 ms. Queued duration rises and falls as capture batches arrive; it need not equal the target at a snapshot. Backlog trimming starts above a batch-dependent threshold of 45–60 ms and resumes in the newest contiguous packet tail. This transient allowance prevents normal catch-up batches from causing unnecessary drops; it does not raise the adaptive target or the transport's 20 ms queue-expiry deadline. Device lead is limited to two negotiated engine periods.

The player follows Windows' default output device and retries after device loss. Use **More → Mute system audio** to mute; muted playback still drains the live stream, so unmuting does not play an old backlog.

The metrics panel distinguishes:

- **Audio buffer / target:** queued source duration and adaptive target, excluding the sound card's queue.
- **Device queue / period:** observed WASAPI padding and negotiated engine period.
- **Audio underruns / loss:** source-buffer starvation events and packet sequences absent at their playback deadline, including late arrivals. These are player counters, not a measurement of raw network packet loss.
- **Device depletion events:** observed empty device padding during an active, primed stream after warmup. This is a scheduling diagnostic, not a driver-provided count of all hardware xruns. It can overlap a source underrun.

The 5 ms packet duration, network RTT and buffer/period metrics are **not measured capture-to-speaker latency**. Mac capture timestamps and Windows monotonic clocks have different origins. Long playback, real listening, Bluetooth output and capture-to-speaker timing need separate desktop acceptance.

## Validation

### Macs without an output device

System audio capture needs a functioning Core Audio output. Some GPU-passthrough VMs have no output device even though video works. Check `system_profiler SPAudioDataType` and a local player before diagnosing the network. In that case, back up the installed host app, its login agent, and `/Library/Preferences/Audio/*.plist` before changing audio devices. An optional [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole) installation can provide a virtual output; select it in macOS Sound settings and set 48 kHz in Audio MIDI Setup. This is a separate installation, not a bundled GlideBeam component or a microphone-forwarding feature. Follow its official installer restart instructions if the device has not appeared.

The macOS 15.8.1 VM used during development initially listed zero audio devices and `afplay` failed. The app, login agent and audio preferences were backed up before installing [BlackHole 2ch 0.7.1](https://github.com/ExistentialAudio/BlackHole/releases/tag/v0.7.1). Its signed and notarized package was verified against [Homebrew's cask checksum](https://github.com/Homebrew/homebrew-cask/blob/master/Casks/b/blackhole-2ch.rb). After installation and a Core Audio service restart, it appeared as a 48 kHz stereo default output and local playback started. A physical HDMI/USB output can be used instead when available. If capture previously degraded to video only, use the Mac host's explicit capture retry to reattempt audio attachment.

### Automated and paired checks

Hosted tests run the packetizer, playout simulations and Windows sample-format probe without recording a desktop or playing sound. Transport tests exercise real authenticated UDP loopback, compatibility, queue pressure, simultaneous video/input and shutdown.

On 2026-10-11, a 30-second paired test used the macOS 15.8.1 Intel VM and Windows x64 client described in [compatibility](compatibility.md). It included an eight-second known stereo tone (440/660 Hz), real ScreenCaptureKit capture, authenticated QUIC transport and the production WASAPI player. Playback was muted to avoid disturbing the local user; only counters and timing metadata were retained.

| Observation | Result |
| --- | --- |
| Format and received stream | 48 kHz stereo PCM16; 5,984 packets, no sequence gaps |
| Source underruns / missing / late / discarded audio | All zero |
| Observed device depletion events | Zero |
| Adaptive target / source-queue snapshot | 20 ms / 32.22 ms, including batch variation |
| Device queue / engine period | 5.33 ms / 2.67 ms |
| Known audio detected | 383,676 nonzero stereo frames |

This establishes a working capture-to-render pipeline in that short run. It does not establish human listening quality, sustained operation or measured capture-to-speaker latency.

For an opt-in local WASAPI check, compile the probe and run `audio-player --device`; it generates silent PCM, opens the default sound device, checks active playback and clean shutdown, and does not use a real Mac session. For paired acceptance, play a known sound on the Mac, test quiet/loud transitions and several minutes of speech/music while moving windows or transferring files, then exercise mute, reconnect and output-device switching. Record actual drop/underrun counters and check that delay does not grow over time. A silent device probe alone is not listening acceptance.

Primary API references: [ScreenCaptureKit audio configuration](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/capturesaudio), [WASAPI shared low-latency initialization](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream), [Windows low-latency audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio).
