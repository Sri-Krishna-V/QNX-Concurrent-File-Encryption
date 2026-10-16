# lib.sh - shared by demo.sh and bench.sh. Plain POSIX sh, so it runs on the
# QNX shell. Source it with:  . "$HERE/lib.sh"

# ---------------------------------------------------------------- colours
if [ -t 1 ] && [ -z "$NO_COLOR" ]; then
    ESC=$(printf '\033')
    C_RESET="$ESC[0m"; C_BOLD="$ESC[1m"; C_DIM="$ESC[2m"
    C_RED="$ESC[1;31m"; C_GREEN="$ESC[1;32m"; C_CYAN="$ESC[36m"; C_YELLOW="$ESC[33m"
    if [ "$ENC_ASCII" = "1" ]; then OK="OK"; BAD="!!"; VBAR="|"; else OK="✔"; BAD="✖"; VBAR="│"; fi
else
    C_RESET=""; C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_CYAN=""; C_YELLOW=""
    OK="OK"; BAD="!!"; VBAR="|"
fi

say()   { echo "$*"; }
info()  { echo "  ${C_DIM}$*${C_RESET}"; }
good()  { echo "  ${C_GREEN}${OK} $*${C_RESET}"; }
fail()  { echo "  ${C_RED}${BAD} $*${C_RESET}"; }
die()   { fail "$*"; exit 1; }

# Step heading:  == Step 1  A text file
heading() {
    echo
    echo "${C_BOLD}${C_CYAN}== $1 ${C_RESET}${C_BOLD}$2${C_RESET}"
}

# Show a command the way a presenter would type it, then run it.
run() {
    echo "${C_YELLOW}\$ $*${C_RESET}"
    "$@"
}

# ---------------------------------------------------------------- locations
# HERE: folder of the calling script. On the Pi everything is in one folder
# (/var/encsvc). In the repo the programs are in ../bin/aarch64le.
find_programs() {
    if [ -x "$HERE/encclient" ]; then
        BIN="$HERE"
    elif [ -x "$HERE/../bin/aarch64le/encclient" ]; then
        BIN=$(cd "$HERE/../bin/aarch64le" && pwd)
    else
        die "encclient not found next to $0. Copy encserver and encclient here (make deploy)."
    fi
    if [ -d "$HERE/examples" ]; then
        EXAMPLES="$HERE/examples"
    elif [ -d "$HERE/../examples" ]; then
        EXAMPLES=$(cd "$HERE/../examples" && pwd)
    else
        EXAMPLES=""
    fi
}

# ---------------------------------------------------------------- server
# encserver registers this name while it runs.
SERVER_NAME=${ENCSVC_PATH:-/dev/name/local/encsvc}

server_running() { [ -e "$SERVER_NAME" ]; }

# start_server WORKERS KEY: start a quiet encserver in the background.
SERVER_PID=""
start_server() {
    "$BIN/encserver" -w "$1" -k "$2" > /dev/null 2>&1 &
    SERVER_PID=$!
    tries=0
    while ! server_running; do
        tries=$((tries + 1))
        if [ $tries -gt 10 ] || ! kill -0 "$SERVER_PID" 2>/dev/null; then
            kill "$SERVER_PID" 2>/dev/null    # never leave a half-started server behind
            SERVER_PID=""
            die "encserver -w $1 did not start (run it by hand to see why)"
        fi
        sleep 1
    done
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
        tries=0
        while server_running && [ $tries -lt 5 ]; do
            tries=$((tries + 1))
            sleep 1
        done
    fi
}

# ---------------------------------------------------------------- numbers
# us_to_ms 50064 -> "50.064 ms"
us_to_ms() {
    printf '%d.%03d ms' $(($1 / 1000)) $(($1 % 1000))
}

# speedup BASE_US US -> "3.41x"
speedup() {
    s=$(($1 * 100 / $2))
    printf '%d.%02dx' $((s / 100)) $((s % 100))
}

# same_bytes A B: true if both files hold exactly the same bytes.
same_bytes() {
    if command -v cmp > /dev/null 2>&1; then
        cmp -s "$1" "$2"
    else
        [ "$(cksum < "$1")" = "$(cksum < "$2")" ]
    fi
}

# ---------------------------------------------------------------- scaling
# scaling_table KEY BYTES RUNS WARMUPS CSV WORKER_COUNTS...
# Starts a quiet server for each worker count, measures, and prints a table.
# CSV may be "" (no CSV file). The live server must already be stopped.
scaling_table() {
    key=$1; bytes=$2; runs=$3; warm=$4; csv=$5
    shift 5
    base=""
    echo
    printf '  %s%-8s  %-18s  %-18s  %-8s  %s%s\n' "$C_BOLD" "WORKERS" "ENCRYPTION (mean)" \
        "ROUND TRIP (mean)" "SPEEDUP" "MB/s" "$C_RESET"
    for w in "$@"; do
        start_server "$w" "$key"
        if [ -n "$csv" ]; then
            line=$("$BIN/encclient" -k "$key" -s "$bytes" -n "$runs" -W "$warm" -c "$csv" -S)
        else
            line=$("$BIN/encclient" -k "$key" -s "$bytes" -n "$runs" -W "$warm" -S)
        fi
        status=$?
        stop_server
        [ $status -eq 0 ] || die "encclient failed with $w workers"

        # workers bytes runs enc_mean_us enc_min_us rtt_mean_us rtt_min_us
        # (the for list was expanded already, so set -- does not disturb it)
        set -- $line
        enc=$4; rtt=$6
        [ "$enc" -gt 0 ] || enc=1          # avoid dividing by zero below
        [ -n "$base" ] || base=$enc
        mbs=$((bytes * 1000000 / 1048576 * 10 / enc))
        printf '  %-8s  %-18s  %-18s  %s%-8s%s  %d.%d\n' "$w" "$(us_to_ms "$enc")" \
            "$(us_to_ms "$rtt")" "$C_GREEN" "$(speedup "$base" "$enc")" "$C_RESET" \
            $((mbs / 10)) $((mbs % 10))
    done
}
