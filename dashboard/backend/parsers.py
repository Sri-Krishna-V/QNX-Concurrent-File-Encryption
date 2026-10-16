"""
parsers.py - turn the text that encclient, encserver and bench.sh print into numbers.

The steps run on a terminal on the Pi (ssh -tt), so the programs print what they
print for a person: colours, Unicode symbols ("✔", "→", "█" bars) and progress bars
redrawn with '\\r'. clean_lines() reduces that to plain lines first. Output saved to
files on the Pi is plain ("OK", "->", "#"); both forms are accepted. The formats come
from src/core/display.c, src/qnx/client.c, src/qnx/client_file.c and src/qnx/server.c.
"""
import re
import statistics

LABEL_WIDTH = 12                    # UI_LABEL_WIDTH in display.c

_UNITS = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3}
_ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
OK_MARKS = ("OK", "✔")         # ✔
FAIL_MARKS = ("!!", "✖")       # ✖


def clean(line):
    """One terminal line as a person reads it: no colour codes, only the last '\\r' redraw."""
    line = line.rstrip("\r")
    if "\r" in line:
        line = line.rsplit("\r", 1)[1]
    return _ANSI.sub("", line)


def clean_lines(text):
    return [clean(l) for l in text.split("\n")]


def size_to_bytes(text):
    """'2.0 MB' -> 2097152 (approximate: display.c rounds to one decimal)."""
    m = re.match(r"\s*([\d.]+)\s*(B|KB|MB|GB)\b", text)
    return int(round(float(m.group(1)) * _UNITS[m.group(2)])) if m else None


def ms_value(text):
    m = re.search(r"([\d.]+)\s*ms", text)
    return float(m.group(1)) if m else None


def count_value(text):
    """'186,167 bytes' -> 186167"""
    m = re.search(r"([\d,]+)\s*bytes?", text)
    return int(m.group(1).replace(",", "")) if m else None


def rows(lines):
    """The '  label        value' rows that ui_row() prints, as {label: value}."""
    out = {}
    for line in lines:
        if line.startswith("  ") and len(line) > LABEL_WIDTH + 2 and not line.startswith("   "):
            label = line[2:2 + LABEL_WIDTH].strip()
            if label:
                out.setdefault(label, line[2 + LABEL_WIDTH + 1:].strip())
    return out


def hex_rows(lines):
    """'    original    48 65 6c ...  Hello' -> {'original': '48 65 6c ...'}"""
    out = {}
    for line in lines:
        m = re.match(r"^    (original|encrypted|decrypted)\s+((?:[0-9a-f]{2}\s+)+)", line)
        if m:
            out[m.group(1)] = " ".join(m.group(2).split())
    return out


def errors(lines):
    """encclient / encserver error lines ('encclient: !! ...' or 'encclient: ✖ ...')."""
    return [l.strip() for l in lines if re.match(r"^\s*(encclient|encserver|ls|cmp|sh|ksh):", l)
            or any(m in l for m in FAIL_MARKS) and not l.lstrip().startswith("#")]


def is_ok(status):
    return status.startswith(OK_MARKS)


# ---------------------------------------------------------------- blocks

def blocks(lines):
    """Split output on '@@begin TAG' / '@@end TAG rc=N' markers -> {tag: {'lines', 'rc'}}."""
    out, current = {}, None
    for line in lines:
        if line.startswith("@@begin "):
            current = line[8:].strip()
            out[current] = {"lines": [], "rc": None}
        elif line.startswith("@@end "):
            m = re.match(r"@@end (\S+) rc=(\d+)", line)
            if m and m.group(1) in out:
                out[m.group(1)]["rc"] = int(m.group(2))
            current = None
        elif current is not None:
            out[current]["lines"].append(line)
    return out


