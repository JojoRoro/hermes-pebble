# Repository release preference

The user has explicitly requested that completed app changes in this repository
be tagged and pushed so GitHub Actions builds and publishes the Android APK.
Treat this as standing authorization for that release workflow; do not stop at
a local build or ask for the same permission again.

- Validate the intended changes before releasing and commit only relevant files.
- Keep Android `versionName`, Android `versionCode`, and the watch version in
  `package.json` consistent. Increment versions for each new app release.
- Push the release commit to `origin`, create an unused annotated `v*` version
  tag, and push that tag. The Android workflow is triggered by tag pushes, not
  ordinary pushes to `main`.
- Never move or overwrite an existing release tag. Check remote state first.
- Verify the workflow and release result and report its link or any failure.

Documentation-only changes do not require a separate app release unless asked.
