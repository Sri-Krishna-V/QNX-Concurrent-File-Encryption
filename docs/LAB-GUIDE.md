# Lab Guide: Build and Run on the Raspberry Pi 4B

This guide takes you from a lab PC with QNX SDP 8.0.3 and Momentics to a benchmark CSV file produced on the Raspberry Pi 4B. Follow the parts in order. A part that starts with **Skip if…** can be left out when your setup is already there.

Commands in `this style` run on the **lab PC** unless the step says **on the Pi**.

---

## Part 0: What you need

- A lab PC with QNX SDP 8.0.3 and the Momentics IDE.
- A Raspberry Pi 4B and its USB-C power supply (5 V, 3 A).
- A microSD card of **32 GB or more** from a known brand, plus a card reader (only for Part 2).
- An Ethernet cable connecting the Pi to the same network as the lab PC. An HDMI monitor and USB keyboard for the Pi also help (Part 3).
- This project folder.

---

## Part 1: Check the lab PC

1. **Copy the project to a path without spaces**, for example `C:\qnxproj\encsvc`. Some QNX and Eclipse tools fail on paths with spaces.
2. **Open a QNX command prompt.** Open `cmd` and run:
   ```
   %USERPROFILE%\qnx800\qnxsdp-env.bat
   ```
   If the SDP is installed somewhere else, run `qnxsdp-env.bat` from that folder. On a Linux lab PC, run `source ~/qnx800/qnxsdp-env.sh` instead.
3. **Check the compiler:** `qcc -V` should list `gcc_ntoaarch64le` among the targets.
4. **Check that OpenSSL is installed for QNX:**
   ```
   dir %QNX_TARGET%\usr\include\openssl\evp.h
   ```
   - If the file exists, carry on.
   - If it does not exist, install the package `com.qnx.qnx800.target.security.crypto.openssl3` in QNX Software Center. If you can't install it, use the bundled AES instead: build with `make CRYPTO=builtin` in Part 4.

---

## Part 2: Prepare the SD card

**Skip if the Pi already boots QNX 8.0.** You can tell from the QNX login prompt on HDMI, or because Part 3 works.

