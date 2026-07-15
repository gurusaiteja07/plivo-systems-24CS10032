/* RECEIVER — matches sender.c's wire format (group XOR parity + NACK retx).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from our sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> NACKs to our sender, via the relay
 *
 * Wire format from the sender (see sender.c):
 *   uint32_t seq          (network byte order)
 *   uint8_t  type          0 = DATA, 1 = PARITY, 2 = RETX
 *   uint8_t  group_size    used only for PARITY
 *   [160 bytes payload]    for DATA/RETX: the frame; for PARITY: XOR of group
 *
 * Strategy:
 *   - DATA/RETX frames are forwarded to the player the instant they arrive
 *     (first-arrival wins; the harness itself ignores duplicates).
 *   - Frames are tracked per FEC group (must match sender's GROUP_SIZE).
 *     Once a group's parity has arrived and exactly one member is still
 *     missing, we reconstruct it locally (XOR) and forward it -- zero
 *     round-trip cost.
 *   - For anything FEC can't fix (2+ losses in a group, or parity itself
 *     lost), we fall back to sending a NACK back to the sender. NACKs are
 *     paced (small initial wait so in-flight/reorderable packets and FEC
 *     get a chance first) and retried a bounded number of times, backing
 *     off once we're within one RTT-ish margin of the frame's own
 *     playout deadline (no point spending bandwidth we can't use in time).
 *   - No separate jitter-buffer/smoothing pass is needed: the harness only
 *     checks "did frame i arrive before its deadline", not steady pacing,
 *     so recovered frames are pushed out as soon as they're ready.
 *
 * Env vars: T0 (epoch seconds, float), DURATION_S, DELAY_MS.
 * Build: make   Run: python3 run.py --delay_ms 60
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PAYLOAD_BYTES 160
#define GROUP_SIZE 4              /* must match sender.c */
#define HISTORY_SIZE 200          /* ring buffer, ~4s at 20ms/frame */
#define NUM_GROUPS (HISTORY_SIZE / GROUP_SIZE)
#define LOOKBACK 60               /* how far behind max_seq_seen we scan for gaps */

#define TYPE_DATA 0
#define TYPE_PARITY 1
#define TYPE_RETX 2

#define NACK_INITIAL_DELAY_S 0.010   /* wait this long after a gap is known before first NACK */
#define NACK_RETRY_S 0.030           /* resend NACK this often while still missing */
#define NACK_MAX_TRIES 4
#define DEADLINE_MARGIN_S 0.015      /* stop nacking once this close to the frame's deadline */

#pragma pack(push, 1)
typedef struct {
    uint32_t seq;
    uint8_t type;
    uint8_t group_size;
} pkt_hdr_t;

typedef struct {
    uint32_t seq;
} nack_pkt_t;
#pragma pack(pop)

#define HDR_LEN (sizeof(pkt_hdr_t))
#define PKT_LEN (HDR_LEN + PAYLOAD_BYTES)

typedef struct {
    uint32_t seq;      /* which seq this slot currently represents */
    int inited;         /* has this slot been claimed for `seq` yet */
    int forwarded;
    int have_payload;
    unsigned char payload[PAYLOAD_BYTES];
    double first_seen;      /* when we first learned this seq should exist */
    double last_nack_sent;  /* 0 = never */
    int nack_count;
} frame_slot_t;

typedef struct {
    int inited;
    uint32_t group_start;
    int have_parity;
    unsigned char parity[PAYLOAD_BYTES];
    uint8_t group_size;
} group_slot_t;

static frame_slot_t frames[HISTORY_SIZE];
static group_slot_t groups[NUM_GROUPS];

static int player_fd;
static struct sockaddr_in player_addr;
static int nack_fd;
static struct sockaddr_in nack_relay_addr;

static double t0 = -1.0, delay_ms = 0.0;
static int64_t max_seq_seen = -1;

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static frame_slot_t *slot_for(uint32_t seq, double t) {
    frame_slot_t *s = &frames[seq % HISTORY_SIZE];
    if (!s->inited || s->seq != seq) {
        s->inited = 1;
        s->seq = seq;
        s->forwarded = 0;
        s->have_payload = 0;
        s->first_seen = t;
        s->last_nack_sent = 0;
        s->nack_count = 0;
    }
    return s;
}

static group_slot_t *group_for(uint32_t group_start) {
    group_slot_t *g = &groups[(group_start / GROUP_SIZE) % NUM_GROUPS];
    if (!g->inited || g->group_start != group_start) {
        g->inited = 1;
        g->group_start = group_start;
        g->have_parity = 0;
        g->group_size = 0;
    }
    return g;
}

static void forward_to_player(uint32_t seq, const unsigned char *payload) {
    frame_slot_t *s = slot_for(seq, now_s());
    if (s->forwarded) return;
    unsigned char out[4 + PAYLOAD_BYTES];
    uint32_t nseq = htonl(seq);
    memcpy(out, &nseq, 4);
    memcpy(out + 4, payload, PAYLOAD_BYTES);
    sendto(player_fd, out, sizeof out, 0, (struct sockaddr *)&player_addr,
           sizeof player_addr);
    s->forwarded = 1;
    s->have_payload = 1;
    memcpy(s->payload, payload, PAYLOAD_BYTES);
    if ((int64_t)seq > max_seq_seen) max_seq_seen = (int64_t)seq;
}

