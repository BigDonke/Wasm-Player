# Wasm-Player

A browser video player for MKV and other containers the browser cannot open natively. A custom FFmpeg build in WebAssembly reads the file in a Web Worker and rewraps it into fragmented MP4. The browser then decodes the video with its own hardware decoder. FFmpeg never decodes video, so 4K HDR plays at full speed.

The wasm module is 2.0 MB, or 1.0 MB gzipped. It holds only the demuxers, audio decoders, the AAC encoder, the MP4 muxer and the subtitle decoders this player needs.

**Try it at https://bigdonke.github.io/Wasm-Player/**, or open a sample directly:

- [HEVC HDR10 with TrueHD, DTS, E-AC-3, AAC, SRT and ASS](https://bigdonke.github.io/Wasm-Player/#url=samples/hevc-hdr10-truehd-dts-eac3-subs.mkv)
- [HEVC with DTS, AAC, SRT and PGS](https://bigdonke.github.io/Wasm-Player/#url=samples/hevc-dts-pgs-3min.mkv)

Nothing is uploaded. Local files are read in the browser, and links are fetched straight from their server.

## Using it

Paste a link and press Play, pick a local file, or drop a file or link onto the page. Brave and Safari block autoplay with sound, so the player then shows a large play button. A link can also go in the address, as `#url=https://example.com/movie.mkv`.

A remote file needs two things from its server. It must answer HTTP Range requests, and it must send CORS headers (`Access-Control-Allow-Origin`). Most file hosts and CDNs do both. If the server ignores Range, the player downloads the whole file into memory before playing and shows the progress. It refuses files over 256 MB from such servers, since a tab on a phone or a low-memory device can run out of memory holding them.

To run it locally, serve the folder with any static server, for example `python -m http.server`. Browsers do not start a worker from `file://`, so opening `index.html` directly does not work.

## How it works

1. The worker reads the source on demand with synchronous range requests. Sequential reads grow to 8 MB per request. Random reads drop back to 1 MB for links and 256 KB for local files.
2. For links, the worker also downloads ahead of the demuxer between reads, with two parallel requests of up to 8 MB. It keeps 60 seconds of the file in memory, at least 32 MB and at most 256 MB. So playback continues through network drops that outlast the few seconds held in the browser's media buffers.
3. FFmpeg demuxes the container. Video packets go into a fragmented MP4 unchanged.
4. Audio goes through unchanged when the browser can play the codec. Otherwise FFmpeg decodes it and encodes AAC, with up to 5.1 channels. Chrome rejects 7.1 AAC in Media Source Extensions, so 7.1 sources are mixed down to 5.1.
5. Both MP4 streams go into Media Source Extensions with their original timestamps. Browsers cap these buffers and evict data past the cap. Chromium caps video at about 150 MB and Firefox at about 100 MB. So the player measures the stream's bitrate and keeps the video buffer under 80 MB in Chromium and 60 MB in Firefox. That is up to 45 seconds ahead for normal files and 6 to 15 seconds for a 4K Blu-ray remux. If a hole still appears, playback restarts demuxing at the playhead.
6. A seek inside the buffer is instant. A seek outside it restarts demuxing at the nearest keyframe before the target.

**HEVC in Firefox.** Firefox starts HEVC video only on an IDR frame and ignores the CRA keyframes that open-GOP encodes use. So in Firefox a seek restarts at the nearest IDR in the 30 seconds before the target. In Matroska the player checks each keyframe's type from the first 64 KB of its cluster, so the search stays fast. Without one, video resumes at the next IDR after the target. Blu-ray remuxes have an IDR every few seconds and seek normally. Many x265 encodes have an IDR only at the start, so Firefox plays them from the beginning but cannot seek in them. The player then plays audio and says so. Chromium accepts CRA keyframes and seeks normally in every file.

Switching the audio track restarts demuxing at the current position. Text subtitles switch instantly. Image subtitles (PGS, VobSub, DVB) are decoded only for the selected track, so switching to one also restarts at the current position.

## Format support

| | Supported | Notes |
|---|---|---|
| Containers | MKV, WebM, MP4/MOV, MPEG-TS/M2TS, AVI, FLV, Ogg, and raw audio files | |
| Video | H.264, HEVC (8 and 10 bit, HDR10, HLG), AV1, VP9 | The browser decodes video, so it must support the codec. MPEG-2 and VC-1 do not play. |
| Audio passthrough | AAC, MP3, Opus, FLAC, AC-3, E-AC-3 | Only where the browser supports the codec. Safari and Edge on Windows play E-AC-3. |
| Audio decoded to AAC | TrueHD, DTS (core, HD MA, HRA, DTS:X), AC-3, E-AC-3, PCM, ALAC, Vorbis, MP2, and the above where unsupported | |
| Subtitles | SRT, ASS/SSA, WebVTT, MP4 text, PGS, VobSub, DVB | ASS styling is dropped and the plain text is kept. |

**Dolby Vision.** Where the browser reports Dolby Vision support (Safari on Apple devices, Edge on some Windows machines), profiles 5 and 8 play natively with a `dvh1` track. Everywhere else, profile 8 plays its HDR10 or HLG base layer. Profile 7 always plays its HDR10 base layer, and the player strips the enhancement layer and RPU first. Profile 5 has no fallback layer, so outside Dolby Vision browsers its colors are wrong.

**Atmos and DTS:X.** No open decoder renders the objects. When the player decodes TrueHD Atmos, E-AC-3 Atmos or DTS:X, you get the channel bed, mixed down to 5.1. E-AC-3 passthrough hands Atmos to the browser unchanged.

## Keyboard

| Key | Action |
|---|---|
| Space, K | Play or pause |
| ← / → | Back or forward 5 seconds |
| J / L | Back or forward 10 seconds |
| ↑ / ↓ | Volume |
| M | Mute |
| F | Fullscreen |
| A | Next audio track |
| C | Next subtitle track |

## Files

| File | Purpose |
|---|---|
| `index.html` | Player page, controls, MSE buffering and subtitle rendering |
| `worker.js` | Range reads, read-ahead cache and the bridge to the wasm module |
| `ff.js`, `ff.wasm` | Emscripten output of `build/src/ffcore.c` linked with FFmpeg 9.0.2 |
| `samples/` | Two generated test files. One has 60 s of 1080p HEVC HDR10 with TrueHD, DTS, E-AC-3 and AAC tracks and SRT and ASS subtitles. The other has 3 min of 720p HEVC with DTS and AAC tracks and SRT and PGS subtitles. Each audio track is a sine at its own pitch. |
| `build/build.sh` | Builds `ff.js` and `ff.wasm` |
| `build/src/ffcore.c` | The C wrapper around FFmpeg that the worker calls |
| `build/tools/` | `make_samples.sh` regenerates `samples/`, and `make_sup.py` writes its PGS track |

## Building the wasm module

Run `build/build.sh` on macOS or Linux. It writes `ff.js` and `ff.wasm` to the repository root. The first run downloads Emscripten 7.0.0 into `build/emsdk` (1.9 GB) and the FFmpeg 9.0.2 source into `build/ffmpeg-9.0.2` (113 MB). It then configures and compiles FFmpeg, which takes about two minutes. Later runs only recompile `ffcore.c`. The FFmpeg component lists sit at the top of `build.sh`.

`build/tools/make_samples.sh` regenerates `samples/`. It needs `ffmpeg` with libx265, and Python for the PGS track.

## Tested

Chromium and Firefox played every sample. Both also played and seeked through 4K HDR files of up to 85 GB over HTTPS without dropped frames or stalls. That covered HEVC 10-bit in MKV, H.264 in MP4 and TS, and VP9 with Opus in WebM. Audio covered TrueHD, DTS, E-AC-3, AC-3 and AAC, and subtitles covered SRT, ASS and PGS. Chromium also played a 4K UHD Blu-ray remux with Dolby Vision profile 7 over HTTPS. It played the HDR10 base layer with TrueHD Atmos audio and PGS subtitles, and seeking stayed smooth. Safari is untested. Native Dolby Vision playback and profiles 5 and 8 were only checked on the muxer output with an injected Dolby Vision configuration.

## License

The player code (`index.html`, `worker.js`, `build/`) is MIT, see `LICENSE`.

`ff.wasm` and `ff.js` contain FFmpeg 9.0.2, which is licensed under the LGPL 2.1 or later, see `licenses/FFmpeg-LGPL-2.1.txt`. The build enables no GPL or nonfree parts. FFmpeg source is at https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, and `build/build.sh` holds the exact configuration. To use a modified FFmpeg, point `build.sh` at it and rebuild. `ff.js` also contains the Emscripten runtime (MIT) and zlib (zlib license).
