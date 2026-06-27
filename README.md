# maki

Anime streaming client for Nintendo 3DS.

The 3DS app talks to a small Node.js proxy. The proxy handles the API work and transcodes video/audio into a format the 3DS can play.

## Setup

### Proxy

```sh
cd proxy
node server.js
```

Requires Node.js 18+ and `ffmpeg` on PATH.

### 3DS

Copy `maki.3dsx` to `/3ds/maki/maki.3dsx` on your SD card.

Copy `config.example.ini` to `/3ds/maki/config.ini` and set your proxy URL:

```ini
[proxy]
base_url = http://YOUR_PROXY_HOST:8787

[player]
quality = 400mq
delete_after_play = true
```

### DSP firmware

Audio needs DSP firmware dumped on the SD card.

1. Open the Luma3DS Rosalina menu: `L + Down + Select`
2. Miscellaneous options -> Dump DSP firmware
3. Reboot

## Controls

| Button | Action |
|--------|--------|
| **A** | Search / Select / Confirm |
| **B** | Back / Stop playback |
| **X** | Settings |
| **Y** | Clean all episode files from SD |
| D-Pad | Navigate lists |
| Start | Exit |

## Architecture

```
3DS Client                    Proxy (Node.js)               AllAnime API
──────────                    ───────────────               ────────────
   ├─POST /prepare───────────────►├─GET sources─────────────────►│
   │◄─job_id──────────────────────┤                              │
   │                              ├─ffmpeg transcode (bg)┐      │
   │                              │  video.h264 + audio.pcm      │
   │   [10-20s later: streaming]  │◄─────────────────────┘      │
   ├─GET /cache/:job/video.h264──►├─raw H.264 stream            │
   │◄─chunked H.264 stream────────┤                              │
   ├─GET /cache/:job/audio.pcm───►├─serve raw PCM               │
   │◄─PCM stream───────────────────┤                              │
   ├─MVD decode + NDSP play                                      │
```

- Video: H.264 -> MVD hardware decoder -> top screen
- Audio: raw PCM -> NDSP
- Proxy quality: fixed `400mq`

## Build

Requires [devkitPro](https://devkitpro.org/) with `3ds-dev`:

```sh
make -j4
```

Output: `maki.3dsx`
