# Security and account data

Authentication is sent directly to NetEase endpoints over verified HTTPS.
The application never needs a third-party API proxy to operate.

`sdmc:/config/netease-switch/cookies.txt` is an account credential. Do not
include it in issues, screenshots, public SD backups or patches. The QR key,
authorization responses, phone numbers and SMS codes are also private.
The background service reads the session without overwriting the foreground
application's newer login. Signing out removes the local session.

The CA bundle is shared with the system module and is required for HTTPS.
Do not “fix” network errors by disabling TLS certificate or hostname checks.
Review proxy settings before providing account credentials.

Eden 0.2.1 disables certificate verification inside its emulated SSL service,
even when the application requests it. Use anonymous requests in this
emulator; authorize real accounts on the native desktop build or the console.
The exact upstream implementation is linked in `docs/EMULATOR.md`.

If a credential is disclosed, invalidate the session from the official
NetEase client and sign out locally. Report a vulnerability without attaching
working credentials or a real user's private data. This repository does not
currently advertise a dedicated security contact.
