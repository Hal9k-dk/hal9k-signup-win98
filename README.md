# hal9k indmeldelse — offline demo

A classic Windows-style 32-bit Win32 form inspired by the Danish hal9k
physics signup page. It includes the name, email, address, birth date,
telephone, membership and optional message fields, with basic required-field
and format checks.

This is an **offline demo**: it does not transmit or save data, create a
membership, or implement the website's physics challenge. Email and date
validation are basic format checks, not verification of real addresses or dates.

## Build

Install the MinGW-w64 cross-compiler, then run:

```sh
make
```

To build a CD image as well (requires `xorriso`):

```sh
make hal9k.iso
```

The ISO contains `HAL9K.EXE` and `AUTORUN.INF`. Mount it as a CD in a
Windows 98 virtual machine or burn it to a CD. AutoRun depends on Windows
and drive settings; if it does not start, open `HAL9K.EXE` manually. The ISO
is not bootable. `make clean` removes the generated executable, resource
object and ISO.

The executable icon is adapted from `https://www.hal9k.dk/favicon.ico`;
`favicon-original.ico` is the downloaded original and `hal9k.ico` is its
classic 8-bit Windows-compatible conversion.

The build bypasses the C runtime and links only to Windows system libraries;
it uses ANSI Win32 APIs and targets i386. The executable's imports and PE
headers can be inspected with `i686-w64-mingw32-objdump -p hal9k.exe`.
Windows 98 compatibility has not yet been verified on an actual Windows 98
installation or emulator.
