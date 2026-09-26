Implementation brief: Pebble Time 2 + Android + Hermes
Place this file in the repository root and give the coding agent this instruction:
Implement the complete v1 described in IMPLEMENTATION.md. Read existing repository instructions first. Create the source, configuration, GitHub workflow, and setup documentation. Do not run builds, tests, emulators, or deployments in this first pass. Android builds belong in the manually triggered GitHub Actions workflow; watch builds belong in CloudPebble. Use the researched version pins and contracts in this brief; do not reopen dependency selection. Finish with an honest implementation summary and the deferred validation items.

Research checked on 25 September 2026 against official documentation, released Maven artifacts, source JARs, and upstream source. This is an implementation specification, not a claim that the complete stack has been compiled or exercised on hardware. Use the following pins as a set. Do not spend the first pass choosing newer versions or installing toolchains.
Important CloudPebble correction: the inspected CloudPebble source regenerates project metadata and drops companionApp. The instructions therefore include a Python metadata finalizer for its downloaded PBW. Compilation still happens exclusively in CloudPebble; the official Android Pebble app remains unchanged. Installing the unmodified CloudPebble output is not sufficient for this companion design. Details and source evidence are in section 14.
0. Researched version baseline — use these exact choices
Component	Pin / setting	Reason or constraint
Android Gradle Plugin	9.2.0	Official release supports API 37 and requires Gradle 9.4.1 [S10]
Gradle wrapper	9.4.1	Exact distribution and wrapper checksums in section 18
CI Java runtime	Eclipse Temurin 21 LTS	Gradle-compatible runtime; receive current security patches within 21
Java and Kotlin bytecode	JVM 17	Set both targets consistently; independent of CI runtime
Kotlin Gradle/compiler version	2.4.20	Official compatibility table includes AGP 9.2 and Gradle 9.4.1 [S11]
Compose compiler plugin	2.4.20	Match Kotlin, not the Compose BOM
Kotlin serialization plugin	2.4.20	Match Kotlin
KSP	2.3.12	Released KSP generation compatible with this AGP baseline [S12]
Android compile SDK	37.0	PebbleKit 1.3.2 AAR explicitly declares minCompileSdk=37; the SDK package is platforms;android-37.0
Android target SDK	36	Deliberate v1 behavior baseline; this is a sideloaded app
Android minimum SDK	26	Project choice; PebbleKit itself declares minimum 24
Android build tools	36.0.0	Documented AGP 9.2 default
Android command-line tools	15859902 (22.0)	Pinned setup-android input
PebbleKit Android 2	io.rebble.pebblekit2:client:1.3.2	Released 18 September 2026; inspected POM, AAR and source JARs
AndroidX Core	1.17.0	Matches PebbleKit's declared Core dependency
AndroidX Activity Compose	1.11.0	Released artifact
AndroidX Lifecycle	2.9.4	Use consistently for runtime and view-model Compose artifacts
Compose BOM	2025.10.01	Deliberate stable library baseline; UI/foundation 1.9.4, Material3 1.4.0
Room	2.8.4	Runtime and KSP compiler must match
WorkManager	2.10.5	Persistent background work
DataStore Preferences	1.1.7	Matches PebbleKit's DataStore baseline
Kotlin coroutines	1.10.2	Android artifact; matches PebbleKit
OkHttp	5.3.0	Released HTTP client
kotlinx.serialization JSON	1.9.0	Runtime version is separate from the Kotlin plugin version
Pebble SDK reference release	4.33.1	Current documented release, supports PT2; actual CloudPebble deployment is host-managed [S13]
Pebble manifest API family	"sdkVersion": "3"	Do not put 4.33.1 in this field
Pebble target	"targetPlatforms": ["emery"]	Pebble Time 2: 200 × 228, 64 colors


These are selected compatible baselines, not a claim that every library is the newest available. Their published artifacts were checked. AGP/Kotlin compatibility was checked in official tables; a complete build is deliberately deferred. The user's installed Hermes version, official Pebble app version, watch firmware, and the hosted CloudPebble SDK have not been queried. Detect the required capabilities at runtime and report them; do not invent minimum installed-version numbers.
AGP 9 trap: use its built-in Kotlin support. Do not apply org.jetbrains.kotlin.android, kotlin-android, or kapt, and do not opt out with android.builtInKotlin=false or android.newDsl=false. AGP 9.2's default Kotlin is too old for PebbleKit 1.3.2's Kotlin 2.4 metadata: explicitly raise KGP to 2.4.20 using the documented classpath mechanism. The exact Gradle files are in section 18 [S10–S12].
1. Hard requirements
- Use one repository, one standalone Android companion, and one native Pebble Time 2 watch app for v1.
- Keep the official Pebble Android app installed and unchanged. Use its supported companion interface. Do not fork it, patch it, replace its Bluetooth stack, or require a custom build of it.
- The user already has a Hermes server with its HTTP API enabled. Connect to that server; do not build or deploy another backend.
- Hermes owns the Matrix integration and sends messages using the user's existing authorization. Neither new app logs into Matrix or stores Matrix credentials.
- The Android app must have separate settings for the Hermes API credential and an additional configurable HTTP header for NetBird service access.
- Target the user's Pebble Time 2 and Android/GrapheneOS phone. The new Android companion must not require Google Play Services.
- Do not execute Gradle, Android or Pebble builds, unit/instrumentation tests, emulators, device installs, or live Hermes requests during the first implementation pass. Do not dispatch CI or start a CloudPebble build. Reading source, reviewing diffs, and checking plain configuration files are allowed.
- Do not install build toolchains just to validate this first pass. Supplying authentic Gradle wrapper files is allowed; executing the wrapper is not.
- Implement functioning production paths, not a demo with canned responses, mock networking, or unimplemented core methods. State explicitly that build and runtime validation remain deferred.
- Preserve existing repository content and instructions. Do not overwrite unrelated files or automatically push, publish, or change the user's running services.
2. Product scope
Android companion	Pebble watch app	Outside v1
Hermes connection settings and independent NetBird header	Quick Launch voice capture	Raw watch audio upload or storage
Durable requests, retries, status, and responses	Transcript review, send, retry, cancel	Offline watch speech transcription
Persistent Hermes conversation and follow-ups	Scrollable Hermes answers and follow-up dictation	Full Matrix inbox, contact directory, or room browser
Local notes and recent history	Save a dictated note locally through the phone	Matrix login, Matrix SDK, or Element X automation
Secure credential storage and connection diagnostics	Recent items and connection state	Official Index Feed integration
Background recovery and optional result notifications	New conversation action	Watch audio playback or TTS
Full text display and manual typed request on phone	Clear pending/error states	Separate watch apps, plugin system, or integrations implemented outside Hermes


