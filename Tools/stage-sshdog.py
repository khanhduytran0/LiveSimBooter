#!/usr/bin/env python3
"""Stage a signed sshdog release, supplied public authorization keys, and a reusable RSA PEM host key under ignored tmp/."""
import argparse
import base64
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import struct
import subprocess
import sys


RELEASE = "https://github.com/NyaMisty/sshdog_ios/releases/tag/v1.1"
ASSET = "https://github.com/NyaMisty/sshdog_ios/releases/download/v1.1/sshdog"
RELEASE_SHA256 = "9aaec8f86fde670c5141754e98f6853c149a117e195ada9e0babb4963a11608c"
LABEL = "com.kdt.LiveSimBooter.sshdog"
PUBLIC_KEY_TYPES = {"ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521"}


def validate_authorized_keys(path):
    data = path.read_bytes()
    if not data.strip() or b"PRIVATE KEY" in data:
        raise ValueError("authorized-keys must contain public keys, never a private key or an empty file")
    keys = 0
    for line in data.decode("utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) < 2 or fields[0] not in PUBLIC_KEY_TYPES:
            raise ValueError("authorized-keys requires bare public-key lines; upstream sshdog does not enforce OpenSSH key options")
        blob = base64.b64decode(fields[1], validate=True)
        if len(blob) < 4:
            raise ValueError("invalid public key")
        length = struct.unpack_from(">I", blob)[0]
        if blob[4:4 + length] != fields[0].encode():
            raise ValueError("public-key type does not match its encoded key")
        keys += 1
    if not keys:
        raise ValueError("authorized-keys contains no public keys")
    subprocess.run(["ssh-keygen", "-l", "-f", str(path)], check=True, stdout=subprocess.DEVNULL)
    return data if data.endswith(b"\n") else data + b"\n"


def host_public_key(path):
    if not path.read_bytes().startswith(b"-----BEGIN RSA PRIVATE KEY-----\n"):
        raise ValueError("host-key must be an unencrypted PKCS#1 RSA PEM key; generate it with ssh-keygen -t rsa -m PEM")
    result = subprocess.run(["ssh-keygen", "-y", "-P", "", "-f", str(path)], check=True, capture_output=True)
    public = result.stdout.strip()
    if not public.startswith(b"ssh-rsa "):
        raise ValueError("host-key is not RSA")
    result = subprocess.run(["ssh-keygen", "-l", "-f", "-"], input=public + b"\n", check=True, capture_output=True)
    if int(result.stdout.split()[0]) < 2048:
        raise ValueError("host-key must have at least 2048 bits")
    return public + b"\n"


def write_file(path, data, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
    path.chmod(mode)


def stage(options):
    repository = Path(__file__).resolve().parents[1]
    output = options.output.resolve()
    try:
        relative = output.relative_to((repository / "tmp").resolve())
    except ValueError:
        raise ValueError("output must be a fresh directory under the repository's ignored tmp/")
    if not relative.parts or output.exists():
        raise ValueError("output must be a new staging directory; existing keys/configuration are never overwritten")
    group = PurePosixPath(options.device_group)
    nested_names = {"LSB", "iOS", "23A343", "RuntimeRoot", "data", "LiveSimBooter", "SimulatorData"}
    if not group.is_absolute() or group == PurePosixPath("/") or ".." in group.parts or group.name in nested_names:
        raise ValueError("device-group must be the selected physical store App Group directory, above LSB")
    if not 1 <= options.port <= 32767:
        raise ValueError("sshdog v1.1 uses a signed 16-bit port; choose 1..32767")
    if options.ssh_host and any(character.isspace() or character in "@,[]" for character in options.ssh_host):
        raise ValueError("ssh-host must be a hostname or address without a username, brackets, or whitespace")
    binary = options.binary.resolve(strict=True)
    binary_bytes = binary.read_bytes()
    header = binary_bytes[:32]
    input_sha256 = hashlib.sha256(binary_bytes).hexdigest()
    if len(header) != 32 or struct.unpack_from("<I", header)[0] != 0xfeedfacf or struct.unpack_from("<I", header, 4)[0] != 0x100000c:
        raise ValueError("binary must be a thin arm64 Mach-O")
    if struct.unpack_from("<I", header, 12)[0] != 2:
        raise ValueError("binary must retain MH_EXECUTE")
    subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(binary)], check=True)
    authorized = validate_authorized_keys(options.authorized_keys)
    public = host_public_key(options.host_key)
    storage = group / "LSB/iOS/23A343"
    root = storage / "RuntimeRoot"
    data = storage / "data"
    program = "/usr/local/libexec/sshdog"
    log = "/private/var/mobile/Library/Logs/" + LABEL + ".log"
    configuration = {
        "Label": LABEL, "Program": program, "ProgramArguments": [program, str(options.port)],
        "RunAtLoad": True, "KeepAlive": False,
        "WorkingDirectory": "/private/var/mobile/Library/sshdog", "StandardOutPath": log, "StandardErrorPath": log,
        "EnvironmentVariables": {
            "HOME": str(data), "CFFIXED_USER_HOME": str(data), "GOMAXPROCS": "2", "LSB_WAIT4_STATUS_ONLY": "1",
            "PATH": ":".join(str(root / item) for item in ("bin", "usr/bin", "usr/sbin", "usr/local/bin")),
            "SSHDOG_SHELL": str(root / "bin/fish"), "XDG_CONFIG_HOME": str(data / "Library/sshdog/xdg")}}
    output.mkdir(parents=True)
    destination = output / "RuntimeRoot/usr/local/libexec/sshdog"
    destination.parent.mkdir(parents=True)
    shutil.copyfile(binary, destination)
    destination.chmod(0o755)
    write_file(output / ("RuntimeRoot/System/Library/LaunchDaemons/" + LABEL + ".plist"), plistlib.dumps(configuration), 0o644)
    config = output / "data/Library/sshdog/config"
    config.mkdir(parents=True, mode=0o700)
    write_file(config / "authorized_keys", authorized, 0o600)
    write_file(config / "port", (str(options.port) + "\n").encode(), 0o644)
    write_file(config / "ssh_host_rsa_key", options.host_key.read_bytes(), 0o600)
    write_file(config / "ssh_host_rsa_key.pub", public, 0o644)
    write_file(output / "data/Library/sshdog/xdg/fish/config.fish",
               b"status job-control none\nset -g fish_use_posix_spawn 1\n", 0o644)
    if options.ssh_host:
        entry = "[{}]:{} ".format(options.ssh_host, options.port).encode() + public
        write_file(output / "client/known_hosts", entry, 0o644)
    runtime_files = sorted(str(path.relative_to(output / "RuntimeRoot")) for path in (output / "RuntimeRoot").rglob("*") if path.is_file())
    data_files = sorted(str(path.relative_to(output / "data")) for path in (output / "data").rglob("*") if path.is_file())
    manifest = {"reference_release": RELEASE, "reference_asset": ASSET, "original_release_sha256": RELEASE_SHA256,
                "signed_binary_input": str(binary), "signed_binary_input_sha256": input_sha256,
                "runtime_root": str(root), "data_root": str(data), "job_label": LABEL, "port": options.port,
                "shell": str(root / "bin/fish"), "runtime_files": runtime_files, "data_files": data_files,
                "host_key_type": "RSA PKCS#1 PEM", "deployment_performed": False}
    write_file(output / "stage.json", (json.dumps(manifest, indent=2) + "\n").encode(), 0o644)
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="sshdog already signed using Tools/sign-sim-executable.py")
    parser.add_argument("--authorized-keys", type=Path, required=True, help="one or more bare OpenSSH public-key lines")
    parser.add_argument("--host-key", type=Path, required=True, help="persistent, unencrypted RSA PEM host key")
    parser.add_argument("--device-group", required=True, help="selected store App Group directory on the phone; storage is its LSB/iOS/23A343 subdirectory")
    parser.add_argument("--output", type=Path, required=True, help="new stage under ignored tmp/")
    parser.add_argument("--port", type=int, default=2223)
    parser.add_argument("--ssh-host", help="optional hostname/address for client/known_hosts; no user@ prefix")
    options = parser.parse_args()
    try:
        stage(options)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, "stage-sshdog: " + str(error) + "\n")


if __name__ == "__main__":
    main()
