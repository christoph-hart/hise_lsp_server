"""
Integration tests for hise-lsp binary.

Spawns the LSP binary as a subprocess, pipes JSON-RPC messages over stdin,
and validates stdout responses. Tests the full protocol including
Content-Length framing.

Usage:
    python test_lsp.py                          # basic protocol tests
    python test_lsp.py --live                   # + live HISE tests (needs HISE running)
    python test_lsp.py --binary path/to/hise-lsp  # custom binary path

Requires: Python 3.6+, no third-party dependencies.
"""

import json
import os
import platform
import queue
import subprocess
import sys
import threading
import time
import unittest


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def default_binary_path():
    """Resolve the default binary path relative to this script."""
    tests_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(tests_dir)

    if platform.system() == "Windows":
        return os.path.join(repo_root, "bin", "windows", "hise-lsp.exe")
    elif platform.system() == "Darwin":
        return os.path.join(repo_root, "bin", "macos", "hise-lsp")
    else:
        return os.path.join(repo_root, "bin", "linux", "hise-lsp")


BINARY = os.environ.get("HISE_LSP_BINARY", default_binary_path())
LIVE_TESTS = False


def encode_message(obj):
    """Encode a JSON object as an LSP message with Content-Length header."""
    body = json.dumps(obj).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    return header + body


def _reader_thread(stdout, msg_queue):
    """Background thread that reads LSP messages from stdout and puts them
    on a queue. Runs until EOF or error."""
    try:
        while True:
            # Read headers byte by byte
            headers = b""
            while True:
                b = stdout.read(1)
                if not b:
                    return  # EOF
                headers += b
                if headers.endswith(b"\r\n\r\n"):
                    break

            # Parse Content-Length
            content_length = None
            for line in headers.decode("ascii").split("\r\n"):
                if line.startswith("Content-Length:"):
                    content_length = int(line.split(":", 1)[1].strip())
                    break

            if content_length is None:
                return

            # Read body
            body = b""
            while len(body) < content_length:
                chunk = stdout.read(content_length - len(body))
                if not chunk:
                    return
                body += chunk

            msg_queue.put(json.loads(body.decode("utf-8")))
    except (OSError, ValueError):
        return  # pipe closed or parse error


# Per-process reader threads and message queues
_readers = {}  # proc -> (thread, queue)


def _ensure_reader(proc):
    """Ensure a reader thread is running for this process."""
    if proc not in _readers:
        q = queue.Queue()
        t = threading.Thread(target=_reader_thread, args=(proc.stdout, q), daemon=True)
        t.start()
        _readers[proc] = (t, q)
    return _readers[proc][1]


def read_message(proc, timeout=5.0):
    """Read a single LSP message from a process's stdout.

    Returns the parsed JSON object, or None on timeout/EOF.
    Uses a background reader thread to handle Windows pipe limitations.
    """
    q = _ensure_reader(proc)
    try:
        return q.get(timeout=timeout)
    except queue.Empty:
        return None


def make_request(method, req_id, params=None):
    """Build a JSON-RPC request object."""
    msg = {"jsonrpc": "2.0", "id": req_id, "method": method}
    if params is not None:
        msg["params"] = params
    return msg


def make_notification(method, params=None):
    """Build a JSON-RPC notification object (no id)."""
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    return msg


def send(proc, msg):
    """Send an LSP message to a process's stdin."""
    proc.stdin.write(encode_message(msg))
    proc.stdin.flush()


def expect_no_message(proc, timeout=0.5):
    """Return True if no message arrives within timeout."""
    result = read_message(proc, timeout=timeout)
    return result is None


# ---------------------------------------------------------------------------
# Subprocess management
# ---------------------------------------------------------------------------

def start_lsp(extra_args=None):
    """Start the LSP binary as a subprocess.

    Uses --port 19999 to ensure connection attempts to HISE fail fast
    (no real HISE running on that port).
    """
    cmd = [BINARY]
    if extra_args:
        cmd.extend(extra_args)
    else:
        # Use a bogus port so HISE connection attempts fail immediately
        cmd.extend(["--port", "19999"])

    return subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def stop_lsp(proc, timeout=5.0):
    """Stop the LSP process gracefully, then forcefully if needed."""
    try:
        proc.stdin.close()
    except Exception:
        pass

    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)

    # Close remaining pipes to avoid ResourceWarning
    for pipe in (proc.stdout, proc.stderr):
        try:
            if pipe:
                pipe.close()
        except Exception:
            pass

    # Clean up reader thread reference
    _readers.pop(proc, None)


# ---------------------------------------------------------------------------
# Protocol Tests
# ---------------------------------------------------------------------------