1. In **QNX Software Center**, install the package `com.qnx.qnx800.quickstart.rpi4`. This is the QNX 8.0 Quick Start Target Image for the Raspberry Pi 4.
2. Find the image file: look in `%USERPROFILE%\qnx800\images\` for a file named like `qnx_sdp8.0_rpi4_quickstart_*.img`.
3. Install **Raspberry Pi Imager** if the PC doesn't have it, and open it.
   - **Choose OS** → **Use custom** → select the `.img` file.
   - **Choose Storage** → select the SD card. Check carefully that it is the card and not a PC drive.
   - **Write**, and wait for the write and the verify step to finish.
4. *Optional, Wi-Fi instead of Ethernet:* on the card's boot partition, edit `wpa_supplicant.conf` and add your network:
   ```
   network={
       ssid="LabWiFi"
       psk="password"
   }
   ```
   Ethernet with DHCP works with no configuration, so use it if you can.

---

## Part 3: Boot the Pi and log in

1. Put the SD card in the Pi, connect Ethernet (and HDMI and the keyboard if you have them), then connect power.
2. Wait about one minute for the first boot.
3. From the lab PC, log in over SSH:
   ```
   ssh -o MACs=hmac-sha2-256 qnxuser@qnxpi.local
   ```
   - The password is `qnxuser`.
   - Windows OpenSSH needs the `-o MACs=hmac-sha2-256` option. On Linux you can leave it out.
   - **If `qnxpi.local` is not found:** log in on the Pi's own screen (user `qnxuser`, password `qnxuser`), run `ifconfig`, note the IPv4 address, and use it instead of `qnxpi.local` everywhere in this guide.
4. The accounts are:

   | User | Password | Note |
   |---|---|---|
   | `qnxuser` | `qnxuser` | Use this account to log in over SSH. |
   | `root` | `root` | SSH login as root is disabled. Log in as `qnxuser`, then run `su`. |

---

## Part 4: Build on the lab PC

### Option A: command line (recommended)

In the QNX command prompt from Part 1:

```
cd C:\qnxproj\encsvc
make
```

This creates `bin\aarch64le\encserver` and `bin\aarch64le\encclient`.

- If OpenSSL is missing (see Part 1), build with `make CRYPTO=builtin` instead.
- After changing the build option, run `make clean` before `make` again.

### Option B: Momentics

1. **File → New → Project → C/C++ → Makefile Project with Existing Code.**
2. **Existing Code Location:** `C:\qnxproj\encsvc`. **Toolchain:** pick the QNX toolchain (QCC). Then **Finish**.
3. Right-click the project → **Build Project**. The console shows the same `qcc` commands as Option A.
4. For the builtin AES: **Project → Properties → C/C++ Build → Behavior** → change the build target from `all` to `all CRYPTO=builtin`.

Menu names can differ slightly between Momentics versions. If you can't find them, use Option A, which needs nothing from the IDE.

---

## Part 5: Copy the programs to the Pi

From the project folder on the lab PC:

```
ssh -o MACs=hmac-sha2-256 qnxuser@qnxpi.local "mkdir -p encsvc"
scp -o MACs=hmac-sha2-256 bin/aarch64le/encserver bin/aarch64le/encclient scripts/bench.sh qnxuser@qnxpi.local:encsvc/
```

The files land in `/data/home/qnxuser/encsvc` on the Pi.

*Alternative:* in Momentics, add the Pi as a target (launch bar → **Launch Target** drop-down → **New Launch Target** → **QNX Target** → host `qnxpi.local`, port `8000`). Then drag the files into **Target File System Navigator**.

---

## Part 6: First run (on the Pi)

1. Log in (Part 3), then:
   ```
   cd encsvc
   chmod +x encserver encclient bench.sh
   ```
2. **Create the key file.** The client and the server must use the same one:
   ```
   dd if=/dev/urandom of=enc.key bs=32 count=1
   ```
   If `dd` is not available, use `head -c 32 /dev/urandom > enc.key`.
3. **Start the server** in the background, with one log line per Job:
   ```
   ./encserver -v &
   ```
   Expected output:
   ```
   encserver: ready as "encsvc", 4 workers, cipher openssl, max block 2097152 bytes, max 8 jobs
   ```
   If you see `name_attach: Operation not permitted`, run `su` (password `root`) and repeat steps 1–3 as root, in the same folder.
4. **Encrypt a 2 MB random Data Block:**
   ```
   ./encclient -s 2097152
   ```
   Expected output (your times will differ):
   ```
   encclient: sent 2097152 bytes, waiting for the reply...
   encserver: job 2097152 bytes, 16 segments, 9.8 ms, ok
   encclient: main thread stayed free for 2 ticks of 5 ms while waiting
   encclient: run 1: 4 workers, 16 segments, encryption 9.812 ms, round trip 14.203 ms, verified
   ```
   The word **verified** means the result equals the Reference Encryption and decrypts back to the original data.
5. **Encrypt a real file.** The output file holds the 16-byte IV followed by the Encrypted Block:
   ```
   ./encclient -f /etc/passwd -o passwd.enc
   ```
6. **Show several Jobs at once.** Start three clients together and watch the server log interleave:
   ```
   ./encclient -s 2097152 -n 20 & ./encclient -s 2097152 -n 20 & ./encclient -s 100000 -n 20 & wait
   ```
7. **Stop the server:** `kill %1` (or `slay encserver`).

---

## Part 7: Benchmark (on the Pi)

1. Make sure no `encserver` is running: run `slay encserver`. An error saying none was found is fine.
2. Run the benchmark:
   ```
   ./bench.sh
   ```
   - This runs 1, 2, 4 and 8 workers, with 200 runs of a 2 MB Data Block each, and writes `results.csv`.
   - After each worker count, the client prints a summary line such as `encryption mean … ms`.
   - Other sizes and run counts: `./bench.sh <bytes> <runs>`, for example `./bench.sh 1048576 100`.
3. Copy the results to the lab PC. Run this **on the PC**:
   ```
   scp -o MACs=hmac-sha2-256 qnxuser@qnxpi.local:encsvc/results.csv .
   ```
4. Make the chart in Excel:
   - **Insert → PivotTable**. Put `workers` in Rows and **Average** of `encryption_us` and `round_trip_us` in Values.
   - **Speedup for N workers** = average with 1 worker ÷ average with N workers. Expect it to rise up to 4 workers (the Pi has 4 cores) and level off at 8.
   - `round_trip_us` is always larger than `encryption_us`. The difference is the cost of copying the Data Block and the Encrypted Block through the kernel.

Tips for steady numbers:
- Run the benchmark twice, and use the second run.
- Keep the Pi cool, because a hot Pi 4 slows its CPU down.
- Don't run anything else on the Pi during the benchmark.

### Later: larger Data Blocks

1. Change `ENC_MAX_BLOCK` in `src/core/config.h`, for example to `(16u * 1024u * 1024u)`.
2. Rebuild (Part 4) and copy both programs again (Part 5).
3. Run `./bench.sh 16777216 100`.

---

## Part 8: Troubleshooting

| Symptom | Fix |
|---|---|
| `qcc: command not found` or `make: command not found` | Run `qnxsdp-env.bat` (Part 1) in the same window first. |
| `openssl/evp.h: No such file or directory` | Install the OpenSSL package (Part 1), or build with `make clean` and then `make CRYPTO=builtin`. |
| On the Pi: `Could not load library libcrypto.so.3` | Rebuild with `make CRYPTO=builtin` and copy the programs again. This version needs no extra libraries. |
| `encserver: name_attach: Operation not permitted` | Run the server as root: `su`, password `root`. |
| `encclient: name_open("encsvc") … is encserver running?` | Start `./encserver &` first, and wait for its `ready` line. |
| `ssh`: `no matching MAC found` | Add `-o MACs=hmac-sha2-256` (Windows OpenSSH). |
| `qnxpi.local` not found | Use the IP address shown by `ifconfig` on the Pi (Part 3). |
| `./bench.sh: Permission denied` | `chmod +x bench.sh encserver encclient` |
| `bad interpreter` or `$'\r': command not found` | `bench.sh` was saved with Windows line endings. On the Pi run `tr -d '\r' < bench.sh > b.sh && mv b.sh bench.sh && chmod +x bench.sh`. |
| `cipher self-test FAILED` | Stop here. The cipher library is broken on this target. Try `make CRYPTO=builtin`, and report which backend failed. |
| `result differs from Reference Encryption` | This is a real bug. Keep the full output and the command line you used. |