static void note_seen(uint32_t seq) {
    /* record that seq exists in the stream even if we don't have its
     * payload yet, so the gap-scanner knows to look for it */
    slot_for(seq, now_s());
    if ((int64_t)seq > max_seq_seen) max_seq_seen = (int64_t)seq;
}

/* try to reconstruct the single missing member of a group via XOR */
static void try_group_recovery(uint32_t group_start) {
    group_slot_t *g = group_for(group_start);
    if (!g->have_parity || g->group_size == 0) return;

    int missing_idx = -1, missing_count = 0;
    unsigned char acc[PAYLOAD_BYTES];
    memcpy(acc, g->parity, PAYLOAD_BYTES);

    for (int k = 0; k < g->group_size; k++) {
        uint32_t seq = group_start + k;
        frame_slot_t *s = &frames[seq % HISTORY_SIZE];
        if (s->inited && s->seq == seq && s->have_payload) {
            for (int b = 0; b < PAYLOAD_BYTES; b++) acc[b] ^= s->payload[b];
        } else {
            missing_idx = k;
            missing_count++;
        }
    }
    if (missing_count == 1) {
        forward_to_player(group_start + missing_idx, acc);
    }
    /* missing_count == 0: nothing to do. missing_count >= 2: FEC can't
     * help this group; NACK path (below) is the only remaining remedy. */
}

static void send_nack(uint32_t seq) {
    nack_pkt_t pkt;
    pkt.seq = htonl(seq);
    sendto(nack_fd, &pkt, sizeof pkt, 0, (struct sockaddr *)&nack_relay_addr,
           sizeof nack_relay_addr);
}

/* deadline for frame i, or +inf if we don't have t0/delay_ms yet */
static double deadline_for(uint32_t seq) {
    if (t0 < 0) return 1e18;
    return t0 + delay_ms / 1000.0 + (double)seq * 0.020;
}

/* scan the recent window for frames we believe exist but haven't arrived,
 * and NACK them (paced, bounded, and stopped once too close to deadline) */
static void scan_for_gaps(void) {
    if (max_seq_seen < 0) return;
    int64_t lo = max_seq_seen - LOOKBACK;
    if (lo < 0) lo = 0;
    double t = now_s();

    for (int64_t seq = lo; seq <= max_seq_seen; seq++) {
        frame_slot_t *s = &frames[seq % HISTORY_SIZE];
        if (!s->inited || s->seq != (uint32_t)seq) continue; /* never noted */
        if (s->forwarded) continue;

        double dl = deadline_for((uint32_t)seq);
        if (t + DEADLINE_MARGIN_S >= dl) continue; /* too late to help */
        if (t - s->first_seen < NACK_INITIAL_DELAY_S) continue; /* give FEC a chance */
        if (s->nack_count >= NACK_MAX_TRIES) continue;
        if (s->last_nack_sent != 0 && t - s->last_nack_sent < NACK_RETRY_S) continue;

        send_nack((uint32_t)seq);
        s->last_nack_sent = t;
        s->nack_count++;
    }
}

int main(void) {
    const char *e;
    if ((e = getenv("T0"))) t0 = atof(e);
    if ((e = getenv("DELAY_MS"))) delay_ms = atof(e);

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&player_addr, 0, sizeof player_addr);
    player_addr.sin_family = AF_INET;
    player_addr.sin_port = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&nack_relay_addr, 0, sizeof nack_relay_addr);
    nack_relay_addr.sin_family = AF_INET;
    nack_relay_addr.sin_port = htons(47003);
    nack_relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 5000; /* 5ms: periodic gap scan cadence */
        int rv = select(in_fd + 1, &rfds, NULL, NULL, &tv);

        if (rv > 0 && FD_ISSET(in_fd, &rfds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n == (ssize_t)PKT_LEN) {
                pkt_hdr_t hdr;
                memcpy(&hdr, buf, HDR_LEN);
                uint32_t seq = ntohl(hdr.seq);
                unsigned char *payload = buf + HDR_LEN;

                if (hdr.type == TYPE_DATA || hdr.type == TYPE_RETX) {
                    forward_to_player(seq, payload);
                    uint32_t group_start = seq - (seq % GROUP_SIZE);
                    try_group_recovery(group_start);
                } else if (hdr.type == TYPE_PARITY) {
                    uint32_t group_start = seq;
                    uint8_t gsize = hdr.group_size ? hdr.group_size : GROUP_SIZE;
                    group_slot_t *g = group_for(group_start);
                    g->have_parity = 1;
                    g->group_size = gsize;
                    memcpy(g->parity, payload, PAYLOAD_BYTES);
                    /* parity tells us the whole group exists, even for
                     * members we haven't seen yet -- note them so the
                     * gap scanner can NACK them if FEC can't recover */
                    for (int k = 0; k < gsize; k++) note_seen(group_start + k);
                    try_group_recovery(group_start);
                }
                /* unknown type: ignore */
            }
            /* malformed length: ignore */
        }

        scan_for_gaps();
    }
    return 0;
}
