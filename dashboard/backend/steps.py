"""
steps.py - the workflow: what each dashboard step runs, and what it reports.

Every step writes everything it runs, and all output, to its own log
(runs/<session>/NN-<step>.log) and to the session's activity log. Those are what
the command-prompt windows show; the dashboard shows the parsed result.

A remote step is a short sh script piped to `ssh pi sh -s`, so nothing has to
survive Windows command-line quoting. Each command in it is wrapped by cmd():
    @@begin TAG / $ the command as typed / its output / @@end TAG rc=N
The @@ lines are hidden in the console windows and split the output for parsing.
"""
import codecs
import os
import re
import subprocess
import threading
import time
from datetime import datetime

import config
import parsers
import remote
from session import LogFile


class StepError(Exception):
    """A step failed for a reason the user can act on; the message says what to do."""


class Cancelled(Exception):
    pass


# ---------------------------------------------------------------- the workflow

def _choices(values, fmt=str):
    return [{"value": v, "label": fmt(v)} for v in values]


WORKFLOW = [
    {"id": "connect", "phase": "Set up", "title": "Connect to the Pi",
     "summary": "Check this PC's QNX toolchain, ping the Pi and log in over SSH.",
     "requires": [], "options": []},
    {"id": "build", "phase": "Set up", "title": "Build the programs",
     "summary": "Cross-compile encserver and encclient for aarch64le with the QNX SDP.",
     "requires": ["connect"],
     "options": [{"name": "crypto", "label": "AES code", "type": "select", "default": "builtin",
                  "choices": [{"value": "builtin", "label": "bundled AES (works on the lab Pi)"},
                              {"value": "openssl", "label": "OpenSSL libcrypto"}]}]},
    {"id": "deploy", "phase": "Set up", "title": "Deploy to the Pi",
     "summary": f"Copy the programs, scripts and example files to {config.PI_DIR}.",
     "requires": ["build"], "options": []},
    {"id": "key", "phase": "Set up", "title": "Prepare the key",
     "summary": "Make sure the Pi has the shared 32-byte AES-256 key (enc.key).",
     "requires": ["deploy"],
     "options": [{"name": "regenerate", "label": "Make a new key", "type": "check", "default": False}]},
    {"id": "server", "phase": "Run", "title": "Start encserver",
     "summary": "Start the encryption server with its Worker Pool; its live log opens in a window.",
     "requires": ["key"],
     "options": [{"name": "workers", "label": "Workers", "type": "select", "default": "auto",
                  "choices": [{"value": "auto", "label": "one per CPU"}] +
                             _choices([1, 2, 3, 4, 6, 8], lambda w: f"{w} worker{'s' if w > 1 else ''}")}]},
    {"id": "roundtrip", "phase": "Run", "title": "Encrypt and decrypt files",
     "summary": "Encrypt example files on the server, decrypt them on the client, compare byte for byte.",
     "requires": ["server"], "needs_server": True,
     "options": [{"name": "file", "label": "File", "type": "select", "default": "all",
                  "choices": [{"value": "all", "label": "all example files"}] +
                             _choices(config.EXAMPLE_FILES)},
                 {"name": "in_flight", "label": "Data Blocks in flight", "type": "select",
                  "default": 4, "choices": _choices([1, 2, 4, 8])}]},
    {"id": "concurrency", "phase": "Run", "title": "Large file, Jobs in parallel",
     "summary": "Encrypt a large random file as several 2 MB Data Blocks at once, and compare with one at a time.",
     "requires": ["server"], "needs_server": True,
     "options": [{"name": "size_mb", "label": "File size", "type": "select", "default": 12,
                  "choices": _choices([4, 8, 12, 16], lambda m: f"{m} MB")},
                 {"name": "in_flight", "label": "Data Blocks in flight", "type": "select",
                  "default": 4, "choices": _choices([2, 4, 8])},
                 {"name": "compare", "label": "Also run one at a time", "type": "check", "default": True}]},
    {"id": "multiclient", "phase": "Run", "title": "Several clients at once",
     "summary": "Run several encclient processes against the server at the same moment.",
     "requires": ["server"], "needs_server": True,
     "options": [{"name": "clients", "label": "Clients", "type": "select", "default": 3,
                  "choices": _choices([2, 3, 4, 6, 8])},
                 {"name": "runs", "label": "Requests each", "type": "select", "default": 10,
                  "choices": _choices([5, 10, 20])}]},
    {"id": "benchmark", "phase": "Measure", "title": "Scaling benchmark",
     "summary": "Measure Encryption Time for each Worker Pool size (stops the live encserver).",
     "requires": ["key"],
     "options": [{"name": "size", "label": "Data Block", "type": "select", "default": 2097152,
                  "choices": [{"value": 262144, "label": "256 KB"}, {"value": 1048576, "label": "1 MB"},
                              {"value": 2097152, "label": "2 MB"}]},
                 {"name": "runs", "label": "Runs per size", "type": "select", "default": 20,
                  "choices": _choices([10, 20, 50, 100, 200])},
                 {"name": "workers", "label": "Worker Pool sizes", "type": "select", "default": "1 2 4 8",
                  "choices": _choices(["1 2 4 8", "1 2 3 4", "1 2 3 4 6 8"], lambda s: s.replace(" ", ", "))}]},
    {"id": "finish", "phase": "Finish", "title": "Stop and clean up",
     "summary": "Stop encserver and delete the files the workflow made in the Pi's RAM.",
     "requires": ["connect"],
     "options": [{"name": "stop_server", "label": "Stop encserver", "type": "check", "default": True}]},
]
STEP_BY_ID = {s["id"]: s for s in WORKFLOW}
for _n, _s in enumerate(WORKFLOW, 1):
    _s["n"] = _n


def q(text):
    """Quote for sh."""
    return "'" + text.replace("'", "'\\''") + "'"


# The start of every step script on the Pi: the same look as scripts/lib.sh and demo.sh.
PRELUDE = r"""exec 2>&1
if [ -t 1 ]; then
    ESC=$(printf '\033')
    C_RESET="$ESC[0m"; C_BOLD="$ESC[1m"; C_DIM="$ESC[2m"
    C_RED="$ESC[1;31m"; C_GREEN="$ESC[1;32m"; C_CYAN="$ESC[36m"; C_YELLOW="$ESC[33m"
    OK="✔"; BAD="✖"; VBAR="│"; ARROW="→"
    ENC_UNICODE=1; export ENC_UNICODE      # encclient output saved to files keeps the symbols
else
    C_RESET=""; C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_CYAN=""; C_YELLOW=""
    OK="OK"; BAD="!!"; VBAR="|"; ARROW="->"
fi
info()    { echo "  ${C_DIM}$*${C_RESET}"; }
good()    { echo "  ${C_GREEN}${OK} $*${C_RESET}"; }
fail()    { echo "  ${C_RED}${BAD} $*${C_RESET}"; }
heading() { echo; echo "${C_BOLD}${C_CYAN}== $1 ${C_RESET}${C_BOLD}$2${C_RESET}"; }
show()    { echo "${C_YELLOW}\$ $*${C_RESET}"; }
boxed()   { while IFS= read -r l; do echo "  ${C_DIM}${VBAR}${C_RESET} $l"; done; }
"""


def cmd(tag, shown, command):
    """One command of a remote script: markers around it, and the command shown as typed."""
    return f"echo '@@begin {tag}'\nshow {q(shown)}\n{command}\necho \"@@end {tag} rc=$?\"\n"


def bg(tag, shown, command):
    """Like cmd(), but also reports the program's pid ('@@pid TAG PID'), so that its Jobs can be
    found in encserver's log, which names every client by pid."""
    return (f"echo '@@begin {tag}'\nshow {q(shown)}\n{command} &\np=$!\necho \"@@pid {tag} $p\"\n"
            f"wait $p\necho \"@@end {tag} rc=$?\"\n")


def sh_info(*lines):
    return "".join(f"info {q(l)}\n" for l in lines)


# Styles for lines the backend itself writes into the logs (the same as PRELUDE's).
_RESET, _BOLD, _DIM, _GREEN, _RED, _CYAN, _YELLOW = (
    "\x1b[0m", "\x1b[1m", "\x1b[2m", "\x1b[1;32m", "\x1b[1;31m", "\x1b[36m", "\x1b[33m")


def fmt_bytes(n):
    if n is None:
        return "-"
    for unit, size in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= size:
            return f"{n / size:.1f} {unit}"
    return f"{n} B"


