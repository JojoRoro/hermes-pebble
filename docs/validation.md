# Validation status

The user confirmed on 28 September that installing the finalized PBW resolved watch routing and that correcting their API key resolved Hermes authentication. They also confirmed manual Fetch retrieves an answer. Those reports establish the existing transport path; the new automatic answer flow below is validated in the emulator and still needs physical-device acceptance.

## 0.1.12 bike comparison and typing rejection (2 October 2026)

- Bike Test shows NEW and OLD side by side from the same 50 Hz sensor stream. The legacy model and HR bonus are unchanged. The revised model ignores HR, requires continuous half-second vibration blocks and three strong four-second windows, reduces large wrist swings/impacts, and uses recent cumulative steps as a negative cue. It remains a heuristic, not a calibrated probability or an automatic feature trigger.
- Up learns five windows of typing features locally; Select retains a completed baseline while resetting evidence, Down shows feature details, and holding Up clears the baseline. The baseline is discarded when leaving the test. Both models clear on invalid timing, haptics, missing samples, or repeated-timestamp callbacks that fail to advance evidence. Covering/leaving/auto-pause releases sensors and restores automatic HR sampling.
- All offline Python and native production checks pass, including new adversarial motion and sensor lifecycle cases. Synthetic typing is 0% new / 55% old; intermittent bursts are 0% / 51%; continuous mixed-frequency vibration reaches 92% new without HR. These are illustrative test signals, not labeled real keyboard or bike recordings. AddressSanitizer/UndefinedBehaviorSanitizer pass with leak detection disabled for the sandbox tracing environment.
- SDK 4.33.1 builds emery with a 64,293-byte RAM footprint and 66,779 bytes free before runtime allocations. Host syntax checks are warning-free, and final PBW companion metadata verification passes. Watch and Android version names are 0.1.12, Android code 13. Android implementation is unchanged; signed APK packaging and certificate verification run in release CI.
- The emulator comparison/sensor test passes for stationary and injected vibration, both visible scores, details, learning, reset, and reentry; screenshots were inspected. The prior half-second subscription exposed alternating backward/forward emulator timestamps (-980/+1020 ms between batches), preventing any valid window. One-second (50-sample) batches retain the same sampling rate and scoring windows while halving callbacks and producing valid timing. Actual keyboard false positives, easy rides, vehicle confounding, and battery impact still need physical-device comparison.

## 0.1.11 watch UI redesign and battery work (2 October 2026)

- The watch UI has a slim accent header with a link dot, icon menus with subtitles, a status card with a colored state pill, an answer view that quotes the request, centered notices for connecting/listening/errors, and a right-edge tab marking screens where Select opens actions. Reconnect, touch navigation, and the bike test moved into Settings. Recent rows show kind and state. Text pages a screen per press. The previous answer is cleared from Status after New conversation, and result pages that omit `input` no longer erase the request on the watch.
- Battery: answers load lazily within two screens of the loaded end instead of downloading the whole 8 KiB window up front; each chunk extends the answer in place with one text measurement; off-screen blocks are not laid out during redraw. The transfer timer now fires at the next real deadline instead of every second. Handwriting sync retries back off from 30 seconds to 5 minutes. Speaker feed ticks every 50 ms instead of 20 ms, and an idle upload ticks once a second. The bike test redraws only on change and pauses itself after 10 minutes, releasing sensors and its timer.
- The full offline Python and native checks pass, including a new bike auto-pause/resume service case. The host `-Wall -Wextra` syntax check reports no warnings. SDK 4.33.1 builds emery with a 61,761-byte RAM footprint and 69,311 bytes free.
- Emulator checks passed and screenshots were inspected: navigation (Settings › Reconnect, scroll bounds, touch-setting persistence, handwriting canvas, Back); conversation (no full download on the first screen, chunks requested while paging, stable reading position, Actions preservation, Reply generation, completion before receipt); the long-answer forward/backward windows; and audio delivery through the speaker finish callback with `SDL_AUDIODRIVER=dummy`. The bike smoke reached the redesigned screen, but synthetic motion never completed a window in this emulator. The unmodified 0.1.10 build fails identically, including after `pebble wipe`, so motion-score behavior relies on the native tests here.
- Watch/Android version name is 0.1.11, Android code 12. Physical battery impact, buzz on arrival, and touch behavior on the new layouts remain device acceptance checks.

