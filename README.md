# hal9k indmeldelse

A classic Windows-style 32-bit Win32 form inspired by the Danish hal9k
physics signup page. It includes the name, email, address, birth date,
telephone, membership and optional message fields, with basic required-field
and format checks.

The details can be posted to a Slack incoming webhook, but only when a
webhook is configured *and* the send is confirmed in the prompt. Without a
configuration file the form stays offline and nothing is transmitted. Signups
are not stored locally, and the website's physics challenge is not
implemented; email and date validation are basic format checks, not
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
classic dialog. `Tab` and `Shift+Tab` still move between fields.

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

**Send indmeldelse** validates the form, asks for confirmation, and then posts
the fields as a Slack message. The request runs on a worker thread, so a slow
connection does not freeze the window. The status line reports the outcome:
`Indmeldelse sendt til Slack.`, `Slack svarede HTTP <code>`, or a WinINet
error code such as `Kunne ikke sende til Slack. Fejl 12175`.

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
