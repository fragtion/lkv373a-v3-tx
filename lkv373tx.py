#!/usr/bin/env python3
"""
lkv373tx.py  -  LKV373A v3 HDMI-over-IP transmitter   (v1.0)
=============================================================

Turns a PC into a stand-in for the LKV373A v3 transmitter: it builds the
proprietary "SIT" control section the receiver needs, paces the transport
stream at the constant PCR-locked rate the receiver's genlock expects, and
announces itself on the receiver's OSD port.

Two ways to run
---------------

  ffmpeg mode (recommended) - the script runs ffmpeg for you:

      lkv373tx.py --dst 192.168.10.213 --mode 20 --fps 30 \\
          --ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=30 \\
                   -c:v libx264 -profile:v baseline -b:v 8M

      Everything after --ffmpeg is the ffmpeg command and must come last.
      The ffmpeg binary is optional: if the first token starts with "-" it
      is treated as an ffmpeg option and "ffmpeg" from PATH is used; give a
      name or path as the first token to use a specific binary. Supply only
      input and encoding options - the output section
      ("-muxrate N -f mpegts pipe:1") is appended automatically.

  pipe mode - feed the script MPEG-TS on stdin:

      ffmpeg ... -f mpegts pipe:1 | lkv373tx.py --dst 192.168.10.213 --mode 20

Frame rate and --fps
--------------------
A receiver mode displays at a fixed rate. It can be fed two ways, selected
by the SIT skip-frame flag:

  * skip-frame off - encode at the mode's displayed rate (1:1).
  * skip-frame on  - encode at half that rate; the receiver doubles it.

Pass --fps with the frame rate you actually encode and the script sets the
flag for you, and refuses a rate that fits neither. Example: --mode 5
(576i50, displays 25 fps) accepts --fps 25 or --fps 12.5. In ffmpeg mode you
still set ffmpeg's -r yourself; --fps must agree with it.

What the script does
--------------------
  1. SIT injection - splices the PID 0x77 section every --sit-period seconds
     over null packets. Without it the receiver will not lock.
  2. CBR pacing    - meters output at the stream's own PCR rate so the
     receiver's genlock stays locked instead of resyncing to black.
  3. OSD           - announces the transmitter IP on UDP port 6000.

Ctrl+C / SIGTERM exits immediately; in ffmpeg mode ffmpeg is stopped too.

Requirements: Python 3.7+, ffmpeg on PATH (or pass a path). No third-party
Python packages.
"""

import argparse
import queue
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

__version__ = '1.0'


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SIT_PID           = 0x77      # PID carrying the proprietary SIT section
NULL_PID          = 0x1FFF    # MPEG-TS null packet PID
TABLE_ID          = 0x77      # SIT table_id (matches real TX hardware)
SECTION_BYTE1     = 0xB0      # SIT section_syntax + length high bits
TS_PACKET_SIZE    = 188
PACKETS_PER_DGRAM = 7
DGRAM_SIZE        = TS_PACKET_SIZE * PACKETS_PER_DGRAM   # 1316 bytes
PCR_HZ            = 27_000_000
OSD_PORT          = 6000
OSD_TX_PORTS      = (7000, 7001, 7003, 7002)

# SIT flags byte: bit7 hdcp, bit6 skip-frame, bit5 resolution, bit4 lock,
# bits3-0 input-source enum.  FLAGS_BASE is every bit except skip-frame at
# the real-TX value (lock=1, source=0xF); _resolve_flags() ORs the skip-frame
# bit in.  FLAGS_DEFAULT (skip-frame on) is used when neither --fps nor
# --flags is supplied.
FLAGS_BASE    = 0x1F
FLAGS_DEFAULT = 0x5F

# A spec-legal MPEG-TS null packet (PID 0x1FFF).
NULL_TS_PKT = bytes([0x47, 0x1F, 0xFF, 0x10]) + b'\xFF' * 184

