# Validation status

The initial implementation review and CI build passed on 26 September 2026. The first installed version subsequently failed on the user's phone/watch: watch messages timed out and API requests reported invalid responses. Live compatibility is therefore unresolved, rather than assumed from the offline checks.

## 0.1.3 permanent signing validation (27 September 2026)

- Local `:app:assembleRelease` passed, including release lint and signing, with version 0.1.3 / code 4. `apksigner verify` passed; the APK certificate matches the pinned public SHA-256 fingerprint and the manifest is not debuggable.
- Actionlint 1.7.12 and the configuration/finalizer checks passed. The matching emery PBW builds and finalizes successfully; watch behavior is unchanged from 0.1.2.
- The new workflow restores a permanent key from repository Actions secrets, fails if signing material is missing, checks the APK fingerprint, and removes the temporary key before upload. The permanent key and password were provisioned as repository Actions secrets with explicit user approval.
- The earlier CI debug private keys were not retained. Migration requires one uninstall/reinstall, which clears local app data. Subsequent releases must retain this key and increase the version code.

## 0.1.2 watch link and navigation verification (27 September 2026)

- Reproduced an emulator app fault on the first phone handshake reply. Removed a multi-kilobyte stack copy from inbound reassembly and moved large scratch buffers off the stack. The build now rejects individual stack frames larger than 768 bytes.
- Repeated the startup handshake and a correlated phone-initiated probe successfully in the emery emulator. Visually checked LINKED, selection of the previously unreachable Reconnect row, and return to the system home screen after two rapid Back presses while a reply was pending.
- Host regression checks cover Back from every screen, preservation of saved drafts, menu row counts, probe correlation, and preserving in-flight transfers. The full offline suite passes.
- The SDK 4.9.169 emery build succeeds with a 42,328-byte RAM footprint and 4,092 bytes of resources. Android `:app:assembleDebug` passes for version 0.1.2 / code 3.
- Android diagnostics now start the watch app without requiring prior listener callbacks, bound host operations with timeouts, reset stale bindings, and write safe event metadata to logcat under `HermesLink`.
- Emulator transport uses a simulated phone peer; physical Pebble host routing and live NetBird/Hermes requests remain unverified.

## 0.1.1 diagnostics and watch UI verification (27 September 2026)

- The complete offline suite passes, including new capability fixtures without idempotency metadata, rejection of malformed metadata, optional legacy replay flags, precise HTML/schema failures, and exclusion of fixture credentials and bodies from diagnostic events.
- The PBW compiles and links with the locally installed Pebble SDK 4.9.169 for emery (37,014 bytes RAM footprint, 4,092 bytes resources). The separate host C check also passes against SDK 4.33.1 headers.
- The home and scrolling menu screens were installed, captured, and visually inspected in the emery emulator. The finalized `build/hermes-pebble-0.1.1.pbw` passed companion metadata and member-preservation checks.
- Android `:app:assembleDebug` passed locally with JDK 21 and SDK 37.0, including Room/KSP and Kotlin/Compose compilation. The constrained local environment required one worker and a 768 MiB Gradle heap. The APK reports version 0.1.1 / code 2. It is a locally signed debug APK; its signing key can differ from the original CI APK.
- Physical watch routing, Android credential-focus behavior, and authenticated requests through the user's NetBird proxy still require device verification. No live Hermes or Matrix requests were sent.


## Completed local checks

