# Android signing maintenance

CI signs the release variant using one permanent RSA key (alias `hermes`, PKCS12 store). The same password protects the store and key. The public certificate SHA-256 fingerprint is committed in `.github/android-signing-cert.sha256`; the private material must never be committed.

Required repository Actions secrets:

- `ANDROID_SIGNING_KEY_BASE64`: base64 encoding of the PKCS12 keystore.
- `ANDROID_SIGNING_PASSWORD`: its password.

The workflow fails if either is absent, verifies the completed APK signature against the pinned public fingerprint, and deletes the temporary keystore before artifact upload. It uploads only the signed APK. Do not replace the signing key on each release or change the fingerprint just to silence a failure.

A protected local backup is kept in the git-ignored `.signing/` directory: `hermes-release.p12` and `password`. Keep an additional secure backup outside the build machine. GitHub Actions secrets cannot be read back through the normal secrets API. Loss of the key prevents compatible future updates; possession of both files permits signing apps as this publisher.

Local release builds use environment variables `HERMES_KEYSTORE_FILE` (absolute path) and `HERMES_KEYSTORE_PASSWORD`, followed by `./gradlew :app:assembleRelease`. Do not put the password in shell history, command arguments, logs, or Gradle properties committed to Git. Without these variables, local release builds remain unsigned; local debug builds use the ordinary developer debug certificate.

## Migration from the early debug builds

Versions through v0.1.2 used fresh debug keys on GitHub-hosted runners. Those keys were not preserved. A new signature cannot update those installations. Save any needed local notes/history and have the Hermes and proxy credentials available, then uninstall the old Android app and install the permanent-key release once. Android uninstall clears the app's local data. Re-enter configuration and select the Pebble host afterward. Future permanent-key releases can update that installation normally.

This follows [Android's signing requirements](https://developer.android.com/studio/publish/app-signing). The APK is distributed directly for sideloading; this setup does not use Play App Signing.
