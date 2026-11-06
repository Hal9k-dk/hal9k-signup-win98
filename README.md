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

The details can be posted to a Slack incoming webhook, but only when a
webhook is configured *and* the send is confirmed in the prompt. Without a
configuration file the form stays offline and nothing is transmitted. Signups
are not stored locally; email and date validation are basic format checks, not
verification of real addresses or dates.

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

## Posting to Slack

Copy `HAL9K.INI.example` to `HAL9K.INI` next to `HAL9K.EXE` (the same folder
as the executable, also when running it from a CD) and fill in the webhook
URL:

```ini
[slack]
url=https://hooks.slack.com/services/T000/B000/XXXX
```

`HAL9K.INI` is git-ignored because the webhook URL is a credential: anyone
holding it can post to that channel. Do not commit it or paste it into a
screenshot.

The last page's **Send indmeldelse** button validates the whole form, asks
for confirmation, and then posts the fields as a Slack message. The request runs on a worker thread, so a slow
connection does not freeze the window. The status line reports the outcome:
`Indmeldelse sendt.`, `Serveren svarede HTTP <code>`, or a WinINet error code
such as `Kunne ikke sende indmeldelsen. Fejl 12175`. When the endpoint rejects
the message its reply is shown too, for example
`Serveren svarede HTTP 400: invalid_payload`.

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
- **Sending** — started by confirming the prompt. All fields, both membership
  options and both buttons are disabled, the send button reads `Sender ...`,
  and the title bar reads `hal9k - Indmeldelse (sender)`.
- **Sent** — reached only on success. The form stays disabled on purpose:
  there is nothing left to resend, the finish button reads `Sendt`, the
  navigation buttons are disabled, `Annuller` becomes `Luk`, and the title bar
  reads `(sendt)`. Close the window when done.

A failed attempt returns the form to **editing** with the values intact, so
the same details can be sent again after fixing the configuration or the
connection. The result dialog repeats the delivery time, name and email, or
the failure reason.

### Testing locally

`tools/fake-slack-webhook.py` is a dependency-free stand-in for a webhook: it
accepts the POST, prints the signup in the terminal, and lists received
signups at the same address in a browser.

```sh
python3 tools/fake-slack-webhook.py --port 8765
```

```ini
[slack]
url=http://127.0.0.1:8765/hook
```

Then open `http://127.0.0.1:8765/` to see the submissions. Nothing leaves the
machine, so this is the safe way to try the send path and the form states.

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

Slack requires TLS 1.2 or newer for webhooks. Windows 98's WinINet/schannel
stops at TLS 1.0, so posting straight to `hooks.slack.com` fails there with a
connection or certificate error. On Windows 95/98/Me, point `url` at an
internal plain-HTTP relay that forwards to Slack over TLS instead, for
example an nginx location:

```nginx
location /hal9k {
    proxy_pass https://hooks.slack.com/services/T000/B000/XXXX;
    proxy_set_header Content-Type application/json;
}
```

and use `url=http://relay.example.internal/hal9k`. Keep the real webhook URL
in the relay's configuration, not in the form. On Windows 2000 and later the
webhook URL can be used directly.

The message contains personal data (name, address, email, phone, birth date).
Everyone who can read the target channel can see it, so choose the channel
and the retention policy accordingly.

## Build

Install the MinGW-w64 cross-compiler, then run:

```sh
make
```

To build a CD image as well (requires `xorriso`):

```sh
make hal9k.iso
```

The ISO contains `HAL9K.EXE` and `AUTORUN.INF`; `HAL9K.INI` is not included,
so add it yourself if the CD should post to Slack. Mount it as a CD in a
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