Asking Hermes to send a Matrix message, create a reminder, control a device, or perform another task is supported only to the extent that the user's Hermes instance already has the corresponding tools. Do not implement those integrations in the companion. Do not infer that a tool available in a Matrix-connected Hermes chat is automatically available to the API profile: document that the user must enable the relevant tools for API requests too.
Element X remains the user's Matrix client. It is not a transport or authentication dependency for this project. Messages that Hermes successfully sends as the user's Matrix account can appear through that account's normal Matrix synchronization. Recipients' replies do not automatically become responses in the new watch app. Existing Element X notifications may continue through the normal Pebble notification path.
3. Repository layout and build ownership
Keep the Pebble project at the repository root so CloudPebble can import it without depending on GitHub subdirectory import support. Put Android in a subdirectory. CloudPebble's upstream subdirectory-import change was still listed as open when this brief was researched [S4].
Path	Purpose
package.json	Pebble package, fixed UUID, target platform, message keys, Android companion declaration
wscript	Standard Pebble build configuration; include only watch sources/resources
src/c/	Native watch app and committed protocol constants
resources/	Watch resources referenced by package metadata
android/	Self-contained Gradle Android project, including wrapper files
protocol/	Versioned wire specification and example messages
tools/finalize_pbw.py	Standard-library Python utility restoring companion metadata after the CloudPebble build
docs/	Setup, architecture, deferred validation, limitations
.github/workflows/android.yml	Android-only, manually triggered build
README.md	User-facing project overview and installation routes
IMPLEMENTATION.md	This brief


Choose application names and Android package identifiers from the actual repository context. Generate the watch UUID once and commit it in all required locations. Do not change UUIDs or package IDs between routine builds.
No Pebble SDK installation, PBW compilation, emulator invocation, or CloudPebble webhook belongs in GitHub Actions. Do not add PebbleKit JS or a JavaScript configuration page; the native Android app owns configuration.
4. End-to-end behavior
1. The user opens the watch app, or invokes it through an assigned Quick Launch button.
2. The watch starts standard Pebble dictation. The normal Pebble phone integration performs its configured speech recognition.
3. The watch shows the resulting transcript. The user can send, dictate again, save as a note, or cancel.
4. A send operation gets a stable capture identifier before transport starts. The watch retains the unsaved item until Android explicitly acknowledges durable receipt.
5. Android stores the command, then creates or resumes the correct Hermes conversation and submits the request.
6. Android persists the returned run identifier, monitors the run, stores the result, and updates the watch while its app is active.
7. If the watch app closes, Android retains the result. Reopening the app retrieves current status and recent results.
8. A follow-up uses the same conversation unless the user explicitly starts a new one.
The standard dictation API supplies a transcript, not reusable microphone audio [S3]. A completed transcript may be queued when network access fails. Do not describe this as offline recording or promise dictation without a functioning phone connection and transcription service.
5. Android implementation
Use Kotlin, Jetpack Compose, Room for durable command/history state, DataStore for non-secret preferences, Android Keystore-backed AES-GCM encryption for credentials, OkHttp, and WorkManager. Use section 0 and section 18 exactly. Use a fresh random GCM IV per encryption and handle unavailable/invalidated keys by requesting secret re-entry. Do not use deprecated encrypted-preferences libraries just to avoid implementing a small SecretStore. Do not introduce dependency injection or navigation frameworks unless the existing repository already requires them.
Keep responsibilities separated without building a general framework:
- SettingsRepository: endpoint configuration, access-header settings, and credential references.
- SecretStore: encrypt/decrypt credentials using a Keystore-held key.
- PebbleBridge: supported PebbleKit listener/sender, protocol validation, and watch lifecycle.
- HermesClient: URL construction, authentication, capabilities, sessions, runs, polling, and stop.
- CommandRepository: durable state, deduplication, histories, and session mapping.
- CommandDispatcher: serialized submission and recovery with one owner per pending item.
- Screen models: setup, home/recent items, request detail, notes, and diagnostics.
The main screen should expose connection readiness, recent requests/notes, and a typed request field. The phone input is a useful fallback and must use the same dispatcher and persistence path as watch input. A request detail screen shows its transcript, status, full response, and relevant retry/stop actions.
Request only permissions actually used. The new companion does not need microphone, contacts, notification-listener, accessibility, or direct Bluetooth access merely to relay Pebble dictation. Result notifications may request Android's notification permission when applicable. Do not make notifications mandatory for sending a request.
6. Connection settings and the separate NetBird header
Provide these independently editable settings:
Setting	Required behavior
Hermes server URL	HTTPS base URL, including an optional path prefix; no embedded credentials, query, or fragment
Hermes API key	Masked secret; sent as Authorization: Bearer <key>
Additional access header	Enabled/disabled toggle; label it clearly as usable for NetBird
Header name	User-editable; suggest X-NetBird-Access as an example, not a required NetBird standard
Header value	Separate masked secret; send exactly the entered value, with no automatic Bearer prefix
Test connection	Read-only authenticated capability check, with clear success/failure


