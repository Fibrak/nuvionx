# NuvioNX tools (PC side)

Helper scripts that run on **Windows** (PowerShell). They talk to NuvioNX over your local Wi-Fi
while the app is open on the Switch, or produce a file you copy to the SD card.

| File | What it does |
|---|---|
| `import-from-windows.ps1` | Exports your **Nuvio for Windows** setup (addons, library + Trakt watchlist, watch progress, watched history) and sends it to NuvioNX, or writes a file for the SD card. |
| `import-to-my-switch.cmd` | Double-click shortcut that runs the import using the IP in `switchip.txt`. |
| `switchip.txt` | Your Switch's IP address, read by the scripts. Edit this when your IP changes. |
| `make-icon.ps1` | Regenerates `../icon.jpg` (the NRO icon) from a logo + an "NX" badge. |

---

## First: set your Switch IP

1. On the Switch, open NuvioNX → **Addons** tab. It shows `http://<ip>:8080`.
2. Put just the IP (e.g. `192.168.1.50`) on the last line of **`switchip.txt`**.
   - `http://192.168.1.50:8080` also works — the script strips the extra parts.
   - A `#` starts a comment.

The IP can change after a router reboot; if the import "can't connect", re-check the Addons tab and
update `switchip.txt`.

---

## Import your Nuvio-for-Windows setup

Requires the official **Nuvio for Windows** app installed on the same PC (data is read from
`%APPDATA%\Nuvio`). NuvioNX must be **open** on the Switch, same Wi-Fi.

**Easiest:** double-click **`import-to-my-switch.cmd`**.

**Or run it directly:**
```powershell
# Uses switchip.txt
powershell -ExecutionPolicy Bypass -File import-from-windows.ps1

# Or give the IP explicitly (overrides switchip.txt)
powershell -ExecutionPolicy Bypass -File import-from-windows.ps1 -SwitchIp 192.168.1.50

# Or write a file instead of sending over Wi-Fi, then copy it to
# sdmc:/switch/NuvioNX/nuvio-import.json and start NuvioNX:
powershell -ExecutionPolicy Bypass -File import-from-windows.ps1 -OutFile nuvio-import.json
```

What transfers: **addons** (order + enabled state), **library + Trakt watchlist**, **watch
progress**, **watched history**. Re-running never creates duplicates.

What does **not** transfer: debrid keys embedded in addon URLs come across automatically; separate
debrid API keys and account logins do not — set those up on the Switch (Account / Settings tabs).
**Trakt sign-in is not transferable** (Trakt ties it to the app that created it): on the Switch do
**Account → Trakt → Connect Trakt** with your own Trakt API app (Client ID; a Secret isn't needed
for newer PKCE apps — create one at <https://trakt.tv/oauth/applications/new>, redirect URI
`urn:ietf:wg:oauth:2.0:oob`).

Options: `-Profile <n>` picks a Nuvio profile; `-NuvioDir <path>` points at a non-default data
folder.

---

## Other ways to get data onto the Switch (no PC script)

With NuvioNX open, from any browser on the same Wi-Fi:

- `http://<switch-ip>:8080` — paste addon manifest URLs.
- `http://<switch-ip>:8080/account` — paste a Nuvio sync server URL + publishable key, and/or a
  Trakt Client ID.

---

## Regenerate the icon

```powershell
powershell -ExecutionPolicy Bypass -File make-icon.ps1
```
Writes `../icon.jpg` (256×256). Rebuild the NRO afterwards so it picks up the new icon.
By default it reads the logo from a local NuvioTV checkout; pass `-Logo <path>` to use another image.

---

## Privacy note

`import-from-windows.ps1` reads data that may contain your debrid keys (inside addon URLs). The
generated `nuvio-import.json` is git-ignored. Don't commit it or share it.
