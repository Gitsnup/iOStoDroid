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
from .dex import classes as dex_classes

# Kept as a fallback for callers without a source filename. Converted results
# use artifact_filename(source) so the APK keeps the imported IPA's basename.
ARTIFACT = "ConvertedIPA.apk"
BUILD_TOOLS = "35.0.0"
NDK_VERSION = "27.2.12479018"


def artifact_filename(source_name: str | Path) -> str:
    """Return a safe `<IPA stem>.apk` output name derived from the input filename."""
    name = str(source_name).replace("\\", "/").rsplit("/", 1)[-1]
    if name.lower().endswith(".ipa"):
        stem = name[:-4]
    else:
        stem = name.rsplit(".", 1)[0] if "." in name else name
    safe = "".join(ch if ch.isalnum() or ch in " ._-" else "_" for ch in stem)
    safe = safe.strip(" ._-")[:80].strip(" ._-") or "ConvertedIPA"
    return f"{safe}.apk"


def run(args: list[str | Path], log=None, timeout=180) -> str:
    argv = [str(a) for a in args]
    if log:
        log("tool", " ".join(argv))
    result = subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout
    )
    if log and result.stdout:
        log("tool-output", result.stdout.rstrip())
    if result.returncode:
        raise RuntimeError(f"{Path(argv[0]).name} failed ({result.returncode}):\n{result.stdout}")
    return result.stdout


@dataclass
class Toolchain:
    sdk: Path
    build: Path
    platform: Path
    clang: Path
    arm32_clang: Path

    @classmethod
    def discover(cls):
        home = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
        if not home:
            raise RuntimeError("ANDROID_SDK_ROOT or ANDROID_HOME is required; see docs/BUILD.md")
        sdk = Path(home)
        build = sdk / "build-tools" / BUILD_TOOLS
        platform = sdk / "platforms/android-35/android.jar"
        prebuilt = sdk / f"ndk/{NDK_VERSION}/toolchains/llvm/prebuilt/linux-x86_64/bin"
        clang = prebuilt / "aarch64-linux-android26-clang"
        arm32_clang = prebuilt / "armv7a-linux-androideabi26-clang"
        for path in (platform, clang, arm32_clang, *(build / name for name in ("aapt2", "d8", "zipalign", "apksigner"))):
            if not path.is_file():
                raise RuntimeError("missing toolchain file: " + str(path))
        for tool in ("java", "javac", "keytool"):
            if not shutil.which(tool):
                raise RuntimeError("Java 17 JDK tool missing: " + tool)
        return cls(sdk, build, platform, clang, arm32_clang)

    def tool(self, name):
        return self.build / name


def development_key(path: Path, log=None) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        # No committed secret. Stable alias/password for DEVELOPMENT ONLY.
        run(
            [
                "keytool",
                "-genkeypair",
                "-keystore",
                path,
                "-storepass",
                "android",
                "-keypass",
                "android",
                "-alias",
                "androiddebugkey",
                "-dname",
                "CN=Radek Development,O=Radek,C=US",
                "-keyalg",
                "RSA",
                "-keysize",
                "2048",
                "-validity",
                "10000",
                "-storetype",
                "JKS",
                "-noprompt",
            ],
            log,
        )
        path.chmod(0o600)
    return path


