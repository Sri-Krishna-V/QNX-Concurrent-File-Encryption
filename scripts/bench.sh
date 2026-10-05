#!/bin/sh
# bench.sh - run the Worker Pool benchmark on the target and write a CSV file.
#
#   ./bench.sh [bytes] [runs] [keyfile]
#
# Defaults: 2097152 bytes (2 MB), 200 runs, ./enc.key
# For each Worker Pool size it starts encserver, sends 5 warm-up requests and
# then [runs] measured ones, and stops the server again.
# Output: results.csv  (one row per measured run)

SIZE=${1:-2097152}
RUNS=${2:-200}
KEY=${3:-./enc.key}
OUT=results.csv
WORKER_COUNTS="1 2 4 8"

if [ ! -f "$KEY" ]; then
    echo "bench.sh: key file $KEY not found. Create one with:"
    echo "    dd if=/dev/urandom of=$KEY bs=32 count=1"
    exit 1
fi

echo "workers,bytes,segments,run,encryption_us,round_trip_us,verified" > "$OUT"

for W in $WORKER_COUNTS; do
    ./encserver -w "$W" -k "$KEY" &
    SERVER=$!
    sleep 1

    echo "bench.sh: $W workers, $RUNS runs of $SIZE bytes..."
    if ! ./encclient -k "$KEY" -s "$SIZE" -n "$RUNS" -W 5 -c >> "$OUT"; then
        echo "bench.sh: client failed with $W workers"
        kill "$SERVER"
        exit 1
    fi

    kill "$SERVER"
    wait "$SERVER" 2>/dev/null
done

echo "bench.sh: done, results in $OUT"