def fmt_ms(ms):
    return "-" if ms is None else (f"{ms:.3f} ms" if ms < 100 else f"{ms:.1f} ms")


# ---------------------------------------------------------------- one step run

class Ctx:
    """A running step: its logs, the process it is waiting on, cancellation."""

    def __init__(self, engine, step, options):
        self.engine = engine
        self.step = step
        self.options = options
        self.cancelled = False
        self.proc = None
        self._timed_out = False
        self.started = time.time()
        name = f"{step['n']:02d}-{step['id']}.log"
        self.log_path = os.path.join(engine.store.session_dir, name)
        self.log = LogFile(self.log_path, f"Step {step['n']}: {step['title']}")
        engine.activity.write(f"### Step {step['n']}: {step['title']}  ({datetime.now():%H:%M:%S})")
        self.say(f"{_DIM}{step['summary']}{_RESET}")

    def say(self, line=""):
        self.log.write(line)
        self.engine.activity.write(line)

    def say_raw(self, text):
        self.log.write_raw(text)
        self.engine.activity.write_raw(text)

    # The same look as the Pi-side helpers in PRELUDE.
    def heading(self, title, detail=""):
        self.say("")
        self.say(f"{_BOLD}{_CYAN}== {title} {_RESET}{_BOLD}{detail}{_RESET}")

    def info(self, text):
        self.say(f"  {_DIM}{text}{_RESET}")

    def good(self, text):
        self.say(f"  {_GREEN}✔ {text}{_RESET}")

    def bad(self, text):
        self.say(f"  {_RED}✖ {text}{_RESET}")

    def show(self, command):
        self.say(f"{_YELLOW}$ {command}{_RESET}")

    def check(self):
        if self.cancelled:
            raise Cancelled()

    def run(self, argv, *, stdin_text=None, cwd=None, env=None, timeout=120, shown=None, quiet=False,
            on_line=None):
        """
        Run a program, copying its output into the logs as it arrives (also half lines, so
        progress bars move in the console window). quiet=True keeps it out of the logs;
        on_line(clean line) is then called for each line as it arrives.
        Returns (exit code, clean lines).
        """
        pending = ""
        self.check()
        if shown:
            self.show(shown)
        chunks = []
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        try:
            self.proc = subprocess.Popen(
                argv, cwd=cwd or config.PROJECT_DIR, env=env,
                stdin=subprocess.PIPE if stdin_text is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError as e:
            raise StepError(f"cannot start {argv[0] if isinstance(argv, list) else argv}: {e}")
        if stdin_text is not None:
            def feed(p=self.proc):
                try:
                    p.stdin.write(stdin_text.encode("utf-8"))
                    p.stdin.close()
                except OSError:
                    pass
            threading.Thread(target=feed, daemon=True).start()

        self._timed_out = False
        timer = threading.Timer(timeout, self._timeout)
        timer.start()
        try:
            while True:
                data = self.proc.stdout.read1(65536)
                if not data:
                    break
                text = decoder.decode(data)
                chunks.append(text)
                if not quiet:
                    self.say_raw(text)
                if on_line:
                    pending += text
                    *done, pending = pending.split("\n")
                    for line in done:
                        on_line(parsers.clean(line))
            tail = decoder.decode(b"", final=True)
            chunks.append(tail)
            if not quiet:
                self.say_raw(tail)
            if on_line and (pending + tail).strip():
                on_line(parsers.clean(pending + tail))
            rc = self.proc.wait()
        finally:
            timer.cancel()
            self.proc = None
        self.check()
        if self._timed_out:
            raise StepError(f"gave up after {timeout} s without an answer")
        return rc, parsers.clean_lines("".join(chunks))

    def _timeout(self):
        self._timed_out = True
        self.kill()

    def kill(self):
        p = self.proc
        if p is not None and p.poll() is None:
            try:
                p.kill()
            except OSError:
                pass

    def remote(self, script, timeout=120, quiet=False):
        """
        Run an sh script on the Pi, on a terminal (ssh -tt), so encclient, encserver and the
        scripts print exactly what they print in an SSH window. The script is copied to the Pi
        first, so nothing in it has to survive command-line quoting.
        Returns (exit code, clean lines); 255 means ssh itself failed.
        """
        self.check()
        rc, out = remote.upload_text(PRELUDE + script, config.PI_STEP_SCRIPT)
        if rc != 0:
            raise StepError("cannot reach the Pi over SSH: " + (out.strip().splitlines() or ["no answer"])[-1])
        rc, lines = self.run(remote.ssh_argv(f"sh {config.PI_STEP_SCRIPT}", tty=True),
                             env=remote.env(), timeout=timeout, quiet=quiet)
        if rc == 255 and not any(l.startswith("@@") for l in lines):
            raise StepError("lost the SSH connection to the Pi: " +
                            (" ".join(l for l in lines[-2:] if l) or "no answer"))
        return rc, lines


# ---------------------------------------------------------------- encserver on the Pi

class ServerManager:
    """encserver -v running on the Pi through one long ssh session; its log is parsed live."""

    def __init__(self, engine):
        self.engine = engine
        self.proc = None
        self.log = None
        self.ready = threading.Event()
        self.banner = []
        self.stop_reason = None
        self._lock = threading.Lock()

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self, ctx, workers):
        store = self.engine.store
        if self.running():
            self.stop(ctx, "restarting it with the new settings")
        elif store.snapshot()["server"]["external"]:
            ctx.say("An encserver started outside the dashboard is running; stopping it.")
            self._slay(ctx)

        path = os.path.join(store.session_dir, "encserver.log")
        if self.log is None:
            self.log = LogFile(path, "encserver on the Pi (live log)")
        command = f"cd {config.PI_DIR} && ./encserver -v -w {workers}"
        self.log.write(f"{_DIM}=== {datetime.now():%H:%M:%S}{_RESET}  {_YELLOW}$ {command}{_RESET}")
        ctx.show(command)

        self.ready.clear()
        self.banner = []
        self.stop_reason = None
        try:
            # On a terminal: the log looks as in an SSH window, and closing the session
            # (hang-up) also ends encserver.
            proc = subprocess.Popen(remote.ssh_argv(f"cd {config.PI_DIR} && exec ./encserver -v -w {workers}",
                                                    tty=True),
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, env=remote.env())
        except OSError as e:
            raise StepError(f"cannot start ssh: {e}")
        self.proc = proc
        threading.Thread(target=self._read, args=(proc, ctx), daemon=True).start()
        self.engine.consoles.open(path, "encserver on the Pi - live log", until_done=False)

        deadline = time.time() + 20
        while not self.ready.wait(0.2):
            if ctx.cancelled:
                self.stop_reason = "start cancelled"
                self.kill_local()
                raise Cancelled()
            if proc.poll() is not None or time.time() > deadline:
                self.stop_reason = "did not start"
                out = " / ".join(l.strip() for l in self.banner[-3:] if l.strip())
                self.kill_local()
                raise StepError(f"encserver did not start: {out or 'no output'}")
        info = parsers.server_banner(self.banner)
        store.update(lambda s: s["server"].update(
            running=True, external=False, workers=info.get("workers"), backend=info.get("backend"),
            started=datetime.now().isoformat(timespec="seconds"), note=None))
        return info

    def _read(self, proc, ctx):
        store = self.engine.store
        for raw in proc.stdout:
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            line = parsers.clean(text)
            self.log.write(text)
            if not self.ready.is_set():
                self.banner.append(line)
                ctx.say(text)
                if line.strip().startswith("log ") or line.strip().startswith("JOB"):
                    self.ready.set()
                continue
            ev = parsers.server_line(line)
            if ev:
                store.update(lambda s, ev=ev: record_job(s["jobs"], ev), immediate=False)
        proc.wait()
        with self._lock:
            mine = self.proc is proc
            if mine:
                self.proc = None
        why = self.stop_reason or "encserver ended (see its window)"
        self.log.write(f"{_DIM}=== {datetime.now():%H:%M:%S}  encserver stopped: {why}{_RESET}")
        if mine:
            store.update(lambda s: s["server"].update(running=False, external=False, note=why))

    def stop(self, ctx=None, why=None):
        if ctx:
            ctx.info(f"Stopping encserver{': ' + why if why else ''}.")
        self.stop_reason = why or "stopped"
        self._slay(ctx)
        p = self.proc
        if p is not None:
            try:
                p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.kill_local()       # closing the terminal session ends it too
        self.engine.store.update(lambda s: s["server"].update(running=False, external=False,
                                                              note=why or "stopped"))

    def _slay(self, ctx):
        script = cmd("slay", "slay encserver", "slay -f -Q encserver; sleep 1; "
                     "if [ -e /dev/name/local/encsvc ]; then fail 'encserver is still running'; "
                     "else good 'encserver has stopped (/dev/name/local/encsvc is gone)'; fi")
        if ctx:
            ctx.remote(script, timeout=30)
        else:
            remote.quick("slay -f -Q encserver", timeout=15)

    def kill_local(self):
        p = self.proc
        if p is not None and p.poll() is None:
            try:
                p.kill()
            except OSError:
                pass


