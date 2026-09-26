# Android setup

This guide covers installing and configuring the Android companion. The Android APK build has passed in GitHub Actions; device and live-server validation remain pending.

## Prerequisites

Before installation, obtain:

- An Android phone with the official Pebble app installed and unchanged.
- A Pebble Time 2 and a functioning Pebble dictation service through that official phone app.
- An HTTPS Hermes server with its HTTP API enabled.
- An API profile and API credential authorized for the tools the user intends to call.
- A NetBird connection and reverse-proxy Custom Header configuration when Hermes is behind NetBird.

The companion targets application ID and namespace `dev.hermespebble.companion`, minimum SDK 26, target SDK 36, and compile SDK 37.0. It does not require Google Play Services.

## First-run no-send procedure

The first run is configuration-only and no-send. Do not send during that first run, even after the read-only check succeeds. Do not submit a request, invoke a live test recipient, or use Quick Launch as a send test until all of the following are complete:

1. Open the official Pebble app and confirm its PebbleKit 2 service is available.
2. Install and open Hermes Pebble.
3. Enter the Hermes API root and API credential.
4. Configure the independent NetBird access header if the service requires it.
5. Select the official Pebble phone host rather than trusting the first package that advertises a service.
6. Run the read-only Test connection action.
7. Confirm the application handshake and connection diagnostics show a supported Hermes route and API profile.

Setup and Test connection must not send a Matrix message. A local note is saved only on the phone. Choose a test recipient explicitly for the later live Matrix acceptance check.

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

## Sideloading the APK

Download `hermes-pebble-debug.apk` from the [latest GitHub Release](https://github.com/JojoRoro/hermes-pebble/releases/latest) and install it using the phone's normal sideload flow. Each version tag matching `v*` builds the tagged source and attaches this APK to a GitHub Release after a successful build.

The workflow can also be dispatched manually from the Actions UI. Manual runs upload the `hermes-pt2-android-debug` Actions artifact without publishing a release. The artifact path is `android/app/build/outputs/apk/debug/app-debug.apk` within the workflow workspace.

The first APK is for sideloading, not Play Store distribution. Keep the same application ID for updates. A CI debug key generated on a disposable runner can change, so it is not a stable update-signing identity. A later signing configuration may provide a protected stable keystore through GitHub Actions secrets; do not commit the keystore, expose its passwords, or change the application ID as a signing workaround.

## Publishing a version

Update `versionName` and increment `versionCode` in `android/app/build.gradle.kts`, then commit and push the change. Create and push an annotated tag matching the app version; for example, for version 0.1.1:

```sh
git tag -a v0.1.1 -m "Hermes Pebble v0.1.1"
git push origin v0.1.1
```

The tag push runs the Android build and creates a release with `hermes-pebble-debug.apk` attached. Tags containing a hyphen, such as `v0.2.0-beta.1`, produce prereleases. Rerunning a tag workflow updates the APK on that release. The build job has read access; only the publish job can write releases through GitHub's automatic workflow token.

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