## 0.1.10 Quiet Time and bike detection test (2 October 2026)

- Quiet Time is checked independently from speaker mute before audio BEGIN/BLOCK/PLAY and during buffering/playback. Native production-code checks pass for rejection with mute disabled, buffer disposal, interruption of playback, and no replay after Quiet Time is disabled. The Kotlin transfer test passes for the distinct Quiet Time message and rejection before sending audio blocks.
- **Test bike detection** adds a watch screen with an experimental percentage, motion summary, and HR observation age. It uses 50 Hz acceleration, four-second windows, sustained rapid-change evidence, temporal smoothing, and a small age-weighted HR boost. It requests 15-second HR samples while visible and cancels that request on exit. No cycling/HR data is stored or sent to Android or Hermes, and the score does not trigger audio.
- Synthetic production-model tests pass for stationary noise, walking-like oscillation, sustained rapid vibration, rotation, isolated shocks, haptic exclusion, stopping, stale/invalid HR, repeated timestamps, timing gaps, wraparound, and extreme inputs. A sanitizer run passed with leak detection disabled because the sandbox cannot run LeakSanitizer under its tracing environment. Service tests pass for subscription/timer cleanup, HR sampling restoration, denied/unavailable health data, fresh-event gating, and missing motion. These tests do not establish outdoor classification accuracy.
- The existing Python, C storage/parser/ink/audio, and Kotlin protocol/network/reply-context/ink suites pass. The answer's existing 8 KiB buffer is now allocated at app startup and released at shutdown to keep the growing watch binary under the SDK's separate static-image limit; its capacity and behavior are unchanged.
- The emery sensor/UI smoke passed for the ninth menu entry, four-second warm-up, stationary score (0%), sustained injected rapid vibration (76% at the captured point), reset, Back, and reopening. Screenshots were inspected. A fresh emulator restart resolved stale sensor state left by repeated installs; continuous sample injection was required because the emulator consumes each sample queue only once. This is synthetic behavior validation, not a measured cycling probability.
- The final SDK 4.33.1 emery build has a 58,432-byte static RAM footprint and 72,640 bytes of heap space before runtime allocations, including the 8,193-byte answer buffer. PBW companion metadata verification passed. The conversation emulator regression passed for joined Unicode answers, stable scrolling, Actions, follow-up generation, completion/receipt ordering, and Back; the emulator reported no remaining app allocations on exit.
- Normal audio delivery with Quiet Time off passed through the speaker finish callback. As in 0.1.9, a stalled host audio backend required a fresh emulator with `SDL_AUDIODRIVER=dummy`. Quiet Time transitions are exercised in the native receiver/pump tests with system speaker mute disabled.
- Watch/Android version name is 0.1.10, Android code 11. Full signed APK packaging and signature verification run in the tag-triggered release workflow. Physical Quiet Time transitions and classification on actual rides remain device acceptance checks.

## 0.1.9 watch speaker test (1 October 2026)

- Android Diagnostics adds **Play test sound on watch**, with a bundled 1.725-second spoken clip and visible transfer/playback progress. It probes an already-open watch app and makes no Hermes or speech API request.
- SDK 4.33.1 builds the emery PBW with a 63,550-byte RAM footprint and 67,522 bytes available for heap allocations. The audio buffer is allocated only during a test, bounded at 16,000 bytes, and freed on completion, cancellation, expiry, or shutdown. This also respects the SDK's separate 65,535-byte static app-image limit.
- Python configuration/finalizer/SQLite and native production-code checks passed. The audio harness covers full-size binary clips, partial and blocked writes, corrupt/incomplete data, wrong sessions, invalid offsets, duplicates, mute/busy/open failure, preemption, expiry, shutdown, and delayed completion receipts.
- The full Kotlin suite passed: audio binary chunking and 16,000-byte delivery, watch/session/correlation matching, waiting for actual completion, timeout cancellation, and existing protocol, HTTP, reply-context, and ink interoperability checks.
- The emery audio smoke transferred all 13,800 bundled PCM bytes, validated every receipt, rejected incomplete playback, observed the natural speaker finish callback, and acknowledged a duplicate play without replaying. The host's default audio output stalled after 8,192 bytes; restarting the emulator with `SDL_AUDIODRIVER=dummy` allowed the speaker to drain and the entire test to pass. This validates playback control and delivery, not audible sound.
- The conversation emulator regression passed for automatic joined answers, stable scrolling, Actions preservation, Reply generation, completion before receipt, and Back. Its first run encountered a screenshot-service timeout; restarting the emulator resolved it. Android Room/KSP and Kotlin/Compose compilation passed locally; the temporary build environment reset before full APK/lint results could be collected, so complete APK packaging is verified in CI.
- The user confirmed on 2 October that the bundled clip plays on their physical watch through the Android button. Muted-watch diagnostics and closing the real watch app during a test remain hardware acceptance checks. No cycling detection or spoken Hermes replies are enabled by this diagnostic.
- Watch and Android version name are 0.1.9; Android version code is 10. Final signed APK packaging and certificate verification run in the tag-triggered release workflow.

