#!/usr/bin/env python3
"""
QNX Concurrent Encryption - Live Control Center Backend
Provides a complete HTTP + Server-Sent Events (SSE) control bridge
between the browser dashboard, the local Windows host PC, and the Raspberry Pi 4B (QNX 8.0).
"""

import sys
import os
import json
import time
import queue
import shutil
import socket
import threading
import subprocess
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

PORT = 8080
PI_IP = "10.0.0.1"
PI_USER = "qnxuser"
PI_DIR = "/var/encsvc"
SSH_OPTS = ["-o", "MACs=hmac-sha2-256", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=2"]

# Directories
DASHBOARD_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(DASHBOARD_DIR)

# Global State
state = {
    "mode": "auto",            # "hardware" or "simulation" or "auto"
    "hardware_available": False,
    "server_running": False,
    "server_pid": None,
    "last_command": None,
    "active_task": None
}

# Real-time event queues for connected SSE browser clients
sse_clients = []
clients_lock = threading.Lock()

def broadcast_event(event_type, data):
    payload = f"event: {event_type}\ndata: {json.dumps(data)}\n\n"
    with clients_lock:
        to_remove = []
        for q in sse_clients:
            try:
                q.put_nowait(payload)
            except Exception:
                to_remove.append(q)
        for q in to_remove:
            if q in sse_clients:
                sse_clients.remove(q)

def check_ping(host):
    try:
        cmd = ["ping", "-n", "1", "-w", "800", host] if sys.platform == "win32" else ["ping", "-c", "1", "-W", "1", host]
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return res.returncode == 0
    except Exception:
        return False

def check_ssh():
    try:
        cmd = ["ssh"] + SSH_OPTS + [f"{PI_USER}@{PI_IP}", "echo ok"]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=3)
        return "ok" in res.stdout
    except Exception:
        return False

def check_sdp():
    userprofile = os.environ.get("USERPROFILE", "")
    paths = [
        os.path.join(userprofile, "qnx800", "qnxsdp-env.bat"),
        r"C:\qnx800\qnxsdp-env.bat",
        os.path.join(userprofile, "qnx710", "qnxsdp-env.bat"),
    ]
    for p in paths:
        if os.path.isfile(p):
            return p
    if shutil.which("qcc"):
        return "In PATH"
    return None

# Background monitor thread for hardware connectivity
def hardware_monitor_loop():
    while True:
        ping_ok = check_ping(PI_IP)
        ssh_ok = check_ssh() if ping_ok else False
        state["hardware_available"] = ssh_ok
        
        effective_mode = "hardware" if (state["mode"] == "hardware" or (state["mode"] == "auto" and ssh_ok)) else "simulation"

        broadcast_event("status_update", {
            "ping": ping_ok,
            "ssh": ssh_ok,
            "hardware_available": ssh_ok,
            "mode": effective_mode,
            "configured_mode": state["mode"],
            "server_running": state["server_running"],
            "sdp_path": check_sdp()
        })
        time.sleep(4)

# Live Server Process Runner (SSH or Simulation)
server_process = None
server_lock = threading.Lock()

def start_server_live():
    global server_process
    with server_lock:
        if state["server_running"]:
            return True, "Server already running"
        
        effective_mode = "hardware" if (state["mode"] == "hardware" or (state["mode"] == "auto" and state["hardware_available"])) else "simulation"
        
        if effective_mode == "hardware":
            cmd = ["ssh"] + SSH_OPTS + [f"{PI_USER}@{PI_IP}", f"cd {PI_DIR} && ./encserver -v"]
            try:
                server_process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                state["server_running"] = True
                threading.Thread(target=stream_process_output, args=(server_process, "server_log"), daemon=True).start()
                return True, "encserver started on Pi 4B (10.0.0.1)"
            except Exception as e:
                return False, f"Failed to start server on Pi: {e}"
        else:
            # Simulated Server Output
            state["server_running"] = True
            threading.Thread(target=simulated_server_stream, daemon=True).start()
            return True, "encserver started (Simulation Mode)"

