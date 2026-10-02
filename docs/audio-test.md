# Watch speaker delivery test

Version 0.1.9 adds **Play test sound on watch** to Android Diagnostics. Use the matching APK and PBW, select the Pebble phone host, and keep Hermes open on the watch. The phone probes the open app, sends a bundled voice clip, and displays progress through playback completion. It never requests a watch app launch.

The bundled phrase is “Hello from your Pebble”: 13,800 bytes, 1.725 seconds, mono signed 8-bit PCM at 8 kHz, without a WAV header. It was synthesized locally using FFmpeg's Flite SLT voice:

```sh
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i "flite=text='Hello from your Pebble':voice=slt" \
  -af 'highpass=f=120,lowpass=f=3500,alimiter=limit=0.8:level=false' \
  -ar 8000 -ac 1 -c:a pcm_s8 -f s8 android/app/src/main/assets/watch_test.s8
```

The test uses no network speech service or Hermes credentials. Speech is generated once for the bundled asset, not on the watch or at runtime. The code uses the [speaker streaming API exposed by SDK 4.33.1](https://github.com/coredevices/PebbleOS/blob/v4.33.1/src/fw/applib/ui/speaker.h) for emery: `speaker_stream_open`, `speaker_stream_write`, `speaker_stream_close`, and `speaker_set_finish_callback`. A physical Time 2 with compatible firmware is required to confirm audible playback.

The watch first buffers the entire clip in temporary heap memory, with a maximum of 16,000 bytes (two seconds at this format). Android sends 1,024-byte blocks over AppMessage, split into 192-byte chunks, and waits for each watch receipt. The watch checks the length and whole-clip checksum before opening its speaker at volume 80/100. This avoids making playback depend on Bluetooth sustaining the sample rate. Upload time depends on the phone host and connection; this test measures delivery rather than real-time streaming.

Writes to the speaker may accept only part of a block. A short watch timer feeds the remaining bytes, closes the stream after all bytes have been accepted, and waits for the finish callback before reporting success. A transport ACK or a full buffer alone never means playback completed. Android includes transfer/playback elapsed time in Diagnostics.

Mute, busy dictation/speaker, invalid data, timeout, cancellation, and playback failure are reported. Back, starting dictation, and app shutdown stop active audio. Incomplete uploads expire after 30 seconds without a block; playback expires after 10 seconds; Android bounds the audio exchange to 90 seconds. Audio is never persisted or resumed after the app closes. The protocol is documented in [the wire contract](../protocol/README.md#speaker-test).

From v0.1.10, the app checks [`quiet_time_is_active()`](https://developer.repebble.com/docs/c/User_Interface/Preferences/) separately from `speaker_is_muted()`. Quiet Time rejects BEGIN, BLOCK, and PLAY, releases any buffered audio, and stops active playback. The existing 100 ms upload / 20 ms playback timer also checks Quiet Time so interruption does not depend on another phone message. Android reports a distinct Quiet Time error. Disabling Quiet Time does not resume the discarded clip; start a new test. This also applies when the user has disabled the system's optional speaker mute during Quiet Time.

Cycling detection, generated Hermes speech, longer clips, and background playback are outside this test.
