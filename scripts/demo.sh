#!/bin/sh
# demo.sh - guided demo of the Concurrent Encryption Service.
#
# Use two SSH windows on the Pi, both in /var/encsvc:
#   left:   ./encserver -v        live server log, one line per Job
#   right:  ./demo.sh             this script
#
#   ./demo.sh        all steps, press Enter between steps
#   ./demo.sh 3      only step 3
#   ./demo.sh -y     all steps without pausing
#
# Steps:
#   0  check the set-up            3  a 12 MB file, 4 Data Blocks in flight
#   1  a text file, step by step   4  three clients at the same time
#   2  every example file          5  Worker Pool scaling: 1, 2, 4, 8 workers
#
# Output files go to demo-out/ next to this script.

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

KEY=${KEY:-$HERE/enc.key}
OUT=$HERE/demo-out
PAUSE=1
ONLY=""
FAILURES=0

for arg in "$@"; do
    case "$arg" in
    -y) PAUSE=0 ;;
    [0-5]) ONLY=$arg ;;
    *) echo "usage: $0 [-y] [step 0-5]"; exit 2 ;;
    esac
done

find_programs
CLIENT_PIDS=""                      # step 4 background clients, stopped on Ctrl-C
trap 'kill $CLIENT_PIDS 2> /dev/null; stop_server; echo; exit 130' INT TERM

# encclient output saved to log files (step 4) keeps the same symbols as the screen.
if [ -t 1 ] && [ "$ENC_ASCII" != "1" ]; then
    ENC_UNICODE=1
    export ENC_UNICODE
fi

# Paths as the presenter sees them: examples relative to demo-out/.
if [ "$EXAMPLES" = "$HERE/examples" ]; then EX=../examples; else EX=$EXAMPLES; fi

pause() {
    if [ "$PAUSE" = 1 ] && [ -z "$ONLY" ]; then
        printf '\n  %sPress Enter for the next step...%s' "$C_DIM" "$C_RESET"
        read reply
    fi
}

# Run encclient with the demo key, showing the command without the key option.
client() {
    echo "${C_YELLOW}\$ encclient $*${C_RESET}"
    "$BIN/encclient" -k "$KEY" "$@"
}

# Print the readable header of an Encrypted File (up to the --- line).
show_header() {
    while IFS= read -r line; do
        echo "  ${C_DIM}${VBAR}${C_RESET} $line"
        [ "$line" = "---" ] && break
    done < "$1"
}

# compare ORIGINAL RESTORED: report whether the round trip gave the same bytes.
compare() {
    set -- $(wc -c < "$1")
    bytes=$1
    if same_bytes "$1" "$2"; then
        good "identical: $2 has exactly the same $bytes bytes as the original $1"
    else
        fail "DIFFERENT: $2 does not match the original $1"
        FAILURES=$((FAILURES + 1))
    fi
    if command -v cksum > /dev/null 2>&1; then
        info "cksum original:  $(cksum < "$1")"
        info "cksum restored:  $(cksum < "$2")"
    fi
}

wait_for_server() {
    if ! server_running; then
        fail "encserver is not running"
        info "In the other window run:   cd $HERE && ./encserver -v"
        printf '  waiting for encserver'
        while ! server_running; do
            printf '.'
            sleep 1
        done
        echo
    fi
    good "encserver is running ($SERVER_NAME)"
}

# ---------------------------------------------------------------- steps

step0() {
    heading "Step 0" "Check the set-up"
    if [ -f "$KEY" ]; then
        good "key file $KEY"
    else
        dd if=/dev/urandom of="$KEY" bs=32 count=1 > /dev/null 2>&1 || die "cannot create $KEY"
        good "created a new key file $KEY (32 random bytes)"
        info "encserver must load this key: start (or restart) it after this point."
    fi
    [ -n "$EXAMPLES" ] || die "examples/ folder not found next to $0 (make deploy copies it)"
    good "example files in $EXAMPLES"
    mkdir -p "$OUT" || die "cannot create $OUT"
    good "output folder $OUT"
    case "$ONLY" in
    "" | 1 | 2 | 3 | 4) wait_for_server ;;     # step 5 runs its own servers
    esac
}

