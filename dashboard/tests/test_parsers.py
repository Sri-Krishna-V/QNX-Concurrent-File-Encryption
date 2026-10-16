"""Tests for dashboard/backend/parsers.py, on output in the exact formats the C programs print.

    py -m unittest discover -s dashboard/tests -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backend"))

import parsers  # noqa: E402

ENCRYPT = """
encclient  encrypt  examples/sensor-log.csv -> /dev/shmem/encdash/sensor-log.csv.enc
  input        sensor-log.csv  181.8 KB (186,167 bytes)
  Data Blocks  1 of up to 2.0 MB, 1 in flight at once
  progress     ############################## 1/1 Data Blocks
  server       4 workers, 12 Segments in 1 Job, 5.871 ms encrypting
  wall time    9.412 ms  (18.9 MB/s)
  saved as     /dev/shmem/encdash/sensor-log.csv.enc  181.9 KB (130 B readable header + 186,167 B encrypted)
  first bytes
    original    74 69 6d 65 73 74 61 6d  70 2c 74 65 6d 70 2c 68   timestamp,temp,h
    encrypted   9a 01 ff 3c 22 5e 61 0b  c4 d2 18 7a 90 11 aa 03   ...<"^a....z....
  status       OK encrypted, every Data Block verified
""".splitlines()

DECRYPT = """
encclient  decrypt  /dev/shmem/encdash/hello.txt.enc -> /dev/shmem/encdash/hello.txt.out
  header       QNX-ENC v1, original name hello.txt, 397 B, 1 Data Block
  progress     ############################## 1/1 Data Blocks
  wall time    0.321 ms on this client, 1 thread
  saved as     /dev/shmem/encdash/hello.txt.out  397 B (397 bytes)
  first bytes
    encrypted   9a 01 ff 3c 22 5e 61 0b  c4 d2 18 7a 90 11 aa 03   ...<"^a....z....
    decrypted   48 65 6c 6c 6f 2c 20 51  4e 58 21 0a 54 68 69 73   Hello, QNX!.This
  status       OK decrypted
""".splitlines()

BANNER = """
encserver ready
  name      /dev/name/local/encsvc
  workers   4
  cipher    AES-256-CTR, builtin backend, self-test passed
  limits    2.0 MB per Data Block, 8 Jobs at once

  JOB    CLIENT          SIZE   SEGMENTS         TIME   WORKERS (segments each)
