"""Real SDK/NDK packaging and independent APK structural/signature validation."""
from __future__ import annotations
import json
import os
import re
import shutil
import struct
import subprocess
import zipfile
from dataclasses import dataclass
from pathlib import Path
from xml.sax.saxutils import quoteattr
from .archive import InputError, safe_name
from .resources import fallback_icon

ARTIFACT = 'RadekiOSConventor-debug.apk'
BUILD_TOOLS = '35.0.0'
NDK_VERSION = '27.2.12479018'


def run(args: list[str | Path], log=None, timeout=180) -> str:
    argv = [str(a) for a in args]
    if log:
        log('tool', ' '.join(argv))
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if log and result.stdout:
        log('tool-output', result.stdout.rstrip())
    if result.returncode:
        raise RuntimeError(f'{Path(argv[0]).name} failed ({result.returncode}):\n{result.stdout}')
    return result.stdout


@dataclass
class Toolchain:
    sdk: Path
    build: Path
    platform: Path
    clang: Path

    @classmethod
    def discover(cls):
        home = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
        if not home:
            raise RuntimeError('ANDROID_SDK_ROOT or ANDROID_HOME is required; see docs/BUILD.md')
        sdk = Path(home)
        build = sdk / 'build-tools' / BUILD_TOOLS
        platform = sdk / 'platforms/android-35/android.jar'
        clang = sdk / f'ndk/{NDK_VERSION}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang'
        for path in (platform, clang, *(build / name for name in ('aapt2', 'd8', 'zipalign', 'apksigner'))):
            if not path.is_file():
                raise RuntimeError('missing toolchain file: ' + str(path))
        for tool in ('java', 'javac', 'keytool'):
            if not shutil.which(tool):
                raise RuntimeError('Java 17 JDK tool missing: ' + tool)
        return cls(sdk, build, platform, clang)

    def tool(self, name):
        return self.build / name


def development_key(path: Path, log=None) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        # No committed secret. Stable alias/password for DEVELOPMENT ONLY.
        run(['keytool', '-genkeypair', '-keystore', path, '-storepass', 'android', '-keypass', 'android',
             '-alias', 'androiddebugkey', '-dname', 'CN=Radek Development,O=Radek,C=US', '-keyalg', 'RSA',
             '-keysize', '2048', '-validity', '10000', '-storetype', 'JKS', '-noprompt'], log)
        path.chmod(0o600)
    return path


def build_apk(work: Path, output: Path, machine_code: bytes, metadata: dict, icon: bytes | None,
              assets: Path, report: dict, tools: Toolchain, key: Path, log=None) -> dict:
    work.mkdir(parents=True, exist_ok=False)
    package = 'dev.radek.converted.p' + metadata['sha256'][:20]
    source = work / 'src/dev/radek/generated'
    source.mkdir(parents=True)
    label = str(metadata['name'])[:200]
    manifest = work / 'AndroidManifest.xml'
    manifest.write_text(f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}" android:versionCode="1" android:versionName="0.1">
<uses-sdk android:minSdkVersion="26" android:targetSdkVersion="35"/>
<application android:label={quoteattr(label)} android:icon="@drawable/app_icon" android:allowBackup="false" android:extractNativeLibs="true" android:theme="@android:style/Theme.Material.Light.NoActionBar">
<activity android:name="dev.radek.generated.MainActivity" android:exported="true"><intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter></activity>
</application></manifest>''', encoding='utf-8')
    java = source / 'MainActivity.java'
    java.write_text('''package dev.radek.generated;