def pids(lines):
    """'@@pid TAG PID' lines -> {tag: pid}"""
    out = {}
    for line in lines:
        m = re.match(r"^@@pid (\S+) (\d+)\s*$", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


# ---------------------------------------------------------------- encclient

def encrypt_output(lines):
    r = rows(lines)
    res = {"ok": "status" in r and is_ok(r["status"]),
           "verified": "verified" in r.get("status", ""), "errors": errors(lines)}
    if "input" in r:
        m = re.match(r"(.+?)\s{2}", r["input"])
        res["name"] = m.group(1) if m else r["input"]
        res["bytes"] = count_value(r["input"])
    if "Data Blocks" in r:
        m = re.match(r"(\d+) of up to (.+?), (\d+) in flight", r["Data Blocks"])
        if m:
            res["blocks"] = int(m.group(1))
            res["block_size"] = size_to_bytes(m.group(2))
            res["in_flight"] = int(m.group(3))
    if "server" in r:
        m = re.match(r"(\d+) workers, ([\d]+) Segments? in (\d+) Jobs?, (.+) encrypting", r["server"])
        if m:
            res["workers"] = int(m.group(1))
            res["segments"] = int(m.group(2))
            res["jobs"] = int(m.group(3))
            res["server_ms"] = ms_value(m.group(4))
    if "wall time" in r:
        res["wall_ms"] = ms_value(r["wall time"])
    if "saved as" in r:
        m = re.search(r"\(([\d,]+) B readable header", r["saved as"])
        res["header_bytes"] = int(m.group(1).replace(",", "")) if m else None
        res["saved_as"] = r["saved as"].split("  ")[0]
    res.update({f"hex_{k}": v for k, v in hex_rows(lines).items()})
    return res


def decrypt_output(lines):
    r = rows(lines)
    res = {"ok": "status" in r and is_ok(r["status"]), "errors": errors(lines)}
    if "wall time" in r:
        res["wall_ms"] = ms_value(r["wall time"])
    if "saved as" in r:
        res["bytes"] = count_value(r["saved as"])
    res.update({f"hex_{k}": v for k, v in hex_rows(lines).items()})
    return res


def bench_summary(line):
    """encclient -S line: workers bytes runs enc_mean_us enc_min_us rtt_mean_us rtt_min_us."""
    parts = line.split()
    if len(parts) != 7 or not all(p.isdigit() for p in parts):
        return None
    w, b, n, em, emin, rm, rmin = (int(p) for p in parts)
    return {"workers": w, "bytes": b, "runs": n, "enc_mean_ms": em / 1000, "enc_min_ms": emin / 1000,
            "rtt_mean_ms": rm / 1000, "rtt_min_ms": rmin / 1000}


# ---------------------------------------------------------------- encserver

# Symbols: OK/✔ done, ->/→ received or waiting, !!/✖ failed. Bars: # or █.
_JOB = re.compile(r"^\s*#(\d+)\s+(OK|->|!!|✔|→|✖)\s*pid\s+(-?\d+)\s+([\d.]+ (?:B|KB|MB|GB))\s\s(.*)$")
_DONE = re.compile(r"^\s*(\d+)\s+([\d.]+) ms\s*(.*)$")
_WORKER = re.compile(r"w(\d+) (?:[#█]+ )?(\d+)")


def server_banner(lines):
    """{'workers': 4, 'backend': 'builtin', 'ready': True} from encserver's start-up text."""
    res = {"ready": any(l.strip() == "encserver ready" for l in lines)}
    for line in lines:
        m = re.match(r"^\s+workers\s+(\d+)\s*$", line)
        if m:
            res["workers"] = int(m.group(1))
        m = re.match(r"^\s+cipher\s+AES-256-CTR, (\S+) backend", line)
        if m:
            res["backend"] = m.group(1)
        m = re.match(r"^\s+limits\s+(.+) per Data Block, (\d+) Jobs at once", line)
        if m:
            res["max_block"] = m.group(1)
            res["max_jobs"] = int(m.group(2))
    return res


def server_line(line):
    """One encserver -v log line -> dict, or None if it is not a Job line."""
    m = _JOB.match(line)
    if not m:
        return None
    job_id, symbol, pid, size, rest = m.groups()
    ev = {"id": int(job_id), "pid": int(pid), "size": size, "bytes": size_to_bytes(size)}
    if symbol in ("->", "→"):
        ev["kind"] = "received" if rest.startswith("received") else "waiting"
        p = re.search(r"(\d+) Jobs? in progress", rest)
        ev["in_progress"] = int(p.group(1)) if p else None
    elif symbol in ("OK", "✔"):
        d = _DONE.match(rest)
        if not d:
            return None
        ev["kind"] = "done"
        ev["segments"] = int(d.group(1))
        ev["ms"] = float(d.group(2))
        workers = {int(w): int(c) for w, c in _WORKER.findall(d.group(3))}
        ev["per_worker"] = [workers.get(i, 0) for i in range(max(workers) + 1)] if workers else []
    else:
        ev["kind"] = "failed"
        ev["what"] = rest.strip()
    return ev


# ---------------------------------------------------------------- bench.sh

def bench_csv(text):
    """results.csv -> per-run rows, ignoring the header and broken lines."""
    out = []
    for line in text.splitlines():
        parts = line.strip().split(",")
        if len(parts) != 7 or not parts[0].isdigit():
            continue
        try:
            out.append({"workers": int(parts[0]), "bytes": int(parts[1]), "segments": int(parts[2]),
                        "run": int(parts[3]), "enc_us": float(parts[4]), "rtt_us": float(parts[5]),
                        "verified": parts[6]})
        except ValueError:
            continue
    return out


def bench_stats(runs):
    """Per worker count: mean/median/min/max/stdev of the Encryption and Round-Trip Times."""
    by_workers = {}
    for r in runs:
        by_workers.setdefault(r["workers"], []).append(r)
    rows_out, base = [], None
    for w in sorted(by_workers):
        group = by_workers[w]
        enc = [r["enc_us"] / 1000 for r in group]
        rtt = [r["rtt_us"] / 1000 for r in group]
        mean = statistics.fmean(enc)
        base = base if base is not None else mean
        nbytes = group[0]["bytes"]
        speedup = base / mean if mean > 0 else None
        rows_out.append({
            "workers": w, "runs": len(group), "segments": group[0]["segments"], "bytes": nbytes,
            "enc_mean": mean, "enc_median": statistics.median(enc), "enc_min": min(enc),
            "enc_max": max(enc), "enc_stdev": statistics.stdev(enc) if len(enc) > 1 else 0.0,
            "rtt_mean": statistics.fmean(rtt), "rtt_min": min(rtt), "rtt_max": max(rtt),
            "mb_per_s": (nbytes / 1048576) / (mean / 1000) if mean > 0 else None,
            "speedup": speedup,
            "efficiency": speedup / w if speedup else None,
            "verified_fail": sum(1 for r in group if r["verified"] == "FAIL"),
        })
    return rows_out
