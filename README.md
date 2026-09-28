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
| Repository | `https://github.com/hermes-pebble/hermes-pebble` |

The root `package.json` is the source of truth for the watch UUID and companion declaration. The Android Gradle files are pinned to the researched AGP, Kotlin, KSP, and library baseline in `IMPLEMENTATION.md`. No version research or generated Room schema is included in this pass.

## Safety rule for first run

The first-run setup is deliberately configuration-only and no-send. Configure the HTTPS Hermes API root, Hermes API credential, and the optional NetBird access header, then use only the read-only Test connection action. Do not send during that first run, even after the check succeeds: do not press Send, dictate a request intended for Hermes, or invoke Quick Launch with a live test recipient.

A successful setup or capability check is not permission to send a Matrix message. The later Matrix acceptance check must use an explicitly chosen test recipient. Local notes do not invoke Hermes. A transport acknowledgment is not proof that Hermes completed a run or that an external action was delivered.

## Hermes prerequisites

The server administrator must enable the Hermes HTTP API and provide an API profile with a credential. The profile used for API requests must have the tools required by the request enabled. A tool visible in a Matrix-connected Hermes chat is not automatically available to the API profile; enable the relevant tool for API requests and configure its approval policy separately.

The companion does not store Matrix credentials. Hermes uses its existing account and tools. A completed Hermes answer is displayed as the answer, not converted into an invented delivery receipt. Server-side approval requests remain blocked until the user acts in Hermes; this project does not automatically approve or weaken policies.

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

In Android **Setup**, first select the Pebble phone host. PebbleKit rejects incoming watch messages until a host is selected. Close and reopen Hermes on the watch after changing the host, or choose **Reconnect** in the watch menu. Install a finalized PBW so the Pebble host knows which Android companion to contact.

The API key and additional header value load from encrypted storage. Tap either field to reveal and edit the saved value; leaving the field masks it again. Save edits before running **Test saved Hermes connection**. Saving unchanged values preserves the current connection profile.

Android **Diagnostics** provides a read-only API test, an **Open watch app & test link** test, network/battery status, and a copyable timestamped event log. HTTP events show status, JSON/HTML response type, size, and elapsed time; schema errors identify missing fields. DNS, TLS, timeout, and authentication failures are distinguished. A 401/403 still cannot identify which authentication layer rejected the request. Credentials, response bodies, and dictated text are excluded from the log; the in-app log retains the last 150 events in memory, and the same metadata is written to Android logcat under `HermesLink`. The watch test opens Hermes through the selected Pebble host and checks a correlated round trip, with separate start, delivery, and response diagnostics. Install the matching watch PBW before testing.

For a watch timeout, open Diagnostics and choose Reconnect on the watch. No RX event points to host selection, companion metadata, or host permissions. RX followed by a failed TX identifies the reply transport failure. TX Success confirms transport delivery, not a completed Hermes run. For API failures behind NetBird, HTTP 200 with HTML indicates a dashboard or proxy page instead of the expected JSON API response. Copy the report after reproducing the failure.

## NetBird Custom Header

When a NetBird reverse proxy protects the Hermes service, configure its Custom Header option with the exact header name and value entered in the companion. NetBird uses that header for service access and removes the matching access header before forwarding upstream. The Hermes Authorization header remains separate; using NetBird's Authorization preset would compete with Hermes authentication.

The access-header value is a service-access secret, not a NetBird management API token. Supplying a header does not create a VPN or make a private endpoint reachable. If the Hermes endpoint is private, the phone must also have a working NetBird connection. A 401 or 403 alone cannot identify whether NetBird or Hermes rejected the request; inspect the proxy and Hermes diagnostics separately without logging either secret.

Header names must be valid HTTP tokens. Control characters, line breaks, case-insensitive collisions with protocol-owned headers, `Authorization`, `Host`, `Content-Length`, `Content-Type`, `Accept`, `Connection`, `Transfer-Encoding`, `Idempotency-Key`, `X-NetBird-User`, and `X-NetBird-Groups` are rejected. Values are validated, not trimmed into a different secret. Authenticated redirects are rejected so credentials are never forwarded to another origin or a browser login page.