def build_apk(
    work: Path,
    output: Path,
    machine_code: bytes,
    metadata: dict,
    icon: bytes | None,
    assets: Path,
    report: dict,
    tools: Toolchain,
    key: Path,
    log=None,
    target_abi: str = "arm64-v8a",
    thumb: bool = False,
) -> dict:
    work.mkdir(parents=True, exist_ok=False)
    package = "dev.radek.converted.p" + metadata["sha256"][:20]
    source = work / "src/dev/radek/generated"
    source.mkdir(parents=True)
    label = str(metadata["name"])[:200]
    manifest = work / "AndroidManifest.xml"
    manifest.write_text(
        f"""<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}" android:versionCode="1" android:versionName="0.1">
<uses-sdk android:minSdkVersion="26" android:targetSdkVersion="35"/>
<application android:label={quoteattr(label)} android:icon="@drawable/app_icon" android:allowBackup="false" android:extractNativeLibs="true" android:theme="@android:style/Theme.Material.Light.NoActionBar">
<activity android:name="dev.radek.generated.MainActivity" android:exported="true"><intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter></activity>
</application></manifest>""",
        encoding="utf-8",
    )
    java = source / "MainActivity.java"
    java.write_text(
        """package dev.radek.generated;
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
}"""
    )
    res = work / "res/drawable"
    res.mkdir(parents=True)
    (res / "app_icon.png").write_bytes(icon or fallback_icon())
    assembly = work / "converted.S"
    symbol = "Java_dev_radek_generated_MainActivity_runNative"
    if target_abi == "arm64-v8a":
        if len(machine_code) % 4:
            raise InputError("ARM64 entry code is not word aligned")
        words = struct.unpack("<" + "I" * (len(machine_code) // 4), machine_code)
        assembly_text = (
            ".text\n.p2align 2\n.global " + symbol + "\n.type " + symbol + ", %function\n"
            + symbol + ":\n"
            + "".join(f"  .inst 0x{word:08x}\n" for word in words)
            + f'.size {symbol}, .-{symbol}\n.section .note.GNU-stack,"",%progbits\n'
        )
        native_clang = tools.clang
    elif target_abi == "armeabi-v7a":
        if len(machine_code) % (2 if thumb else 4):
            raise InputError("ARM32 entry code is not instruction aligned")
        mode = ".thumb\n.thumb_func\n" if thumb else ".arm\n"
        alignment = ".p2align 1\n" if thumb else ".p2align 2\n"
        octets = ",".join(f"0x{byte:02x}" for byte in machine_code)
        assembly_text = (
            ".syntax unified\n.text\n" + mode + alignment + ".global " + symbol
            + "\n.type " + symbol + ", %function\n" + symbol + ":\n"
            + "  .byte " + octets + f'\n.size {symbol}, .-{symbol}\n.section .note.GNU-stack,"",%progbits\n'
        )
        native_clang = tools.arm32_clang
    else:
        raise InputError("unsupported native APK ABI: " + target_abi)
    assembly.write_text(assembly_text, encoding="utf-8")
    native = work / "libconverted.so"
    run(
        [
            native_clang,
            "-shared",
            "-fPIC",
            "-nostdlib",
            "-Wl,-soname,libconverted.so",
            "-Wl,-z,defs",
            "-Wl,-z,noexecstack",
            "-Wl,-z,max-page-size=16384",
            "-Wl,--build-id=sha1",
            assembly,
            "-o",
            native,
        ],
        log,
    )
    classes, dex = work / "classes", work / "dex"
    classes.mkdir()
    dex.mkdir()
    run(["javac", "--release", "8", "-classpath", tools.platform, "-d", classes, java], log)
    run(
        [
            tools.tool("d8"),
            "--lib",
            tools.platform,
            "--min-api",
            "26",
            "--output",
            dex,
            *sorted(classes.rglob("*.class")),
        ],
        log,
    )
    (assets / "conversion.json").write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "package": package,
                "source": metadata,
                "conversion": report["conversion"],
                "targetAbi": target_abi,
                "resourceInventory": report["resources"],
                "contract": "closed-integer-entry-v1",
            },
            indent=2,
        )
    )
    compiled, unsigned, aligned = work / "compiled.zip", work / "unsigned.apk", work / "aligned.apk"
    run([tools.tool("aapt2"), "compile", "--dir", work / "res", "-o", compiled], log)
    run(
        [
            tools.tool("aapt2"),
            "link",
            "-o",
            unsigned,
            "-I",
            tools.platform,
            "--manifest",
            manifest,
            "--min-sdk-version",
            "26",
            "--target-sdk-version",
            "35",
            "-A",
            assets,
            compiled,
        ],
        log,
    )
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_DEFLATED) as z:
        z.write(dex / "classes.dex", "classes.dex")
        z.write(native, f"lib/{target_abi}/libconverted.so")
    run([tools.tool("zipalign"), "-f", "-P", "16", "4", unsigned, aligned], log)
    key = development_key(key, log)
    output.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            tools.tool("apksigner"),
            "sign",
            "--ks",
            key,
            "--ks-key-alias",
            "androiddebugkey",
            "--ks-pass",
            "pass:android",
            "--key-pass",
            "pass:android",
            "--out",
            output,
            aligned,
        ],
        log,
    )
    return {"package": package, "entryPoint": "dev.radek.generated.MainActivity", "apk": str(output), "abi": target_abi}


