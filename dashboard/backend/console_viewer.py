"""
console_viewer.py - runs inside a command-prompt window that the dashboard opens.

Shows a log written by the backend and keeps following it:
    console_viewer.py LOG [--title TEXT] [--until-done]

--until-done   stop when LOG.done appears (one workflow step);
               otherwise follow until the backend session ends (activity, encserver).

The logs hold the Pi's terminal output exactly as it came: colours, Unicode symbols, and
progress bars redrawn with '\\r'. This window replays it the same way, as it arrives, so
it looks like an SSH window on the Pi. Lines starting with "@@" are markers for the
backend's parsers and are not shown.
"""
import argparse
import codecs
import os
import sys
import time

RESET, BOLD, DIM = "\033[0m", "\033[1m", "\033[2m"
RED, GREEN, YELLOW, CYAN, MAGENTA = "\033[91m", "\033[92m", "\033[93m", "\033[96m", "\033[95m"


def enable_terminal():
    """Turn on ANSI colours (virtual terminal processing) and UTF-8 output in this console."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)                     # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if k.GetConsoleMode(h, ctypes.byref(mode)):
            k.SetConsoleMode(h, mode.value | 0x0004)    # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        k.SetConsoleOutputCP(65001)
    except (ImportError, AttributeError, OSError):
        os.system("")                               # older fallback: also enables VT


def set_title(title):
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.kernel32.SetConsoleTitleW(title)
        except (ImportError, AttributeError, OSError):
            pass


def colour(line):
    """Colours for lines that have none of their own (written by the backend or saved files)."""
    s = line.lstrip()
    if line.startswith("###"):
        return f"\n{BOLD}{MAGENTA}{line}{RESET}"
    if s.startswith("$ "):
        return f"{YELLOW}{line}{RESET}"
    if s.startswith("== "):
        return f"{BOLD}{CYAN}{line}{RESET}"
    low = s.lower()
    if s.startswith("!!") or " !! " in s or "error" in low or "fail" in low or "cannot" in low or "denied" in low:
        return f"{RED}{line}{RESET}"
    if s.startswith("OK") or "identical" in low or "passed" in low or s.startswith("#") and " OK " in s:
        return f"{GREEN}{line}{RESET}"
    if s.startswith("#") and " -> " in s:
        return f"{CYAN}{line}{RESET}"
    return line


class Screen:
    """Prints log text as it arrives: whole lines, and also half lines (progress bars)."""

    def __init__(self, out):
        self.out = out
        self.line = ""          # the current line so far
        self.shown = 0          # how much of it is already on screen

    def feed(self, text):
        for part in text.splitlines(keepends=True):
            if part.endswith("\n"):
                self.line += part[:-1]
                self._end_line()
            else:
                self.line += part
        self._show_partial()
        self.out.flush()

    def _end_line(self):
        line = self.line.rstrip("\r")
        if self.shown == 0:
            if not line.startswith("@@"):
                self.out.write((line if "\x1b[" in line else colour(line)) + "\n")
        else:
            self.out.write(line[self.shown:] + "\n")
        self.line, self.shown = "", 0

    def _show_partial(self):
        # A half line is shown as soon as it cannot be a hidden "@@" marker: that is what makes
        # "progress ███░░ 3/6" move while it is drawn.
        if not self.line or self.line.startswith("@") and len(self.line) < 2 or self.line.startswith("@@"):
            return
        self.out.write(self.line[self.shown:])
        self.shown = len(self.line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--title", default="QNX encryption")
    ap.add_argument("--until-done", action="store_true")
    args = ap.parse_args()

    enable_terminal()
    set_title(args.title)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    done_marker = args.log + ".done"
    closed_marker = os.path.join(os.path.dirname(args.log), ".closed")
    print(f"{BOLD}{args.title}{RESET}")
    print(f"{DIM}Live output from the dashboard. It is also saved in {args.log}{RESET}")

    screen = Screen(sys.stdout)
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    pos = 0
    try:
        while True:
            finished = os.path.exists(done_marker) if args.until_done else os.path.exists(closed_marker)
            try:
                with open(args.log, "rb") as f:
                    f.seek(pos)
                    data = f.read()
            except OSError:
                data = b""
            if data:
                pos += len(data)
                screen.feed(decoder.decode(data))       # keeps a half-written character for later
            elif finished:
                break
            else:
                time.sleep(0.1)
    except KeyboardInterrupt:
        pass

    if args.until_done:
        print(f"\n{DIM}This step has finished. Its results are in the dashboard.{RESET}")
    else:
        print(f"\n{DIM}The dashboard backend has stopped.{RESET}")
    try:
        input(f"{DIM}Press Enter to close this window.{RESET}")
    except (EOFError, KeyboardInterrupt):
        pass


if __name__ == "__main__":
    main()
