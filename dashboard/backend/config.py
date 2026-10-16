"""
config.py - where things are: the Pi, the project, the dashboard files.

Every value can be overridden with an environment variable before starting
the backend, for example:  set ENC_PI_HOST=192.168.1.50
"""
import os
import sys

# ---------------------------------------------------------------- the Pi
PI_HOST = os.environ.get("ENC_PI_HOST", "10.0.0.1")
PI_USER = os.environ.get("ENC_PI_USER", "qnxuser")
# Default password of the QNX 8.0 Raspberry Pi image. ssh reads it through
# askpass.cmd (SSH_ASKPASS) and only if this PC has no SSH key on the Pi.
PI_PASSWORD = os.environ.get("ENC_PI_PASSWORD", "qnxuser")

PI_DIR = "/var/encsvc"              # programs, scripts, examples, enc.key
PI_SCRATCH = "/dev/shmem/encdash"   # RAM folder for the files the workflow creates
PI_STEP_SCRIPT = "/dev/shmem/encdash-step.sh"   # each step's script is copied here, then run

# ---------------------------------------------------------------- this PC
BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
DASHBOARD_DIR = os.path.dirname(BACKEND_DIR)
PROJECT_DIR = os.path.dirname(DASHBOARD_DIR)
WEB_DIR = os.path.join(DASHBOARD_DIR, "web")
RUNS_DIR = os.environ.get("ENC_DASH_RUNS", os.path.join(DASHBOARD_DIR, "runs"))  # logs, one folder per session
ASKPASS = os.path.join(BACKEND_DIR, "askpass.cmd")
VIEWER = os.path.join(BACKEND_DIR, "console_viewer.py")

BIN_DIR = os.path.join(PROJECT_DIR, "bin", "aarch64le")
EXAMPLES_DIR = os.path.join(PROJECT_DIR, "examples")

PORT = int(os.environ.get("ENC_DASH_PORT", "8080"))

# 0 = never open command-prompt windows (used by the automated tests).
OPEN_CONSOLES = os.environ.get("ENC_DASH_CONSOLES", "1") != "0" and sys.platform == "win32"

# Example files offered by the file round-trip step (README.md is not one).
EXAMPLE_FILES = ["hello.txt", "config.json", "report.pdf", "sensor-log.csv", "tone.wav", "qnx-pi.png"]


def sdp_env_script():
    """Path of qnxsdp-env.bat, "PATH" if qcc is already on PATH, or None."""
    home = os.environ.get("USERPROFILE", os.path.expanduser("~"))
    candidates = [
        os.environ.get("ENC_SDP_ENV", ""),
        os.path.join(home, "qnx800", "qnxsdp-env.bat"),
        r"C:\qnx800\qnxsdp-env.bat",
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    from shutil import which
    return "PATH" if which("qcc") else None
