/* SENDER — rewritten with FEC (group XOR parity) + NACK-based retransmit.
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i here at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (our wire format below)
 *   bind 47004  <- feedback (NACKs) from our receiver, via the relay
 *
 * Wire format to the relay (our own design):
 *   uint32_t seq          (network byte order)
 *   uint8_t  type          0 = DATA, 1 = PARITY, 2 = RETX
 *   uint8_t  group_size    used only for PARITY
 *   [160 bytes payload]    for DATA/RETX: the frame; for PARITY: XOR of group
 *
 * Strategy:
 *   - Every frame is sent once as DATA immediately (no added delay).
 *   - Frames are grouped in fixed-size groups (default 4). At the end of
 *     each group we send one PARITY packet = XOR of that group's payloads.
 *     A single lost frame in a group is then reconstructable by the
 *     receiver with zero round-trip cost. Overhead: +1/group_size (25% at
 *     group=4), well under the 2.0x cap.
 *   - A short history ring buffer lets us serve NACKs (sent by the
 *     receiver on 47003 -> relay -> our 47004) as a backup for frames FEC
 *     couldn't recover (e.g. 2+ losses in the same group).
 *
 * Build: make   Run: python3 run.py --delay_ms 60
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_BYTES 160
#define GROUP_SIZE 4              /* frames per FEC group; tune if needed */
#define HISTORY_SIZE 150          /* ~3s of history at 20ms/frame */

#define TYPE_DATA 0
#define TYPE_PARITY 1
#define TYPE_RETX 2

#pragma pack(push, 1)
typedef struct {
    uint32_t seq;
    uint8_t type;
    uint8_t group_size;
} pkt_hdr_t;
#pragma pack(pop)

#define HDR_LEN (sizeof(pkt_hdr_t))
#define PKT_LEN (HDR_LEN + PAYLOAD_BYTES)

/* ---- shared history buffer (for NACK retransmit) ---- */
typedef struct {
    int valid;
    uint32_t seq;
    unsigned char payload[PAYLOAD_BYTES];
} history_slot_t;

static history_slot_t history[HISTORY_SIZE];
static pthread_mutex_t history_lock = PTHREAD_MUTEX_INITIALIZER;

static int out_fd; /* shared UDP socket, sender -> relay (47001) */
static struct sockaddr_in relay_addr;

static void history_put(uint32_t seq, const unsigned char *payload) {
    pthread_mutex_lock(&history_lock);
    history_slot_t *slot = &history[seq % HISTORY_SIZE];
    slot->valid = 1;
    slot->seq = seq;
    memcpy(slot->payload, payload, PAYLOAD_BYTES);
    pthread_mutex_unlock(&history_lock);
}

/* returns 1 and fills out_payload if found, else 0 */
static int history_get(uint32_t seq, unsigned char *out_payload) {
    int found = 0;
    pthread_mutex_lock(&history_lock);
    history_slot_t *slot = &history[seq % HISTORY_SIZE];
    if (slot->valid && slot->seq == seq) {
        memcpy(out_payload, slot->payload, PAYLOAD_BYTES);
        found = 1;
    }
    pthread_mutex_unlock(&history_lock);
    return found;
}

static void send_pkt(uint32_t seq, uint8_t type, uint8_t group_size,
                      const unsigned char *payload) {
    unsigned char buf[PKT_LEN];
    pkt_hdr_t hdr;
    hdr.seq = htonl(seq);
    hdr.type = type;
    hdr.group_size = group_size;
    memcpy(buf, &hdr, HDR_LEN);
    memcpy(buf + HDR_LEN, payload, PAYLOAD_BYTES);
    sendto(out_fd, buf, PKT_LEN, 0, (struct sockaddr *)&relay_addr,
           sizeof relay_addr);
}

/* ---- feedback thread: listen for NACKs on 47004, resend from history --- */
typedef struct {
    uint32_t seq;
} nack_pkt_t;

static void *feedback_loop(void *arg) {
    (void)arg;
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof fb_addr) < 0) {
        perror("bind 47004");
        return NULL;
    }

    unsigned char buf[64];
    for (;;) {
        ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < (ssize_t)sizeof(nack_pkt_t)) continue;
        nack_pkt_t nack;
        memcpy(&nack, buf, sizeof nack);
        uint32_t seq = ntohl(nack.seq);

        unsigned char payload[PAYLOAD_BYTES];
        if (history_get(seq, payload)) {
            send_pkt(seq, TYPE_RETX, 0, payload);
        }
        /* if not in history, it's too old / already delivered — ignore */
    }
    return NULL;
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&relay_addr, 0, sizeof relay_addr);
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(47001);
    relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    pthread_t fb_thread;
    pthread_create(&fb_thread, NULL, feedback_loop, NULL);
    pthread_detach(fb_thread);

    unsigned char group_xor[PAYLOAD_BYTES];
    memset(group_xor, 0, sizeof group_xor);
    uint32_t group_start = 0;
    int have_group_start = 0;

    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n != (ssize_t)(4 + PAYLOAD_BYTES)) continue; /* malformed */

        uint32_t seq;
        memcpy(&seq, buf, 4);
        seq = ntohl(seq);
        unsigned char *payload = buf + 4;

        /* 1. forward as DATA immediately -- no added delay */
        send_pkt(seq, TYPE_DATA, 0, payload);

        /* 2. remember it for possible retransmit */
        history_put(seq, payload);

        /* 3. fold into this group's XOR parity accumulator */
        if (!have_group_start) {
            group_start = seq - (seq % GROUP_SIZE);
            have_group_start = 1;
        }
        for (int b = 0; b < PAYLOAD_BYTES; b++) group_xor[b] ^= payload[b];

        /* 4. end of group? emit parity, reset accumulator */
        if ((seq % GROUP_SIZE) == (GROUP_SIZE - 1)) {
            send_pkt(group_start, TYPE_PARITY, GROUP_SIZE, group_xor);
            memset(group_xor, 0, sizeof group_xor);
            have_group_start = 0;
        }
    }
    return 0;
}
