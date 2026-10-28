#!/usr/bin/env python3
"""Run one literal launchctl command through the guest's already running control job."""
import argparse
import base64
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import uuid


# Arguments and paths travel on stdin as JSON; only this fixed program is quoted for the SSH command shell.
REMOTE = r'''
import base64, json, os, pathlib, re, stat, sys

request = json.load(sys.stdin)
identifier = request["id"]
if not re.fullmatch(r"[0-9a-f]{32}", identifier):
    raise ValueError("invalid request ID")
root = pathlib.Path(request["data_root"])
if not root.is_absolute() or root.name != "data":
    raise ValueError("data root must name an absolute data directory")
root = root.resolve(strict=True)
if root.name != "data" or not (root.parent / "RuntimeRoot").is_dir():
    raise ValueError("data must have a sibling RuntimeRoot directory")
directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
for component in ("var", "run", "launchctl-control"):
    next_directory = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
    os.close(directory)
    directory = next_directory
os.fchdir(directory)
os.close(directory)

def exists(name):
    return os.path.lexists(name)

def read_regular(name, limit):
    descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("not a regular file: " + name)
        data = stream.read(limit + 1)
    return data[:limit], len(data) > limit

def owner_matches():
    try:
        data, truncated = read_regular("client.lock/owner.json", 4096)
        return not truncated and json.loads(data)["id"] == identifier
    except (FileNotFoundError, ValueError, KeyError):
        return False

def release_lock():
    if owner_matches():
        os.unlink("client.lock/owner.json")
        os.rmdir("client.lock")

def active_matches():
    try:
        prefix, _ = read_regular("active", 33)
    except FileNotFoundError:
        return False
    return prefix == identifier.encode() + b"\0"

def output_for_status(status, state):
    try:
        output, truncated = read_regular(identifier + ".log", 16 * 1024 * 1024)
    except FileNotFoundError:
        output, truncated = b"", False
    return {"state": state, "status": status, "output": base64.b64encode(output).decode(), "output_truncated": truncated}

published = False
locked = False
temporary_created = False
temporary = "." + identifier + ".new"
try:
    if request["operation"] == "submit":
        arguments = request["arguments"]
        if not 1 <= len(arguments) <= 63 or any(not isinstance(a, str) or not a or "\0" in a for a in arguments):
            raise ValueError("requires 1..63 nonempty arguments without NUL")
        payload = identifier.encode() + b"\0" + b"\0".join(a.encode() for a in arguments) + b"\0"
        if len(payload) > 16384:
            raise ValueError("request exceeds 16384 bytes")
        os.mkdir("client.lock", 0o700)
        locked = True
        with open("client.lock/owner.json", "x") as stream:
            json.dump({"id": identifier, "arguments": arguments}, stream)
        if any(exists(name) for name in ("request", "active", identifier + ".json", identifier + ".json.new", identifier + ".log", temporary)):
            raise FileExistsError("queue busy or request ID already used")
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
        temporary_created = True
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fchmod(stream.fileno(), 0o644)
            os.fsync(stream.fileno())
        # link publishes the complete file atomically and, unlike rename, cannot replace another pending request.
        os.link(temporary, "request", follow_symlinks=False)
        published = True
        os.unlink(temporary)
        answer = {"state": "submitted", "id": identifier}
    elif request["operation"] == "poll":
        try:
            data, truncated = read_regular(identifier + ".json", 16384)
        except FileNotFoundError:
            answer = {"state": "pending", "queued": exists("request"), "active": exists("active")}
        else:
            if truncated:
                raise ValueError("oversize status")
            status = json.loads(data)
            if status.get("id") != identifier:
                raise ValueError("status request ID mismatch")
            if status.get("wait_error"):
                answer = output_for_status(status, "unknown")
            elif active_matches():
                answer = {"state": "pending", "active": True}
            else:
                answer = output_for_status(status, "complete")
                release_lock()
    else:
        raise ValueError("unknown operation")
except Exception as error:
    if locked and not published:
        if temporary_created and exists(temporary):
            os.unlink(temporary)
        release_lock()
    answer = {"state": "error", "published": published, "error": str(error)}
print(json.dumps(answer))
'''


