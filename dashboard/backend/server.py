"""
server.py - the dashboard backend. Start it with launch_dashboard.bat, or:

    py dashboard\\backend\\server.py

then open http://localhost:8080. It serves the page in dashboard/web, runs the
workflow steps (steps.py) and pushes every state change to the page (SSE).
Only this PC can reach it: it listens on 127.0.0.1 and refuses requests that
come from another web site.
"""
import atexit
import json
import os
import queue
import re
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import config                       # noqa: E402
import remote                       # noqa: E402
from session import Consoles, Hub, Store       # noqa: E402
from steps import STEP_BY_ID, WORKFLOW, Engine, close_session   # noqa: E402

hub = Hub()
store = Store(hub)
consoles = Consoles()
engine = Engine(store, consoles)
settings = {"auto_console": config.OPEN_CONSOLES}


def decorate(snap):
    """Add what the page needs besides the raw state: why each step is locked, settings."""
    snap["blocked"] = {s["id"]: engine.blocked(s["id"], snap) for s in WORKFLOW}
    snap["settings"] = dict(settings, consoles_available=config.OPEN_CONSOLES)
    return snap


store.decorate = decorate

ALLOWED_HOSTS = {f"localhost:{config.PORT}", f"127.0.0.1:{config.PORT}"}
ALLOWED_ORIGINS = {f"http://{h}" for h in ALLOWED_HOSTS}


class Handler(SimpleHTTPRequestHandler):
    server_version = "encdash"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=config.WEB_DIR, **kwargs)

    def log_request(self, code="-", size="-"):
        # Keep the backend window quiet: log only failed requests.
        try:
            failed = int(code) >= 400
        except (TypeError, ValueError):
            failed = True
        if failed:
            super().log_request(code, size)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    # ------------------------------------------------------------ helpers
    def _host_ok(self):
        return self.headers.get("Host", "") in ALLOWED_HOSTS

    def send_json(self, data, code=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def send_text(self, text, content_type, filename=None):
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        if filename:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    # ------------------------------------------------------------ GET
    def do_GET(self):
        if not self._host_ok():
            return self.send_json({"error": "use http://localhost:%d" % config.PORT}, 403)
        path = urlparse(self.path).path
        if path == "/favicon.ico":          # the page has an inline icon; browsers ask anyway
            self.send_response(204)
            self.end_headers()
            return
        if path == "/api/workflow":
            return self.send_json({"steps": WORKFLOW, "pi": {"host": config.PI_HOST, "user": config.PI_USER,
                                                             "dir": config.PI_DIR}})
        if path == "/api/state":
            return self.send_json(decorate(store.snapshot()))
        if path == "/api/stream":
            return self.stream()
        if path == "/api/benchmark.csv":
            if not engine.benchmark_csv:
                return self.send_json({"error": "no benchmark yet"}, 404)
            return self.send_text(engine.benchmark_csv, "text/csv", "results.csv")
        if path == "/api/session.json":
            return self.send_text(json.dumps(store.snapshot(), indent=2), "application/json",
                                  f"session-{store.snapshot()['session']['id']}.json")
        if path.startswith("/api/"):
            return self.send_json({"error": "not found"}, 404)
        return super().do_GET()

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = hub.add()
        try:
            first = f"event: state\ndata: {json.dumps(decorate(store.snapshot()))}\n\n"
            self.wfile.write(first.encode("utf-8"))
            self.wfile.flush()
            while True:
                try:
                    msg = q.get(timeout=15)
                except queue.Empty:
                    msg = ": keep-alive\n\n"
                self.wfile.write(msg.encode("utf-8"))
                self.wfile.flush()
        except OSError:
            pass                        # the tab was closed or reloaded
        finally:
            hub.remove(q)

    # ------------------------------------------------------------ POST
    def do_POST(self):
        origin = self.headers.get("Origin")
        if not self._host_ok() or (origin and origin not in ALLOWED_ORIGINS):
            return self.send_json({"error": "requests from other sites are refused"}, 403)
        if "application/json" not in self.headers.get("Content-Type", ""):
            return self.send_json({"error": "send JSON"}, 415)
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length) or b"{}") if length else {}
        except (ValueError, json.JSONDecodeError):
            return self.send_json({"error": "bad JSON"}, 400)
        path = urlparse(self.path).path

        m = re.fullmatch(r"/api/steps/([a-z]+)/(run|console)", path)
        if m and m.group(1) in STEP_BY_ID:
            step_id, action = m.groups()
            if action == "run":
                if settings["auto_console"]:
                    engine.open_console("activity")
                ok, msg = engine.start(step_id, body.get("options") or {})
                return self.send_json({"ok": ok, "message": msg}, 200 if ok else 409)
            ok = engine.open_console(step_id)
            return self.send_json({"ok": ok, "message": "opened" if ok else "no console for this step"},
                                  200 if ok else 409)
        if path in ("/api/console/activity", "/api/console/server"):
            ok = engine.open_console(path.rsplit("/", 1)[1])
            return self.send_json({"ok": ok, "message": "opened" if ok else "nothing to show yet"},
                                  200 if ok else 409)
        if path == "/api/settings":
            if "auto_console" in body:
                settings["auto_console"] = bool(body["auto_console"]) and config.OPEN_CONSOLES
            store.publish_now()
            return self.send_json({"ok": True, "settings": settings})
        if path == "/api/cancel":
            ok, msg = engine.cancel()
            return self.send_json({"ok": ok, "message": msg}, 200 if ok else 409)
        if path == "/api/server/stop":
            if store.snapshot()["busy"]:
                return self.send_json({"ok": False, "message": "wait for the running step"}, 409)
            if not (engine.server.running() or store.snapshot()["server"]["external"]):
                return self.send_json({"ok": False, "message": "encserver is not running"}, 409)
            engine.server.stop(None, "stopped from the dashboard")
            return self.send_json({"ok": True, "message": "encserver stopped"})
        if path == "/api/reset":
            ok, msg = engine.reset()
            return self.send_json({"ok": ok, "message": msg}, 200 if ok else 409)
        return self.send_json({"error": "not found"}, 404)


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], ConnectionError):
            return                      # a browser dropped a connection: normal
        super().handle_error(request, client_address)


def shutdown():
    if engine.server.running():
        print("Stopping encserver on the Pi...")
        remote.quick("slay -f -Q encserver", timeout=10)
        engine.server.kill_local()
    close_session(store.session_dir)


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    try:
        httpd = Server(("127.0.0.1", config.PORT), Handler)
    except OSError as e:
        print(f"Cannot listen on port {config.PORT}: {e}")
        print("Is the dashboard already running? Close its window, or set ENC_DASH_PORT to another port.")
        return 1
    atexit.register(shutdown)
    print("QNX Concurrent Encryption - dashboard backend")
    print(f"  open        http://localhost:{config.PORT}")
    print(f"  Pi          {config.PI_USER}@{config.PI_HOST}:{config.PI_DIR}")
    print(f"  logs        {store.session_dir}")
    print("  stop        Ctrl+C in this window")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping the dashboard backend...")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
