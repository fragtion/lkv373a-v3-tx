# LKV373A v3 — HDMI-over-IP Extender: Reverse-Engineering Reference

A consolidated technical description of the **Lenkeng LKV373A v3** HDMI
extender pair — how the transmitter and receiver work, the wire protocol
between them, the structure of their firmware images, and the device-specific
discoveries made by unpacking both firmwares and decoding packet captures of
real hardware.

This document is about the **devices**. A separate software transmitter was
built to validate every claim here end-to-end; its usage is documented apart
from this file and is out of scope.

**Sources behind this document.** Both firmware packages
(`IPTV_TX_PKG_v4_0_0_0_20170103.PKG`, `IPTV_RX_PKG_v0_5_0_0_20170103.PKG`)
were unpacked and statically disassembled (ARM, via capstone), and four
nanosecond-resolution packet captures of genuine hardware were decoded and
cross-checked against the firmware:

| Capture | Source |
|---|---|
| `pcap-realtx.pcap` | real TX, 1080p60 HDMI input |
| `pcap-realtx480i5994.pcap` | real TX, 480i HDMI input |
| `pcap-realtx576i5000.pcap` | real TX, 576i50 HDMI input |
| `pcap-injector.pcap` | the software transmitter, for comparison |

All firmware addresses below are in the **decompressed** images at load
base `0x00000000`.

---

## DISCLAIMER

As with most of this repo, this document is AI-assisted/generated. Everything in this document is a best-effort/approximation - accuracy should be taken with a pinch of salt and is in no way guaranteed. Use at your own risk - proceed at your own risk and discretion; I will not be held liable for any loss/damage resulting from your use of this project.

---

## 1. Overview — how the device pair works

The LKV373A is a two-box HDMI extender. The **transmitter (TX)** takes an HDMI
signal, encodes it, and sends it over a 100 Mbit Ethernet network; the
**receiver (RX)** decodes it and reproduces an HDMI signal at the far end.
Multiple receivers can listen to one transmitter (it can multicast).

```
HDMI source ─▶ [ LKV373A TX ] ─▶ Ethernet / UDP ─▶ [ LKV373A RX ] ─▶ HDMI display
```

Both boxes are built on the same **ITE ITE9x3x** SoC family (ARM926EJ-S core)
and run a small **RTOS** — not Linux. Each device's entire application is a
single statically-linked ARM firmware image; the streaming libraries are
linked *into* that image rather than existing as separate programs (see §2).

End to end, a frame's journey is:

1. The TX's hardware encoder turns the HDMI input into an **H.264** video
   elementary stream and an **MPEG-1 Layer II (MP2)** audio elementary stream.
2. The TX multiplexes those into an **MPEG-TS** transport stream and adds a
   proprietary control table — the **SIT** — that tells the RX what HDMI
   output to produce.
3. The TS is sent as **bare UDP** (no RTP) on port 5004, paced at a constant
   bitrate, and the link is kept continuously "warm" with a sub-millisecond
   heartbeat.
4. The RX demuxes the TS with an **FFmpeg-derived** demuxer, decodes H.264 in
   the ITE "Jedi" hardware decoder, runs a closed-loop **genlock** that trims
   its own display clock to the incoming stream, and drives the HDMI output
   through an ITE **ISP scaler**.

Three mechanisms make the v3 generation different from a generic
"MPEG-TS over UDP" stream, and all three must be reproduced for the RX to
behave:

- **The SIT** — a private PSI section on PID `0x77`. The RX takes its entire
  HDMI output configuration (resolution, refresh, scan type, audio) from this
  table, *not* from the H.264 bitstream. No SIT → the RX never reconfigures.
- **Constant-rate (genlocked) delivery** — the RX trims its display clock to
  the stream's arrival rate. Bursty delivery makes the loop drift until it
  resyncs the decoder to a black screen.
- **The link heartbeat** — the TX never lets UDP port 5004 fall silent; every
  idle ~1 ms slot gets a zero-length datagram. The RX's receive path depends
  on that invariant.

---

## 2. Firmware images — packaging and structure

### 2.1 The container format

A factory `.PKG` update file is an ITE **ITEPKG033** container (magic
`ITEPKG033.1.1` at offset 0). It nests:

```
ITEPKG033 package
  └─ one or more SMEDIA02 sections
       └─ one SMAZ blob per section
            └─ chunked UCL-compressed data  ──►  raw ARM firmware image
```

- **SMEDIA02** — a flashable section. The 16-byte header after the magic is
  three big-endian 32-bit words — a setup-script offset, the section's
  SMAZ-blob offset, and the blob length — followed by a 12-byte key
  `"456789ABCDEF"`. The "setup script" at `SMEDIA02+0x70` is a NAND-controller
  register table used at flash time, not code.
- **SMAZ** — ITE's wrapper around **UCL** compression (UCL by Markus F.X.J.
  Oberhumer; the NRV/UPX family). The 8 bytes before the `SMAZ` magic carry a
  CRC and the total decompressed length; the blob is a series of independently
  UCL-compressed chunks.

The packages are **not encrypted.** Early analysis guessed "scrambled / high
entropy" — the entropy is simply UCL compression. Decompressed, the images are
ordinary ARM binaries.

