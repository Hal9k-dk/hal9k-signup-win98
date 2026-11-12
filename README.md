# hal9k indmeldelse

A classic Windows-style 32-bit Win32 signup wizard for Hal9k, the hackerspace
in Aalborg (`Sofiendalsvej 80, 9200 Aalborg SV`, open Thursdays from 19.00),
inspired by the club's signup page at `signup.hal9k.dk`. It collects name,
email, address, birth date, telephone, membership and an optional message,
with basic required-field and format checks.

The wizard text follows the club's published details: the welcome page
describes the hackerspace, the Thursday opening and points to the survival
guide, and the membership page lists the monthly fee (150 kr., 75 kr. for
students), the PROSA discount and the access card that is issued after the
intro period.

The details can be posted to the signup service at `signup.hal9k.dk` — the
`POST /api/signup` endpoint deployed from the drillpress repository — or, when
no service is configured, directly to a Slack incoming webhook. Either way the
send needs a configured address *and* confirmation in the prompt. Without a
configuration file the form stays offline and nothing is transmitted. Signups
are not stored locally; the form's email and date checks are basic format
checks, and the signup service re-checks every field, including that the birth
date is a real, non-future date.

## Keyboard shortcuts

Every field has a classic mnemonic: the underlined letter in each label is
reached with `Alt`+that letter.

| Shortcut | Field |
| -------- | ----- |
| `Alt+F` | Fornavn(e) |
| `Alt+E` | Efternavn(e) |
| `Alt+M` | Email |
| `Alt+V` | Vejnavn og nr. |
| `Alt+P` | Postnummer |
| `Alt+B` | By |
| `Alt+D` | Fødselsdato |
| `Alt+T` | Telefon nr. |
| `Alt+A` | Almindeligt medlem |
| `Alt+S` | Studiemedlem |
| `Alt+K` | Evt. besked |

Mnemonics use the first free letter of each label, so "Email" and
"Fødselsdato" are reached by a letter inside the word where the initial
letter is already taken. Membership keys select the radio button, as in a
classic dialog. Each mnemonic applies to the wizard page that holds the field.
`Tab` and `Shift+Tab` still move between the fields of the current page.

## Posting a signup

