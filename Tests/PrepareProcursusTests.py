#!/usr/bin/env python3
"""Exercise Procursus staging and alias handling without a keychain or device."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("prepare", Path(__file__).resolve().parents[1] / "Tools/prepare-procursus.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


class ProcursusTests(unittest.TestCase):
    def test_prepare(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, products, output = root / "bootstrap.tar", root / "apt", root / "prepared"
            products.mkdir()
            with tarfile.open(archive, "w") as tar:
                def member(relative, kind=tarfile.REGTYPE, body=b"", target="", mode=0o755):
                    entry = tarfile.TarInfo("./var/jb" + ("/" + relative if relative else ""))
                    entry.type, entry.linkname, entry.mode, entry.size = kind, target, mode, len(body)
                    tar.addfile(entry, io.BytesIO(body))

                for name in ("", "usr", "usr/bin", "usr/lib", "usr/libexec", "usr/libexec/apt", "usr/libexec/apt/methods", "bin", "etc"):
                    member(name, tarfile.DIRTYPE)
                for relative in prepare.APT_FILES:
                    member(relative, body=b"image-stock")
                    (products / Path(relative).name).write_bytes(b"image-patched")
                member("usr/bin/stock", body=b"image-stock", mode=0o4755)
                member("usr/bin/hardlink", tarfile.LNKTYPE, target="./var/jb/usr/bin/stock")
                member("bin/tool", tarfile.SYMTYPE, target="/var/jb/usr/bin/stock")
                member("bin/jb", tarfile.SYMTYPE, target="/var/jb")
                member("usr/bin/relative", tarfile.SYMTYPE, target="stock")
                member("etc/time", tarfile.SYMTYPE, target="/var/db/timezone/localtime")
                member("etc/sibling", tarfile.SYMTYPE, target="/var/jb-other/file")
                member("etc/config", body=b"preserved", mode=0o444)

            args = SimpleNamespace(bootstrap=archive, output=output, apt_output=products, identity="a" * 40, identifier="app", cms_helper=None)
            apt = SimpleNamespace(BOOTSTRAP_SHA256=digest(archive), sha256=digest)

            def macho(path):
                if path.read_bytes().startswith(b"image"):
                    return {"filetype": 6 if path.suffix == ".dylib" else 2, "platforms": [2], "subtype": 0,
                            "install_name": "@rpath/libapt-pkg.6.0.dylib"}

            def sign(source, dest, *unused):
                dest.write_bytes(source.read_bytes() + b"-signed")

            runtime = SimpleNamespace(macho=macho, sha=digest, sign_binary=sign)
            signer = SimpleNamespace(cms_helper=lambda args, work: None)
            modules = {"build-procursus-apt": apt, "setup-ios26-runtime": runtime, "sign-sim-executable": signer}
            with patch.object(prepare, "tool", modules.__getitem__):
                prepare.prepare(args)
                jb = output / "var/jb"
                for name in prepare.APT_FILES:
                    self.assertEqual((jb / name).read_bytes(), b"image-patched-signed")
                self.assertEqual((jb / "usr/bin/stock").read_bytes(), b"image-stock-signed")
                self.assertTrue((jb / "usr/bin/hardlink").samefile(jb / "bin/tool"))
                self.assertEqual(os.readlink(jb / "bin/tool"), "../usr/bin/stock")
                self.assertEqual(os.readlink(jb / "bin/jb"), "..")
                self.assertEqual(os.readlink(jb / "etc/time"), "/var/db/timezone/localtime")
                self.assertEqual(os.readlink(jb / "etc/sibling"), "/var/jb-other/file")
                self.assertEqual(os.readlink(jb / "usr/bin/relative"), "stock")
                self.assertEqual(stat.S_IMODE((jb / "usr/bin/stock").stat().st_mode), 0o755)
                self.assertEqual(stat.S_IMODE((jb / "etc/config").stat().st_mode), 0o444)
                self.assertEqual((jb / "etc/config").read_bytes(), b"preserved")
                manifest = json.loads((output / "stock-code.json").read_bytes())
                self.assertEqual(set(manifest), {"usr/bin/hardlink", "usr/bin/stock", *prepare.APT_FILES})
                self.assertEqual(set(manifest.values()), {hashlib.sha256(b"image-stock").hexdigest()})
                self.assertEqual(digest(archive), apt.BOOTSTRAP_SHA256)
                with self.assertRaisesRegex(ValueError, "already exists"):
                    prepare.prepare(args)
                args.output = root / "failed"
                with patch.object(runtime, "sign_binary", side_effect=ValueError("signing failed")):
                    with self.assertRaisesRegex(ValueError, "signing failed"):
                        prepare.prepare(args)
                self.assertFalse(args.output.exists())
                with patch.object(apt, "BOOTSTRAP_SHA256", "0" * 64):
                    with self.assertRaisesRegex(ValueError, "original Procursus"):
                        prepare.prepare(args)


if __name__ == "__main__":
    unittest.main()
