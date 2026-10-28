#!/usr/bin/env python3
"""Exercise the queue protocol locally without SSH, device access, or launchctl execution."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("guest_launchctl", Path(__file__).resolve().parents[1] / "Tools/guest-launchctl.py")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class QueueTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="LiveSimBooter-control-")
        self.root = Path(self.temporary.name).resolve()
        (self.root / "RuntimeRoot").mkdir()
        self.data = self.root / "data"
        self.queue = self.data / "var/run/launchctl-control"
        self.queue.mkdir(parents=True)
        self.identifier = "a" * 32

    def tearDown(self):
        self.temporary.cleanup()

    def remote(self, operation, identifier=None, arguments=None):
        request = {"id": identifier or self.identifier, "operation": operation,
                   "arguments": arguments or [], "data_root": str(self.data)}
        result = subprocess.run([sys.executable, "-c", client.REMOTE], input=json.dumps(request).encode(),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        return json.loads(result.stdout)

    def submit(self, identifier=None):
        self.assertEqual(self.remote("submit", identifier, ["list"])["state"], "submitted")

    def status(self, identifier=None, wait_error=0):
        identifier = identifier or self.identifier
        result = {"id": identifier, "pid": 123, "spawn_error": 0, "wait_error": wait_error,
                  "status": 0, "exit": 0 if not wait_error else -1, "signal": 0, "timed_out": False}
        (self.queue / (identifier + ".json")).write_text(json.dumps(result))
        (self.queue / (identifier + ".log")).write_bytes(b"literal output\n")

    def test_literal_arguments_and_busy_queue(self):
        arguments = ["print", "user/501/test;$(touch /tmp/should-not-run)", "日本語"]
        self.assertEqual(self.remote("submit", arguments=arguments)["state"], "submitted")
        expected = self.identifier.encode() + b"\0" + b"\0".join(value.encode() for value in arguments) + b"\0"
        self.assertEqual((self.queue / "request").read_bytes(), expected)
        self.assertEqual(self.remote("submit", "b" * 32, ["list"])["state"], "error")
        self.assertEqual((self.queue / "request").read_bytes(), expected)
        self.assertEqual(json.loads((self.queue / "client.lock/owner.json").read_text())["id"], self.identifier)

    def test_same_active_waits_for_worker_cleanup(self):
        self.submit()
        (self.queue / "request").rename(self.queue / "active")
        self.status()
        self.assertEqual(self.remote("poll")["state"], "pending")
        self.assertTrue((self.queue / "client.lock").is_dir())
        (self.queue / "active").unlink()
        self.assertEqual(self.remote("poll")["state"], "complete")
        self.assertFalse((self.queue / "client.lock").exists())

    def test_old_result_retrievable_during_new_active(self):
        self.submit()
        (self.queue / "request").unlink()
        self.status()
        self.assertEqual(self.remote("poll")["state"], "complete")
        new_identifier = "b" * 32
        self.submit(new_identifier)
        (self.queue / "request").rename(self.queue / "active")
        self.assertEqual(self.remote("poll")["state"], "complete")
        self.assertTrue((self.queue / "active").exists())
        self.assertEqual(json.loads((self.queue / "client.lock/owner.json").read_text())["id"], new_identifier)
        self.assertEqual(self.remote("poll", new_identifier)["state"], "pending")

    def test_unknown_wait_preserves_evidence(self):
        self.submit()
        (self.queue / "request").rename(self.queue / "active")
        self.status(wait_error=60)
        self.assertEqual(self.remote("poll")["state"], "unknown")
        self.assertTrue((self.queue / "client.lock").is_dir())
        self.assertTrue((self.queue / "active").is_file())

    def test_status_symlink_rejected(self):
        self.submit()
        outside = self.root / "other.json"
        outside.write_text(json.dumps({"id": self.identifier}))
        (self.queue / (self.identifier + ".json")).symlink_to(outside)
        self.assertEqual(self.remote("poll")["state"], "error")
        self.assertTrue((self.queue / "client.lock").is_dir())


if __name__ == "__main__":
    unittest.main()
