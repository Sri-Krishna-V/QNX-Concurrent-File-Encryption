# Concurrent Encryption Service (QNX 8.0, Raspberry Pi 4B)

A client sends a Data Block of up to 2 MB to a QNX server. The server splits the block into Segments, encrypts them in parallel on a Worker Pool with AES-256-CTR, and returns the Encrypted Block asynchronously. Any file of any size can be encrypted: the client sends it as several Data Blocks at once and writes an Encrypted File, which it can also decrypt again.

The code shows:
- mutex-protected shared output,
- condition-variable completion signalling,
- QNX message passing with a deferred reply.

## Documents

| File | What it is |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | Architecture, decisions, pseudocode, communication flow |
| [docs/LAB-GUIDE.md](docs/LAB-GUIDE.md) | Step by step: SD card, build, copy to the Pi, run, benchmark |
| [docs/adr/0001-deferred-reply.md](docs/adr/0001-deferred-reply.md) | Why the server replies late instead of using pulse notification |
| [docs/adr/0002-encrypted-file-without-integrity-check.md](docs/adr/0002-encrypted-file-without-integrity-check.md) | Why Encrypted Files have no MAC and are decrypted on the client |
| [CONTEXT.md](CONTEXT.md) | Glossary: Data Block, Segment, Job, Task, Worker Pool… |

## Layout

```
src/core/      portable: Worker Pool, Job, Segment planning, AES-256-CTR (POSIX only)
src/qnx/       QNX message passing: server.c (encserver), client.c + client_file.c (encclient)
tests/         host tests, plus a message-passing stand-in for end-to-end tests
scripts/       demo.sh (guided demo), bench.sh (benchmark), lib.sh; all run on the Pi
examples/      real sample files for the demo: text, CSV, JSON, PDF, PNG, WAV
bin/aarch64le/ build output for the Pi
```

## Build

```sh
# For the Pi (QNX SDP 8.0 environment loaded with qnxsdp-env.bat / qnxsdp-env.sh)
make                    # AES from OpenSSL libcrypto
make CRYPTO=builtin     # bundled AES instead, if OpenSSL is not available

# On Linux / WSL (gcc + libssl-dev)
make test               # all tests, with AddressSanitizer + UBSan
make tsan               # all tests, with ThreadSanitizer
make qnx-syntax         # compile-check the QNX sources against stub headers

# Copy everything to the Pi (default qnxuser@10.0.0.1:/var/encsvc)
make deploy             # or: make deploy PI=qnxuser@<ip> PI_DIR=<folder>
```

## Run (on the Pi)

```sh
cd /var/encsvc
dd if=/dev/urandom of=enc.key bs=32 count=1      # shared key, once
./encserver -v &                                 # -w N workers (default: CPU count), -v live log
./encclient -e examples/report.pdf               # -> report.pdf.enc  (readable header + encrypted bytes)
./encclient -d report.pdf.enc -o report2.pdf     # decrypt on the client; never overwrites a file
./encclient -s 2097152 -n 20                     # benchmark: 20 random 2 MB Data Blocks
./bench.sh                                       # 1/2/4/8 workers x 200 runs -> results.csv + speedup table
```

## Demo

Open two SSH windows on the Pi, both in `/var/encsvc`:

| Left window | Right window |
|---|---|
| `./encserver -v`: live log, one line per Job, with the Segments each worker encrypted | `./demo.sh`: steps 0 to 5, press Enter between steps |

The steps: check the set-up, a text file step by step, every example file in one table, a 12 MB file with 4 Data Blocks in flight, three clients at once, and Worker Pool scaling with speedup. `./demo.sh 3` runs one step; `./demo.sh -y` runs all without pausing. See [docs/LAB-GUIDE.md](docs/LAB-GUIDE.md), Part 7.

## Limits

These are set in `src/core/config.h`:

| Constant | Value | Meaning |
|---|---|---|
| `ENC_MAX_BLOCK` | 2 MB | Largest Data Block |
| `ENC_SINGLE_SEGMENT_MAX` | 64 KB | At or below this, a Data Block is one Segment |
| `ENC_MIN_SEGMENT` | 16 KB | Smallest Segment when splitting |
| `ENC_TASKS_PER_WORKER` | 4 | Segments per worker, for load balancing |
| `ENC_MAX_JOBS` | 8 | Jobs in progress at once; further clients wait |
| `ENC_MAX_WORKERS` | 64 | Largest Worker Pool (`-w`) |
