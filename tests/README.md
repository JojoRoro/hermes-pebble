# Offline regression checks

Run configuration and Python finalizer checks from the repository root:

```sh
python3 tools/check_local.py
```

The optional checks use host GCC/Java and explicitly supplied SDK headers and Maven libraries. They build temporary **host test programs**, not an APK or PBW; they do not install a toolchain, call Hermes, dispatch CI, or contact a watch. The watch harness uses actual production functions and emulates only persistent storage. The HTTP harness uses OkHttp application interceptors, so requests never reach a socket.

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

It exercises the startup reply, a correlated phone probe, five rapid Down presses to Reconnect, and two rapid Back presses while awaiting a reply. It fails on missing handshakes or logged app faults. Inspect the screenshots in `build/watch-smoke`: linked screens should say LINKED, the fifth Down should select Reconnect, and the final screenshot should show the system home screen. This does not exercise Android's Bluetooth/host binding.

The emulator smoke also sends a multiline diagnostic and compares the visible text before/after Up and Down, checks both scroll boundaries, toggles the watch touch-navigation setting, relaunches the app to check persistence, and re-enables touch using buttons. Use SDK 4.33.1 firmware (`pebble kill` before switching from an older running emulator). Touch gesture delivery itself still requires a touch-capable watch with system touch navigation enabled.

## Automatic reply and conversation smoke

After `pebble build`, run `tests/watch_conversation_smoke.py --emulator emery` in the same SDK Python environment as the navigation smoke. Optional `--pbw` and `--output` arguments work the same way. Do not run both emulator scripts concurrently.

This test supplies a local dictation fixture and acts as the phone peer. It dictates/reviews/sends, sends progress and completion, and requires the watch to request and display the result without another button press. It checks duplicate completion does not replace the answer; Reply creates a new capture in the same conversation generation; late status does not replace a new draft; completion arriving before the saved receipt still fetches the result; and Back remains effective. Screenshots are written to `build/conversation-smoke` for visual inspection. It makes no live Hermes, speech-service, or Matrix requests.

The CloudPebble operator patch includes separate manifest round-trip tests; apply and run them as described in [CloudPebble instructions](../docs/cloudpebble.md).