- Five Python finalizer tests passed: metadata restoration and repeat finalization, nonmetadata byte preservation, wrong UUID rejection, duplicate ZIP rejection, existing-output protection, and cleanup after verification failure.
- Host GCC syntax check passed against the official Pebble SDK 4.33.1 emery headers. This initial check did not build a PBW; the 0.1.1 build is recorded above. The host check substitutes only the SDK's libc time declarations and empty generated resource IDs.
- Native host regression checks passed for exact persistent-storage return counts, negative errors, maximum-size pending transcript round trips, checksum corruption, generation zero, recent/result JSON parsing, surrogate pairs, and six-chunk UTF-8 text.
- Isolated Kotlin 2.4.20 compilation and execution passed for the production wire protocol and retry policy, using PebbleKit dictionary models. Checks include duplicate final chunks, changed chunk counts, timeout recovery, invalid UTF-8, and idempotency deadlines.
- Isolated HTTP client compilation and six offline interceptor checks passed for bounded response reads, both authentication headers, base paths, terminal replays, numeric history metadata/tool events, HTML rejection, redirect classification, and oversized bodies. No Hermes request was made.
- Authentic Gradle wrapper JAR SHA-256 verified: `55243ef57851f12b070ad14f7f5bb8302daceeebc5bce5ece5fa6edb23e1145c`. Both wrapper scripts and checksum-pinned distribution properties are present. Gradle was subsequently executed in CI as described below.

See [test instructions](../tests/README.md) for repeatable local commands. WorkManager scheduling, Keystore behavior, and real transport still require device checks.

## Completed Android build

