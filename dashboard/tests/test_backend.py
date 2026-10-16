"""Tests for the dashboard HTTP API. They start the real backend on a spare port, pointed
at an address no Pi answers on, with console windows turned off.

    py -m unittest discover -s dashboard/tests -v
"""
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "backend", "server.py")
PORT = 8097


def request(method, path, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    h = {"Host": f"localhost:{PORT}"}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    h.update(headers or {})
    conn.request(method, path, body=data, headers=h)
    r = conn.getresponse()
    raw = r.read()
    conn.close()
    try:
        return r.status, json.loads(raw)
    except ValueError:
        return r.status, raw.decode("utf-8", errors="replace")


class Api(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runs = tempfile.mkdtemp(prefix="encdash-test-")
        env = dict(os.environ, ENC_DASH_PORT=str(PORT), ENC_DASH_CONSOLES="0", ENC_DASH_RUNS=cls.runs,
                   ENC_PI_HOST="203.0.113.1")          # TEST-NET-3: never answers
        cls.proc = subprocess.Popen([sys.executable, "-u", SERVER], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(50):
            try:
                request("GET", "/api/state")
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("backend did not start: " + cls.proc.stdout.read(2000).decode())

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.proc.wait()
        shutil.rmtree(cls.runs, ignore_errors=True)

    def wait_step(self, step, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, s = request("GET", "/api/state")
            st = s["steps"].get(step)
            if st and st["state"] != "running":
                return s
            time.sleep(0.3)
        self.fail(f"step {step} still running after {timeout} s")

    def test_1_page_served(self):
        status, body = request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("<title>QNX Encryption Workflow</title>", body)
        for f in ("/app.js", "/charts.js", "/style.css"):
            self.assertEqual(request("GET", f)[0], 200, f)

    def test_2_starts_empty(self):
        status, s = request("GET", "/api/state")
        self.assertEqual(status, 200)
        self.assertIsNone(s["pi"])
        self.assertIsNone(s["pc"])
        self.assertEqual(s["steps"], {})
        self.assertIsNone(s["busy"])
        self.assertFalse(s["server"]["running"])
        self.assertEqual(s["jobs"]["count"], 0)
        self.assertEqual(s["analytics"], {"benchmark": None, "roundtrips": [], "inflight": [], "multiclient": []})
        self.assertIsNone(s["blocked"]["connect"])
        self.assertIn("step 1", s["blocked"]["build"])

    def test_3_workflow_order(self):
        _, wf = request("GET", "/api/workflow")
        ids = [x["id"] for x in wf["steps"]]
        self.assertEqual(ids, ["connect", "build", "deploy", "key", "server", "roundtrip", "concurrency",
                               "multiclient", "benchmark", "finish"])
        self.assertEqual([x["n"] for x in wf["steps"]], list(range(1, 11)))

    def test_4_locked_step_refused(self):
        status, r = request("POST", "/api/steps/deploy/run", {"options": {}})
        self.assertEqual(status, 409)
        self.assertIn("step 2", r["message"])

    def test_5_other_sites_refused(self):
        status, _ = request("POST", "/api/steps/connect/run", {}, {"Origin": "http://evil.example"})
        self.assertEqual(status, 403)
        status, _ = request("GET", "/api/state", headers={"Host": "evil.example"})
        self.assertEqual(status, 403)
        conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
        conn.request("POST", "/api/reset", body="x", headers={"Host": f"localhost:{PORT}", "Content-Type": "text/plain"})
        self.assertEqual(conn.getresponse().status, 415)
        conn.close()

    def test_6_bad_option_refused(self):
        request("POST", "/api/reset", {})
        status, r = request("POST", "/api/steps/connect/run", {"options": {"bogus": 1}})
        self.assertEqual(status, 200)       # unknown options are ignored
        self.wait_step("connect")

    def test_7_connect_fails_cleanly_without_pi(self):
        request("POST", "/api/reset", {})
        status, r = request("POST", "/api/steps/connect/run", {"options": {}})
        self.assertEqual(status, 200, r)
        status, r = request("POST", "/api/steps/connect/run", {"options": {}})
        self.assertEqual(status, 409)       # one step at a time
        s = self.wait_step("connect")
        st = s["steps"]["connect"]
        self.assertEqual(st["state"], "failed")
        self.assertIn("does not answer", st["error"])
        self.assertIsNone(s["busy"])
        self.assertTrue(os.path.isfile(os.path.join(self.runs, s["session"]["id"], st["log"])))

    def test_8_reset_clears(self):
        status, _ = request("POST", "/api/reset", {})
        self.assertEqual(status, 200)
        _, s = request("GET", "/api/state")
        self.assertEqual(s["steps"], {})
        self.assertIsNone(s["pc"])

    def test_9_no_benchmark_yet(self):
        status, _ = request("GET", "/api/benchmark.csv")
        self.assertEqual(status, 404)


if __name__ == "__main__":
    unittest.main()