def elf_info(data: bytes) -> dict:
    """Inspect the native ABI, dynamic dependencies, and exported JNI entry."""
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[5] != 1:
        raise InputError("native library is not little-endian ELF")
    elf_class = data[4]
    if elf_class not in (1, 2):
        raise InputError("unsupported ELF class")
    is_64 = elf_class == 2
    header_size = 64 if is_64 else 52
    ph_stride_expected = 56 if is_64 else 32
    sh_stride_expected = 64 if is_64 else 40
    symbol_stride_expected = 24 if is_64 else 16
    dynamic_stride = 16 if is_64 else 8
    e_type, machine = struct.unpack_from("<HH", data, 16)
    if is_64:
        if len(data) < 64 or machine != 183:
            raise InputError("native library is not Android ARM64")
        phoff = struct.unpack_from("<Q", data, 32)[0]
        shoff = struct.unpack_from("<Q", data, 40)[0]
        ph_stride, ph_count = struct.unpack_from("<HH", data, 54)
        sh_stride, sh_count = struct.unpack_from("<HH", data, 58)
    else:
        if machine != 40:
            raise InputError("native library is not Android ARM32")
        phoff = struct.unpack_from("<I", data, 28)[0]
        shoff = struct.unpack_from("<I", data, 32)[0]
        ph_stride, ph_count = struct.unpack_from("<HH", data, 42)
        sh_stride, sh_count = struct.unpack_from("<HH", data, 46)
    if e_type != 3:
        raise InputError("native library is not ELF ET_DYN")
    if (
        ph_stride != ph_stride_expected
        or not ph_count
        or ph_count > 1024
        or phoff + ph_count * ph_stride > len(data)
    ):
        raise InputError("malformed ELF program headers")

    loads, dynamic = [], None
    executable = False
    for index in range(ph_count):
        offset = phoff + index * ph_stride
        if is_64:
            kind, flags, file_offset, vaddr, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, offset)
        else:
            kind, file_offset, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from("<IIIIIIII", data, offset)
        if file_offset + filesz > len(data) or filesz > memsz:
            raise InputError("ELF segment outside file")
        if kind == 1:
            if flags & 3 == 3:
                raise InputError("writable executable ELF segment")
            executable |= bool(flags & 1)
            loads.append((vaddr, file_offset, filesz))
        if kind == 2:
            dynamic = (file_offset, filesz)
        if kind == 0x6474E551 and flags & 1:
            raise InputError("executable native stack")
    if not executable or dynamic is None:
        raise InputError("ELF has no code or dynamic loader information")

    tags = {}
    for offset in range(dynamic[0], dynamic[0] + dynamic[1], dynamic_stride):
        if offset + dynamic_stride > dynamic[0] + dynamic[1]:
            raise InputError("truncated ELF dynamic table")
        tag, value = struct.unpack_from("<qQ" if is_64 else "<iI", data, offset)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    needed = []
    if 1 in tags:
        if 5 not in tags or 10 not in tags:
            raise InputError("ELF missing string table")
        address, size = tags[5][0], tags[10][0]
        mapping = next(
            ((base, offset, length) for base, offset, length in loads
             if base <= address and address + size <= base + length),
            None,
        )
        if mapping is None:
            raise InputError("ELF strings not mapped")
        base, offset, _ = mapping
        table = data[offset + address - base : offset + address - base + size]
        for index in tags[1]:
            end = table.find(b"\x00", index)
            if index >= size or end < 0:
                raise InputError("invalid ELF dependency")
            needed.append(table[index:end].decode("ascii"))

    if (
        sh_stride != sh_stride_expected
        or not sh_count
        or sh_count > 65535
        or shoff + sh_count * sh_stride > len(data)
    ):
        raise InputError("missing/malformed ELF section table")
    sections = []
    for index in range(sh_count):
        offset = shoff + index * sh_stride
        sections.append(
            struct.unpack_from("<IIQQQQIIQQ" if is_64 else "<IIIIIIIIII", data, offset)
        )
    exports, undefined = {}, []
    for section in sections:
        _, kind, _, _, offset, size, link, _, _, stride = section
        if kind != 11:
            continue
        if (
            stride != symbol_stride_expected
            or size % symbol_stride_expected
            or size // symbol_stride_expected > 200000
            or offset + size > len(data)
            or link >= sh_count
        ):
            raise InputError("invalid ELF dynamic symbols")
        strings = sections[link]
        if strings[1] != 3 or strings[4] + strings[5] > len(data):
            raise InputError("invalid ELF dynamic strings")
        table = data[strings[4] : strings[4] + strings[5]]
        for symbol_offset in range(offset + symbol_stride_expected, offset + size, symbol_stride_expected):
            if is_64:
                index, info, visibility, shndx, address, length = struct.unpack_from(
                    "<IBBHQQ", data, symbol_offset
                )
            else:
                index, address, length, info, visibility, shndx = struct.unpack_from(
                    "<IIIBBH", data, symbol_offset
                )
            end = table.find(b"\x00", index)
            if index >= len(table) or end < 0 or end - index > 4096:
                raise InputError("invalid ELF symbol name")
            name = table[index:end].decode("utf-8", errors="strict")
            if info >> 4 not in (1, 2):
                continue
            if shndx == 0:
                undefined.append(name)
            elif visibility & 3 in (0, 3):
                item = {"address": address, "size": length, "type": info & 15}
                code_address = address & ~1 if machine == 40 else address
                mapping = next(
                    ((base, file_offset) for base, file_offset, amount in loads
                     if base <= code_address and code_address + length <= base + amount),
                    None,
                )
                if mapping is not None and length:
                    import hashlib
                    base, file_offset = mapping
                    item["sha256"] = hashlib.sha256(
                        data[file_offset + code_address - base : file_offset + code_address - base + length]
                    ).hexdigest()
                exports[name] = item
    return {
        "architecture": "arm64-v8a" if is_64 else "armeabi-v7a",
        "needed": needed,
        "exports": exports,
        "undefinedSymbols": undefined,
    }

