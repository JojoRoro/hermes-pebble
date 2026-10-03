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

## Opt-in spoken replies (v0.1.13)

The dictation review offers **Send + voice reply**, alongside the existing text-only send. Once that capture completes, the first result fetch carries the voice flag. Android returns text first, then synthesizes up to 400 characters using an installed offline Android TTS voice and converts PCM to the existing watch format. A longer answer includes a spoken reminder to read the rest. Speech synthesis is limited to 45 seconds of processing and 60 seconds of output; the whole delivery is limited to ten minutes. No Hermes speech endpoint or bike classifier is involved.

The conversion supports the 8-bit unsigned, 16-bit signed, and float PCM delivered by Android's [UtteranceProgressListener](https://developer.android.com/reference/android/speech/tts/UtteranceProgressListener). Mono/stereo output is averaged down to 8 kHz signed 8-bit PCM. A temporary synthesis file is deleted when synthesis finishes or is cancelled. The installed offline voice uses the configured TTS language; missing voice data is reported rather than silently switching to a network voice.

Playback reuses the 16,000-byte receiver, so there are pauses while each section uploads. Every section carries the original request capture in ItemId. The watch rejects sections after Back, new dictation, or relaunch, even if synthesis was still running when the user cancelled. The phone deduplicates opted-in result fetches by watch, capture, and transfer ID and never starts speech for notes, unfinished requests, or normal text-only fetches. Voice errors do not change command state or remove the text answer. Diagnostics contains the full status/error; the answer footer shows a short status.

Run `tests/watch_conversation_smoke.py --emulator emery --voice-only` for the review option, first-page voice flag, capture-bound audio acceptance, cancellation between sections, and subsequent text-only send. `AudioCheck.kt` also checks PCM conversion, stereo downmixing, rate conversion, invalid audio, Unicode text bounds, and preservation of the originating capture on all clip operations. Physical Android TTS synthesis and audible playback still require a phone/watch acceptance test.

### Playback of existing replies (v0.1.14)

Open **Recent → an answer → Select → Play voice reply** to speak an existing completed reply, including replies from an older conversation. Each explicit selection creates a new playback request, so replay is supported. Ordinary opening, pagination, and **Refresh answer** remain silent. The action waits for any text page already in flight. Notes and unfinished replies do not offer playback. Existing offline voice requirements, 400-character limit, transfer pauses, and Back/mute/Quiet Time behavior apply.

`tests/watch_conversation_smoke.py --emulator emery --recent-voice-only` covers old-conversation playback, repeat selection, selection while another text page is loading, silent refresh, and Back. The Kotlin audio checks distinguish duplicate transport delivery from a fresh user request for the same capture.

### Up shortcut and arrival vibration (v0.1.15)

A fresh **Up** press at the true top of a completed answer invokes the same explicit playback action. This includes replies originally sent without voice and replies opened from Recent. Scrolling up, auto-repeating a held button, and loading an earlier long-answer window do not request speech. The action menu remains available too.

For the most recently submitted watch request, one short vibration now fires on receipt of completed status, without waiting for the answer page to load. If a completed result page arrives first, it can trigger the same notification. Leaving the status screen does not cancel the notification while Hermes stays open. Duplicate completion, refresh, and playback do not buzz again; Quiet Time consumes the notification silently.

The host watch tests cover shortcut eligibility, repeating presses, long-answer boundaries, duplicate completion, leaving the status screen, and Quiet Time. The voice emulator test sends text-only, presses Up to request speech, holds Up across result delivery to check against repeated playback, and checks Back cancellation.
