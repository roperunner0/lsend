# lsend — LocalSend for the CardputerZero

LocalSend protocol v2.1 client in a single Python file (standard library only).
Works with the official LocalSend apps on Android, iOS, Windows, macOS and Linux.

## Install
```sh
scp -r lsend/ pi@<cardputer-ip>:~      # or copy via ADB / SD card
cd ~/lsend && sh install.sh            # add --service for an always-on receiver
```
Needs `python3` and `openssl` (both are in stock Raspberry Pi OS). If `openssl` is missing, it falls back to plain HTTP.
In that case, turn off **Encryption** in the phone app's settings.

## Use
| Command | What it does |
|---|---|
| `lsend` | Full-screen TUI |
| `lsend receive` / `receive -y` | Headless receiver (prompt / auto-accept) |
| `lsend send a.jpg dir/ --to Pixel` | Send files/folders (alias substring, IP, or IP:port) |
| `lsend send --text "hi" --to Pixel` | Send a text message |
| `lsend scan` | List devices |

Global flags: `--alias NAME`, `--pin 1234` (require PIN to receive), `--send-pin 1234`, `--dir PATH`, `--http`, `--port`.

### TUI keys
| Key | Action |
|---|---|
| ↑/↓ (or Fn+`;` / Fn+`.`) | Move |
| Enter / → | Pick device → file browser → send file |
| Backspace / ← | Parent folder |
| Esc | Back |
| `t` | Send text to selected device |
| `a` | Send the whole current folder |
| `H` | Show hidden files |
| `r` | Rescan |
| `y` / `n` | Accept / reject incoming |
| `q` | Quit |

Received files go to `~/Downloads/LocalSend`. Incoming text messages pop up on screen.

## Config (optional) `~/.config/lsend/config.json`
```json
{"alias": "Gummy-CZ", "downloadDir": "~/Downloads/LocalSend", "pin": null, "deviceType": "desktop"}
```

## Notes
- Discovery is UDP multicast `224.0.0.167:53317`, with a /24 HTTP sweep as fallback (press `r`).
  Some routers and guest networks block multicast; the sweep still finds devices.
- Only one program can hold TCP 53317. Stop the service before running the TUI, or give one of them `--port`.