step1() {
    heading "Step 1" "Encrypt and decrypt a text file"
    info "hello.txt is small, so it travels as one Data Block. Watch the left window:"
    info "the server receives it, splits it into Segments and the workers encrypt them."
    echo
    rm -f hello.txt.enc hello.txt
    client -e "$EX/hello.txt" -x

    echo
    info "The Encrypted File starts with a readable header, then the encrypted bytes:"
    show_header hello.txt.enc

    echo
    info "Turn it back into the original file. Decryption runs on the client, with the same key:"
    client -d hello.txt.enc -x
    echo
    compare "$EX/hello.txt" hello.txt

    echo
    info "The decrypted text:"
    while IFS= read -r line; do
        echo "  ${C_DIM}${VBAR}${C_RESET} $line"
    done < hello.txt
}

# make_big: a 12 MB file of random bytes (6 Data Blocks), made again if a
# previous run left a wrong-sized one (for example after Ctrl-C during dd).
BIG_BYTES=12582912
make_big() {
    set -- $(wc -c < big.bin 2>/dev/null || echo 0)
    if [ -f big.bin ] && [ "$1" = "$BIG_BYTES" ]; then
        return 0
    fi
    rm -f big.bin
    run dd if=/dev/urandom of=big.bin bs=65536 count=192 2> /dev/null
    set -- $(wc -c < big.bin); [ "$1" = "$BIG_BYTES" ] || die "could not make a 12 MB big.bin"
}

# file_type NAME -> short description from the extension
file_type() {
    case "$1" in
    *.txt) echo "text" ;;
    *.csv) echo "CSV data" ;;
    *.json) echo "JSON" ;;
    *.pdf) echo "PDF document" ;;
    *.png) echo "PNG image" ;;
    *.wav) echo "WAV audio" ;;
    *.jpg | *.jpeg) echo "JPEG image" ;;
    *) echo "file" ;;
    esac
}

step2() {
    heading "Step 2" "Every example file: encrypt, decrypt, compare"
    info "Any file is just bytes: text, data, documents, images and audio all work the same way."
    echo
    printf '  %s%-16s %-13s %9s  %6s  %12s  %12s  %s%s\n' "$C_BOLD" "FILE" "TYPE" "SIZE" \
        "BLOCKS" "ENCRYPT" "DECRYPT" "SAME?" "$C_RESET"
    for f in "$EXAMPLES"/*; do
        name=$(basename "$f")
        case "$name" in README*) continue ;; esac
        [ -f "$f" ] || continue
        rm -f "$name.enc" "$name"

        if ! eout=$("$BIN/encclient" -k "$KEY" -e "$f" 2>&1); then
            printf '  %-16s %s%s encrypt failed%s\n' "$name" "$C_RED" "$BAD" "$C_RESET"
            echo "$eout" | sed 's/^/      /'
            FAILURES=$((FAILURES + 1))
            continue
        fi
        if ! dout=$("$BIN/encclient" -k "$KEY" -d "$name.enc" 2>&1); then
            printf '  %-16s %s%s decrypt failed%s\n' "$name" "$C_RED" "$BAD" "$C_RESET"
            echo "$dout" | sed 's/^/      /'
            FAILURES=$((FAILURES + 1))
            continue
        fi
        size=$(echo "$eout" | sed -n 's/^  input .*  \([0-9.]* [KMG]\{0,1\}B\) (.*/\1/p')
        blocks=$(echo "$eout" | sed -n 's/^  Data Blocks *\([0-9]*\) of.*/\1/p')
        etime=$(echo "$eout" | sed -n 's/^  wall time *\([0-9.]* ms\).*/\1/p')
        dtime=$(echo "$dout" | sed -n 's/^  wall time *\([0-9.]* ms\).*/\1/p')
        if same_bytes "$f" "$name"; then
            same="${C_GREEN}${OK}${C_RESET}"
        else
            same="${C_RED}${BAD} different${C_RESET}"
            FAILURES=$((FAILURES + 1))
        fi
        printf '  %-16s %-13s %9s  %6s  %12s  %12s  %s\n' "$name" "$(file_type "$name")" \
            "$size" "$blocks" "$etime" "$dtime" "$same"
    done
    echo
    info "ENCRYPT is the client's wall time including the server round trip; DECRYPT runs on the client."
    info "The restored files are in $OUT. To open the image and PDF on the PC:"
    info "  scp -o MACs=hmac-sha2-256 qnxuser@<pi>:$OUT/qnx-pi.png ."
}

