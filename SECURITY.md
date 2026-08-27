# Security policy

express-ghalib is firmware for a single-user, offline device. It has no server
component, no accounts, and no network listeners. The realistic risk surface is
small but not empty: the device parses untrusted files from a microSD card
(text, WAV, dictionary index), speaks BLE HID to a paired keyboard, and stores
Wi-Fi credentials in NVS.

## Supported versions

Only the tip of `main` is supported. There are no release branches and no
backported fixes.

## Reporting a vulnerability

Report privately through GitHub's **[Report a
vulnerability](https://github.com/anubhabbehera/express-ghalib/security/advisories/new)**
form (Security tab → Advisories). Please do not open a public issue for
something that is exploitable.

Include, where you can:

- what an attacker controls (SD card contents, BLE peer, USB serial, radio),
- the affected file/function,
- a crash log or a minimal reproducer file.

This is a hobby project maintained by one person. Expect a first response
within a couple of weeks, not a couple of hours.

## Scope

In scope:

- memory-safety bugs reachable from data the device did not author — SD card
  files, BLE HID reports, NTP responses, serial input;
- anything that discloses stored Wi-Fi credentials off-device;
- flashing/build tooling in `tools/` that executes untrusted input.

Out of scope:

- physical attacks on an unlocked device (there is no lock screen — by design);
- the device sending plaintext NTP over your own LAN;
- Wi-Fi credentials being recoverable from a flash dump by someone holding the
  board (documented in `docs/build-flash-debug.md`);
- denial of service that requires already having the board in hand.

## Secrets

No credentials belong in this repository. Wi-Fi credentials are entered on the
device and live in NVS. `.gitignore` blocks the usual credential filenames
(`secrets.h`, `wifi_secrets.h`, `.env`, `*.pem`, `id_rsa*`); if you add a new
one, add it there too. Push protection for secret scanning is enabled on this
repository — if it blocks your push, rotate the key rather than bypassing.