### 2.2 Section map

| Device | Section | SMEDIA02 @ | SMAZ @ | Decompressed | Contents |
|---|---|---|---|---|---|
| TX | 0 | `0x00004c` | `0x000b80` | 327,680 B | bootloader (ARM) |
| TX | 1 | `0x025160` | `0x025c94` | 2,490,368 B | **main CPU firmware** (ARM926EJ-S) |
| TX | 2 | `0x15db85` | `0x15eae1` | 2,637,308 B | data partition — `jedi.rom` + assets |
| RX | 0 | `0x00004c` | `0x000bb0` | 5,619,712 B | **full RX firmware** (ARM32) |

The RX package additionally has an **unencrypted tail** starting at file
offset `0x275844` (618,866 B) holding the web-UI assets (jQuery 1.8.3, the
login page) and the default `iptv.ini` / `multicast.ini`.

### 2.3 What the images are — and are not

The TX `sec0`/`sec1` and the RX `sec0` images begin with the ARM exception
vector table (`b8 ff 9f e5` = `ldr pc,[pc,#-8]`), confirming a load base of
`0x00000000`: every literal-pool pointer equals its own file offset, which is
what makes static analysis tractable.

These are **flat, headerless, monolithic ARM images** — no ELF wrapper, no
sections, just code and data from address 0. There is **no embedded
filesystem** (a scan for squashfs, cramfs, jffs2, romfs, ubifs, yaffs, cpio,
tar found nothing) and **no separate executables** (no ELF anywhere). The
streaming stacks — LIVE555 on the TX, FFmpeg on the RX — are linked *into* the
one image as libraries. File paths that appear in the firmware
(`/jedi.rom`, `/iptv.ini`, `/user_login.html`, `/dev/info.cgi`) are string
constants resolved by a built-in virtual-filesystem layer to embedded
resource blobs; they are not entries in a mountable partition.

TX `sec2` is the exception in kind: it does not start with ARM vectors and is
~99.8% non-printable binary — a flashed resource blob (in practice `jedi.rom`,
the ITE codec co-processor image, plus binary assets), again not a filesystem.

---

## 3. The wire protocol

Everything in this section is decoded from the real-hardware captures and
corroborated against the firmware.

### 3.1 Transport — UDP port 5004

The TX→RX media flow is **bare MPEG-TS over UDP**, source port 5004 → dest
port 5004 (the TX binds 5004 as its source port). There is **no RTP** — the TX
uses LIVE555's `BasicUDPSink`, the raw-UDP sink.

Exactly **two** datagram types appear on port 5004:

| Datagram | Size | Meaning |
|---|---|---|
| MPEG-TS data | **1316 B** | exactly 7 × 188-byte TS packets |
| keep-alive | **0 B** | empty UDP datagram — a link heartbeat |

**The heartbeat is a hard invariant.** The TX runs a ≈1 ms tick and on every
tick sends *something*: a 1316-byte datagram if the encoder has data, or a
zero-length datagram if it does not. Port 5004 is never silent. Measured from
`pcap-realtx.pcap`:

- gap between any two port-5004 packets: median **0.34 ms**, p99 **1.20 ms**,
  steady-state worst ~3 ms;
- **825 empty datagrams precede the first video packet** — the TX heartbeats
  for ~862 ms while the encoder spins up;
- empties stay interleaved with data for the whole session (a 2 s mid-stream
  window held 2194 data + 1404 empty datagrams).

The empties carry no TS bytes, so they do not touch PCR or the multiplex; they
exist purely so the RX's receive path is never tested against a
silent-but-linked-up port. A stream that omits the heartbeat drives the RX
into lock-ups, per-frame pauses, drift-to-black, and an ungraceful shutdown
state that only a power-cycle clears — because an abrupt silence with the
Ethernet link still up is a path the RX firmware never exercises (unplugging
real hardware drops the *link*, a completely different and well-handled case).

### 3.2 The MPEG-TS multiplex

Standard 188-byte MPEG-TS, 7 packets per UDP datagram. The TX runs the mux at
a near-constant ~11.5 Mbps and pads with null packets (~11%) to hold CBR.

| PID | Contents | Notes |
|---|---|---|
| `0x0000` | PAT | program 256 |
| `0x0010` | NIT | network name `Private Network` |
| `0x0011` | SDT | table_id `0x42`; provider `ITE`, service `AIR_CH_521_6M` |
| `0x0077` | **SIT** | the proprietary control table — §3.3 |
| `0x0741` | proprietary control channel | stream_type `0xD6` in the PMT; not required by the RX for video |
| `0x07D1` | H.264 video | stream_type `0x1B`; **carries the PCR** |
| `0x07D2` | MP2 audio | stream_type `0x04`; present when the HDMI source has audio, otherwise declared-but-empty |
| `0x1000` | PMT | program 256 |
| `0x1FFF` | null packets | CBR padding — and the carrier the SIT is spliced over |

PCR is on the video PID, sparse (~one per 140 ms — loose but legal). PAT/PMT
recur about every 67 ms. The RX parses all of this with an FFmpeg-derived
`mpegts.c` demuxer.

### 3.3 The SIT — proprietary control table (PID 0x77)