# HDMI output-mode table.
# Index -> (width, height, label)
# The index is written into the SIT's `info` field; the receiver uses it to
# select its output timing.  Interlaced modes report the field rate.
MODE_TABLE = {
    0:  (640,  480,  '640x480p60'),
    1:  (720,  480,  '480i59.94'),
    2:  (720,  480,  '480p59.94'),
    3:  (720,  480,  '480i60'),
    4:  (720,  480,  '480p60'),
    5:  (720,  576,  '576i50'),
    6:  (720,  576,  '576p50'),
    7:  (1280, 720,  '720p50'),
    8:  (1280, 720,  '720p59.94'),
    9:  (1280, 720,  '720p60'),
    10: (1920, 1080, '1080p23.976'),
    11: (1920, 1080, '1080p24'),
    12: (1920, 1080, '1080p25'),
    13: (1920, 1080, '1080p29.97'),
    14: (1920, 1080, '1080p30'),
    15: (1920, 1080, '1080i50'),
    16: (1920, 1080, '1080p50'),
    17: (1920, 1080, '1080i59.94'),
    18: (1920, 1080, '1080p59.94'),
    19: (1920, 1080, '1080i60'),
    20: (1920, 1080, '1080p60'),
    21: (800,  600,  '800x600p60'),
    22: (1024, 768,  '1024x768p60'),
    23: (1280, 768,  '1280x768p60'),
    24: (1280, 800,  '1280x800p60'),
    25: (1280, 960,  '1280x960p60'),
    26: (1280, 1024, '1280x1024p60'),
    27: (1360, 768,  '1360x768p60'),
    28: (1366, 768,  '1366x768p60'),
    29: (1440, 900,  '1440x900p60'),
    30: (1400, 1050, '1400x1050p60'),
    31: (1440, 1050, '1440x1050p60'),
    32: (1600, 900,  '1600x900p60'),
    33: (1680, 1050, '1680x1050p60'),
}


def _mode_timing(mode):
    """Return (displayed_fps, interlaced, refresh_hz) for a mode.

    `refresh_hz` is the rate in the mode label - the field rate for
    interlaced modes.  `displayed_fps` is the rate at which whole frames
    reach the screen: the refresh rate for progressive modes, half of it for
    interlaced modes (two fields make one frame).
    """
    label = MODE_TABLE[mode][2]
    m = re.search(r'([pi])(\d+(?:\.\d+)?)$', label)
    if not m:
        return 60.0, False, 60.0
    interlaced = (m.group(1) == 'i')
    refresh = float(m.group(2))
    return (refresh / 2.0 if interlaced else refresh), interlaced, refresh


def _mode_extra(mode):
    """SIT `extra` field: nominal refresh rate x 1000.

    The receiver divides this by 1000.0 and uses the result as the reference
    frequency for its HDMI display-clock genlock loop.  It tracks the mode's
    refresh rate (the field rate for interlaced modes).
    """
    _displayed, _interlaced, refresh = _mode_timing(mode)
    return round(refresh * 1000)


def _resolve_flags(args):
    """Resolve the SIT flags byte from --flags / --fps / the default.

    --flags, when given, is a raw override.  Otherwise --fps selects the
    skip-frame bit so that

        encoded_fps x (2 if skip-frame else 1)  ==  the displayed frame
        rate of --mode

    With neither option, FLAGS_DEFAULT (skip-frame on) is used.
    """
    if args.flags is not None:
        if args.fps is not None:
            sys.stderr.write('[lkv] note: --flags is set; --fps ignored for '
                             'the skip-frame bit\n')
        return args.flags

    if args.fps is None:
        sys.stderr.write('[lkv] note: no --fps given - assuming skip-frame on '
                         '(encode at half the mode rate).  Pass --fps for '
                         'automatic handling.\n')
        return FLAGS_DEFAULT

    if args.fps <= 0:
        sys.exit('[lkv] error: --fps must be positive')

    displayed, _interlaced, _refresh = _mode_timing(args.mode)
    ratio = displayed / args.fps
    if abs(ratio - 1.0) <= 0.1:
        skip = 0
    elif abs(ratio - 2.0) <= 0.1:
        skip = 1
    else:
        label = MODE_TABLE[args.mode][2]
        sys.exit('[lkv] error: --fps %g does not fit --mode %d (%s).\n'
                 '            That mode puts %g frames/s on screen - encode at '
                 '%g fps (1:1)\n'
                 '            or %g fps (the receiver doubles it).'
                 % (args.fps, args.mode, label,
                    displayed, displayed, displayed / 2.0))
    return FLAGS_BASE | (skip << 6)


