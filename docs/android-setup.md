# Android setup

This guide covers installing and configuring the Android companion. Install the signed APK and matching watch PBW from the latest release; the two apps share one dedicated Hermes conversation.

## Prerequisites

Before installation, obtain:

- An Android phone with the official Pebble app installed and unchanged.
- A Pebble Time 2 and a functioning Pebble dictation service through that official phone app.
- An HTTPS Hermes server with its HTTP API enabled.
- An API profile and API credential authorized for the tools the user intends to call.
- A NetBird connection and reverse-proxy Custom Header configuration when Hermes is behind NetBird.

The companion targets application ID and namespace `dev.hermespebble.companion`, minimum SDK 26, target SDK 36, and compile SDK 37.0. It does not require Google Play Services.

## First-run setup

1. Install the official Pebble phone app and pair the watch.
2. Install `hermes-pebble.apk` and open `hermes-pebble-store.pbw` with the Pebble phone app. Both are on the [latest release](https://github.com/JojoRoro/hermes-pebble/releases/latest). A published Pebble app-store listing can replace the PBW installation step.
3. Open Hermes Pebble on Android. The official host (`coredevices.coreapp`) is selected automatically if it is the only eligible host; select the intended host when more than one is available.
4. Enter the HTTPS Hermes API root and API key, without adding `Bearer `. Configure the separate access header only if required by your proxy. Save the settings.
5. Run **Test saved Hermes connection**, then **Open watch app & test link** in Diagnostics.
6. On the watch, choose **Ask Hermes**, dictate, review, and **Send**. Progress changes to the answer automatically while that screen is open. Press Select and choose **Reply to Hermes** to continue the conversation.

Setup and Test connection make no Hermes run or Matrix send. An intentional Send submits the reviewed request. Local notes stay on the phone. If you leave the conversation screen before completion, use Recent to retrieve the answer later.

## Hermes API profile prerequisites

The Hermes server's HTTP API must be enabled. The API profile used by this companion needs the relevant tools enabled for API requests; a tool available in a Matrix chat does not automatically belong to the API profile. Confirm tool credentials, allowed targets, and approval policy with the Hermes administrator.

The companion does not log in to Matrix, read Element X data, or store Matrix credentials. Hermes performs Matrix work with the server's existing authorization. If a run waits for approval, the phone and watch show an approval-needed state and do not auto-approve it.

## Connection settings

Set the server URL to the Hermes API root, not a chat-completions URL. An optional path prefix is preserved. Do not include credentials, a query string, or a fragment. The Hermes API key is sent as `Authorization: Bearer <key>`.

The additional access header is separate and can be disabled without disabling Hermes authentication. Its name must be a valid HTTP token. The value is sent exactly as entered; the app must not add a Bearer prefix or trim the secret into a different value. Reject control characters, line breaks, reserved protocol headers, and case-insensitive collisions. In particular, do not use `Authorization` for the NetBird value.

The Test connection action performs a read-only authenticated capability check. A 401 or 403 does not reveal whether NetBird or Hermes rejected the request. A redirect, HTML login page, missing route, invalid JSON, TLS failure, or network failure is reported as a configuration or transport problem rather than treated as success.

## NetBird Custom Header

In the NetBird reverse proxy configuration, enable the Custom Header option and enter the same name and value configured in the companion. For example, a name such as `X-NetBird-Access` may be used, but the name is not a required NetBird standard. NetBird uses the matching header for access control and strips it before forwarding to Hermes.

The value is a NetBird service-access secret, not a NetBird management API token. A custom header does not create a VPN connection. The phone must have a working NetBird connection when the Hermes API is on a private network. Keep the header independent from `Authorization`; using NetBird's Authorization preset would compete with Hermes authentication.

## Sideloading and updating

Download `hermes-pebble.apk` from the [latest GitHub Release](https://github.com/JojoRoro/hermes-pebble/releases/latest), or use Obtainium to track this repository. Install it using Android's normal APK installation flow. Install the matching `hermes-pebble-store.pbw` through the Pebble phone app as well; Obtainium updating the APK does not update the watch app.

Since v0.1.3, release APKs share one permanent signing key, so later versions install as updates and keep settings. Releases through v0.1.2 used disposable debug keys and require a one-time uninstall/reinstall, which clears local data. A developer debug APK cannot replace a signed release APK in place. See [signing maintenance](android-signing.md).

Manual **App Release** workflow runs upload the signed APK and verified PBW as Actions artifacts. A pushed version tag also publishes both to GitHub Releases.

## Publishing a version

See [publishing instructions](publishing.md) for matching version numbers, tag-triggered builds, and the PBW to upload to the Pebble app store. Keep the existing application ID, watch UUID, and permanent signing key for updates.

## Runtime secret handling

Enter the Hermes API key and NetBird access-header value only in the running app's masked settings. They must be encrypted at rest, excluded from backups and exports, omitted from logs and exception text, and absent from source, Gradle properties, APK resources, watch messages, screenshots, and CI configuration. Disabling the access header must not disable Hermes authentication.

## Android and GrapheneOS troubleshooting

When a request is queued or a result is late:

- Confirm the phone has network access to the Hermes API and, when applicable, an active NetBird connection.
- Check the app's GrapheneOS network permission and any per-app VPN or battery settings.
- Reopen the app to request reconciliation; do not force-stop it as a routine delivery mechanism.
- Expect Doze, app standby, process death, and OS background limits to delay WorkManager execution.
- Do not assume a result notification means the closed watch app received data.
- Use private lock-screen notification content if result notifications are enabled.

The project does not require blanket battery exemptions and does not promise instant delivery. It avoids a permanent foreground service and uses bounded background work.

## Privacy and limitations

The app needs no microphone, contacts, notification-listener, accessibility, or direct Bluetooth permission merely to relay Pebble dictation. Dictation is performed by the normal phone integration; the companion does not retain raw audio. Hermes answers, tool traces, Matrix delivery, and external side effects remain server concerns. The watch is limited to a reviewable transcript, bounded transfer pages, status, and follow-up in one dedicated conversation. See `docs/architecture.md` and `docs/validation.md` for the full contract and deferred checks.