The SIT ("Selection Information Table" name borrowed from DVB, but the layout
is ITE-proprietary) is the single most important discovery. It is a private
PSI section the TX emits on **PID 0x77** roughly every 200 ms. The RX reads
its **entire HDMI output configuration** from it — resolution, refresh, scan
type, audio. The H.264 SPS is *not* used to drive the HDMI mode; ITE put that
signalling in this side-channel so the RX can program its HDMI transmitter and
ISP scaler before the first decoded frame arrives, and can keep mode/lock
state even when video is momentarily lost.

A SIT travels in one 188-byte TS packet:

```
47 40 77 cc | 00 | <28-byte section> | FF FF … (padding)
└ TS hdr ──┘ ptr  └─ PSI section ──┘
```

The 28-byte section:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `table_id` | `0x77` |
| 1–2 | 2 | section_syntax + length | `0xB0 \| len_hi`, `len_lo`; length `0x19` (25) |
| 3–4 | 2 | table_id_extension | `0xFF 0xFF` — unused by the parser |
| 5 | 1 | version byte | `0xC0 \| (version<<1) \| 0x01`; RX reads `(byte5>>1)&0x1F` |
| 6–7 | 2 | section / last-section number | `0x00 0x00` |
| **8** | 1 | **flags** | bitfield — see below |
| **9–10** | 2 | **width** | big-endian pixels |
| **11–12** | 2 | **height** | big-endian pixels |
| **13–14** | 2 | **info** | HDMI output-mode index, 0–33 (§3.4) |
| **15** | 1 | **audio** | low nibble = audio codec; high nibble ignored |
| 16–19 | 4 | sample_rate | big-endian Hz |
| **20–23** | 4 | **extra** | big-endian; refresh rate × 1000 (§3.4) |
| 24–27 | 4 | CRC-32 | MPEG-2 PSI CRC, poly `0x04C11DB7` |

**The flags byte** (offset 8) decodes as five sub-fields. This was confirmed
two independent ways in the RX firmware — by the bit-by-bit reassembly the
parser performs, and by the argument order of its `#SIT` debug log:

| Bit(s) | Field | Meaning |
|---|---|---|
| 7 | `hdcp` | HDCP-active flag |
| 6 | `skipframe` | frame-doubling control — see §3.4 |
| 5 | `resolution` | parsed and cached; no branch acts on 0 vs 1 |
| 4 | `lock` | TX input signal-lock flag |
| 3–0 | `source` | 4-bit input-source enum |

**The audio byte** (offset 15) — the RX uses only the **low nibble** (it masks
`& 0x0F`); the high nibble is discarded. Low nibble `0` = MPEG/MP2. The real
TX sends `0xF0` (high nibble `0xF` ignored, codec `0`).

The three real-TX captures, decoded byte-for-byte:

| Capture | flags | width×height | info | audio | sample_rate | extra |
|---|---|---|---|---|---|---|
| 1080p60 | `0x5F` | 1920×1080 | 20 | `0xF0` | 48000 | 59997 |
| 480i (→ 480i60) | `0x1F` | 720×480 | 3 | `0xF0` | 48000 | 59997 |
| 576i50 | `0x1F` | 720×576 | 5 | `0xF0` | 48000 | 50050 |

`0x5F` = hdcp 0, **skipframe 1**, resolution 0, lock 1, source 0xF.
`0x1F` = hdcp 0, **skipframe 0**, resolution 0, lock 1, source 0xF.

`table_id` is `0x77` and section byte 1 is `0xB0` — an early guess of `0x7F` /
`0xF0` was wrong; the captures settled it.

### 3.4 The display-mode model — `info`, `extra`, and skip-frame

#### The mode table

The `info` field indexes a 34-entry HDMI output-mode table baked into the RX
firmware. `width`/`height` in the SIT is the receiver's output resolution;
the table's refresh is its output refresh (field rate for interlaced rows).

| info | Resolution | Refresh | Scan | info | Resolution | Refresh | Scan |
|--:|---|---|---|--:|---|---|---|
| 0 | 640×480 | 60 | p | 17 | 1920×1080 | 59.94 | i |
| 1 | 720×480 | 59.94 | i | 18 | 1920×1080 | 59.94 | p |
| 2 | 720×480 | 59.94 | p | 19 | 1920×1080 | 60 | i |
| 3 | 720×480 | 60 | i | 20 | 1920×1080 | 60 | p |
| 4 | 720×480 | 60 | p | 21 | 800×600 | 60 | p |
| 5 | 720×576 | 50 | i | 22 | 1024×768 | 60 | p |
| 6 | 720×576 | 50 | p | 23 | 1280×768 | 60 | p |
| 7 | 1280×720 | 50 | p | 24 | 1280×800 | 60 | p |
| 8 | 1280×720 | 59.94 | p | 25 | 1280×960 | 60 | p |
| 9 | 1280×720 | 60 | p | 26 | 1280×1024 | 60 | p |
| 10 | 1920×1080 | 23.976 | p | 27 | 1360×768 | 60 | p |
| 11 | 1920×1080 | 24 | p | 28 | 1366×768 | 60 | p |
| 12 | 1920×1080 | 25 | p | 29 | 1440×900 | 60 | p |
| 13 | 1920×1080 | 29.97 | p | 30 | 1400×1050 | 60 | p |
| 14 | 1920×1080 | 30 | p | 31 | 1440×1050 | 60 | p |
| 15 | 1920×1080 | 50 | i | 32 | 1600×900 | 60 | p |
| 16 | 1920×1080 | 50 | p | 33 | 1680×1050 | 60 | p |

