# Dashboard

A page on this PC that runs the whole project on the Raspberry Pi, one click per step, and
shows the results. It does no encryption itself: it sends the Pi the same commands you would
type by hand in [BEGINNER-GUIDE.md](../docs/BEGINNER-GUIDE.md), over SSH, and reads what they
print. The page starts empty every time: every value on it comes from a step you ran.

```text
  browser tab                  dashboard backend (this PC)          Raspberry Pi 4B (QNX 8.0)
  http://localhost:8080        backend\server.py                    qnxuser@10.0.0.1
 ┌──────────────────┐  click  ┌─────────────────────────┐ ssh, scp ┌──────────────────────────┐
 │ Workflow: steps  │ ──────▶ │ runs one step at a time │ ───────▶ │ /var/encsvc              │
 │ Analytics: charts│ ◀────── │ reads the output into   │ ◀─────── │   encserver, encclient,  │
 └──────────────────┘ results │ numbers                 │  output  │   lib.sh, examples       │
                       (SSE)  └────────────┬────────────┘          └──────────────────────────┘
                                           │ writes logs
                                           ▼
                              command-prompt windows: the output, as in an SSH window
```

The page shows **results** (facts, tables, charts). The command-prompt windows show the
**work** (colours, progress bars, the encserver log), exactly as an SSH window on the Pi would.

---

## Before you start

| Check | How | If it is missing |
|---|---|---|
| The Pi is on and wired to this PC | the Pi has booted (about 1 min) and the Ethernet cable is in | [LAB-GUIDE.md](../docs/LAB-GUIDE.md) Part 3 |
| The Pi answers at `10.0.0.1` | `ping 10.0.0.1` in a Command Prompt | give this PC's Ethernet adapter an address in `10.0.0.x`, or set `ENC_PI_HOST` (see [Settings](#settings)) |
| Python 3 | `py --version` | install it from python.org |
| Windows' OpenSSH client | `ssh -V` | Settings > System > Optional features > OpenSSH Client |
| QNX SDP 8.0 in `%USERPROFILE%\qnx800` | the file `qnxsdp-env.bat` is there | step 2 (Build) needs it; install it, or set `ENC_SDP_ENV` |

You do not need an SSH key: if this PC has none on the Pi, the dashboard logs in with the
QNX image's default password (`qnxuser`) without asking you.

## Start it

Double-click `launch_dashboard.bat`. It opens a window for the backend and the page at
<http://localhost:8080>. (Or run `py dashboard\backend\server.py` and open the address
yourself.)

Stop it with **Ctrl+C** in the backend window. That also stops encserver on the Pi if the
dashboard started it.

---

## A first run, click by click

1. **Step 1, Connect to the Pi.** It is highlighted: that is always the next step to run.
   Click **Run**. A window called *Dashboard activity* opens and shows what happens. When the
   step is done, its card shows **✓ Done** and its results, and the chip at the top turns green:
   `Pi 10.0.0.1 · QNX 8.0.x · 4 CPUs`.
2. **Steps 2, 3, 4** (Build, Deploy, Key). Click **Run** on each, in order. Each one unlocks
   the next.
3. **Step 5, Start encserver.** A second window opens: *encserver on the Pi*. Keep it
   visible. It is the left window of the beginner's guide: a line when a Job arrives (`→`) and a
   line when it is done (`✔`), with the Segments each worker encrypted.
4. **Steps 6, 7, 8** in any order. Watch both windows: encclient's progress bars in the activity
   window, the Jobs arriving in the encserver window.
5. **Step 9, Scaling benchmark.** It stops the live encserver, because it starts a fresh one
   for each Worker Pool size. Run step 5 again afterwards if you want to repeat steps 6 to 8.
6. Open the **Analytics** tab: speedup, Encryption Time, throughput, load balance and the
   concurrency results, all from the steps you ran.
7. **Step 10, Stop and clean up.** Then close the backend with Ctrl+C.