class TestProtocol(unittest.TestCase):
    """Tests for the LSP protocol behavior (no HISE connection needed)."""

    def setUp(self):
        self.proc = start_lsp()

    def tearDown(self):
        stop_lsp(self.proc)

    def test_initialize_handshake(self):
        """initialize request returns correct capabilities and serverInfo."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        resp = read_message(self.proc)

        self.assertIsNotNone(resp, "Should receive initialize response")
        self.assertEqual(resp["id"], 1)
        self.assertEqual(resp["jsonrpc"], "2.0")

        result = resp["result"]
        caps = result["capabilities"]
        sync = caps["textDocumentSync"]
        self.assertTrue(sync["openClose"])
        self.assertEqual(sync["change"], 1)
        self.assertTrue(sync["save"])

        info = result["serverInfo"]
        self.assertEqual(info["name"], "hise-lsp")
        self.assertEqual(info["version"], "1.0.0")

    def test_initialized_no_response(self):
        """initialized notification produces no response."""
        # Must initialize first
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)  # consume initialize response

        send(self.proc, make_notification("initialized"))
        self.assertTrue(expect_no_message(self.proc, timeout=0.5),
                        "initialized notification should not produce a response")

    def test_did_save_without_hise(self):
        """didSave when HISE is not running produces synthetic error diagnostic."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_notification("textDocument/didSave", {
            "textDocument": {"uri": "file:///D:/test/Scripts/test.js"}
        }))

        resp = read_message(self.proc)
        self.assertIsNotNone(resp, "Should receive publishDiagnostics")
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")

        params = resp["params"]
        self.assertEqual(params["uri"], "file:///D:/test/Scripts/test.js")

        diags = params["diagnostics"]
        self.assertEqual(len(diags), 1)
        self.assertEqual(diags[0]["severity"], 1)  # Error
        self.assertIn("Cannot connect", diags[0]["message"])
        self.assertEqual(diags[0]["range"]["start"]["line"], 0)

    def test_did_open_without_hise(self):
        """didOpen when HISE is not running produces same synthetic error as didSave."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_notification("textDocument/didOpen", {
            "textDocument": {
                "uri": "file:///D:/test/Scripts/test.js",
                "languageId": "hisescript",
                "version": 1,
                "text": ""
            }
        }))

        resp = read_message(self.proc)
        self.assertIsNotNone(resp, "Should receive publishDiagnostics")
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")

        diags = resp["params"]["diagnostics"]
        self.assertEqual(len(diags), 1)
        self.assertIn("Cannot connect", diags[0]["message"])

    def test_did_close_clears_diagnostics(self):
        """didClose clears diagnostics for the file."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_notification("textDocument/didClose", {
            "textDocument": {"uri": "file:///D:/test/Scripts/test.js"}
        }))

        resp = read_message(self.proc)
        self.assertIsNotNone(resp, "Should receive publishDiagnostics")
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")

        params = resp["params"]
        self.assertEqual(params["uri"], "file:///D:/test/Scripts/test.js")
        self.assertEqual(len(params["diagnostics"]), 0)

    def test_did_change_triggers_diagnostics(self):
        """didChange with full content triggers diagnostics (OpenCode compatibility)."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_notification("textDocument/didChange", {
            "textDocument": {"uri": "file:///D:/test/Scripts/test.js", "version": 2},
            "contentChanges": [{"text": "// changed"}]
        }))

        resp = read_message(self.proc)
        self.assertIsNotNone(resp, "Should receive publishDiagnostics")
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")

        params = resp["params"]
        self.assertEqual(params["uri"], "file:///D:/test/Scripts/test.js")

        diags = params["diagnostics"]
        self.assertEqual(len(diags), 1)
        self.assertIn("Cannot connect", diags[0]["message"])

    def test_unknown_request_method_not_found(self):
        """Unknown request method returns MethodNotFound error."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_request("foo/bar", 42))

        resp = read_message(self.proc)
        self.assertIsNotNone(resp, "Should receive error response")
        self.assertEqual(resp["id"], 42)
        self.assertIn("error", resp)
        self.assertEqual(resp["error"]["code"], -32601)
        self.assertIn("foo/bar", resp["error"]["message"])

    def test_unknown_notification_ignored(self):
        """Unknown notification is silently ignored (no response)."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_notification("foo/baz", {}))

        self.assertTrue(expect_no_message(self.proc, timeout=0.5),
                        "Unknown notification should be silently ignored")

    def test_shutdown_and_exit(self):
        """shutdown returns null result, exit terminates process."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_request("shutdown", 99))
        resp = read_message(self.proc)

        self.assertIsNotNone(resp, "Should receive shutdown response")
        self.assertEqual(resp["id"], 99)
        self.assertIn("result", resp)

        send(self.proc, make_notification("exit"))

        # Process should terminate
        exit_code = self.proc.wait(timeout=5)
        self.assertEqual(exit_code, 0, "Process should exit cleanly with code 0")

    def test_eof_exits_cleanly(self):
        """Closing stdin causes clean exit."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        self.proc.stdin.close()

        exit_code = self.proc.wait(timeout=5)
        self.assertEqual(exit_code, 0, "Process should exit cleanly on EOF")

    def test_full_session(self):
        """Full session: initialize → initialized → didSave → didClose → shutdown → exit."""
        # Initialize
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        resp = read_message(self.proc)
        self.assertEqual(resp["id"], 1)

        # Initialized
        send(self.proc, make_notification("initialized"))

        # didSave (will fail to connect to HISE — that's fine, we just check the flow)
        send(self.proc, make_notification("textDocument/didSave", {
            "textDocument": {"uri": "file:///D:/test/Scripts/test.js"}
        }))
        resp = read_message(self.proc)
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")

        # didClose
        send(self.proc, make_notification("textDocument/didClose", {
            "textDocument": {"uri": "file:///D:/test/Scripts/test.js"}
        }))
        resp = read_message(self.proc)
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")
        self.assertEqual(len(resp["params"]["diagnostics"]), 0)

        # Shutdown
        send(self.proc, make_request("shutdown", 2))
        resp = read_message(self.proc)
        self.assertEqual(resp["id"], 2)

        # Exit
        send(self.proc, make_notification("exit"))
        exit_code = self.proc.wait(timeout=5)
        self.assertEqual(exit_code, 0)


# ---------------------------------------------------------------------------
# Live HISE Tests (optional, requires HISE running on port 1900)
# ---------------------------------------------------------------------------

class TestLiveHise(unittest.TestCase):
    """Tests that require HISE running on localhost:1900."""

    def setUp(self):
        if not LIVE_TESTS:
            self.skipTest("Live HISE tests disabled (use --live to enable)")
        # Use default port (1900) for live tests
        self.proc = start_lsp(extra_args=[])

    def tearDown(self):
        stop_lsp(self.proc)

    def test_did_save_real_file(self):
        """didSave on a file included in HISE returns real diagnostics."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        # This test requires knowing a real included file path.
        # Use the HISE_LSP_TEST_FILE env var, or skip.
        test_file = os.environ.get("HISE_LSP_TEST_FILE")
        if not test_file:
            self.skipTest("Set HISE_LSP_TEST_FILE to a .js file included in HISE")

        uri = "file:///" + test_file.replace("\\", "/")
        send(self.proc, make_notification("textDocument/didSave", {
            "textDocument": {"uri": uri}
        }))

        resp = read_message(self.proc, timeout=15)
        self.assertIsNotNone(resp, "Should receive publishDiagnostics")
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")
        self.assertEqual(resp["params"]["uri"], uri)
        # Diagnostics could be empty (clean file) or populated — just check structure
        self.assertIsInstance(resp["params"]["diagnostics"], list)

    def test_did_save_nonexistent_file(self):
        """didSave on a file not included in HISE returns error diagnostic."""
        send(self.proc, make_request("initialize", 1, {"capabilities": {}}))
        read_message(self.proc)

        send(self.proc, make_notification("textDocument/didSave", {
            "textDocument": {"uri": "file:///D:/nonexistent/fake_file_12345.js"}
        }))

        resp = read_message(self.proc, timeout=15)
        self.assertIsNotNone(resp, "Should receive publishDiagnostics")
        self.assertEqual(resp["method"], "textDocument/publishDiagnostics")

        diags = resp["params"]["diagnostics"]
        self.assertGreater(len(diags), 0, "Should have at least one error diagnostic")
        self.assertEqual(diags[0]["severity"], 1)  # Error


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # Parse custom args before passing to unittest
    argv = sys.argv[:]

    if "--live" in argv:
        LIVE_TESTS = True
        argv.remove("--live")

    if "--binary" in argv:
        idx = argv.index("--binary")
        if idx + 1 < len(argv):
            BINARY = argv[idx + 1]
            del argv[idx:idx + 2]
        else:
            print("--binary requires a path argument", file=sys.stderr)
            sys.exit(1)

    if not os.path.isfile(BINARY):
        print(f"Binary not found: {BINARY}", file=sys.stderr)
        print("Build the project first, or use --binary to specify the path.", file=sys.stderr)
        sys.exit(1)

    print(f"Binary: {BINARY}")
    print(f"Live HISE tests: {'enabled' if LIVE_TESTS else 'disabled (use --live)'}")
    print()

    unittest.main(argv=argv, verbosity=2)
