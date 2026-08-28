#!/usr/bin/env python3
"""One-shot stdlib HTTP server for deterministic Lazy rollout integration tests."""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--action", action="append", default=[])
    parser.add_argument("--require-init-substring", action="append", default=[])
    parser.add_argument("--require-run-id")
    parser.add_argument("--require-iteration", type=int)
    parser.add_argument("--delay-seconds", type=float, default=0.0)
    args = parser.parse_args()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(size).decode("utf-8"))
            init = request.get("init", "")
            missing = [
                value for value in args.require_init_substring if value not in init
            ]
            errors = []
            if missing:
                errors.append("missing init fragments: %s" % missing)
            if (
                args.require_run_id is not None
                and request.get("run_id") != args.require_run_id
            ):
                errors.append(
                    "run_id=%r, expected %r"
                    % (request.get("run_id"), args.require_run_id)
                )
            if (
                args.require_iteration is not None
                and request.get("iteration") != args.require_iteration
            ):
                errors.append(
                    "iteration=%r, expected %r"
                    % (request.get("iteration"), args.require_iteration)
                )
            if args.delay_seconds > 0:
                time.sleep(args.delay_seconds)
            if errors:
                response = {
                    "status": "invalid_request",
                    "actions": [],
                    "action_chains": [],
                    "error": "; ".join(errors),
                }
                status = 400
            else:
                response = {
                    "status": "ok",
                    "actions": args.action,
                    "action_chains": [args.action],
                }
                status = 200
            payload = json.dumps(response).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            print(
                "MOCK_LLM_REQUEST run_id=%s request_id=%s iteration=%s "
                "init_bytes=%d actions=%d delay_seconds=%.3f"
                % (
                    request.get("run_id"),
                    request.get("request_id"),
                    request.get("iteration"),
                    len(init.encode("utf-8")),
                    len(args.action),
                    args.delay_seconds,
                ),
                flush=True,
            )

        def log_message(self, _format, *_args):
            pass

    server = HTTPServer(("127.0.0.1", args.port), Handler)
    server.handle_request()
    server.server_close()


if __name__ == "__main__":
    main()