Example conceptual request headers, using placeholders only:
Authorization: Bearer <hermes-api-key>
X-NetBird-Access: <netbird-service-secret>
Content-Type: application/json
For NetBird reverse-proxy header authentication, configure its Custom Header option with the same name and value. NetBird strips the matching access header before forwarding upstream. Using its Authorization-based preset would compete with Hermes' credential; keep the two headers distinct [S1]. This is a service-access secret, not a NetBird management API token. A header also does not establish a VPN connection where private-network access is required.
Implementation requirements:
1. Centralize HTTP configuration. Attach both credentials to every request to this configured service, including connection checks, session operations, submission, polling, history recovery, and stop. Future streaming must use that same authenticated client.
2. Preserve a base path. For https://example.invalid/hermes/, append v1/runs to produce /hermes/v1/runs; a leading slash must not discard the prefix. Explain that the setting is the API root, not the chat-completions endpoint.
3. Validate the header name using HTTP token rules. Reject line breaks and invalid control characters in either field. Reject collisions, case-insensitively, with Authorization, Host, Content-Length, Content-Type, Accept, Connection, Transfer-Encoding, Idempotency-Key, and protocol-owned Hermes headers. Also reject NetBird's reserved identity headers X-NetBird-User and X-NetBird-Groups.
4. Do not silently alter secret values. Validate rather than trim an access-header value into a different credential. Disabling this header must leave Hermes authentication enabled.
5. Disable automatic redirects for authenticated requests. Report redirects as a configuration problem; never forward credentials to another origin or a browser login page.
6. Require valid TLS. Do not implement trust-all certificates or a release-mode cleartext bypass.
7. Store secrets encrypted at rest; exclude secret storage from backups and exports. Do not log headers, request bodies, responses, or sensitive URLs by default. Mask fields and keep secrets out of exception messages.
8. Never put either credential in source, Gradle properties committed to Git, APK resources, GitHub variables, watch messages, screenshots in documentation, or a sample configuration.
9. Changing the server URL/profile must pause unresolved work for the old target. Do not silently send existing requests to a different server. Credential changes with an uncertain submission outcome require reconciliation, since server idempotency may be credential-scoped.
10. Distinguish network/TLS failures, unsupported API routes, invalid response formats, and authentication rejection. A 401 or 403 alone does not identify whether NetBird or Hermes rejected the request. An HTML login page is not a successful API response.
7. Hermes transport contract
Use Hermes' native asynchronous HTTP runs interface, with polling in v1. Do not use a synchronous chat request as the only durable transport, or silently switch APIs after an uncertain submission.
Implement against the selected server API contract [S2, S5]:
Operation	Endpoint
Inspect supported capabilities	GET /v1/capabilities
Create an empty conversation	POST /api/sessions
Reconcile a known conversation	GET /api/sessions/{session_id}
Submit a turn	POST /v1/runs
Read current run state/result	GET /v1/runs/{run_id}
Request cancellation	POST /v1/runs/{run_id}/stop
Recover conversation history when appropriate	GET /api/sessions/{session_id}/messages


A turn uses input and the persisted session_id. Omit model/provider overrides, caller-supplied history, and previous-response chaining. Let the configured Hermes profile select its model and tools and retain conversation context.
The following shapes were inspected in Hermes source at commit 59004a62356f3a4697ab0fe8ad5086d2b405e2a6 [S14]. Implement these contracts with unknown JSON fields ignored; required identifiers and types must still be validated.
- Capabilities: object is hermes.api_server.capabilities. Read features.run_submission and features.runs_idempotency.{supported,durable,retention_seconds}. These are nested under features, not top-level flags. The endpoints object maps names to {method,path}. Require run submission; clearly identify missing routes/capabilities as an unsupported server. Recheck capabilities after changing the server or credentials. Do not probe the real endpoint during the implementation pass.
- Create session: persist a client-generated pt2_<random-UUID> identifier first, then POST {"id":"pt2_<persisted-UUID>"}. The 201 response is {"object":"hermes.session","session":{"id":"pt2_<persisted-UUID>",...}}. The identifier is session.id, not a top-level session_id. Do not give every session the same title: titles can conflict globally. On 409 with error code session_exists, GET that already-persisted session ID and reconcile it. Do not generate another ID after an ambiguous create result. Session creation contains no user command.
- Submit: POST {"input":"the confirmed transcript","session_id":"the persisted session ID"} with the persisted Idempotency-Key. A 202 response contains run_id, status, and replayed; initial status can be started or queued. Persist run_id transactionally before further work. Do not expect a finished answer in the acceptance response.
- Poll: GET returns an object of type hermes.run with run_id, status, and, as available, session_id, output, error, timestamps, and additional fields. Final output is the answer string. Handle queued, started, running, waiting_for_approval, stopping, completed, failed, cancelled, and interrupted, plus an explicit unknown-state fallback. Fields such as output can be absent before completion.
- Session continuity: if polling provides an updated session_id after server-side session rotation, persist it for subsequent turns. Never rewrite the already-submitted request body used for an idempotent retry.
- Stop: POST to /v1/runs/{run_id}/stop; treat its returned status as a request to stop, then reconcile the run. Stop is not an undo operation.
- History: GET /api/sessions/{session_id}/messages?limit=50&offset=0&order=latest. The response has object: "list", session_id, a data array and a pagination object with limit, offset, order, and returned. Do not parse a nonexistent top-level messages array. History supports review and reconciliation; it does not prove an external action's delivery.
Use only fields needed by v1. No API route guessing, compatibility fallback to chat-completions, or endpoint discovery by sending a real user command.
Use a dedicated PT2 conversation, with a visible New conversation action. Do not automatically select an unrelated Matrix session or imply that the two conversations share a transcript. Store confirmed server identifiers exactly, including confirmation of the client-supplied session ID. Serialize turns within a conversation. A queued follow-up must retain its intended conversation even if the user subsequently starts another one.
Treat HTTP acceptance, run completion, and external action success as separate facts. Display the actual Hermes answer. A finished run is not independent proof that a Matrix recipient received a message. Do not synthesize a delivery receipt from HTTP 202 or a transport ACK.
Use bounded polling: prompt while the watch/phone screen is active, slower while waiting in background, exponential backoff on network failures, and Retry-After when provided. Do not run an endless tight loop or keep a permanent foreground service alive. Streaming tokens, detailed tool traces, and server administration are deferred.
Honor existing Hermes approval policies. If execution waits for a server-side approval, show an explicit “Approval needed in Hermes” state and stop pretending the request is progressing normally. A full approval-management UI is outside v1; do not automatically approve or weaken server policies. Surface unknown future statuses safely.
8. Durable submission and duplicate prevention
This app can trigger real messages, so safe recovery is a core feature.
Hermes documents an Idempotency-Key for run creation: identical retries reuse the original run; a different payload with that key conflicts. Its documented retention is 24 hours after the last status update, scoped to the API credential/profile [S2]. This is not an unlimited exactly-once guarantee.
Implement the following local behavior:
- Store each accepted watch command in a Room transaction, with a unique constraint on its watch origin and capture ID. Duplicate AppMessages return the existing command state.
- Generate a random HTTP idempotency key on Android and persist it before the first submission. Persist the exact logical request payload, destination configuration identity, and conversation mapping as well.
- Freeze the payload once submission is attempted. A transport retry uses the same key and payload. Do not add changing timestamps or regenerate instruction text on retry.
- Save the returned run_id before starting monitoring. After process death, resume monitoring the same run instead of creating another.
- If a run-create response is lost, automatically retry the original keyed request only when capabilities advertise both supported=true and durable=true. Record first-attempt time and use a conservative deadline of the earlier of 23 hours and the advertised retention minus a safety margin. A missing, nonpositive, or implausible retention disables automatic ambiguous-outcome retries. The phone cannot know the exact server update time. Without durable support, an initially explicit user submission is allowed, but an ambiguous outcome requires review.
- If the server version lacks verified safe retry support, or the retry window has passed, show “Outcome unknown — review before retrying.” Do not automatically submit with a fresh key.
- A conflict is an implementation or state error, not a reason to generate another key.
- Never automatically re-run a terminal failed, interrupted, or cancelled turn that might already have performed a side effect. Offer review and an explicit new request when appropriate.
- Distinguish an unsent queued item from a request already accepted by Hermes. Removing an unsent item prevents dispatch; requesting stop after dispatch cannot undo a message already sent.
- If a run record expires, use available history only for reconciliation. Missing run status must not trigger automatic re-execution.
Suggested local states: PENDING_PHONE_TRANSFER on watch; QUEUED, SUBMITTING, ACCEPTED, RUNNING, NEEDS_APPROVAL, COMPLETED, FAILED, STOPPING, CANCELLED, INTERRUPTED, and OUTCOME_UNKNOWN on Android. Store server state separately from local transport state. Map server spellings explicitly rather than assuming these labels are its enum.
Persist input, output, timestamps, origin, session/run IDs, retry metadata, and structured error categories. Do not store secret header values in every command row. Limit retained history and expose a clear-history action that does not erase unresolved work or duplicate-prevention records accidentally.
9. PebbleKit Android integration
Use io.rebble.pebblekit2:client:1.3.2 from Maven Central. Do not use its server artifact or legacy com.getpebble.android.kit broadcasts. Its published source JARs, rather than the README alone, establish the signatures below [S6, S15]. The README sender example omits a now-required Context.
Required imports and signatures:
import io.rebble.pebblekit2.client.BasePebbleListenerService
import io.rebble.pebblekit2.client.DefaultPebbleAndroidAppPicker
import io.rebble.pebblekit2.client.DefaultPebbleSender
import io.rebble.pebblekit2.client.PebbleSender
import io.rebble.pebblekit2.common.model.PebbleDictionary
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import io.rebble.pebblekit2.common.model.ReceiveResult
import io.rebble.pebblekit2.common.model.TransmissionResult
import io.rebble.pebblekit2.common.model.WatchIdentifier
import java.util.UUID
The listener overrides suspend fun onMessageReceived(watchappUUID: UUID, data: PebbleDictionary, watch: WatchIdentifier): ReceiveResult, plus non-suspending onAppOpened(watchappUUID: UUID, watch: WatchIdentifier) and onAppClosed(...). Return ReceiveResult.Ack or .Nack; there is no SUCCESS enum. Own and cancel the listener's coroutine scope on destruction without canceling durable WorkManager jobs.
val sender: PebbleSender = DefaultPebbleSender(applicationContext)
val results: Map<WatchIdentifier, TransmissionResult>? =
    sender.sendDataToPebble(APP_UUID, data, watches = listOf(watch))