Copy `HAL9K.INI.example` to `HAL9K.INI` next to `HAL9K.EXE` — the same folder
as the executable, also on a CD — and fill in where the signup should go. The
file is looked up beside the executable, so `make hal9k.iso` folds it into the
CD image when it is present (see [Build](#build)). The signup service is the
normal choice:

```ini
[signup]
url=https://signup.hal9k.dk/api/signup
```

That is the service deployed from the drillpress repository. It accepts one
JSON object, checks every field again and forwards the signup to hal9k. The
wizard sends the documented v1 contract — `firstName`, `lastName`, `email`,
`street`, `postalCode`, `city`, `birthDate`, `phone`, `membership` (`ordinary`
or `student`), the optional `message`, and `experience` set to
`hal9k-signup-win98`. User-entered values are sent as typed, so the service
receives the text itself rather than Slack markup. The direct Slack path also
includes the experience identifier in its message.

Alternatively the fields can be posted straight to a Slack incoming webhook:

```ini
[slack]
url=https://hooks.slack.com/services/T000/B000/XXXX
```

When both sections are filled in, `[signup]` wins. A webhook URL is a
credential: anyone holding it can post to that channel. `HAL9K.INI` is
git-ignored, so do not commit it or paste it into a screenshot.

The last page's **Send indmeldelse** button validates the whole form, asks for
confirmation, and then posts it. The request runs on a worker thread, so a
slow connection does not freeze the window, and the status line counts the
seconds it is taking (see [Diagnostics and timeouts](#diagnostics-and-timeouts)).
The status line reports the outcome: `Indmeldelse sendt.` or a WinINet error
code such as `Kunne ikke sende indmeldelsen. Fejl 12175`. A rejected signup is
reported with the service's own explanation and the fields it objected to, for
example:

```
Serveren svarede HTTP 422: Kontrollér felterne.
Fødselsdato: Brug en gyldig fødselsdato i formatet dd-mm-yyyy.
```

Other responses (`HTTP 400`, `HTTP 415`, `HTTP 502`) show the service's
message, while a Slack webhook error shows its short reply such as
`Serveren svarede HTTP 400: invalid_payload`. On a successful send the result
dialog also repeats the receipt returned by the signup service. Successful
signups play the Windows-installed `Media\tada.wav` asynchronously; failed
or timed-out attempts do not. If the sound file is missing, success stays
silent.

### Diagnostics and timeouts

A send always ends. WinINet is given a 30-second timeout for connecting,
sending and receiving, connection retries are turned off, and a watchdog gives
the whole attempt 120 seconds. While the request runs the status line names
the step it is on and counts the seconds:

```
Sender indmeldelse ... (sender data og venter på svar, 12 s)
```

Without an answer before the deadline the form returns to **editing** with the
values intact, the status line names the step that stopped, and a dialog says
so and points at the log file. A late answer from that attempt is discarded, so
it cannot overwrite the result of a later one, and **Annuller** stays available
throughout: closing the window ends the process and with it the blocked
request.

Every run appends to `HAL9K.LOG` next to `HAL9K.EXE`, or in the temp folder
when that folder is read-only (on a CD). A line is written *before* and
*after* each WinINet call and flushed immediately, so when a send hangs the
last line names the call that blocked:

```
[12:13:51] attempt: mode=signup-api host=signup.hal9k.dk port=443 secure=1 path=/api/signup payload=272 bytes
[12:13:51] step: connecting to the host
[12:13:51] InternetConnectA ok after 12 ms
[12:13:51] step: sending the request and waiting for the response
[12:14:11] HttpSendRequestA failed: error 12029 (cannot connect) after 20001 ms
```

The log records hosts, ports, timings, error codes and the server's own reply,
but never the signup fields, and for a Slack webhook not the path either,
because the webhook URL is the credential. When a send fails, include the log
when reporting it.

### Wizard pages

The window is a classic four-page wizard, each page with a bold header and a
short description:

1. **Velkommen til hal9k** — what the signup is and that nothing is sent before
   the confirmation on the last page.
2. **Personlige oplysninger** — name, email, address, postcode, city, birth
   date and phone. Leaving this page validates the fields and puts the cursor
   on the first one that needs attention.
3. **Medlemskab** — ordinary or student membership.
4. **Valgfri besked** — the optional message, and the finish button.

**Næste >** and **< Tilbage** move between pages; on the last page **Næste >**
becomes **Send indmeldelse**. `Enter` advances (except inside the message box,
where it inserts a newline), while `Escape` and **Annuller** close the window.
Pages can be revisited until the signup is actually sent, and Tab skips the
hidden pages. Mnemonics still work, but only for fields on the visible page.

### Form state

The window has three states, which control what can be edited:

- **Editing** — the initial state; every field and button is enabled.
- **Sending** — started by confirming the prompt. Every field, both membership
  options and both navigation buttons are disabled, the send button reads
  `Sender ...`, the status line names the current step and the seconds spent,
  and the title bar reads `hal9k - Indmeldelse (sender)`. `Annuller` stays
  available so a stuck request cannot trap the user.
- **Sent** — reached only on success. The form stays disabled on purpose:
  there is nothing left to resend, the finish button reads `Sendt`, the
  navigation buttons are disabled, `Annuller` becomes `Luk`, and the title bar
  reads `(sendt)`. Close the window when done.

A failed attempt returns the form to **editing** with the values intact, so
the same details can be sent again after fixing the configuration or the
connection. The result dialog repeats the delivery time, name and email, or
the failure reason.

### Testing locally

`tools/fake-signup-api.py` is a dependency-free stand-in for the signup
service: it validates the same contract, prints each signup in the terminal,
lists received signups at the same address in a browser, and can simulate a
Slack delivery failure.

```sh
python3 tools/fake-signup-api.py --port 8766
```

```ini
[signup]
url=http://127.0.0.1:8766/api/signup
```

Open `http://127.0.0.1:8766/` to see the submissions. Add `--slack-fail` to
answer every valid signup with the service's `HTTP 502`, which exercises the
failure path without touching the network. `tools/fake-slack-webhook.py` does
the same for the direct webhook path:

```sh
python3 tools/fake-slack-webhook.py --port 8765
```

```ini
[slack]
url=http://127.0.0.1:8765/hook
```

Nothing leaves the machine either way, so this is the safe way to try the
send path, the error display and the form states.

### Prefilled test data

Start the form with the fields already filled in:

```sh
hal9k.exe --test-data
```

`--test-data` (also `-test-data` or `/test-data`) fills every field with sample
data that passes validation, including Danish characters, a message and a
membership choice, so the send path can be exercised without typing. The wizard
opens on the personal details page with that data filled in. Nothing is sent
automatically: the usual confirmation still applies, and the status line notes
that test data was loaded.

### Windows 98 and TLS

`signup.hal9k.dk` and Slack both require TLS 1.2 or newer. Windows 98's
WinINet/schannel stops at TLS 1.0, so posting straight to either fails there
with a connection or certificate error — usually error 12175, reported in the
status line and in `HAL9K.LOG` as a secure channel failure. On Windows
95/98/Me, point the URL at an internal plain-HTTP relay that forwards over TLS
instead, for example an nginx location:

```nginx
location /signup/api/ {
    proxy_pass https://signup.hal9k.dk/api/;
    proxy_set_header Content-Type application/json;
}
```

and use `url=http://relay.example.internal/signup/api/signup`. The webhook path
has the same shape:

```nginx
location /hal9k {
    proxy_pass https://hooks.slack.com/services/T000/B000/XXXX;
    proxy_set_header Content-Type application/json;
}
```

with `[slack] url=http://relay.example.internal/hal9k`. Keep the real webhook
URL in the relay's configuration, not in the form. On Windows 2000 and later
the addresses can be used directly.

The signup contains personal data (name, address, email, phone, birth date).
The signup service logs it and sends it to Slack, so restrict access to the
service logs and to the target channel, and choose the retention policy
accordingly.

## Build

Install the MinGW-w64 cross-compiler, then run:

```sh
make
```

To build a CD image as well (requires `xorriso`):

```sh
make hal9k.iso
```

The ISO contains `HAL9K.EXE` and `AUTORUN.INF`, plus `HAL9K.INI` when that
file exists in the folder — a CD image built next to a filled-in configuration
therefore sends signups without further steps. Its address is a credential,
so keep such an image to yourself instead of publishing it. Without
`HAL9K.INI` the image is built without it and the form stays offline; the
recipe says which of the two it did. Mount the image as a CD in a
Windows 98 virtual machine or burn it to a CD. AutoRun depends on Windows
and drive settings; if it does not start, open `HAL9K.EXE` manually. The ISO
is not bootable. `make clean` removes the generated executable, resource
object and ISO.

The executable icon is adapted from `https://www.hal9k.dk/favicon.ico`;
`favicon-original.ico` is the downloaded original and `hal9k.ico` is its
classic 8-bit Windows-compatible conversion.

The build bypasses the C runtime and links only to Windows system libraries
(kernel32, user32, gdi32 and wininet);
it uses ANSI Win32 APIs and targets i386. The executable's imports and PE
headers can be inspected with `i686-w64-mingw32-objdump -p hal9k.exe`.
Windows 98 compatibility has not yet been verified on an actual Windows 98
installation or emulator.
