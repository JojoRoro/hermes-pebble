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

`ProtocolCheck.kt` covers chunk retry/reassembly and idempotency deadlines. `NetworkCheck.kt` covers short HTTP responses, headers, URL prefixes, replayed terminal runs, history metadata, and rejection paths. `watch_core_test.c` covers UTF-8 boundaries, JSON parsing, pending-record recovery, checksum failure, and exact persistence return values. `test_finalize_pbw.py` covers metadata-only PBW transformations and failure cleanup.

These checks do **not** compile the full Android app, run Room's processor, execute WorkManager, or validate Keystore/Compose behavior. The manual Android workflow, CloudPebble build, and real-device acceptance matrix remain in [validation.md](../docs/validation.md).