PebbleDictionary is Map<UInt, PebbleDictionaryItem>. Items include Text(String), Bytes(ByteArray), UInt32(UInt), and Int32(Int). All received numeric tuples are normalized to UInt32 or Int32, even when C sent an 8-bit or 16-bit value. Decode by numeric value and bounds, not a cast to UInt8. WatchIdentifier.value is an opaque string.
A null send result means the official phone host is unreachable. An empty map when broadcasting with watches=null means no connected watches. Per-watch results include Success, FailedWatchNotConnected, FailedWatchNacked, FailedTimeout, FailedDifferentAppOpen, FailedNoPermissions, and Unknown. Map each to useful recovery; do not treat a non-null map as success. Call sender.close() when its owning component is destroyed.
Register the listener inside Android's application element:
<service android:name=".pebble.PebbleListenerService" android:exported="true">
    <intent-filter>
        <action android:name="io.rebble.pebblekit2.RECEIVE_DATA_FROM_WATCH" />
    </intent-filter>
</service>
Do not invent an android:permission with that action string. The client library's manifest contributes the package-visibility query for io.rebble.pebblekit2.SEND_DATA_TO_WATCH. Use its base service and supported host selection so binder caller checks remain intact.
Provide a phone host-selection control using DefaultPebbleAndroidAppPicker.getInstance(context). Set enableAutoSelect=false before first use in each process, list candidates with getAllEligibleApps(): List<String>, and let the user select their official Pebble installation. selectApp(packageName: String?) and getCurrentlySelectedApp(): String? are suspending methods; the library persists the selection. Do not auto-trust the first package advertising a service.
Declare the Android companion in the root Pebble package as shown in section 19. Its apps field contains objects with a package field, not a list of package-name strings. Section 14's PBW finalization must preserve that declaration all the way to installation.
- Use the same watch UUID in the package and Android routing.
- Reject messages for a different UUID or unsupported protocol version.
- Track the source watch identifier and address replies to the correct watch using the actual library API. Do not assume only one phone-to-watch destination exists.
- Treat inbound messages as untrusted structured input. Follow the library's supported binding/caller validation; a UUID check alone is not caller authentication. Do not invent a manifest permission that blocks the official app.
- Keep callbacks short. Copy input, validate, persist, acknowledge, and schedule asynchronous work.
- Do not wait for Hermes inside the incoming message callback.
- Handle app-open/app-close events to refresh state and stop futile watch transmissions.
- Close sender resources according to the lifecycle above.
- Document PebbleKit client 1.3.2 and require an official Pebble app exposing the PebbleKit 2 service. No numeric minimum host-app release was established from the inspected sources. Detect the advertised service, selected host, and application handshake; report “Update the official Pebble app” if unavailable. Do not claim compatibility with legacy Pebble phone apps.
10. Versioned watch/phone protocol
Write a concrete protocol/README.md before implementing both sides. Commit matching C and Kotlin constants. CloudPebble must not need to run a custom code generator or fetch Android build artifacts.
Use AppMessage dictionaries with fixed numeric keys and explicit types. Define at least:
Field	Meaning
Protocol version	Major wire-format version; reject incompatible versions clearly
Message kind	Operation or response type
Transfer ID	Identifies a logical message across chunks and retries
Capture/request ID	Identifies the durable user command separately from transport retries
Chunk index/count	Bounded multipart reassembly, when needed
Payload	Bounded UTF-8 text or explicitly documented structured bytes
Correlation/status/error fields	Match replies to requests and describe actual state