A whole run takes about five minutes; step 9 takes the longest.

---

## The page

**At the top**

| Control | What it does |
|---|---|
| **Pi** chip | grey: not connected yet; amber: connecting; green: connected (address, QNX release, CPUs); red: no answer |
| **encserver** chip | not started / running with N workers and the number of Jobs so far / stopped. Amber means an encserver started outside the dashboard (for example in an SSH window) is running |
| **Pop-up console** | on: the *Dashboard activity* window opens by itself when a step starts (on by default) |
| **Open console** | opens the *Dashboard activity* window now |
| **encserver log** | opens the *encserver on the Pi* window again (once step 5 has run) |
| **Start over** | clears every result, stops encserver if the dashboard started it, and starts a new session |

**On each step card**

| Control | What it does |
|---|---|
| options (drop-downs, tick boxes) | change how the step runs; see [the steps](#the-ten-steps). The defaults are the ones the guides use |
| **Run** / **Run again** | runs the step. Running a done step again replaces its results |
| **Cancel** | shown while the step runs. Stops it; any encclient it started on the Pi is stopped too |
| **View output** | opens this step's output in its own window, from the start |
| **Results** | shows or hides the step's results on the card |

**What a card looks like**

| Card | Means |
|---|---|
| its number | not run yet |
| spinner, elapsed time | running. Only one step runs at a time |
| **✓ Done** | finished; the time it took and its results are on the card |
| **✕ Failed** (red, `!`) | the reason is on the card; the step's window has the full output |
| **Cancelled** | you cancelled it; run it again when you are ready |
| greyed out, with a reason | locked, for example `run step 3 (Deploy to the Pi) first` or `encserver is not running; start it in step 5` |

---

## The ten steps

| Step | What it runs on the Pi | What the dashboard shows |
|---|---|---|
| 1 Connect to the Pi | ping, SSH login, `uname`, `pidin info` | QNX version, CPUs, memory, what is already on the Pi |
| 2 Build the programs | `make clean && make CRYPTO=builtin` with the SDP (on this PC) | program sizes, compiler warnings |
| 3 Deploy to the Pi | `scp` programs, scripts and examples to `/var/encsvc` | bytes sent and speed; every file checked against this PC |
| 4 Prepare the key | `dd if=/dev/urandom of=enc.key bs=32 count=1` if missing | key size and checksum (never the key) |
| 5 Start encserver | `./encserver -v -w N` | workers, cipher; then every Job, live |
| 6 Encrypt and decrypt files | `encclient -e`, `-d`, `cmp` for each example file | sizes, Segments, times, header, first bytes |
| 7 Large file, Jobs in parallel | a 12 MB file with `-j 1` and with `-j 4` | wall time, throughput, most Jobs at once |
| 8 Several clients at once | 3 `encclient -s 2097152` processes together | Round-Trip Time of each against one client alone |
| 9 Scaling benchmark | `scaling_table` from `lib.sh` for 1, 2, 4, 8 workers | speedup, Encryption Time, throughput, `results.csv` |
| 10 Stop and clean up | `slay encserver`, delete `/dev/shmem/encdash` | what was kept on the Pi |

**When each step unlocks, and its options**

| Step | Unlocks when | Options (default first) |
|---|---|---|
| 1 Connect | always | none |
| 2 Build | 1 is done | AES code: bundled AES (works on the lab Pi), or OpenSSL libcrypto |
| 3 Deploy | 2 is done | none |
| 4 Key | 3 is done | Make a new key (off). A new key also stops encserver, which still holds the old one |
| 5 Start encserver | 4 is done | Workers: one per CPU, or 1, 2, 3, 4, 6, 8. Running it again restarts encserver |
| 6 Encrypt and decrypt | 5 is done and encserver runs | File: all example files, or one; Data Blocks in flight: 4, or 1, 2, 8 |
| 7 Large file | 5 is done and encserver runs | File size: 12 MB, or 4, 8, 16 MB; in flight: 4, or 2, 8; also run one at a time (on) |
| 8 Several clients | 5 is done and encserver runs | Clients: 3, or 2, 4, 6, 8; requests each: 10, or 5, 20 |
| 9 Benchmark | 4 is done | Data Block: 2 MB, or 256 KB, 1 MB; runs per size: 20, or 10 to 200; Worker Pool sizes: 1, 2, 4, 8, or others |
| 10 Stop and clean up | 1 is done | Stop encserver (on) |

**The same thing by hand.** Each step is a part of [BEGINNER-GUIDE.md](../docs/BEGINNER-GUIDE.md).
Read that part to learn what the commands do:

| Step | Guide part |
|---|---|
| 1 Connect | Part 4 (log in) |
| 2 Build | Part 2 |
| 3 Deploy | Part 3 |
| 4 Key | Part 5 |
| 5 Start encserver | Part 4, left window; Part 7.J for the worker count |
| 6 Encrypt and decrypt | 7.A to 7.E |
| 7 Large file | 7.F |
| 8 Several clients | 7.H |
| 9 Benchmark | 7.I and 7.K |
| 10 Stop and clean up | Part 8 |

---

## The Analytics tab

Empty until a step has measured something; then it fills as you go.

| Panel | Filled by | Shows |
|---|---|---|
| Tiles at the top | steps 5 to 9 | best speedup, fastest MB/s, Jobs encrypted, most Jobs at once, files identical after decryption |
| Worker Pool scaling | step 9 | speedup against the ideal N×, mean Encryption Time (whiskers: fastest to slowest run), throughput; **Show table** for all statistics; **results.csv** to download |
| Load balance across the Worker Pool | every Job the live encserver ran (steps 6 to 8) | Segments encrypted per worker; the latest Jobs with their client pid, time and how many Jobs were in progress |
| Concurrency | steps 7 and 8 | wall time with 1 Data Block in flight against N; Round-Trip Time of each client against one client alone |
| File round-trips | step 6 | each file: size, Data Blocks, Segments, times, identical or not |

**Download all results (JSON)** at the bottom saves everything the page knows.

---

## Seeing the work in a terminal

The page has no terminal. The work shows in command-prompt windows instead:

| Window | Opens | Shows |
|---|---|---|
| **Dashboard activity** | by itself when a step starts (**Pop-up console**, on by default), or with **Open console** | every step as it runs |
| **encserver on the Pi** | when step 5 starts encserver, or with **encserver log** | the server's live log (`encserver -v`) |
| **Step N** | with **View output** on a step | that step only, from the start |

Each step runs on a terminal on the Pi (`ssh -tt`), so the windows show what an SSH window
would: colours, `✔` / `✖` marks, progress bars that fill up as Data Blocks come back, the
`█` bars of each worker in the server log. Like `demo.sh`, each step also says what is
happening and what to look for:

- **Deploy**: each file as it reaches the Pi, then scp's own figures, e.g.
  `sent 119,284 bytes over SSH in 0.1 s (1.1 MB/s)`, and an `ls -l` check on the Pi.
- **Encrypt and decrypt files**: the ENCRYPT → Event → FETCH exchange, `encclient`'s
  output (sizes, Segments, times, first 16 bytes before and after), the readable header of
  the Encrypted File, the decryption, `cmp` and `cksum` of both files, the text of `hello.txt`.
- **Large file**: the 12 MB file one Data Block at a time, then 4 in flight, and the
  Encryption Time against the wall time.
- **Several clients**: the pid of each client (find them in the encserver window), each
  client's result, and the Round-Trip Time of each against one client alone.
- **Scaling benchmark**: the table from `lib.sh` with the speedup for 1, 2, 4 and 8 workers.

Every step ends with a summary line and `=== Step N done in … s`. All of it is also saved in
`dashboard\runs\<session>\` (the last 10 sessions are kept): `activity.log`, `encserver.log`
and one log per step. Send these when you ask for help.

---

## When something goes wrong

The card of a failed step says why; **View output** shows the whole step.

| What you see | What to do |
|---|---|
| `No working Python 3 found` (backend window) | install Python 3 from python.org, or turn off the `python.exe` App execution alias in Settings > Apps > Advanced app settings |
| `Cannot listen on port 8080` | the dashboard is already running: use that window, or close it. Or set `ENC_DASH_PORT` to another port |
| red chip `Dashboard backend not answering` | the backend window was closed. Start `launch_dashboard.bat` again and reload the page. The page starts empty; the logs of the old session are still in `dashboard\runs` |
| Step 1: `The Pi does not answer at 10.0.0.1` | check the cable, that the Pi has booted, and that this PC's Ethernet adapter has an address in `10.0.0.x` |
| Step 1: `SSH login to qnxuser@10.0.0.1 failed` | the Pi's password is not `qnxuser`: set `ENC_PI_PASSWORD`. Check that `ssh -o MACs=hmac-sha2-256 qnxuser@10.0.0.1` works by hand |
| Step 1: `OpenSSH client not found` | Settings > System > Optional features > OpenSSH Client |
| Step 1: `QNX SDP 8.0 not found` | install the SDP in `%USERPROFILE%\qnx800`, or set `ENC_SDP_ENV` to your `qnxsdp-env.bat` |
| A step is greyed out | its reason says which step to run first. Steps 6 to 8 also need encserver running: run step 5 again |
| amber chip `encserver running (started outside the dashboard)` | someone started encserver in an SSH window. Step 5 stops it and starts the dashboard's own; step 10 stops it too |
| Step 4: `enc.key has N bytes` | run step 4 again with **Make a new key** ticked |
| `lost the SSH connection to the Pi` | the Pi rebooted or the cable came out. Run step 1 again, then step 5 |
| A step seems stuck | press **Cancel**, then **View output** to see where it stopped |
| No command-prompt windows open | they only open on Windows, and not when `ENC_DASH_CONSOLES=0`. The logs are still in `dashboard\runs` |

Problems with the programs themselves (not the dashboard): [BEGINNER-GUIDE.md](../docs/BEGINNER-GUIDE.md)
Part 9 and [LAB-GUIDE.md](../docs/LAB-GUIDE.md) Part 9.

---

## How it works

For whoever changes the dashboard.

1. The page (`web/`) asks the backend for the list of steps (`GET /api/workflow`) and builds a
   card for each. It opens one Server-Sent Events stream (`GET /api/stream`) and redraws
   itself every time the backend sends a new state. The page computes no results; it only
   draws what the backend sends.
2. **Run** posts `POST /api/steps/<id>/run` with the card's options. The `Engine` in
   `steps.py` checks that the step is not locked (its `requires` are done, encserver runs if
   it `needs_server`) and that each option is one of the offered values, then runs the step's
   function on a thread. One step runs at a time.
3. A step function writes an `sh` script. `remote.upload_text` copies it to
   `/dev/shmem/encdash-step.sh` on the Pi, and `ssh -tt sh /dev/shmem/encdash-step.sh` runs it on a
   terminal, so the programs print what they print for a person. Copying the script first means
   nothing in it has to survive command-line quoting.
4. The output goes, colours and all, to the step's log and the activity log, which the console
   windows follow (`console_viewer.py`). `parsers.py` turns the same output into numbers.
5. The step returns `facts`, `tables` and `texts`. They go into the `Store` (`session.py`),
   and the `Store` pushes the new state to every open tab.
6. encserver runs in one long `ssh -tt` session of its own (`ServerManager`). Each line of its
   log becomes a Job record (the load balance panel). Closing that session, or the backend,
   ends encserver.
7. SSH tries this PC's key first (`BatchMode=yes`), then the password. ssh never prompts:
   `SSH_ASKPASS_REQUIRE=force` makes it ask `askpass.cmd`, which prints `ENC_PI_PASSWORD`.

**The API** (only from `localhost`; POST bodies are JSON)

| Request | Does |
|---|---|
| `GET /api/workflow` | the ten steps, their options, the Pi address |
| `GET /api/state` | the whole state, with why each step is locked |
| `GET /api/stream` | Server-Sent Events: a `state` event on every change |
| `GET /api/benchmark.csv` | the last benchmark's `results.csv` |
| `GET /api/session.json` | the whole state as a file |
| `POST /api/steps/<id>/run` | runs a step: `{"options": {...}}` |
| `POST /api/steps/<id>/console` | opens that step's window |
| `POST /api/console/activity`, `/api/console/server` | opens the activity or the encserver window |
| `POST /api/settings` | `{"auto_console": true}` turns Pop-up console on or off |
| `POST /api/cancel` | cancels the running step |
| `POST /api/server/stop` | stops encserver |
| `POST /api/reset` | Start over |

**Changing it**

- **A new step:** add an entry to `WORKFLOW` in `steps.py` (`id`, `phase`, `title`, `summary`,
  `requires`, `options`), write `step_<id>(ctx, opt)` that returns `{"facts": [...], "tables":
  [...], "texts": [...]}`, and add it to `RUNNERS`. The page builds its card by itself. A new
  chart needs code in `web/app.js` and `web/charts.js`.
- **If you change what encclient, encserver or `lib.sh` print**, update `parsers.py` and
  `tests/test_parsers.py`. The dashboard reads their output; a changed line format shows up
  as missing numbers, not as an error.
- **Tests** (no Pi needed): `py -m unittest discover -s dashboard/tests`.

---

## Files

```
launch_dashboard.bat     starts the backend and opens the page
backend/
  server.py              HTTP server: the page, the API, live updates to the page (SSE)
  steps.py               the ten steps: the script each runs on the Pi, and what it reports
  parsers.py             reads encclient / encserver / results.csv output (terminal or plain) into numbers
  session.py             the page's state, the logs, the console windows
  remote.py              ping, ssh and scp to the Pi
  config.py              Pi address, folders, port (each can be set by environment variable)
  console_viewer.py      runs inside a console window: replays a log as it grows, colours and all
  askpass.cmd            gives ssh the Pi password when this PC has no SSH key on the Pi
web/                     index.html, app.js, charts.js, style.css
tests/                   py -m unittest discover -s dashboard/tests
runs/                    logs, one folder per session, the last 10 kept (not in git)
```

## Settings

Set these in a Command Prompt before starting the backend, for example
`set ENC_PI_HOST=192.168.1.50` and then `launch_dashboard.bat` from the same window:

| Variable | Default |
|---|---|
| `ENC_PI_HOST` | `10.0.0.1` |
| `ENC_PI_USER` | `qnxuser` |
| `ENC_PI_PASSWORD` | `qnxuser` (the QNX Pi image's default; used only if no SSH key works) |
| `ENC_SDP_ENV` | `%USERPROFILE%\qnx800\qnxsdp-env.bat` (then `C:\qnx800\qnxsdp-env.bat`, then `qcc` on `PATH`) |
| `ENC_DASH_PORT` | `8080` |
| `ENC_DASH_RUNS` | `dashboard\runs` (where the session logs go) |
| `ENC_DASH_CONSOLES` | `1`; `0` never opens command-prompt windows (the tests use this) |

The backend listens only on `127.0.0.1` and refuses requests sent from other web sites.

On the Pi, each step's script is copied to `/dev/shmem/encdash-step.sh` and run from there;
the files a step makes go to `/dev/shmem/encdash` (RAM), which step 10 removes. The programs,
examples, `enc.key` and `results.csv` stay in `/var/encsvc`.
