#!/usr/bin/env python3
"""Local stand-in for the signup service deployed from drillpress.

Accepts ``POST /api/signup`` with the v1 JSON contract, validates it the way
the Go service does, prints each signup in the terminal and lists received
signups at http://127.0.0.1:8766/ in a browser. Standard library only.

    python3 tools/fake-signup-api.py --port 8766

Then point HAL9K.INI at it:

    [signup]
    url=http://127.0.0.1:8766/api/signup

Use ``--slack-fail`` to answer every valid signup with the service's HTTP 502
response, which exercises the client's failure path. Nothing leaves this
machine either way.
"""

import argparse
import datetime
import html
import http.server
import json
import re
import time

MAX_FIELD = 256
MAX_MESSAGE = 4000
MAX_BODY = 16 << 10

PERSONAL = ("firstName", "lastName", "email", "street",
            "postalCode", "city", "birthDate", "phone")
PROPERTIES = PERSONAL + ("membership", "message", "experience")

LABELS = {
    "firstName": "Fornavn",
    "lastName": "Efternavn",
    "email": "Email",
    "street": "Vejnavn og nr.",
    "postalCode": "Postnummer",
    "city": "By",
    "birthDate": "Fødselsdato",
    "phone": "Telefon nr.",
    "membership": "Medlemsskab",
    "message": "Besked",
    "experience": "Experience",
}

EMAIL = re.compile(r"[^@\s<>,;]+@[^@\s<>,;]+\.[^@\s<>,;]+")
POSTAL = re.compile(r"[0-9]{4}")

submissions = []
slack_failing = False


def validate(data):
    """Validate personal fields, membership and optional message/experience."""
    errors = {}
    for key in PROPERTIES:
        value = data.get(key, "")
        if not isinstance(value, str):
            errors[key] = "Forventede tekst."
            continue
        if key == "message":
            if len(value.encode("utf-8")) > MAX_MESSAGE:
                errors[key] = "Beskeden må højst fylde 4000 bytes."
            continue
        if key == "experience":
            if len(value.encode("utf-8")) > MAX_FIELD:
                errors[key] = "Brug højst 256 tegn."
            continue
        if key == "membership":
            if value not in ("ordinary", "student"):
                errors[key] = "Vælg en medlemstype."
            continue
        if not value.strip():
            errors[key] = "Dette felt skal udfyldes."
        elif len(value.encode("utf-8")) > MAX_FIELD:
            errors[key] = "Brug højst 256 tegn."
    if "email" not in errors and not EMAIL.fullmatch(data.get("email", "")):
        errors["email"] = "Indtast en gyldig emailadresse."
    if "postalCode" not in errors and not POSTAL.fullmatch(data.get("postalCode", "")):
        errors["postalCode"] = "Indtast et firecifret postnummer."
    if "birthDate" not in errors:
        text = data.get("birthDate", "")
        try:
            date = datetime.datetime.strptime(text, "%d-%m-%Y").date()
        except ValueError:
            date = None
        if date is None or date.strftime("%d-%m-%Y") != text or date > datetime.date.today():
            errors["birthDate"] = "Brug en gyldig fødselsdato i formatet dd-mm-yyyy."
    return errors


def decode_body(body):
    """Return (data, error). Unknown properties and non-objects are rejected."""
    try:
        data = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None, "Ugyldige formulardata."
    if not isinstance(data, dict):
        return None, "Ugyldige formulardata."
    if any(key not in PROPERTIES for key in data):
        return None, "Ugyldige formulardata."
    return data, None


def print_signup(data):
    width = max(len(label) for label in LABELS.values())
    for key in PROPERTIES:
        value = data.get(key, "")
        print(f"  {LABELS[key].ljust(width)}  {value}", flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "fake-signup-api"

    def send_json(self, status, value):
        body = (json.dumps(value, ensure_ascii=False) + "\n").encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/api/signup":
            self.send_json(404, {"error": "Not found."})
            return
        content_type = (self.headers.get("Content-Type") or "").split(";")[0]
        if content_type != "application/json":
            self.send_json(415, {"error": "Forventede JSON."})
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self.send_json(400, {"error": "Forventede ét JSON-objekt."})
            return
        data, error = decode_body(self.rfile.read(length))
        if error:
            self.send_json(400, {"error": error})
            return
        errors = validate(data)
        if errors:
            self.send_json(422, {"error": "Kontrollér felterne.", "fields": errors})
            return

        stamp = time.strftime("%H:%M:%S")
        submissions.append((stamp, data))
        print(f"[{stamp}] POST /api/signup", flush=True)
        print_signup(data)
        print(flush=True)

        if slack_failing:
            self.send_json(502, {"error": "Din tilmelding kunne ikke sendes. Prøv igen senere."})
            return
        self.send_json(200, {"message": "Modtaget! Din tilmelding er sendt til foreningen."})

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
        for stamp, data in reversed(submissions):
            page.append(f"<section><h2>{html.escape(data.get('firstName', ''))} "
                        f"{html.escape(data.get('lastName', ''))} "
                        f"<small>{stamp}</small></h2><dl>")
            for key in PROPERTIES:
                value = data.get(key, "")
                page.append(f"<dt>{html.escape(LABELS[key])}</dt>"
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
    global slack_failing
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to listen on (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8766,
                        help="port to listen on (default: 8766)")
    parser.add_argument("--slack-fail", action="store_true",
                        help="answer valid signups with the service's HTTP 502")
    args = parser.parse_args()
    slack_failing = args.slack_fail
    server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Listening on http://{args.host}:{args.port}/api/signup "
          f"(open http://{args.host}:{args.port}/ to see signups)", flush=True)
    if slack_failing:
        print("Every valid signup will get HTTP 502.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print(flush=True)


if __name__ == "__main__":
    main()