## Install routes

### Android APK

Download `hermes-pebble-debug.apk` from the [latest GitHub Release](https://github.com/JojoRoro/hermes-pebble/releases/latest) and sideload it through the phone's normal APK installation flow.

The Android workflow builds version tags matching `v*` and publishes the APK as a GitHub Release asset. Manual runs from the Actions UI upload the `hermes-pt2-android-debug` artifact. The authentic Gradle 9.4.1 wrapper files are included; the wrapper JAR checksum was verified during review. The workflow does not compile a watch app, call Hermes, or need any service secret. See [release instructions](docs/android-setup.md#publishing-a-version) for creating a new version.

Starting with v0.1.3, CI publishes `hermes-pebble.apk`, a release APK signed with one permanent key for `dev.hermespebble.companion`. It restores the PKCS12 key from `ANDROID_SIGNING_KEY_BASE64` and its password from `ANDROID_SIGNING_PASSWORD` repository Actions secrets. Missing secrets fail the build; there is no temporary-key fallback. The uploaded APK certificate must match `.github/android-signing-cert.sha256`. Local debug builds still use a developer debug key and cannot replace an installed release build.

Versions through v0.1.2 used disposable CI debug keys. Their private keys were not retained, so migrating requires a one-time uninstall/reinstall. Uninstalling clears local settings, credentials, notes, and history: save anything needed first. Subsequent releases with the permanent key and increasing version codes support in-place updates. See [Android signing maintenance](docs/android-signing.md).

### Pebble PBW

Open CloudPebble, import the repository root, select the intended branch, and target Pebble Time 2/`emery`. CloudPebble manages the hosted SDK. The manifest keeps `"sdkVersion": "3"`; `4.33.1` is the reference SDK release, not a per-project pin. Build manually, download the PBW, and run the standard-library finalizer locally:

```sh
python3 tools/finalize_pbw.py --package package.json --input downloads/hermes-pt2.pbw --output downloads/hermes-pt2-ready.pbw
```

Install the `-ready.pbw` through the ordinary Pebble file-install flow. Do not use CloudPebble's direct-install button for an unfinalized PBW because it bypasses the companion metadata correction. Keep GitHub as the source of truth; CloudPebble exports must not replace the root `package.json` declaration.

The finalizer restores only the PBW's top-level `companionApp`, checks the UUID, rejects duplicate archive members, and verifies every other member's uncompressed bytes. It refuses an existing or in-place output and removes an incomplete output on failure. `docs/cloudpebble.md` contains the complete sequence.

## Quick Launch

Assign a Quick Launch button to Hermes Pebble in the official Pebble app's normal settings. When the watch reports the Quick Launch launch reason, the app starts the standard Pebble dictation flow. The resulting transcript is shown for review before any request is submitted. The user can save it as a local note, dictate again, send it after review, or cancel. Normal launcher entry exposes the menu instead of assuming a Quick Launch reason.

Quick Launch still depends on the phone's configured Pebble dictation service. It is not offline speech recognition and does not store microphone audio.

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

`docs/validation.md` records the passing local regression checks and Android APK build, plus the remaining CloudPebble and device acceptance checks.

Implementation reviewed and local regression checks passed. The [Android workflow passed on 26 September 2026](https://github.com/JojoRoro/hermes-pebble/actions/runs/36271723438), building and uploading the debug APK. Version 0.1.1 also builds an emery PBW locally and has emulator screenshots checked. The first installed version reported connection failures; physical watch routing and live NetBird/Hermes compatibility still require verification using the new diagnostics.

## Watch navigation

Up/Down scroll text and move through menus; holding a button repeats. In the watch main menu, open **Settings** and select **Touch navigation: On/Off** to toggle touchscreen navigation. The preference survives restarts and buttons remain available in both modes. Touch navigation defaults to On and requires compatible watch firmware with system touch navigation enabled. Build the watch with SDK 4.33.1 or newer to include the touch API.

Android's **Open watch app & test link** waits after requesting launch and retries `FailedDifferentAppOpen` up to eight times. A successful launch request alone does not mean the watch is ready to receive a probe. Persistent failures after these retries are reported separately from a successful round trip.