# ---------------------------------------------------------------------------
# CRC-32 (MPEG-2 PSI)
# ---------------------------------------------------------------------------

def _mpeg_crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF \
                  if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


# ---------------------------------------------------------------------------
# SIT section
#
# Private PSI section on PID 0x77.  The receiver parses it to configure its
# HDMI output.  Layout (verified against RX firmware and a real-TX capture):
#
#   off  size  field
#    0    8    PSI header  (table_id=0x77, version in byte 5)
#    8    1    flags       bit7=hdcp, bit6=skipframe, bit5=resolution,
#                          bit4=lock, bits3-0=input-source enum
#    9    2    width       big-endian pixels
#   11    2    height      big-endian pixels
#   13    2    info        HDMI output-mode index (MODE_TABLE key)
#   15    1    audio       low nibble = codec (0=MPEG/MP2); high nibble ignored
#   16    4    sample_rate big-endian Hz
#   20    4    extra       refresh-rate x 1000; receiver uses extra/1000.0 as
#                          its genlock reference rate
#   24    4    CRC-32
# ---------------------------------------------------------------------------

def build_sit_section(width, height, info, sample_rate=48000, extra=60000,
                      flags=0x5F, byte15=0xF0, version=0):
    payload = (bytes([flags & 0xFF])
               + struct.pack('>H', width & 0xFFFF)
               + struct.pack('>H', height & 0xFFFF)
               + struct.pack('>H', info & 0xFFFF)
               + bytes([byte15 & 0xFF])
               + struct.pack('>I', sample_rate & 0xFFFFFFFF)
               + struct.pack('>I', extra & 0xFFFFFFFF))
    body_len = 5 + len(payload) + 4
    h = bytearray(8)
    h[0] = TABLE_ID
    h[1] = SECTION_BYTE1 | ((body_len >> 8) & 0x0F)
    h[2] = body_len & 0xFF
    h[3] = 0xFF
    h[4] = 0xFF
    h[5] = 0xC0 | ((version & 0x1F) << 1) | 0x01
    h[6] = 0x00
    h[7] = 0x00
    section = bytes(h) + payload
    return section + struct.pack('>I', _mpeg_crc32(section))


def _build_sit_ts_packet(section, cc):
    hdr = bytes([0x47,
                 0x40 | ((SIT_PID >> 8) & 0x1F),
                 SIT_PID & 0xFF,
                 0x10 | (cc & 0x0F)])
    payload = bytes([0x00]) + section
    pad = TS_PACKET_SIZE - len(hdr) - len(payload)
    return hdr + payload + b'\xFF' * pad


# ---------------------------------------------------------------------------
# OSD / TX-info announcement
# ---------------------------------------------------------------------------

def _build_osd_packet(tx_ip, ports=OSD_TX_PORTS):
    pkt = bytes([0x01, 0x00]) + socket.inet_aton(tx_ip)
    for p in ports:
        pkt += struct.pack('<H', p & 0xFFFF)
    return pkt   # 14 bytes


class OsdSender(threading.Thread):
    """Sends the TX-info packet to the receiver's OSD port once per period."""

    def __init__(self, rx_ip, tx_ip, period=1.08):
        super().__init__(daemon=True)
        self.rx_ip = rx_ip
        self.tx_ip = tx_ip
        self.period = period
        self._stop = threading.Event()

    def run(self):
        pkt = _build_osd_packet(self.tx_ip)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        while not self._stop.is_set():
            try:
                s.sendto(pkt, (self.rx_ip, OSD_PORT))
            except OSError:
                pass
            self._stop.wait(self.period)
        s.close()

    def stop(self):
        self._stop.set()


# ---------------------------------------------------------------------------
# PCR extraction
# ---------------------------------------------------------------------------

def _ts_pcr(pkt):
    """Return the 27 MHz PCR value from a TS packet, or None."""
    if len(pkt) < 12 or pkt[0] != 0x47:
        return None
    if not (pkt[3] & 0x20):       # adaptation field present?
        return None
    if pkt[4] < 7:                # AF long enough for PCR?
        return None
    if not (pkt[5] & 0x10):       # PCR_flag set?
        return None
    b = pkt[6:12]
    base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
    ext  = ((b[4] & 1) << 8) | b[5]
    return base * 300 + ext


