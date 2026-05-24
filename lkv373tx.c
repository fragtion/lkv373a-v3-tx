/*
 * lkv373tx.c - LKV373A v3 HDMI-over-IP transmitter (C port of lkv373tx.py 1.0)
 *
 * Faithful C port of the Python reference. Same behaviour:
 *   - builds the proprietary SIT section (PID 0x77)
 *   - splices SIT over null packets, with fallback insertion
 *   - CBR pacing locked to the stream's PCR rate
 *   - empty-datagram heartbeats between paced sends
 *   - OSD announcement thread on UDP/6000
 *   - optional embedded ffmpeg subprocess
 *
 * Build:  cc -O2 -Wall -Wextra -o lkv373tx lkv373tx.c -lpthread -lm
 *
 * Usage (pipe mode):
 *     ffmpeg ... -f mpegts pipe:1 | ./lkv373tx --dst 192.168.10.213 --mode 20
 *
 * Usage (ffmpeg mode):
 *     ./lkv373tx --dst 192.168.10.213 --mode 20 --fps 30 \
 *         --ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=30 \
 *                  -c:v libx264 -profile:v baseline -b:v 8M
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LKV_VERSION         "1.0"

#define TS_PACKET_SIZE      188
#define PACKETS_PER_DGRAM   7
#define DGRAM_SIZE          (TS_PACKET_SIZE * PACKETS_PER_DGRAM) /* 1316 */
#define SIT_PID             0x77
#define NULL_PID            0x1FFF
#define TABLE_ID            0x77
#define SECTION_BYTE1       0xB0
#define PCR_HZ              27000000LL
#define OSD_PORT            6000

#define FLAGS_BASE          0x1F
#define FLAGS_DEFAULT       0x5F

#define MAX_MODES           34
#define PCR_SAMPLE_MAX      64
#define QUEUE_CAP           16384

/* ------------------------------------------------------------------ */
/* Mode table                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int  width;
    int  height;
    const char *label;
    double refresh_hz;   /* rate in the label - field rate for interlaced */
    bool   interlaced;
} mode_t_;

/* refresh_hz is the rate in the label (field rate for interlaced). */
static const mode_t_ MODE_TABLE[MAX_MODES] = {
    /* 0  */ { 640,  480,  "640x480p60",     60.0,    false },
    /* 1  */ { 720,  480,  "480i59.94",      59.94,   true  },
    /* 2  */ { 720,  480,  "480p59.94",      59.94,   false },
    /* 3  */ { 720,  480,  "480i60",         60.0,    true  },
    /* 4  */ { 720,  480,  "480p60",         60.0,    false },
    /* 5  */ { 720,  576,  "576i50",         50.0,    true  },
    /* 6  */ { 720,  576,  "576p50",         50.0,    false },
    /* 7  */ { 1280, 720,  "720p50",         50.0,    false },
    /* 8  */ { 1280, 720,  "720p59.94",      59.94,   false },
    /* 9  */ { 1280, 720,  "720p60",         60.0,    false },
    /* 10 */ { 1920, 1080, "1080p23.976",    23.976,  false },
    /* 11 */ { 1920, 1080, "1080p24",        24.0,    false },
    /* 12 */ { 1920, 1080, "1080p25",        25.0,    false },
    /* 13 */ { 1920, 1080, "1080p29.97",     29.97,   false },
    /* 14 */ { 1920, 1080, "1080p30",        30.0,    false },
    /* 15 */ { 1920, 1080, "1080i50",        50.0,    true  },
    /* 16 */ { 1920, 1080, "1080p50",        50.0,    false },
    /* 17 */ { 1920, 1080, "1080i59.94",     59.94,   true  },
    /* 18 */ { 1920, 1080, "1080p59.94",     59.94,   false },
    /* 19 */ { 1920, 1080, "1080i60",        60.0,    true  },
    /* 20 */ { 1920, 1080, "1080p60",        60.0,    false },
    /* 21 */ { 800,  600,  "800x600p60",     60.0,    false },
    /* 22 */ { 1024, 768,  "1024x768p60",    60.0,    false },
    /* 23 */ { 1280, 768,  "1280x768p60",    60.0,    false },
    /* 24 */ { 1280, 800,  "1280x800p60",    60.0,    false },
    /* 25 */ { 1280, 960,  "1280x960p60",    60.0,    false },
    /* 26 */ { 1280, 1024, "1280x1024p60",   60.0,    false },
    /* 27 */ { 1360, 768,  "1360x768p60",    60.0,    false },
    /* 28 */ { 1366, 768,  "1366x768p60",    60.0,    false },
    /* 29 */ { 1440, 900,  "1440x900p60",    60.0,    false },
    /* 30 */ { 1400, 1050, "1400x1050p60",   60.0,    false },
    /* 31 */ { 1440, 1050, "1440x1050p60",   60.0,    false },
    /* 32 */ { 1600, 900,  "1600x900p60",    60.0,    false },
    /* 33 */ { 1680, 1050, "1680x1050p60",   60.0,    false },
};

static double mode_displayed_fps(int mode) {
    const mode_t_ *m = &MODE_TABLE[mode];
    return m->interlaced ? m->refresh_hz / 2.0 : m->refresh_hz;
}

static int mode_extra(int mode) {
    /* refresh rate * 1000, rounded */
    return (int)lround(MODE_TABLE[mode].refresh_hz * 1000.0);
}

/* spec-legal null TS packet (PID 0x1FFF) */
static uint8_t NULL_TS_PKT[TS_PACKET_SIZE];
static void init_null_ts_pkt(void) {
    NULL_TS_PKT[0] = 0x47;
    NULL_TS_PKT[1] = 0x1F;
    NULL_TS_PKT[2] = 0xFF;
    NULL_TS_PKT[3] = 0x10;
    memset(NULL_TS_PKT + 4, 0xFF, TS_PACKET_SIZE - 4);
}

