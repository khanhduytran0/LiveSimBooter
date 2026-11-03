#!/usr/bin/env python3
"""Install or patch the pinned Procursus bootstrap in a stopped LiveSimBooter guest over SSH."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
ARCHIVE_URL = "https://apt.procurs.us/bootstraps/1900/bootstrap-iphoneos-arm64.tar.zst"


def remote(args, operation, root, mode=None, payload=None):
    code = (TOOLS / "procursus/install-tree.py").read_text()
    command = ["python3", "-c", code, operation, root]
    if mode:
        command.append(mode)
    ssh = ["ssh", "-C", "-p", str(args.port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
    for option in args.ssh_option:
        ssh.extend(["-o", option])
    result = subprocess.run(ssh + ["--", args.host, shlex.join(command)], stdin=payload or subprocess.DEVNULL,
                            stdout=subprocess.PIPE)
    if result.returncode:
        raise SystemExit(f"Procursus {operation} failed on {args.host} (SSH exit {result.returncode})")
    return json.loads(result.stdout)


def build(args, stage):
    builder = TOOLS / "build-procursus-apt.py"
    archive = args.bootstrap
    if archive is None:
        recipe = runpy.run_path(str(builder))
        downloads = args.work / "downloads"
        downloads.mkdir(parents=True, exist_ok=True)
        archive = recipe["fetch_input"](downloads, recipe["BOOTSTRAP_SHA256"] + " " + ARCHIVE_URL)
    products, prepared = stage / "apt", stage / "prepared"
    subprocess.run([sys.executable, builder, "--bootstrap", archive, "--work", args.work, "--output", products], check=True)
    command = [sys.executable, TOOLS / "prepare-procursus.py", "--bootstrap", archive, "--apt-output", products,
               "--output", prepared, "--identity", args.identity, "--identifier", args.identifier]
    if args.cms_helper:
        command.extend(["--cms-helper", args.cms_helper])
    subprocess.run(command, check=True)
    return prepared


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Native iPhone SSH destination (mobile or root)")
    parser.add_argument("--port", type=int, default=2222, help="Native SSH port (default 2222)")
    parser.add_argument("--ssh-option", action="append", default=[], help="Additional ssh -o option (repeatable)")
    parser.add_argument("--runtime-root", help="Selected physical RuntimeRoot on the phone; detected if unique")
    parser.add_argument("--work", type=Path, default=TOOLS.parent / "tmp/procursus", help="Local build/download workspace")
    parser.add_argument("--bootstrap", type=Path, help="Original 1900 bootstrap-iphoneos-arm64.tar.zst; downloaded if omitted")
    parser.add_argument("--identity", help="Signing certificate SHA-1")
    parser.add_argument("--identifier", help="Native host bundle ID for signing")
    parser.add_argument("--cms-helper", type=Path, help="Previously built sign-code-directory for this signing key")
    parser.add_argument("--prepared", type=Path, help="Deploy an existing signed output of prepare-procursus.py")
    args = parser.parse_args()
    if args.prepared and any((args.bootstrap, args.identity, args.identifier, args.cms_helper)):
        parser.error("Choose --prepared or the build/sign options")
    if not args.prepared and not (args.identity and args.identifier):
        parser.error("Building requires --identity and --identifier")
    target = remote(args, "inspect", args.runtime_root or "")
    print(f"Will {target['mode']} Procursus at {target['root']}", flush=True)
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="install-", dir=args.work) as temporary:
        stage = Path(temporary)
        prepared = args.prepared or build(args, stage)
        payload = stage / "procursus.tar"
        subprocess.run(["/usr/bin/tar", "--no-xattrs", "-cf", payload, "-C", prepared, "stock-code.json", "var/jb"],
                       env={**os.environ, "COPYFILE_DISABLE": "1"}, check=True)
        with payload.open("rb") as source:
            print(json.dumps(remote(args, "apply", target["root"], target["mode"], source), indent=2))


if __name__ == "__main__":
    main()