# ---------------------------------------------------------------------------
# TS reader thread
#
# Reads 188-byte TS packets from a binary stream (stdin or ffmpeg stdout),
# splices the SIT over null packets, samples PCR for rate measurement, and
# packs the result into 1316-byte (7-packet) datagrams on an output queue.
# ---------------------------------------------------------------------------

class TsReader(threading.Thread):

    def __init__(self, stream, out_q, section, sit_period):
        super().__init__(daemon=True)
        self.stream     = stream
        self.q          = out_q
        self.section    = section
        self.sit_period = sit_period
        self.eof        = threading.Event()
        self._stop      = threading.Event()
        self._cc        = 0
        # (byte_offset, pcr27) pairs used to estimate the stream bitrate
        self.pcr_samples = []
        self._pcr_lock   = threading.Lock()
        self.stats = dict(ts_in=0, nulls=0, replaced=0, inserted=0, dgrams=0)

    def _make_sit(self):
        p = _build_sit_ts_packet(self.section, self._cc)
        self._cc = (self._cc + 1) & 0x0F
        return p

    def estimate_rate(self):
        """Stream bitrate in bits/sec measured from PCR, or None."""
        with self._pcr_lock:
            s = list(self.pcr_samples)
        if len(s) < 2:
            return None
        (b0, p0), (b1, p1) = s[0], s[-1]
        dp = p1 - p0
        if dp <= 0:
            dp += (1 << 33) * 300   # handle PCR wrap
        if dp <= 0 or b1 <= b0:
            return None
        return (b1 - b0) * 8 * PCR_HZ / dp

    def run(self):
        buf       = b''
        batch     = []
        byte_off  = 0
        next_sit  = time.monotonic()
        sit_pending  = False
        sit_due_at   = 0.0
        warned       = False

        try:
            while not self._stop.is_set():
                chunk = self.stream.read(DGRAM_SIZE)
                if not chunk:
                    break
                buf += chunk

                now = time.monotonic()
                if now >= next_sit and not sit_pending:
                    sit_pending = True
                    sit_due_at  = now
                    next_sit    = now + self.sit_period

                while len(buf) >= TS_PACKET_SIZE:
                    if buf[0] != 0x47:
                        nxt = buf.find(b'\x47', 1)
                        buf = b'' if nxt < 0 else buf[nxt:]
                        continue
                    pkt = buf[:TS_PACKET_SIZE]
                    buf = buf[TS_PACKET_SIZE:]

                    pcr = _ts_pcr(pkt)
                    if pcr is not None and len(self.pcr_samples) < 64:
                        with self._pcr_lock:
                            self.pcr_samples.append((byte_off, pcr))

                    pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
                    if pid == NULL_PID:
                        self.stats['nulls'] += 1
                        if sit_pending:
                            pkt = self._make_sit()
                            sit_pending = False
                            self.stats['replaced'] += 1

                    batch.append(pkt)
                    byte_off += TS_PACKET_SIZE
                    self.stats['ts_in'] += 1

                    if len(batch) >= PACKETS_PER_DGRAM:
                        self.q.put(b''.join(batch))
                        self.stats['dgrams'] += 1
                        batch = []

                # Fallback: if no null packets arrived within 0.5 s of a SIT
                # being due, insert one anyway (stream lacks -muxrate padding).
                if sit_pending and time.monotonic() - sit_due_at > 0.5:
                    batch.append(self._make_sit())
                    sit_pending = False
                    self.stats['inserted'] += 1
                    if not warned:
                        sys.stderr.write(
                            '[lkv] WARNING: no null packets - inserting SIT. '
                            'Add -muxrate to ffmpeg for cleaner injection.\n')
                        warned = True
                    if len(batch) >= PACKETS_PER_DGRAM:
                        self.q.put(b''.join(batch))
                        self.stats['dgrams'] += 1
                        batch = []
        finally:
            if batch:
                while len(batch) < PACKETS_PER_DGRAM:
                    batch.append(NULL_TS_PKT)
                try:
                    self.q.put(b''.join(batch), timeout=1.0)
                    self.stats['dgrams'] += 1
                except queue.Full:
                    pass
            self.eof.set()

    def stop(self):
        self._stop.set()


# ---------------------------------------------------------------------------
# Networking helpers
# ---------------------------------------------------------------------------