def record_job(jobs, ev):
    """Fold one encserver log event into the dashboard's Job table."""
    now = time.time()
    lst = jobs["list"]
    if ev["kind"] in ("received", "waiting"):
        if ev["kind"] == "received":
            lst.append({"id": ev["id"], "pid": ev["pid"], "size": ev["size"], "bytes": ev["bytes"],
                        "state": "running", "t": now, "in_progress": ev.get("in_progress"),
                        "segments": None, "ms": None, "per_worker": []})
            if ev.get("in_progress"):
                jobs["peak_in_progress"] = max(jobs["peak_in_progress"], ev["in_progress"])
        if ev["pid"] not in jobs["pids"]:
            jobs["pids"].append(ev["pid"])
    elif ev["kind"] == "done":
        job = next((j for j in reversed(lst) if j["id"] == ev["id"]), None)
        if job is None:
            job = {"id": ev["id"], "pid": ev["pid"], "size": ev["size"], "bytes": ev["bytes"], "t": now,
                   "in_progress": None}
            lst.append(job)
        job.update(state="done", segments=ev["segments"], ms=ev["ms"], per_worker=ev["per_worker"])
        jobs["count"] += 1
        jobs["bytes"] += ev["bytes"] or 0
        pw = jobs["per_worker"]
        for i, n in enumerate(ev["per_worker"]):
            if i >= len(pw):
                pw.append(0)
            pw[i] += n
    else:
        jobs["failed"] += 1
        job = next((j for j in reversed(lst) if j["id"] == ev["id"]), None) if ev["id"] else None
        if job:
            job.update(state="failed", what=ev["what"])
        else:
            lst.append({"id": ev["id"], "pid": ev["pid"], "size": ev["size"], "bytes": ev["bytes"],
                        "state": "failed", "what": ev["what"], "t": now})
    if len(lst) > 300:
        del lst[:len(lst) - 300]


def jobs_of(state, pids):
    """The Jobs the live encserver logged for the client processes `pids`."""
    return [j for j in state["jobs"]["list"] if j.get("pid") in pids]


# ---------------------------------------------------------------- the steps

FILE_TYPES = {".txt": "text", ".csv": "CSV data", ".json": "JSON", ".pdf": "PDF document",
              ".png": "PNG image", ".wav": "WAV audio"}


def unbox(line):
    """'  │ text' (boxed() in PRELUDE) -> 'text'."""
    return re.sub(r"^\s*[│|] ?", "", line)


def step_connect(ctx, opt):
    e = ctx.engine
    ctx.heading("This PC", "the tools the workflow uses")
    sdp = config.sdp_env_script()
    if sdp:
        ctx.good(f"QNX SDP 8.0: {sdp}")
    else:
        ctx.bad("QNX SDP 8.0 not found (the Build step needs it)")
    if not remote.SSH or not remote.SCP:
        ctx.bad("OpenSSH client not found")
        raise StepError("OpenSSH client not found. Install it in Settings > System > Optional features.")
    ctx.good(f"OpenSSH client: {remote.SSH}")
    e.store.update(lambda s: s.__setitem__("pc", {"sdp": sdp, "ssh": remote.SSH}))

    ctx.heading("The Pi", remote.target())
    ctx.show(f"ping {config.PI_HOST}")
    ms = remote.ping(config.PI_HOST)
    ctx.check()
    if ms is None:
        ctx.bad("no answer")
        raise StepError(f"The Pi does not answer at {config.PI_HOST}. Check the Ethernet cable, that the "
                        f"Pi has booted, and that this PC's Ethernet adapter has an address in 10.0.0.x.")
    ctx.good(f"reply from {config.PI_HOST} in {ms:g} ms")
    ctx.show(f"ssh {remote.target()}")
    rc, out = remote.quick("echo login-ok", timeout=15, key_only=True)
    auth = "SSH key"
    if "login-ok" not in out:
        rc, out = remote.quick("echo login-ok", timeout=20)
        auth = "password"
    ctx.check()
    if "login-ok" not in out:
        ctx.bad(out.strip() or "no answer")
        raise StepError(f"SSH login to {remote.target()} failed: {out.strip() or 'no answer'}")
    ctx.good("logged in with this PC's SSH key" if auth == "SSH key" else
             "logged in with the password (askpass.cmd)")

    script = ("heading System 'what the Pi runs'\n" +
              cmd("uname", "uname -a", "uname -a") +
              cmd("pidin", "pidin info", "pidin info") +
              f"heading Project {q(config.PI_DIR)}\n" +
              cmd("deployed", f"ls {config.PI_DIR}", f"ls -1 {config.PI_DIR} 2>/dev/null") +
              f"[ -d {config.PI_DIR} ] || info 'nothing there yet: step 3 copies the programs'\n" +
              cmd("server", "ls /dev/name/local", "ls -1 /dev/name/local 2>/dev/null") +
              "if [ -e /dev/name/local/encsvc ]; then info 'encserver is already running (encsvc is registered)'; "
              "else info 'encserver is not running'; fi\n")
    rc, lines = ctx.remote(script, timeout=30)
    b = parsers.blocks(lines)

    uname = " ".join(b.get("uname", {}).get("lines", [""])[1:]).split()
    pidin = b.get("pidin", {}).get("lines", [])
    cpus = [l for l in pidin if re.match(r"\s*Processor\d+:", l)]
    model = re.sub(r"^\s*Processor\d+:\s*\d+\s*", "", cpus[0]).replace("FPU", "").strip() if cpus else None
    mem = next((re.search(r"FreeMem:(\d+)MB/(\d+)MB", l) for l in pidin if "FreeMem" in l), None)
    deployed = [l.strip() for l in b.get("deployed", {}).get("lines", [])[1:] if l.strip()]
    server_up = any(l.strip() == "encsvc" for l in b.get("server", {}).get("lines", []))

    pi = {"host": config.PI_HOST, "user": config.PI_USER, "auth": auth, "ping_ms": ms,
          "release": uname[2] if len(uname) > 2 else None, "board": uname[-2] if len(uname) > 4 else None,
          "cpus": len(cpus) or None, "cpu_model": model,
          "mem_free_mb": int(mem.group(1)) if mem else None, "mem_total_mb": int(mem.group(2)) if mem else None,
          "deployed": deployed, "checked": datetime.now().isoformat(timespec="seconds")}

    def apply(s):
        s["pi"] = pi
        if server_up and not e.server.running():
            s["server"].update(running=True, external=True, note="started outside the dashboard")
    e.store.update(apply)

    ctx.heading("Ready", "")
    ctx.good(f"QNX {pi['release'] or '?'} on {pi['board'] or 'the Pi'}, {pi['cpus'] or '?'} x {model or '?'}")
    facts = [
        ("Pi", f"{config.PI_HOST}, ping {ms:g} ms"),
        ("Login", f"{config.PI_USER} with {auth}"),
        ("System", f"QNX {pi['release'] or '?'} on {pi['board'] or 'the Pi'}"),
        ("CPU", f"{pi['cpus'] or '?'} x {model or '?'}"),
        ("Free memory", f"{pi['mem_free_mb']} of {pi['mem_total_mb']} MB" if mem else "-"),
        ("Programs on the Pi", "found in " + config.PI_DIR if "encserver" in deployed else "not deployed yet"),
        ("encserver", "already running" if server_up else "not running"),
        ("QNX SDP on this PC", sdp if sdp else "not found - the Build step needs it"),
    ]
    return {"facts": facts}