## 0.1.8 continuous answer scrolling (30 September 2026)

- The watch joins UTF-8 result chunks automatically in an 8-KiB text window and keeps its reading position during incoming chunks and Actions navigation. Down/Up load adjacent windows without a next-page action; the drawing height is bounded for newline-heavy text.
- Python configuration/finalizer/SQLite checks and native production C checks passed. New cases cover decoded UTF-8 offsets, joining text, invalid/empty chunks, exact buffer boundaries, and stopping at a previous window's end. SDK 4.33.1 builds emery with a 61,581-byte RAM footprint and 69,491 bytes free; PBW metadata verification passed.
- The conversation emulator checks passed with screenshot comparisons for automatic multi-chunk Unicode answers, stable scrolling, an open Actions menu, duplicate/late statuses, Reply, completion before receipt, and Back. The separate long-answer check passed for forward and backward byte offsets with an answer over 8 KiB; the emulator reported no remaining app allocations on exit. The SDK screenshot service timed out during the long transfer, so large-window assertions use protocol messages; physical long-answer reading remains a device check.
- Android's polling behavior is unchanged: asynchronous runs are monitored about every 3 seconds in the Android foreground, 30 seconds in the background, or 5 minutes while approval is required, subject to WorkManager/OS delays. Further watch text chunks come from the saved phone answer. No live Hermes, speech-service, or Matrix request is part of these checks.
- Watch and Android version name are 0.1.8; Android version code is 9. The tag-triggered workflow builds and verifies the signed APK and paired PBW.

## 0.1.7 Hermes reply context (29 September 2026)

- The user's screenshots show the same conversation ID on both requests. The published Hermes Agent 0.19.0 wheel's `/v1/runs` parser was exercised directly: a session ID alone does not restore history; explicit user/assistant history is retained. The verified wheel hash is recorded in the test instructions.
- The production Kotlin compatibility checks passed for version detection, a follow-up containing the prior request and answer, first-turn omission, chronological role pairs, the 20-turn/64-KiB limits, UTF-8 boundaries, and byte-identical frozen retries. SQLite checks use the actual Room query and exclude other profiles, other conversations, notes, unfinished commands, cleared answers, and later commands.
- The full offline Python, native C, Kotlin protocol, HTTP, and ink checks passed. Local Android Room/KSP and release Kotlin compilation passed. Final signed APK packaging and certificate verification run in the tag-triggered release workflow.
- A clean emery build with SDK 4.33.1 passed with a 51,636-byte RAM footprint and 79,436 bytes free. The PBW version is 0.1.7, matching Android version name 0.1.7 / code 8.
- The compatibility history contains locally retained user/assistant text, not full server tool traces. The actual follow-up on the user's server remains a device acceptance check. No live Hermes or Matrix request was sent.

## 0.1.6 handwritten notes (29 September 2026)

