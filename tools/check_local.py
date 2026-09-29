"""Run offline regression checks with explicitly supplied host tools/libraries."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'android/app/src/main/java/dev/hermespebble/companion'


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def metadata_checks():
    for path in [ROOT / 'package.json', *ROOT.glob('protocol/examples/*.json')]:
        json.loads(path.read_text())
    for path in (ROOT / 'android/app/src/main').rglob('*.xml'):
        ET.parse(path)
    manifest = json.loads((ROOT / 'package.json').read_text())
    package = manifest['pebble']
    native = (ROOT / 'src/c/protocol.h').read_text()
    kotlin = (SOURCES / 'pebble/Protocol.kt').read_text()
    for name, value in package['messageKeys'].items():
        symbol = re.sub(r'(?<!^)(?=[A-Z])', '_', name).upper()
        assert re.search(rf'HERMES_KEY_{symbol}\s+{value}u\b', native), name
        assert re.search(rf'KEY_{symbol}\s*=\s*{value}u\b', kotlin), name
    assert package['uuid'] in native and package['uuid'] in kotlin
    app_id = package['companionApp']['android']['apps'][0]['package']
    android = (ROOT / 'android/app/build.gradle.kts').read_text()
    assert f'applicationId = "{app_id}"' in android
    assert f'versionName = "{manifest["version"]}"' in android, 'Watch and Android versions differ'
    print('JSON, XML, protocol keys, UUID, application ID, and release version checks passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-headers', type=Path, help='Official SDK emery/include directory')
    parser.add_argument('--kotlin-libs', type=Path, help='Directory containing the documented compiler/library JARs')
    parser.add_argument('--pebble-model', type=Path, help='PebbleKit PebbleDictionaryItem.kt source')
    args = parser.parse_args()
    metadata_checks()
    run('python3', '-m', 'unittest', 'discover', '-s', 'tests', '-v')
    with tempfile.TemporaryDirectory(prefix='hermes-check-') as temp_name:
        temp = Path(temp_name)
        if args.sdk_headers:
            headers = temp / 'headers'
            shutil.copytree(args.sdk_headers, headers)
            (headers / 'src').mkdir(exist_ok=True)
            (headers / 'src/resource_ids.auto.h').write_text('')
            keys = json.loads((ROOT / 'package.json').read_text())['pebble']['messageKeys']
            (headers / 'message_keys.auto.h').write_text('\n'.join(f'#define MESSAGE_KEY_{key} {value}' for key, value in keys.items()))
            # Pebble declares struct tm; host libc declares its own incompatible copy.
            (headers / 'time.h').write_text('#pragma once\n#include <stddef.h>\n#include <stdint.h>\ntypedef long time_t;\nstruct tm;\n')
            flags = ['-std=c11', f'-I{headers}', '-DPBL_PLATFORM_EMERY', '-DPBL_COLOR', '-DPBL_RECT']
            run('gcc', '-fsyntax-only', '-Wall', '-Wextra', *flags, 'src/c/hermes_pt2.c')
            run('gcc', *flags, '-ffunction-sections', '-fdata-sections', 'tests/watch_core_test.c', '-Wl,--gc-sections', '-o', temp / 'watch-check')
            run(temp / 'watch-check')
            run('gcc', *flags, '-ffunction-sections', '-fdata-sections', 'tests/watch_ink_test.c', '-Wl,--gc-sections', '-o', temp / 'ink-check')
            run(temp / 'ink-check', temp / 'ink-fixture.bin')
        if args.kotlin_libs:
            if not args.pebble_model:
                parser.error('--pebble-model is required with --kotlin-libs')
            jars = sorted(args.kotlin_libs.resolve().glob('*.jar'))
            classpath = ':'.join(map(str, jars))
            plugin = args.kotlin_libs.resolve() / 'kotlin-serialization-compiler-plugin-embeddable-2.4.20.jar'
            output = temp / 'kotlin-checks.jar'
            run('java', '-cp', classpath, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
                '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', classpath,
                f'-Xplugin={plugin}', '-d', output,
                SOURCES / 'network/HermesModels.kt', SOURCES / 'network/HermesClient.kt',
                SOURCES / 'pebble/Protocol.kt', SOURCES / 'pebble/InkCodec.kt', args.pebble_model.resolve(),
                'tests/ProtocolCheck.kt', 'tests/NetworkCheck.kt', 'tests/InkCheck.kt')
            for entry in ['pebble.ProtocolCheckKt', 'network.NetworkCheckKt']:
                run('java', '-cp', f'{classpath}:{output}', f'dev.hermespebble.companion.{entry}')
            fixture = [temp / 'ink-fixture.bin'] if args.sdk_headers else []
            run('java', '-cp', f'{classpath}:{output}', 'dev.hermespebble.companion.pebble.InkCheckKt', *fixture)


if __name__ == '__main__':
    main()