""".splitlines()


class Rows(unittest.TestCase):
    def test_encrypt(self):
        r = parsers.encrypt_output(ENCRYPT)
        self.assertTrue(r["ok"])
        self.assertTrue(r["verified"])
        self.assertEqual(r["name"], "sensor-log.csv")
        self.assertEqual(r["bytes"], 186167)
        self.assertEqual((r["blocks"], r["in_flight"], r["block_size"]), (1, 1, 2097152))
        self.assertEqual((r["workers"], r["segments"], r["jobs"]), (4, 12, 1))
        self.assertAlmostEqual(r["server_ms"], 5.871)
        self.assertAlmostEqual(r["wall_ms"], 9.412)
        self.assertEqual(r["header_bytes"], 130)
        self.assertTrue(r["hex_original"].startswith("74 69 6d"))
        self.assertEqual(len(r["hex_encrypted"].split()), 16)

    def test_encrypt_failure(self):
        r = parsers.encrypt_output(["encclient: !! cannot reach \"encsvc\": No such file or directory. Is encserver running?"])
        self.assertFalse(r["ok"])
        self.assertIn("encserver running", r["errors"][0])

    def test_decrypt(self):
        r = parsers.decrypt_output(DECRYPT)
        self.assertTrue(r["ok"])
        self.assertAlmostEqual(r["wall_ms"], 0.321)
        self.assertEqual(r["bytes"], 397)
        self.assertTrue(r["hex_decrypted"].startswith("48 65 6c 6c"))

    def test_bench_summary(self):
        s = parsers.bench_summary("4 2097152 10 50123 49001 52000 51000")
        self.assertEqual(s["workers"], 4)
        self.assertAlmostEqual(s["enc_mean_ms"], 50.123)
        self.assertIsNone(parsers.bench_summary("encclient: !! request failed"))


class Server(unittest.TestCase):
    def test_banner(self):
        b = parsers.server_banner(BANNER)
        self.assertEqual(b, {"ready": True, "workers": 4, "backend": "builtin", "max_block": "2.0 MB", "max_jobs": 8})

    def test_received(self):
        ev = parsers.server_line("  #12   -> pid 4231      2.0 MB  received, 3 Jobs in progress")
        self.assertEqual((ev["kind"], ev["id"], ev["pid"], ev["bytes"], ev["in_progress"]),
                         ("received", 12, 4231, 2097152, 3))

    def test_done(self):
        ev = parsers.server_line("  #7    OK pid 458       2.0 MB         16    35.488 ms   "
                                 " w0 #### 4  w1 ### 3  w2 ##### 5  w3 #### 4 ")
        self.assertEqual(ev["kind"], "done")
        self.assertEqual(ev["segments"], 16)
        self.assertAlmostEqual(ev["ms"], 35.488)
        self.assertEqual(ev["per_worker"], [4, 3, 5, 4])

    def test_done_idle_worker(self):
        ev = parsers.server_line("  #3    OK pid 77         397 B          1     0.051 ms    w0 # 1  w1 0  w2 0  w3 0 ")
        self.assertEqual(ev["per_worker"], [1, 0, 0, 0])

    def test_failed(self):
        ev = parsers.server_line("  #0    !! pid 990       2.9 MB  rejected: Message too long")
        self.assertEqual((ev["kind"], ev["id"]), ("failed", 0))
        self.assertIn("Message too long", ev["what"])

    def test_not_a_job(self):
        self.assertIsNone(parsers.server_line("encserver ready"))


class Bench(unittest.TestCase):
    CSV = ("workers,bytes,segments,run,encryption_us,round_trip_us,verified\n"
           "1,2097152,4,1,200000.0,203000.0,pass\n1,2097152,4,2,190000.0,193000.0,-\n"
           "4,2097152,16,1,52000.0,55000.0,pass\n4,2097152,16,2,48000.0,51000.0,-\n"
           "garbage line\n")

    def test_stats(self):
        runs = parsers.bench_csv(self.CSV)
        self.assertEqual(len(runs), 4)
        stats = parsers.bench_stats(runs)
        self.assertEqual([s["workers"] for s in stats], [1, 4])
        self.assertAlmostEqual(stats[0]["enc_mean"], 195.0)
        self.assertAlmostEqual(stats[1]["enc_mean"], 50.0)
        self.assertAlmostEqual(stats[1]["speedup"], 3.9)
        self.assertAlmostEqual(stats[1]["efficiency"], 0.975)
        self.assertAlmostEqual(stats[1]["mb_per_s"], 40.0)


class Terminal(unittest.TestCase):
    """The same output as printed on a terminal (ssh -tt): colours, Unicode, '\\r' redraws."""
    ESC = "\x1b"

    def test_clean(self):
        e = self.ESC
        raw = (f"\r  {e}[2mprogress    {e}[0m {e}[36m███░░░{e}[0m  1/6 Data Blocks"
               f"\r  {e}[2mprogress    {e}[0m {e}[36m██████{e}[0m  6/6 Data Blocks\r\n")
        self.assertEqual(parsers.clean_lines(raw)[0], "  progress     ██████  6/6 Data Blocks")

    def test_encrypt_on_terminal(self):
        e = self.ESC
        text = "\r\n".join([
            f"  {e}[2minput       {e}[0m hello.txt  397 B (397 bytes)",
            f"  {e}[2mserver      {e}[0m 4 workers, 1 Segment in 1 Job, 0.058 ms encrypting",
            f"  {e}[2mstatus      {e}[0m {e}[1;32m✔ encrypted, every Data Block verified{e}[0m", ""])
        r = parsers.encrypt_output(parsers.clean_lines(text))
        self.assertTrue(r["ok"])
        self.assertTrue(r["verified"])
        self.assertEqual((r["bytes"], r["segments"]), (397, 1))

    def test_server_on_terminal(self):
        e = self.ESC
        received = parsers.clean(f"  {e}[1m#12  {e}[0m {e}[36m→{e}[0m pid 4231      2.0 MB  "
                                 f"{e}[2mreceived, 3 Jobs in progress{e}[0m\r")
        ev = parsers.server_line(received)
        self.assertEqual((ev["kind"], ev["id"], ev["in_progress"]), ("received", 12, 3))
        done = parsers.clean(f"  {e}[1m#12  {e}[0m {e}[1;32m✔{e}[0m pid 4231      2.0 MB         16    "
                             f"50.064 ms   {e}[2mw0{e}[0m {e}[36m████{e}[0m 4  {e}[2mw1{e}[0m {e}[36m████{e}[0m 4 ")
        ev = parsers.server_line(done)
        self.assertEqual((ev["kind"], ev["segments"], ev["per_worker"]), ("done", 16, [4, 4]))

    def test_failure_mark(self):
        self.assertTrue(parsers.errors(["encclient: ✖ cannot reach \"encsvc\""]))


class Blocks(unittest.TestCase):
    def test_blocks(self):
        b = parsers.blocks(["@@begin a", "$ ls", "x", "@@end a rc=0", "noise", "@@begin b", "@@end b rc=2"])
        self.assertEqual(b["a"], {"lines": ["$ ls", "x"], "rc": 0})
        self.assertEqual(b["b"]["rc"], 2)

    def test_pids(self):
        self.assertEqual(parsers.pids(["@@begin j1", "@@pid j1 4103", "x", "@@pid client2 77 ", "@@pid bad x"]),
                         {"j1": 4103, "client2": 77})


if __name__ == "__main__":
    unittest.main()
