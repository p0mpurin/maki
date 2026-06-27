const http = require("http");
const crypto = require("crypto");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawn } = require("child_process");

const PORT = Number(process.env.PORT || 8787);
const ALLANIME_BASE = "https://api.allanime.day/api";
const REFERER = "https://youtu-chan.com";
const USER_AGENT =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:150.0) Gecko/20100101 Firefox/150.0";
const SOURCE_QUERY_HASH =
  "d405d0edd690624b66baba3068e0edc3ac90f1597d898a1ec8db4e5c43c00fec";
const CACHE_ROOT = path.join(__dirname, "cache", "jobs");

fs.mkdirSync(CACHE_ROOT, { recursive: true });

const jobs = new Map();

function json(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    "Content-Type": "application/json",
    "Content-Length": Buffer.byteLength(payload),
    "Access-Control-Allow-Origin": "*",
  });
  res.end(payload);
}

function decryptTobeparsed(value) {
  const blob = Buffer.from(value, "base64");
  if (blob.length <= 29) throw new Error("encrypted payload too short");
  const key = crypto.createHash("sha256").update("Xot36i3lK3:v1").digest();
  const iv = Buffer.concat([blob.subarray(1, 13), Buffer.from([0, 0, 0, 2])]);
  const cipherText = blob.subarray(13, blob.length - 16);
  const decipher = crypto.createDecipheriv("aes-256-ctr", key, iv);
  return JSON.parse(Buffer.concat([decipher.update(cipherText), decipher.final()]).toString("utf8"));
}

async function allanimePost(body) {
  let lastErr = null;
    for (let attempt = 0; attempt < 4; attempt++) {
    try {
      const controller = new AbortController();
      const to = setTimeout(() => controller.abort(), 15000);
      const response = await fetch(ALLANIME_BASE, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "User-Agent": USER_AGENT,
          Referer: REFERER,
          Origin: REFERER,
        },
        body: JSON.stringify(body),
        signal: controller.signal,
      });
      clearTimeout(to);
      if (!response.ok) throw new Error(`AllAnime HTTP ${response.status}`);
      const payload = await response.json();
      if (payload.data && payload.data.tobeparsed) return decryptTobeparsed(payload.data.tobeparsed);
      return payload.data ? payload.data : payload;
    } catch (err) {
      lastErr = err;
      if (attempt < 3) await new Promise(r => setTimeout(r, 1000 * (attempt + 1)));
    }
  }
  throw lastErr || new Error("AllAnime request failed");
}

function episodeCount(availableEpisodes) {
  if (!availableEpisodes) return 0;
  if (typeof availableEpisodes === "number") return availableEpisodes;
  return availableEpisodes.sub || availableEpisodes.dub || availableEpisodes.raw || 0;
}

async function search(q) {
  const data = await allanimePost({
    query:
      "query($search:SearchInput$limit:Int){shows(search:$search limit:$limit){edges{_id name availableEpisodes}}}",
    variables: { search: { query: q }, limit: 20 },
  });
  return {
    results: (data?.shows?.edges || []).map((item) => ({
      id: item._id,
      name: item.name,
      episode_count: episodeCount(item.availableEpisodes),
    })),
  };
}

async function episodes(id) {
  const data = await allanimePost({
    query: "query($id:String!){show(_id:$id){availableEpisodesDetail}}",
    variables: { id },
  });
  const detail = data?.show?.availableEpisodesDetail || {};
  return { episodes: detail.sub || detail.dub || detail.raw || [] };
}

async function rawSources(id, episode) {
  const data = await allanimePost({
    variables: { showId: id, translationType: "sub", episodeString: episode },
    extensions: { persistedQuery: { version: 1, sha256Hash: SOURCE_QUERY_HASH } },
  });
  return (data?.episode?.sourceUrls || []).map((item) => ({
    name: item.sourceName || item.source || "source",
    url: item.sourceUrl,
    type: item.type || "",
  }));
}