def validate_apk(
    path: Path,
    tools: Toolchain,
    expected_package: str,
    expected_entry: str,
    converted: bool = True,
    log=None,
    expected_abi: str = "arm64-v8a",
) -> dict:
    if not path.is_file() or not 0 < path.stat().st_size <= 512 * 1024 * 1024:
        raise InputError("APK is missing/empty")
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if (
            len(names) > 20000
            or sum(i.file_size for i in z.infolist()) > 1024 * 1024 * 1024
            or any(i.file_size > 256 * 1024 * 1024 for i in z.infolist())
        ):
            raise InputError("APK ZIP size/entry limit exceeded")
        if len(names) != len(set(names)):
            raise InputError("APK duplicate ZIP paths")
        for name in names:
            safe_name(name)
        if z.testzip():
            raise InputError("APK CRC failure")
        for required in ("AndroidManifest.xml", "resources.arsc", "classes.dex"):
            if required not in names:
                raise InputError("APK missing " + required)
        defined_classes = set()
        for name in names:
            if re.fullmatch(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex", name):
                defined_classes.update(dex_classes(z.read(name)))
        if "L" + expected_entry.replace(".", "/") + ";" not in defined_classes:
            raise InputError("Android entry class is absent from DEX definitions")
        if z.read("AndroidManifest.xml")[:2] != b"\x03\x00":
            raise InputError("APK manifest is not compiled binary XML")
        if expected_abi not in ("arm64-v8a", "armeabi-v7a"):
            raise InputError("unsupported expected APK ABI: " + expected_abi)
        native_names = [name for name in names if name.startswith("lib/") and name.endswith(".so")]
        libraries = {
            name.rsplit("/", 1)[-1]: elf_info(z.read(name))
            for name in native_names
            if name.startswith(f"lib/{expected_abi}/")
        }
        if not libraries:
            raise InputError(f"APK has no {expected_abi} native libraries")
        if any(not name.startswith(f"lib/{expected_abi}/") for name in native_names):
            raise InputError("unexpected native architecture")
        android_system = {
            "libc.so",
            "libm.so",
            "libdl.so",
            "liblog.so",
            "libandroid.so",
            "libGLESv2.so",
            "libEGL.so",
            "libz.so",
        }
        for name, info in libraries.items():
            unresolved = set(info["needed"]) - libraries.keys() - android_system
            if unresolved:
                raise InputError(f"{name}: unresolved native dependencies {sorted(unresolved)}")
        if converted:
            if "libconverted.so" not in libraries:
                raise InputError("converted native library is absent")
            metadata = json.loads(z.read("assets/conversion.json"))
            if (
                metadata.get("package") != expected_package
                or metadata.get("contract") != "closed-integer-entry-v1"
                or metadata.get("targetAbi") != expected_abi
            ):
                raise InputError("conversion metadata/package/ABI mismatch")
            conversion = metadata.get("conversion", {})
            if not conversion.get("outputBytes"):
                raise InputError("missing reconstruction provenance")
            native = libraries["libconverted.so"]
            if native["architecture"] != expected_abi:
                raise InputError("native ELF architecture does not match APK ABI path")
            entry = native["exports"].get("Java_dev_radek_generated_MainActivity_runNative")
            if (
                entry is None
                or entry["type"] != 2
                or entry["size"] != conversion["outputBytes"]
                or entry.get("sha256") != conversion.get("machineCodeSha256")
            ):
                raise InputError("native JNI entry/code does not match verified reconstruction")
            if native["undefinedSymbols"] or native["needed"]:
                raise InputError("closed native program unexpectedly depends on external code")
            for resource in metadata.get("resourceInventory", []):
                import hashlib

                name = "assets/bundle/" + resource["path"]
                if hashlib.sha256(z.read(name)).hexdigest() != resource["sha256"]:
                    raise InputError("resource integrity failure: " + name)
            for name in names:
                if name.endswith(".ipa"):
                    raise InputError("original IPA must not be included")
                if name.startswith("assets/") and z.read(name)[:4] in (
                    b"\xcf\xfa\xed\xfe",
                    b"\xce\xfa\xed\xfe",
                ):
                    raise InputError("Apple executable leaked into assets")
    badging = run([tools.tool("aapt2"), "dump", "badging", path], log)
    if not re.search(r"^package: name='" + re.escape(expected_package) + r"'", badging, re.M):
        raise InputError("manifest package identity mismatch")
    if not re.search(r"^launchable-activity: name='" + re.escape(expected_entry) + r"'", badging, re.M):
        raise InputError("manifest Android entry point missing/mismatched")
    icons = re.findall(r"^application-icon-\d+:'([^']+)'", badging, re.M)
    if not icons or not any(icon in names for icon in icons):
        raise InputError("manifest icon is absent")
    signature = run(
        [tools.tool("apksigner"), "verify", "--verbose", "--print-certs", "--min-sdk-version", "26", path],
        log,
    )
    run([tools.tool("zipalign"), "-c", "-P", "16", "4", path], log)
    return {
        "status": "PASSED",
        "abi": expected_abi,
        "checks": [
            "structure",
            "binary-manifest",
            "package",
            "launcher",
            "DEX-integrity-and-entry",
            "signing",
            "ARM64-ELF" if expected_abi == "arm64-v8a" else "ARM32-ELF",
            "dependencies",
            "resources",
            "icon",
            "alignment",
        ]
        + (
            ["assets", "resource-hashes", "native-JNI-entry", "native-code-hash", "conversion-provenance"]
            if converted
            else []
        ),
        "libraries": libraries,
        "signature": signature.strip(),
        "runtimeExecution": "NOT_TESTED",
    }