- The emery watch builds with SDK 4.33.1 (51,636-byte RAM footprint, 79,436 bytes free). Emulator checks passed for handwriting menu/canvas/empty-save/Back, handshakes, scrolling bounds, touch-setting persistence, and navigation with menu touch disabled. The handwriting screen was visually inspected.
- The conversation emulator regression passed: automatic answer, duplicate completion, Reply in the same conversation, fast completion before receipt, and Back. These checks use a simulated phone peer.
- Native production-code tests passed for touch samples, straight-line simplification, multi-stroke characters, advance/undo/space, exact drawing capacity, interrupted writes, restart recovery, checksum corruption, and coexistence with a maximum pending transcript within 4 KiB. Kotlin decoded a C-produced stroke file and passed full-size binary reassembly, duplicate/reordered delivery, malformed envelopes, conflicts, offsets, and checksum checks. The existing protocol and offline HTTP tests also passed.
- Room/KSP generated schema 2. The SQLite migration check passed for the new ink table, capture uniqueness, and preservation of existing table definitions and conversation data. Existing entities are unchanged.
- Android 0.1.6 / code 7 signed `assembleRelease` and `lintRelease` passed; its APK signature matches the permanent certificate. Lint reports no errors and eight existing manifest/resource/target warnings. An initial Gradle process disappeared under local memory pressure; retrying with a 512 MiB heap, serial GC, and one worker succeeded.
- PBW finalization and companion metadata verification passed. The watch retains one pending ink file until a correlated complete phone receipt. Data Logging supplies an additional background route; it does not replace the retained copy.
- Physical finger sampling/letter pacing, Android viewer and notification taps, Bluetooth reconnect delivery with the watch app closed, and force-stop/host behavior remain device acceptance checks. No live Hermes or Matrix request was sent. Handwriting has no automatic Hermes submission path.

## 0.1.5 automatic answers and public distribution (29 September 2026)

- The emery conversation smoke dictates, reviews, sends, receives completion, and requires an automatic result request without another button press. It verifies answer display, Reply in the same conversation generation, duplicate completion, late status during a new draft, completion before the durable receipt, and Back. Screenshots of the answer, Reply action, and follow-up answer were visually inspected.
- The existing emulator checks passed for handshake/probe, scrolling in both directions and at bounds, Settings persistence, buttons with touch disabled, and Back to the launcher. Physical touch gestures remain a device check.
- The full offline Python, native C, Kotlin protocol, and HTTP suites passed. New native cases check automatic-result eligibility and cancellation. Android `:app:assembleRelease` passed for 0.1.5 / code 6. The watch builds with SDK 4.33.1; the release workflow passes actionlint.
- Result pages now include their conversation generation so Reply is offered only for the current conversation. The sole official Android host is selected automatically; multiple-host selection remains explicit.
- The release workflow now builds and verifies a store PBW alongside the signed APK. Publishing waits for both builds. Its public companion download URL points to the actual repository.
- Inspected CloudPebble upstream commit `08298a28fab376452b880409364913904f5ea135`: import/build omit companion metadata and regenerate `wscript`. The included operator patch passes four pure manifest tests. Hosted deployment, database migration, and direct installation from a patched hosted service were not exercised.
- Emulator tests use a simulated phone peer. No live Hermes or Matrix request was sent during these checks. Physical automatic replies, follow-up context through the real server, Android background delivery, and fresh app-store installation remain acceptance checks.

## 0.1.4 scrolling, touch setting, and launch readiness

- Reproduced v0.1.3's stationary text after Down using screenshot comparisons. Text was attached to ScrollLayer's fixed root instead of its scrolling content. The fixed emulator smoke checks movement in both directions and top/bottom clamping.
- Emulator checks passed for text movement/bounds, Settings On/Off, persistence after relaunch, and button navigation with touch disabled. Local signed Android release build (0.1.4 / code 5), release lint, and the full offline suite passed.
- Added a persistent watch Settings toggle for system touch navigation, enabled by default. Host tests cover default, saved Off/On, failed writes without changing the current preference, and Back from Settings. Buttons repeat while held.
- Watch now builds with SDK 4.33.1 and opts into its touch-navigation API. The matching emulator firmware is required; an already-running older emulator cannot execute that API. Actual touchscreen gesture delivery remains a hardware acceptance check.
- Android's launch probe waits one second, then retries only DifferentAppOpen responses up to eight attempts, releasing the sender lock between attempts. It still requires a correlated round trip before reporting success. User logs showed probe delivery 4–14 ms after a successful launch request; physical-host resolution remains unverified.

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
| Manual App Release workflow | Produces signed APK and verified PBW artifacts | Local builds passed; see release workflow for CI result |
| Version tag release | A pushed v* tag builds and attaches the APK to its GitHub Release | Passed: v0.1.0 built and APK attached to the published release |
| CloudPebble root import/build | Produces a PT2 PBW independently of Android; SDK version is recorded | Deferred, not run |
| PBW metadata finalizer | Companion declaration is restored; UUID is checked; all other member bytes are unchanged | Passed on locally built 0.1.1 PBW |
| Companion registration | Stock Pebble app routes watch messages to the installed companion | User confirmed finalized PBW fixes link; fresh store installation pending |
| Both authentication headers | Correct pair succeeds; either wrong credential fails clearly | Deferred, not run |
| Prefix URL and redirect handling | Prefix remains intact; no credentials follow a redirect | Deferred, not run |
| Quick Launch and dictation | Capture, review, send, re-dictate, and cancel work on PT2 | Deferred, not run |
| Normal Hermes request | Real response appears on phone and active watch | User confirmed manual Fetch; automatic flow passed emulator, physical check pending |
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