Each `info` value is a full CEA-style video identity:
**resolution + scan type + nominal refresh + exact-vs-NTSC-fractional**. The
apparent duplicates (e.g. 1 vs 3, 8 vs 9) are the `/1.001` family: 59.94 Hz
(NTSC-derived) and 60.000 Hz exact are distinct timings the firmware labels
separately, as are 30/29.97 and 24/23.976. A full sweep of all 34 values
plays correctly except **mode 33** (1680×1050), which the RX firmware does not
support under any settings.

#### `extra` and the genlock reference

The `extra` field is the **refresh rate × 1000**. The RX parser converts the
big-endian uint32 to float, divides it by the float constant `1000.0`, and
stores the result as the reference frequency for its display-clock genlock
loop. The captures confirm it tracks the input refresh and is a live
measurement, not a constant: a 60 Hz-class input yields `59997`, a 50 Hz input
yields `50050` (note the non-round values). It is **not** a 90 kHz tick count
and not a frame period.

#### Skip-frame — and why "Hz" is a display-side number

The LKV373A was designed as a dumb HDMI extender, so the receiver firmware
thinks in terms of an HDMI *output* mode. But the TX hardware never sends
full-rate video: to fit a 100 Mbit link it encodes at **half** the HDMI
refresh and sets the SIT `skipframe` flag, which tells the RX to **display
each decoded frame twice** (2:2 pulldown). All three captures confirm it — the
coded video runs at ~30 fps for the 60 Hz-class inputs and ~25 fps for the
50 Hz input, i.e. half the refresh:

| Capture | display refresh | coded video fps | skipframe |
|---|---|---|---|
| 1080p60 | 60 | ~30 | 1 |
| 480i60 | 60 fields | ~30 | 0 |
| 576i50 | 50 fields | ~25 | 0 |

The general rule, with **D** the mode's displayed frame rate (the refresh for
progressive modes, the field rate ÷ 2 for interlaced):

```
coded_fps × (2 if skipframe else 1)  =  D
```

So the "Hz" in a mode is really the receiver's *display* refresh — a remnant
of the HDMI-passthrough design — while the encoded stream beneath it runs at D
or D/2. The receiver also performs hardware resolution scaling, so the SIT
`width`/`height`, the `info` mode, and the encoded picture are three
independent values; a stream can legitimately carry any combination.

### 3.5 OSD / TX-info announcement — UDP port 6000

So the receiver can show the transmitter's IP in its on-screen menu, the TX
sends a small UDP packet to **RX port 6000** about every 1.08 s. It is **14
bytes**:

```
01 00 | c0 a8 0a d3 | 58 1b | 59 1b | 5b 1b | 5a 1b
└msg┘   └─ TX IP ─┘   7000    7001    7003    7002
```

- `01 00` — message id (TX-announce);
- 4 bytes — TX IP, network byte order (`inet_aton`), shown on the RX OSD;
- 4 × uint16 **little-endian** — TX service ports 7000, 7001, 7003, 7002
  (RTSP / UART tunnel / IR / control). There is **no trailing field** — an
  earlier 18-byte decode with a 4-byte tail was wrong.

On the RX this is handled by `_TxInfoRecvThread` / `_TxInfoRecvCallback`,
which binds UDP port 6000 (the constant `0x1770` sits next to the thread name
in the firmware).

### 3.6 Discovery / pairing

On start-up the TX emits a single multicast probe to **228.67.43.91:15947**
(a `hostIdTest` packet). This is pairing/discovery only and is not part of the
media path.

### 3.7 Video and audio elementary streams

The H.264 the TX produces, decoded from the SPS/PPS/bitstream in the captures:

| Parameter | Value |
|---|---|
| Profile | **Constrained Baseline** (`profile_idc` 66, `constraint_set1`) |
| Level | 3.1 at 720p, 4.0 at 1080p (the RX does not hard-enforce level) |
| Entropy coding | **CAVLC** (no CABAC) |
| B-frames | none (`pic_order_cnt_type` 2) |
| Reference frames | **1** |
| Slices / frame | 1 |
| Scan | **progressive** (`frame_mbs_only_flag` 1) — interlaced inputs are deinterlaced before encode |
| GOP | closed, IDR every ~1 s; SPS+PPS repeated at each IDR |
| Per access unit | AUD + `pic_timing` SEI + slice |
| PES | PTS only, no DTS, `PES_packet_length` = 0 |

It is a deliberately *simple* encoder profile — the shape a small hardware
decoder likes. Audio is **MPEG-1 Layer II (MP2)**, 48 kHz, 128 kbps, declared
on PID `0x07D2` whenever the HDMI source carries audio. AAC strings exist in
the firmware but the working audio path is MPEG; AAC is not usable.