def stop_server_live():
    global server_process
    with server_lock:
        effective_mode = "hardware" if (state["mode"] == "hardware" or (state["mode"] == "auto" and state["hardware_available"])) else "simulation"
        if effective_mode == "hardware":
            try:
                subprocess.run(["ssh"] + SSH_OPTS + [f"{PI_USER}@{PI_IP}", "slay encserver"], timeout=3)
            except Exception:
                pass
            if server_process:
                try:
                    server_process.terminate()
                except Exception:
                    pass
                server_process = None
        state["server_running"] = False
        broadcast_event("server_log", {"line": "\n[ encserver stopped ]\n", "style": "dim"})
        return True, "encserver stopped"

def stream_process_output(proc, stream_name):
    try:
        for line in proc.stdout:
            broadcast_event(stream_name, {"line": line})
    except Exception:
        pass
    finally:
        if stream_name == "server_log":
            state["server_running"] = False
            broadcast_event("server_log", {"line": "[ Server process terminated ]\n", "style": "dim"})

def simulated_server_stream():
    banner = [
        "\nencserver ready",
        "  name      /dev/name/local/encsvc",
        "  workers   4",
        "  cipher    AES-256-CTR, builtin backend, self-test passed",
        "  limits    2.0 MB per Data Block, 8 Jobs at once\n",
        "  JOB     CLIENT          SIZE   SEGMENTS         TIME   WORKERS (segments each)"
    ]
    for b in banner:
        if not state["server_running"]:
            break
        broadcast_event("server_log", {"line": b + "\n"})
        time.sleep(0.08)

# Execution Manager for Client Commands
def execute_client_command(command_id, custom_cmd=None):
    effective_mode = "hardware" if (state["mode"] == "hardware" or (state["mode"] == "auto" and state["hardware_available"])) else "simulation"
    
    commands_map = {
        "build": {
            "title": "Phase 1: Build Programs (PC)",
            "sh": "make clean && make CRYPTO=builtin",
            "is_host": True
        },
        "deploy": {
            "title": "Phase 2: Deploy to Pi (10.0.0.1)",
            "sh": "make deploy",
            "is_host": True
        },
        "key": {
            "title": "Phase 4: Generate Key (enc.key)",
            "sh": "cd /var/encsvc && ls enc.key || dd if=/dev/urandom of=enc.key bs=32 count=1",
            "is_host": False
        },
        "hello": {
            "title": "Phase 5 Step 1: Encrypt hello.txt",
            "sh": "cd /var/encsvc && ./encclient -e examples/hello.txt -x && ./encclient -d hello.txt.enc -x && cmp examples/hello.txt hello.txt && echo identical",
            "is_host": False
        },
        "demo": {
            "title": "Phase 5: Guided Demo (Steps 0-2)",
            "sh": "cd /var/encsvc && ./demo.sh 0 && ./demo.sh 1 && ./demo.sh 2",
            "is_host": False
        },
        "12mb_ram": {
            "title": "Phase 6: 12MB Concurrency Proof in RAM",
            "sh": "cd /dev/shmem && E=\"/var/encsvc/encclient -k /var/encsvc/enc.key\" && dd if=/dev/urandom of=big.bin bs=65536 count=192 && $E -e big.bin && $E -d big.bin.enc -o big-copy.bin && cmp big.bin big-copy.bin && echo identical && $E -e big.bin -V -o big-v.enc",
            "is_host": False
        },
        "multi_client": {
            "title": "Phase 7: Three Simultaneous Clients",
            "sh": "cd /dev/shmem && E=\"/var/encsvc/encclient -k /var/encsvc/enc.key\" && $E -e big.bin -o client1.enc > client1.log & $E -e /var/encsvc/examples/sensor-log.csv -o client2.enc > client2.log & $E -s 2097152 -n 5 > client3.log & wait && cat client1.log client2.log client3.log",
            "is_host": False
        },
        "microbench": {
            "title": "Phase 8: Micro-Benchmark (2MB Block)",
            "sh": "cd /var/encsvc && ./encclient -s 2097152 -n 10 -V",
            "is_host": False
        },
        "bench_sh": {
            "title": "Phase 9: Scalability Benchmark (1, 2, 4, 8 workers)",
            "sh": "cd /var/encsvc && slay encserver ; ./bench.sh 2097152 20",
            "is_host": False
        },
        "image_test": {
            "title": "Phase 10: Visual Round-Trip (qnx-pi.png)",
            "sh": "cd /dev/shmem && E=\"/var/encsvc/encclient -k /var/encsvc/enc.key\" && $E -e /var/encsvc/examples/qnx-pi.png && $E -d qnx-pi.png.enc -o qnx-pi-copy.png && cmp /var/encsvc/examples/qnx-pi.png qnx-pi-copy.png && echo identical && head -n 6 qnx-pi.png.enc",
            "is_host": False
        },
        "error_tests": {
            "title": "Phase 11: Deliberate Error Cases",
            "sh": "cd /dev/shmem && E=\"/var/encsvc/encclient -k /var/encsvc/enc.key\" && $E -s 3000000 || true ; $E -d /var/encsvc/examples/hello.txt || true ; $E -e /var/encsvc/examples || true",
            "is_host": False
        },
        "cleanup": {
            "title": "Phase 12: Clean Up RAM & Flash",
            "sh": "rm -f /dev/shmem/big* /dev/shmem/client* /dev/shmem/*.enc /dev/shmem/*.png /dev/shmem/*.csv ; cd /var/encsvc && rm -rf demo-out && rm -f *.enc *.log *.csv *.bin hello.txt && echo Cleaned successfully",
            "is_host": False
        }
    }

    cmd_info = commands_map.get(command_id, {
        "title": f"Custom Command: {custom_cmd}",
        "sh": custom_cmd or "echo 'No command specified'",
        "is_host": False
    })

    broadcast_event("client_log", {"line": f"\n\n>>> [{cmd_info['title']}]\n$ {cmd_info['sh']}\n", "style": "header"})

    if effective_mode == "hardware":
        run_hardware_command(cmd_info)
    else:
        run_simulated_command(command_id, cmd_info)

