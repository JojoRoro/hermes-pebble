# Hermes Pebble

Hermes Pebble is a Pebble Time 2 watch app paired with a standalone Android companion for an existing Hermes server. Hermes owns Matrix integration and sends messages with the authorization already configured on that server. This project does not log into Matrix, replace the official Pebble Android app, or add another backend.

The watch side uses the native Pebble protocol and standard Pebble dictation. The phone side provides durable commands, Hermes run reconciliation, recent history, local notes, and connection settings. The official Pebble app remains the Bluetooth and phone-side host.

## Repository scope

The Pebble project is rooted at the repository root so CloudPebble can import it without a subdirectory importer. Android is a nested Gradle project. The implementation brief and the protocol document are the source of truth for the contracts.

| Component | Identity or pin |
| --- | --- |
| Watch app | Hermes Pebble, Pebble Time 2, `emery` |
| Watch UUID | `7d07aa22-7d13-48c1-a400-2602a5ae4647` |
| Android application ID and namespace | `dev.hermespebble.companion` |
| Android minimum/target SDK | 26 / 36 |
| Android compile SDK | 37.0 |
| PebbleKit Android client | `io.rebble.pebblekit2:client:1.3.2` |
| Repository | `https://github.com/JojoRoro/hermes-pebble` |

The root `package.json` is the source of truth for the watch UUID and companion declaration. The Android Gradle files are pinned to the researched AGP, Kotlin, KSP, and library baseline in `IMPLEMENTATION.md`. Release tags build both the Android APK and the watch PBW.

## Get started

