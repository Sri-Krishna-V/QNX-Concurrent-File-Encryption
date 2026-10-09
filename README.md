# Concurrent Encryption Service (QNX 8.0, Raspberry Pi 4B)

A client sends a Data Block of up to 2 MB to a QNX server. The server splits the block into Segments, encrypts them in parallel on a Worker Pool with AES-256-CTR, and returns the Encrypted Block asynchronously.

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
| [CONTEXT.md](CONTEXT.md) | Glossary: Data Block, Segment, Job, Task, Worker Pool… |

## Layout

```
src/core/      portable: Worker Pool, Job, Segment planning, AES-256-CTR (POSIX only)
src/qnx/       QNX message passing: server.c (encserver), client.c (encclient), protocol.h
tests/         host tests, plus a message-passing stand-in for end-to-end tests
scripts/       bench.sh, run on the Pi
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
```

## Run (on the Pi)

```sh
dd if=/dev/urandom of=enc.key bs=32 count=1      # shared key, once
./encserver -v &                                 # -w N workers (default: CPU count)
./encclient -s 2097152                           # 2 MB random Data Block, verified
./encclient -f input.bin -o input.enc            # encrypt a file: output = IV + Encrypted Block
./bench.sh                                       # 1/2/4/8 workers × 200 runs -> results.csv
```

## Limits

These are set in `src/core/config.h`:

| Constant | Value | Meaning |
|---|---|---|
| `ENC_MAX_BLOCK` | 2 MB | Largest Data Block |
| `ENC_SINGLE_SEGMENT_MAX` | 64 KB | At or below this, a Data Block is one Segment |
| `ENC_MIN_SEGMENT` | 16 KB | Smallest Segment when splitting |
| `ENC_TASKS_PER_WORKER` | 4 | Segments per worker, for load balancing |
| `ENC_MAX_JOBS` | 8 | Jobs in progress at once; further clients wait |
