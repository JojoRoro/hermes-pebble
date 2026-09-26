# CloudPebble setup and PBW finalization

CloudPebble is the only watch compilation route for v1. This document describes a later manual operation; no CloudPebble project was opened or built during implementation.

## Required sequence

1. Open CloudPebble and import the native C project from the repository root at `https://github.com/hermes-pebble/hermes-pebble`. Select the intended branch explicitly; do not assume the importer chose the branch containing this metadata. Select Pebble Time 2 and the `emery` target platform.
2. Let CloudPebble manage its hosted SDK. The documented reference release is `4.33.1`, but the project manifest must retain `"sdkVersion": "3"`. There is no supported per-project SDK selector to reproduce. Record the actual SDK version from the later CloudPebble build output rather than claiming the reference release was installed.
3. Build manually when ready and download the resulting PBW. Do not start an automatic webhook or a local Pebble compiler.
4. Run the standard-library metadata finalizer on the downloaded file:

   ```sh
   python3 tools/finalize_pbw.py --package package.json --input downloads/hermes-pt2.pbw --output downloads/hermes-pt2-ready.pbw
   ```

   Create the `downloads` directory through the normal local workflow before running the command. Python 3 is the only utility dependency. Inspect the report for the package path, watch UUID, separate output path, and either `metadata restored` or `metadata already present`.
5. Install `hermes-pt2-ready.pbw` through the ordinary Pebble file-install flow. Do not use CloudPebble's direct-install button for an unfinalized PBW; that route bypasses the required companion metadata correction.
6. Install the Android APK separately, select the official Pebble phone host, enter the Hermes API credential and independent NetBird access header at runtime, and perform the application handshake. If the watch reports missing companion permission, return to the finalized-PBW step and confirm the exact output was installed.

## Why finalization is required

The inspected CloudPebble project assembly path regenerates package metadata and does not retain `companionApp`. The official Pebble phone app uses the PBW's top-level `appInfo.companionApp.android.apps` entry to select the PebbleKit 2 companion. An unfinalized PBW can therefore compile but fail companion registration.

The finalizer treats the repository `package.json` as the source of truth. It requires a valid package UUID and a nonempty Android companion app-object array, checks the PBW UUID, and sets only the top-level `companionApp` in root `appinfo.json`. It does not add a nested `pebble` object and does not alter executable, resource, or platform-manifest bytes.

The tool rejects duplicate ZIP member names, bounds total uncompressed input, refuses an existing output or an in-place output, and reopens the result to verify the UUID, exact companion declaration, and SHA-256 equality of every other member's uncompressed bytes. An incomplete output is removed on failure. Recompression may change the physical ZIP bytes, which is expected; the member payload contract is what is preserved.

If the correct declaration is already present, the tool still writes and verifies the requested separate output and reports `metadata already present`. No real PBW has been finalized in this pass.

## Source-of-truth and route rules

Keep GitHub as the source of truth. A CloudPebble export must not overwrite the root package declaration. There is no automatic cloud webhook, local Pebble SDK requirement, Node build, or Android artifact in this route. A one-click cloud build-and-install flow cannot be promised until the hosted implementation preserves the declaration; the finalizer can verify and leave an already-correct declaration intact.

The Android workflow is separate and manually triggered. It produces only an APK; it does not produce or install a PBW. Do not send credentials to CloudPebble, GitHub Actions, package metadata, or the watch.

## Later checks

The deferred checks in `docs/validation.md` include a real root import/build, SDK recording, finalized metadata, ordinary file installation, companion registration, wrong UUID rejection, duplicate archive-member rejection, duplicate companion validation, and byte-preservation checks. None is marked as passed here.