function pickDirectSource(sources) {
  return (
    sources.find((s) => s.name === "Yt-mp4" && /^https?:\/\//.test(s.url)) ||
    sources.find((s) => s.type === "player" && /^https?:\/\//.test(s.url)) ||
    sources.find((s) => /^https?:\/\/.*\.(mp4|m3u8)(\?|$)/i.test(s.url))
  );
}

const PROFILES = {
  "800hq": [
    "-vf", "scale=-2:240,pad=800:240:(800-iw)/2:0,fps=24",
    "-c:v", "libx264", "-preset", "fast", "-tune", "fastdecode", "-profile:v", "high", "-level:v", "3.1",
    "-crf", "22", "-x264-params", "bframes=0:ref=4",
  ],
  "800mq": [
    "-vf", "scale=-2:240,pad=800:240:(800-iw)/2:0,fps=24",
    "-c:v", "libx264", "-preset", "fast", "-tune", "fastdecode", "-profile:v", "baseline", "-level:v", "3.0",
    "-crf", "24", "-x264-params", "bframes=0:ref=4",
  ],
  "800lq": [
    "-vf", "scale=-2:240,pad=800:240:(800-iw)/2:0,fps=24",
    "-c:v", "libx264", "-preset", "veryfast", "-tune", "fastdecode", "-profile:v", "baseline", "-level:v", "3.0",
    "-crf", "26", "-x264-params", "bframes=0:ref=4",
  ],
  "400hq": [
    "-vf", "scale=400:240:force_original_aspect_ratio=increase,crop=400:240,fps=24",
    "-c:v", "libx264", "-preset", "fast", "-tune", "fastdecode", "-profile:v", "high", "-level:v", "3.0",
    "-crf", "20", "-x264-params", "bframes=0:ref=4",
  ],
  "400mq": [
    "-vf", "scale=400:240:force_original_aspect_ratio=increase:flags=lanczos,crop=400:240,unsharp=3:3:0.35:3:3:0.0,fps=24,format=yuv420p",
    "-c:v", "libx264", "-preset", "superfast", "-tune", "fastdecode", "-profile:v", "baseline", "-level:v", "3.0",
    "-crf", "21", "-x264-params", "bframes=0:ref=3:deblock=-1,-1",
  ],
  o3ds: [
    "-vf", "scale=400:trunc(ow/a/2)*2,pad=400:240:(ow-iw)/2:(oh-ih)/2,fps=24",
    "-c:v", "libx264", "-preset", "ultrafast", "-tune", "fastdecode", "-profile:v", "baseline", "-level:v", "3.0",
    "-crf", "26", "-x264-params", "bframes=0:ref=2",
  ],
};

function sessionKey(id, episode) {
  return crypto.createHash("sha1").update(`${id}:${episode}`).digest("hex").substring(0, 16);
}

async function getHLSDuration(urlStr, headers = {}) {
  try {
    const res = await fetch(urlStr, { headers });
    if (!res.ok) return 0;
    const text = await res.text();
    
    // Check if it is a master playlist
    if (text.includes("#EXT-X-STREAM-INF")) {
      const lines = text.split("\n");
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i].trim();
        if (line && !line.startsWith("#")) {
          const resolvedUrl = new URL(line, urlStr).toString();
          return await getHLSDuration(resolvedUrl, headers);
        }
      }
    }
    
    let total = 0;
    const matches = text.matchAll(/#EXTINF:\s*(\d+\.?\d*)/g);
    for (const match of matches) {
      total += parseFloat(match[1]);
    }
    return total;
  } catch (e) {
    console.error("Failed to parse HLS duration:", e);
    return 0;
  }
}

async function estimateDuration(urlStr) {
  const headers = {
    "User-Agent": USER_AGENT,
    "Referer": REFERER,
    "Origin": REFERER
  };
  
  if (urlStr.includes(".m3u8")) {
    const hlsDur = await getHLSDuration(urlStr, headers);
    if (hlsDur > 0) return hlsDur;
  }
  
  return new Promise((resolve) => {
    const ffprobe = spawn("ffprobe", [
      "-v", "error",
      "-show_entries", "format=duration",
      "-of", "default=noprint_wrappers=1:nokey=1",
      "-user_agent", USER_AGENT,
      "-headers", `Referer: ${REFERER}\r\nOrigin: ${REFERER}\r\n`,
      urlStr
    ]);
    
    let out = "";
    ffprobe.stdout.on("data", (d) => { out += d.toString(); });
    ffprobe.on("close", (code) => {
      const val = parseFloat(out.trim());
      if (code === 0 && !isNaN(val) && val > 0) {
        resolve(val);
      } else {
        resolve(1440); // 24 minutes default fallback for anime
      }
    });
  });
}

async function runJob(jobId) {
  const job = jobs.get(jobId);
  if (!job) return;

  try {
    job.state = "resolving";
    const sources = await rawSources(job.showId, job.episode);
    const direct = pickDirectSource(sources);
    if (!direct) throw new Error("no direct source available");

    job.state = "downloading";
    const profile = PROFILES[job.quality] || PROFILES["800mq"];
    const dir = path.join(CACHE_ROOT, jobId);
    fs.mkdirSync(dir, { recursive: true });
    const h264Path = path.join(dir, "video.h264");
    const pcmPath = path.join(dir, "audio.pcm");
    const metaPath = path.join(dir, "meta.json");

    /* audio: mono PCM; 16000Hz keeps speech clear without huge downloads */
    const pcmRate = 16000;
    const pcmChannels = 1;

    console.log(`[JOB] Estimating duration for job ${jobId}...`);
    let durationSec = await estimateDuration(direct.url);
    console.log(`[JOB] Estimated duration for job ${jobId}: ${durationSec} seconds`);
    const expectedAudioSize = Math.ceil(durationSec * pcmRate * pcmChannels * 2);

    const args = [
      "-hide_banner", "-loglevel", "info", "-stats", "-y",
      "-threads", "0",
      "-user_agent", USER_AGENT,
      "-headers", `Referer: ${REFERER}\r\nOrigin: ${REFERER}\r\n`,
      "-i", direct.url,
      ...profile,
      "-an", "-f", "h264", h264Path,
      "-vn", "-acodec", "pcm_s16le", "-ar", String(pcmRate), "-ac", String(pcmChannels),
      "-f", "s16le", pcmPath,
    ];

    const child = spawn("ffmpeg", args, { stdio: ["ignore", "pipe", "pipe"] });

    job.state = "transcoding";
    job.progress = 0;
    job.audioReady = false;   // audio download can start once 60s encoded
    job.videoReady = false;   // video served only after full encode

    // Unlock audio quickly; the endpoint streams the growing PCM until ffmpeg exits.
    const AUDIO_EARLY_START_SECS = 8;

    let stderrBuffer = "";
    let currentSec = 0;
    child.stderr.on("data", (chunk) => {
      stderrBuffer += chunk.toString();

      if (durationSec <= 0) {
        const durMatch = stderrBuffer.match(/Duration:\s*(\d+):(\d+):(\d+)/);
        if (durMatch) {
          durationSec =
            parseInt(durMatch[1]) * 3600 +
            parseInt(durMatch[2]) * 60 +
            parseInt(durMatch[3]);
        }
      }

      const timeMatches = stderrBuffer.match(/time=(\d+):(\d+):(\d+)/g);
      if (timeMatches && durationSec > 0) {
        const lastTime = timeMatches[timeMatches.length - 1];
        const timeMatch = lastTime.match(/time=(\d+):(\d+):(\d+)/);
        if (timeMatch) {
          currentSec =
            parseInt(timeMatch[1]) * 3600 +
            parseInt(timeMatch[2]) * 60 +
            parseInt(timeMatch[3]);
          job.progress = Math.min(0.99, currentSec / durationSec);
        }
      }

      // Early-start: unlock audio download after 60s.
      // Video is NOT served yet — MP4 is still being written and cannot be demuxed.
      // The client (STATE_DOWNLOAD_AUDIO) will download audio while encoding continues.
      // By the time audio finishes downloading, encoding is much further along or done.
      if (!job.audioReady && currentSec >= AUDIO_EARLY_START_SECS) {
        job.audioReady = true;
        job.state = "streaming";
        // Video URL is set but size=0 so the client knows to wait for video separately.
        // Report the expected final PCM size so client progress remains sane.
        job.audio_size = expectedAudioSize;
        job.audio_rate = pcmRate;
        job.audio_channels = pcmChannels;
        // Placeholder video info — real size reported on "ready"
        job.url = `/cache/${jobId}/video.h264`;
        job.size = fs.existsSync(h264Path) ? fs.statSync(h264Path).size : 0;
        console.log(`[JOB] ${jobId} audio early-start unlocked at ${currentSec}s encoded`);
      }

      if (stderrBuffer.length > 32768) {
        stderrBuffer = stderrBuffer.slice(-16384);
      }
    });

    await new Promise((resolve, reject) => {
      child.on("exit", (code) => {
        if (code !== 0) return reject(new Error(`ffmpeg exit ${code}`));
        resolve();
      });
    });

    if (!fs.existsSync(h264Path)) throw new Error("video.h264 not produced");

    const meta = {
      fps: 24,
      sample_rate: pcmRate,
      channels: pcmChannels,
      width: 400,
      height: 240,
      duration_s: Math.round(durationSec),
    };
    fs.writeFileSync(metaPath, JSON.stringify(meta));

    // Encoding fully done. Now the MP4 is complete and safe to demux.
    job.state = "ready";
    job.progress = 1;
    job.videoReady = true;
    job.url = `/cache/${jobId}/video.h264`;
    job.size = fs.statSync(h264Path).size;
    job.audio_size = fs.existsSync(pcmPath) ? fs.statSync(pcmPath).size : 0;
    job.audio_rate = pcmRate;
    job.audio_channels = pcmChannels;
  } catch (err) {
    job.state = "error";
    job.message = err.message;

  }
}

const server = http.createServer(async (req, res) => {
  req.on("error", () => {});
  res.on("error", () => {});

  try {
    const url = new URL(req.url, `http://${req.headers.host}`);
    console.log(`[REQUEST] ${req.method} ${url.pathname}${url.search}`);

    if (url.pathname === "/health") return json(res, 200, { ok: true });
    if (url.pathname === "/search") {
      const q = url.searchParams.get("q") || "";
      if (!q) return json(res, 400, { error: "missing q" });
      return json(res, 200, await search(q));
    }
    if (url.pathname === "/episodes") {
      const id = url.searchParams.get("id") || "";
      if (!id) return json(res, 400, { error: "missing id" });
      return json(res, 200, await episodes(id));
    }
    if (url.pathname === "/sources") {
      const id = url.searchParams.get("id") || "";
      const ep = url.searchParams.get("episode") || "";
      if (!id || !ep) return json(res, 400, { error: "missing id or episode" });
      const upstream = await rawSources(id, ep);
      const key = sessionKey(id, ep);
      return json(res, 200, {
        sources: [{ name: "Proxy-HLS", url: `http://${req.headers.host}/hls/${key}/index.m3u8?id=${encodeURIComponent(id)}&episode=${encodeURIComponent(ep)}`, type: "hls" }, ...upstream],
      });
    }

    if (req.method === "POST" && url.pathname === "/prepare") {
      const chunks = [];
      req.on("data", (c) => chunks.push(c));
      req.on("end", async () => {
        try {
          const body = JSON.parse(Buffer.concat(chunks).toString());
          const { id, episode, quality } = body;
          if (!id || !episode) return json(res, 400, { error: "missing id or episode" });
          const q = PROFILES[quality] ? quality : "800mq";
          const jobId = crypto.randomUUID();
          const job = { id: jobId, state: "queued", showId: id, episode, quality: q, progress: 0 };
          jobs.set(jobId, job);
          runJob(jobId);
          return json(res, 200, { job_id: jobId, state: "queued" });
        } catch (err) {
          return json(res, 400, { error: err.message });
        }
      });
      return;
    }

    if (url.pathname.startsWith("/status/")) {
      const jobId = url.pathname.split("/status/")[1];
      if (!jobId) return json(res, 400, { error: "missing job_id" });
      const job = jobs.get(jobId);
      if (!job) return json(res, 404, { error: "job not found" });
      const resp = { state: job.state, progress: job.progress };
      if (job.state === "streaming" || job.state === "ready") { resp.url = job.url; resp.size = job.size; resp.audio_size = job.audio_size; resp.audio_rate = job.audio_rate; resp.audio_channels = job.audio_channels; }
      if (job.state === "error") resp.message = job.message;
      return json(res, 200, resp);
    }

    if (url.pathname.startsWith("/cache/")) {
      const parts = url.pathname.split("/").filter(Boolean);
      if (parts.length < 3) return json(res, 400, { error: "bad path" });
      const jobId = parts[1];
      const file = parts[2];
      if (!/^[a-f0-9-]+$/i.test(jobId)) return json(res, 400, { error: "bad job_id" });

      if (file === "stream.h264" || file === "video.h264") {
        const h264Path = path.join(CACHE_ROOT, jobId, "video.h264");
        const job = jobs.get(jobId);
        res.writeHead(200, {
          "Content-Type": "video/h264",
          "Cache-Control": "no-cache",
        });

        console.log(`[VIDEO] ${jobId}: streaming raw h264 file`);
        let bytesSent = 0;
        let offset = 0;
        let closed = false;
        req.on("close", () => { closed = true; });

        while (!closed) {
          if (fs.existsSync(h264Path)) {
            const stat = fs.statSync(h264Path);
            if (stat.size > offset) {
              const stream = fs.createReadStream(h264Path, { start: offset, end: stat.size - 1 });
              await new Promise((resolve) => {
                stream.on("data", (chunk) => {
                  bytesSent += chunk.length;
                  if (!res.write(chunk)) stream.pause();
                });
                res.on("drain", () => stream.resume());
                stream.on("end", resolve);
                stream.on("error", resolve);
              });
              offset = stat.size;
            }
          }

          const latest = jobs.get(jobId);
          if (!latest || latest.state === "ready" || latest.state === "error") break;
          await new Promise((resolve) => setTimeout(resolve, 250));
        }

        console.log(`[VIDEO] ${jobId}: sent ${bytesSent} bytes`);
        if (!res.writableEnded) res.end();
        return;
      }

      if (file === "audio.pcm") {
        const fp = path.join(CACHE_ROOT, jobId, "audio.pcm");
        const job = jobs.get(jobId);
        if (!fs.existsSync(fp) && (!job || job.state === "error")) return json(res, 404, { error: "audio not found" });

        res.writeHead(200, {
          "Content-Type": "audio/raw",
          "Cache-Control": "no-cache",
        });

        console.log(`[AUDIO] ${jobId}: streaming growing pcm file`);
        let offset = 0;
        let closed = false;
        req.on("close", () => { closed = true; });

        while (!closed) {
          if (fs.existsSync(fp)) {
            const stat = fs.statSync(fp);
            if (stat.size > offset) {
              const stream = fs.createReadStream(fp, { start: offset, end: stat.size - 1 });
              await new Promise((resolve) => {
                stream.on("data", (chunk) => {
                  if (!res.write(chunk)) stream.pause();
                });
                res.on("drain", () => stream.resume());
                stream.on("end", resolve);
                stream.on("error", resolve);
              });
              offset = stat.size;
            }
          }

          const latest = jobs.get(jobId);
          if (!latest || latest.state === "ready" || latest.state === "error") break;
          await new Promise((resolve) => setTimeout(resolve, 250));
        }

        if (!res.writableEnded) res.end();
        return;
      }

      if (file === "meta.json") {
        const fp = path.join(CACHE_ROOT, jobId, "meta.json");
        if (!fs.existsSync(fp)) return json(res, 404, { error: "meta not found" });
        const stat = fs.statSync(fp);
        res.writeHead(200, { "Content-Type": "application/json", "Content-Length": stat.size, "Cache-Control": "max-age=3600" });
        return fs.createReadStream(fp).pipe(res);
      }

      if (!/^output\.mp4$/.test(file)) return json(res, 400, { error: "unknown file" });
      const fp = path.join(CACHE_ROOT, jobId, file);
      if (!fs.existsSync(fp)) return json(res, 404, { error: "not found" });
      const stat = fs.statSync(fp);
      res.writeHead(200, { "Content-Type": "video/mp4", "Content-Length": stat.size, "Cache-Control": "max-age=3600" });
      return fs.createReadStream(fp).pipe(res);
    }

    if (req.method === "DELETE" && url.pathname.startsWith("/cache/")) {
      const parts = url.pathname.split("/").filter(Boolean);
      if (parts.length < 2) return json(res, 400, { error: "bad path" });
      const jobId = parts[1];
      if (!/^[a-f0-9-]+$/i.test(jobId)) return json(res, 400, { error: "bad job_id" });
      const dir = path.join(CACHE_ROOT, jobId);
      if (fs.existsSync(dir)) fs.rmSync(dir, { recursive: true });
      jobs.delete(jobId);
      return json(res, 200, { ok: true });
    }

    if (url.pathname === "/play") {
      const id = url.searchParams.get("id") || "";
      const ep = url.searchParams.get("episode") || "";
      if (!id || !ep) return json(res, 400, { error: "missing id or episode" });
      const key = sessionKey(id, ep);
      const dir = path.join(CACHE_ROOT, `rgba_${key}`);
      if (fs.existsSync(path.join(dir, "ready"))) {
        const count = parseInt(fs.readFileSync(path.join(dir, "ready"), "utf8"));
        if (count > 0) return json(res, 200, { frames: count, base: `http://${req.headers.host}/frame/${key}` });
      }
      fs.mkdirSync(dir, { recursive: true });
      const sources = await rawSources(id, ep);
      const direct = pickDirectSource(sources);
      if (!direct) return json(res, 500, { error: "no direct source" });
      const child = spawn("ffmpeg", [
        "-hide_banner", "-loglevel", "warning", "-y",
        "-user_agent", USER_AGENT,
        "-headers", `Referer: ${REFERER}\r\nOrigin: ${REFERER}\r\n`,
        "-i", direct.url,
        "-vf", "scale=400:240,fps=8",
        "-vcodec", "rawvideo", "-pix_fmt", "rgba",
        "-f", "image2", path.join(dir, "frame_%05d.rgba"),
      ], { stdio: ["ignore", "ignore", "pipe"] });
      let stderr = "";
      child.stderr.on("data", (c) => { stderr += c.toString(); });
      await new Promise((resolve, reject) => {
        child.on("exit", (code) => {
          if (code !== 0) return reject(new Error(`ffmpeg exit ${code}`));
          const poll = () => {
            const files = fs.readdirSync(dir).filter(f => f.endsWith(".rgba"));
            if (files.length > 0) {
              fs.writeFileSync(path.join(dir, "ready"), String(files.length));
              resolve(files.length);
            } else setTimeout(poll, 500);
          };
          poll();
        });
      });
      const count = parseInt(fs.readFileSync(path.join(dir, "ready"), "utf8"));
      return json(res, 200, { frames: count, base: `http://${req.headers.host}/frame/${key}` });
    }
    if (url.pathname.startsWith("/frame/")) {
      const parts = url.pathname.split("/").filter(Boolean);
      const key = parts[1]; const file = parts[2];
      if (!/^[a-f0-9]{40}$/.test(key) || !/^frame_\d+\.rgba$/.test(file))
        return json(res, 400, { error: "bad path" });
      const fp = path.join(CACHE_ROOT, `rgba_${key}`, file);
      if (!fs.existsSync(fp)) return json(res, 404, { error: "not found" });
      const stat = fs.statSync(fp);
      res.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Length": stat.size, "Cache-Control": "max-age=3600" });
      return fs.createReadStream(fp).pipe(res);
    }

    return json(res, 404, { error: "not found" });
  } catch (err) {
    console.error(err);
    return json(res, 500, { error: err.message });
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`maki proxy listening on http://0.0.0.0:${PORT}`);
});
