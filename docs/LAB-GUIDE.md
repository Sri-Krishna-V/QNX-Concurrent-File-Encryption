# Lab Guide: Build and Run on the Raspberry Pi 4B

This guide takes you from a lab PC with QNX SDP 8.0.3 and Momentics to the live demo and a benchmark CSV file on the Raspberry Pi 4B. Follow the parts in order. A part that starts with **Skip if…** can be left out when your setup is already there.

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
   - In our lab the Pi is at **10.0.0.1**: `ssh -o MACs=hmac-sha2-256 qnxuser@10.0.0.1`. The rest of this guide uses that address.
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
- Our lab build uses `make CRYPTO=builtin` (the server banner then says `builtin backend`).
- After changing the build option, run `make clean` before `make` again.

### Option B: Momentics

1. **File → New → Project → C/C++ → Makefile Project with Existing Code.**
2. **Existing Code Location:** `C:\qnxproj\encsvc`. **Toolchain:** pick the QNX toolchain (QCC). Then **Finish**.
3. Right-click the project → **Build Project**. The console shows the same `qcc` commands as Option A.
4. For the builtin AES: **Project → Properties → C/C++ Build → Behavior** → change the build target from `all` to `all CRYPTO=builtin`.

Menu names can differ slightly between Momentics versions. If you can't find them, use Option A, which needs nothing from the IDE.

---

## Part 5: Copy everything to the Pi

From the project folder on the lab PC, after building (Part 4):

```
make deploy
```

This copies `encserver`, `encclient`, `demo.sh`, `bench.sh`, `lib.sh` and the `examples` folder to `/var/encsvc` on the Pi at `qnxuser@10.0.0.1`, and makes the programs executable. You type the `qnxuser` password up to four times.

- **Different address or folder:** `make deploy PI=qnxuser@qnxpi.local PI_DIR=/data/home/qnxuser/encsvc`
- **By hand**, if `make deploy` does not work on your PC:
  ```
  ssh -o MACs=hmac-sha2-256 qnxuser@10.0.0.1 "mkdir -p /var/encsvc"
  scp -o MACs=hmac-sha2-256 bin/aarch64le/encserver bin/aarch64le/encclient scripts/demo.sh scripts/bench.sh scripts/lib.sh qnxuser@10.0.0.1:/var/encsvc/
  scp -r -o MACs=hmac-sha2-256 examples qnxuser@10.0.0.1:/var/encsvc/
  ```

*Alternative:* in Momentics, add the Pi as a target (launch bar → **Launch Target** drop-down → **New Launch Target** → **QNX Target** → host `10.0.0.1`, port `8000`). Then drag the files into **Target File System Navigator**.

---

## Part 6: First run (on the Pi)

1. Log in (Part 3), then:
   ```
   cd /var/encsvc
   chmod +x encserver encclient demo.sh bench.sh
   ```
2. **Create the key file**, once. The client and the server must use the same one:
   ```
   dd if=/dev/urandom of=enc.key bs=32 count=1
   ```
   If `dd` is not available, use `head -c 32 /dev/urandom > enc.key`.
3. **Start the server** with its live log:
   ```
   ./encserver -v &
   ```
   It prints a short banner (name, workers, cipher, limits) and a header line for the log. If you see `name_attach: Operation not permitted`, run `su` (password `root`) and repeat steps 1–3 as root, in the same folder.
4. **Encrypt a file:**
   ```
   ./encclient -e examples/hello.txt -x
   ```
   - The output `hello.txt.enc` is an **Encrypted File**. `head -n 6 hello.txt.enc` shows its readable header: the original name, the size, and one IV per Data Block.
   - `-x` shows the first 16 bytes before and after encryption.
   - The server prints two lines for the Job: `→ received`, then `✔` with the time and how many Segments each worker encrypted.