import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
public final class MainActivity extends Activity {
    static { System.loadLibrary("converted"); }
    private static native int runNative();
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int result = runNative();
        android.util.Log.i("RadekNative", "native entry returned " + result);
        TextView view = new TextView(this);
        view.setTextSize(22); view.setPadding(28, 60, 28, 28);
        view.setText("Standalone native program\\n\\nEntry returned: " + result +
            "\\n\\nThis closed integer program runs as Android ARM64 code. It is not an iOS UI port.");
        setContentView(view);
    }
}''')
    res = work / 'res/drawable'
    res.mkdir(parents=True)
    (res / 'app_icon.png').write_bytes(icon or fallback_icon())
    assembly = work / 'converted.S'
    words = struct.unpack('<' + 'I' * (len(machine_code) // 4), machine_code)
    assembly.write_text('.text\n.p2align 2\n.global Java_dev_radek_generated_MainActivity_runNative\n.type Java_dev_radek_generated_MainActivity_runNative_runNative, %function\n'.replace('MainActivity_runNative_runNative', 'MainActivity_runNative') +
                        'Java_dev_radek_generated_MainActivity_runNative:\n' +
                        ''.join(f'  .inst 0x{w:08x}\n' for w in words) +
                        '.size Java_dev_radek_generated_MainActivity_runNative, .-Java_dev_radek_generated_MainActivity_runNative\n.section .note.GNU-stack,"",%progbits\n')
    native = work / 'libconverted.so'
    run([tools.clang, '-shared', '-fPIC', '-nostdlib', '-Wl,-soname,libconverted.so', '-Wl,-z,defs',
         '-Wl,-z,noexecstack', '-Wl,-z,max-page-size=16384', '-Wl,--build-id=sha1', assembly, '-o', native], log)
    classes, dex = work / 'classes', work / 'dex'
    classes.mkdir(); dex.mkdir()
    run(['javac', '--release', '8', '-classpath', tools.platform, '-d', classes, java], log)
    run([tools.tool('d8'), '--lib', tools.platform, '--min-api', '26', '--output', dex, *sorted(classes.rglob('*.class'))], log)
    (assets / 'conversion.json').write_text(json.dumps({'schemaVersion': 1, 'package': package, 'source': metadata,
        'conversion': report['conversion'], 'resourceInventory': report['resources'], 'contract': 'closed-integer-entry-v1'}, indent=2))
    compiled, unsigned, aligned = work / 'compiled.zip', work / 'unsigned.apk', work / 'aligned.apk'
    run([tools.tool('aapt2'), 'compile', '--dir', work / 'res', '-o', compiled], log)
    run([tools.tool('aapt2'), 'link', '-o', unsigned, '-I', tools.platform, '--manifest', manifest,
         '--min-sdk-version', '26', '--target-sdk-version', '35', '-A', assets, compiled], log)
    with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as z:
        z.write(dex / 'classes.dex', 'classes.dex')
        z.write(native, 'lib/arm64-v8a/libconverted.so')
    run([tools.tool('zipalign'), '-f', '-P', '16', '4', unsigned, aligned], log)
    key = development_key(key, log)
    output.parent.mkdir(parents=True, exist_ok=True)
    run([tools.tool('apksigner'), 'sign', '--ks', key, '--ks-key-alias', 'androiddebugkey',
         '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', output, aligned], log)
    return {'package': package, 'entryPoint': 'dev.radek.generated.MainActivity', 'apk': str(output)}


def elf_info(data: bytes) -> dict:
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise InputError('native library is not ELF64 little-endian')
    etype, machine = struct.unpack_from('<HH', data, 16)
    if etype != 3 or machine != 183:
        raise InputError('native library is not Android ARM64 ET_DYN')
    offset = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride != 56 or not count or count > 1024 or offset + count * stride > len(data):
        raise InputError('malformed ELF program headers')
    loads, dynamic = [], None
    executable = False
    for i in range(count):
        kind, flags, off, va, _, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', data, offset + i * stride)
        if off + filesz > len(data) or filesz > memsz:
            raise InputError('ELF segment outside file')
        if kind == 1:
            if flags & 3 == 3:
                raise InputError('writable executable ELF segment')
            executable |= bool(flags & 1)
            loads.append((va, off, filesz))
        if kind == 2:
            dynamic = (off, filesz)
        if kind == 0x6474e551 and flags & 1:
            raise InputError('executable native stack')
    if not executable or dynamic is None:
        raise InputError('ELF has no code or dynamic loader information')
    tags = {}
    for off in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        if off + 16 > dynamic[0] + dynamic[1]:
            raise InputError('truncated ELF dynamic table')
        tag, value = struct.unpack_from('<qQ', data, off)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    needed = []
    if 1 in tags:
        if 5 not in tags or 10 not in tags:
            raise InputError('ELF missing string table')
        address, size = tags[5][0], tags[10][0]
        mapping = next(((base, off, length) for base, off, length in loads if base <= address and address+size <= base+length), None)
        if mapping is None:
            raise InputError('ELF strings not mapped')
        base, off, _ = mapping
        table = data[off+address-base:off+address-base+size]
        for index in tags[1]:
            end = table.find(b'\x00', index)
            if index >= size or end < 0:
                raise InputError('invalid ELF dependency')
            needed.append(table[index:end].decode('ascii'))
    return {'architecture': 'arm64-v8a', 'needed': needed}


def validate_apk(path: Path, tools: Toolchain, expected_package: str, expected_entry: str,
                 converted: bool = True, log=None) -> dict:
    if not path.is_file() or path.stat().st_size == 0:
        raise InputError('APK is missing/empty')
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if len(names) != len(set(names)):
            raise InputError('APK duplicate ZIP paths')
        for name in names:
            safe_name(name)
        if z.testzip():
            raise InputError('APK CRC failure')
        for required in ('AndroidManifest.xml', 'resources.arsc', 'classes.dex'):
            if required not in names:
                raise InputError('APK missing ' + required)
        if not z.read('classes.dex').startswith(b'dex\n'):
            raise InputError('APK entry code is not DEX')
        if z.read('AndroidManifest.xml')[:2] != b'\x03\x00':
            raise InputError('APK manifest is not compiled binary XML')
        libraries = {name.rsplit('/', 1)[-1]: elf_info(z.read(name)) for name in names if name.startswith('lib/arm64-v8a/') and name.endswith('.so')}
        if not libraries:
            raise InputError('APK has no ARM64 native libraries')
        if any(name.startswith('lib/') and not name.startswith('lib/arm64-v8a/') for name in names if name.endswith('.so')):
            raise InputError('unexpected native architecture')
        android_system = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libandroid.so', 'libGLESv2.so', 'libEGL.so', 'libz.so'}
        for name, info in libraries.items():
            unresolved = set(info['needed']) - libraries.keys() - android_system
            if unresolved:
                raise InputError(f'{name}: unresolved native dependencies {sorted(unresolved)}')
        if converted:
            if 'libconverted.so' not in libraries:
                raise InputError('converted native library is absent')
            metadata = json.loads(z.read('assets/conversion.json'))
            if metadata.get('package') != expected_package or metadata.get('contract') != 'closed-integer-entry-v1':
                raise InputError('conversion metadata/package mismatch')
            if not metadata.get('conversion', {}).get('outputBytes'):
                raise InputError('missing reconstruction provenance')
            for resource in metadata.get('resourceInventory', []):
                import hashlib
                name = 'assets/bundle/' + resource['path']
                if hashlib.sha256(z.read(name)).hexdigest() != resource['sha256']:
                    raise InputError('resource integrity failure: ' + name)
            for name in names:
                if name.endswith('.ipa'):
                    raise InputError('original IPA must not be included')
                if name.startswith('assets/') and z.read(name)[:4] in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe'):
                    raise InputError('Apple executable leaked into assets')
    badging = run([tools.tool('aapt2'), 'dump', 'badging', path], log)
    if not re.search(r"^package: name='" + re.escape(expected_package) + r"'", badging, re.M):
        raise InputError('manifest package identity mismatch')
    if not re.search(r"^launchable-activity: name='" + re.escape(expected_entry) + r"'", badging, re.M):
        raise InputError('manifest Android entry point missing/mismatched')
    icons = re.findall(r"^application-icon-\d+:'([^']+)'", badging, re.M)
    if not icons or not any(icon in names for icon in icons):
        raise InputError('manifest icon is absent')
    signature = run([tools.tool('apksigner'), 'verify', '--verbose', '--print-certs', '--min-sdk-version', '26', path], log)
    run([tools.tool('zipalign'), '-c', '-P', '16', '4', path], log)
    return {'status': 'PASSED', 'checks': ['structure', 'binary-manifest', 'package', 'launcher', 'DEX', 'signing', 'ARM64-ELF', 'dependencies', 'resources', 'icon', 'alignment'] + (['assets', 'resource-hashes', 'conversion-provenance'] if converted else []),
            'libraries': libraries, 'signature': signature.strip(), 'runtimeExecution': 'NOT_TESTED'}