One detail worth noting from the 576i50 capture: that interlaced stream is
field-pair coded — two PES units per frame sharing a PTS (25 frames/s ⇒
50 fields/s) — whereas the 60 Hz-class captures are ordinary
one-PES-per-frame progressive H.264.

---

## 4. TX — transmitter firmware

*(Decompressed image: `sec1`, 2,490,368 B, ARM926EJ-S, base 0x0. Built from
the ITE IPTV TX SDK; SDK path fragment `…/Products/EX36/…/ITE_IPTV_TX_SDK/`,
build date 3 Jan 2017.)*

### 4.1 What the firmware is

The TX streaming engine is **LIVE555** (Streaming Media library v0.74, dated
2012.05.17) with an ITE-custom source class:

- **`IteAirTsStreamSource`** — ITE's LIVE555 source. Pulls the H.264 + MP2
  elementary streams from the hardware encoder and feeds the multiplexer. Its
  session description string is *"Ite Air Transport Stream, streamed by the
  LIVE555 Media Server"*.
- **`MPEG2TransportStreamMultiplexor`** / **`MPEG2TransportStreamFromESSource`**
  — the TS multiplexer.
- **`BasicUDPSink`** — LIVE555's raw-UDP sink. This is why the wire format is
  bare MPEG-TS over UDP with no RTP header.
- A full **`RTSPServer`** for a unicast/control path, plus the fire-and-forget
  multicast path to `239.255.42.42:5004`.

### 4.2 Streaming pipeline and key functions

```
HDMI input ─▶ ITE hardware H.264 + MP2 encoder
                     │  elementary streams
                     ▼
         IteAirTsStreamSource          (LIVE555 custom source)
                     │
                     ▼
   MPEG2TransportStreamMultiplexor  ──  PAT/PMT/SDT/NIT, SIT on PID 0x77,
                     │                  null-packet CBR padding
                     ▼
              BasicUDPSink             1316-byte datagrams
                     │
                     ▼
        UDP :5004 ─▶ RX               + ~1 ms keep-alive heartbeat
                                       + OSD packet to RX:6000 every ~1.08 s
```

| Address | Role |
|---|---|
| `0x001fb8` | `_Live555Thread` entry |
| `0x001fe8` | call into the streaming function — multicast path |
| `0x00202c` | call into the streaming function — unicast path |
| `0x01b124` | stop-stream |
| `0x01b29c` | start-stream |
| `0x01b588` | streaming function |
| `0x01bdcc` | `BasicUDPSink::createNew` — `maxPayloadSize` arg `0x524` = **1316** |
| `~0x01b4xx` | streaming-setup; the 1316 datagram-size constant lives here, beside the `"video"`, `"MP2T"`, `"play"` strings |
| `0x0032dc` | streaming watchdog (see §4.4) |
| `0x212748` | 256-entry MPEG-2 PSI CRC-32 table (poly `0x04C11DB7`) |

The datagram size **1316** (7 × 188) is a literal constant — the TX **only
ever emits full 1316-byte data datagrams**; a short final datagram is never
produced.

### 4.3 The heartbeat

The behaviour in §3.1 — never letting port 5004 go silent — is the defining
TX trait. The ~1 ms tick emits a TS datagram when the encoder has data and a
zero-length datagram when it does not. This is *the* invariant a software
transmitter must reproduce; a stream that merely forwards TS data and goes
silent between frames will not survive on a real RX.

### 4.4 The streaming watchdog

At `0x0032dc` the firmware runs a watchdog that polls a device handle
(`0x1e00`, command `0x2c` = HDMI-input status) on a 5-second timeout
(`0x1388` ms). On timeout it logs `"main.c timeout"` / `"multicast reset
stream"` and restarts the stream. This is HDMI-input-loss handling and is not
involved in the RX-side symptoms.

### 4.5 Default configuration (`iptv.ini`)

From the package's unencrypted region:

```ini
[tcpip]   ipaddr=192.168.1.11
[video]   videoout_brate_fhd=15000  videoout_brate_hd=12000  videoout_brate_sd=4000
[audio]   audio_type=0_MPEG  audio_sprate=48000  audio_brate=128
[stream]  udp=y  rtp=n  multicast=y  unicast=n
          mcastaddr=239.255.42.42  port=5004
          key=123456789abcdef03456789abcdef012
```

The TX defaults to multicast UDP on `239.255.42.42:5004`; `rtp=n` confirms raw
UDP (no RTP) is the default. Target bitrates: 15 Mbps FHD, 12 Mbps HD, 4 Mbps
SD. The 32-character `key` is also present in the RX config; the captured TS
payloads are plain, unscrambled H.264/MP2, so with the default key the field
appears to act as an identifier rather than an encryption key.

### 4.6 The SIT, transmitter side

The TX builds and emits the SIT described in §3.3. Its field values are a
snapshot of the HDMI input: `width`/`height` from the input timing, `info`
the matching mode index, `flags` carrying the input's lock/HDCP/skip state,
`extra` the measured refresh × 1000. The SIT content was confirmed
byte-for-byte against all three captures; the exact builder function inside
`sec1` was not pinned down by disassembly because the captured output already
fully specifies it.

---

## 5. RX — receiver firmware