5. **Decrypt it again**, on the client only:
   ```
   ./encclient -d hello.txt.enc -o hello-copy.txt
   cmp examples/hello.txt hello-copy.txt && echo identical
   ```
   Without `-o`, `-d` uses the original name from the header. It never overwrites an existing file.
6. **The benchmark mode** sends random Data Blocks instead of a file:
   ```
   ./encclient -s 2097152
   ```
   The word **verified** means the result equals the Reference Encryption and decrypts back to the original data.
7. **Stop the server:** `kill %1` (or `slay encserver`).

---

## Part 7: The demo (on the Pi)

**Or from the PC:** `dashboard\launch_dashboard.bat` runs the same demo as ten steps from a page at
<http://localhost:8080>. It opens one window with the encserver log and one with the client side,
with the same output as the two SSH windows below, and collects the numbers as charts.
See [dashboard/README.md](../dashboard/README.md).

Use **two SSH windows** side by side, both logged in to the Pi (Part 3):

| Left window: the server | Right window: the demo |
|---|---|
| `cd /var/encsvc` | `cd /var/encsvc` |
| `./encserver -v` | `./demo.sh` |

Keep the server in the **foreground** in the left window, so its live log stays there and never mixes into the demo output. `demo.sh` waits until the server is up, then runs these steps. Press Enter between steps.

| Step | What it shows | Watch in the left window |
|---|---|---|
| 0 | Key file, example files, output folder, server running | |
| 1 | `hello.txt`: encrypt, the readable header, the first bytes before and after, decrypt, `identical ✔` | 1 Job; small files are one Segment |
| 2 | Every file in `examples/` (text, CSV, JSON, PDF, PNG, WAV) in one table: size, Data Blocks, times, `✔` | one Job per file; bigger files use more workers |
| 3 | A 12 MB file: 6 Data Blocks, 4 in flight at once, then decrypt and compare | several `→ received` lines before the first `✔`: Jobs overlap |
| 4 | Three clients at the same time | three different pids interleaved |
| 5 | Worker Pool scaling: 1, 2, 4 and 8 workers, with the speedup | the script asks you to **stop the left server (Ctrl-C)**; it runs its own |

- `./demo.sh 3` runs only step 3. `./demo.sh -y` runs everything without pausing.
- Output files go to `/var/encsvc/demo-out`. To open the decrypted image or PDF on the PC:
  ```
  scp -o MACs=hmac-sha2-256 qnxuser@10.0.0.1:/var/encsvc/demo-out/qnx-pi.png .
  ```
- After step 5, start `./encserver -v` again in the left window if you want to repeat a step.
- If the symbols `✔ █ →` show as odd characters, run `export ENC_ASCII=1` in **both** windows first. `NO_COLOR=1` turns colours off too.

---

## Part 8: Benchmark (on the Pi)

1. Stop any `encserver` you started yourself (Ctrl-C in its window, or `slay encserver`). `bench.sh` starts its own servers.
2. Run the benchmark:
   ```
   ./bench.sh
   ```
   - This runs 1, 2, 4 and 8 workers, with 200 runs of a 2 MB Data Block each, and writes `results.csv` (one row per run).
   - At the end it prints a table: mean encryption time, mean round trip, **speedup** over 1 worker, and MB/s.
   - Other sizes and run counts: `./bench.sh <bytes> <runs>`, for example `./bench.sh 1048576 100`.
3. Copy the results to the lab PC. Run this **on the PC**:
   ```
   scp -o MACs=hmac-sha2-256 qnxuser@10.0.0.1:/var/encsvc/results.csv .
   ```
4. Make the chart in Excel:
   - **Insert → PivotTable**. Put `workers` in Rows and **Average** of `encryption_us` and `round_trip_us` in Values.
   - **Speedup for N workers** = average with 1 worker ÷ average with N workers. Expect it to rise up to 4 workers (the Pi has 4 cores) and level off at 8.
   - `round_trip_us` is always larger than `encryption_us`. The difference is the cost of copying the Data Block and the Encrypted Block through the kernel, plus the event and the second message (FETCH) that bring the Encrypted Block back.

