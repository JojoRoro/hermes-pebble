# Offline regression checks

Run configuration and Python finalizer checks from the repository root:

```sh
python3 tools/check_local.py
```

The optional checks use host GCC/Java and explicitly supplied SDK headers and Maven libraries. They build temporary **host test programs**, not an APK or PBW; they do not install a toolchain, call Hermes, dispatch CI, or contact a watch. The watch harness uses actual production functions and stubs persistent storage and drawing timers. The HTTP harness uses OkHttp application interceptors, so requests never reach a socket.

```sh
python3 tools/check_local.py \
  --sdk-headers /path/to/sdk-core/pebble/emery/include \
  --kotlin-libs /path/to/kotlin-check-jars \
  --pebble-model /path/to/PebbleDictionaryItem.kt
```

Use SDK 4.33.1 headers and the `PebbleDictionaryItem.kt` file from PebbleKit Android 2's `common-api/src/main/kotlin/io/rebble/pebblekit2/common/model/`. The review used the repository's existing upstream source snapshot. The helper creates temporary resource/message headers and a host libc time shim; it leaves the SDK untouched.

The Kotlin library directory must contain these Maven artifacts (no repository secrets or Gradle execution needed):

| Maven group | Artifacts | Version |
| --- | --- | --- |
| org.jetbrains.kotlin | kotlin-compiler-embeddable, kotlin-build-tools-api, kotlin-stdlib, kotlin-script-runtime, kotlin-daemon-embeddable, kotlin-serialization-compiler-plugin-embeddable | 2.4.20 |
| org.jetbrains.kotlin | kotlin-reflect | 1.6.10 (compiler dependency) |
| org.jetbrains | annotations | 13.0 |
| org.jetbrains.kotlinx | kotlinx-coroutines-core-jvm | 1.10.2 |
| org.jetbrains.kotlinx | kotlinx-serialization-core-jvm, kotlinx-serialization-json-jvm | 1.9.0 |
| com.squareup.okhttp3 | okhttp-jvm | 5.3.0 |
| com.squareup.okio | okio-jvm | 3.16.2 |

`ProtocolCheck.kt` covers chunk retry/reassembly and idempotency deadlines. `NetworkCheck.kt` covers short HTTP responses, headers, URL prefixes, replayed terminal runs, history metadata, optional legacy idempotency/replay metadata, precise HTML/schema diagnostics, secret exclusion from diagnostic events, and rejection paths. `watch_core_test.c` covers UTF-8 boundaries, JSON parsing, pending-record recovery, checksum failure, and exact persistence return values. `test_finalize_pbw.py` covers metadata-only PBW transformations and failure cleanup.

These checks do **not** compile the full Android app, run Room's processor, execute WorkManager, or validate Keystore/Compose behavior. Build results and the real-device acceptance matrix are recorded in [validation.md](../docs/validation.md).

## Emulator smoke

With the Pebble SDK installed, build using `pebble build`. Run `tests/watch_emulator_smoke.py --emulator emery` using the Python environment that contains `pebble-tool`, `libpebble2`, and Pillow; put the SDK toolchain directory on PATH so QEMU can start. The script installs `build/hermes-pebble.pbw` into the emulator and acts as a phone peer. Optional `--pbw` and `--output` arguments select the bundle and screenshot directory.

It exercises the startup reply, a correlated phone probe, six rapid Down presses to Settings, Reconnect inside Settings, and two rapid Back presses while awaiting a reply. It fails on missing handshakes or logged app faults. Inspect the screenshots in `build/watch-smoke`: linked screens show a green header dot (offline screens say offline), the sixth Down should select Settings, and the final screenshot should show the system home screen. This does not exercise Android's Bluetooth/host binding.

The emulator smoke also sends a multiline diagnostic and compares the visible text before/after Up and Down, checks both scroll boundaries, toggles the watch touch-navigation setting, relaunches the app to check persistence, and re-enables touch using buttons. Settings comparisons cover only the touch row because the Reconnect row shows the live link state. Use SDK 4.33.1 firmware (`pebble kill` before switching from an older running emulator). Touch gesture delivery itself still requires a touch-capable watch with system touch navigation enabled.

## Automatic reply and conversation smoke

After `pebble build`, run `tests/watch_conversation_smoke.py --emulator emery` in the same SDK Python environment as the navigation smoke. Optional `--pbw` and `--output` arguments work the same way. Do not run both emulator scripts concurrently.

This test supplies a local dictation fixture and acts as the phone peer. It dictates/reviews/sends, sends progress and completion, and requires the watch to request and display the result without another button press. A multiline Unicode answer larger than the old text buffers checks that only the first chunk loads until the reader pages toward the end, joining of later chunks, stable scroll position while text arrives, and preservation of an open Actions menu. It also checks duplicate completion does not replace the answer; Reply creates a new capture in the same conversation generation; late status does not replace a new draft; completion arriving before the saved receipt still fetches the result; and Back remains effective. Screenshots are written to `build/conversation-smoke` for visual inspection. It makes no live Hermes, speech-service, or Matrix requests.