Required operations: handshake/state refresh, submit Hermes request, save local note, durable-receipt acknowledgment, fetch recent items, fetch one result page, status update, start conversation, stop a submitted request, and structured error. Design application-level acknowledgment separately from Pebble's transport acknowledgment.
Choose and document exact key numbers, type widths, operation numbers, payload format, maximum counts, timeout values, and buffer allocation. Use one source-of-truth table and include example exchanges. Keep control messages small; transfer long text in chunks/pages.
Reliability requirements:
- Never send a complete long response as one dictionary and hope it fits. Choose a conservative payload chunk, such as 192 bytes, only after calculating full dictionary overhead against the selected buffers and library limits.
- Use byte counts, not Kotlin character counts. Reassemble bytes before decoding, or split at valid UTF-8 boundaries. Never silently corrupt emoji or multibyte characters.
- Bound allocations and chunk counts; reject malformed indexes, oversized messages, and conflicting duplicate chunks.
- Keep at most one outstanding outbound AppMessage per direction; retry boundedly with backoff. Ignore stale responses after a newer request changes the screen.
- A transport ACK proves transport progress. “Saved on phone” requires an explicit application acknowledgment issued after a successful database commit.
- Persist a capture ID before sending. Use a stable watch-install identifier and persistent monotonic counter, or another documented collision-resistant scheme; watch timestamps alone are insufficient.
- Preserve the original capture ID on retry/reopen. Android generates its own persisted HTTP idempotency key when first receiving that command.
- Make partial transfers resumable or restartable under the same logical ID. A restart must not create a second user command.
- Keep watch history bounded and paginate from Android. Put full responses on the phone, with a clear “More on phone” indication when a watch display limit is reached.
- When protocol versions differ, ask the user to update the apps instead of interpreting unknown fields as a command.
11. Watch UI and implementation
Use native C for PT2's emery platform [S9], using the 4.33.1 SDK reference and manifest/wscript in section 19. CloudPebble controls its deployed toolchain; record the SDK reported by its eventual build log. Use standard fonts and button navigation with layouts based on actual bounds. Do not require touch APIs, a JavaScript companion, a phone microphone permission, or a nonexistent Pebble microphone manifest capability.
Entry behavior:
- Quick Launch opens dictation immediately when the platform reports that launch reason. Document assigning the button in normal Pebble settings.
- Normal launcher entry shows a small menu: Ask Hermes, Save note, Recent, New conversation, and Status.
- If there is unsaved input or unresolved transfer state, provide recovery instead of silently replacing it with a new recording.
Use dictation_session_create(0, callback, context) so the SDK allocates the transcript buffer dynamically; a fixed small buffer can silently truncate a command. Check for a null session. In the callback, accept only successful, nonempty UTF-8 text of at most 1,024 bytes excluding NUL, copy it into owned storage before returning, and otherwise ask the user to dictate a shorter request. Never submit a silently shortened command. Compare dictation_session_start(...) to DictationSessionStatusSuccess: its return type is a status enum, not a Boolean. Disable the SDK's extra confirmation with dictation_session_enable_confirmation(session, false) because the app supplies its own mandatory transcript review [S3, S18]. Handle all failure/cancellation states and release the session, windows, layers, timers and buffers.
After dictation, show a scrollable transcript and actions for Send/Save, Dictate again, and Cancel. Do not automatically execute a command before the user accepts the transcript. Do not advertise a full keyboard editor on the watch.
After send, show actual progress: waiting for phone, saved/queued, accepted by Hermes, working, needs attention, or completed. A short haptic may accompany durable receipt or completion; do not use an identical success signal for an error.
Answers must be readable with Up/Down scrolling, a follow-up action using the same conversation, and a route back to recent items. Prevent double clicks from submitting duplicate commands. Leaving a screen does not imply canceling the server run.
For v1, persist one pending transcript of up to 1,024 UTF-8 bytes with its capture ID and operation. Write it in chunks of at most 240 bytes, keeping every persistence value under the SDK's 256-byte limit. Use a commit marker/checksum so a partial write is never treated as a complete command. Budget metadata and journal overhead within a conservative 4 KiB total. Do not overwrite this pending item; require receipt or explicit discard before a new capture. Android can queue many requests after durable receipt. This is transcript persistence, not audio storage [S18].
For Save note, persist the text and timestamp on Android and return a durable acknowledgment. Notes do not invoke Hermes until the user explicitly chooses to send one. Label this as local notes, not Pebble's official Index Feed.
12. Background behavior and recovery
Use unique WorkManager work and transactional command ownership to recover pending requests after process death [S7]. Foreground screens may request immediate refresh through the same coordinator; they must not create a second submission path.
Do not assume the Pebble binding keeps Android alive indefinitely. Separate submission from result monitoring so an accepted Hermes run can continue while Android is suspended. Reopening either UI resumes reconciliation. Stop polling terminal requests and bound each worker execution.
Use Android's supported background execution mechanisms. If expedited work is used, implement quota fallback. Avoid a permanent foreground service, recurring exact alarms, or polling solely to keep the process alive. WorkManager scheduling is best effort, so do not promise instant delivery under Doze or force-stop.
Optional result notifications should have private lock-screen content and open the relevant request. Do not enable an Android notification listener. Describe official Pebble forwarding of those notifications as a user setting, not guaranteed delivery into a closed watch app.
Document GrapheneOS network-permission checks, OS background restrictions, and any required NetBird network connectivity. Present these as troubleshooting steps when relevant; do not require blanket battery exemptions during onboarding.
13. GitHub Actions: Android only
Create .github/workflows/android.yml with these properties:
- Initial trigger is only workflow_dispatch. This keeps the first implementation pass from causing an automatic build on push. The user will run it later from GitHub Actions [S8].
- Use the complete workflow in section 20: Ubuntu 24.04, Temurin 21, explicit SDK packages, and immutable action revisions verified from release tags. Do not replace its pins with guessed or floating versions.
- Work in android/ for Gradle commands. Commit an authentic gradlew, gradlew.bat, wrapper properties, and wrapper JAR. Record the Gradle distribution checksum. Do not commit machine-specific local.properties or an Android SDK.
- The build step runs ./gradlew --no-daemon :app:assembleDebug. Do not add test, lint, emulator, or Pebble jobs to the initial workflow.
- Upload the resulting APK as an Actions artifact with a clear name. Fail artifact upload if the APK is absent; do not turn build failures into successful jobs.
- Use minimal permissions, normally contents: read, a bounded timeout, and appropriate Gradle caching. Avoid caching or printing secrets.
- Do not require the Hermes URL, Hermes key, NetBird secret, or Matrix credentials in CI. These are runtime app settings.
- Document that manual workflows must be available on the repository's default branch before the Actions UI can run them.
The first artifact may be a debug APK for sideloading. Explain that signing must remain stable for seamless updates: disposable CI debug keys can change between runners. Document an optional stable signing-keystore setup through GitHub secrets, without committing a private key or blocking the first secret-free debug build. Do not present that APK as a Play Store release.
No workflow dispatch or build is part of the coding agent's first run. Future automated triggers or testing workflows require a later change; do not silently enable them now.
14. CloudPebble instructions and required PBW metadata finalizer
Source-confirmed issue: CloudPebble commit 08298a28fab376452b880409364913904f5ea135 rebuilds package.json from its project model. generate_v3_manifest_dict does not emit companionApp, and its import path does not retain that field. project_assembly.py also regenerates wscript, so a repository wscript post-build hook is not a reliable CloudPebble workaround. The current official mobile app selects PebbleKit 2 through appInfo.companionApp.android.apps. Losing that metadata therefore breaks this integration [S16–S17]. This conclusion comes from upstream source inspection; the user's hosted project has not been opened or built.
Implement tools/finalize_pbw.py, a small Python 3 standard-library command. It edits packaging metadata after the cloud compiler has produced the app; it does not compile Pebble code or require a local Pebble SDK. No GitHub watch-build job is needed.
Required CLI, documented for use after a later CloudPebble build:
python3 tools/finalize_pbw.py --package package.json --input downloads/hermes-pt2.pbw --output downloads/hermes-pt2-ready.pbw
Implementation contract:
1. Read package.json and require a valid pebble.uuid and nonempty pebble.companionApp.android.apps array of package objects. Treat the repository metadata as the source of truth.
2. Open the PBW as a ZIP. Require exactly one root appinfo.json, reject duplicate archive member names, and reject a UUID mismatch with the repository. Bound total uncompressed input, for example to 32 MiB, before reading members into memory.
3. Parse root appinfo.json and set its top-level companionApp to the repository's pebble.companionApp. Preserve all other JSON values and all other ZIP members. Do not add a nested pebble object to appinfo.
4. Write a separate output file with exclusive-create behavior; refuse in-place edits or overwriting an existing output. Python zipfile regenerates the edited member's ZIP CRC. Do not modify executable bytes, resource files, platform manifests, or embedded binary/resource CRC values.
5. Reopen the result and verify its UUID, exact companion declaration, and SHA-256 equality of every other member's uncompressed bytes against the input. Remove an incomplete output on failure. Preserve original archive metadata where practical; recompression may change the ZIP's physical bytes, which is expected.
6. If the declaration was already correct, still produce and verify the requested separate output; report “metadata already present.” Print package/UUID/output diagnostics only, never secrets.
7. Write this utility in the first pass but do not run it against a real build yet. Add it to the later acceptance checklist, including wrong UUID, duplicate entries, and preservation of nonmetadata members.
Write docs/cloudpebble.md with this sequence:
1. Open CloudPebble, import the root native C project from the actual GitHub repository and branch, and select PT2/emery as the target. Do not assume the importer defaults to the right branch.
2. CloudPebble manages SDK installation. The documented reference release is 4.33.1, but the project manifest must retain sdkVersion: "3". Do not invent a UI for pinning 4.33.1; upstream removed per-project SDK selection. Record the actual host SDK later from its build output.
3. Build manually in CloudPebble when the user is ready, then download the PBW.
4. Run the documented metadata-finalizer command locally on that downloaded file. This requires Python 3 only. Inspect its success report.
5. Install the -ready.pbw through the ordinary Pebble file-install flow. Do not use CloudPebble's direct-install button for an unfinalized output, because it bypasses the required correction.
6. Install the Android APK, select the official Pebble phone host, enter both service credentials, and perform the application handshake. Missing companion permission should direct the user back to the finalized PBW step.
Keep GitHub as the source of truth; CloudPebble exports must not overwrite the correct companion declaration. No automatic cloud webhook, local Pebble compiler, Node build, or Android artifact is required. This preserves the requested cloud-only watch compilation. A completely one-click CloudPebble build-and-install flow cannot be promised until its hosted implementation preserves the declaration; if that changes, the finalizer can verify and leave it intact.
15. Implementation sequence and deliverables
Complete these steps without executing the deferred validation:
1. Inspect repository instructions, current files, branch, and naming. Preserve useful existing setup. Adopt the section 0 pins and record only concrete conflicts with existing repository requirements; do not restart version research.
2. Establish the root Pebble project and nested Android project. Add only compatible, pinned dependencies and authentic build infrastructure.
3. Define the protocol, states, IDs, acknowledgment rules, and persistence model before writing both transports.
4. Implement Android settings, secure secrets, shared HTTP authentication, Hermes transport, durable dispatch, notes, and history UI.
5. Implement PebbleKit binding and native watch UI, dictation, bounded transfer, persistence, and recovery.
6. Add the exact manual Android workflow, PBW metadata finalizer, and CloudPebble setup documentation.
7. Perform source-level review of package IDs, UUIDs, dictionary types, state transitions, URL construction, header handling, and workflow scope. Do not run builds or tests to do this review.
8. Finish documentation and report what was implemented and what remains unvalidated.
Required documentation: README.md, docs/android-setup.md, docs/cloudpebble.md, docs/architecture.md, docs/validation.md, and protocol/README.md. Include the NetBird Custom Header setup, Hermes API-profile prerequisites, runtime secret entry, sideloading, Quick Launch setup, first-run prohibition, and the v1 limitations.
If useful, add narrowly scoped test source for risky behavior such as duplicate suppression, immutable retries, header scoping, and UTF-8 transfer. Do not run it in this pass. Tests are not a substitute for the later integration checks.
Do not finish with empty service implementations or TODOs for the main send/receive path. If a real upstream API incompatibility prevents completion, isolate it, cite the exact dependency contract, and report the concrete missing capability. Do not substitute a custom official Pebble app, a Matrix login, or a new backend to hide the gap.
16. Deferred acceptance checks — do not run in the first pass
Record these in docs/validation.md for a later session:
Check	Expected result
Manual Android workflow	Produces a sideloadable APK; no Pebble job runs
CloudPebble root import/build	Produces a PT2 PBW independently of Android; SDK version recorded
PBW metadata finalizer	Companion declaration restored; UUID checked; all other member bytes unchanged
Companion registration	Stock Pebble app routes watch messages to the installed companion
Both authentication headers	Correct pair succeeds; either wrong credential fails clearly
Prefix URL and redirect handling	Prefix remains intact; no credentials follow a redirect
Quick Launch and dictation	Capture, review, send, re-dictate, and cancel work on PT2
Normal Hermes request	Real response appears on phone and active watch
Follow-up	Remembers the same session; New conversation separates context
Authorized Matrix request	Hermes uses the intended account/tool; resulting message is checked in Matrix
Duplicate/retried watch message	Creates one durable command
Lost HTTP create response	Original key/payload reconcile without a second run
Process death/restart	Accepted run is monitored, not resubmitted
Network/NetBird outage	Completed text remains queued; recovery is explicit
Interrupted/unknown outcome	No automatic repeat of a potentially executed action
Watch app closed during completion	Result is available when reopened
Long Unicode text	Correct chunking/pagination without corruption or silent truncation
Server approval required	Clear blocked state; no automatic approval
Local note	Durable phone save; no unexpected Hermes request
Secret handling	No credentials in repository, APK resources, logs, exports, or watch traffic
Android restrictions	Real-device Doze/restart behavior documented without an instant-delivery claim