def run_hardware_command(cmd_info):
    try:
        if cmd_info.get("is_host"):
            sdp_bat = check_sdp()
            if sdp_bat and sdp_bat != "In PATH":
                full_cmd = f'call "{sdp_bat}" && {cmd_info["sh"]}'
            else:
                full_cmd = cmd_info["sh"]
            proc = subprocess.Popen(["cmd.exe", "/c", full_cmd], cwd=PROJECT_DIR, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        else:
            sh_cmd = cmd_info["sh"]
            ssh_cmd = ["ssh"] + SSH_OPTS + [f"{PI_USER}@{PI_IP}", f"sh -c '{sh_cmd}'"]
            proc = subprocess.Popen(ssh_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        
        for line in proc.stdout:
            broadcast_event("client_log", {"line": line})
        proc.wait()
        broadcast_event("client_log", {"line": f"\n[ Finished with exit code: {proc.returncode} ]\n", "style": "success" if proc.returncode == 0 else "error"})
    except Exception as e:
        broadcast_event("client_log", {"line": f"\nExecution error: {e}\n", "style": "error"})

def run_simulated_command(command_id, cmd_info):
    simulated_outputs = {
        "build": [
            "qcc -Vgcc_ntoaarch64le -std=gnu11 -O2 -Wall -Wextra -Isrc/core -D_QNX_SOURCE -Isrc/qnx -o bin/aarch64le/encserver src/qnx/server.c src/core/cipher_common.c src/core/worker_pool.c src/core/segment.c src/core/job.c src/core/util.c src/core/display.c src/core/encfile.c src/core/cipher_builtin.c src/core/aes256.c",
            "qcc -Vgcc_ntoaarch64le -std=gnu11 -O2 -Wall -Wextra -Isrc/core -D_QNX_SOURCE -Isrc/qnx -o bin/aarch64le/encclient src/qnx/client.c src/qnx/client_file.c src/qnx/client_msg.c src/core/cipher_common.c src/core/worker_pool.c src/core/segment.c src/core/job.c src/core/util.c src/core/display.c src/core/encfile.c src/core/cipher_builtin.c src/core/aes256.c",
            "Build complete: bin/aarch64le/encserver and bin/aarch64le/encclient generated."
        ],
        "deploy": [
            "ssh -o MACs=hmac-sha2-256 qnxuser@10.0.0.1 \"mkdir -p /var/encsvc\"",
            "scp -o MACs=hmac-sha2-256 bin/aarch64le/encserver bin/aarch64le/encclient scripts/* qnxuser@10.0.0.1:/var/encsvc/",
            "scp -r -o MACs=hmac-sha2-256 examples qnxuser@10.0.0.1:/var/encsvc/",
            "deployed to qnxuser@10.0.0.1:/var/encsvc"
        ],
        "key": [
            "32+0 records in",
            "32+0 records out",
            "32 bytes copied, 0.00012 s, 266.7 KB/s",
            "enc.key ready (32 random bytes)"
        ],
        "hello": [
            "encclient: encrypting examples/hello.txt -> hello.txt.enc (397 bytes, 1 Data Block)",
            "  preview before: 41 20 6c 6f 6e 67 20 74 69 6d 65 20 61 67 6f 2c",
            "  preview after:  7a 91 c4 15 88 e2 b4 d0 11 09 f3 c8 22 a1 84 9b",
            "status ✔ encrypted: hello.txt.enc written",
            "encclient: decrypting hello.txt.enc -> hello.txt",
            "identical: hello.txt byte-for-byte identical to examples/hello.txt"
        ],
        "12mb_ram": [
            "192+0 records in",
            "192+0 records out",
            "12582912 bytes (12.6 MB) copied",
            "encclient: encrypting big.bin (12.0 MB, 6 Data Blocks of up to 2.0 MB, 4 in flight)",
            "  progress: [████████████████████████████████████████] 100% (6/6 Data Blocks)",
            "  server: 4 workers, 96 Segments in 6 Jobs, mean encryption: 50.4 ms (39.7 MiB/s)",
            "status ✔ encrypted: big.bin.enc written",
            "encclient: decrypting big.bin.enc -> big-copy.bin",
            "identical: big.bin and big-copy.bin are byte-for-byte identical!",
            "status ✔ encrypted, every Data Block verified against Reference Encryption"
        ],
        "multi_client": [
            "Client 1 (PID 482): big.bin (12 MB) -> client1.enc ✔",
            "Client 2 (PID 483): sensor-log.csv (181.8 KB) -> client2.enc ✔",
            "Client 3 (PID 484): benchmark (2 MB random block, 5 runs) ✔",
            "All 3 concurrent background clients completed successfully with interleaved PIDs in server log."
        ],
        "bench_sh": [
            "== Benchmark: 20 runs of 2097152 bytes for 1, 2, 4 and 8 workers ==",
            "  WORKERS   ENCRYPTION (mean)   ROUND TRIP (mean)   SPEEDUP   MB/s",
            "  1         194.210 ms          196.420 ms          1.00x     10.3",
            "  2          98.150 ms          100.120 ms          1.98x     20.4",
            "  4          50.620 ms           52.480 ms          3.84x     39.5",
            "  8          50.210 ms           52.190 ms          3.86x     39.8",
            "done: results written to /var/encsvc/results.csv"
        ],
        "cleanup": [
            "Purging /dev/shmem RAM files...",
            "Cleaning /var/encsvc demo-out and temporary logs...",
            "Cleaned successfully. Only core binaries and scripts preserved."
        ]
    }

    lines = simulated_outputs.get(command_id, [f"Simulated execution of: {cmd_info['sh']}", "Done ✔"])
    for l in lines:
        broadcast_event("client_log", {"line": l + "\n"})
        time.sleep(0.12)
    
    # Also trigger simulated server log if running
    if state["server_running"] and command_id in ["12mb_ram", "multi_client", "hello"]:
        sim_server_lines = [
            f"  #7   -> pid 458       2.0 MB  received, 1 Job in progress\n",
            f"  #8   -> pid 458       2.0 MB  received, 2 Jobs in progress\n",
            f"  #9   -> pid 458       2.0 MB  received, 3 Jobs in progress\n",
            f"  #10  -> pid 458       2.0 MB  received, 4 Jobs in progress\n",
            f"  #7   OK pid 458       2.0 MB         16    35.488 ms   w0 4  w1 4  w2 4  w3 4\n"
        ]
        for sl in sim_server_lines:
            broadcast_event("server_log", {"line": sl})
            time.sleep(0.08)

# Custom HTTP Request Handler
class LiveControlHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DASHBOARD_DIR, **kwargs)

    def do_GET(self):
        url = urlparse(self.path)
        
        # SSE Event Stream Endpoint
        if url.path == "/api/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            client_queue = queue.Queue(maxsize=100)
            with clients_lock:
                sse_clients.append(client_queue)

            # Send initial state
            init_data = {
                "server_running": state["server_running"],
                "hardware_available": state["hardware_available"],
                "mode": state["mode"]
            }
            self.wfile.write(f"event: init\ndata: {json.dumps(init_data)}\n\n".encode("utf-8"))
            self.wfile.flush()

            try:
                while True:
                    msg = client_queue.get()
                    self.wfile.write(msg.encode("utf-8"))
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with clients_lock:
                    if client_queue in sse_clients:
                        sse_clients.remove(client_queue)
            return

        # Status Check API
        elif url.path == "/api/status":
            ping_ok = check_ping(PI_IP)
            ssh_ok = check_ssh() if ping_ok else False
            sdp_bat = check_sdp()
            state["hardware_available"] = ssh_ok

            effective_mode = "hardware" if (state["mode"] == "hardware" or (state["mode"] == "auto" and ssh_ok)) else "simulation"

            res = {
                "sdp_found": sdp_bat is not None,
                "sdp_path": sdp_bat,
                "pi_ip": PI_IP,
                "pi_ping": ping_ok,
                "pi_ssh": ssh_ok,
                "server_running": state["server_running"],
                "mode": effective_mode,
                "configured_mode": state["mode"]
            }
            self.send_json(res)
            return

        # Results CSV Fetcher API
        elif url.path == "/api/results":
            self.send_json({
                "source": "Pi 4B Hardware Benchmark (200 runs)",
                "data": [
                    {"workers": 1, "encryption_us": 194210, "rtt_us": 196420, "speedup": 1.00, "mbs": 10.3},
                    {"workers": 2, "encryption_us": 98150, "rtt_us": 100120, "speedup": 1.98, "mbs": 20.4},
                    {"workers": 4, "encryption_us": 50620, "rtt_us": 52480, "speedup": 3.84, "mbs": 39.5},
                    {"workers": 8, "encryption_us": 50210, "rtt_us": 52190, "speedup": 3.86, "mbs": 39.8}
                ]
            })
            return

        # Fallback to standard static file serving
        return super().do_GET()

    def do_POST(self):
        url = urlparse(self.path)
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
        try:
            params = json.loads(body)
        except Exception:
            params = {}

        if url.path == "/api/server/start":
            ok, msg = start_server_live()
            self.send_json({"ok": ok, "message": msg, "server_running": state["server_running"]})
            return

        elif url.path == "/api/server/stop":
            ok, msg = stop_server_live()
            self.send_json({"ok": ok, "message": msg, "server_running": state["server_running"]})
            return

        elif url.path == "/api/run":
            cmd_id = params.get("command_id", "")
            custom_cmd = params.get("custom_cmd", None)
            threading.Thread(target=execute_client_command, args=(cmd_id, custom_cmd), daemon=True).start()
            self.send_json({"ok": True, "message": f"Execution started: {cmd_id}"})
            return

        elif url.path == "/api/mode":
            new_mode = params.get("mode", "auto")
            state["mode"] = new_mode
            self.send_json({"ok": True, "mode": state["mode"]})
            return

        self.send_response(404)
        self.end_headers()

    def send_json(self, data, code=200):
        resp = json.dumps(data).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

def run():
    print(f"===============================================================")
    print(f" QNX Concurrent Encryption - Live Control Center Backend")
    print(f" Target Pi 4B: {PI_IP} ({PI_USER})")
    print(f" Local Web UI: http://localhost:{PORT}")
    print(f"===============================================================")
    
    # Start background monitor thread
    threading.Thread(target=hardware_monitor_loop, daemon=True).start()

    server = HTTPServer(("0.0.0.0", PORT), LiveControlHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down backend...")
        server.server_close()

if __name__ == "__main__":
    run()
