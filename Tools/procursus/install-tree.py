"""Install the staged Procursus tree on the phone. Invoked over SSH by install-procursus.py."""
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import pwd
import re
import stat
import subprocess
import sys
import tempfile

APT_FILES = {"usr/lib/libapt-pkg.6.0.0.dylib", "usr/libexec/apt/methods/gpgv"}
BUILD = "23A343"
# The pinned 1900 bootstrap has no retained package depending on these four. Verify its status and ownership lists before pruning a fresh copy.
STOCK_STATUS_SHA256 = "d351eb63d0f58fd8eb87f2c9bd9e595edb0c19ed58a1c12dd96b3e2a053cc62e"
CLI_OMISSIONS = {
    "essential": "0d5d8a16131f581e25e58a1f04788f1f1d8cb95f287c9bacd2124733242c46df",
    "shshd": "39db381753dfd0b0c5ce8b3e1adfb9c7e2e2b39042b6bdb6ce418105129fd87a",
    "libdimentio0": "65d17c9821499a44fa1a34eebfc549bffd70e71502cf3405295c591dcaf01906",
    "libkrw0": "8f4d48b720837de72c791dd763c2f3db5f5a8182c8078b153eb9402337e0834c",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def real_directory(path):
    return path.is_dir() and path.resolve() == path


def runtime_root(argument):
    if argument:
        root = Path(argument).resolve(strict=True)
    else:
        roots = list(Path("/private/var/mobile/Containers/Shared/AppGroup").glob("*/LSB/iOS/" + BUILD + "/RuntimeRoot"))
        require(len(roots) == 1, "Specify the selected store App Group's RuntimeRoot with --runtime-root")
        root = roots[0]
    require(root.parts[-4:] == ("LSB", "iOS", BUILD, "RuntimeRoot") and real_directory(root), "Expected LSB/iOS/23A343/RuntimeRoot")
    with (root / "System/Library/CoreServices/SystemVersion.plist").open("rb") as source:
        require(plistlib.load(source).get("ProductBuildVersion") == BUILD, "Runtime build must be " + BUILD)
    return root


def require_stopped():
    lib = ctypes.CDLL("/usr/lib/libproc.dylib")
    buffer = ctypes.create_string_buffer(4096)
    for line in subprocess.check_output(["ps", "-axo", "pid=,comm="], text=True).splitlines():
        fields = line.split(None, 1)
        if not fields:
            continue
        pid, name = fields[0], fields[1] if len(fields) == 2 else ""
        if name.startswith(("LSB:", "LSBHelper:")) or (lib.proc_pidpath(int(pid), buffer, len(buffer)) > 0 and
                b"/LiveSimBooter.app/PlugIns/LiveProcess.appex/" in buffer.value):
            raise RuntimeError("Power off the simulator in LiveSimBooter Settings before installing Procursus")


def check_bootstrap(jb):
    status = jb / "Library/dpkg/status"
    require(status.resolve() == status and status.is_file(), "Guest dpkg database missing or redirected")
    versions = {}
    for paragraph in status.read_text().split("\n\n"):
        fields = dict(line.split(": ", 1) for line in paragraph.splitlines() if not line.startswith((" ", "\t")) and ": " in line)
        if fields.get("Status", "").split()[1:] == ["ok", "installed"]:
            versions[fields.get("Package")] = (fields.get("Version"), fields.get("Architecture"))
    require(all(versions.get(p) == ("2.9.4-1", "iphoneos-arm64") for p in ("apt", "libapt-pkg6.0")), "Expected installed Procursus APT 2.9.4-1")


def check_layout(root):
    data = root.parent / "data"
    require(real_directory(data), "Guest data must be a real directory")
    require(not os.path.lexists(data / "var") or real_directory(data / "var"), "Guest data/var must be a real directory")
    jb, var = data / "var/jb", root / "var"
    if os.path.lexists(var):
        require(real_directory(var), "RuntimeRoot/var must be a real directory")
    alias = var / "jb"
    if os.path.lexists(alias):
        require(alias.is_symlink() and os.readlink(alias) == "../../data/var/jb", "Conflicting RuntimeRoot/var/jb entry")
    if os.path.lexists(jb):
        require(real_directory(jb), "Guest data/var/jb must be a real directory")
        check_bootstrap(jb)
        return jb, alias, "patch"
    return jb, alias, "install"


def sha256(path):
    with path.open("rb") as source:
        digest = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def stock_images(stage, staged):
    stock = json.loads((stage / "stock-code.json").read_text())
    require(APT_FILES <= stock.keys(), "Prepared manifest is missing the APT pair")
    for name in stock:
        relative = Path(name)
        require(not relative.is_absolute() and ".." not in relative.parts and relative.parts, "Invalid stock image path")
        src = staged / relative
        require(src.resolve() == src and src.is_file(), f"Missing staged image: {relative}")
    return stock


def configure_fresh_bootstrap(root, staged):
    with (root / "System/Library/CoreServices/SystemVersion.plist").open("rb") as source:
        runtime = plistlib.load(source)
    version = runtime.get("ProductVersion")
    require(runtime.get("ProductBuildVersion") == BUILD and isinstance(version, str) and re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", version),
            "Expected the pinned guest firmware version")
    db = staged / "Library/dpkg"
    info, status = db / "info", db / "status"
    require(real_directory(info) and sha256(status) == STOCK_STATUS_SHA256, "Fresh installation requires the original 1900 package database")
    require(real_directory(db / "updates") and not any((db / "updates").iterdir()) and
            not os.path.lexists(db / "status-old") and not os.path.lexists(db / "triggers"),
            "Unexpected dpkg transaction in prepared bootstrap")
    declaration = info / "uikittools.triggers"
    require(list(info.glob("*.triggers")) == [declaration] and declaration.resolve() == declaration and declaration.is_file() and
            declaration.read_bytes() == b"interest /var/jb/Applications\n", "Unexpected 1900 package trigger declarations")
    apps = staged / "Applications"
    require(not os.path.lexists(apps) or (real_directory(apps) and not any(apps.iterdir())), "Expected an empty fresh package Applications directory")
    cache = staged / "var/cache/apt"
    require(real_directory(cache) and not any(os.path.lexists(cache / name) for name in ("pkgcache.bin", "srcpkgcache.bin")),
            "Expected a fresh APT cache")
    apt = staged / "etc/apt"
    require(real_directory(apt), "Expected a real guest APT configuration directory")
    parts = apt / "apt.conf.d"
    parts.mkdir(exist_ok=True)
    require(real_directory(parts), "Redirected guest APT configuration directory")
    # APT would otherwise reintroduce the omitted Essential metapackage during a full upgrade.
    with (parts / "50livesimbooter-cli").open("x") as config:
        config.write('pkgCacheGen::Essential "installed";\n')

    omitted, retained = set(), set()
    for listing in info.glob("*.list"):
        require(listing.resolve() == listing and listing.is_file(), f"Redirected package file list: {listing.name}")
        paths = listing.read_text().splitlines()
        if listing.stem in CLI_OMISSIONS:
            require(sha256(listing) == CLI_OMISSIONS[listing.stem], f"Unexpected stock file list: {listing.name}")
            omitted.update(paths)
        else:
            retained.update(paths)
    require(all((info / (n + ".list")).is_file() for n in CLI_OMISSIONS), "Missing stock package file list")
    for name in sorted(omitted - {"/.", "/var", "/var/jb"}, key=lambda item: -item.count("/")):
        relative = name[len("/var/jb/"):]
        require(name.startswith("/var/jb/") and not any(p in ("", ".", "..") for p in relative.split("/")), f"Invalid package path: {name}")
        path = staged / relative
        require(path.resolve() == path, f"Redirected package path: {name}")
        if path.is_dir():
            if name not in retained:
                path.rmdir()
        else:
            require(path.is_file() and name not in retained, f"Missing or shared package file: {name}")
            path.unlink()
    for package in CLI_OMISSIONS:
        for metadata in info.glob(package + ".*"):
            require(metadata.resolve() == metadata and metadata.is_file(), f"Invalid package metadata: {metadata.name}")
            metadata.unlink()
    # darwintools generates this zero-payload package and /. file list directly in dpkg's database.
    # Read the guest OS version instead of running its native-path firmware or user-account setup scripts.
    omitted_headers = {"Package: " + name for name in CLI_OMISSIONS}
    records = [p for p in status.read_text().strip().split("\n\n") if p.partition("\n")[0] not in omitted_headers]
    records.append(f"Package: firmware\nStatus: install ok installed\nEssential: yes\nPriority: required\nSection: System\n"
                   f"Architecture: all\nVersion: {version}\nInstalled-Size: 0\nMaintainer: LiveSimBooter <guest@localhost>\nDescription: Simulator OS firmware")
    with (info / "firmware.list").open("x") as listing:
        listing.write("/.\n")
    (info / "firmware.md5sums").touch(exist_ok=False)
    status.unlink()
    with status.open("x") as output:
        output.write("\n\n".join(records) + "\n\n")
    # dpkg 1.22.6 trig_incorporate() treats a missing Unincorp as a pre-trigger database and activates all declared interests.
    # With no package apps to register, record the existing file interest and an empty deferred queue.
    triggers = db / "triggers"
    triggers.mkdir()
    (triggers / "File").write_bytes(b"/var/jb/Applications uikittools\n")
    (triggers / "Unincorp").touch(exist_ok=False)


def plan_patch(staged, jb, stock):
    changes, preserved = [], 0
    for name, original in stock.items():
        relative = Path(name)
        src, dst = staged / relative, jb / relative
        if dst.resolve() != dst or not dst.is_file():
            require(name not in APT_FILES, f"Guest APT file is missing or redirected: {relative}")
            continue
        current = sha256(dst)
        if current != original and name not in APT_FILES:
            preserved += 1
            continue
        if current != sha256(src):
            changes.append(relative)
    # Change only the shipped aliases with their original absolute target. Preserve custom and external links.
    for directory, dirs, files in os.walk(staged):
        for name in dirs + files:
            src = Path(directory) / name
            if not src.is_symlink():
                continue
            relative = src.relative_to(staged)
            dst = jb / relative
            if dst.parent.resolve() != dst.parent or not dst.is_symlink():
                continue
            target = os.readlink(dst)
            if target == "/var/jb" or target.startswith("/var/jb/"):
                expected = os.path.relpath(jb / target[len("/var/jb"):].lstrip("/"), dst.parent)
                if os.readlink(src) == expected:
                    changes.append(relative)
    return changes, preserved


def activate(root, stage, expected_mode):
    jb, alias, mode = check_layout(root)
    require(mode == expected_mode, "Guest bootstrap changed during preparation; rerun the installer")
    staged = stage / "var/jb"
    require(real_directory(staged), "Invalid staged var/jb")
    check_bootstrap(staged)
    stock = stock_images(stage, staged)
    changes, preserved = [], 0
    if mode == "patch":
        changes, preserved = plan_patch(staged, jb, stock)
    else:
        configure_fresh_bootstrap(root, staged)
    backup = Path(tempfile.mkdtemp(dir=root.parent, prefix="procursus-backup-")) if changes else None
    applied, added_alias = [], False
    try:
        if not alias.is_symlink():
            alias.parent.mkdir(exist_ok=True)
            alias.symlink_to("../../data/var/jb")
            added_alias = True
        if mode == "install":
            jb.parent.mkdir(exist_ok=True)
            os.rename(staged, jb)
        for relative in changes:
            src, dst, previous = staged / relative, jb / relative, backup / relative
            previous.parent.mkdir(parents=True, exist_ok=True)
            if dst.is_symlink():
                previous.symlink_to(os.readlink(dst))
            else:
                os.link(dst, previous)
                src.chmod(stat.S_IMODE(dst.stat().st_mode) & ~(stat.S_ISUID | stat.S_ISGID))
            applied.append(relative)
            os.replace(src, dst)
    except BaseException:
        for relative in reversed(applied):
            os.replace(backup / relative, jb / relative)
        if added_alias:
            alias.unlink()
        raise
    return dict(mode=mode, root=str(root), changed=len(changes), existing_nonstock_images_kept=preserved, rollback=str(backup) if backup else None)


def main():
    operation, argument = sys.argv[1:3]
    require(operation in ("inspect", "apply"), "Unknown installer operation")
    mobile = pwd.getpwnam("mobile")
    if os.getuid() == 0:
        os.setgroups([])
        os.setgid(mobile.pw_gid)
        os.setuid(mobile.pw_uid)
    require(os.getuid() == mobile.pw_uid, "SSH must run as mobile or root")
    root = runtime_root(argument)
    require_stopped()
    _, _, mode = check_layout(root)
    if operation == "inspect":
        print(json.dumps(dict(root=str(root), mode=mode)))
        return
    require(len(sys.argv) == 4 and sys.argv[3] == mode, "Installation mode changed since discovery")
    lock = os.open(root.parent, os.O_RDONLY)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with tempfile.TemporaryDirectory(prefix=".procursus-incoming-", dir=root.parent) as directory:
            subprocess.run(["tar", "-xpf", "-", "--no-same-owner", "-C", directory], check=True)
            require_stopped()
            print(json.dumps(activate(root, Path(directory), mode)))
    finally:
        os.close(lock)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"Procursus installer: {error}")