Source review removed the abandoned duplicate UI/runtime and corrected unresolved internal APIs, suspend calls, model mappings, and metadata. No Room schema was fabricated. The Android workflow supports manual builds and version tag releases, and now builds both the APK and PBW. Later emulator results and user reports are recorded above. No live Hermes call or Matrix send was made during implementation.

## Runtime and security checks

- Confirm credentials are entered only on the phone, encrypted at rest, excluded from backups/exports, absent from logs and exception messages, and never sent to the watch.
- Confirm a wrong Hermes credential and a wrong NetBird header fail distinctly enough for diagnosis without claiming which layer rejected a 401 or 403.
- Confirm a base path remains intact and an authenticated redirect is rejected without forwarding either credential.
- Confirm a lost create response is retried only with the original persisted key and payload when durable capability and retention allow it.
- Confirm GrapheneOS network permission, NetBird connectivity, Doze, app standby, process death, and force-stop behavior on the actual phone.
- Confirm no external Matrix delivery claim is derived solely from HTTP acceptance, a run completion, a Pebble transport ACK, or a notification.

Local builds and emulator checks passed. User reports confirm the existing physical watch link and manual Hermes result retrieval; the new automatic flow and fresh store installation still need hardware acceptance.


## Voice playback correction (v0.1.16)

Validated on 3 October 2026 with SDK 4.33.1. The full offline C/Kotlin/Python suite passed, including source-rate/pitch fixtures, audio packet bounds, and upload/playback ordering. The final watch build passed at 65,335 bytes of static RAM footprint, with 65,737 bytes available for heap before runtime allocations. The emulator audio smoke accepted reordered/duplicate 768-byte chunks and completed the 1.725-second bundled clip in 1.752 seconds with the dummy backend. The conversation voice smoke passed opt-in, text-only send then Up playback, held-button protection, replay, and Back cancellation. The PBW metadata finalizer passed.

The user reports normal physical playback of the bundled test clip and accelerated voice replies. The final change explicitly normalizes Android TTS speed/pitch and retains the existing watch speaker format. Tests verify conversion and transfer behavior; audible reply intelligibility on the user's speech engine and Bluetooth transfer timing still require physical acceptance.

## Continuous voice replies (v0.1.17)

Validated on 4 October 2026 with SDK 4.33.1. The full offline C/Kotlin/Python suite passed, including a 480,000-byte reply, one speaker session, partial speaker writes, cache I/O failures, restart cleanup, cancellation, packet bounds, progress and duration-aware timeouts. The watch build passed at 64,615 bytes of static footprint. PBW metadata finalization passed.

The emulator completed a 193,200-byte cached reply in one speaker session (23.024 seconds wall-clock versus 24.15 seconds nominal PCM duration, within the smoke test's coarse timing tolerance). The unchanged diagnostic fixture completed in 1.705 seconds versus 1.725 nominal. Saved-reply playback, replay queued during text loading, silent refresh and Back cancellation passed. The conversation harness now distinguishes a queued text-page retry from the fresh replay request. Dummy-backend emulator timing does not establish physical sound quality or Bluetooth throughput.

Replies use Android TTS at 1.5× speed with normal pitch and fully preload before playback. Initial loading remains; inter-clip Bluetooth loading is removed. Physical testing of intelligibility and loading time with the matching APK and PBW remains necessary.
