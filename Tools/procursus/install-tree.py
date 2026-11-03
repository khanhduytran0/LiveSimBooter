"""Install the staged Procursus tree on the phone. Invoked over SSH by install-procursus.py."""
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import pwd
import stat
import subprocess
import sys
import tempfile

APT_FILES = {"usr/lib/libapt-pkg.6.0.0.dylib", "usr/libexec/apt/methods/gpgv"}
BUILD = "23A343"


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