def step_build(ctx, opt):
    sdp = config.sdp_env_script()
    if sdp is None:
        raise StepError("QNX SDP 8.0 not found on this PC. Install it, or set ENC_SDP_ENV to the path "
                        "of qnxsdp-env.bat, then start the dashboard again.")
    crypto = opt["crypto"]
    ctx.heading("Build", "cross-compile for the Pi: aarch64le, QNX 8.0")
    ctx.info("qcc builds encserver (the server) and encclient (the client) from src/core and src/qnx,")
    ctx.info("with " + ("the bundled AES code (src/core/aes256.c)." if crypto == "builtin"
                        else "OpenSSL libcrypto for AES."))
    make = f"make clean && make CRYPTO={crypto}"
    line = make if sdp == "PATH" else f'call "{sdp}" >nul && {make}'
    if sdp != "PATH":
        ctx.show(f'call "{sdp}"')
    t0 = time.time()
    # A string, not a list: cmd.exe does not understand the backslash quoting of list arguments.
    rc, lines = ctx.run(f'cmd.exe /d /s /c "{line}"', shown=make, timeout=600)
    seconds = time.time() - t0
    warnings = [l.strip() for l in lines if "warning:" in l]
    errs = [l.strip() for l in lines if " error:" in l or "fatal error" in l or "undefined reference" in l]
    errs += [l.strip() for l in lines if "Error " in l and l.strip() not in errs]     # make's own line last
    if rc != 0:
        first = errs[0] if errs else (lines[-1] if lines else "no output")
        hint = " Try 'bundled AES' - OpenSSL is not installed for QNX." if "openssl" in " ".join(errs).lower() else ""
        ctx.bad(f"make failed (exit {rc})")
        raise StepError(f"make failed (exit {rc}): {first}.{hint}")
    binaries = []
    ctx.heading("Programs", "bin/aarch64le")
    for name in ("encserver", "encclient"):
        p = os.path.join(config.BIN_DIR, name)
        if not os.path.isfile(p):
            raise StepError(f"make finished but {p} is missing")
        size = os.path.getsize(p)
        ctx.good(f"{name:<10} {fmt_bytes(size):>8}  ({size:,} bytes)")
        binaries.append([name, fmt_bytes(size), size,
                         datetime.fromtimestamp(os.path.getmtime(p)).strftime("%H:%M:%S")])
    (ctx.info if not warnings else ctx.bad)(f"{len(warnings)} compiler warning{'s' if len(warnings) != 1 else ''}, "
                                            f"built in {seconds:.1f} s")
    ctx.engine.store.update(lambda s: s["pc"].update(
        built={"crypto": crypto, "at": datetime.now().isoformat(timespec="seconds")}) if s["pc"] else None)
    facts = [("AES code", "bundled AES" if crypto == "builtin" else "OpenSSL libcrypto"),
             ("Target", "aarch64le, QNX 8.0 (qcc -Vgcc_ntoaarch64le)"),
             ("Build time", f"{seconds:.1f} s"),
             ("Compiler warnings", str(len(warnings)))]
    res = {"facts": facts, "tables": [{"title": "Programs", "columns": ["Program", "Size", "Bytes", "Built at"],
                                       "rows": binaries}]}
    if warnings:
        res["texts"] = [{"title": "Warnings", "text": "\n".join(warnings[:20])}]
    return res


def _local_files(sources):
    """(path as sent, size) for each file scp will copy, folders expanded."""
    out = []
    for src in sources:
        full = os.path.join(config.PROJECT_DIR, src)
        if os.path.isdir(full):
            for name in sorted(os.listdir(full)):
                p = os.path.join(full, name)
                if os.path.isfile(p):
                    out.append((f"{src}/{name}", os.path.getsize(p)))
        else:
            out.append((src, os.path.getsize(full)))
    return out


def transfer(ctx, sources, dest, recursive=False):
    """scp to the Pi, showing each file as it lands and scp's own totals."""
    files = _local_files(sources)
    pending = list(files)
    ctx.show(f"scp {'-r ' if recursive else ''}{' '.join(sources)} {remote.target()}:{dest}")
    seen = {"auth": None, "sent": None, "seconds": None, "rate": None, "errors": []}

    def on_line(line):
        m = re.search(r'Authenticated to (\S+) .*using "([^"]+)"', line)
        if m:
            seen["auth"] = m.group(2)
            ctx.info(f"connected to {m.group(1)}, logged in with {m.group(2)}")
            return
        m = re.search(r"truncating at (\d+)", line)        # the end of one file's upload (SFTP)
        if m:
            size = int(m.group(1))
            hit = next((f for f in pending if f[1] == size), None)
            if hit:
                pending.remove(hit)
            name = (hit[0] if hit else "file").rsplit("/", 1)[-1]
            ctx.say(f"  {_CYAN}→{_RESET} {name:<22} {fmt_bytes(size):>9}  {_DIM}{size:>9,} bytes  "
                    f"100%{_RESET}")
            return
        m = re.match(r"Transferred: sent (\d+), received (\d+) bytes, in ([\d.]+) seconds", line)
        if m:
            seen["sent"], seen["seconds"] = int(m.group(1)), float(m.group(3))
            return
        m = re.match(r"Bytes per second: sent ([\d.]+)", line)
        if m:
            seen["rate"] = float(m.group(1))
            return
        if line and not line.startswith(("debug", "OpenSSH", "Executing", "Warning: Permanently")) \
                and "debug1" not in line:
            seen["errors"].append(line)
            ctx.bad(line)

    t0 = time.time()
    rc, _ = ctx.run(remote.scp_argv(sources, dest, recursive=recursive, verbose=True), env=remote.env(),
                    timeout=120, quiet=True, on_line=on_line)
    wall = time.time() - t0
    if rc != 0:
        raise StepError(f"scp failed (exit {rc}): {(seen['errors'] or ['see the console'])[-1]}")
    for name, size in pending:                              # an scp that does not log each file
        ctx.say(f"  {_CYAN}→{_RESET} {name.rsplit('/', 1)[-1]:<22} {fmt_bytes(size):>9}  "
                f"{_DIM}{size:>9,} bytes{_RESET}")
    total = sum(s for _, s in files)
    if seen["sent"] is not None:
        rate = seen["rate"] or (seen["sent"] / seen["seconds"] if seen["seconds"] else 0)
        ctx.good(f"{len(files)} files, {fmt_bytes(total)}: sent {seen['sent']:,} bytes over SSH in "
                 f"{seen['seconds']:.1f} s ({fmt_bytes(int(rate))}/s), {wall:.1f} s with the login")
    else:
        ctx.good(f"{len(files)} files, {fmt_bytes(total)} in {wall:.1f} s")
    return {"files": len(files), "bytes": total, "sent": seen["sent"], "seconds": seen["seconds"],
            "rate": seen["rate"], "wall": wall}


def step_deploy(ctx, opt):
    e = ctx.engine
    files = ["bin/aarch64le/encserver", "bin/aarch64le/encclient", "scripts/demo.sh", "scripts/bench.sh",
             "scripts/lib.sh"]
    for f in files:
        if not os.path.isfile(os.path.join(config.PROJECT_DIR, f)):
            raise StepError(f"{f} is missing; run the Build step first")
    ctx.heading("Deploy", f"copy everything the Pi needs to {remote.target()}:{config.PI_DIR}")
    if e.server.running() or e.store.snapshot()["server"]["external"]:
        e.server.stop(ctx, "its program file is about to be replaced")

    ctx.show(f"ssh {remote.target()} mkdir -p {config.PI_DIR}")
    rc, out = remote.quick(f"mkdir -p {config.PI_DIR}", timeout=20)
    if rc != 0:
        raise StepError(f"cannot create {config.PI_DIR} on the Pi: {out.strip()}")
    dest = config.PI_DIR + "/"
    ctx.heading("Programs and scripts", "")
    t1 = transfer(ctx, files, dest)
    ctx.heading("Example files", "")
    t2 = transfer(ctx, ["examples"], dest, recursive=True)

    names = ["encserver", "encclient", "demo.sh", "bench.sh", "lib.sh"] + \
            [f"examples/{f}" for f in config.EXAMPLE_FILES]
    script = ("heading Check 'the files as the Pi sees them'\n" +
              cmd("chmod", "chmod +x encserver encclient demo.sh bench.sh",
                  f"cd {config.PI_DIR} && chmod +x encserver encclient demo.sh bench.sh") +
              cmd("ls", "ls -l " + " ".join(names), f"cd {config.PI_DIR} && ls -l " + " ".join(names)))
    rc, lines = ctx.remote(script, timeout=30)
    listing = {}
    for l in parsers.blocks(lines).get("ls", {}).get("lines", [])[1:]:
        parts = l.split()
        if len(parts) >= 6 and parts[0][0] in "-l" and parts[4].isdigit():
            listing[parts[-1]] = int(parts[4])
    rows, bad = [], 0
    local = {"encserver": "bin/aarch64le/encserver", "encclient": "bin/aarch64le/encclient",
             "demo.sh": "scripts/demo.sh", "bench.sh": "scripts/bench.sh", "lib.sh": "scripts/lib.sh"}
    for n in names:
        lp = os.path.join(config.PROJECT_DIR, local.get(n, n))
        lsize = os.path.getsize(lp) if os.path.isfile(lp) else None
        rsize = listing.get(n)
        same = lsize is not None and lsize == rsize
        bad += 0 if same else 1
        rows.append([n, fmt_bytes(rsize), "same size" if same else "DIFFERENT"])
    if bad:
        ctx.bad(f"{bad} file(s) differ from this PC")
        raise StepError(f"{bad} file(s) on the Pi do not match this PC; see the step's console")
    ctx.good(f"all {len(rows)} files on the Pi have the same size as on this PC")
    e.store.update(lambda s: s["pi"].update(deployed=list(listing)) if s["pi"] else None)
    total = t1["bytes"] + t2["bytes"]
    sent = (t1["sent"] or 0) + (t2["sent"] or 0)
    secs = (t1["seconds"] or 0) + (t2["seconds"] or 0)
    facts = [("Copied to", f"{remote.target()}:{config.PI_DIR}"),
             ("Files", f"{len(rows)} checked, all the same size as on this PC"),
             ("Data", f"{fmt_bytes(total)} in {t1['files'] + t2['files']} files")]
    if sent and secs:
        facts.append(("Transfer", f"{sent:,} bytes sent over SSH in {secs:.1f} s ({fmt_bytes(int(sent / secs))}/s)"))
    return {"facts": facts, "tables": [{"title": "On the Pi", "columns": ["File", "Size", "Check"], "rows": rows}]}