Tips for steady numbers:
- Run the benchmark twice, and use the second run.
- Keep the Pi cool, because a hot Pi 4 slows its CPU down.
- Don't run anything else on the Pi during the benchmark.

### Later: larger Data Blocks

1. Change `ENC_MAX_BLOCK` in `src/core/config.h`, for example to `(16u * 1024u * 1024u)`.
2. Rebuild (Part 4) and copy both programs again (Part 5).
3. Run `./bench.sh 16777216 100`.

Encrypted Files record their Data Block size in the header, so files made before the change still decrypt.

---

## Part 9: Troubleshooting

| Symptom | Fix |
|---|---|
| `qcc: command not found` or `make: command not found` | Run `qnxsdp-env.bat` (Part 1) in the same window first. |
| `openssl/evp.h: No such file or directory` | Install the OpenSSL package (Part 1), or build with `make clean` and then `make CRYPTO=builtin`. |
| On the Pi: `Could not load library libcrypto.so.3` | Rebuild with `make CRYPTO=builtin` and copy the programs again. This version needs no extra libraries. |
| `encserver: name_attach: Operation not permitted` | Run the server as root: `su`, password `root`. |
| `encclient: ✖ cannot reach "encsvc"… Is encserver running?` | Start `./encserver -v` first (in the other window), and wait for its `encserver ready` banner. |
| `encserver: another encserver is already running` | One is running already: use it, or stop it with `slay encserver`. |
| `-d`: `… already exists; delete it or choose another name with -o` | Decryption never overwrites a file. Delete the old file or add `-o newname`. |
| `-d`: `… is not an Encrypted File` or `… is damaged` | The file has no `QNX-ENC v1` header, or it was cut short (for example an interrupted `scp`). Encrypt the original again. |
| `demo.sh` waits forever for encserver | Start `./encserver -v` in the other window, in the same folder as `demo.sh`. |
| Odd characters instead of `✔ █ →` | `export ENC_ASCII=1` in both windows. |
| `ssh`: `no matching MAC found` | Add `-o MACs=hmac-sha2-256` (Windows OpenSSH). |
| `qnxpi.local` not found | Use the IP address shown by `ifconfig` on the Pi (Part 3). |
| `./bench.sh: Permission denied` (or `demo.sh`) | `chmod +x encserver encclient demo.sh bench.sh` |
| `bad interpreter` or `$'\r': command not found` | A script was saved with Windows line endings. On the Pi, for each of `demo.sh`, `bench.sh` and `lib.sh`: `tr -d '\r' < demo.sh > x.sh && mv x.sh demo.sh`, then `chmod +x demo.sh bench.sh`. |
| `encclient: ✖ … no Encrypted Block within 10 s (is encserver overloaded or stopped?)` (after `request failed:` or `Data Block N failed:`) | The client waited 10 s and gave up. Look at the left window: the server may have been stopped (start it again), or it is overloaded (many clients at once, or a very slow Pi). Run the command again. |
| Server log: `client gone, Encrypted Block discarded` or `client timed out waiting for a Job slot` | Normal when a client was stopped, killed or timed out: the server freed that client's Jobs and keeps serving. |
| Server log: `cannot deliver the event, Encrypted Block discarded: Permission denied` | The kernel refused the client's pulse (QNX 8.0 accepts only events registered with `MsgRegisterEvent`). The Pi has an `encclient` from another build: rebuild and copy **both** programs again. |
| `cipher self-test FAILED` | Stop here. The cipher library is broken on this target. Try `make CRYPTO=builtin`, and report which backend failed. |
| `Encrypted Block differs from the Reference Encryption` (or `-V`: `Data Block 1 failed: differs…`) | First check that `encserver` and `encclient` use the same `enc.key`: stop the server and start it again from the folder you run `encclient` in. If it still happens, it is a real bug: keep the full output and the command line you used. |