Only conduct the real Matrix-send check later with an explicitly chosen test recipient. Do not send any message as part of implementing or documenting this project.
The first-pass completion report must say: “Implementation prepared; builds, tests, workflow execution, CloudPebble execution, and hardware validation were intentionally not run.” Separately list any concrete missing work. Do not label deferred acceptance checks as passed.
17. Primary references
These sources and the pinned snapshots establish the researched contracts. Product choices and local safeguards are requirements of this project. The first pass should implement these contracts, not perform another open-ended search for versions.
- S1: NetBird reverse-proxy authentication
- S2: Hermes API server, authentication, sessions, and run retry semantics
- S3: Pebble dictation
- S4: CloudPebble source and open import changes
- S5: Hermes programmatic integration and API implementation
- S6: PebbleKit Android 2 client
- S7: Android persistent background work
- S8: GitHub workflow triggering
- S9: Pebble hardware platforms
- Pebble communication
- Pebble launch reasons
- S10: AGP 9.2 release compatibility, built-in Kotlin migration, and documented KGP override
- S11: Kotlin Gradle/AGP compatibility table, Gradle Java compatibility, and official Gradle checksums
- S12: KSP 2.3.12 release, Compose BOM POM, Room releases, WorkManager releases, and DataStore releases
- S13: Pebble SDK 4.33.1 changelog and SDK package metadata
- S14: Hermes snapshot: API server and run handlers
- S15: Published PebbleKit 1.3.2 POM, AAR, client source JAR, client API source JAR, and common model source JAR
- S16: CloudPebble snapshot: manifest import/generation, project assembly, and removal of per-project SDK selection
- S17: Official mobile app snapshot: PBW metadata model and Android companion selection
- S18: Dictation C API and persistent storage C API
- S19: Modern native app wscript template
- S20: Action releases: checkout v7.0.1, setup-java v6.0.1, setup-gradle v6.3.0, setup-android v4.0.4, and upload-artifact v7.0.1
18. Exact Android build files and wrapper
These are the required build configuration shapes. Replace the illustrative dev.example.hermespt2 application ID and display name with identifiers chosen from the actual repository, once. Keep debug and release on that same application ID for v1 so watch companion registration matches; do not add an unnoticed .debug suffix. Names and UUIDs are project-specific values, not unresolved dependency versions.
android/settings.gradle.kts:
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "HermesPT2"
include(":app")
android/build.gradle.kts:
buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        // Upgrade AGP's built-in Kotlin; do not apply kotlin-android.
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:2.4.20")
    }
}
plugins {
    id("com.android.application") version "9.2.0" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.20" apply false
    id("org.jetbrains.kotlin.plugin.serialization") version "2.4.20" apply false
    id("com.google.devtools.ksp") version "2.3.12" apply false
}
android/app/build.gradle.kts:
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
    id("com.google.devtools.ksp")
}