/* ------------------------------------------------------------------ */
/* CRC-32 (MPEG-2 PSI)                                                 */
/* ------------------------------------------------------------------ */

static uint32_t mpeg_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint32_t)data[i]) << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000u)
                crc = (crc << 1) ^ 0x04C11DB7u;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* SIT section                                                         */
/*                                                                     */
/*   off  size  field                                                  */
/*    0    8    PSI header                                             */
/*    8    1    flags                                                  */
/*    9    2    width  (BE)                                            */
/*   11    2    height (BE)                                            */
/*   13    2    info   (BE)  -- HDMI mode index                        */
/*   15    1    byte15 (audio codec; 0xF0 matches real TX)             */
/*   16    4    sample_rate (BE) Hz                                    */
/*   20    4    extra (BE) = refresh_hz * 1000                         */
/*   24    4    CRC-32                                                 */
/* ------------------------------------------------------------------ */

#define SIT_SECTION_LEN 28

static int build_sit_section(uint8_t out[SIT_SECTION_LEN],
                             int width, int height, int info,
                             uint32_t sample_rate, uint32_t extra,
                             uint8_t flags, uint8_t byte15, uint8_t version) {
    uint8_t payload[16];
    payload[0]  = flags;
    payload[1]  = (width  >> 8) & 0xFF;
    payload[2]  =  width        & 0xFF;
    payload[3]  = (height >> 8) & 0xFF;
    payload[4]  =  height       & 0xFF;
    payload[5]  = (info   >> 8) & 0xFF;
    payload[6]  =  info         & 0xFF;
    payload[7]  = byte15;
    payload[8]  = (sample_rate >> 24) & 0xFF;
    payload[9]  = (sample_rate >> 16) & 0xFF;
    payload[10] = (sample_rate >>  8) & 0xFF;
    payload[11] =  sample_rate        & 0xFF;
    payload[12] = (extra >> 24) & 0xFF;
    payload[13] = (extra >> 16) & 0xFF;
    payload[14] = (extra >>  8) & 0xFF;
    payload[15] =  extra        & 0xFF;

    int body_len = 5 + (int)sizeof(payload) + 4;  /* PSI body after byte 2 */
    out[0] = TABLE_ID;
    out[1] = SECTION_BYTE1 | ((body_len >> 8) & 0x0F);
    out[2] = body_len & 0xFF;
    out[3] = 0xFF;
    out[4] = 0xFF;
    out[5] = 0xC0 | ((version & 0x1F) << 1) | 0x01;
    out[6] = 0x00;
    out[7] = 0x00;
    memcpy(out + 8, payload, sizeof(payload));

    uint32_t crc = mpeg_crc32(out, 8 + sizeof(payload));
    out[24] = (crc >> 24) & 0xFF;
    out[25] = (crc >> 16) & 0xFF;
    out[26] = (crc >>  8) & 0xFF;
    out[27] =  crc        & 0xFF;
    return SIT_SECTION_LEN;
}

static void build_sit_ts_packet(uint8_t out[TS_PACKET_SIZE],
                                const uint8_t *section, int section_len,
                                int cc) {
    out[0] = 0x47;
    out[1] = 0x40 | ((SIT_PID >> 8) & 0x1F);   /* payload_unit_start_indicator */
    out[2] = SIT_PID & 0xFF;
    out[3] = 0x10 | (cc & 0x0F);                /* payload only, CC */
    out[4] = 0x00;                              /* pointer_field */
    memcpy(out + 5, section, section_len);
    int filled = 5 + section_len;
    memset(out + filled, 0xFF, TS_PACKET_SIZE - filled);
}

/* ------------------------------------------------------------------ */
/* PCR extraction                                                      */
/* ------------------------------------------------------------------ */

/* Returns 27 MHz PCR or -1 if not present. */
static int64_t ts_pcr(const uint8_t *pkt) {
    if (pkt[0] != 0x47) return -1;
    if (!(pkt[3] & 0x20)) return -1;     /* adaptation field present? */
    if (pkt[4] < 7) return -1;           /* AF long enough for PCR? */
    if (!(pkt[5] & 0x10)) return -1;     /* PCR_flag */
    const uint8_t *b = pkt + 6;
    int64_t base = ((int64_t)b[0] << 25)
                 | ((int64_t)b[1] << 17)
                 | ((int64_t)b[2] <<  9)
                 | ((int64_t)b[3] <<  1)
                 | ((int64_t)b[4] >>  7);
    int64_t ext  = (((int64_t)(b[4] & 1)) << 8) | b[5];
    return base * 300 + ext;
}

/* ------------------------------------------------------------------ */
/* Logging + time helpers                                              */
/* ------------------------------------------------------------------ */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&log_lock);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

static double monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_seconds(double s) {
    if (s <= 0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec > 999999999L) ts.tv_nsec = 999999999L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Bounded blocking queue of fixed-size datagrams                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  (*buf)[DGRAM_SIZE];
    int      cap;
    int      head;
    int      tail;
    int      size;
    pthread_mutex_t mu;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    bool     closed;
} dgram_q_t;