Restart with `pebble kill`, then run the same script with `--long-scroll-only` to open a saved answer over 8 KiB and check forward and backward text-window loading with Down/Up. The fixture pages Down until the watch asks for more text, and pages Up to the top of the final window to reload the preceding one. This check asserts every requested UTF-8 byte offset. The SDK emulator's screenshot service timed out during long transfers, so large-window validation uses protocol assertions; the regular conversation smoke checks the displayed text and reading position with screenshots. Physical long-answer reading remains a device acceptance check.

The CloudPebble operator patch includes separate manifest round-trip tests; apply and run them as described in [CloudPebble instructions](../docs/cloudpebble.md).

## Handwriting checks

The full optional local check also runs `watch_ink_test.c` and `InkCheck.kt`. These exercise production touch/stroke handling, simplification, multi-stroke letters, undo, spaces, capacity boundaries, 4 KiB coexistence with a maximum text capture, interrupted saves, restart recovery, and corrupt saved data. A file emitted by the C implementation is decoded by Kotlin. Kotlin checks binary AppMessage handling, Data Logging envelopes, full-size reassembly, reordered/duplicate blocks, conflicting bytes, bad offsets, and checksum rejection. `test_ink_migration.py` uses SQLite and the Room-generated schema to check the migration and preservation of existing tables/data.

The navigation emulator smoke opens **Handwritten note**, captures the canvas, verifies an empty drawing cannot be saved, and returns to the menu. Raw touch samples run in the C host harness; finger sampling, Android rendering/notification interaction, and Bluetooth background Data Logging delivery require physical-device acceptance.

## Reply context checks

`ReplyContextCheck.kt` exercises production capability/version discovery, request serialization, the HTTP client, context selection/bounds, and frozen retry payloads against an interceptor modeling the verified Hermes 0.19.0 history parser. It reproduces a same-session follow-up without context, then checks that explicit prior messages restore the first exchange. It covers unknown/newer versions, unavailable health, old capability caches, UTF-8 shortening, pair order, and byte-identical retries. `test_reply_history.py` executes the production Room DAO query in SQLite against the generated schema to check profile/generation isolation and exclusion of notes, unfinished/cleared/future requests.

The implementation was also checked against the official `hermes_agent-0.19.0-py3-none-any.whl` (SHA-256 `bd0bac012aee38a60894781f4597dc29ee7bedb3448540249921f10d3bef327f`). Executing its extracted request-history parser confirmed that session ID alone yields empty history while explicit message pairs are retained. No live model or user server request is part of these checks.

## Speaker test checks

The optional local suite runs `watch_audio_test.c` against the production receiver/pump and `AudioCheck.kt` against the production phone transfer. These check arbitrary binary PCM, maximum-size clips, partial/blocked speaker writes, corruption and bounds rejection, duplicate blocks/play, muted/busy/open failure, cancellation, expiry, correlated receipts, and waiting for the finish callback rather than a transport ACK.

After building the PBW, `tests/watch_audio_smoke.py --emulator emery` uses the same SDK Python environment as the other smoke scripts. It uploads the bundled speech clip, rejects an incomplete play, validates every block receipt, and requires playback completion. On a Linux build host without working audio, stop the existing emulator with `pebble kill`, then run the test with `SDL_AUDIODRIVER=dummy`. Without a draining audio backend, the emulator fills its 8 KiB speaker buffer and playback times out. The dummy backend exercises completion without producing audible sound. Physical audibility and Android Bluetooth/host behavior require the real phone and watch.

Quiet Time cases run with speaker mute explicitly false: reject BEGIN, discard on BLOCK/PLAY, discard while waiting for blocks, stop active playback, and reject a replay after Quiet Time turns off. Kotlin checks that the new status displays an actionable message and sends no audio blocks after rejection.

## Bike detection checks

The default local check uses host GCC for `watch_bike_test.c`, which tests the production integer scoring model against deterministic synthetic accelerometer signals and HR freshness/bounds. The SDK-header check also runs `watch_bike_service_test.c` for subscription cleanup, sampling restoration, denied/unavailable HR, fresh-event gating, sensor timeouts, and the 10-minute auto-pause/resume. These establish algorithm behavior, not real-world classification accuracy.

Run `tests/watch_bike_smoke.py --emulator emery` with the same SDK Python environment as the other smoke tests. It opens Settings › Bike detection test, checks stationary and continuously injected rapid-motion scores, resets, and returns/reopens the screen. Screenshots are written under `build/bike-smoke`. Do not run emulator scripts concurrently. Road testing is still needed to tune the heuristic.