def _is_multicast(ip):
    try:
        return 224 <= int(ip.split('.')[0]) <= 239
    except (ValueError, IndexError):
        return False


def _detect_local_ip(peer):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer, 9))
        return s.getsockname()[0]
    except OSError:
        return '0.0.0.0'
    finally:
        s.close()


# ---------------------------------------------------------------------------
# ffmpeg subprocess helpers
# ---------------------------------------------------------------------------

def _build_ffmpeg_cmd(ffmpeg_bin, ffmpeg_args, muxrate):
    """Build the ffmpeg argv.

    The caller supplies only input and encoding options; this appends the
    MPEG-TS-to-stdout output section ("-muxrate N -f mpegts pipe:1").  The
    appended "-f mpegts" applies to the output and is independent of any
    "-f" used on the inputs.  If the args already end in an explicit pipe
    target they are used unchanged.
    """
    out = list(ffmpeg_args)
    if out and out[-1] in ('pipe:1', 'pipe:', '-'):
        return [ffmpeg_bin] + out
    if '-muxrate' not in out:
        out += ['-muxrate', str(muxrate)]
    out += ['-f', 'mpegts', 'pipe:1']
    return [ffmpeg_bin] + out


def _prefix_stderr(label, pipe, dest):
    """Relay lines from pipe to dest, prepending label. Runs in a thread."""
    try:
        for raw in pipe:
            dest.buffer.write((label + raw.decode(errors='replace')).encode())
            dest.buffer.flush()
    except (OSError, ValueError):
        pass


# ---------------------------------------------------------------------------
# Core sender loop
# ---------------------------------------------------------------------------

