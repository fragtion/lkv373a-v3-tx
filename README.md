# LKV373A v3 TX Proxy

**Stream directly to the Lenkeng LKV373A v3 RX (Receiver) - no TX/Transmitter required**

`lkv373tx` is a software replacement for the *transmitter* half of the Lenkeng
LKV373A v3 HDMI-over-IP extender. Point it at the LKV373A **receiver**
unit and it will drive the receiver's HDMI output from anything ffmpeg can
produce: a screen grab, a file, an RTSP camera, a test pattern, a live
encoder — at the resolution and refresh rate you choose.

Under normal circuimstances, the lkv373a v3 won't play a standard MPEG-TS stream; 
it expects a modified control table, a constant-rate "genlocked" delivery, and a
link-level heartbeat. A plain `ffmpeg ... udp://` stream makes a v3 receiver
lock up, stutter, or sit on a black screen. `lkv373tx` reproduces what the
real transmitter does, byte-for-byte, so the receiver behaves exactly as it
does with genuine hardware.

> This is an independent reverse-engineering project. It is not affiliated
> with Lenkeng or ITE Tech.

---

## Contents

- [What you need](#what-you-need)
- [Build / install](#build--install)
- [Quick start](#quick-start)
- [The two modes](#the-two-modes)
- [Required and key arguments](#required-and-key-arguments)
- [Understanding `--mode` and `--fps`](#understanding---mode-and---fps)
- [The HDMI mode table](#the-hdmi-mode-table)
- [Recommended encoder settings](#recommended-encoder-settings)
- [Examples](#examples)
- [Full option reference](#full-option-reference)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [The reverse-engineering, in brief](#the-reverse-engineering-in-brief)
- [Credits](#credits)
- [License](#license)

---

## What you need

- An **LKV373A v3 receiver** unit on your network. Its factory-default IP is
  `192.168.1.12`; you will pass that (or whatever you have set) as `--dst`.
- **ffmpeg** on your `PATH` (any reasonably recent build).
- For `lkv373tx.py`: **Python 3.7+**. No third-party Python packages.
- For `lkv373tx.c`: a C compiler. No external libraries beyond libc + pthreads.

Two interchangeable implementations are provided. They behave identically —
same arguments, same output on the wire:

| File | Use it when |
|---|---|
| `lkv373tx.py` | You just want to run it. Zero build step. |
| `lkv373tx.c`  | Low-powered or headless hosts, no Python available, embedding into other software, or pushing high bitrates where you want the leanest possible pacer. |

---

## Build / install

```sh
git clone https://github.com/fragtion/lkv373a-v3-tx
cd lkv373a-v3-tx
```

**Python** — nothing to build:

```sh
python3 lkv373tx.py --help
```

**C** — one command:

```sh
cc -O2 -Wall -Wextra -o lkv373tx lkv373tx.c -lpthread -lm
./lkv373tx --help
```

The rest of this README writes `lkv373tx` for brevity; substitute
`python3 lkv373tx.py` if you are using the Python version.

---

## Quick start

Send a 1080p test pattern with a tone to a receiver at `192.168.1.12`:

```sh
lkv373tx --dst 192.168.1.12 --mode 20 --fps 30 \
  --ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=30 \
           -f lavfi -i sine=frequency=1000:sample_rate=48000 \
           -map 0:v -map 1:a \
           -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
           -x264-params cabac=0:ref=1:bframes=0:aud=1 \
           -c:a mp2 -b:a 128k -ar 48000 -ac 2
```

The receiver should switch its HDMI output to 1080p60 and show the test
pattern. (Yes — `--mode 20` is 1080p**60** but you encode **30** fps. That is
correct and explained in detail [below](#understanding---mode-and---fps).)

---

## The two modes

`lkv373tx` reads an MPEG-TS stream, splices in the control table, paces it,
and sends it to the receiver. You can give it that stream in two ways.

### ffmpeg mode (recommended)

Put `--ffmpeg` last on the command line; everything after it is the ffmpeg
command. `lkv373tx` runs ffmpeg for you and consumes its output directly.

```sh
lkv373tx --dst <RX-IP> --mode <N> --fps <F> --ffmpeg <ffmpeg args ...>
```

- The ffmpeg **binary is optional**. If the first token after `--ffmpeg`
  starts with `-`, it is treated as an ffmpeg option and `ffmpeg` from `PATH`
  is used. Give a name or path as the first token to use a specific build.
- You supply only **input and encoding** options. The **output section**
  (`-muxrate N -f mpegts pipe:1`) is appended automatically — do not add it
  yourself.

### pipe mode

Run ffmpeg yourself and pipe MPEG-TS into `lkv373tx` on stdin:

```sh
ffmpeg ... -f mpegts -muxrate 12000000 pipe:1 | lkv373tx --dst <RX-IP> --mode <N> --fps <F>
```

In pipe mode you are responsible for the ffmpeg output section. Always include
`-muxrate` (see [How it works](#how-it-works) — the control table needs the
null packets that `-muxrate` produces).

---

## Required and key arguments

| Argument | Required? | Notes |
|---|---|---|
| `--dst <ip>` | **Yes** | The receiver's IP address (or a multicast group). |
| `--mode <0-33>` | Strongly recommended | The HDMI output format the receiver produces. Default `14` (1080p30). |
| `--fps <rate>`  | Strongly recommended | The frame rate you actually encode. Lets the tool configure frame handling automatically. |

`--dst` is the only argument the program will refuse to start without.
Everything else has a default — but a stream is only correct if `--mode`
matches the resolution/refresh you want and `--fps` matches what your encoder
produces. Treat all three as mandatory in practice.

---

## Understanding `--mode` and `--fps`

This is the part that trips everyone up. Read it once and it is simple.

The LKV373A was built as a **dumb HDMI extender**: the transmitter box takes a
real HDMI cable, and the receiver box reproduces that exact signal at the far
end. So the receiver firmware thinks in terms of an **HDMI output mode** — a
fixed resolution + refresh rate + scan type, e.g. *1080p60*.

`lkv373tx` keeps that model but exposes it as **two independent software
knobs**:

- **`--mode`** picks the HDMI signal the *receiver* outputs — what the TV or
  monitor plugged into the receiver actually sees. (Full table below.)
- **`--fps`** is the frame rate you are *actually encoding* in ffmpeg.

**They are deliberately not the same number**, and here is why.

The original transmitter hardware never sends full-rate video. To fit a
100 Mbit link, its encoder runs at **half** the HDMI refresh rate and sets a
"skip-frame" flag in the stream telling the receiver to **show each decoded
frame twice**. A real 1080p**60** HDMI input therefore leaves the box as a
**30 fps** H.264 stream, and the receiver doubles it back to a 60 Hz output.
We confirmed this against packet captures of real transmitters fed 60 Hz,
59.94 Hz and 50 Hz signals — all three sent half-rate video.

The "Hz" baked into a mode name is thus really the receiver's **display
refresh** — a remnant of the extender's HDMI-passthrough origin. The *stream*
underneath runs at half that.

### Pairing `--fps` with your encoder

You do **not** set the skip-frame flag by hand. You tell `lkv373tx` the
`--mode` you want and the `--fps` you encode, and it derives the flag. There
are two valid pairings:

| | `--fps` you encode | What the receiver does |
|---|---|---|
| **Half-rate** (default, what real hardware does) | mode refresh **÷ 2** | doubles every frame back up to the mode rate — lowest bandwidth |
| **Full-rate** | mode refresh **× 1** | shows frames 1:1 — true full-rate motion, roughly double the bitrate |

For the common progressive modes, the half-rate path means:

| `--mode` refresh | encode at `--fps` |
|---|---|
| 60 Hz / 59.94 | 30 |
| 50 Hz | 25 |
| 30 Hz / 29.97 | 15 |
| 25 Hz | 12.5 |
| 24 Hz / 23.976 | 12 |

So: **set 60 Hz, send 30 fps. Set 50 Hz, send 25 fps. Set 30 Hz, send
15 fps** — and so on. Or encode at the full mode rate and `lkv373tx` switches
to the 1:1 path automatically.

If `--fps` is neither the mode's rate nor exactly half of it, the tool stops
and tells you the two rates that *would* work. If you omit `--fps` entirely it
assumes the half-rate path and prints a warning.

> **Interlaced modes** (`480i`, `576i`, `1080i`, …): the number in the table
> is the *field* rate; whole frames arrive at half that. `--mode 5` (576i50)
> wants `--fps 25`. The tool handles the arithmetic — just pass the real
> encoded frame rate.

### The receiver as a transcoder

Because `--mode` and `--fps` are decoupled software values — there is no real
HDMI cable forcing them — you can combine them however you like. Feed a
1080p source and select a 720p mode: the receiver's hardware scaler outputs
720p.

---

## The HDMI mode table

`--mode N` selects one row. `width`/`height` is the receiver's output
resolution; `refresh` is its output refresh (field rate for interlaced rows).

| N | Resolution | Refresh | Scan | N | Resolution | Refresh | Scan |
|--:|---|---|---|--:|---|---|---|
| 0 | 640×480 | 60 | p | 17 | 1920×1080 | 59.94 | i |
| 1 | 720×480 | 59.94 | i | 18 | 1920×1080 | 59.94 | p |
| 2 | 720×480 | 59.94 | p | 19 | 1920×1080 | 60 | i |
| 3 | 720×480 | 60 | i | **20** | **1920×1080** | **60** | **p** |
| 4 | 720×480 | 60 | p | 21 | 800×600 | 60 | p |
| 5 | 720×576 | 50 | i | 22 | 1024×768 | 60 | p |
| 6 | 720×576 | 50 | p | 23 | 1280×768 | 60 | p |
| 7 | 1280×720 | 50 | p | 24 | 1280×800 | 60 | p |
| 8 | 1280×720 | 59.94 | p | 25 | 1280×960 | 60 | p |
| **9** | **1280×720** | **60** | **p** | 26 | 1280×1024 | 60 | p |
| 10 | 1920×1080 | 23.976 | p | 27 | 1360×768 | 60 | p |
| 11 | 1920×1080 | 24 | p | 28 | 1366×768 | 60 | p |
| 12 | 1920×1080 | 25 | p | 29 | 1440×900 | 60 | p |
| 13 | 1920×1080 | 29.97 | p | 30 | 1400×1050 | 60 | p |
| **14** | **1920×1080** | **30** | **p** | 31 | 1440×1050 | 60 | p |
| 15 | 1920×1080 | 50 | i | 32 | 1600×900 | 60 | p |
| 16 | 1920×1080 | 50 | p | 33 | 1680×1050 | 60 | p |

Common picks: **`20`** = 1080p60, **`14`** = 1080p30, **`9`** = 720p60,
**`16`** = 1080p50. Modes 21–32 are PC/VESA resolutions.

---

## Recommended encoder settings

`lkv373tx` does not encode — it only frames and paces. The receiver's hardware
decoder is simple and is happiest with a stream shaped like the real
transmitter's. When you write your ffmpeg command:

- **Video:** H.264 **baseline** / constrained-baseline, `yuv420p`, **no
  B-frames**, CABAC off, AUD on. With libx264:
  `-c:v libx264 -profile:v baseline -pix_fmt yuv420p -x264-params cabac=0:ref=1:bframes=0:aud=1`
  With VAAPI: `-c:v h264_vaapi -profile:v constrained_baseline`.
- **Audio:** MP2 or MP3, 48 kHz, stereo (`-c:a mp2 -b:a 128k -ar 48000 -ac 2`).
  AAC does **not** work with the receiver's decoder. Audio is optional.
- **CBR / `-muxrate`:** the stream must carry MPEG-TS null packets (the
  control table is spliced over them) and must be near-constant bitrate so the
  receiver's clock recovery stays locked. In **ffmpeg mode** `lkv373tx` adds
  `-muxrate` for you (`--muxrate`, default 10 Mbit/s). In **pipe mode** add it
  yourself. Keep `-muxrate` comfortably above your video+audio bitrate.

---

## Examples

**Transcode an RTSP camera to 720p60 on the receiver:**

```sh
lkv373tx --dst 192.168.1.12 --mode 9 --fps 30 \
  --ffmpeg -rtsp_transport tcp -i rtsp://user:pass@192.168.1.50/stream \
           -an -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
           -x264-params cabac=0:ref=1:bframes=0:aud=1 \
           -r 30 -g 30 -b:v 8M -maxrate 8M -bufsize 16M
```

**Stream a file at native 1080p60 (full-rate, 1:1):**

```sh
lkv373tx --dst 192.168.1.12 --mode 20 --fps 60 \
  --ffmpeg -re -i movie.mp4 \
           -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
           -x264-params cabac=0:ref=1:bframes=0:aud=1 -r 60 \
           -c:a mp2 -b:a 128k -ar 48000 -ac 2
```

**Pipe mode (you run ffmpeg):**

```sh
ffmpeg -re -i movie.mp4 \
  -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
  -x264-params cabac=0:ref=1:bframes=0:aud=1 -r 30 \
  -c:a mp2 -b:a 128k -ar 48000 -ac 2 \
  -f mpegts -muxrate 12000000 pipe:1 \
| lkv373tx --dst 192.168.1.12 --mode 20 --fps 30
```

**Multicast** (pass `--rx-ip` so the on-screen TX-IP announcement still has a
destination):

```sh
lkv373tx --dst 239.255.42.42 --rx-ip 192.168.1.12 --mode 14 --fps 15 --ffmpeg ...
```

Press **Ctrl+C** to stop; in ffmpeg mode ffmpeg is shut down with it.

---

## Full option reference

```
--dst IP             Receiver IP (unicast) or multicast group.   [required]
--port N             UDP port. Default 5004 (the real TX value).
--mode N             HDMI output-mode index, 0–33. Default 14 (1080p30).
--fps F              Frame rate you actually encode. Sets frame handling
                     automatically; pass it instead of --flags.
--sit-width N        Override control-table width  (default: width of --mode).
--sit-height N       Override control-table height (default: height of --mode).
--audio-rate N       Audio sample rate written to the control table (48000).
--muxrate BPS        TS mux rate; in ffmpeg mode also passed to ffmpeg.
                     Pacing fallback until the rate is measured. Default 1e7.
--sit-period S       Seconds between control-table injections. Default 0.2.
--prebuffer N        Datagrams buffered before paced output starts. Default 24.
--heartbeat-ms F     Idle-gap threshold for keep-alive datagrams. Default 1.0.
--tail-beats N       Keep-alive datagrams sent after the stream ends. Default 20.
--rx-ip IP           Receiver IP for the on-screen TX-IP packet
                     (defaults to --dst when --dst is unicast).
--osd-ip IP          IP advertised as the transmitter (default: auto-detected).
--no-osd             Disable the TX-IP announcement on UDP port 6000.
--ttl N              Multicast TTL. Default 16.
--extra N            Raw control-table "extra" value (default: from --mode).
--flags 0xNN         Raw control-table flags byte (expert override of --fps).
--byte15 0xNN        Raw control-table audio byte. Default 0xF0.
--dump-sit           Print the constructed control section as hex and run.
--version            Print version and exit.
--ffmpeg ARGS...     Run ffmpeg internally; everything after this is the
                     ffmpeg command. Must come last.
```

For almost all use you only touch `--dst`, `--mode`, `--fps` and `--ffmpeg`.
`--extra`, `--flags` and `--byte15` are raw escape hatches for experimentation
and are derived correctly from `--mode`/`--fps` on their own.

---

## How it works

```
  source ─▶ ffmpeg ──H.264 + MP2 in MPEG-TS──▶ lkv373tx ──UDP :5004──▶ LKV373A RX ─▶ HDMI
                                                  │
                                    · splices the SIT control table (PID 0x77)
                                    · paces output to a constant, PCR-locked rate
                                    · fills idle gaps with keep-alive datagrams
                                    · announces the TX IP on UDP :6000
```

A v3 receiver needs three things a normal MPEG-TS stream does not provide, and
`lkv373tx` supplies all three:

1. **The SIT control table.** The receiver takes its entire HDMI output
   configuration — resolution, refresh, scan type, audio — from a proprietary
   PSI section on **PID 0x77**, not from the H.264 stream itself. Without a
   regular SIT the receiver never reconfigures and you get a black screen or a
   stale, mis-sized picture. `lkv373tx` builds this section (from `--mode` and
   friends) and splices it into the stream a few times a second by overwriting
   MPEG-TS null packets — which is why the stream must carry null packets, and
   why `-muxrate` matters.

2. **Constant-rate delivery.** The receiver runs a closed-loop genlock: it
   trims its own display clock to track the incoming stream's rate. Hand it a
   bursty stream (which is what `ffmpeg | udp://` produces — a frame's worth of
   data, then nothing) and the loop never settles; it drifts and eventually
   resyncs to black. `lkv373tx` buffers the burst and meters datagrams out at a
   steady rate measured from the stream's own PCR clock.

3. **A link heartbeat.** The real transmitter never lets UDP port 5004 fall
   silent — roughly every millisecond it sends either a 1316-byte data
   datagram (7 × 188-byte TS packets) or, if the encoder has nothing ready, an
   empty 0-byte datagram. The receiver's input path depends on that. `lkv373tx`
   reproduces the heartbeat exactly.

It also sends the small UDP packet on port **6000** that makes the receiver
display the transmitter's IP in its on-screen menu.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Black screen, no lock | Control table not landing. Ensure the stream is CBR with null packets — add/raise `-muxrate`. Confirm `--dst` and the network path. |
| Plays then stutters / pauses | `--fps` does not match `--mode`. Re-check the [pairing table](#pairing---fps-with-your-encoder). Make sure the encode is genuinely CBR. |
| Picture is the wrong size or offset | `--sit-width`/`--sit-height` must equal the resolution you actually encode. By default they follow `--mode`; override them if your encode differs. |
| `no data datagrams sent` on exit | ffmpeg failed to start or produced nothing — read the `[ffmpeg]` lines above it. |
| Receiver shows no transmitter IP | OSD announcement disabled or undeliverable. With a multicast `--dst`, also pass `--rx-ip`. |
| A VESA mode (21–32) shows "invalid signal" | The monitor on the receiver does not advertise that timing in its EDID. Try it on a PC monitor, or pick a standard mode. |
| `--mode 33` never works | 1680×1050 is not supported by the receiver firmware. Avoid it. |

---

## Limitations

- **AAC audio** is not supported by the receiver; use MP2 or MP3.
- The Python pacer is comfortable up to roughly 35 Mbit/s; for higher rates,
  or on very weak CPUs, use the C build.
- `lkv373tx` does not transcode. Shape the stream correctly in ffmpeg (see
  [Recommended encoder settings](#recommended-encoder-settings)).
- Tested against the **v3** firmware generation
  (`IPTV_RX_PKG_v0_5_0_0_20170103`). Older LKV373A revisions use a different
  protocol and are out of scope.

---

## The reverse-engineering, in brief

This tool is the product of unpacking both the transmitter and receiver
firmware (ITE9x3x / ARM, packed in `ITEPKG033` containers with UCL-compressed
sections) and decoding nanosecond-resolution packet captures of real
transmitter hardware, then checking each against the other.

Highlights:

- The receiver runs an FFmpeg-derived MPEG-TS demuxer with an ITE back-end. It
  hardcodes a section filter on **PID 0x77** whose callback pulls width,
  height, an HDMI **mode index**, audio parameters and a set of flag bits out
  of a private PSI section and uses them to program its HDMI transmitter and
  ISP scaler.
- One flag bit — **skip-frame** — tells the receiver to display each decoded
  frame twice. The real transmitter sets it whenever it is encoding at half
  the HDMI refresh, which it always does for 50/60 Hz inputs. This is the
  mechanism behind the `--mode`/`--fps` split.
- The `extra` field is the refresh rate × 1000; the receiver divides it back
  out and uses it as the reference frequency for its display-clock genlock.
- The transmitter's streaming engine is LIVE555 emitting bare MPEG-TS over UDP
  via `BasicUDPSink` in 1316-byte datagrams, with a ~1 ms keep-alive heartbeat
  so the link is never silent.

---

## Credits

Built on the broader LKV373A reverse-engineering community's prior work, and
on the `otl-lkv373a-tools` project, whose `smazdec` UCL decompressor made the
firmware images readable.

Not affiliated with, endorsed by, or supported by Lenkeng or ITE Tech.
"LKV373A" and "Lenkeng" are used only to identify the hardware this tool
interoperates with.

---

## License

MIT License. See [`LICENSE`](LICENSE) for details.

> **Note:** This license does not apply to any components embedded within the firmware which may be licensed under the GPL, proprietary licenses, or other terms.

---

## Contributing

Pull requests, forks, issues and suggestions are all welcome.

---

## Support

If this project has been useful to you, consider buying me a coffee:

**PayPal:** [![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate/?business=2CGE77L7BZS3S&no_recurring=0)  
**BTC:** `1Q4QkBn2Rx4hxFBgHEwRJXYHJjtfusnYfy`  
**XMR:** `4AfeGxGR4JqDxwVGWPTZHtX5QnQ3dTzwzMWLBFvysa6FTpTbz8Juqs25XuysVfowQoSYGdMESqnvrEQ969nR9Q7mEgpA5Zm`
