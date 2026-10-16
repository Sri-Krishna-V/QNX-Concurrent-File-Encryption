# Dashboard

A page on this PC that runs the whole project on the Raspberry Pi, one click per step, and
shows the results. It starts empty every time: every value on it comes from a step you ran.
The work itself shows in command-prompt windows, exactly as it looks in an SSH window on
the Pi. The page uses a light theme.

## Start it

Double-click `launch_dashboard.bat` (or run `py dashboard\backend\server.py`), then open
<http://localhost:8080>. Stop it with Ctrl+C in its window.

It needs Python 3, Windows' OpenSSH client, and the QNX SDP 8.0 in `%USERPROFILE%\qnx800`
(for the Build step). The Pi must answer at `qnxuser@10.0.0.1`.

## The workflow

| Step | What it runs | What the dashboard shows |
|---|---|---|
| 1 Connect to the Pi | ping, SSH login, `uname`, `pidin info` | QNX version, CPUs, memory, what is already on the Pi |
| 2 Build the programs | `make clean && make CRYPTO=builtin` with the SDP | program sizes, compiler warnings |
| 3 Deploy to the Pi | `scp` programs, scripts and examples to `/var/encsvc` | bytes sent and speed; every file checked against this PC |
| 4 Prepare the key | `dd if=/dev/urandom of=enc.key bs=32 count=1` if missing | key size and checksum (never the key) |
| 5 Start encserver | `./encserver -v -w N` | workers, cipher; then every Job, live |
| 6 Encrypt and decrypt files | `encclient -e`, `-d`, `cmp` for each example file | sizes, Segments, times, header, first bytes |
| 7 Large file, Jobs in parallel | a 12 MB file with `-j 1` and with `-j 4` | wall time, throughput, most Jobs at once |
| 8 Several clients at once | 3 `encclient -s 2097152` processes together | Round-Trip Time of each against one client alone |
| 9 Scaling benchmark | `scaling_table` from `lib.sh` for 1, 2, 4, 8 workers | speedup, Encryption Time, throughput, `results.csv` |
| 10 Stop and clean up | `slay encserver`, delete `/dev/shmem/encdash` | what was kept on the Pi |

A step unlocks when the steps it needs are done. The **Analytics** tab collects the
measurements of steps 5 to 9 as charts and tables.

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
`dashboard\runs\<session>\` (the last 10 sessions are kept).

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

Set these before starting the backend, for example `set ENC_PI_HOST=192.168.1.50`:

| Variable | Default |
|---|---|
| `ENC_PI_HOST` | `10.0.0.1` |
| `ENC_PI_USER` | `qnxuser` |
| `ENC_PI_PASSWORD` | `qnxuser` (the QNX Pi image's default; used only if no SSH key works) |
| `ENC_SDP_ENV` | `%USERPROFILE%\qnx800\qnxsdp-env.bat` |
| `ENC_DASH_PORT` | `8080` |

The backend listens only on `127.0.0.1` and refuses requests sent from other web sites.

On the Pi, each step's script is copied to `/dev/shmem/encdash-step.sh` and run from there;
the files a step makes go to `/dev/shmem/encdash` (RAM), which step 10 removes.
