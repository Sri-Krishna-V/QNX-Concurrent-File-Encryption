#!/bin/sh
# bench.sh - Worker Pool benchmark on the Pi: a CSV file plus a summary table.
#
#   ./bench.sh [bytes] [runs] [keyfile]
#
# Defaults: 2097152 bytes (2 MB), 200 runs, ./enc.key
# For each Worker Pool size (1, 2, 4, 8) it starts its own encserver, sends 5
# warm-up requests and then [runs] measured ones, and stops the server again.
# Stop any encserver you started yourself first.
# Output: results.csv (one row per measured run) and a table with the speedup.

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

SIZE=${1:-2097152}
RUNS=${2:-200}
KEY=${3:-./enc.key}
OUT=results.csv

find_programs
trap 'stop_server; echo; exit 130' INT TERM

if [ ! -f "$KEY" ]; then
    fail "key file $KEY not found. Create one with:"
    info "dd if=/dev/urandom of=$KEY bs=32 count=1"
    exit 1
fi
if server_running; then
    die "an encserver is already running; stop it first (Ctrl-C in its window, or: slay encserver)"
fi

echo "workers,bytes,segments,run,encryption_us,round_trip_us,verified" > "$OUT"

heading "Benchmark" "$RUNS runs of $SIZE bytes for 1, 2, 4 and 8 workers"
info "each row takes a while: $RUNS runs plus 5 warm-up runs on a fresh server"
scaling_table "$KEY" "$SIZE" "$RUNS" 5 "$OUT" 1 2 4 8

echo
good "done: one row per run in $OUT"
info "SPEEDUP = mean encryption time with 1 worker / mean with N workers"