step3() {
    heading "Step 3" "A 12 MB file: 6 Data Blocks, 4 in flight at once"
    info "The client splits the file into 2 MB Data Blocks and sends 4 at a time from 4 sender"
    info "threads. In the left window several Jobs are in progress together, sharing the workers."
    echo
    make_big
    rm -f big.bin.enc big.decrypted.bin
    client -e big.bin
    info "The Encryption Time is summed over the 6 Jobs. It is larger than the wall time because"
    info "the Jobs overlapped: that is the concurrency."
    echo
    client -d big.bin.enc -o big.decrypted.bin
    echo
    compare big.bin big.decrypted.bin
}

step4() {
    heading "Step 4" "Three clients at the same time"
    info "Three separate encclient processes start together. The server's Receive Thread keeps"
    info "accepting while earlier Jobs run, so their Jobs interleave in the left window."
    echo
    make_big
    rm -f client1.enc client2.enc
    echo "${C_YELLOW}\$ encclient -e big.bin -o client1.enc &${C_RESET}"
    echo "${C_YELLOW}\$ encclient -e $EX/sensor-log.csv -o client2.enc &${C_RESET}"
    echo "${C_YELLOW}\$ encclient -s 2097152 -n 5 &${C_RESET}"
    "$BIN/encclient" -k "$KEY" -e big.bin -o client1.enc > client1.log 2>&1 &
    p1=$!
    "$BIN/encclient" -k "$KEY" -e "$EXAMPLES/sensor-log.csv" -o client2.enc > client2.log 2>&1 &
    p2=$!
    "$BIN/encclient" -k "$KEY" -s 2097152 -n 5 > client3.log 2>&1 &
    p3=$!
    CLIENT_PIDS="$p1 $p2 $p3"
    info "started client 1 (pid $p1), client 2 (pid $p2), client 3 (pid $p3)"
    info "find these pids in the CLIENT column of the server log"

    n=1
    for p in $p1 $p2 $p3; do
        if wait "$p"; then status=0; else status=$?; fi
        echo
        if [ $status -eq 0 ]; then
            echo "  ${C_BOLD}client $n${C_RESET} (pid $p)  ${C_GREEN}${OK} finished${C_RESET}"
        else
            echo "  ${C_BOLD}client $n${C_RESET} (pid $p)  ${C_RED}${BAD} failed${C_RESET}"
            FAILURES=$((FAILURES + 1))
        fi
        grep -v "^$" "client$n.log" | sed "s/^/  $VBAR /"
        n=$((n + 1))
    done
    CLIENT_PIDS=""
}

step5() {
    heading "Step 5" "Worker Pool scaling: 1, 2, 4 and 8 workers"
    info "For each row the script starts its own encserver with that many workers and sends"
    info "20 Data Blocks of 2 MB (after 3 warm-up runs), then stops it again."
    if server_running; then
        echo
        echo "  ${C_YELLOW}Stop the server in the left window now (press Ctrl-C there).${C_RESET}"
        printf '  waiting'
        while server_running; do
            printf '.'
            sleep 1
        done
        echo
        good "server stopped"
    fi
    scaling_table "$KEY" 2097152 20 3 "" 1 2 4 8
    echo
    info "The Pi 4B has 4 cores: the speedup grows up to 4 workers and levels off at 8,"
    info "because 8 workers still share 4 cores."
    info "Start the live server again in the left window with:  ./encserver -v"
}

# ---------------------------------------------------------------- main

echo
echo "${C_BOLD}Concurrent Encryption Service on QNX${C_RESET}  ${C_DIM}demo${C_RESET}"

step0
cd "$OUT" || die "cannot enter $OUT"

if [ -n "$ONLY" ]; then
    [ "$ONLY" = 0 ] || "step$ONLY"
else
    for s in 1 2 3 4 5; do
        pause
        "step$s"
    done
fi

echo
if [ $FAILURES -eq 0 ]; then
    echo "${C_GREEN}${OK} Demo finished: every check passed.${C_RESET}"
else
    echo "${C_RED}${BAD} Demo finished with $FAILURES failed check(s).${C_RESET}"
    exit 1
fi
