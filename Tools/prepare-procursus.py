#!/usr/bin/env python3
"""Prepare a signed Procursus 1900 tree with patched APT for the guest's data directory."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

APT_FILES = {"usr/lib/libapt-pkg.6.0.0.dylib": 6, "usr/libexec/apt/methods/gpgv": 2}


def tool(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def inventory(jb):
    """Traverse the fresh private stage, never the referents of its symlinks."""
    regular = {}
    for directory, dirs, files in os.walk(jb):
        for name in dirs + files:
            path = Path(directory) / name
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                target = os.readlink(path)
                if target == "/var/jb" or target.startswith("/var/jb/"):
                    destination = jb / target[len("/var/jb"):].lstrip("/")
                    path.unlink()
                    path.symlink_to(os.path.relpath(destination, path.parent))
            elif stat.S_ISREG(metadata.st_mode):
                # The guest owns these files as mobile. Root-owned setuid/setgid semantics do not apply.
                if metadata.st_mode & (stat.S_ISUID | stat.S_ISGID):
                    path.chmod(stat.S_IMODE(metadata.st_mode) & ~(stat.S_ISUID | stat.S_ISGID))
                regular.setdefault((metadata.st_dev, metadata.st_ino), []).append(path)
            elif not stat.S_ISDIR(metadata.st_mode):
                raise ValueError(f"Unexpected entry in Procursus bootstrap: {path}")
    return regular.values()


def prepare(args):
    apt = tool("build-procursus-apt")
    runtime = tool("setup-ios26-runtime")
    signer = tool("sign-sim-executable")
    archive = args.bootstrap.resolve(strict=True)
    if apt.sha256(archive) != apt.BOOTSTRAP_SHA256:
        raise ValueError("Expected the original Procursus 1900 iphoneos-arm64 bootstrap archive")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", args.identity) or not args.identifier:
        raise ValueError("Supply the signing certificate SHA-1 and host bundle identifier")
    output = args.output.absolute()
    if os.path.lexists(output):
        raise ValueError(f"Staging directory already exists: {output}")
    replacements = {}
    for relative, kind in APT_FILES.items():
        source = args.apt_output / Path(relative).name
        info = runtime.macho(source)
        if not info or info["subtype"] != 0 or info["filetype"] != kind or 2 not in info["platforms"]:
            raise ValueError(f"Expected an arm64 iOS APT build product: {source}")
        if kind == 6 and info.get("install_name") != "@rpath/libapt-pkg.6.0.dylib":
            raise ValueError(f"Unexpected APT library install name: {source}")
        replacements[relative] = source

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".procursus-", dir=output.parent) as directory:
        work = Path(directory)
        # bsdtar rejects traversing members and extraction through a symlink. Extract only from the verified archive, as the calling user.
        subprocess.run(["/usr/bin/tar", "-xpf", archive, "--no-same-owner", "--no-xattrs", "-C", work, "./var/jb"], check=True)
        jb = work / "var/jb"
        images, stock = [], {}
        for paths in inventory(jb):
            path = paths[0]
            info = runtime.macho(path)
            if info:
                if info["subtype"] or info["filetype"] not in (2, 6, 8):
                    raise ValueError(f"Unsupported Procursus image: {path}")
                digest = runtime.sha(path)
                stock.update((alias.relative_to(jb).as_posix(), digest) for alias in paths)
                images.append(path)
        for relative, replacement in replacements.items():
            if relative not in stock:
                raise ValueError(f"Missing original APT image: {relative}")
            shutil.copyfile(replacement, jb / relative)
        helper = signer.cms_helper(args, work)
        print(f"Signing {len(images)} guest images", flush=True)

        def sign(path):
            with tempfile.TemporaryDirectory(dir=work) as scratch:
                signed = Path(scratch) / "signed"
                runtime.sign_binary(path, signed, args.identity, args.identifier, helper, signer)
                # Update the private staging inode so all archive hard links retain their identity.
                mode = stat.S_IMODE(path.stat().st_mode)
                path.chmod(mode | stat.S_IWUSR)
                shutil.copyfile(signed, path)
                path.chmod(mode)

        with ThreadPoolExecutor(max_workers=4) as pool:
            for _ in pool.map(sign, images):
                pass
        manifest = work / "stock-code.json"
        manifest.write_text(json.dumps(stock, indent=2, sort_keys=True) + "\n")
        output.mkdir()
        try:
            manifest.rename(output / manifest.name)
            (work / "var").rename(output / "var")
        except OSError:
            if not manifest.exists():
                (output / manifest.name).unlink()
            output.rmdir()
            raise
    print(f"Prepared {output / 'var/jb'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", type=Path, required=True, help="original bootstrap-iphoneos-arm64.tar.zst")
    parser.add_argument("--apt-output", type=Path, required=True, help="products from Tools/build-procursus-apt.py")
    parser.add_argument("--output", type=Path, required=True, help="new local directory that will contain var/jb")
    parser.add_argument("--identity", required=True, help="signing certificate SHA-1")
    parser.add_argument("--identifier", required=True, help="signed host app identifier")
    parser.add_argument("--cms-helper", type=Path, help="existing approved sign-code-directory tool")
    try:
        prepare(parser.parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"prepare-procursus: {error}\n")


if __name__ == "__main__":
    main()