def _run_sender(args, stream, ff_proc=None):
    """
    Read TS from `stream`, inject the SIT, and pace the output to `args.dst`.
    `ff_proc`, if given, is the ffmpeg subprocess; it is stopped on exit.
    Returns 0 on a clean run, 1 if no data was sent.
    """
    mw, mh, label = MODE_TABLE[args.mode]
    sit_w  = args.sit_width  if args.sit_width  is not None else mw
    sit_h  = args.sit_height if args.sit_height is not None else mh
    extra  = args.extra      if args.extra      is not None else _mode_extra(args.mode)

    section = build_sit_section(
        width=sit_w, height=sit_h, info=args.mode,
        sample_rate=args.audio_rate, extra=extra,
        flags=args.flags, byte15=args.byte15, version=0)

    hb_interval = max(0.0002, args.heartbeat_ms / 1000.0)

    skipframe = (args.flags >> 6) & 1
    displayed, _interlaced, _refresh = _mode_timing(args.mode)
    expect_fps = displayed / 2.0 if skipframe else displayed

    sys.stderr.write(
        '[lkv] mode %d (%s)  SIT %dx%d  flags 0x%02X (skip-frame %s)\n'
        % (args.mode, label, sit_w, sit_h, args.flags,
           'on' if skipframe else 'off'))
    sys.stderr.write(
        '[lkv] genlock %.3f Hz, expecting ~%g fps input  ->  %s:%d\n'
        % (extra / 1000.0, expect_fps, args.dst, args.port))
    if args.dump_sit:
        sys.stderr.write('[lkv] SIT section: %s\n' % section.hex(' '))

    # ---- UDP socket ----
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    if _is_multicast(args.dst):
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, args.ttl)
    else:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(('', args.port))   # source port matches real TX hardware
    except OSError as e:
        sys.stderr.write('[lkv] note: could not bind src port %d (%s) - '
                         'using ephemeral port\n' % (args.port, e))
    target = (args.dst, args.port)

    # ---- OSD sender ----
    osd = None
    if not args.no_osd:
        rx_ip = args.rx_ip or (args.dst if not _is_multicast(args.dst) else None)
        if rx_ip:
            tx_ip = args.osd_ip or _detect_local_ip(rx_ip)
            osd = OsdSender(rx_ip, tx_ip)
            osd.start()
            sys.stderr.write('[lkv] OSD: announcing %s to %s:%d\n'
                             % (tx_ip, rx_ip, OSD_PORT))
        else:
            sys.stderr.write('[lkv] OSD disabled (multicast --dst, no --rx-ip)\n')

    # ---- Reader thread ----
    out_q  = queue.Queue(maxsize=16384)
    reader = TsReader(stream, out_q, section, args.sit_period)
    reader.start()

    # ---- Signal handling: immediate exit ----
    _quit = threading.Event()

    def _on_signal(signum, frame):
        _quit.set()

    signal.signal(signal.SIGINT,  _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    # ---- Pacing state ----
    interval  = DGRAM_SIZE * 8.0 / float(args.muxrate)  # refined from PCR
    t0        = None    # absolute schedule origin, latched after prebuffer
    seq       = 0       # index of next data datagram on the schedule
    next_hb   = time.monotonic()
    sent_data = sent_empty = underflows = 0

    def _heartbeat(now):
        nonlocal next_hb, sent_empty
        if now >= next_hb:
            try:
                sock.sendto(b'', target)
            except OSError:
                pass
            sent_empty += 1
            next_hb = now + hb_interval

    try:
        while not _quit.is_set():
            now = time.monotonic()

            # -- Prebuffer: wait until enough datagrams are queued, then
            #    latch the clock and measure the real stream rate from PCR.
            if t0 is None:
                if out_q.qsize() >= args.prebuffer:
                    rate = reader.estimate_rate()
                    if rate and 250_000 <= rate <= 60_000_000:
                        interval = DGRAM_SIZE * 8.0 / rate
                        src = 'measured from PCR'
                    else:
                        rate = args.muxrate
                        src  = 'from --muxrate (PCR not seen yet)'
                    t0 = time.monotonic()
                    sys.stderr.write(
                        '[lkv] pacing at %.3f Mbps (%s) - '
                        '%.3f ms per datagram\n'
                        % (rate / 1e6, src, interval * 1000.0))
                elif reader.eof.is_set() and out_q.empty():
                    break
                else:
                    _heartbeat(now)
                    time.sleep(0.0005)
                continue

            # -- Exit cleanly when input is exhausted.
            if reader.eof.is_set() and out_q.empty():
                break

            # -- Paced output: one datagram every `interval` seconds on an
            #    absolute schedule so jitter does not accumulate.
            due = t0 + seq * interval
            if now >= due:
                try:
                    dg = out_q.get_nowait()
                except queue.Empty:
                    dg = None

                if dg is not None:
                    try:
                        sock.sendto(dg, target)
                    except OSError:
                        pass
                    sent_data += 1
                    seq       += 1
                    next_hb    = time.monotonic() + hb_interval
                else:
                    # Reader fell behind: slide the schedule forward one slot
                    # rather than emitting a gap, and fill with a heartbeat.
                    t0 += interval
                    underflows += 1
                    _heartbeat(now)
                    time.sleep(0.0003)
            else:
                _heartbeat(now)
                slp = min(due, next_hb) - time.monotonic()
                if slp > 0:
                    time.sleep(min(slp, 0.002))

    finally:
        reader.stop()
        if osd:
            osd.stop()
        # Brief tail of keep-alives so the receiver settles before we vanish.
        try:
            for _ in range(args.tail_beats):
                sock.sendto(b'', target)
                time.sleep(hb_interval)
        except OSError:
            pass
        sock.close()

    s = reader.stats
    sys.stderr.write(
        '[lkv] done. %d data + %d keep-alive datagrams, %d underflows | '
        'TS: %d pkts, %d nulls, SIT %d replaced + %d inserted\n'
        % (sent_data, sent_empty, underflows,
           s['ts_in'], s['nulls'], s['replaced'], s['inserted']))

    # ---- Stop ffmpeg if we spawned it ----
    if ff_proc is not None and ff_proc.poll() is None:
        try:
            ff_proc.terminate()
            ff_proc.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            try:
                ff_proc.kill()
            except OSError:
                pass

    # Non-zero exit if nothing went out - usually a bad ffmpeg command.
    if sent_data == 0:
        sys.stderr.write('[lkv] error: no data datagrams sent - check the '
                         'ffmpeg command and its input.\n')
        return 1
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(
        prog='lkv373tx.py',
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    p.add_argument('--version', action='version',
                   version='lkv373tx.py ' + __version__)

    # -- Destination --
    p.add_argument('--dst', required=True,
                   help='Receiver IP address (unicast) or multicast group. '
                        'This is the LKV373A RX unit you want to drive.')
    p.add_argument('--port', type=int, default=5004,
                   help='UDP destination port. The real TX hardware uses 5004; '
                        'only change this if your RX firmware differs. '
                        '(default: 5004)')

    # -- HDMI output mode --
    mode_list = ', '.join('%d=%s' % (k, v[2]) for k, v in sorted(MODE_TABLE.items()))
    p.add_argument('--mode', type=int, default=14,
                   help='HDMI output-mode index written into the SIT info field. '
                        'The receiver uses this to select its output timing and '
                        'resolution. Must match what you are actually encoding. '
                        'Valid values: ' + mode_list + '. (default: 14 = 1080p30)')
    p.add_argument('--fps', type=float, default=None,
                   help='Frame rate you are actually encoding. The script uses '
                        'it to set the SIT skip-frame flag automatically: a '
                        "mode is fed either at its displayed rate (1:1) or at "
                        'half that rate (the receiver doubles each frame). '
                        'Pass this instead of a manual --flags; the script '
                        'rejects a rate that fits neither. In ffmpeg mode it '
                        "must agree with ffmpeg's -r. (default: unset)")
    p.add_argument('--sit-width', dest='sit_width', type=int, default=None,
                   help='Pixel width written into the SIT. Defaults to the '
                        'native width of --mode. Override when your encoded '
                        'resolution differs from the mode nominal (e.g. you are '
                        'encoding 1280x720 but outputting at 1080p30).')
    p.add_argument('--sit-height', dest='sit_height', type=int, default=None,
                   help='Pixel height written into the SIT. Same rules as '
                        '--sit-width. (default: height of --mode)')

    # -- SIT tuning --
    p.add_argument('--audio-rate', dest='audio_rate', type=int, default=48000,
                   help='Audio sample rate (Hz) written into the SIT. '
                        'Should match the encoded audio. (default: 48000)')
    p.add_argument('--extra', type=int, default=None,
                   help='SIT extra field = refresh rate x 1000. The receiver '
                        'divides this by 1000 and uses it as its display-clock '
                        'genlock reference. Default: auto-derived from --mode '
                        '(e.g. mode 14 -> 30000). Override only for testing.')
    p.add_argument('--flags', type=lambda x: int(x, 0), default=None,
                   help='Raw SIT flags byte - an expert override that bypasses '
                        '--fps. Bit 7=HDCP, bit 6=skip-frame, bit 5=resolution, '
                        'bit 4=lock, bits 3-0=input source. 0x5F (skip-frame on) '
                        'and 0x1F (off) are the real-TX values. (default: '
                        'derived from --fps, or 0x5F if --fps is also unset)')
    p.add_argument('--byte15', type=lambda x: int(x, 0), default=0xF0,
                   help='SIT audio-codec byte. Low nibble selects the codec: '
                        '0=MPEG/MP2. High nibble is ignored by the receiver. '
                        '0xF0 matches the real TX hardware. (default: 0xF0)')
    p.add_argument('--sit-period', dest='sit_period', type=float, default=0.2,
                   help='Seconds between SIT injections. The real TX sends one '
                        'every 200 ms; reducing this helps a cold receiver lock '
                        'faster. (default: 0.2)')

    # -- Bitrate / pacing --
    p.add_argument('--muxrate', type=int, default=10_000_000,
                   help='Transport stream mux rate in bits/sec. In pipe mode '
                        'this is the fallback pacing rate used until PCR is '
                        'measured. In ffmpeg mode it is also passed to ffmpeg '
                        'as -muxrate. Should match your ffmpeg -muxrate / -b:v '
                        'budget. (default: 10000000)')
    p.add_argument('--prebuffer', type=int, default=24,
                   help='Number of 1316-byte datagrams to buffer before paced '
                        'output starts. Larger values absorb more input jitter '
                        'at the cost of startup latency (~100 ms at 10 Mbps '
                        'with the default of 24). (default: 24)')
    p.add_argument('--heartbeat-ms', dest='heartbeat_ms', type=float, default=1.0,
                   help='Sub-millisecond gaps between paced datagrams are filled '
                        'with empty (0-byte) keep-alive datagrams to match the '
                        'real TX heartbeat pattern. This sets the minimum gap '
                        'before a keep-alive is sent. (default: 1.0 ms)')
    p.add_argument('--tail-beats', dest='tail_beats', type=int, default=20,
                   help='Number of keep-alive datagrams sent after the stream '
                        'ends so the receiver has time to settle before the '
                        'socket closes. (default: 20)')

    # -- OSD / TX announcement --
    p.add_argument('--rx-ip', dest='rx_ip', default=None,
                   help='Receiver IP for OSD packets (port 6000). Defaults to '
                        '--dst when --dst is a unicast address.')
    p.add_argument('--osd-ip', dest='osd_ip', default=None,
                   help='IP address to advertise as the transmitter in OSD '
                        'packets. Defaults to the local interface IP facing the '
                        'receiver (auto-detected).')
    p.add_argument('--no-osd', action='store_true',
                   help='Disable OSD / TX-info announcements on port 6000. '
                        'The receiver will still display video but may not show '
                        'the transmitter IP in its on-screen menu.')

    # -- Multicast --
    p.add_argument('--ttl', type=int, default=16,
                   help='IP TTL for multicast packets. Only used when --dst is '
                        'a multicast address (224.x.x.x - 239.x.x.x). '
                        '(default: 16)')

    # -- Diagnostics --
    p.add_argument('--dump-sit', action='store_true',
                   help='Print the constructed SIT section as a hex string to '
                        'stderr on startup. Useful for verifying the SIT '
                        'against a real-TX capture.')

    # -- ffmpeg mode --
    p.add_argument('--ffmpeg', nargs=argparse.REMAINDER, default=None,
                   metavar='[ffmpeg-binary] ffmpeg-args ...',
                   help='Run ffmpeg internally (recommended). Everything after '
                        '--ffmpeg is the ffmpeg command and must come last on '
                        'the line. The binary is optional: if the first token '
                        'starts with "-" it is treated as an ffmpeg option and '
                        '"ffmpeg" from PATH is used; otherwise the first token '
                        'is the binary (name or path). Supply only input and '
                        'encode options - the output section '
                        '(-muxrate N -f mpegts pipe:1) is appended for you. '
                        'Without --ffmpeg the script reads MPEG-TS from stdin. '
                        'Example: --ffmpeg -re -i input.mp4 -c:v libx264 -b:v 8M')

    args = p.parse_args()

    # ---- Validate and resolve ----
    if args.mode not in MODE_TABLE:
        p.error('--mode %d is out of range (valid 0-33)' % args.mode)
    args.flags = _resolve_flags(args)

    # ---- pipe mode ----
    if args.ffmpeg is None:
        sys.stderr.write('[lkv] lkv373tx.py %s - pipe mode (reading stdin)\n'
                         % __version__)
        sys.exit(_run_sender(args, sys.stdin.buffer))

    # ---- ffmpeg mode ----
    rem = list(args.ffmpeg)
    if not rem:
        p.error('--ffmpeg needs an ffmpeg command, e.g. '
                '--ffmpeg -re -i input.mp4 -c:v libx264 -b:v 8M')
    if rem[0].startswith('-'):
        ffmpeg_bin, ffmpeg_args = 'ffmpeg', rem
    else:
        ffmpeg_bin, ffmpeg_args = rem[0], rem[1:]
    if not ffmpeg_args:
        p.error('--ffmpeg: no ffmpeg arguments given after the binary name')

    ffmpeg_cmd = _build_ffmpeg_cmd(ffmpeg_bin, ffmpeg_args, args.muxrate)
    sys.stderr.write('[lkv] lkv373tx.py %s - ffmpeg mode\n' % __version__)
    sys.stderr.write('[lkv] ffmpeg: %s\n' % ' '.join(ffmpeg_cmd))

    try:
        ff_proc = subprocess.Popen(
            ffmpeg_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        sys.exit('[lkv] error: ffmpeg binary not found: %s' % ffmpeg_bin)
    except OSError as e:
        sys.exit('[lkv] error: could not start ffmpeg: %s' % e)

    # Relay ffmpeg's stderr with a prefix so it is distinguishable.
    relay = threading.Thread(
        target=_prefix_stderr,
        args=('[ffmpeg] ', ff_proc.stderr, sys.stderr),
        daemon=True)
    relay.start()

    rc = _run_sender(args, ff_proc.stdout, ff_proc)
    relay.join(timeout=2)
    sys.exit(rc)


if __name__ == '__main__':
    main()
