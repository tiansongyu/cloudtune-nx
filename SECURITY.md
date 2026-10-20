# CloudTune NX security and account data

Authentication is sent directly to NetEase endpoints over verified HTTPS.
The application never needs a third-party API proxy to operate.

`sdmc:/config/netease-switch/accounts.json` contains all locally saved account
credentials. `cookies.txt` mirrors the active session. Both files, their backups
and staging files must stay private; never include them in issues, screenshots,
public SD backups or patches. QR keys and authorization responses are private too.
QR scanning is the only supported sign-in method.
The background service reads the session without overwriting the foreground
application's newer login. Removing an account erases its local saved session;
other accounts remain saved. Switching accounts clears the old music queue before
publishing the new active cookie jar. On desktop the files are written with mode
0600; FAT SD storage has no equivalent per-user permissions. These are local
session stores, not encrypted vaults.
The directory keeps its original name so CloudTune NX can reuse existing
sessions after the project rename; it is not a stale installation artifact.

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
The canonical repository is
[tiansongyu/cloudtune-nx](https://github.com/tiansongyu/cloudtune-nx);
never publish working credentials in its public issues or pull requests.