*(Decompressed image: `sec0`, 5,619,712 B, ARM32, base 0x0. ITE IPTV RX SDK;
SDK path fragment `E:/Products/EX36/Foxun_release_20161120/ITE_IPTV_RX_SDK/`,
build Tue 3 Jan 2017 11:44:52.)*

### 5.1 What the firmware is

The RX is built around a stripped-down **FFmpeg `libavformat`/`libavcodec`**
(FFmpeg source paths survive as `__FILE__` strings) handling the network input
and TS demux, plus an ITE proprietary back-end (`hdmitx_drv.c`,
`hdmitx_sys.c`, `mmpIsp*`) driving the HDMI transmitter and ISP scaler. H.264
is decoded by the ITE "Jedi" hardware decoder.

Threads identified by their name strings:

| Thread | Role |
|---|---|
| `_UdpRecvThread` / `_MulticastRecvThread` | UDP / multicast receiver |
| `_TxInfoRecvThread` / `_TxInfoRecvCallback` | TX-info / OSD packets (port 6000) |
| `_RecvSwitchThread` | reacts to stream/mode changes |
| `_UartTxThread` | UART pass-through |
| `_PeriodicalCheckThread` | periodic state check |

The RX subscribes to a stream by one of two URL forms — `tsmulti://@<ip>:<port>`
(raw multicast TS) or `rtsp://@<ip>/channel.airts` — and in both cases the
bytes are parsed by FFmpeg's `mpegts.c` demuxer. Its default IP is
`192.168.1.12`; its config has no `videoout_*` block, confirming the HDMI
output is configured entirely from the stream.

### 5.2 Receive and display flow

```
UDP :5004 ─▶ _UdpRecvThread / _MulticastRecvThread
                    │  MPEG-TS
                    ▼
        FFmpeg-derived mpegts.c demuxer
          │                              │
 PID 0x77 │ section                video │ PID 0x07D1
          ▼                              ▼
 sit_cb @ 0x2dd5bc                 Jedi H.264 decoder
          │                              │
 state struct @ 0x67e520                 ▼
   │      │      │      │          display genlock loop
   ▼      ▼      ▼      ▼          (drift → "Display clock speed up/
 hdcp   lock  skip+  extra          slow down" → "Abnormal Resync"
 glob   glob  info     │             → terminate/restart decoder)
              │        ▼                    │
              ▼   ÷1000 ⇒ genlock            ▼
       display thread   reference rate  ITE ISP scaler + HDMI TX ─▶ HDMI out
```

### 5.3 The PID-0x77 filter and `sit_cb`

At `0x2e10fc..0x2e1158`, immediately after installing the standard
`sdt_cb` (`0x2dd098`) for PID `0x11`, the demuxer allocates a second
`MpegTSFilter` and stores `pid = 0x77`, `type = MPEGTS_SECTION`,
`section_cb = 0x2dd5bc`. Unlike `sdt_cb` (which checks `table_id == 0x42`),
**`sit_cb` performs no table-id check** — it accepts any PSI-style section on
PID 0x77.

`sit_cb` at **`0x2dd5bc`** parses the section as in §3.3 and writes the result
into a global state struct at **`0x67e520`**:

| state offset | field |
|---|---|
| `+0x14` | composite flag byte — bits 0-3 = the four single-bit flags, bits 4-7 = `source` |
| `+0x16` | width |
| `+0x18` | height |
| `+0x1a` | info |
| `+0x1c` | low nibble audio codec, high nibble version |
| `+0x20` | sample_rate |
| `+0x24` | extra |

A short-circuit compare in `sit_cb` skips the update when the incoming state
equals the cached one.

Key points inside `sit_cb`:

| Address | What it does |
|---|---|
| `0x2dd5f4` | reads `version_number` as `(byte5 >> 1) & 0x1F` |
| `0x2dd9c4` | splits the flags byte into hdcp(b7)/skipframe(b6)/resolution(b5)/lock(b4)/source(b3-0) |
| `0x2dd79c` | reassembles the composite flag byte for `state+0x14` |
| `0x2dd700` | reads the audio byte masked `& 0x0F` (low nibble only) |
| `0x2dd764`–`0x2dd798` | parses the big-endian `extra` uint32 |
| `0x2dd860` | the `#SIT` debug-log `printf` (its argument order independently confirms the flag-bit names) |
| `0x2dd90c`–`0x2dd91c` | `extra` → `__aeabi_i2f` (`0x3f6010`) → `__aeabi_fdiv` by `1000.0f` (constant `0x447a0000`, routine `0x3f625c`) → stored via `0x2732f8` |

The `#SIT` log format string lives at `0x482800`:

```
#SIT
#   version=%X, hdcp=%X, skipframe=%X, resolution=%X, lock=%X,
#   source=%X, width=%u, height=%u, info=%u, audio_codec=%X, sample_rate=%u
```

The parsed flags fan out to dedicated consumers: `hdcp` → `0x273178` (stored
to a global at `0x5a7db8+0x668`); `lock` → `0x2731a4` (`0x5a7db8+0x675`,
read back by the `get_video_lock` path); `skipframe` packed with `info` as
`info | (skipframe<<8)` and posted via `0x272d60` to the display-reconfigure
thread. `extra/1000` is stored into the display/genlock config struct at
`0x49e1c0+0x110`.

