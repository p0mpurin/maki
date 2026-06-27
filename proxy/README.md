# maki proxy

Small helper server for the 3DS client.

It handles API requests, starts ffmpeg jobs, and serves 3DS-friendly video/audio streams.

Run:

```sh
node server.js
```

Requirements:

- Node.js 18+
- ffmpeg on PATH

On the 3DS SD card, set `/3ds/maki/config.ini`:

```ini
[proxy]
base_url = http://YOUR_PC_OR_PI_IP:8787
```

Endpoints:

- `GET /health`
- `GET /search?q=one`
- `GET /episodes?id=SHOW_ID`
- `GET /sources?id=SHOW_ID&episode=1`
- `POST /prepare`
- `GET /status/:job`
- `GET /cache/:job/video.h264`
- `GET /cache/:job/audio.pcm`
