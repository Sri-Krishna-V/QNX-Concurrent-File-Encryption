"""
session.py - what the dashboard knows during one backend run.

  Store     the state shown in the browser. It starts empty every time the
            backend starts; every change is pushed to the browser (SSE).
  LogFile   a text log that a console window can follow.
  Consoles  opens command-prompt windows that follow a log (console_viewer.py).
"""
import copy
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime

import config


# ---------------------------------------------------------------- browser push

class Hub:
    """Server-Sent Events: one queue per open browser tab."""

    def __init__(self):
        self._clients = []
        self._lock = threading.Lock()

    def add(self):
        q = queue.Queue(maxsize=50)
        with self._lock:
            self._clients.append(q)
        return q

    def remove(self, q):
        with self._lock:
            if q in self._clients:
                self._clients.remove(q)

    def publish(self, event, data):
        payload = f"event: {event}\ndata: {json.dumps(data)}\n\n"
        with self._lock:
            for q in list(self._clients):
                try:
                    q.put_nowait(payload)
                except queue.Full:
                    # A tab that stopped reading: drop its oldest message, keep the newest state.
                    try:
                        q.get_nowait()
                        q.put_nowait(payload)
                    except (queue.Empty, queue.Full):
                        pass


def empty_state(session_id):
    return {
        "session": {"id": session_id, "started": datetime.now().isoformat(timespec="seconds")},
        "busy": None,                   # id of the running step
        "pc": None,                     # filled in by Connect
        "pi": None,                     # filled in by Connect
        "server": {"running": False, "external": False, "workers": None, "backend": None,
                   "started": None, "note": None},
        "steps": {},                    # id -> {state, started, finished, seconds, error, result, runs}
        "jobs": {"list": [], "count": 0, "failed": 0, "bytes": 0, "peak_in_progress": 0,
                 "per_worker": [], "pids": []},
        "analytics": {"benchmark": None, "roundtrips": [], "inflight": [], "multiclient": []},
    }


class Store:
    """The dashboard state. Change it only through update(); every change is published."""

    def __init__(self, hub):
        self.hub = hub
        self._lock = threading.RLock()
        self._pending = False
        self.session_dir = None
        self.state = None
        self.decorate = lambda snap: snap      # server.py adds derived fields before publishing
        self.reset()

    def reset(self):
        session_id = datetime.now().strftime("%Y%m%d-%H%M%S")
        prune_runs(keep=10)
        with self._lock:
            self.session_dir = os.path.join(config.RUNS_DIR, session_id)
            os.makedirs(self.session_dir, exist_ok=True)
            self.state = empty_state(session_id)
        self.publish_now()

    def snapshot(self):
        with self._lock:
            return copy.deepcopy(self.state)

    def update(self, fn, immediate=True):
        """fn(state) changes the state in place. With immediate=False, bursts are merged (≤5/s)."""
        with self._lock:
            fn(self.state)
        if immediate:
            self.publish_now()
        else:
            self._publish_soon()

    def publish_now(self):
        self.hub.publish("state", self.decorate(self.snapshot()))

    def _publish_soon(self):
        with self._lock:
            if self._pending:
                return
            self._pending = True

        def later():
            time.sleep(0.2)
            with self._lock:
                self._pending = False
            self.publish_now()

        threading.Thread(target=later, daemon=True).start()


# ---------------------------------------------------------------- logs

def prune_runs(keep):
    """Delete all but the newest `keep` session log folders."""
    try:
        names = sorted(n for n in os.listdir(config.RUNS_DIR)
                       if os.path.isdir(os.path.join(config.RUNS_DIR, n)))
    except OSError:
        return
    for name in names[:-keep] if len(names) > keep else []:
        shutil.rmtree(os.path.join(config.RUNS_DIR, name), ignore_errors=True)


class LogFile:
    """
    Append-only UTF-8 log. Terminal output from the Pi is kept exactly as it came
    (colours, '\\r' progress redraws), so a console window can replay it as it looked.
    A '<path>.done' file marks the log finished for the console viewer.
    """

    def __init__(self, path, title):
        self.path = path
        self._lock = threading.Lock()
        self._at_line_start = True
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(f"### {title}\n")

    def write(self, line):
        """Append one whole line."""
        with self._lock, open(self.path, "a", encoding="utf-8", newline="") as f:
            f.write(("" if self._at_line_start else "\n") + line.rstrip("\r\n") + "\n")
            self._at_line_start = True

    def write_raw(self, text):
        """Append terminal output as it arrived, possibly part of a line."""
        if not text:
            return
        with self._lock, open(self.path, "a", encoding="utf-8", newline="") as f:
            f.write(text)
            self._at_line_start = text.endswith("\n")

    def finish(self, footer):
        self.write(footer)
        with open(self.path + ".done", "w", encoding="utf-8") as f:
            f.write(footer + "\n")


# ---------------------------------------------------------------- console windows

class Consoles:
    """Command-prompt windows, one per followed log; reopening a closed one starts a new window."""

    def __init__(self):
        self._windows = {}              # log path -> Popen
        self._lock = threading.Lock()

    def open(self, path, title, until_done):
        """Open (or keep) a window that shows the log at `path` and follows it."""
        if not config.OPEN_CONSOLES:
            return False
        with self._lock:
            proc = self._windows.get(path)
            if proc is not None and proc.poll() is None:
                return True             # already on screen
            argv = [sys.executable, config.VIEWER, path, "--title", title]
            if until_done:
                argv.append("--until-done")
            self._windows[path] = subprocess.Popen(
                argv, cwd=config.PROJECT_DIR, creationflags=subprocess.CREATE_NEW_CONSOLE)
            return True

    def is_open(self, path):
        with self._lock:
            proc = self._windows.get(path)
            return proc is not None and proc.poll() is None