### 5.4 The genlock and resync watchdog

This loop is the heart of the v3 receiver and the reason a constant delivery
rate matters. The RX does **not** simply decode and display — its video
thread runs a closed-loop genlock that, cycle by cycle:

1. measures how full its video buffer is and how arrival timing compares to
   the stream clock;
2. keeps an Upper/Lower threshold window and a drift accumulator;
3. when drift leaves the window (threshold `0x13` = 19) it **trims the RX's
   own HDMI display clock** — *"Display clock speed up"* / *"Display clock
   slow down"* — via the routine at `0x26fc68`, to genlock onto the stream;
4. if it cannot settle it declares a *"video Gap"* and resyncs;
5. after repeated resyncs it hits *"Abnormal Resync"*, terminates and restarts
   the video thread / decoder;
6. if that keeps failing the hardware decoder wedges — black screen, a state
   only a power-cycle clears.

The reference frequency this loop locks against is `extra/1000` from the SIT.
Elsewhere the firmware logs that value as `"mode: %d, rate: %f"` and computes
`rate / nominal_mode_rate` as the display-clock trim ratio
(at `0x271a3c`; the rate is read from `0x49e1c0+0x110` at `0x2719e8`).

The loop lives roughly at `0x2d79xx`–`0x2d818x`; its diagnostic strings:

| Address | String |
|---|---|
| `0x481a10` | `videoq is %d, need to resync again` |
| `0x481b0c` | `video Gap is %d, old: %u, new: %u` |
| `0x481bb8` | `Display clock speed up` |
| `0x481bd0` | `Display clock slow down` |
| `0x481c30` | `Abnormal Resync` |

A real TX clocks its TS onto the wire from hardware at true CBR, so arrival
timing and PCR track perfectly and this loop stays settled. A stream delivered
in bursts makes the loop lurch every frame, drift, and eventually collapse —
which is exactly the failure mode described in §3.1.

### 5.5 The ISP scaler

Decoded frames are not blitted directly. They pass through the ITE ISP
scaler/compositor (`mmpIspSetVideoWindow`, `mmpIspSetDisplayWindow`,
`mmpIspSetOutputWindow`). Those windows are programmed from the SIT, not from
the H.264 SPS — so the SIT `width`/`height` must match the encoded picture or
the scaler maps the wrong source rectangle and the image renders
partial/offset even when video decodes fine.

### 5.6 Default configuration

```ini
[tcpip]      ipaddr=192.168.1.12
[multicast]  devicename=IPRX  multicast_streaming=y  osd=0
             key=123456789abcdef03456789abcdef012
[user]       name=admin  pwd=admin
```

No `videoout_*` section — the HDMI output settings come from the SIT.

---

## 6. Offset reference and flow maps

All addresses are in the decompressed images at load base `0x0`. File offsets
are marked *(file)*.

### 6.1 TX — `sec1` main firmware

| Address | Symbol / role |
|---|---|
| `0x001fb8` | `_Live555Thread` |
| `0x001fe8` / `0x00202c` | streaming-function call sites (multicast / unicast) |
| `0x01b124` | stop-stream |
| `0x01b29c` | start-stream |
| `0x01b4xx` | streaming-setup (1316-byte datagram constant; `"video"`/`"MP2T"`/`"play"`) |
| `0x01b588` | streaming function |
| `0x01bdcc` | `BasicUDPSink::createNew` (`maxPayloadSize` 0x524 = 1316) |
| `0x0032dc` | streaming watchdog (`"main.c timeout"`, 5 s = 0x1388) |
| `0x212748` | MPEG-2 PSI CRC-32 table |
| `0x00004c`/`0x025160`/`0x15db85` *(file)* | SMEDIA02 sections 0 / 1 / 2 |
| `0x000b80`/`0x025c94`/`0x15eae1` *(file)* | SMAZ blobs, sections 0 / 1 / 2 |

### 6.2 RX — `sec0` firmware

| Address | Symbol / role |
|---|---|
| `0x2dd098` | `sdt_cb` — standard SDT callback (PID 0x11, checks table_id 0x42) |
| `0x2dd5bc` | **`sit_cb`** — the PID-0x77 control-section callback |
| `0x2dd5f4` | `sit_cb`: version-number parse |
| `0x2dd700` | `sit_cb`: audio byte read (low nibble) |
| `0x2dd764`–`0x2dd798` | `sit_cb`: `extra` uint32 parse |
| `0x2dd79c` | `sit_cb`: composite flag byte assembled |
| `0x2dd860` | `sit_cb`: `#SIT` debug-log printf |
| `0x2dd90c`–`0x2dd91c` | `sit_cb`: `extra` → float → ÷1000 → store |
| `0x2dd9c4` | `sit_cb`: flags-byte split |
| `0x2e10fc`–`0x2e1158` | demuxer: PID-0x77 `MpegTSFilter` registration |
| `0x272d60` | display-reconfigure message post |
| `0x273178` | `hdcp` setter → global `0x5a7db8+0x668` |
| `0x2731a4` | `lock` setter → global `0x5a7db8+0x675` |
| `0x2719e8` | genlock: reads the `rate` reference, logs `"mode: %d, rate: %f"` |
| `0x271a3c` | genlock: `rate / nominal_mode_rate` clock-trim ratio |
| `0x2732f8` | `rate` setter → display-config struct `0x49e1c0+0x110` |
| `0x26fc68` | display-clock adjust (`"speed up"` / `"slow down"`) |
| `0x2d79xx`–`0x2d818x` | genlock / resync watchdog |
| `0x2dcd00` | XOR descrambler — not on the SIT path; unconfirmed (see §8) |
| `0x3f6010` / `0x3f625c` | `__aeabi_i2f` / `__aeabi_fdiv` |
| `0x49e1c0` *(data)* | display / genlock config struct (`+0x110` = `rate` reference) |
| `0x67e520` *(data)* | parsed-SIT state struct (see §5.3) |
| `0x5a7db8` *(data)* | global flag block (`+0x668` hdcp, `+0x675` lock) |
| `0x482800` *(data)* | `#SIT …` log format string |
| `0x481a10`/`0x481b0c`/`0x481bb8`/`0x481bd0`/`0x481c30` *(data)* | genlock strings |
| `0x00004c` *(file)* | SMEDIA02 section |
| `0x000bb0` *(file)* | SMAZ blob |
| `0x275844` *(file)* | unencrypted tail — web UI + default config |

