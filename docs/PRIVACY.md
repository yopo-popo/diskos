# diskOS privacy and network disclosure

This describes network use by the installer and the diskOS UI (`mq_ui`). The UI source
is published under GPL-3.0-or-later in [`ui/`](../ui/).

## The installer (this repo)

- **`./install.sh` uses `pip`** to upgrade `pip` itself and download two packages - `pyusb` and
  `pycryptodome` - from **PyPI** into a local `.venv`. That `pip`/PyPI traffic is the only network
  activity in setup. Configure `pip` to use your own mirror or offline package files if you prefer.
- **The installer app itself makes no network requests.** Building the image, decrypting/extracting
  your firmware, saving the recovery image, and flashing over USB are all fully local. It does not
  phone home and sends no telemetry.

## The diskOS UI (`mq_ui`) on the device

diskOS is a local music player. The table below describes when each online feature sends a request. Each request reveals the device's public IP to the service it contacts.

| Feature | Endpoint | Protocol | What is sent |
|---|---|---|---|
| **Online album art** | `itunes.apple.com`, `*.mzstatic.com` | HTTPS | When Online Album Art is enabled and a playing track has no cover, artist and album or title metadata, plus a storefront country selected from the player's language, are sent to search for artwork. Matching images are downloaded from Apple's image hosts. Off by default. |
| **Lyrics** | `lrclib.net` | HTTPS | Online Lyrics is on by default. Opening Lyrics for a song without local or embedded words sends the current track's **title and artist** to find matching lyrics, if Online Lyrics is enabled. |
| **Scrobbling (Last.fm)** | `ws.audioscrobbler.com` | HTTPS | Off by default. If enabled and connected: the device sends `track.updateNowPlaying` and scrobbles with track metadata (artist/title/album/duration, plus a timestamp for scrobbles), API key and session key to **your** Last.fm account. |
| **Update check and download** | `api.github.com`, `github.com` | HTTPS | Opening Update diskOS checks the latest release. The 1.2.1 release image includes the diskOS release key by default; confirming a supported update downloads its signed app bundle. Switching Allow diskOS Updates off prevents download and staging but still permits a release check. |
| **Last.fm sign-in** | `ws.audioscrobbler.com/2.0/`, `www.last.fm/api/auth/`, `www.last.fm/api/account/create` | HTTPS | The device POSTs `auth.getToken` and polls `auth.getSession` at `https://ws.audioscrobbler.com/2.0/` with the API key and authorization token. It displays a QR approval URL; your phone opens that URL. The local setup page also links to `www.last.fm/api/account/create` for obtaining an API key. |

### Last.fm credential setup happens over your LOCAL network in plaintext

To connect Last.fm, diskOS runs a **temporary web server on the device** at
`http://<device-wifi-ip>:8080/<random-token>/`, shows a QR for it, and your phone posts your Last.fm
**API key and shared secret** to it. Important properties:

- It is **plain HTTP on your Wi-Fi LAN** - your API key/secret cross your local network unencrypted.
  Do this on a network you trust.
- The server is **transient** (runs only during setup), bound to the Wi-Fi interface, and gated by a
  random URL token.
- Your Last.fm **API key, secret, and session key are then stored on the device** under `/usr/data`
  (device config); **offline scrobbles are queued** in `/usr/data/lastfm.queue` until they can be sent.
  Nothing Last.fm-related is sent anywhere except your device, your phone (during setup), and Last.fm.

### The stock FiiO player still runs alongside diskOS

diskOS replaces only the UI; FiiO's original player process still runs underneath it. That stock
component is responsible for Bluetooth, FiiO firmware-update checks, and LAN media
endpoints (DLNA/AirPlay/Roon/QPlay-style discovery) the device may present on your local network. Those
behaviours are inherited from the stock firmware and are outside diskOS's code. On V2.57, the stock player also performs an `ipinfo.io` time-zone lookup and network time synchronization after Wi-Fi connects when Automatic Time is enabled; changing that setting takes effect after the player next starts.

## What diskOS does NOT do

- No analytics, telemetry, or usage tracking.
- It does not upload your music library, your listening history (beyond Last.fm scrobbles if you turn
  them on), or any personal files.

## Reporting

Report anything that looks like unexpected data leaving the device via the process in
[`SECURITY.md`](../SECURITY.md).