def invoke_remote(options, operation, identifier, arguments, timeout):
    command = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-p", str(options.port)]
    for option in options.ssh_option:
        command += ["-o", option]
    command += [options.host, "python3 -c " + shlex.quote(REMOTE)]
    request = {"operation": operation, "id": identifier, "arguments": arguments, "data_root": options.data_root}
    result = subprocess.run(command, input=json.dumps(request).encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace").strip() or "SSH exited " + str(result.returncode))
    return json.loads(result.stdout)


def save_result(folder, identifier, answer):
    status = answer["status"]
    output = base64.b64decode(answer["output"], validate=True)
    (folder / (identifier + ".json")).write_text(json.dumps(status, indent=2) + "\n")
    (folder / (identifier + ".log")).write_bytes(output)
    sys.stdout.buffer.write(output)
    sys.stdout.buffer.flush()
    print("\nrequest {}: {}".format(identifier, json.dumps(status, sort_keys=True)), file=sys.stderr)
    if answer.get("output_truncated"):
        print("Local output stops at 16 MiB; the complete log remains on the device.", file=sys.stderr)
    if answer["state"] == "unknown":
        print("Child outcome remains unknown; active request and client lock are preserved for inspection.", file=sys.stderr)
        return 125
    if status.get("timed_out"):
        return 124
    if status.get("spawn_error") or status.get("wait_error"):
        return 125
    return min(255, 128 + status["signal"]) if status.get("signal") else max(0, min(255, status["exit"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="SSH destination, for example root@device.local")
    parser.add_argument("--port", type=int, default=2222)
    parser.add_argument("--data-root", required=True, help="physical data directory on the device")
    parser.add_argument("--output-dir", type=Path, required=True, help="local directory for request metadata, output and status")
    parser.add_argument("--timeout", type=float, default=90, help="maximum seconds to await a result (default 90)")
    parser.add_argument("--ssh-option", action="append", default=[], help="additional SSH -o option; repeatable")
    parser.add_argument("--request-id", help="retrieve a previous request without submitting or repeating it")
    parser.add_argument("arguments", nargs=argparse.REMAINDER, help="-- LAUNCHCTL_SUBCOMMAND [ARGUMENT ...]")
    options = parser.parse_args()
    arguments = options.arguments[1:] if options.arguments[:1] == ["--"] else options.arguments
    if not options.host or options.host.startswith("-") or not 1 <= options.port <= 65535:
        parser.error("invalid SSH host or port")
    if not 0 < options.timeout <= 3600:
        parser.error("timeout must be between 0 and 3600 seconds")
    data = Path(options.data_root)
    if not data.is_absolute() or data.name != "data" or ".." in data.parts:
        parser.error("data-root must name an absolute data directory without '..'")
    if options.request_id:
        if arguments or not re.fullmatch(r"[0-9a-f]{32}", options.request_id):
            parser.error("request-id needs 32 lowercase hex digits and no command arguments")
    elif not 1 <= len(arguments) <= 63 or any(not argument or "\0" in argument for argument in arguments):
        parser.error("provide 1..63 nonempty launchctl arguments without NUL")
    if sum(len(argument.encode()) + 1 for argument in arguments) + 33 > 16384:
        parser.error("request exceeds 16384 bytes")
    identifier = options.request_id or uuid.uuid4().hex
    options.output_dir.mkdir(parents=True, exist_ok=True)
    record = options.output_dir / (identifier + ".request.json")
    if not record.exists():
        record.write_text(json.dumps({"id": identifier, "host": options.host, "port": options.port,
                                     "data_root": options.data_root, "arguments": arguments}, indent=2) + "\n")
    print("request " + identifier, file=sys.stderr, flush=True)
    deadline = time.monotonic() + options.timeout
    if not options.request_id:
        try:
            answer = invoke_remote(options, "submit", identifier, arguments, min(15, options.timeout))
            if answer["state"] == "error" and not answer.get("published"):
                print("Request not submitted: " + answer["error"], file=sys.stderr)
                return 75
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            print("Submission acknowledgement unavailable; checking the same ID without resubmitting: " + str(error), file=sys.stderr)
    last_error = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            answer = invoke_remote(options, "poll", identifier, [], min(15, remaining))
            if answer["state"] in ("complete", "unknown"):
                return save_result(options.output_dir, identifier, answer)
            if answer["state"] == "error":
                last_error = answer["error"]
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            last_error = str(error)
        time.sleep(min(2, max(0, deadline - time.monotonic())))
    print("Result pending or unknown; no command was repeated. Retrieve with --request-id " + identifier, file=sys.stderr)
    if last_error:
        print("Last retrieval error: " + last_error, file=sys.stderr)
    return 125


if __name__ == "__main__":
    sys.exit(main())
