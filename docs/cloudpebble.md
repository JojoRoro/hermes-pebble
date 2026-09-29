# CloudPebble and companion metadata

For normal installation and app-store publishing, use `hermes-pebble-store.pbw` from the [GitHub release](https://github.com/JojoRoro/hermes-pebble/releases/latest). The release workflow builds and verifies it automatically. Users install that PBW (or the future store listing), install the APK, and configure their own Hermes server on Android. CloudPebble is a developer tool, not an installation requirement.

## Why direct installation loses the phone connection

Think of the PBW as a parcel with an address label: `companionApp` tells the Pebble phone app which Android package should receive watch messages. CloudPebble builds the watch program but drops that label. Opening the program on the watch can succeed while messages to the companion fail.

The inspected upstream is [coredevices/cloudpebble at 08298a2](https://github.com/coredevices/cloudpebble/tree/08298a28fab376452b880409364913904f5ea135), checked on 29 September 2026:

- [Manifest import and generation](https://github.com/coredevices/cloudpebble/blob/08298a28fab376452b880409364913904f5ea135/cloudpebble/ide/utils/sdk/manifest.py) copy a fixed set of project properties and omit `companionApp`.
- [Project assembly](https://github.com/coredevices/cloudpebble/blob/08298a28fab376452b880409364913904f5ea135/cloudpebble/ide/utils/sdk/project_assembly.py) generates both the manifest and `wscript`, so changing this repository's build script does not repair hosted builds.
- [The build task](https://github.com/coredevices/cloudpebble/blob/08298a28fab376452b880409364913904f5ea135/cloudpebble/ide/tasks/build.py) uses npm with `--ignore-scripts`, so a package install hook cannot repair it either.

The PBW needs root `appinfo.json` → `companionApp.android.apps` to contain `dev.hermespebble.companion`. The root repository `package.json` already declares this correctly under `pebble.companionApp`. This is packaging metadata used by the phone host; changing the watch's C code cannot restore it after installation.

## Fix for CloudPebble operators

[cloudpebble-companion-metadata.patch](../tools/upstream/cloudpebble-companion-metadata.patch) targets the upstream commit above. It adds a project JSON field, a database migration, import/export support for the declaration, template-copy support, and four manifest round-trip tests.

Apply it in a CloudPebble checkout, then use the deployment's normal environment to migrate and restart both the web service and build workers:

```sh
git apply /path/to/cloudpebble-companion-metadata.patch
python3 cloudpebble/ide/tests/test_companion_manifest.py
cd cloudpebble
python manage.py migrate
```

Reimport Hermes Pebble from `https://github.com/JojoRoro/hermes-pebble` after the change: existing imported projects have already lost the metadata. Build with an SDK supporting PebbleKit 2 companion declarations (this repository uses 4.33.1). Inspect the resulting PBW's root `appinfo.json` before testing direct installation.

The patch's manifest tests pass locally. It has not been deployed to the hosted CloudPebble service, and its full Django/database deployment has not been exercised here. The service operator must adopt the patch before that direct-install route is fixed.

## Temporary route for hosted CloudPebble builds

1. Import the repository root and target Pebble Time 2 / `emery`. Keep `sdkVersion: "3"` in project metadata; this is the manifest format, not a pin to SDK release 4.33.1. Record the actual hosted SDK from build output.
2. Download the built PBW. Run this command locally with Python 3 and the matching repository checkout:

   ```sh
   python3 tools/finalize_pbw.py --package package.json --input downloads/hermes.pbw --output downloads/hermes-ready.pbw
   ```

3. Open `hermes-ready.pbw` with the Pebble phone app. Installing through CloudPebble's direct-install button bypasses the correction.
4. Install the matching Android APK, configure Hermes, and test the watch link.

The finalizer checks the UUID, restores only the top-level companion declaration, and verifies that every other ZIP member's uncompressed bytes are unchanged. It rejects duplicate members, an existing output, and an in-place output; incomplete outputs are removed. It also verifies already-correct metadata. The release workflow runs this same check automatically.

Keep GitHub as the source of truth; an export from unpatched CloudPebble must not replace the repository's companion declaration. Credentials belong only in Android runtime settings, never in CloudPebble or app metadata.