def step_key(ctx, opt):
    e = ctx.engine
    regenerate = bool(opt["regenerate"])
    script = (f"cd {config.PI_DIR} || exit 1\n"
              "heading Key 'enc.key: one AES-256 key, shared by encserver and encclient'\n" +
              sh_info("encserver encrypts with it; encclient checks and decrypts with it.",
                      "It is 32 random bytes from /dev/urandom and never leaves the Pi."))
    if regenerate:
        script += cmd("rm", "rm -f enc.key", "rm -f enc.key")
    script += cmd("make", "test -f enc.key || dd if=/dev/urandom of=enc.key bs=32 count=1",
                  "if [ -f enc.key ]; then good 'enc.key is already there; keeping it'; "
                  "else dd if=/dev/urandom of=enc.key bs=32 count=1 && good 'enc.key created'; fi")
    script += cmd("check", "ls -l enc.key; cksum enc.key", "ls -l enc.key && cksum enc.key")
    rc, lines = ctx.remote(script, timeout=30)
    b = parsers.blocks(lines)
    check = b.get("check", {})
    if check.get("rc") != 0:
        raise StepError("enc.key could not be created in " + config.PI_DIR)
    size = next((int(l.split()[4]) for l in check["lines"] if l.split()[-1:] == ["enc.key"] and
                 len(l.split()) > 5 and l.split()[4].isdigit()), None)
    ck = next((l.split()[0] for l in check["lines"] if re.match(r"^\d+ 32 enc\.key", l.strip())), None)
    if size != 32:
        raise StepError(f"enc.key has {size} bytes; it must have exactly 32. Run this step with "
                        f"'Make a new key'.")
    created = any("created" in l for l in b.get("make", {}).get("lines", []))
    ctx.good(f"enc.key: 32 bytes, checksum {ck}" + (" (new)" if created else ""))
    if regenerate and e.server.running():
        e.server.stop(ctx, "it still holds the old key; start it again in step 5")
    return {"facts": [("Key file", f"{config.PI_DIR}/enc.key, 32 bytes (AES-256)"),
                      ("This run", "made a new key" if created else "kept the existing key"),
                      ("Key checksum", f"cksum {ck}" if ck else "-"),
                      ("Shared by", "encserver and encclient on the Pi; it never leaves the Pi")]}


def step_server(ctx, opt):
    e = ctx.engine
    pi = e.store.snapshot()["pi"] or {}
    workers = (pi.get("cpus") or 4) if opt["workers"] == "auto" else int(opt["workers"])
    ctx.heading("encserver", f"the Concurrent Encryption Service, {workers} workers")
    ctx.info("It registers /dev/name/local/encsvc (name_attach) and waits for messages (MsgReceive).")
    ctx.info("Its live log opens in the 'encserver on the Pi' window: a line when a Job arrives (→)")
    ctx.info("and a line when it is done (✔) with the Segments each worker encrypted.")
    info = e.server.start(ctx, workers)
    ctx.good(f"encserver is running: {info.get('workers')} workers, {info.get('backend')} AES, "
             f"{info.get('max_jobs')} Jobs at once")
    return {"facts": [("Workers", f"{info.get('workers')} (Worker Pool threads)"),
                      ("Cipher", f"AES-256-CTR, {info.get('backend')} backend, self-test passed"),
                      ("Limits", f"{info.get('max_block')} per Data Block, {info.get('max_jobs')} Jobs at once"),
                      ("Name", "/dev/name/local/encsvc (name_attach)")]}


def _need_server(ctx):
    if not ctx.engine.server.running():
        raise StepError("encserver is not running. Start it in step 5 first.")


