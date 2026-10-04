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

The watch first buffers the entire clip in temporary heap memory, with a maximum of 16,000 bytes (two seconds at this format). From v0.1.17 Android sends 768-byte blocks in one AppMessage each and waits for each watch receipt. Earlier releases used 1,024-byte blocks split across multiple messages. The watch checks the length and whole-clip checksum before opening its speaker at volume 80/100. This avoids making playback depend on Bluetooth sustaining the sample rate. Upload time depends on the phone host and connection; this test measures delivery rather than real-time streaming.

Writes to the speaker may accept only part of a block. A short watch timer feeds the remaining bytes, closes the stream after all bytes have been accepted, and waits for the finish callback before reporting success. A transport ACK or a full buffer alone never means playback completed. Android includes transfer/playback elapsed time in Diagnostics.

Mute, busy dictation/speaker, invalid data, timeout, cancellation, and playback failure are reported. Back, starting dictation, and app shutdown stop active audio. Incomplete uploads expire after 30 seconds without a block; playback expires after its nominal duration plus 10 seconds; Android bounds the audio exchange to 90 seconds. This diagnostic clip is never persisted or resumed after the app closes. The protocol is documented in [the wire contract](../protocol/README.md#speaker-test).

From v0.1.10, the app checks [`quiet_time_is_active()`](https://developer.repebble.com/docs/c/User_Interface/Preferences/) separately from `speaker_is_muted()`. Quiet Time rejects BEGIN, BLOCK, and PLAY, releases any buffered audio, and stops active playback. The audio timer (1 s while receiving, 100 ms for pending receipts, 50 ms during playback) also checks Quiet Time so interruption does not depend on another phone message. Android reports a distinct Quiet Time error. Disabling Quiet Time does not resume the discarded clip; start a new test. This also applies when the user has disabled the system's optional speaker mute during Quiet Time.

Cycling detection, generated Hermes speech, longer clips, and background playback are outside this test.

## Opt-in spoken replies (v0.1.13)

The dictation review offers **Send + voice reply**, alongside the existing text-only send. Once that capture completes, the first result fetch carries the voice flag. Android returns text first, then synthesizes up to 400 characters using an installed offline Android TTS voice and converts PCM to the existing watch format. A longer answer includes a spoken reminder to read the rest. Speech synthesis is limited to 45 seconds of processing and 60 seconds of output; the whole delivery is limited to ten minutes. No Hermes speech endpoint or bike classifier is involved.

The conversion supports the 8-bit unsigned, 16-bit signed, and float PCM delivered by Android's [UtteranceProgressListener](https://developer.android.com/reference/android/speech/tts/UtteranceProgressListener). Mono/stereo output is averaged down to 8 kHz signed 8-bit PCM. A temporary synthesis file is deleted when synthesis finishes or is cancelled. The installed offline voice uses the configured TTS language; missing voice data is reported rather than silently switching to a network voice.

Through v0.1.16 playback reused the 16,000-byte receiver, with pauses while each section uploaded. Version 0.1.17 replaced this with full-reply buffering; v0.1.18 replaces that with streaming below. Every section carries the original request capture in ItemId. The watch rejects sections after Back, new dictation, or relaunch, even if synthesis was still running when the user cancelled. The phone deduplicates opted-in result fetches by watch, capture, and transfer ID and never starts speech for notes, unfinished requests, or normal text-only fetches. Voice errors do not change command state or remove the text answer. Diagnostics contains the full status/error; the answer footer shows a short status.

Run `tests/watch_conversation_smoke.py --emulator emery --voice-only` for the review option, first-page voice flag, capture-bound audio acceptance, cancellation between sections, and subsequent text-only send. `AudioCheck.kt` also checks PCM conversion, stereo downmixing, rate conversion, invalid audio, Unicode text bounds, and preservation of the originating capture on all clip operations. Physical Android TTS synthesis and audible playback still require a phone/watch acceptance test.

### Playback of existing replies (v0.1.14)

Open **Recent → an answer → Select → Play voice reply** to speak an existing completed reply, including replies from an older conversation. Each explicit selection creates a new playback request, so replay is supported. Ordinary opening, pagination, and **Refresh answer** remain silent. The action waits for any text page already in flight. Notes and unfinished replies do not offer playback. Existing offline voice requirements, 400-character limit, transfer pauses, and Back/mute/Quiet Time behavior apply.

`tests/watch_conversation_smoke.py --emulator emery --recent-voice-only` covers old-conversation playback, repeat selection, selection while another text page is loading, silent refresh, and Back. The Kotlin audio checks distinguish duplicate transport delivery from a fresh user request for the same capture.

### Up shortcut and arrival vibration (v0.1.15)

A fresh **Up** press at the true top of a completed answer invokes the same explicit playback action. This includes replies originally sent without voice and replies opened from Recent. Scrolling up, auto-repeating a held button, and loading an earlier long-answer window do not request speech. The action menu remains available too.

For the most recently submitted watch request, one short vibration now fires on receipt of completed status, without waiting for the answer page to load. If a completed result page arrives first, it can trigger the same notification. Leaving the status screen does not cancel the notification while Hermes stays open. Duplicate completion, refresh, and playback do not buzz again; Quiet Time consumes the notification silently.

The host watch tests cover shortcut eligibility, repeating presses, long-answer boundaries, duplicate completion, leaving the status screen, and Quiet Time. The voice emulator test sends text-only, presses Up to request speech, holds Up across result delivery to check against repeated playback, and checks Back cancellation.

### Playback speed and transfer fixes (v0.1.16)

Android now explicitly requests normal TTS rate and pitch (1.0), independent of the phone's speech preferences. Diagnostics records the source sample rate, channels, encoding, byte count, and converted duration to help investigate any engine-specific issue without logging spoken text. The user reported that the bundled test clip plays normally; both that clip and replies retain the same 8 kHz signed 8-bit watch playback path.

Audio packets now carry up to 768 bytes within the existing 1,024-byte inbox. Each full logical block takes two packet exchanges instead of six. This reduces transfer overhead but does not promise continuous streaming: sections still buffer before playback. The footer now says **Loading voice n/N** during upload and **Playing n/N** when the phone requests playback after the last receipt. Install both v0.1.16 artifacts.

The host tests check duration and pitch at common Android source rates, packet size/bounds, and the order of upload/playback progress. The emulator audio smoke also sends reordered/duplicate large chunks and checks that playback does not complete before the expected clip duration. Audible voice-reply intelligibility and Bluetooth transfer timing require physical acceptance.


### Historical full preload at 1.5× speed (v0.1.17)

Voice replies now load completely before playback, with a single percentage indicator, then play through one continuous speaker session. This trades the repeated loading gaps for one initial wait. Android asks the speech engine for 1.5× speed with normal pitch, shortening the spoken audio and the amount transferred. The PCM format is unchanged.

To fit a complete reply without exhausting RAM, the watch spools up to 60 seconds of audio in its app storage and reads it through a 1 KiB buffer. The firmware must provide enough app storage; the app reports a storage error if it cannot reserve the cache. Audio is erased after playback or cancellation in bounded batches. If the app closes before erasure finishes, remaining cache pages are removed next time it opens; cached audio is never automatically resumed. Saved notes and settings use separate keys.

Each audio block now fits a single 768-byte packet, and the watch replies immediately when its outbox is free. This removes the extra packet per full block and the artificial receipt delay. Physical loading time still depends on the phone/watch link.

The v0.1.17 revision of `tests/watch_audio_smoke.py --emulator emery --full-reply` tested a 24.15-second cached reply in one speaker session. That release's host checks covered a full minute, odd final lengths, partial speaker writes, duplicate blocks, storage errors, cancellation, Quiet Time, restart cleanup, and upload/playback timeout bounds. Current streaming checks are described in `tests/README.md`. The emulator uses a dummy audio backend; audible quality and loading speed still need the physical phone/watch.

### Buffered streaming (v0.1.18)

The full-reply flash cache in v0.1.17 caused long startup waits and user-reported watch slowdown. Replies now use a fixed 24,576-byte RAM ring and no audio flash writes. After roughly 1.7 seconds of audio is buffered, playback starts and the phone continues sending ahead while the watch plays. This describes audio duration, not a promised wall-clock loading time. Android TTS remains at 1.5× with normal pitch.

Independent IMA ADPCM packets carry up to 1,529 samples in 768 bytes, approximately halving audio payload traffic compared with the previous PCM transfer. This is lossy speech compression; decoded playback still uses the same 8 kHz speaker format. The implementation follows the [IMA reference algorithm](https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf). The watch immediately reports a full ring and the phone retries that block after 100 ms, so unplayed audio cannot be overwritten. A fast sender spaces new blocks at least 75 ms apart to leave time for watch events. If the connection falls behind, playback waits for another useful buffer instead of starting each tiny packet separately. Slow links can still cause pauses.

Back, Quiet Time and shutdown stop playback and release the ring. Leftover v0.1.17 flash pages are only removed while audio is inactive, one page per 250 ms event, and never replayed. Install both v0.1.18 APK and PBW; the retired full-cache format is rejected. The diagnostic test sound remains unchanged.