android {
    namespace = "dev.example.hermespt2"
    compileSdk {
        version = release(37) {
            minorApiLevel = 0
        }
    }
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "dev.example.hermespt2"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }
    buildFeatures { compose = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildTypes {
        release { isMinifyEnabled = false }
    }
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}
ksp { arg("room.schemaLocation", "$projectDir/schemas") }

dependencies {
    implementation("io.rebble.pebblekit2:client:1.3.2")
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.activity:activity-compose:1.11.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.4")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.4")

    implementation(platform("androidx.compose:compose-bom:2025.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation("androidx.room:room-runtime:2.8.4")
    implementation("androidx.room:room-ktx:2.8.4")
    ksp("androidx.room:room-compiler:2.8.4")
    implementation("androidx.work:work-runtime-ktx:2.10.5")
    implementation("androidx.datastore:datastore-preferences:1.1.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
    implementation("com.squareup.okhttp3:okhttp:5.3.0")
}
Use Room schema export, but do not fabricate generated schema files before the first real compilation. Do not add old composeOptions.kotlinCompilerExtensionVersion, legacy kotlinOptions, Jetifier, JitPack, dynamic versions, or a second Android Kotlin plugin. Use android.useAndroidX=true and org.gradle.jvmargs=-Xmx2g -XX:MaxMetaspaceSize=768m -Dfile.encoding=UTF-8 in android/gradle.properties; omit machine-specific SDK locations. Compiling against SDK 37.0 does not mean the app requires Android 17 to run.
Commit genuine wrapper files without running Gradle. Download these three files from Gradle's immutable release tag v9.4.1:
- gradlew → android/gradlew, executable bit set.
- gradlew.bat → android/gradlew.bat.
- gradle-wrapper.jar → android/gradle/wrapper/gradle-wrapper.jar.
The researched wrapper JAR SHA-256 is:
55243ef57851f12b070ad14f7f5bb8302daceeebc5bce5ece5fa6edb23e1145c
Checking downloaded bytes against this checksum is allowed; invoking the wrapper to generate or validate itself is not. Supply android/gradle/wrapper/gradle-wrapper.properties:
distributionBase=GRADLE_USER_HOME
distributionPath=wrapper/dists
distributionUrl=https\://services.gradle.org/distributions/gradle-9.4.1-bin.zip
distributionSha256Sum=2ab2958f2a1e51120c326cad6f385153bb11ee93b3c216c5fccebfdfbb7ec6cb
networkTimeout=30000
validateDistributionUrl=true
zipStoreBase=GRADLE_USER_HOME
zipStorePath=wrapper/dists
The ZIP checksum differs from the wrapper JAR checksum. Both were checked against Gradle's published values [S11]. If downloads are blocked in the agent environment, report the specific missing authentic file; never create a text file named .jar or claim wrapper completeness.
19. Exact Pebble project metadata and native build shape
Use the following root package shape. Replace the name, author, example UUID, example Android package, and repository URL consistently with actual project values. Generate the real UUID once. Fill messageKeys with the finalized numeric protocol table; preserve those mappings across CloudPebble import.
{
  "name": "hermes-pt2",
  "author": "Repository owner",
  "version": "0.1.0",
  "keywords": ["pebble-app"],
  "private": true,
  "dependencies": {},
  "pebble": {
    "displayName": "Hermes PT2",
    "uuid": "11111111-2222-4333-8444-555555555555",
    "sdkVersion": "3",
    "enableMultiJS": false,
    "targetPlatforms": ["emery"],
    "watchapp": { "watchface": false },
    "messageKeys": {
      "ProtocolVersion": 0,
      "MessageKind": 1,
      "TransferId": 2,
      "CaptureId": 3,
      "ChunkIndex": 4,
      "ChunkCount": 5,
      "Payload": 6,
      "Status": 7,
      "ErrorCode": 8
    },
    "resources": { "media": [] },
    "companionApp": {
      "android": {
        "url": "https://github.com/OWNER/REPOSITORY",
        "apps": [{ "package": "dev.example.hermespt2" }]
      }
    }
  }
}
Use these key numbers, extending with higher numbers only if needed. Spell out tuple types and operation values in protocol/README.md; both implementations must agree. No pkjs, pebblejs, Clay, npm install, or JavaScript build is necessary. sdkVersion is a manifest family, not an SDK installation pin. The Android package entry must match the APK's final application ID.
Use the native build pattern from the modern upstream template [S19], including pbl_build(..., bin_type='app'). Do not copy an obsolete template that guesses a pbl_program method:
top = '.'
out = 'build'

def options(ctx):
    ctx.load('pebble_sdk')

def configure(ctx):
    ctx.load('pebble_sdk')

def build(ctx):
    ctx.load('pebble_sdk')
    binaries = []
    saved_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(
            source=ctx.path.ant_glob('src/c/**/*.c'),
            target=elf,
            bin_type='app',
        )
        binaries.append({'platform': platform, 'app_elf': elf})
    ctx.env = saved_env
    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries, js=[])
CloudPebble may generate its own equivalent build script; do not depend on a custom wscript hook surviving import. Allocate 1,024-byte AppMessage input/output buffers for this PT2-only app, check app_message_open for success, and use at most 192 payload bytes per chunk. Calculate dictionary overhead for the final tuple set before coding it. The 1,024-byte transcript limit is a logical-message limit, not permission to send it in one AppMessage. Chunk large response pages too.
20. Exact manual GitHub Actions workflow
Create .github/workflows/android.yml using these release-tag-resolved immutable commits [S20]. No automatic push or PR trigger. Actions run from the repository root; only shell steps inherit android/ as their working directory.
name: Android APK
on:
  workflow_dispatch:

permissions:
  contents: read

jobs:
  android:
    runs-on: ubuntu-24.04
    timeout-minutes: 30
    defaults:
      run:
        working-directory: android
    steps:
      - name: Checkout
        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false

      - name: Set up Java
        uses: actions/setup-java@de7274f081f381c8f8158605e0321c36c376e2e6 # v6.0.1
        with:
          distribution: temurin
          java-version: '21'

      - name: Set up Android SDK
        uses: android-actions/setup-android@be39fa834029ff78f1a44aa3bb0819b8fc2bd8fd # v4.0.4
        with:
          cmdline-tools-version: '15859902'
          packages: 'platform-tools platforms;android-37.0 build-tools;36.0.0'
          log-accepted-android-sdk-licenses: 'false'

      - name: Set up Gradle
        uses: gradle/actions/setup-gradle@9c971963bec38e04b3d30dcc455b5382be2fdbfb # v6.3.0

      - name: Build debug APK
        run: ./gradlew --no-daemon :app:assembleDebug

      - name: Upload APK
        uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1
        with:
          name: hermes-pt2-android-debug
          path: android/app/build/outputs/apk/debug/app-debug.apk
          if-no-files-found: error
          retention-days: 14
Leave Gradle installation to the committed wrapper. Do not also enable setup-java's Gradle cache. Do not publish build scans or submit dependency graphs from this first workflow. It needs no service secrets. Do not run or dispatch it during implementation. A later workflow failure should be diagnosed against its actual log, without silently replacing these pins or moving watch compilation into CI.