def step_roundtrip(ctx, opt):
    _need_server(ctx)
    files = config.EXAMPLE_FILES if opt["file"] == "all" else [opt["file"]]
    j = int(opt["in_flight"])
    w = config.PI_SCRATCH
    script = f"W={w}; [ -d $W ] || mkdir $W; cd {config.PI_DIR} || exit 1\n"
    for f in files:
        size = os.path.getsize(os.path.join(config.EXAMPLES_DIR, f))
        blocks = max(1, -(-size // (2 * 1024 * 1024)))
        kind = FILE_TYPES.get(os.path.splitext(f)[1], "file")
        script += f"heading {q(f)} {q(f'{kind}, {fmt_bytes(size)}')}\n"
        script += sh_info(
            f"The client sends it as {blocks} Data Block{'s' if blocks > 1 else ''} (ENCRYPT). encserver answers at once",
            "with a Job id, splits the block into Segments for the Worker Pool, delivers the client's",
            "Event (a pulse) when they are encrypted, and the client FETCHes the Encrypted Block.")
        script += cmd(f"enc:{f}", f"encclient -e examples/{f} -o {w}/{f}.enc -j {j} -V -x",
                      f"./encclient -e examples/{f} -o $W/{f}.enc -j {j} -V -x")
        script += "info 'The Encrypted File: a readable header, then the encrypted bytes:'\n"
        script += cmd(f"hdr:{f}", f"sed '/^---$/q' {w}/{f}.enc", f"sed '/^---$/q' $W/{f}.enc | boxed")
        script += "info 'Decryption runs on the client, with the same key and the IV from the header:'\n"
        script += cmd(f"dec:{f}", f"encclient -d {w}/{f}.enc -o {w}/{f}.out -x",
                      f"rm -f $W/{f}.out; ./encclient -d $W/{f}.enc -o $W/{f}.out -x")
        script += cmd(f"cmp:{f}", f"cmp examples/{f} {w}/{f}.out",
                      f"cmp -s examples/{f} $W/{f}.out; r=$?; "
                      f"if [ $r -eq 0 ]; then good 'identical: the decrypted file has the same {size:,} bytes "
                      f"as examples/{f}'; else fail 'DIFFERENT from examples/{f}'; fi; "
                      f"info \"cksum original: $(cksum < examples/{f})\"; "
                      f"info \"cksum restored: $(cksum < $W/{f}.out)\"; (exit $r)")
        if f == "hello.txt":
            script += "info 'The decrypted text:'\nboxed < $W/hello.txt.out\n"
    rc, lines = ctx.remote(script, timeout=60 + 30 * len(files))
    b = parsers.blocks(lines)

    rows, results, texts = [], [], []
    for f in files:
        enc = parsers.encrypt_output(b.get(f"enc:{f}", {}).get("lines", []))
        dec = parsers.decrypt_output(b.get(f"dec:{f}", {}).get("lines", []))
        identical = b.get(f"cmp:{f}", {}).get("rc") == 0
        r = {"file": f, "bytes": enc.get("bytes"), "blocks": enc.get("blocks"), "segments": enc.get("segments"),
             "server_ms": enc.get("server_ms"), "enc_wall_ms": enc.get("wall_ms"),
             "dec_wall_ms": dec.get("wall_ms"), "header_bytes": enc.get("header_bytes"),
             "verified": enc.get("verified", False), "identical": identical,
             "ok": bool(enc.get("ok") and dec.get("ok") and identical),
             "error": (enc.get("errors") or dec.get("errors") or [None])[0]}
        results.append(r)
        rows.append([f, fmt_bytes(r["bytes"]), r["blocks"], r["segments"], fmt_ms(r["server_ms"]),
                     fmt_ms(r["enc_wall_ms"]), fmt_ms(r["dec_wall_ms"]),
                     "yes" if r["identical"] else "NO"])
        if len(files) == 1 or f == "hello.txt":
            header = [unbox(l) for l in b.get(f"hdr:{f}", {}).get("lines", [])[1:]]
            texts.append({"title": f"Encrypted File header of {f}", "text": "\n".join(header)})
            hx = [("original ", enc.get("hex_original")), ("encrypted", enc.get("hex_encrypted")),
                  ("decrypted", dec.get("hex_decrypted"))]
            texts.append({"title": f"First 16 bytes of {f}",
                          "text": "\n".join(f"{k}  {v}" for k, v in hx if v)})

    ctx.heading("Summary", f"{len(results)} file{'s' if len(results) > 1 else ''}")
    ctx.say(f"  {_BOLD}{'FILE':<16} {'SIZE':>9}  {'SEGMENTS':>8}  {'SERVER':>10}  {'ENCRYPT':>10}  "
            f"{'DECRYPT':>10}  SAME?{_RESET}")
    for r in results:
        same = f"{_GREEN}✔{_RESET}" if r["identical"] else f"{_RED}✖{_RESET}"
        ctx.say(f"  {r['file']:<16} {fmt_bytes(r['bytes']):>9}  {r['segments'] or '-':>8}  "
                f"{fmt_ms(r['server_ms']):>10}  {fmt_ms(r['enc_wall_ms']):>10}  {fmt_ms(r['dec_wall_ms']):>10}  {same}")
    ctx.info("SERVER: Encryption Time on encserver. ENCRYPT: the client's wall time for the whole")
    ctx.info("exchange. DECRYPT: on the client, one thread.")

    def apply(s):
        keep = [x for x in s["analytics"]["roundtrips"] if x["file"] not in files]
        s["analytics"]["roundtrips"] = keep + results
    ctx.engine.store.update(apply)

    ok = sum(1 for r in results if r["ok"])
    if ok < len(results):
        bad = next(r for r in results if not r["ok"])
        raise StepError(f"{bad['file']}: {bad['error'] or 'the decrypted file differs from the original'}")
    return {"facts": [("Files", f"{ok} of {len(results)} round-trips identical"),
                      ("Bytes encrypted", fmt_bytes(sum(r["bytes"] or 0 for r in results))),
                      ("Checked", "every Encrypted Block against the Reference Encryption (-V)"),
                      ("Data Blocks in flight", str(j))],
            "tables": [{"title": "Round-trips", "columns": ["File", "Size", "Data Blocks", "Segments",
                                                             "Server encryption", "Encrypt (client)",
                                                             "Decrypt (client)", "Identical"],
                        "rows": rows}],
            "texts": texts}


def step_concurrency(ctx, opt):
    _need_server(ctx)
    mb, j, compare = int(opt["size_mb"]), int(opt["in_flight"]), bool(opt["compare"])
    blocks = -(-mb // 2)
    w = config.PI_SCRATCH
    script = f"W={w}; [ -d $W ] || mkdir $W; cd {config.PI_DIR} || exit 1\n"
    script += f"heading 'Large file' {q(f'{mb} MB of random bytes = {blocks} Data Blocks of 2 MB')}\n"
    script += cmd("make", f"dd if=/dev/urandom of={w}/big.bin bs=65536 count={mb * 16}",
                  f"dd if=/dev/urandom of=$W/big.bin bs=65536 count={mb * 16} 2>/dev/null && "
                  f"good \"big.bin: $(wc -c < $W/big.bin) bytes\"")
    # Both timed runs without -V: verifying re-encrypts every block on the client, which would
    # compete with the Worker Pool for the Pi's four cores. Correctness is checked by decrypt + cmp.
    if compare:
        script += "heading 'One at a time' '-j 1: the next Data Block goes out when the previous one is back'\n"
        script += bg("j1", f"encclient -e {w}/big.bin -o {w}/big-1.enc -j 1",
                     "./encclient -e $W/big.bin -o $W/big-1.enc -j 1")
    script += f"heading 'In parallel' {q(f'-j {j}: {j} sender threads, {j} Data Blocks in flight at once')}\n"
    script += sh_info("Each sender thread sends its Data Block and waits for its own Event; in the encserver",
                      f"window several Jobs are in progress together (up to {j}), sharing the 4 workers.")
    script += bg("jn", f"encclient -e {w}/big.bin -o {w}/big.enc -j {j}",
                 f"./encclient -e $W/big.bin -o $W/big.enc -j {j}")
    script += sh_info(f"The Encryption Time is summed over the {blocks} Jobs: with Data Blocks in flight it is",
                      "larger than the wall time, because the Jobs overlapped. That is the concurrency.")
    script += "heading Check 'decrypt on the client and compare'\n"
    script += cmd("dec", f"encclient -d {w}/big.enc -o {w}/big.out",
                  "rm -f $W/big.out; ./encclient -d $W/big.enc -o $W/big.out")
    script += cmd("cmp", f"cmp {w}/big.bin {w}/big.out",
                  "cmp -s $W/big.bin $W/big.out; r=$?; "
                  "if [ $r -eq 0 ]; then good \"identical: big.out has the same $(wc -c < $W/big.bin) bytes "
                  "as big.bin\"; else fail 'DIFFERENT from big.bin'; fi; "
                  "info \"cksum original: $(cksum < $W/big.bin)\"; info \"cksum restored: $(cksum < $W/big.out)\"; "
                  "(exit $r)")
    script += "rm -f $W/big-1.enc\n"
    rc, lines = ctx.remote(script, timeout=180)
    time.sleep(0.6)                     # let the last encserver log lines arrive
    b = parsers.blocks(lines)
    pids = parsers.pids(lines)
    state = ctx.engine.store.snapshot()

    runs = []
    for tag, label, inflight in (("j1", "one at a time", 1), ("jn", f"{j} in flight", j)):
        if tag not in b:
            continue
        enc = parsers.encrypt_output(b[tag]["lines"])
        if not enc.get("ok"):
            raise StepError(f"encrypting big.bin ({label}) failed: {(enc.get('errors') or ['see console'])[0]}")
        jobs = jobs_of(state, {pids.get(tag)})
        peak = max((x.get("in_progress") or 0 for x in jobs), default=None)
        wall = enc.get("wall_ms")
        runs.append({"label": label, "in_flight": inflight, "wall_ms": wall, "server_ms": enc.get("server_ms"),
                     "blocks": enc.get("blocks"), "segments": enc.get("segments"),
                     "mb_per_s": (mb / (wall / 1000)) if wall else None, "jobs_seen": len(jobs), "peak": peak,
                     "pid": pids.get(tag)})
    identical = b.get("cmp", {}).get("rc") == 0
    if not identical:
        raise StepError("the decrypted big file differs from the original")

    entry = {"size_mb": mb, "runs": runs, "identical": identical,
             "speedup": (runs[0]["wall_ms"] / runs[-1]["wall_ms"]) if len(runs) == 2 and runs[-1]["wall_ms"] else None}
    ctx.engine.store.update(lambda s: s["analytics"].__setitem__("inflight", [entry]))

    ctx.heading("Summary", "")
    for r in runs:
        ctx.say(f"  {r['label']:<14} wall {fmt_ms(r['wall_ms']):>10}  {r['mb_per_s'] or 0:5.1f} MB/s   "
                f"up to {r['peak'] or '?'} Job{'s' if (r['peak'] or 0) != 1 else ''} at once on encserver (client pid {r['pid']})")
    if entry["speedup"]:
        sp = entry["speedup"]
        (ctx.good if sp >= 1 else ctx.info)(f"{j} in flight: {sp:.2f}x {'faster' if sp >= 1 else 'as fast'} "
                                            f"than one at a time")
    rows = [[r["label"], fmt_ms(r["wall_ms"]), f"{r['mb_per_s']:.1f} MB/s" if r["mb_per_s"] else "-",
             fmt_ms(r["server_ms"]), r["blocks"], r["segments"],
             r["peak"] if r["peak"] is not None else "-"] for r in runs]
    facts = [("File", f"{mb} MB of random bytes = {runs[-1]['blocks']} Data Blocks of up to 2 MB"),
             ("Result", "decrypted file identical to the original (cmp)")]
    if entry["speedup"]:
        sp = entry["speedup"]
        facts.insert(1, (f"{j} in flight vs one at a time",
                         f"{sp:.2f}x {'faster' if sp >= 1 else 'as fast (slower)'}"))
    if runs[-1]["peak"]:
        facts.insert(2, ("Most Jobs at once on the server", str(runs[-1]["peak"])))
    return {"facts": facts,
            "tables": [{"title": "Encrypting the large file", "columns": [
                "Run", "Wall time", "Throughput", "Server encryption (sum)", "Data Blocks", "Segments",
                "Most Jobs at once"], "rows": rows}]}


def step_multiclient(ctx, opt):
    _need_server(ctx)
    n, runs = int(opt["clients"]), int(opt["runs"])
    w = config.PI_SCRATCH
    script = f"W={w}; [ -d $W ] || mkdir $W; cd {config.PI_DIR} || exit 1\n"
    script += f"heading 'One client alone' {q(f'{runs} requests of 2 MB: the baseline')}\n"
    script += bg("solo", f"encclient -s 2097152 -n {runs} -V -S", f"./encclient -s 2097152 -n {runs} -V -S")
    script += "info 'workers bytes runs, then mean and fastest Encryption Time and Round-Trip Time in microseconds'\n"
    script += f"heading Together {q(f'{n} encclient processes start at the same moment')}\n"
    script += sh_info("encserver's Receive Thread keeps accepting while earlier Jobs run, so the clients' Jobs",
                      "interleave in the encserver window: look for the different pids in its CLIENT column.")
    # Start every client in the background, note its pid (to find its Jobs in encserver's log),
    # then wait for each and append its exit status to its output.
    script += cmd("together", f"{n} x encclient -s 2097152 -n {runs} -V -S &",
                  f"pids=''; i=1; while [ $i -le {n} ]; do "
                  f"./encclient -s 2097152 -n {runs} -V -S > $W/client$i.out 2>&1 & "
                  f"echo \"@@pid client$i $!\"; info \"started client $i (pid $!)\"; pids=\"$pids $!\"; "
                  f"i=$((i+1)); done; "
                  f"i=1; for p in $pids; do wait $p; echo \"exit $?\" >> $W/client$i.out; i=$((i+1)); done; "
                  f"good 'all clients finished'")
    for i in range(1, n + 1):
        script += cmd(f"client{i}", f"cat {w}/client{i}.out", f"boxed < $W/client{i}.out")
    rc, lines = ctx.remote(script, timeout=120 + n * runs * 2)
    time.sleep(0.6)
    b = parsers.blocks(lines)
    client_pids = {pid for tag, pid in parsers.pids(lines).items() if tag.startswith("client")}
    state = ctx.engine.store.snapshot()

    solo_lines = b.get("solo", {}).get("lines", [])
    solo = next((parsers.bench_summary(l) for l in solo_lines if parsers.bench_summary(l)), None)
    if solo is None:
        raise StepError("the single client run failed: " + (" ".join(solo_lines[-2:]) or "no output"))
    clients, failed = [], 0
    for i in range(1, n + 1):
        out = [unbox(l) for l in b.get(f"client{i}", {}).get("lines", [])[1:]]
        summary = next((parsers.bench_summary(l) for l in out if parsers.bench_summary(l)), None)
        code = next((int(l.split()[1]) for l in out if re.match(r"^exit \d+$", l.strip())), None)
        ok = summary is not None and code == 0
        failed += 0 if ok else 1
        clients.append({"client": i, "ok": ok, **(summary or {}),
                        "error": None if ok else (" ".join(l for l in out if not l.startswith("exit")) or "failed")})
    jobs = jobs_of(state, client_pids)
    pids = sorted({x["pid"] for x in jobs})
    peak = max((x.get("in_progress") or 0 for x in jobs), default=None)
    entry = {"clients": n, "runs": runs, "solo": solo, "together": clients, "pids": len(pids), "peak": peak}
    ctx.engine.store.update(lambda s: s["analytics"].__setitem__("multiclient", [entry]))
    if failed:
        raise StepError(f"{failed} of {n} clients failed: {next(c['error'] for c in clients if not c['ok'])}")

    ctx.heading("Summary", "mean Round-Trip Time of a 2 MB Data Block")
    ctx.say(f"  {'alone':<10} {fmt_ms(solo['rtt_mean_ms']):>10}")
    for c in clients:
        ctx.say(f"  {'client ' + str(c['client']):<10} {fmt_ms(c['rtt_mean_ms']):>10}   "
                f"{c['rtt_mean_ms'] / solo['rtt_mean_ms']:.2f}x the single client")
    ctx.good(f"encserver served {len(pids)} client processes, {len(jobs)} Jobs, up to {peak or '?'} at once; "
             f"every Encrypted Block verified")
    rows = [["alone (baseline)", runs, fmt_ms(solo["enc_mean_ms"]), fmt_ms(solo["rtt_mean_ms"]), "1.00x"]]
    for c in clients:
        rows.append([f"client {c['client']}", c["runs"], fmt_ms(c["enc_mean_ms"]), fmt_ms(c["rtt_mean_ms"]),
                     f"{c['rtt_mean_ms'] / solo['rtt_mean_ms']:.2f}x"])
    return {"facts": [("Clients", f"{n} processes at once, {runs} requests of 2 MB each, all verified"),
                      ("Seen by encserver", f"{len(pids)} client processes, {len(jobs)} Jobs"),
                      ("Most Jobs at once", str(peak) if peak else "-"),
                      ("Result", "every client got correct Encrypted Blocks")],
            "tables": [{"title": "Per client (mean of its requests)", "columns": [
                "Client", "Requests", "Encryption Time", "Round-Trip Time", "Round trip vs alone"],
                "rows": rows}]}


def step_benchmark(ctx, opt):
    e = ctx.engine
    size, runs, workers = int(opt["size"]), int(opt["runs"]), str(opt["workers"])
    listed = workers.replace(" ", ", ")
    if e.server.running() or e.store.snapshot()["server"]["external"]:
        e.server.stop(ctx, "the benchmark starts its own encserver for every Worker Pool size")
    script = (f"cd {config.PI_DIR} || exit 1\nHERE={config.PI_DIR}; . ./lib.sh; find_programs\n"
              "echo 'workers,bytes,segments,run,encryption_us,round_trip_us,verified' > results.csv\n"
              f"heading Benchmark {q(f'{runs} runs of {fmt_bytes(size)} for {listed} workers')}\n" +
              sh_info("Each Worker Pool size gets a fresh encserver (encserver -w N), 5 warm-up requests,",
                      f"then {runs} measured ones. Every run is one row in results.csv.") +
              cmd("bench", f"scaling_table enc.key {size} {runs} 5 results.csv {workers}",
                  f"scaling_table ./enc.key {size} {runs} 5 results.csv {workers}") +
              sh_info("SPEEDUP = mean Encryption Time with 1 worker / mean with N workers.",
                      "The Pi 4B has 4 cores: the speedup grows up to 4 workers and levels off beyond."))
    count = len(workers.split())
    rc, lines = ctx.remote(script, timeout=180 + count * (runs + 5) * 2)
    if parsers.blocks(lines).get("bench", {}).get("rc") != 0:
        raise StepError("the benchmark stopped: " + next((l.strip() for l in reversed(lines)
                                                          if any(m in l for m in parsers.FAIL_MARKS)),
                                                         "see the step's console"))
    ctx.show("cat results.csv   (copied to the dashboard)")
    code, text = remote.quick(f"cat {config.PI_DIR}/results.csv", timeout=30)
    measured = parsers.bench_csv(text)
    if not measured:
        raise StepError("results.csv on the Pi has no measured runs")
    stats = parsers.bench_stats(measured)
    ctx.good(f"{len(measured)} measured runs in results.csv")
    e.benchmark_csv = text
    bench = {"size": size, "runs": runs, "workers": [int(x) for x in workers.split()], "stats": stats,
             "at": datetime.now().isoformat(timespec="seconds"), "rows": len(measured)}
    e.store.update(lambda s: s["analytics"].__setitem__("benchmark", bench))

    best = max(stats, key=lambda r: r["speedup"] or 0)
    ctx.good(f"best speedup {best['speedup']:.2f}x with {best['workers']} workers "
             f"({best['mb_per_s']:.1f} MB/s); encserver stays stopped until step 5")
    rows = [[r["workers"], r["segments"], fmt_ms(r["enc_mean"]), fmt_ms(r["enc_min"]), fmt_ms(r["enc_max"]),
             fmt_ms(r["rtt_mean"]), f"{r['mb_per_s']:.1f}", f"{r['speedup']:.2f}x"] for r in stats]
    return {"facts": [("Measured", f"{len(measured)} runs of {fmt_bytes(size)}; the first of each size verified"),
                      ("Best speedup", f"{best['speedup']:.2f}x with {best['workers']} workers"),
                      ("Fastest", f"{max(r['mb_per_s'] for r in stats):.1f} MB/s"),
                      ("encserver", "stopped; start it again in step 5 to run more files")],
            "tables": [{"title": "Encryption Time by Worker Pool size", "columns": [
                "Workers", "Segments", "Mean", "Min", "Max", "Round trip", "MB/s", "Speedup"], "rows": rows}]}


def step_finish(ctx, opt):
    e = ctx.engine
    ctx.heading("Finish", "")
    if opt["stop_server"] and (e.server.running() or e.store.snapshot()["server"]["external"]):
        e.server.stop(ctx, "the workflow is finished")
    w = config.PI_SCRATCH
    script = ("heading 'Clean up' 'the files the workflow made in the Pi RAM'\n" +
              cmd("clean", f"rm -rf {w}",
                  f"if [ -d {w} ]; then rm -rf {w} && good 'removed {w}'; "
                  f"else info 'nothing to remove: {w} is already gone'; fi") +
              f"heading Kept {q(config.PI_DIR)}\n" +
              cmd("kept", f"ls {config.PI_DIR}", f"ls {config.PI_DIR}") +
              f"rm -f {config.PI_STEP_SCRIPT}\n")
    ctx.remote(script, timeout=30)
    s = e.store.snapshot()
    ctx.good(f"this session: {s['jobs']['count']} Jobs encrypted by the live encserver, "
             f"{fmt_bytes(s['jobs']['bytes'])}")
    return {"facts": [("encserver", "running" if e.server.running() else "stopped"),
                      ("Pi RAM", f"{w} removed"),
                      ("Kept on the Pi", f"programs, examples, enc.key and results.csv in {config.PI_DIR}"),
                      ("This session", f"{s['jobs']['count']} Jobs encrypted by the live encserver, "
                                       f"{fmt_bytes(s['jobs']['bytes'])}")]}


RUNNERS = {"connect": step_connect, "build": step_build, "deploy": step_deploy, "key": step_key,
           "server": step_server, "roundtrip": step_roundtrip, "concurrency": step_concurrency,
           "multiclient": step_multiclient, "benchmark": step_benchmark, "finish": step_finish}


# ---------------------------------------------------------------- engine

class Engine:
    """Runs one step at a time and keeps the step states in the Store."""

    def __init__(self, store, consoles):
        self.store = store
        self.consoles = consoles
        self.current = None
        self.server = ServerManager(self)
        self.benchmark_csv = None
        self._lock = threading.Lock()
        self.activity = None
        self._new_logs()

    def _new_logs(self):
        path = os.path.join(self.store.session_dir, "activity.log")
        self.activity = LogFile(path, "Dashboard activity - every step, as it runs")

    def activity_path(self):
        return self.activity.path

    def reset(self):
        if self.current:
            return False, "a step is still running"
        if self.server.running():
            self.server.stop(None, "the session was reset")
        close_session(self.store.session_dir)
        self.store.reset()
        self.server = ServerManager(self)
        self.benchmark_csv = None
        self._new_logs()
        return True, "new session"

    def blocked(self, step_id, state=None):
        """Why the step cannot run now, or None."""
        s = state or self.store.snapshot()
        step = STEP_BY_ID[step_id]
        if s["busy"]:
            return f"step {STEP_BY_ID[s['busy']]['n']} is running"
        missing = [STEP_BY_ID[r] for r in step["requires"] if s["steps"].get(r, {}).get("state") != "done"]
        if missing:
            m = missing[0]
            return f"run step {m['n']} ({m['title']}) first"
        if step.get("needs_server") and not self.server.running():
            return "encserver is not running; start it in step 5"
        return None

    def start(self, step_id, options):
        if step_id not in STEP_BY_ID:
            return False, "no such step"
        with self._lock:
            why = self.blocked(step_id)
            if why:
                return False, why
            step = STEP_BY_ID[step_id]
            opt = {o["name"]: options.get(o["name"], o["default"]) for o in step["options"]}
            for o in step["options"]:          # accept only the offered values
                if o["type"] == "select" and str(opt[o["name"]]) not in [str(c["value"]) for c in o["choices"]]:
                    return False, f"bad value for {o['label']}"
                if o["type"] == "select":
                    opt[o["name"]] = next(c["value"] for c in o["choices"] if str(c["value"]) == str(opt[o["name"]]))
                if o["type"] == "check":
                    opt[o["name"]] = bool(opt[o["name"]])
            ctx = Ctx(self, step, opt)
            self.current = ctx
            started = ctx.started

            def mark_running(s):
                s["busy"] = step_id
                prev = s["steps"].get(step_id, {})
                s["steps"][step_id] = {"state": "running", "started": started, "finished": None,
                                       "seconds": None, "error": None, "result": None, "options": opt,
                                       "runs": prev.get("runs", 0) + 1, "log": os.path.basename(ctx.log_path)}
            self.store.update(mark_running)
        threading.Thread(target=self._run, args=(ctx,), daemon=True).start()
        return True, "started"

    def _run(self, ctx):
        step = ctx.step
        result, error, outcome = None, None, "done"
        try:
            result = RUNNERS[step["id"]](ctx, ctx.options)
        except Cancelled:
            outcome, error = "cancelled", "cancelled"
            self._cleanup_after_cancel(step["id"])
        except StepError as e:
            outcome, error = "failed", str(e)
        except Exception as e:          # a bug: show it rather than leave the step spinning
            outcome, error = "failed", f"internal error: {e!r}"
        finished = time.time()

        def mark_done(s):
            st = s["steps"][step["id"]]
            st.update(state=outcome, finished=finished, seconds=round(finished - st["started"], 1),
                      error=error, result=_jsonable(result))
            s["busy"] = None
        if error and outcome == "failed":
            ctx.say("")
            ctx.bad(error)
        took = f"{finished - ctx.started:.1f} s"
        if outcome == "done":
            footer = f"{_GREEN}=== Step {step['n']} done in {took}{_RESET}  {_DIM}the results are in the dashboard{_RESET}"
        elif outcome == "failed":
            footer = f"{_RED}=== Step {step['n']} failed after {took}{_RESET}"
        else:
            footer = f"{_DIM}=== Step {step['n']} cancelled after {took}{_RESET}"
        ctx.log.finish(footer)
        self.activity.write(footer)
        self.current = None
        self.store.update(mark_done)

    def _cleanup_after_cancel(self, step_id):
        if step_id in ("roundtrip", "concurrency", "multiclient", "benchmark"):
            remote.quick("slay -f -Q encclient", timeout=15)
        if step_id == "benchmark":
            remote.quick("slay -f -Q encserver", timeout=15)

    def cancel(self):
        ctx = self.current
        if ctx is None:
            return False, "nothing is running"
        ctx.cancelled = True
        ctx.kill()
        return True, "cancelling"

    def open_console(self, which):
        if which == "activity":
            return self.consoles.open(self.activity.path, "Dashboard activity", until_done=False)
        if which == "server":
            if self.server.log is None:
                return False
            return self.consoles.open(self.server.log.path, "encserver on the Pi - live log", until_done=False)
        st = self.store.snapshot()["steps"].get(which)
        if not st:
            return False
        step = STEP_BY_ID[which]
        path = os.path.join(self.store.session_dir, st["log"])
        return self.consoles.open(path, f"Step {step['n']}: {step['title']}", until_done=True)


def _jsonable(result):
    if not result:
        return result
    out = dict(result)
    if "facts" in out:
        out["facts"] = [{"label": a, "value": b} for a, b in out["facts"]]
    return out


def close_session(session_dir):
    """Tell the console windows of this session that the backend has finished with it."""
    try:
        with open(os.path.join(session_dir, ".closed"), "w") as f:
            f.write(datetime.now().isoformat())
    except OSError:
        pass