[Workflow run 36271723438](https://github.com/JojoRoro/hermes-pebble/actions/runs/36271723438) passed on 26 September 2026 at commit `413901cbf6c827dd0b402f2559a8e79efa5fbf0e`. The Gradle wrapper ran `:app:assembleDebug`, including Room/KSP generation and Kotlin/Compose compilation, and uploaded `hermes-pt2-android-debug` containing `app-debug.apk`.

The build fixes select the published `platforms;android-37.0` package and explicit compile SDK minor level 0, give Gradle a 2 GiB heap and 768 MiB metaspace, and convert the credential envelope version to a byte before storage. The APK has not been installed or exercised on a device.

The release workflow passed actionlint 1.7.12. Pushing `v0.1.0` then triggered [workflow run 36272251691](https://github.com/JojoRoro/hermes-pebble/actions/runs/36272251691), which passed both the Android build and release publishing jobs. The [published v0.1.0 release](https://github.com/JojoRoro/hermes-pebble/releases/tag/v0.1.0) contains `hermes-pebble-debug.apk` as a downloadable asset and was verified as the latest release on 26 September 2026.

## Acceptance checks

| Check | Expected result | Status |
| --- | --- | --- |
| Manual Android workflow | Produces a sideloadable APK; no Pebble job runs | Passed: APK built and uploaded; device installation deferred |
| Version tag release | A pushed v* tag builds and attaches the APK to its GitHub Release | Passed: v0.1.0 built and APK attached to the published release |
| CloudPebble root import/build | Produces a PT2 PBW independently of Android; SDK version is recorded | Deferred, not run |
| PBW metadata finalizer | Companion declaration is restored; UUID is checked; all other member bytes are unchanged | Passed on locally built 0.1.1 PBW |
| Companion registration | Stock Pebble app routes watch messages to the installed companion | Deferred, not run |
| Both authentication headers | Correct pair succeeds; either wrong credential fails clearly | Deferred, not run |
| Prefix URL and redirect handling | Prefix remains intact; no credentials follow a redirect | Deferred, not run |
| Quick Launch and dictation | Capture, review, send, re-dictate, and cancel work on PT2 | Deferred, not run |
| Normal Hermes request | Real response appears on phone and active watch | Deferred, not run |
| Follow-up | Remembers the same session; New conversation separates context | Deferred, not run |
| Authorized Matrix request | Hermes uses the intended account/tool; resulting message is checked in Matrix | Deferred, not run |
| Duplicate/retried watch message | Creates one durable command | Deferred, not run |
| Lost HTTP create response | Original key and payload reconcile without a second run | Deferred, not run |
| Process death/restart | Accepted run is monitored, not resubmitted | Deferred, not run |
| Network/NetBird outage | Completed text remains queued; recovery is explicit | Deferred, not run |
| Interrupted/unknown outcome | No automatic repeat of a potentially executed action | Deferred, not run |
| Watch app closed during completion | Result is available when reopened | Deferred, not run |
| Long Unicode text | Correct chunking and pagination without corruption or silent truncation | Deferred, not run |
| Server approval required | Clear blocked state; no automatic approval | Deferred, not run |
| Local note | Durable phone save; no unexpected Hermes request | Deferred, not run |
| Secret handling | No credentials in repository, APK resources, logs, exports, or watch traffic | Deferred, not run |
| Android restrictions | Real-device Doze/restart behavior is documented without an instant-delivery claim | Deferred, not run |

Only the later Matrix check may use an explicitly chosen test recipient. No message is sent as part of implementing or documenting this project.

## PBW finalizer checks

Use synthetic archives and copied fixtures for these checks before using a real downloaded PBW. The completed subset is listed above; remaining edge cases are still deferred.

| Check | Expected result | Status |
| --- | --- | --- |
| Missing or invalid package UUID | Command fails before creating output | Deferred, not run |
| Missing or empty Android companion apps | Command fails safely | Deferred, not run |
| Non-object companion app entry or duplicate package | Command fails safely | Deferred, not run |
| Wrong appinfo UUID | Command fails without an output | Passed with synthetic fixtures |
| Duplicate ZIP member name | Command fails without an output | Passed with synthetic fixtures |
| Uncompressed input over 32 MiB | Command rejects before member reads | Deferred, not run |
| Existing output or in-place output | Command refuses to overwrite | Deferred, not run |
| Authentic Gradle wrapper | `gradlew`, `gradlew.bat`, wrapper JAR, and properties match the Gradle 9.4.1 checksums before workflow dispatch | JAR verified; checksum-pinned distribution executed successfully in CI |
| Metadata already correct | Separate output is still created and verified; report says `metadata already present` | Passed with synthetic fixtures |
| Metadata changed | Top-level `companionApp` exactly matches package metadata; no nested `pebble` object is added | Passed with synthetic fixtures |
| Nonmetadata member preservation | SHA-256 of every other uncompressed member matches the input | Passed with synthetic fixtures |
| Corrupt or failed verification | Incomplete output is removed | Passed with synthetic fixtures |
| Secret-safe diagnostics | Package, UUID, and output diagnostics contain no credential values | Deferred, not run |

The pinned wrapper values are JAR SHA-256 `55243ef57851f12b070ad14f7f5bb8302daceeebc5bce5ece5fa6edb23e1145c` and Gradle 9.4.1 distribution SHA-256 `2ab2958f2a1e51120c326cad6f385153bb11ee93b3c216c5fccebfdfbb7ec6cb`.

## Source and packaging checks

Source review removed the abandoned duplicate UI/runtime and corrected unresolved internal APIs, suspend calls, model mappings, and metadata. No Room schema was fabricated. The Android workflow supports manual builds and version tag releases, and the watch build remains owned by CloudPebble. The Android workflow passed; no emulator, device install, live Hermes call, or Matrix send was run.

## Runtime and security checks

- Confirm credentials are entered only on the phone, encrypted at rest, excluded from backups/exports, absent from logs and exception messages, and never sent to the watch.
- Confirm a wrong Hermes credential and a wrong NetBird header fail distinctly enough for diagnosis without claiming which layer rejected a 401 or 403.
- Confirm a base path remains intact and an authenticated redirect is rejected without forwarding either credential.
- Confirm a lost create response is retried only with the original persisted key and payload when durable capability and retention allow it.
- Confirm GrapheneOS network permission, NetBird connectivity, Doze, app standby, process death, and force-stop behavior on the actual phone.
- Confirm no external Matrix delivery claim is derived solely from HTTP acceptance, a run completion, a Pebble transport ACK, or a notification.

Local checks and the emery PBW build passed. Hardware acceptance and live NetBird/Hermes compatibility remain unresolved; the complete stack is not yet validated.
