#!/usr/bin/env python3
"""Build the Arduino ESP8266 network reference firmware without Arduino CLI."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


CORE_SOURCES = (
    "cores/esp8266",
    "libraries/ESP8266WiFi/src",
    "libraries/ESP8266HTTPClient/src",
    "libraries/ESP8266httpUpdate/src",
    "libraries/EEPROM",
    "libraries/ESP8266WebServer/src",
    "libraries/ESP8266HTTPUpdateServer/src",
)
INPUT_DIRS = (
    "cores/esp8266",
    "libraries/ESP8266WiFi/src",
    "libraries/ESP8266HTTPClient/src",
    "libraries/ESP8266httpUpdate/src",
    "libraries/EEPROM",
    "libraries/ESP8266WebServer/src",
    "libraries/ESP8266HTTPUpdateServer/src",
    "variants/generic",
    "tools/sdk/include",
    "tools/sdk/lwip2/include",
    "tools/sdk/ld",
    "tools/sdk/lib/NONOSDK22x_190703",
    "bootloaders/eboot",
)
EXTENSIONS = {".c", ".cpp", ".S"}
FLASH_FREQ = "40"
FLASH_LINKER_SCRIPTS = {
    "1M": "eagle.flash.1m64.ld",
    "2M": "eagle.flash.2m64.ld",
    "4M": "eagle.flash.4m1m.ld",
}


def run(command, *, cwd=None, capture=False):
    try:
        result = subprocess.run(
            [str(part) for part in command],
            cwd=cwd,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        if capture and isinstance(exc, subprocess.CalledProcessError):
            detail = exc.stdout or ""
            if detail:
                print(detail, file=sys.stderr, end="")
        raise RuntimeError("command failed: " + " ".join(map(str, command))) from exc
    return result.stdout.strip() if capture else ""


def toolchain_bin(path):
    candidates = [path / "bin", path / "xtensa-lx106-elf" / "bin"]
    for candidate in candidates:
        if (candidate / "xtensa-lx106-elf-gcc").is_file():
            return candidate.resolve()
    raise ValueError(f"cannot find Xtensa GCC under {path}")


def source_files(core):
    paths = []
    for relative in CORE_SOURCES:
        paths.extend(p for p in (core / relative).rglob("*")
                     if p.is_file() and p.suffix in EXTENSIONS)
    return sorted(paths)


def input_digest(core, bin_dir):
    digest = hashlib.sha256()
    digest.update(f"core={core.resolve()}\n".encode())
    digest.update(f"toolchain={bin_dir.resolve()}\n".encode())
    for executable in ("xtensa-lx106-elf-gcc", "xtensa-lx106-elf-g++",
                       "xtensa-lx106-elf-ld", "xtensa-lx106-elf-objcopy"):
        digest.update(run([bin_dir / executable, "--version"], capture=True).encode())
        digest.update(b"\0")
    files = []
    for relative in INPUT_DIRS:
        folder = core / relative
        if not folder.is_dir():
            raise ValueError(f"missing Arduino core input directory: {folder}")
        files.extend(p for p in folder.rglob("*") if p.is_file())
    files.extend(p for p in (core / "tools/sdk/lib").glob("*.a") if p.is_file())
    for folder in (bin_dir.parent / "include",
                   bin_dir.parent / "xtensa-lx106-elf/include",
                   bin_dir.parent / "xtensa-lx106-elf/lib"):
        if folder.is_dir():
            files.extend(p for p in folder.rglob("*") if p.is_file())
    files.append(Path(__file__).resolve())
    files.append(core / "platform.txt")
    files.append(core / "tools/elf2bin.py")
    for path in sorted(set(files)):
        digest.update(str(path.relative_to(core) if path.is_relative_to(core) else path).encode())
        digest.update(b"\0")
        with path.open("rb") as source:
            while True:
                block = source.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
        digest.update(b"\0")
    return digest.hexdigest()


def compile_reference(core, bin_dir, build_dir, object_dir, version, flash_size, flash_mode):
    toolroot = bin_dir.parent
    target = toolroot / "xtensa-lx106-elf"
    sdk = core / "tools/sdk"
    libc = target
    packaged_libc = sdk / "libc/xtensa-lx106-elf"
    if packaged_libc.is_dir():
        libc = sdk / "libc/xtensa-lx106-elf"
    core_dir = core / "cores/esp8266"
    variant = core / "variants/generic"
    library_dirs = [core / p for p in (
        "libraries/ESP8266WiFi/src",
        "libraries/ESP8266HTTPClient/src",
        "libraries/ESP8266httpUpdate/src",
        "libraries/EEPROM",
        "libraries/ESP8266WebServer/src",
        "libraries/ESP8266HTTPUpdateServer/src",
    )]
    object_dir.mkdir(parents=True, exist_ok=True)

    common = [
        "-D__ets__", "-DICACHE_FLASH", "-D_GNU_SOURCE", "-DESP8266",
        "-DLWIP_OPEN_SRC", "-DTCP_MSS=536", "-DLWIP_FEATURES=1", "-DLWIP_IPV6=0",
        "-DVTABLES_IN_FLASH",
        "-DMMU_IRAM_SIZE=0x8000", "-DMMU_ICACHE_SIZE=0x8000",
        "-DNONOSDK22x_190703=1", "-DF_CPU=80000000L",
        "-DARDUINO=10819", "-DARDUINO_ESP8266_GENERIC",
        "-DARDUINO_ARCH_ESP8266", '-DARDUINO_BOARD="ESP8266_GENERIC"',
        "-I" + str(core_dir), "-I" + str(variant),
        "-I" + str(sdk / "include"), "-I" + str(sdk / "lwip2/include"),
        "-I" + str(toolroot / "include"), "-I" + str(libc / "include"),
        "-I" + str(sdk / "ssl/bearssl/inc"),
    ]
    for folder in [core_dir / "spiffs", core_dir / "libb64",
                   core_dir / "umm_malloc", *library_dirs]:
        common.append("-I" + str(folder))

    object_files = []
    sources = source_files(core)
    for source_index, source in enumerate(sources, 1):
        if source_index == 1 or source_index % 10 == 0:
            print(f"Compiling {version}: {source_index}/{len(sources)} {source.name}",
                  flush=True)
        suffix = source.suffix
        compiler = bin_dir / ("xtensa-lx106-elf-gcc" if suffix in (".c", ".S")
                              else "xtensa-lx106-elf-g++")
        obj = object_dir / (str(source.relative_to(core)) + ".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        if obj.is_file():
            object_files.append(obj)
            continue
        flags = common + ["-Os", "-g", "-mlongcalls", "-mtext-section-literals",
                          "-free", "-fipa-pta", "-falign-functions=4",
                          "-ffunction-sections", "-fdata-sections",
                          "-MMD", "-MP", "-c", str(source), "-o", str(obj)]
        if suffix == ".cpp":
            flags += ["-std=gnu++17", "-fno-rtti", "-fno-exceptions"]
        elif suffix == ".c":
            flags += ["-std=gnu17", "-fno-inline-functions"]
        temporary = obj.with_name(obj.name + ".tmp")
        flags[flags.index(str(obj))] = str(temporary)
        run([compiler, *flags])
        temporary.replace(obj)
        object_files.append(obj)

    reference_source = Path(__file__).resolve().parents[1] / "examples/network_reference/reference.cpp"
    reference_obj = build_dir / ("reference_" + version + ".cpp.o")
    reference_flags = common + [
        "-DREFERENCE_VERSION=\"" + version + "\"",
        "-DREFERENCE_UPDATE_ENABLED=" + ("1" if version == "A" else "0"),
        "-Os", "-g", "-mlongcalls", "-mtext-section-literals", "-free", "-fipa-pta",
        "-falign-functions=4",
        "-ffunction-sections", "-fdata-sections", "-MMD", "-MP", "-c",
        str(reference_source), "-o", str(reference_obj), "-std=gnu++17", "-fno-rtti",
        "-fno-exceptions",
    ]
    if not reference_obj.is_file():
        run([bin_dir / "xtensa-lx106-elf-g++", *reference_flags])

    ld_script = core / "tools/sdk/ld"
    common_ld = build_dir / "local.eagle.app.v6.common.ld"
    run([bin_dir / "xtensa-lx106-elf-gcc", "-E", "-P", "-x", "c",
         "-DVTABLES_IN_FLASH", "-DMMU_IRAM_SIZE=0x8000", "-DMMU_ICACHE_SIZE=0x8000",
         "-I" + str(ld_script), str(ld_script / "eagle.app.v6.common.ld.h"),
         "-o", common_ld])
    shutil.copy2(ld_script / FLASH_LINKER_SCRIPTS[flash_size],
                 build_dir / "local.eagle.flash.ld")

    sdk_lib = sdk / "lib"
    libc_lib = libc / "lib"
    elf = build_dir / "reference.elf"
    map_file = build_dir / "reference.map"
    archive = build_dir / "core.a"
    run([bin_dir / "xtensa-lx106-elf-gcc-ar", "crs", archive, *map(str, object_files)])
    link = [bin_dir / "xtensa-lx106-elf-gcc", "-fno-exceptions", "-Wl,-EL",
            "-Wl,--no-check-sections",
            "-u", "app_entry", "-u", "_printf_float", "-g", "-Os", "-nostdlib",
            "-Wl,-static", "-Wl,--gc-sections", "-Wl,-wrap,system_restart_local",
            "-Wl,-wrap,spi_flash_read", "-Wl,-Map," + str(map_file),
            "-Tlocal.eagle.flash.ld", "-L" + str(sdk_lib),
            "-L" + str(sdk_lib / "NONOSDK22x_190703"),
            "-L" + str(libc_lib), "-o", str(elf), str(reference_obj),
            "-Wl,--start-group", str(archive), "-lhal", "-lphy", "-lpp", "-lnet80211",
            "-llwip2-536-feat", "-lwpa", "-lcrypto", "-lmain", "-lwps",
            "-lbearssl", "-lespnow", "-lsmartconfig", "-lairkiss", "-lwpa2",
            "-lstdc++", "-lm", "-lc", "-lgcc", "-Wl,--end-group"]
    run(link, cwd=build_dir)

    elf2bin = core / "tools/elf2bin.py"
    firmware = build_dir / "reference.bin"
    run([sys.executable, "-I", elf2bin, "--eboot", core / "bootloaders/eboot/eboot.elf",
         "--app", elf, "--flash_mode", flash_mode, "--flash_freq", FLASH_FREQ,
         "--flash_size", flash_size, "--path", bin_dir, "--out", firmware])
    return elf, firmware


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path,
                        help="Arduino ESP8266 core 3.1.2 directory")
    parser.add_argument("--toolchain", required=True, type=Path,
                        help="directory containing xtensa-lx106-elf/bin")
    parser.add_argument("--out", required=True, type=Path,
                        help="directory for the ELF, BIN, and build cache")
    parser.add_argument("--version", required=True, choices=("A", "B"),
                        help="A accepts OTA requests; B reports itself and ignores them")
    parser.add_argument("--flash-size", choices=tuple(FLASH_LINKER_SCRIPTS), default="4M",
                        help="physical flash size and matching linker map (default: 4M)")
    parser.add_argument("--flash-mode", choices=("dio", "dout"), default="dio",
                        help="SPI flash mode (default: dio)")
    args = parser.parse_args()
    core = args.core.resolve()
    if not (core / "platform.txt").is_file():
        parser.error(f"--core is not an Arduino ESP8266 package: {core}")
    if "version=3.1.2" not in (core / "platform.txt").read_text().splitlines():
        parser.error("the reference build requires Arduino ESP8266 core 3.1.2")
    if args.out.resolve().is_relative_to(core):
        parser.error("--out must be outside the Arduino core directory")
    try:
        bin_dir = toolchain_bin(args.toolchain.resolve())
        args.out.mkdir(parents=True, exist_ok=True)
        key = input_digest(core, bin_dir)
        cache_root = args.out / ".cache" / key
        fixture = Path(__file__).resolve().parents[1] / "examples/network_reference/reference.cpp"
        fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
        variant = f"{args.version}-{args.flash_size}-{args.flash_mode}"
        cache = cache_root / (variant + "-" + fixture_hash[:16])
        elf_cache = cache / "reference.elf"
        bin_cache = cache / "reference.bin"
        marker = cache / "build.json"
        def file_hash(path):
            return hashlib.sha256(path.read_bytes()).hexdigest()
        try:
            saved = json.loads(marker.read_text())
            complete = (saved['bin_sha256'] == file_hash(bin_cache) and
                        saved['elf_sha256'] == file_hash(elf_cache) and
                        saved['version'] == args.version and
                        saved['flash_size'] == args.flash_size and
                        saved['flash_mode'] == args.flash_mode and
                        saved['fixture_sha256'] == fixture_hash and
                        saved['core_inputs_sha256'] == key)
        except (OSError, ValueError, KeyError):
            complete = False
        if not complete:
            if cache.exists():
                shutil.rmtree(cache)
            cache.mkdir(parents=True)
            elf, firmware = compile_reference(core, bin_dir, cache,
                                               cache_root / "objects", args.version,
                                               args.flash_size, args.flash_mode)
            if not elf.is_file() or not firmware.is_file():
                raise RuntimeError("compiler completed without producing ELF and BIN files")
            marker.write_text(json.dumps({'core_version': '3.1.2', 'version': args.version,
                'flash_size': args.flash_size, 'flash_mode': args.flash_mode,
                'linker_script': FLASH_LINKER_SCRIPTS[args.flash_size],
                'core_inputs_sha256': key, 'fixture_sha256': fixture_hash,
                'bin_sha256': file_hash(bin_cache), 'elf_sha256': file_hash(elf_cache)}, indent=2) + '\n')
        output = args.out / variant
        output.mkdir(parents=True, exist_ok=True)
        shutil.copy2(elf_cache, output / "reference.elf")
        shutil.copy2(bin_cache, output / "reference.bin")
        shutil.copy2(marker, output / "build.json")
        print(f"ELF: {output / 'reference.elf'}")
        print(f"BIN: {output / 'reference.bin'}")
        print(f"cache key: {key} ({args.flash_size}, {args.flash_mode})")
    except (ValueError, RuntimeError) as exc:
        print(f"build_reference: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
