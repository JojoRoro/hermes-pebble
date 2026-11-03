# Publishing Hermes Pebble

The public installation consists of the watch app, the standalone Android APK, and the user's own Hermes server settings. Users do not need CloudPebble, Python, or a local compiler.

## Release both apps together

1. Set the same version in root `package.json` and Android `versionName` in `android/app/build.gradle.kts`. Increase Android `versionCode`.
2. Run the regression checks and builds described in [tests](../tests/README.md). Commit and push the intended changes.
3. Check remote tags and create an unused annotated tag matching the version, such as `v0.1.5`. Push that tag; a normal branch push does not trigger the release workflow. Never move an existing release tag.
4. Confirm **App Release** passes all three jobs and the GitHub release has both `hermes-pebble.apk` and `hermes-pebble-store.pbw`.

The workflow builds Android with the permanent signing key and verifies its pinned certificate. It builds the watch with Pebble SDK 4.33.1, checks protocol identities and matching versions, then verifies the PBW's companion metadata. A failed watch build prevents publishing the APK alone. A manual workflow run provides both Actions artifacts without creating a release.

Keep watch UUID `7d07aa22-7d13-48c1-a400-2602a5ae4647`, Android package `dev.hermespebble.companion`, and the Android signing key stable. Store listing updates and APK updates must keep their existing identities.

## Pebble app-store upload

Upload the release's **`hermes-pebble-store.pbw`** to your Pebble developer listing for Pebble Time 2. It includes the Android companion package and a download URL pointing to this repository's latest release. Use that release page as the Android download link wherever the listing requests one:

`https://github.com/JojoRoro/hermes-pebble/releases/latest`

The GitHub workflow prepares the artifact; it does not submit or publish the Pebble listing. Use the store's current submission process to add the listing's screenshots and description and publish it.

Suggested installation text:

> Install Hermes Pebble on your watch and its Android companion from the linked GitHub release. Open the Android app, enter your Hermes API URL and key, and save/test the connection. Configure an extra access header only if your proxy requires it. On the watch, dictate, review, and Send. The answer appears automatically while the conversation screen is open; choose Reply to Hermes to continue.

Test a fresh store installation on a phone that has not installed a development PBW. Confirm companion discovery, the read-only connection test, dictation, automatic answer display, and follow-up. The emulator cannot verify store ingestion or physical Pebble host registration.

The official Pebble host is selected automatically when `coredevices.coreapp` is the only eligible host. Multiple hosts require a choice in Android Setup. Server credentials are necessarily a one-time user setup; the public APK cannot include a shared server key.

## CloudPebble development

The unpatched hosted importer drops the companion declaration. See [CloudPebble details and the operator patch](cloudpebble.md). This does not affect the SDK-built, verified release PBW used for store publishing.
