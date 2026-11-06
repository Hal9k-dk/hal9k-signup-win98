#!/usr/bin/env python3
"""Local stand-in for a Slack incoming webhook.

Accepts the POST that HAL9K.EXE sends, prints the signup in the terminal and
serves the received signups at http://127.0.0.1:8765/ so a successful run is
visible in a browser too. Standard library only.

    python3 tools/fake-slack-webhook.py --port 8765

Then point HAL9K.INI at it:

    [slack]
    url=http://127.0.0.1:8765/hook

The payload is shown here and nowhere else; no request leaves this machine.
"""

import argparse
import html
import http.server
import json
import time

submissions = []


def unescape_slack(text):
    """Undo the entity escaping hal9k.c applies for Slack's mrkdwn."""
    return html.unescape(text).replace("*", "")


def parse_payload(body):
    """Return (title, fields) from the Slack payload, tolerating junk."""
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None, [("raw body", body.decode("utf-8", "replace"))]
    text = payload.get("text", "")
    lines = [line for line in text.splitlines() if line.strip()]
    title = unescape_slack(lines[0]) if lines else "submission"
    fields = []
    for line in lines[1:]:
        label, _, value = line.partition(":")
        fields.append((unescape_slack(label).strip(), unescape_slack(value).strip()))
    return title, fields


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "fake-slack-webhook"

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        title, fields = parse_payload(body)
        stamp = time.strftime("%H:%M:%S")
        submissions.append((stamp, self.path, title, fields))

        width = max((len(label) for label, _ in fields), default=0)
        print(f"[{stamp}] POST {self.path}", flush=True)
        if title:
            print(f"  {unescape_slack(title)}", flush=True)
        for label, value in fields:
            print(f"  {label.ljust(width)}  {value}", flush=True)
        print(flush=True)

        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.end_headers()
        self.wfile.write(b"ok")

    def do_GET(self):
        page = ["<!doctype html><meta charset=utf-8>",
                "<title>hal9k signups</title>",
                "<style>body{font:14px/1.5 system-ui,sans-serif;margin:2rem;"
                "max-width:40rem}dt{font-weight:600;margin-top:.6rem}"
                "dd{margin:0 0 .2rem 1rem}section{border-top:1px solid #ccc;"
                "padding:.4rem 0}</style>",
                "<h1>hal9k signups</h1>"]
        if not submissions:
            page.append("<p>No signups received yet.</p>")
        for stamp, path, title, fields in reversed(submissions):
            page.append(f"<section><h2>{html.escape(title or 'submission')}"
                        f" <small>{stamp} &middot; {html.escape(path)}</small></h2><dl>")
            for label, value in fields:
                page.append(f"<dt>{html.escape(label)}</dt>"
                            f"<dd>{html.escape(value) or '&mdash;'}</dd>")
            page.append("</dl></section>")
        body = "".join(page).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to listen on (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765,
                        help="port to listen on (default: 8765)")
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Listening on http://{args.host}:{args.port}/hook "
          f"(open http://{args.host}:{args.port}/ to see signups)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print(flush=True)


if __name__ == "__main__":
    main()
