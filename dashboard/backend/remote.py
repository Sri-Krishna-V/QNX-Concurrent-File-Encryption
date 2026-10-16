"""
remote.py - how the backend reaches the Pi: ping, ssh and scp.

ssh never prompts on a console: if this PC has no SSH key on the Pi, ssh asks
askpass.cmd for the password (SSH_ASKPASS_REQUIRE=force), which prints
config.PI_PASSWORD.
"""
import os
import re
import shutil
import subprocess
import sys

import config


def _exe(name):
    # Prefer Windows' own OpenSSH: Git's ssh writes a file literally called NUL
    # for UserKnownHostsFile=NUL and ignores SSH_ASKPASS_REQUIRE.
    if sys.platform == "win32":
        system = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "OpenSSH",
                              name + ".exe")
        if os.path.isfile(system):
            return system
    return shutil.which(name)


SSH = _exe("ssh")
SCP = _exe("scp")

OPTIONS = [
    "-o", "ConnectTimeout=5",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=" + ("NUL" if sys.platform == "win32" else "/dev/null"),
    "-o", "LogLevel=ERROR",
    "-o", "MACs=hmac-sha2-256",         # the one MAC the QNX image's sshd and Windows agree on
    "-o", "NumberOfPasswordPrompts=1",
    "-o", "ServerAliveInterval=5",      # notice a Pi that went away during a long step
]


def target():
    return f"{config.PI_USER}@{config.PI_HOST}"


def env():
    e = os.environ.copy()
    e["SSH_ASKPASS"] = config.ASKPASS
    e["SSH_ASKPASS_REQUIRE"] = "force"
    e["DISPLAY"] = "dashboard:0"
    e["ENC_PI_PASSWORD"] = config.PI_PASSWORD
    return e


def ssh_argv(remote_command, key_only=False, tty=False):
    """tty=True gives the command a terminal on the Pi (ssh -tt), so the programs print what
    they print for a person: colours, Unicode symbols, progress bars."""
    extra = ["-o", "BatchMode=yes"] if key_only else []
    if tty:
        extra.append("-tt")
    return [SSH] + extra + OPTIONS + [target(), remote_command]


def upload_text(text, path, timeout=20):
    """Write `text` to `path` on the Pi. Returns (exit code, output)."""
    folder = path.rsplit("/", 1)[0]
    # QNX's mkdir -p complains about the read-only /dev even when /dev/shmem exists, so test first.
    command = f"{{ [ -d {folder} ] || mkdir -p {folder}; }} && cat > {path}"
    try:
        res = subprocess.run(ssh_argv(command), input=text.encode("utf-8"),
                             capture_output=True, env=env(), timeout=timeout)
    except subprocess.TimeoutExpired:
        return 255, "ssh timed out"
    except OSError as e:
        return 255, str(e)
    return res.returncode, (res.stdout + res.stderr).decode("utf-8", errors="replace")


def scp_argv(sources, destination, recursive=False, verbose=False):
    """verbose=True: scp -v, whose log names each file it finishes and the bytes and time in total."""
    flags = (["-r"] if recursive else []) + (["-v"] if verbose else [])
    return [SCP] + flags + OPTIONS + list(sources) + [f"{target()}:{destination}"]


def ping(host):
    """Return the round-trip time in ms of one ping, or None if there was no answer."""
    if sys.platform == "win32":
        argv = ["ping", "-n", "1", "-w", "1500", host]
    else:
        argv = ["ping", "-c", "1", "-W", "2", host]
    try:
        out = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                             timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    # Windows: "time=1ms" / "time<1ms" (any language uses the same token); Linux: "time=0.42 ms"
    m = re.search(r"(?:time|zeit|temps|tiempo)[=<]\s*([\d.,]+)\s*ms", out, re.IGNORECASE)
    if m and ("TTL=" in out.upper()):
        return float(m.group(1).replace(",", "."))
    return None


def quick(remote_command, timeout=15, key_only=False):
    """Run a short command on the Pi. Returns (exit code, output). Exit code 255: ssh failed."""
    try:
        res = subprocess.run(ssh_argv(remote_command, key_only), stdin=subprocess.DEVNULL,
                             capture_output=True, env=env(), timeout=timeout)
    except subprocess.TimeoutExpired:
        return 255, "ssh timed out"
    except OSError as e:
        return 255, str(e)
    text = (res.stdout + res.stderr).decode("utf-8", errors="replace")
    return res.returncode, text
