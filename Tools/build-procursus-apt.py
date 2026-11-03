#!/usr/bin/env python3
"""Cross-build the Procursus 1900 APT 2.9.4 acquisition library and gpgv method for an arm64 iPhone guest."""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


RECIPES = Path(__file__).resolve().parent / "procursus"
PROCURSUS = "https://raw.githubusercontent.com/ProcursusTeam/Procursus/b6185221bf1ce3dda8b4baeee61e75e3f7c7d399/"
BOOTSTRAP_SHA256 = "8354c3aa1ecdad8ebc47d9a76dfca6f830a2b757278068bd33b98bf1d638a9cb"
DYLIB = "libapt-pkg.6.0.0.dylib"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_input(cache, record):
    expected, source = record.split()
    if source.startswith("build_patch/"):
        url = PROCURSUS + source
    elif source.startswith("pool/"):
        url = "https://apt.procurs.us/" + source
    else:
        url = source
    name = "apt-2.9.4.tar.gz" if url.startswith("https://codeload.github.com/") else Path(source).name
    path = cache / name
    if path.is_file() and sha256(path) == expected:
        return path
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        download = Path(temporary) / name
        subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--proto", "=https", "--proto-redir", "=https",
                        "--retry", "2", "--output", download, url], check=True)
        if sha256(download) != expected:
            raise ValueError(f"SHA-256 mismatch for {url}")
        os.replace(download, path)
    return path


def prepare(stage, bootstrap, inputs):
    source = stage / "apt-2.9.4"
    sysroot = stage / "sysroot"
    sysroot.mkdir()
    subprocess.run(["tar", "-xf", bootstrap, "-C", sysroot, "./var/jb/usr/lib/*.dylib"], check=True)
    for path in inputs:
        if path.suffix == ".deb":
            subprocess.run(["dpkg-deb", "-x", path, sysroot], check=True)
        elif path.suffix == ".gz":
            subprocess.run(["tar", "-xf", path, "-C", stage], check=True)
    project_patches = [RECIPES / name for name in ("apt-gpgv-spawn.patch", "apt-libcxx.patch", "apt-worker-spawn.patch")]
    for path in [p for p in inputs if p.suffix == ".diff"] + project_patches:
        patch = path.read_bytes().replace(b"@MEMO_PREFIX@", b"/var/jb").replace(b"@MEMO_SUB_PREFIX@", b"/usr")
        subprocess.run(["patch", "--batch", "-p1"], input=patch, cwd=source, check=True)
    # Same source moves as Procursus build_info/apt.mk; algorithms uses Foundation.
    for name in ("apt-private/private-output.cc", "apt-pkg/algorithms.cc"):
        (source / name).rename((source / name).with_suffix(".mm"))
    shutil.copyfile(source / "apt-pkg/memrchr.cc", source / "ftparchive/memrchr.cc")
    return source, sysroot / "var/jb/usr"


def build(stage, source, prefix):
    build_dir = stage / "build"
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    triehash = stage / "triehash"
    triehash.write_bytes(b"#!/usr/bin/perl -w\n" + (prefix / "bin/triehash").read_bytes().split(b"\n", 1)[1])
    triehash.chmod(0o755)
    include = prefix / "include"
    lib = prefix / "lib"
    cflags = shlex.join(["-include", str(RECIPES / "apt-ios.h"), "-I" + str(RECIPES), "-I" + str(include),
                         "-D_DARWIN_C_SOURCE", "-Wno-deprecated-declarations"])
    definitions = {
        "CMAKE_SYSTEM_NAME": "iOS", "CMAKE_MACOSX_BUNDLE": "OFF", "CMAKE_OSX_ARCHITECTURES": "arm64", "CMAKE_OSX_SYSROOT": sdk,
        "CMAKE_OSX_DEPLOYMENT_TARGET": "16.0", "CMAKE_BUILD_TYPE": "Release", "CMAKE_INSTALL_PREFIX": "/var/jb/usr",
        "CMAKE_C_COMPILER": subprocess.check_output(["xcrun", "-f", "clang"], text=True).strip(),
        "CMAKE_CXX_COMPILER": subprocess.check_output(["xcrun", "-f", "clang++"], text=True).strip(),
        "CMAKE_CXX_FLAGS": cflags, "CMAKE_SHARED_LINKER_FLAGS": os.path.relpath(lib / "libiosexec.dylib", build_dir),
        "CMAKE_BUILD_WITH_INSTALL_RPATH": "ON", "CMAKE_INSTALL_RPATH": "/var/jb/usr/lib", "CMAKE_POLICY_VERSION_MINIMUM": "3.5",
        "TRIEHASH_EXECUTABLE": triehash, "BERKELEY_INCLUDE_DIRS": include / "db181", "BERKELEY_LIBRARIES": lib / "libdb-18.1.a",
        "Intl_INCLUDE_DIR": include, "Intl_LIBRARY": lib / "libintl.dylib", "USE_NLS": "ON", "WITH_TESTS": "OFF", "WITH_DOC": "OFF",
        "CURRENT_VENDOR": "procursus", "COMMON_ARCH": "iphoneos-arm64", "ROOT_GROUP": "wheel", "DPKG_DATADIR": "/var/jb/usr/share/dpkg",
        "STATE_DIR": "/var/jb/var/lib/apt", "CACHE_DIR": "/var/jb/var/cache/apt", "LOG_DIR": "/var/jb/var/log/apt", "CONF_DIR": "/var/jb/etc/apt",
    }
    for key, name in [("GCRYPT", "gcrypt"), ("XXHASH", "xxhash"), ("LZ4", "lz4"), ("LZMA", "lzma"), ("ZSTD", "zstd")]:
        definitions[key + "_INCLUDE_DIRS"] = include
        definitions[key + "_LIBRARIES"] = lib / f"lib{name}.dylib"
    subprocess.run(["cmake", "-G", "Ninja", "-S", source, "-B", build_dir, *[f"-D{k}={v}" for k, v in definitions.items()]], check=True)
    subprocess.run(["cmake", "--build", build_dir, "--target", "apt-pkg", "gpgv"], check=True)
    return build_dir / "apt-pkg" / DYLIB, build_dir / "methods/gpgv"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", type=Path, required=True, help="original Procursus 1900 bootstrap-iphoneos-arm64.tar.zst")
    parser.add_argument("--work", type=Path, required=True, help="work directory for verified downloads and temporary builds")
    parser.add_argument("--output", type=Path, required=True, help="local staging directory for the two resulting Mach-O files")
    args = parser.parse_args()
    bootstrap = args.bootstrap.resolve(strict=True)
    if sha256(bootstrap) != BOOTSTRAP_SHA256:
        parser.error("bootstrap SHA-256 does not match the pinned Procursus 1900 iphoneos-arm64 archive")
    cache = args.work.resolve() / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    records = [line for line in (RECIPES / "build-inputs.sha256").read_text().splitlines() if line and not line.startswith("#")]
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        inputs = list(pool.map(lambda line: fetch_input(cache, line), records))
    with tempfile.TemporaryDirectory(prefix="build-", dir=cache.parent) as temporary:
        stage = Path(temporary)
        source, prefix = prepare(stage, bootstrap, inputs)
        products = build(stage, source, prefix)
        args.output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".apt-", dir=args.output) as incoming:
            for product in products:
                shutil.copy2(product, Path(incoming) / product.name)
            for product in products:
                destination = args.output / product.name
                os.replace(Path(incoming) / product.name, destination)
                print(destination.resolve())


if __name__ == "__main__":
    main()
