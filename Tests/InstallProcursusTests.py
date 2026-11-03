#!/usr/bin/env python3
"""Test the on-device tree update locally, without an iPhone or signing identity."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import stat
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("install", Path(__file__).resolve().parents[1] / "Tools/procursus/install-tree.py")
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)

LIB = "usr/lib/libapt-pkg.6.0.0.dylib"
METHOD = "usr/libexec/apt/methods/gpgv"
IMAGES = (LIB, METHOD, "usr/bin/gpgv", "usr/bin/cat", "usr/bin/removed")
STATUS = "\n\n".join(f"Package: {name}\nVersion: 2.9.4-1\nArchitecture: iphoneos-arm64\nStatus: install ok installed" for name in ("apt", "libapt-pkg6.0"))


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def fixture(jb, staged):
    write(jb / "Library/dpkg/status", STATUS.encode())
    for name in IMAGES:
        write(jb / name, (("signed " if staged else "stock ") + name).encode())
    (jb / "bin").mkdir()
    (jb / "bin/cat").symlink_to("../usr/bin/cat" if staged else "/var/jb/usr/bin/cat")
    (jb / "bin/manual").symlink_to("../usr/bin/gpgv" if staged else "/custom/verifier")
    (jb / "bin/time").symlink_to("/var/db/timezone/localtime")


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.base = Path(self.work.name).resolve() / "LSB/iOS/23A343"
        self.root = self.base / "RuntimeRoot"
        write(self.root / "System/Library/CoreServices/SystemVersion.plist", plistlib.dumps({"ProductBuildVersion": "23A343"}))
        self.jb = self.base / "data/var/jb"
        self.jb.parent.mkdir(parents=True)
        self.alias = self.root / "var/jb"

    def staged(self, label="incoming"):
        stage = self.base / label
        fixture(stage / "var/jb", True)
        hashes = {name: hashlib.sha256(("stock " + name).encode()).hexdigest() for name in IMAGES}
        (stage / "stock-code.json").write_text(json.dumps(hashes))
        return stage

    def test_fresh_and_repeat(self):
        stage = self.staged()
        self.jb.parent.rmdir()  # A new guest data directory may not have var yet.
        self.assertEqual(install.runtime_root(str(self.root)), self.root)
        self.assertEqual(install.check_layout(self.root), (self.jb, self.alias, "install"))
        self.assertEqual(install.activate(self.root, stage, "install")["mode"], "install")
        self.assertEqual(os.readlink(self.alias), "../../data/var/jb")
        self.assertTrue(self.alias.samefile(self.jb))
        self.assertTrue((self.jb / "bin/cat").samefile(self.jb / "usr/bin/cat"))
        self.assertFalse((stage / "var/jb").exists())
        result = install.activate(self.root, self.staged("repeat"), "patch")
        self.assertEqual(result["changed"], 0)
        self.assertIsNone(result["rollback"])
        self.assertFalse(list(self.base.glob("procursus-backup-*")))

    def test_incomplete_fresh_stage_is_never_published(self):
        stage = self.staged()
        for relative in (LIB, METHOD, "usr/bin/cat"):
            path = stage / "var/jb" / relative
            value = path.read_bytes()
            path.unlink()
            with self.assertRaisesRegex(ValueError, "Missing staged image"):
                install.activate(self.root, stage, "install")
            self.assertFalse(os.path.lexists(self.alias))
            self.assertFalse(os.path.lexists(self.jb))
            path.write_bytes(value)
        manifest = stage / "stock-code.json"
        stock = json.loads(manifest.read_bytes())
        del stock[METHOD]
        manifest.write_text(json.dumps(stock))
        with self.assertRaisesRegex(ValueError, "missing the APT pair"):
            install.activate(self.root, stage, "install")
        self.assertFalse(os.path.lexists(self.jb))

    def test_patch_preserves_guest_data_and_backs_up_changed_inodes(self):
        fixture(self.jb, False)
        (self.jb / "usr/bin/removed").unlink()
        (self.jb / "usr/bin/gpgv").write_bytes(b"locally installed")
        (self.jb / "usr/bin/cat").chmod(0o4751)
        write(self.jb / "var/lib/apt/lists/my-cache", b"my cached list")
        (self.jb / METHOD).write_bytes(b"previous method build")  # APT pair may already have a different signature.
        inode = (self.jb / LIB).stat().st_ino
        result = install.activate(self.root, self.staged(), "patch")
        self.assertEqual(result["changed"], 4)  # Both APT images, stock cat, and its alias.
        backup = Path(result["rollback"])
        self.assertEqual((backup / LIB).stat().st_ino, inode)
        self.assertEqual((backup / METHOD).read_bytes(), b"previous method build")
        self.assertEqual(os.readlink(backup / "bin/cat"), "/var/jb/usr/bin/cat")
        for name in (LIB, METHOD, "usr/bin/cat"):
            self.assertEqual((self.jb / name).read_bytes(), ("signed " + name).encode())
        self.assertEqual(stat.S_IMODE((self.jb / "usr/bin/cat").stat().st_mode), 0o751)
        self.assertEqual((self.jb / "usr/bin/gpgv").read_bytes(), b"locally installed")
        self.assertFalse((self.jb / "usr/bin/removed").exists())
        self.assertEqual((self.jb / "var/lib/apt/lists/my-cache").read_bytes(), b"my cached list")
        self.assertEqual(os.readlink(self.jb / "bin/manual"), "/custom/verifier")
        self.assertEqual(os.readlink(self.jb / "bin/time"), "/var/db/timezone/localtime")
        self.assertEqual(install.activate(self.root, self.staged("again"), "patch")["changed"], 0)

    def test_wrong_version_mode_or_alias_fails_before_swapping(self):
        stage = self.staged()
        with self.assertRaises(ValueError):
            install.activate(self.root, stage, "patch")
        fixture(self.jb, False)
        status = self.jb / "Library/dpkg/status"
        status.write_text(STATUS.replace("Version: 2.9.4-1", "Version: 2.9.5", 1))
        with self.assertRaisesRegex(ValueError, "2.9.4-1"):
            install.activate(self.root, stage, "patch")
        status.write_text(STATUS)
        self.alias.parent.mkdir()
        self.alias.symlink_to("/unrelated")
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            install.activate(self.root, stage, "patch")
        self.assertEqual((self.jb / LIB).read_bytes(), ("stock " + LIB).encode())
        self.assertEqual(os.readlink(self.alias), "/unrelated")
        self.assertFalse(list(self.base.glob("procursus-backup-*")))

    def test_symlinked_directories_and_manifest_traversal(self):
        fixture(self.jb, False)
        stage = self.staged()
        outside = self.base / "outside"
        (self.jb / "usr/libexec").rename(outside)
        (self.jb / "usr/libexec").symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "redirected"):
            install.activate(self.root, stage, "patch")
        self.assertEqual((outside / "apt/methods/gpgv").read_bytes(), ("stock " + METHOD).encode())
        (self.jb / "usr/libexec").unlink()
        outside.rename(self.jb / "usr/libexec")
        manifest = stage / "stock-code.json"
        hashes = json.loads(manifest.read_bytes())
        hashes["../bin/escape"] = "0" * 64
        manifest.write_text(json.dumps(hashes))
        with self.assertRaisesRegex(ValueError, "Invalid stock"):
            install.activate(self.root, stage, "patch")
        self.assertFalse(self.alias.is_symlink())
        self.assertFalse(list(self.base.glob("procursus-backup-*")))

    def test_mid_update_failure_restores_original_bytes_inodes_and_alias(self):
        fixture(self.jb, False)
        stage = self.staged()
        inodes = {name: (self.jb / name).stat().st_ino for name in IMAGES}
        replace = os.replace

        def injected(source, dest):
            if source == stage / "var/jb/bin/cat":  # Fails after the binary swaps have succeeded.
                raise OSError("injected error")
            replace(source, dest)

        with patch.object(install.os, "replace", injected), self.assertRaisesRegex(OSError, "injected"):
            install.activate(self.root, stage, "patch")
        self.assertEqual(inodes, {name: (self.jb / name).stat().st_ino for name in IMAGES})
        for name in IMAGES:
            self.assertEqual((self.jb / name).read_bytes(), ("stock " + name).encode())
        self.assertEqual(os.readlink(self.jb / "bin/cat"), "/var/jb/usr/bin/cat")
        self.assertFalse(self.alias.is_symlink())


if __name__ == "__main__":
    unittest.main()