static void q_init(dgram_q_t *q, int cap) {
    q->buf = calloc((size_t)cap, DGRAM_SIZE);
    q->cap = cap;
    q->head = q->tail = q->size = 0;
    q->closed = false;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void q_destroy(dgram_q_t *q) {
    free(q->buf);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
}

/* Blocking put. Returns false if queue is closed. */
static bool q_put(dgram_q_t *q, const uint8_t *dg) {
    pthread_mutex_lock(&q->mu);
    while (q->size == q->cap && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mu);
    if (q->closed) { pthread_mutex_unlock(&q->mu); return false; }
    memcpy(q->buf[q->tail], dg, DGRAM_SIZE);
    q->tail = (q->tail + 1) % q->cap;
    q->size++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return true;
}

/* Non-blocking get. Returns false if empty. */
static bool q_try_get(dgram_q_t *q, uint8_t *out) {
    pthread_mutex_lock(&q->mu);
    if (q->size == 0) { pthread_mutex_unlock(&q->mu); return false; }
    memcpy(out, q->buf[q->head], DGRAM_SIZE);
    q->head = (q->head + 1) % q->cap;
    q->size--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return true;
}

static int q_size(dgram_q_t *q) {
    pthread_mutex_lock(&q->mu);
    int n = q->size;
    pthread_mutex_unlock(&q->mu);
    return n;
}

static void q_close(dgram_q_t *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

/* ------------------------------------------------------------------ */
/* OSD sender thread                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_t th;
    char      rx_ip[64];
    char      tx_ip[64];
    double    period;
    volatile bool stop;
    bool      started;
} osd_t;

static void build_osd_packet(uint8_t out[14], const char *tx_ip) {
    out[0] = 0x01;
    out[1] = 0x00;
    struct in_addr a;
    inet_pton(AF_INET, tx_ip, &a);
    memcpy(out + 2, &a.s_addr, 4);
    /* Little-endian ports: 7000, 7001, 7003, 7002 */
    static const uint16_t ports[4] = { 7000, 7001, 7003, 7002 };
    for (int i = 0; i < 4; i++) {
        out[6 + i * 2]     =  ports[i]       & 0xFF;
        out[6 + i * 2 + 1] = (ports[i] >> 8) & 0xFF;
    }
}

static void *osd_thread(void *arg) {
    osd_t *o = (osd_t *)arg;
    uint8_t pkt[14];
    build_osd_packet(pkt, o->tx_ip);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return NULL;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(OSD_PORT);
    inet_pton(AF_INET, o->rx_ip, &dst.sin_addr);

    while (!o->stop) {
        sendto(s, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&dst, sizeof(dst));
        /* Coarse interruptible sleep */
        double t_end = monotonic() + o->period;
        while (!o->stop && monotonic() < t_end)
            sleep_seconds(0.05);
    }
    close(s);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Reader thread: parse TS, splice SIT, pack datagrams                 */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_t  th;
    int        fd;                   /* input fd (stdin or ffmpeg stdout) */
    dgram_q_t *q;
    uint8_t    section[SIT_SECTION_LEN];
    int        section_len;
    double     sit_period;

    /* state */
    int        cc;
    volatile bool stop;
    volatile bool eof;

    /* PCR samples for rate estimation */
    pthread_mutex_t pcr_lock;
    int        pcr_n;
    int64_t    pcr_byte_off[PCR_SAMPLE_MAX];
    int64_t    pcr_val27[PCR_SAMPLE_MAX];

    /* stats */
    uint64_t   ts_in;
    uint64_t   nulls;
    uint64_t   replaced;
    uint64_t   inserted;
    uint64_t   dgrams;
} reader_t;

static void reader_make_sit(reader_t *r, uint8_t pkt[TS_PACKET_SIZE]) {
    build_sit_ts_packet(pkt, r->section, r->section_len, r->cc);
    r->cc = (r->cc + 1) & 0x0F;
}

/* Returns bitrate (bits/sec) or 0 if not enough samples / invalid. */
static double reader_estimate_rate(reader_t *r) {
    pthread_mutex_lock(&r->pcr_lock);
    int n = r->pcr_n;
    int64_t b0 = 0, p0 = 0, b1 = 0, p1 = 0;
    if (n >= 2) {
        b0 = r->pcr_byte_off[0];     p0 = r->pcr_val27[0];
        b1 = r->pcr_byte_off[n - 1]; p1 = r->pcr_val27[n - 1];
    }
    pthread_mutex_unlock(&r->pcr_lock);
    if (n < 2) return 0.0;
    int64_t dp = p1 - p0;
    if (dp <= 0) dp += (1LL << 33) * 300;  /* PCR wrap */
    if (dp <= 0 || b1 <= b0) return 0.0;
    return (double)(b1 - b0) * 8.0 * (double)PCR_HZ / (double)dp;
}

static void *reader_run(void *arg) {
    reader_t *r = (reader_t *)arg;

    /* Read buffer: up to DGRAM_SIZE * a few rounds */
    enum { BUF_CAP = DGRAM_SIZE * 8 };
    uint8_t *buf = malloc(BUF_CAP);
    int  buf_len = 0;

    uint8_t batch[PACKETS_PER_DGRAM][TS_PACKET_SIZE];
    int     batch_n = 0;

    int64_t byte_off = 0;
    double  now0     = monotonic();
    double  next_sit = now0;            /* SIT due time */
    bool    sit_pending = false;
    double  sit_due_at  = 0.0;
    bool    warned   = false;

    while (!r->stop) {
        int want = DGRAM_SIZE;
        if (BUF_CAP - buf_len < want) want = BUF_CAP - buf_len;
        if (want <= 0) {
            /* Buffer full and we haven't consumed - shouldn't happen, but
             * push through what we have rather than spinning. */
            want = 1;
        }
        ssize_t n = read(r->fd, buf + buf_len, (size_t)want);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        buf_len += (int)n;

        double now = monotonic();
        if (now >= next_sit && !sit_pending) {
            sit_pending = true;
            sit_due_at  = now;
            next_sit    = now + r->sit_period;
        }

        /* Drain whole TS packets */
        int pos = 0;
        while (buf_len - pos >= TS_PACKET_SIZE) {
            if (buf[pos] != 0x47) {
                /* Resync: find next 0x47 after this byte */
                int next = -1;
                for (int i = pos + 1; i < buf_len; i++) {
                    if (buf[i] == 0x47) { next = i; break; }
                }
                if (next < 0) { pos = buf_len; break; }
                pos = next;
                continue;
            }

            uint8_t pkt[TS_PACKET_SIZE];
            memcpy(pkt, buf + pos, TS_PACKET_SIZE);
            pos += TS_PACKET_SIZE;

            int64_t pcr = ts_pcr(pkt);
            if (pcr >= 0) {
                pthread_mutex_lock(&r->pcr_lock);
                if (r->pcr_n < PCR_SAMPLE_MAX) {
                    r->pcr_byte_off[r->pcr_n] = byte_off;
                    r->pcr_val27[r->pcr_n]    = pcr;
                    r->pcr_n++;
                }
                pthread_mutex_unlock(&r->pcr_lock);
            }

            int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
            if (pid == NULL_PID) {
                r->nulls++;
                if (sit_pending) {
                    reader_make_sit(r, pkt);
                    sit_pending = false;
                    r->replaced++;
                }
            }

            memcpy(batch[batch_n], pkt, TS_PACKET_SIZE);
            batch_n++;
            byte_off += TS_PACKET_SIZE;
            r->ts_in++;

            if (batch_n >= PACKETS_PER_DGRAM) {
                uint8_t dg[DGRAM_SIZE];
                for (int i = 0; i < PACKETS_PER_DGRAM; i++)
                    memcpy(dg + i * TS_PACKET_SIZE, batch[i], TS_PACKET_SIZE);
                if (!q_put(r->q, dg)) goto done;
                r->dgrams++;
                batch_n = 0;
            }
        }

        /* Compact buffer */
        if (pos > 0) {
            memmove(buf, buf + pos, (size_t)(buf_len - pos));
            buf_len -= pos;
        }

        /* SIT fallback: no null seen within 0.5s of SIT being due */
        if (sit_pending && monotonic() - sit_due_at > 0.5) {
            uint8_t pkt[TS_PACKET_SIZE];
            reader_make_sit(r, pkt);
            memcpy(batch[batch_n], pkt, TS_PACKET_SIZE);
            batch_n++;
            sit_pending = false;
            r->inserted++;
            if (!warned) {
                logf_("[lkv] WARNING: no null packets - inserting SIT. "
                      "Add -muxrate to ffmpeg for cleaner injection.\n");
                warned = true;
            }
            if (batch_n >= PACKETS_PER_DGRAM) {
                uint8_t dg[DGRAM_SIZE];
                for (int i = 0; i < PACKETS_PER_DGRAM; i++)
                    memcpy(dg + i * TS_PACKET_SIZE, batch[i], TS_PACKET_SIZE);
                if (!q_put(r->q, dg)) goto done;
                r->dgrams++;
                batch_n = 0;
            }
        }
    }

done:
    /* Flush partial batch padded with null TS packets */
    if (batch_n > 0) {
        while (batch_n < PACKETS_PER_DGRAM) {
            memcpy(batch[batch_n], NULL_TS_PKT, TS_PACKET_SIZE);
            batch_n++;
        }
        uint8_t dg[DGRAM_SIZE];
        for (int i = 0; i < PACKETS_PER_DGRAM; i++)
            memcpy(dg + i * TS_PACKET_SIZE, batch[i], TS_PACKET_SIZE);
        q_put(r->q, dg);
        r->dgrams++;
    }
    r->eof = true;
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Networking helpers                                                  */
/* ------------------------------------------------------------------ */

static bool is_multicast(const char *ip) {
    int a = 0;
    if (sscanf(ip, "%d", &a) != 1) return false;
    return a >= 224 && a <= 239;
}

static bool detect_local_ip(const char *peer, char out[64]) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(9);
    if (inet_pton(AF_INET, peer, &dst.sin_addr) != 1) {
        close(s);
        return false;
    }
    if (connect(s, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        close(s);
        strcpy(out, "0.0.0.0");
        return false;
    }
    struct sockaddr_in local;
    socklen_t ll = sizeof(local);
    if (getsockname(s, (struct sockaddr *)&local, &ll) != 0) {
        close(s);
        return false;
    }
    inet_ntop(AF_INET, &local.sin_addr, out, 64);
    close(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ------------------------------------------------------------------ */
/* Arguments                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Destination */
    const char *dst;
    int   port;

    /* Mode */
    int   mode;
    double fps;          /* NaN if unset */
    int   sit_width;     /* -1 = default */
    int   sit_height;    /* -1 = default */

    /* SIT tuning */
    int   audio_rate;
    int   extra;         /* -1 = default */
    int   flags;         /* -1 = derive from fps */
    int   byte15;
    double sit_period;

    /* Pacing */
    long long muxrate;
    int   prebuffer;
    double heartbeat_ms;
    int   tail_beats;

    /* OSD */
    const char *rx_ip;
    const char *osd_ip;
    bool  no_osd;

    /* Multicast */
    int   ttl;

    /* Diagnostics */
    bool  dump_sit;

    /* ffmpeg pass-through */
    int   ffmpeg_argc;
    char **ffmpeg_argv;
    bool  has_ffmpeg;
} args_t;

static void args_defaults(args_t *a) {
    memset(a, 0, sizeof(*a));
    a->port = 5004;
    a->mode = 14;
    a->fps  = NAN;
    a->sit_width  = -1;
    a->sit_height = -1;
    a->audio_rate = 48000;
    a->extra      = -1;
    a->flags      = -1;
    a->byte15     = 0xF0;
    a->sit_period = 0.2;
    a->muxrate    = 10000000LL;
    a->prebuffer  = 24;
    a->heartbeat_ms = 1.0;
    a->tail_beats = 20;
    a->ttl        = 16;
}

static int parse_int(const char *s, long long *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s || (end && *end != '\0')) return -1;
    *out = v;
    return 0;
}

static int parse_double(const char *s, double *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || (end && *end != '\0')) return -1;
    *out = v;
    return 0;
}

static void usage(const char *prog) {
    (void)prog;
    fprintf(stderr,
        "lkv373tx.c " LKV_VERSION " - LKV373A v3 HDMI-over-IP transmitter\n"
        "\n"
        "Required:\n"
        "  --dst IP           Receiver IP or multicast group\n"
        "Common:\n"
        "  --port N           UDP port (default 5004)\n"
        "  --mode N           HDMI output-mode index (0-33, default 14 = 1080p30)\n"
        "  --fps F            Frame rate you encode at (auto-sets skip-frame flag)\n"
        "  --muxrate BPS      TS mux rate / fallback pacing (default 10000000)\n"
        "Tuning:\n"
        "  --sit-width N      Override SIT width\n"
        "  --sit-height N     Override SIT height\n"
        "  --audio-rate N     SIT audio sample rate (default 48000)\n"
        "  --extra N          SIT extra = refresh*1000 (default from --mode)\n"
        "  --flags 0xNN       Raw SIT flags byte (overrides --fps)\n"
        "  --byte15 0xNN      SIT audio-codec byte (default 0xF0)\n"
        "  --sit-period S     Seconds between SITs (default 0.2)\n"
        "  --prebuffer N      Datagrams to buffer before paced send (default 24)\n"
        "  --heartbeat-ms F   Min ms between paced sends before keep-alive (default 1.0)\n"
        "  --tail-beats N     Keep-alives sent after EOF (default 20)\n"
        "OSD:\n"
        "  --rx-ip IP         Receiver IP for OSD (defaults to --dst if unicast)\n"
        "  --osd-ip IP        TX IP advertised in OSD (auto-detected)\n"
        "  --no-osd           Disable OSD announcements\n"
        "Multicast:\n"
        "  --ttl N            Multicast TTL (default 16)\n"
        "Diagnostics:\n"
        "  --dump-sit         Print built SIT section as hex\n"
        "  --version          Print version and exit\n"
        "ffmpeg passthrough:\n"
        "  --ffmpeg ARGS...   Run ffmpeg internally; everything after this is\n"
        "                     the ffmpeg command (binary name optional).\n"
        "                     Output section (-muxrate N -f mpegts pipe:1) is\n"
        "                     appended automatically. Must come last.\n"
        "\n"
        "If --ffmpeg is not given, MPEG-TS is read from stdin.\n");
}

/* Returns 0 on success, nonzero to exit. */
static int parse_args(int argc, char **argv, args_t *a) {
    args_defaults(a);

    int i = 1;
    while (i < argc) {
        const char *k = argv[i];

        if (!strcmp(k, "--help") || !strcmp(k, "-h")) {
            usage(argv[0]);
            return 2;
        }
        if (!strcmp(k, "--version")) {
            printf("lkv373tx.c " LKV_VERSION "\n");
            return 2;
        }
        if (!strcmp(k, "--no-osd")) { a->no_osd = true; i++; continue; }
        if (!strcmp(k, "--dump-sit")) { a->dump_sit = true; i++; continue; }
        if (!strcmp(k, "--ffmpeg")) {
            a->has_ffmpeg = true;
            a->ffmpeg_argc = argc - (i + 1);
            a->ffmpeg_argv = (a->ffmpeg_argc > 0) ? &argv[i + 1] : NULL;
            return 0;  /* everything else belongs to ffmpeg */
        }

        /* Two-arg options */
        if (i + 1 >= argc) {
            fprintf(stderr, "[lkv] error: %s needs a value\n", k);
            return 1;
        }
        const char *v = argv[i + 1];
        long long ll;
        double dd;

        if      (!strcmp(k, "--dst"))         { a->dst = v; }
        else if (!strcmp(k, "--port"))        { if (parse_int(v, &ll)) goto bad; a->port = (int)ll; }
        else if (!strcmp(k, "--mode"))        { if (parse_int(v, &ll)) goto bad; a->mode = (int)ll; }
        else if (!strcmp(k, "--fps"))         { if (parse_double(v, &dd)) goto bad; a->fps = dd; }
        else if (!strcmp(k, "--sit-width"))   { if (parse_int(v, &ll)) goto bad; a->sit_width = (int)ll; }
        else if (!strcmp(k, "--sit-height"))  { if (parse_int(v, &ll)) goto bad; a->sit_height = (int)ll; }
        else if (!strcmp(k, "--audio-rate"))  { if (parse_int(v, &ll)) goto bad; a->audio_rate = (int)ll; }
        else if (!strcmp(k, "--extra"))       { if (parse_int(v, &ll)) goto bad; a->extra = (int)ll; }
        else if (!strcmp(k, "--flags"))       { if (parse_int(v, &ll)) goto bad; a->flags = (int)ll; }
        else if (!strcmp(k, "--byte15"))      { if (parse_int(v, &ll)) goto bad; a->byte15 = (int)ll; }
        else if (!strcmp(k, "--sit-period"))  { if (parse_double(v, &dd)) goto bad; a->sit_period = dd; }
        else if (!strcmp(k, "--muxrate"))     { if (parse_int(v, &ll)) goto bad; a->muxrate = ll; }
        else if (!strcmp(k, "--prebuffer"))   { if (parse_int(v, &ll)) goto bad; a->prebuffer = (int)ll; }
        else if (!strcmp(k, "--heartbeat-ms")){ if (parse_double(v, &dd)) goto bad; a->heartbeat_ms = dd; }
        else if (!strcmp(k, "--tail-beats"))  { if (parse_int(v, &ll)) goto bad; a->tail_beats = (int)ll; }
        else if (!strcmp(k, "--rx-ip"))       { a->rx_ip = v; }
        else if (!strcmp(k, "--osd-ip"))      { a->osd_ip = v; }
        else if (!strcmp(k, "--ttl"))         { if (parse_int(v, &ll)) goto bad; a->ttl = (int)ll; }
        else {
            fprintf(stderr, "[lkv] error: unknown option: %s\n", k);
            return 1;
        }
        i += 2;
        continue;
bad:
        fprintf(stderr, "[lkv] error: bad value for %s: %s\n", k, v);
        return 1;
    }

    if (!a->dst) {
        fprintf(stderr, "[lkv] error: --dst is required\n");
        return 1;
    }
    if (a->mode < 0 || a->mode >= MAX_MODES || !MODE_TABLE[a->mode].label) {
        fprintf(stderr, "[lkv] error: --mode out of range (0-33)\n");
        return 1;
    }
    return 0;
}

/* Resolve SIT flags from --flags / --fps / default. Returns -1 on fatal. */
static int resolve_flags(args_t *a) {
    if (a->flags >= 0) {
        if (!isnan(a->fps))
            logf_("[lkv] note: --flags is set; --fps ignored for the "
                  "skip-frame bit\n");
        return a->flags;
    }
    if (isnan(a->fps)) {
        logf_("[lkv] note: no --fps given - assuming skip-frame on (encode "
              "at half the mode rate). Pass --fps for automatic handling.\n");
        return FLAGS_DEFAULT;
    }
    if (a->fps <= 0) {
        fprintf(stderr, "[lkv] error: --fps must be positive\n");
        return -1;
    }
    double disp = mode_displayed_fps(a->mode);
    double ratio = disp / a->fps;
    int skip;
    if (fabs(ratio - 1.0) <= 0.1)       skip = 0;
    else if (fabs(ratio - 2.0) <= 0.1)  skip = 1;
    else {
        fprintf(stderr,
            "[lkv] error: --fps %g does not fit --mode %d (%s).\n"
            "            That mode puts %g frames/s on screen - encode at "
            "%g fps (1:1)\n"
            "            or %g fps (the receiver doubles it).\n",
            a->fps, a->mode, MODE_TABLE[a->mode].label,
            disp, disp, disp / 2.0);
        return -1;
    }
    return FLAGS_BASE | (skip << 6);
}

/* ------------------------------------------------------------------ */
/* ffmpeg subprocess                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    pid_t pid;
    int   stdout_fd;
    int   stderr_fd;
    pthread_t stderr_relay;
    bool  has_stderr_relay;
} ff_proc_t;

static void *stderr_relay_fn(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    char buf[4096];
    char line[4096];
    int  ll = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (ll < (int)sizeof(line) - 1) line[ll++] = buf[i];
            if (buf[i] == '\n' || ll >= (int)sizeof(line) - 1) {
                line[ll] = '\0';
                logf_("[ffmpeg] %s%s", line, (buf[i] == '\n') ? "" : "\n");
                ll = 0;
            }
        }
    }
    if (ll > 0) { line[ll] = '\0'; logf_("[ffmpeg] %s\n", line); }
    return NULL;
}

/* Build the ffmpeg argv. Returns malloc'd NULL-terminated argv,
 * caller frees the outer array (string pointers are not owned). */
static char **build_ffmpeg_cmd(const char *bin, char **uargs, int n_uargs,
                               long long muxrate, char *mux_buf, size_t mux_buf_sz)
{
    /* Detect tail = pipe target already supplied */
    bool tail_pipe = (n_uargs > 0 &&
        (!strcmp(uargs[n_uargs - 1], "pipe:1") ||
         !strcmp(uargs[n_uargs - 1], "pipe:")  ||
         !strcmp(uargs[n_uargs - 1], "-")));

    bool has_muxrate = false;
    for (int i = 0; i < n_uargs; i++)
        if (!strcmp(uargs[i], "-muxrate")) { has_muxrate = true; break; }

    int extra = 0;
    if (!tail_pipe) {
        if (!has_muxrate) extra += 2; /* -muxrate N */
        extra += 3;                   /* -f mpegts pipe:1 */
    }

    char **out = calloc((size_t)(1 + n_uargs + extra + 1), sizeof(char *));
    int o = 0;
    out[o++] = (char *)bin;
    for (int i = 0; i < n_uargs; i++) out[o++] = uargs[i];
    if (!tail_pipe) {
        if (!has_muxrate) {
            snprintf(mux_buf, mux_buf_sz, "%lld", muxrate);
            out[o++] = "-muxrate";
            out[o++] = mux_buf;
        }
        out[o++] = "-f";
        out[o++] = "mpegts";
        out[o++] = "pipe:1";
    }
    out[o] = NULL;
    return out;
}

/* Spawn ffmpeg. Returns 0 on success. */
static int spawn_ffmpeg(char **cmd, ff_proc_t *out) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) return -1;
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* child */
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        /* Detach from controlling tty signals so Ctrl+C only hits parent
         * once - parent will tear ffmpeg down explicitly. */
        execvp(cmd[0], cmd);
        fprintf(stderr, "[lkv] exec %s failed: %s\n", cmd[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    out->pid       = pid;
    out->stdout_fd = out_pipe[0];
    out->stderr_fd = err_pipe[0];

    int *farg = malloc(sizeof(int));
    *farg = out->stderr_fd;
    if (pthread_create(&out->stderr_relay, NULL, stderr_relay_fn, farg) == 0)
        out->has_stderr_relay = true;
    else {
        free(farg);
        out->has_stderr_relay = false;
    }
    return 0;
}

static void stop_ffmpeg(ff_proc_t *ff) {
    if (ff->pid <= 0) return;
    int status;
    if (waitpid(ff->pid, &status, WNOHANG) == 0) {
        kill(ff->pid, SIGTERM);
        /* Wait up to 3 seconds */
        for (int i = 0; i < 60; i++) {
            if (waitpid(ff->pid, &status, WNOHANG) != 0) break;
            sleep_seconds(0.05);
        }
        if (waitpid(ff->pid, &status, WNOHANG) == 0) {
            kill(ff->pid, SIGKILL);
            waitpid(ff->pid, &status, 0);
        }
    }
    if (ff->stdout_fd >= 0) { close(ff->stdout_fd); ff->stdout_fd = -1; }
    if (ff->has_stderr_relay) {
        pthread_join(ff->stderr_relay, NULL);
        ff->has_stderr_relay = false;
    }
    if (ff->stderr_fd >= 0) { close(ff->stderr_fd); ff->stderr_fd = -1; }
}

/* ------------------------------------------------------------------ */
/* Core sender                                                          */
/* ------------------------------------------------------------------ */

static int run_sender(args_t *a, int in_fd, ff_proc_t *ff) {
    const mode_t_ *m = &MODE_TABLE[a->mode];
    int sit_w = (a->sit_width  >= 0) ? a->sit_width  : m->width;
    int sit_h = (a->sit_height >= 0) ? a->sit_height : m->height;
    int extra = (a->extra      >= 0) ? a->extra      : mode_extra(a->mode);

    uint8_t section[SIT_SECTION_LEN];
    int section_len = build_sit_section(section, sit_w, sit_h, a->mode,
                                        (uint32_t)a->audio_rate,
                                        (uint32_t)extra,
                                        (uint8_t)a->flags,
                                        (uint8_t)a->byte15, 0);

    double hb_interval = a->heartbeat_ms / 1000.0;
    if (hb_interval < 0.0002) hb_interval = 0.0002;

    int  skipframe = (a->flags >> 6) & 1;
    double disp = mode_displayed_fps(a->mode);
    double expect_fps = skipframe ? disp / 2.0 : disp;

    logf_("[lkv] mode %d (%s)  SIT %dx%d  flags 0x%02X (skip-frame %s)\n",
          a->mode, m->label, sit_w, sit_h, a->flags, skipframe ? "on" : "off");
    logf_("[lkv] genlock %.3f Hz, expecting ~%g fps input  ->  %s:%d\n",
          extra / 1000.0, expect_fps, a->dst, a->port);

    if (a->dump_sit) {
        char hex[SIT_SECTION_LEN * 3 + 1];
        for (int i = 0; i < SIT_SECTION_LEN; i++)
            snprintf(hex + i * 3, 4, "%02x ", section[i]);
        logf_("[lkv] SIT section: %s\n", hex);
    }

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { logf_("[lkv] socket: %s\n", strerror(errno)); return 1; }
    if (is_multicast(a->dst)) {
        unsigned char ttl = (unsigned char)a->ttl;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    } else {
        int on = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }
    /* Bind source port to match real TX hardware. */
    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_port = htons(a->port);
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&src, sizeof(src)) != 0) {
        logf_("[lkv] note: could not bind src port %d (%s) - using ephemeral\n",
              a->port, strerror(errno));
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(a->port);
    inet_pton(AF_INET, a->dst, &dst.sin_addr);

    /* OSD */
    osd_t osd_ctx; memset(&osd_ctx, 0, sizeof(osd_ctx));
    if (!a->no_osd) {
        const char *rx_ip = a->rx_ip;
        if (!rx_ip && !is_multicast(a->dst)) rx_ip = a->dst;
        if (rx_ip) {
            char auto_tx[64];
            const char *tx_ip = a->osd_ip;
            if (!tx_ip) {
                if (detect_local_ip(rx_ip, auto_tx)) tx_ip = auto_tx;
                else tx_ip = "0.0.0.0";
            }
            size_t rxn = strlen(rx_ip);
            if (rxn >= sizeof(osd_ctx.rx_ip)) rxn = sizeof(osd_ctx.rx_ip) - 1;
            memcpy(osd_ctx.rx_ip, rx_ip, rxn);
            osd_ctx.rx_ip[rxn] = '\0';
            size_t txn = strlen(tx_ip);
            if (txn >= sizeof(osd_ctx.tx_ip)) txn = sizeof(osd_ctx.tx_ip) - 1;
            memcpy(osd_ctx.tx_ip, tx_ip, txn);
            osd_ctx.tx_ip[txn] = '\0';
            osd_ctx.period = 1.08;
            if (pthread_create(&osd_ctx.th, NULL, osd_thread, &osd_ctx) == 0) {
                osd_ctx.started = true;
                logf_("[lkv] OSD: announcing %s to %s:%d\n",
                      tx_ip, rx_ip, OSD_PORT);
            }
        } else {
            logf_("[lkv] OSD disabled (multicast --dst, no --rx-ip)\n");
        }
    }

    /* Reader thread */
    dgram_q_t q; q_init(&q, QUEUE_CAP);
    reader_t r;
    memset(&r, 0, sizeof(r));
    r.fd = in_fd;
    r.q  = &q;
    memcpy(r.section, section, section_len);
    r.section_len = section_len;
    r.sit_period  = a->sit_period;
    pthread_mutex_init(&r.pcr_lock, NULL);
    pthread_create(&r.th, NULL, reader_run, &r);

    /* Signals */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Pacing state */
    double interval = (double)DGRAM_SIZE * 8.0 / (double)a->muxrate;
    bool   latched  = false;
    double t0       = 0.0;
    int64_t seq     = 0;
    double  next_hb = monotonic();
    uint64_t sent_data = 0, sent_empty = 0, underflows = 0;

    while (!g_quit) {
        double now = monotonic();

        if (!latched) {
            if (q_size(&q) >= a->prebuffer) {
                double rate = reader_estimate_rate(&r);
                const char *src;
                if (rate >= 250000.0 && rate <= 60000000.0) {
                    interval = (double)DGRAM_SIZE * 8.0 / rate;
                    src = "measured from PCR";
                } else {
                    rate = (double)a->muxrate;
                    src = "from --muxrate (PCR not seen yet)";
                }
                t0 = monotonic();
                latched = true;
                logf_("[lkv] pacing at %.3f Mbps (%s) - "
                      "%.3f ms per datagram\n",
                      rate / 1e6, src, interval * 1000.0);
            } else if (r.eof && q_size(&q) == 0) {
                break;
            } else {
                if (now >= next_hb) {
                    sendto(sock, "", 0, 0, (struct sockaddr *)&dst, sizeof(dst));
                    sent_empty++;
                    next_hb = now + hb_interval;
                }
                sleep_seconds(0.0005);
                continue;
            }
        }

        if (r.eof && q_size(&q) == 0) break;

        double due = t0 + (double)seq * interval;
        if (now >= due) {
            uint8_t dg[DGRAM_SIZE];
            if (q_try_get(&q, dg)) {
                sendto(sock, dg, DGRAM_SIZE, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
                sent_data++;
                seq++;
                next_hb = monotonic() + hb_interval;
            } else {
                /* Reader fell behind: slip schedule one slot and heartbeat */
                t0 += interval;
                underflows++;
                if (now >= next_hb) {
                    sendto(sock, "", 0, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
                    sent_empty++;
                    next_hb = now + hb_interval;
                }
                sleep_seconds(0.0003);
            }
        } else {
            if (now >= next_hb) {
                sendto(sock, "", 0, 0, (struct sockaddr *)&dst, sizeof(dst));
                sent_empty++;
                next_hb = now + hb_interval;
            }
            double target = due < next_hb ? due : next_hb;
            double slp = target - monotonic();
            if (slp > 0.002) slp = 0.002;
            if (slp > 0) sleep_seconds(slp);
        }
    }

    /* Shutdown */
    r.stop = true;
    q_close(&q);
    pthread_join(r.th, NULL);

    if (osd_ctx.started) {
        osd_ctx.stop = true;
        pthread_join(osd_ctx.th, NULL);
    }

    /* Tail heartbeats */
    for (int i = 0; i < a->tail_beats; i++) {
        sendto(sock, "", 0, 0, (struct sockaddr *)&dst, sizeof(dst));
        sleep_seconds(hb_interval);
    }
    close(sock);

    logf_("[lkv] done. %llu data + %llu keep-alive datagrams, %llu underflows | "
          "TS: %llu pkts, %llu nulls, SIT %llu replaced + %llu inserted\n",
          (unsigned long long)sent_data,
          (unsigned long long)sent_empty,
          (unsigned long long)underflows,
          (unsigned long long)r.ts_in,
          (unsigned long long)r.nulls,
          (unsigned long long)r.replaced,
          (unsigned long long)r.inserted);

    q_destroy(&q);
    pthread_mutex_destroy(&r.pcr_lock);

    if (ff) stop_ffmpeg(ff);

    if (sent_data == 0) {
        logf_("[lkv] error: no data datagrams sent - check the ffmpeg "
              "command and its input.\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    init_null_ts_pkt();

    args_t a;
    int rc = parse_args(argc, argv, &a);
    if (rc != 0) return (rc == 2) ? 0 : rc;

    int flags = resolve_flags(&a);
    if (flags < 0) return 1;
    a.flags = flags;

    if (!a.has_ffmpeg) {
        logf_("[lkv] lkv373tx.c " LKV_VERSION " - pipe mode (reading stdin)\n");
        return run_sender(&a, STDIN_FILENO, NULL);
    }

    /* ffmpeg mode */
    if (a.ffmpeg_argc <= 0) {
        fprintf(stderr,
            "[lkv] error: --ffmpeg needs an ffmpeg command, e.g.\n"
            "             --ffmpeg -re -i input.mp4 -c:v libx264 -b:v 8M\n");
        return 1;
    }

    const char *bin;
    char **uargs;
    int n_uargs;
    if (a.ffmpeg_argv[0][0] == '-') {
        bin = "ffmpeg";
        uargs = a.ffmpeg_argv;
        n_uargs = a.ffmpeg_argc;
    } else {
        bin = a.ffmpeg_argv[0];
        uargs = &a.ffmpeg_argv[1];
        n_uargs = a.ffmpeg_argc - 1;
        if (n_uargs <= 0) {
            fprintf(stderr,
                "[lkv] error: --ffmpeg: no arguments after binary name\n");
            return 1;
        }
    }

    char mux_buf[32];
    char **cmd = build_ffmpeg_cmd(bin, uargs, n_uargs, a.muxrate,
                                  mux_buf, sizeof(mux_buf));

    /* Print resolved ffmpeg command */
    {
        char line[2048];
        int  ll = 0;
        for (int i = 0; cmd[i]; i++) {
            int n = snprintf(line + ll, sizeof(line) - ll,
                             "%s%s", (i ? " " : ""), cmd[i]);
            if (n <= 0 || ll + n >= (int)sizeof(line)) break;
            ll += n;
        }
        logf_("[lkv] lkv373tx.c " LKV_VERSION " - ffmpeg mode\n");
        logf_("[lkv] ffmpeg: %s\n", line);
    }

    ff_proc_t ff;
    memset(&ff, 0, sizeof(ff));
    if (spawn_ffmpeg(cmd, &ff) != 0) {
        free(cmd);
        fprintf(stderr, "[lkv] error: could not start ffmpeg: %s\n",
                strerror(errno));
        return 1;
    }
    free(cmd);

    int rc2 = run_sender(&a, ff.stdout_fd, &ff);
    return rc2;
}