1. Download `hermes-pebble.apk` and `hermes-pebble-store.pbw` from the [latest release](https://github.com/JojoRoro/hermes-pebble/releases/latest).
2. Install the APK on Android and open the PBW with the official Pebble phone app. Once published, the Pebble app-store listing can supply the watch app instead.
3. Open Android Setup and enter your Hermes API root and key. Enable the additional access header only if your proxy requires it. Save and test the connection.
4. The official Pebble host is selected automatically when it is the only eligible host. Otherwise select it in Setup. Open the watch app and confirm the link.
5. Choose **Ask Hermes**, dictate, review, and **Send**. Keep that screen open: progress changes to the answer automatically. Select **Reply to Hermes** from the answer's actions to continue the same conversation. **New conversation** starts a separate context.

Long answers page with Up/Down, one screen per press with a line of overlap. The watch shows your request above the answer, vibrates once when an awaited answer arrives (not during Quiet Time), and loads further text only as you read toward the end, so unread parts of an answer are not sent over Bluetooth. Your reading position is preserved as more text arrives. Very long answers load adjacent text at either scroll boundary using the same buttons. Fetch remains available for recovering an answer later. Test connection only checks the API; sending a request is a separate action.

Hermes requests use the asynchronous run API: Android submits once, then polls that run until it finishes. Polls are scheduled about every 3 seconds while the Android app is in the foreground, 30 seconds in the background, or 5 minutes when approval is required. Having the watch app open does not select the Android foreground interval. Network failures, Doze, and Android background restrictions can delay those checks. Once complete, Android stores the answer and notifies the active watch; fetching further text reads that saved answer, without another Hermes run.

## Hermes prerequisites

The server administrator must enable the Hermes HTTP API and provide an API profile with a credential. The profile used for API requests must have the tools required by the request enabled. A tool visible in a Matrix-connected Hermes chat is not automatically available to the API profile; enable the relevant tool for API requests and configure its approval policy separately.

The companion does not store Matrix credentials. Hermes uses its existing account and tools. A completed Hermes answer is displayed as the answer, not converted into an invented delivery receipt. Server-side approval requests remain blocked until the user acts in Hermes; this project does not automatically approve or weaken policies.

## Reply context on Hermes 0.19.0

The pip release `hermes-agent==0.19.0` accepts `session_id` on `/v1/runs` but does not load that session's earlier messages. Two requests can therefore show the same **Conversation** ID while Hermes forgets the first exchange. This is the [upstream history-loading bug](https://github.com/NousResearch/hermes-agent/issues/69204).

Version 0.1.7 detects that release through the authenticated `/health` request and includes recent completed message/answer pairs with follow-ups. Existing locally retained conversations work after an in-place APK update. **Test saved Hermes connection** reports **Reply context compatibility enabled** when active. The first submission after upgrading also refreshes older cached capabilities automatically.

The workaround includes up to 20 completed exchanges from the same connection profile and conversation, with a 64 KiB text budget. It uses the text retained by this companion; cleared history and full Hermes tool traces are unavailable. Oversized recent text is marked as shortened. **New conversation** starts without previous exchanges. Other server versions keep server-managed context. Version detection refreshes at least daily during use and immediately when testing the saved connection.

## Runtime connection settings

Enter all connection settings on the phone. They are runtime data, not CI variables or source configuration.

- Hermes server URL: an HTTPS API root, optionally including a path prefix, with no embedded credentials, query, or fragment.
- Hermes API key: sent only as `Authorization: Bearer <key>`.
- Additional access header: independently enabled or disabled, with a user-editable token name and a separate masked value.
- Suggested header name: `X-NetBird-Access`. It is an example, not a required NetBird standard.
- Header value: sent exactly as entered, without an automatic `Bearer` prefix.
- Test connection: read-only authenticated capability inspection.

Both credentials are attached to every Hermes request when enabled. The URL is the API root, not a chat-completions endpoint. For example, a configured `https://example.invalid/hermes/` root must retain `/hermes/` when `/v1/runs` is appended.

## Connection diagnostics and credential editing

Android **Setup** selects `coredevices.coreapp` automatically when it is the sole eligible Pebble host. If multiple hosts are installed, select the intended one. PebbleKit rejects incoming watch messages until a host is selected. Close and reopen Hermes on the watch after changing the host, or choose **Settings › Reconnect phone** on the watch. Install a finalized PBW so the Pebble host knows which Android companion to contact.

The API key and additional header value load from encrypted storage. Tap either field to reveal and edit the saved value; leaving the field masks it again. Save edits before running **Test saved Hermes connection**. Saving unchanged values preserves the current connection profile.

Android **Diagnostics** provides a read-only API test, an **Open watch app & test link** test, network/battery status, and a copyable timestamped event log. HTTP events show status, JSON/HTML response type, size, and elapsed time; schema errors identify missing fields. DNS, TLS, timeout, and authentication failures are distinguished. A 401/403 still cannot identify which authentication layer rejected the request. Credentials, response bodies, and dictated text are excluded from the log; the in-app log retains the last 150 events in memory, and the same metadata is written to Android logcat under `HermesLink`. The watch test opens Hermes through the selected Pebble host and checks a correlated round trip, with separate start, delivery, and response diagnostics. Install the matching watch PBW before testing.

For a watch timeout, open Diagnostics and choose Settings › Reconnect phone on the watch. No RX event points to host selection, companion metadata, or host permissions. RX followed by a failed TX identifies the reply transport failure. TX Success confirms transport delivery, not a completed Hermes run. For API failures behind NetBird, HTTP 200 with HTML indicates a dashboard or proxy page instead of the expected JSON API response. Copy the report after reproducing the failure.

## Watch speaker test

Install the matching APK and PBW, open Hermes on Pebble Time 2, then tap **Diagnostics → Play test sound on watch** in Android. The watch should say **“Hello from your Pebble”** after the transfer finishes. A sample is bundled; no audio file, Hermes connection, or speech API is needed. The button checks that the watch app is already open and does not launch it.

Android shows transfer progress and waits for the watch's speaker completion callback. If the speaker is muted or busy, it reports that instead. Check the watch's Sounds & Haptics and Quiet Time settings if muted. Back or leaving the watch app stops the test. This is a short playback experiment; spoken Hermes replies and cycling detection are not yet connected. See [audio test details](docs/audio-test.md).

From v0.1.10, **Quiet Time always blocks audio**, independently of the system speaker-mute preference. Turning it on during a transfer or playback discards the clip and stops playback. Turn Quiet Time off and start a new test to play audio again.

## Bike detection test (experimental)

On the watch, open **Settings › Bike detection test**. **NEW** and **OLD** scores appear side by side on the same live sensor stream. Allow about 4 seconds for the first window and 12–20 seconds for sustained evidence. **Down** shows feature details. **Select** resets evidence; **Back** leaves and releases the sensors. After 10 minutes the test pauses to save battery; **Select** resumes it.

The new score requires continuous vibration across short blocks and repeated windows, reduces impacts and wrist swings, and treats recent steps as a walking cue. It **never uses heart rate**, so low BPM during an easy ride is not a penalty. The unchanged old score still gets its optional fresh-HR bonus; both ignore the watch's own haptics. The test keeps the old 15-second HR request for comparison and restores automatic sampling on exit.

If typing still scores high, press **Up** and type normally for the 20-second desk-learning session. Similar motion then suppresses the new score. The baseline survives Select and auto-pause within this test; leaving discards it. **Hold Up** clears it. Compare rides with and without the baseline, since overlapping desk/ride motion can suppress real cycling too.

These are heuristic scores, not measured probabilities. Rough car/bus travel or hand shaking can look similar; smooth roads, a loose watch, or hands off the handlebars can produce low scores. No sensor data leaves the watch or is saved. Neither score activates any feature. See [sensor assessment, algorithm, and field-test details](docs/bike-detection.md).

## NetBird Custom Header

When a NetBird reverse proxy protects the Hermes service, configure its Custom Header option with the exact header name and value entered in the companion. NetBird uses that header for service access and removes the matching access header before forwarding upstream. The Hermes Authorization header remains separate; using NetBird's Authorization preset would compete with Hermes authentication.

The access-header value is a service-access secret, not a NetBird management API token. Supplying a header does not create a VPN or make a private endpoint reachable. If the Hermes endpoint is private, the phone must also have a working NetBird connection. A 401 or 403 alone cannot identify whether NetBird or Hermes rejected the request; inspect the proxy and Hermes diagnostics separately without logging either secret.

Header names must be valid HTTP tokens. Control characters, line breaks, case-insensitive collisions with protocol-owned headers, `Authorization`, `Host`, `Content-Length`, `Content-Type`, `Accept`, `Connection`, `Transfer-Encoding`, `Idempotency-Key`, `X-NetBird-User`, and `X-NetBird-Groups` are rejected. Values are validated, not trimmed into a different secret. Authenticated redirects are rejected so credentials are never forwarded to another origin or a browser login page.

## Install routes

### Android APK

Download `hermes-pebble.apk` from the [latest GitHub Release](https://github.com/JojoRoro/hermes-pebble/releases/latest) and sideload it through the phone's normal APK installation flow.

The **App Release** workflow builds version tags matching `v*` and publishes both the signed APK and a verified PBW. Manual runs upload `hermes-pt2-android-release` and `hermes-pebble-store` Actions artifacts without publishing a release. Builds do not contact Hermes or need Hermes/proxy credentials. See [publishing instructions](docs/publishing.md).

Starting with v0.1.3, CI publishes `hermes-pebble.apk`, a release APK signed with one permanent key for `dev.hermespebble.companion`. It restores the PKCS12 key from `ANDROID_SIGNING_KEY_BASE64` and its password from `ANDROID_SIGNING_PASSWORD` repository Actions secrets. Missing secrets fail the build; there is no temporary-key fallback. The uploaded APK certificate must match `.github/android-signing-cert.sha256`. Local debug builds still use a developer debug key and cannot replace an installed release build.

Versions through v0.1.2 used disposable CI debug keys. Their private keys were not retained, so migrating requires a one-time uninstall/reinstall. Uninstalling clears local settings, credentials, notes, and history: save anything needed first. Subsequent releases with the permanent key and increasing version codes support in-place updates. See [Android signing maintenance](docs/android-signing.md).

### Pebble PBW

Use `hermes-pebble-store.pbw` from the same GitHub release as the APK. It contains the companion declaration needed by the Pebble phone app and is ready to upload to the Pebble app store. End users do not need to build or modify it.

CloudPebble currently loses that declaration while importing/building. Its direct-install button can install an app that opens but cannot contact Android. [CloudPebble details](docs/cloudpebble.md) include an upstream patch and a manual finalizer for developers who use the hosted builder. The release workflow runs the SDK and metadata verification automatically.

## Quick Launch

Assign a Quick Launch button to Hermes Pebble in the official Pebble app's normal settings. When the watch reports the Quick Launch launch reason, the app starts the standard Pebble dictation flow. The resulting transcript is shown for review before any request is submitted. The user can save it as a local note, dictate again, send it after review, or cancel. Normal launcher entry exposes the menu instead of assuming a Quick Launch reason.

Quick Launch still depends on the phone's configured Pebble dictation service. It is not offline speech recognition and does not store microphone audio.

## Handwritten notes (experimental)

Open **Handwritten note** in the watch menu, whether connected or offline. Draw one character in the box. After you lift your finger and pause for one second, the canvas slides left for the next character. Add multiple strokes before that pause for letters such as `i` or `t`.

- **Up:** undo; after an advance, the first press reopens the previous character.
- **Down:** insert a word space.
- **Select:** save on the watch and start syncing if the phone is connected.
- **Back:** continue, save, or discard the current drawing.

Open **Local notes** on Android to view the handwriting, including a larger zoom. This works without Hermes credentials. **Allow note sync notifications** enables an alert that opens the received note. Handwriting is stored as strokes, without recognition or automatic submission to Hermes.

This first version supports **one pending handwritten note**. All watch persistence stays within a 4 KiB budget: the drawing gets up to 2,048 bytes, leaving room for the existing pending transcript, settings, and record overhead. Length depends on stroke complexity; the watch shows the remaining space as a percentage. Save before leaving the app: unfinished drawings exist only in memory.

The saved copy survives app restarts. The watch tries direct delivery immediately and after reconnection while the app is open. A Data Logging copy also allows background delivery through a compatible Pebble host. If background delivery is delayed, reopen Hermes on the watch. The watch frees the pending slot only after a direct phone receipt confirms durable storage; reopen the watch app if a note has reached the phone but its watch slot is still occupied. See [the storage and transfer format](protocol/README.md#handwritten-notes).

Touch capture requires compatible PT2 firmware exposing the SDK 4.33.1 touch API. The drawing area uses raw touch independently of the app's menu touch-navigation preference. Finger drawing quality and background reconnect behavior still need testing on physical devices.

## Android and GrapheneOS behavior

The companion does not require Google Play Services, microphone permission, contacts, notification-listener access, accessibility access, or direct Bluetooth permission. Result notifications are optional and can use private lock-screen content.

On GrapheneOS or another restricted Android phone, check the app's network permission, NetBird connectivity, battery policy, and background-activity settings when a request remains queued. Doze, app standby, force-stop, process death, and OS background limits can delay WorkManager execution; reopening the app explicitly requests reconciliation. The project does not promise instant delivery or keep a permanent foreground service alive. Do not grant blanket battery exemptions during onboarding unless a device-specific diagnosis requires one.

## v1 limitations

- One dedicated Hermes conversation is used for the watch flow. New conversation starts a separate context; it does not select an arbitrary Matrix session.
- The watch stores a bounded transcript and transfer state, not raw audio. Standard dictation must work through the phone.
- Hermes tool availability, authorization, approvals, and external-action delivery remain server-side responsibilities.
- The companion is not a Matrix client, contact directory, room browser, Element X replacement, or full notification bridge.
- There is no watch TTS, offline transcription, streaming-token UI, full watch inbox, or complete approval-management UI.
- A stopped Hermes run is not an undo operation. A completed HTTP run is not proof of Matrix delivery.
- Official Pebble notification forwarding into a closed watch app is a user setting and is not guaranteed.
- Background execution is best effort under Android and GrapheneOS restrictions.

## Validation status

[Validation results](docs/validation.md) distinguish emulator/build checks from physical-device checks. The user confirmed that installing the finalized PBW fixed watch routing and correcting the API key fixed Hermes authentication. Version 0.1.6 adds handwritten notes with bounded watch storage, duplicate-safe phone delivery, and a local viewer. Host tests cover drawing/storage and the binary format; emulator checks cover navigation and the existing conversation flow. Live Hermes and physical-watch verification of the new automatic flow remain device checks.

## Watch navigation

Up/Down page through text and move through menus; holding a button repeats. A blue tab at the right edge marks screens where **Select** opens actions, and the header dot shows whether the phone link is verified. **Settings** in the main menu holds **Touch navigation**, **Reconnect phone**, and the **Bike detection test**. Select **Touch navigation** to toggle touchscreen navigation. The preference survives restarts and buttons remain available in both modes. Touch navigation defaults to On and requires compatible watch firmware with system touch navigation enabled. Build the watch with SDK 4.33.1 or newer to include the touch API.

Android's **Open watch app & test link** waits after requesting launch and retries `FailedDifferentAppOpen` up to eight times. A successful launch request alone does not mean the watch is ready to receive a probe. Persistent failures after these retries are reported separately from a successful round trip.