### 6.3 SIT data-flow map

```
SIT TS packet on PID 0x77
        │  FFmpeg mpegts.c section filter (registered @ 0x2e10fc)
        ▼
   sit_cb  @ 0x2dd5bc
        │  parse → write state struct @ 0x67e520
        ├─ flags.hdcp   ─▶ 0x273178 ─▶ global 0x5a7db8+0x668
        ├─ flags.lock   ─▶ 0x2731a4 ─▶ global 0x5a7db8+0x675
        ├─ flags.skip + info ─▶ 0x272d60 ─▶ display-reconfigure thread
        ├─ width/height ─▶ state+0x16/+0x18 ─▶ ISP scaler windows
        └─ extra ─▶ ÷1000.0 ─▶ 0x2732f8 ─▶ display cfg 0x49e1c0+0x110
                                              │
                                              ▼
                                  genlock loop (0x2d79xx)
                                  rate / nominal_mode_rate  @ 0x271a3c
                                  └▶ display-clock trim @ 0x26fc68
```

---

## 7. Corroboration — confirming and extending prior knowledge

Earlier community work on the LKV373A targeted **older hardware revisions**,
which use a different SoC and a much simpler protocol (a near-raw video stream,
on an OpenRISC-class core — the lineage visible in the public
`otl-lkv373a-tools` project, which includes OpenRISC tooling). The **v3**
generation moved to the ITE9x3x ARM platform and introduced the SIT control
table, the display-clock genlock, and the link heartbeat. Those three are new
ground; the v3 receiver is materially more stateful than its predecessors, and
the older tools do not drive it.

This project's own working notes were also corrected as evidence accumulated:

- The firmware sections were first thought "scrambled / encrypted" (high
  entropy). They are plain **UCL compression**; once decompressed they are
  ordinary ARM code.
- The SIT `table_id` was first guessed `0x7F` and section byte 1 `0xF0`. The
  captures show **`0x77`** and **`0xB0`**.
- The `info` field was first thought "probably fps or interlace flags". It is
  the **HDMI output-mode index** — the actual resolution/refresh switch.
- The `extra` field was first thought "stored but never read, safe to leave
  zero". It is **read** — converted to a float and used as the genlock
  reference frequency (`refresh × 1000`).
- The resolution switch was first attributed to the flag bits. The captures
  show `lock` and `resolution` can both be 0 while the RX still switches;
  **`info` alone drives it**.
- The OSD packet was first decoded as 18 bytes with a 4-byte tail. It is
  **14 bytes**, no tail.
- An early theory blamed RX lock-ups on a missing link heartbeat alone. The
  heartbeat is necessary but the deeper cause is the **display-clock genlock**
  (§5.4): it needs delivery that is constant *and* consistent with PCR.

Every field, offset and behaviour in this document has been validated by a
software transmitter built from these findings, which a genuine v3 receiver
accepts indistinguishably from real TX hardware across all working modes.

---

## 8. Open questions

- **`jedi.rom`** — the binary blob in TX `sec2` (the ITE codec co-processor
  image) has not been analysed; it was out of scope.
- **The TX SIT builder** — the SIT output is fully specified by the captures,
  but the exact function in TX `sec1` that assembles it was not located by
  disassembly.
- **The XOR descrambler at RX `0x2dcd00`** — a byte-wise EOR against a small
  lookup table. It is *not* on the SIT or video path. It is plausibly the
  optional stream scrambler keyed by the config `key=` value, but this is
  unconfirmed; with the default key the captured payloads are plain.
- **The `resolution` flag bit and the `source` nibble** — both are parsed and
  cached in the state struct, but no downstream code was found that branches
  on the `resolution` bit's 0/1 value, and the `source` nibble's consumer was
  not fully traced.
- **Mode 33 (1680×1050)** — does not work on the receiver under any
  combination of settings; the firmware reason was not pinned down.
- **The TX bootloader** (`sec0`, 327,680 B) is extracted but not analysed.
