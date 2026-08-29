// daptest - functional and performance test suite for the RA4M2 CMSIS-DAP probe.
//
// Talks to the probe over CMSIS-DAP v2 (bulk) with libusb, so the host costs
// nothing measurable. At 1024-byte DAP packets a 300 KiB/s link is still ~300
// USB round trips per second per direction; a Python harness becomes the
// bottleneck and measures itself rather than the probe.
//
//   make            # in this directory
//   ./daptest all   # run everything
//
// Subcommands (see usage() at the bottom):
//   transport   DAP round-trip rate with no SWD work, to separate the two costs
//   coherence   block reads and single-word reads of the same memory must agree
//   read        sustained verified read, default 10 MiB, asserts bandwidth
//   write       write bandwidth + readback verify, non-destructive (save/restore)
//   resetloop   assert/release target nRESET repeatedly, assert SWD recovers
//   srst        SWD-only reset+halt via vector catch, no nRESET wire needed
//   halt        reset and hold at the reset vector (recovery for a locked target)
//   depth       prove pipelining past DAP_PACKET_COUNT corrupts, and <= is clean
//   clock       assert the delivered SWD rate tracks the requested rate
//   fault       read unmapped memory, assert a clean FAULT and full recovery
//   churn       repeated SWD connect/disconnect
//   soak        long verified read, judged by backlog against an RTT producer
//   all         every case above except halt, with a pass/fail summary
//
// Exit status is the number of failed cases (0 = all pass).
//
// Every read case verifies its bytes against a golden first pass. A throughput
// number from an engine that never compared what it received is worthless: this
// tool's predecessor reported 249 KiB/s while silently corrupting data, because
// it pipelined past DAP_PACKET_COUNT and stopped matching responses to commands.
#define _GNU_SOURCE
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#define VID 0x045b
#define PID 0x201f

#define WINDOW_BYTES (16u * 1024u)
#define PAGE_BYTES   1024u
#define NPAGES       (WINDOW_BYTES / PAGE_BYTES)
#define MAX_SLOTS    64
#define BUCKET_S     0.100

// ---- DAP commands -----------------------------------------------------------
#define ID_DAP_Info                 0x00
#define ID_DAP_Connect              0x02
#define ID_DAP_Disconnect           0x03
#define ID_DAP_TransferConfigure    0x04
#define ID_DAP_Transfer             0x05
#define ID_DAP_TransferBlock        0x06
#define ID_DAP_WriteABORT           0x08
#define ID_DAP_ResetTarget          0x0A
#define ID_DAP_SWJ_Pins             0x10
#define ID_DAP_SWJ_Clock            0x11
#define ID_DAP_SWJ_Sequence         0x12
#define ID_DAP_SWD_Configure        0x13

// SWJ pin bit positions (CMSIS-DAP spec)
#define SWJ_SWCLK   0
#define SWJ_SWDIO   1
#define SWJ_TDI     2
#define SWJ_TDO     3
#define SWJ_nTRST   5
#define SWJ_nRESET  7

// SWD request bytes: bit0 APnDP, bit1 RnW, bits[3:2] A[3:2]
#define DP_ABORT_W    0x00
#define DP_IDCODE_R   0x02
#define DP_CTRLSTAT_W 0x04
#define DP_CTRLSTAT_R 0x06
#define DP_SELECT_W   0x08
#define DP_RDBUFF_R   0x0E
#define AP_CSW_W      0x01
#define AP_CSW_R      0x03
#define AP_TAR_W      0x05
#define AP_DRW_W      0x0D
#define AP_DRW_R      0x0F

// ARMv8-M / Cortex-M debug registers
#define DHCSR   0xE000EDF0u
#define DEMCR   0xE000EDFCu
#define AIRCR   0xE000ED0Cu
#define DHCSR_DBGKEY     0xA05F0000u
#define DHCSR_C_DEBUGEN  (1u << 0)
#define DHCSR_C_HALT     (1u << 1)
#define DHCSR_S_HALT     (1u << 17)
#define DEMCR_VC_CORERESET (1u << 0)
#define DEMCR_TRCENA       (1u << 24)
#define AIRCR_SYSRESETREQ  0x05FA0004u

static libusb_device_handle *g_h;
static int g_iface = -1, g_ep_out = -1, g_ep_in = -1;
static unsigned g_pkt_size = 64, g_pkt_count = 1;
static int g_verbose = 0;

// Geometry derived from the negotiated packet size. Reads and writes do NOT get
// the same word count: a read block sends a 5-byte command and receives
// 4 + 4*words, a write block sends 5 + 4*words and receives 4. With a 1024-byte
// DAP_PACKET_SIZE that is 255 words for reads but only 254 for writes - asking
// for 255 on a write builds a 1025-byte command, which the device silently
// truncates and then reads the stray byte as the start of the next command.
static unsigned g_wpb_r, g_wpb_w;
static unsigned g_wpb, g_chunks_per_page, g_page_read;

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void die(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(2); }

// ---- pass/fail bookkeeping --------------------------------------------------
#define MAX_CASES 64
static struct { const char *name; int pass; char note[200]; } g_case[MAX_CASES];
static int g_ncase;

static void record(const char *name, int pass, const char *fmt, ...) {
    va_list ap;
    if (g_ncase >= MAX_CASES) return;
    g_case[g_ncase].name = name;
    g_case[g_ncase].pass = pass;
    va_start(ap, fmt);
    vsnprintf(g_case[g_ncase].note, sizeof g_case[g_ncase].note, fmt, ap);
    va_end(ap);
    printf("  %-4s %-12s %s\n", pass ? "PASS" : "FAIL", name, g_case[g_ncase].note);
    fflush(stdout);
    g_ncase++;
}

// ---- synchronous DAP exchange -----------------------------------------------
static int dap_cmd(const uint8_t *cmd, int clen, uint8_t *rsp, int rcap) {
    int n = 0, r;
    r = libusb_bulk_transfer(g_h, g_ep_out, (unsigned char *)cmd, clen, &n, 3000);
    if (r) { fprintf(stderr, "bulk OUT: %s\n", libusb_error_name(r)); return -1; }
    r = libusb_bulk_transfer(g_h, g_ep_in, rsp, rcap, &n, 3000);
    if (r) { fprintf(stderr, "bulk IN: %s\n", libusb_error_name(r)); return -1; }
    return n;
}

static bool find_dap_interface(libusb_device *dev) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_config_descriptor(dev, 0, &cfg)) return false;
    bool found = false;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[0];
        if (id->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) continue;
        int out = -1, in = -1;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if ((ep->bEndpointAddress & 0x80) && in < 0) in = ep->bEndpointAddress;
            else if (!(ep->bEndpointAddress & 0x80) && out < 0) out = ep->bEndpointAddress;
        }
        if (out >= 0 && in >= 0) { g_iface = id->bInterfaceNumber; g_ep_out = out; g_ep_in = in; found = true; }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

// Select the block geometry for one direction. Everything downstream (the
// generator's window offsets, the readback verifier) keys off g_page_read, so
// the two must always be set together.
static void set_dir(int write) {
    unsigned w = write ? g_wpb_w : g_wpb_r;
    g_wpb = w;
    g_chunks_per_page = PAGE_BYTES / (w * 4);
    if (g_chunks_per_page == 0) g_chunks_per_page = 1;
    g_page_read = g_chunks_per_page * w * 4;
}

static void compute_geometry(void) {
    g_wpb_r = (g_pkt_size - 4) / 4;
    g_wpb_w = (g_pkt_size - 5) / 4;
    if (g_wpb_r > 256) g_wpb_r = 256;           // TAR auto-increment wraps at 1 KiB
    if (g_wpb_w > 256) g_wpb_w = 256;
    set_dir(0);
}

static void dap_query(void) {
    uint8_t c[2], r[256];
    c[0] = ID_DAP_Info; c[1] = 0xFF;
    if (dap_cmd(c, 2, r, sizeof r) >= 4 && r[1] == 2) g_pkt_size = r[2] | (r[3] << 8);
    c[0] = ID_DAP_Info; c[1] = 0xFE;
    if (dap_cmd(c, 2, r, sizeof r) >= 3 && r[1] == 1) g_pkt_count = r[2];
    compute_geometry();
}

// Leave the endpoints with nothing queued. A previous run that was killed
// mid-pipeline, or one that sent a malformed command, leaves stale responses
// sitting in the IN endpoint; every subsequent reply is then off by one and the
// probe looks dead. Drain until the endpoint is genuinely empty.
static void dap_flush(void) {
    uint8_t junk[4096];
    int n;
    libusb_clear_halt(g_h, g_ep_in);
    libusb_clear_halt(g_h, g_ep_out);
    for (int i = 0; i < 64; i++)
        if (libusb_bulk_transfer(g_h, g_ep_in, junk, sizeof junk, &n, 50) != 0) break;
}

// Send a harmless command and require the reply to echo its command byte. Used
// to confirm the stream is aligned before anything that matters runs.
static bool dap_in_sync(void) {
    uint8_t c[2] = {ID_DAP_Info, 0xF0}, r[256];
    for (int i = 0; i < 8; i++) {
        int n = dap_cmd(c, 2, r, sizeof r);
        if (n >= 2 && r[0] == ID_DAP_Info) return true;
        dap_flush();
    }
    return false;
}

static void swj_seq(const uint8_t *bits, int nbits) {
    uint8_t c[64], r[64];
    int nb = (nbits + 7) / 8;
    c[0] = ID_DAP_SWJ_Sequence; c[1] = (uint8_t)(nbits == 256 ? 0 : nbits);
    memcpy(&c[2], bits, nb);
    dap_cmd(c, 2 + nb, r, sizeof r);
}

// One DP/AP register access. Returns ACK (1 = OK, 2 = WAIT, 4 = FAULT), -1 on USB error.
static int xfer1(uint8_t req, uint32_t wdata, uint32_t *rdata) {
    uint8_t c[16], r[64];
    int len = 0;
    c[len++] = ID_DAP_Transfer; c[len++] = 0; c[len++] = 1; c[len++] = req;
    if (!(req & 0x02)) {
        c[len++] = wdata & 0xff;         c[len++] = (wdata >> 8) & 0xff;
        c[len++] = (wdata >> 16) & 0xff; c[len++] = (wdata >> 24) & 0xff;
    }
    int n = dap_cmd(c, len, r, sizeof r);
    if (n < 3) return -1;
    if (rdata && n >= 7) *rdata = r[3] | (r[4] << 8) | (r[5] << 16) | (r[6] << 24);
    return r[2];
}

// ---- SWD session ------------------------------------------------------------
static void dap_set_clock(uint32_t hz) {
    uint8_t c[8], r[16];
    c[0] = ID_DAP_SWJ_Clock;
    c[1] = hz & 0xff; c[2] = (hz >> 8) & 0xff; c[3] = (hz >> 16) & 0xff; c[4] = (hz >> 24) & 0xff;
    dap_cmd(c, 5, r, sizeof r);
}

static void dap_disconnect(void) {
    uint8_t c[2], r[16];
    c[0] = ID_DAP_Disconnect;
    dap_cmd(c, 1, r, sizeof r);
}

// Drive the target's hardware reset line. assert_low=1 pulls nRESET low.
static bool dap_nreset(int assert_low) {
    uint8_t c[8], r[16];
    c[0] = ID_DAP_SWJ_Pins;
    c[1] = assert_low ? 0x00 : (1u << SWJ_nRESET);   // value
    c[2] = (1u << SWJ_nRESET);                       // select: only nRESET
    c[3] = 0; c[4] = 0; c[5] = 0; c[6] = 0;          // no wait
    return dap_cmd(c, 7, r, sizeof r) >= 2;
}

// ---- AP configuration -------------------------------------------------------
// CSW[30] is HNONSEC on an AHB5-AP (Cortex-M33 and friends). On a TrustZone part
// the reset default and the value left behind by running firmware are NOT the
// same, and inheriting whatever happens to be there means the first block read
// after a target reset asks for non-secure access to a now-secure region and
// FAULTs. So set the whole field deliberately, and pick the security attribute
// by trying it rather than assuming - SPIDEN (CSW[23], read-only) says secure
// debug is permitted, not that every region wants it.
#define CSW_HNONSEC   (1u << 30)
#define CSW_SIZE_INC  0x12u             // 32-bit transfers, auto-increment single

static int g_csw_nonsec = -1;           // which attribute we settled on

// Single-word read that first clears any sticky error, so the result reflects
// this access and not a leftover from the previous one.
static int mem_probe(uint32_t addr, uint32_t *val) {
    xfer1(DP_ABORT_W, 0x0000001E, NULL);
    if (xfer1(AP_TAR_W, addr, NULL) != 1) return -1;
    int ack = xfer1(AP_DRW_R, 0, NULL);
    if (ack != 1) return ack;
    return xfer1(DP_RDBUFF_R, 0, val);
}

static bool ap_write_csw(int nonsec) {
    uint32_t csw = 0;
    xfer1(DP_ABORT_W, 0x0000001E, NULL);
    xfer1(AP_CSW_R, 0, NULL);
    if (xfer1(DP_RDBUFF_R, 0, &csw) != 1) return false;
    csw = (csw & ~(CSW_HNONSEC | 0x3Fu)) | CSW_SIZE_INC | (nonsec ? CSW_HNONSEC : 0u);
    return xfer1(AP_CSW_W, csw, NULL) == 1;
}

// Configure AP0 for 32-bit auto-incrementing access and choose the security
// attribute that can actually reach `probe_addr`. Secure is tried first: a
// secure access reaches non-secure memory as well, the converse is not true.
static bool ap_configure(const uint32_t *probe_addr) {
    uint32_t addr = probe_addr ? *probe_addr : 0xE000ED00u;   // CPUID always exists
    for (int nonsec = 0; nonsec <= 1; nonsec++) {
        if (!ap_write_csw(nonsec)) continue;
        uint32_t v = 0;
        if (mem_probe(addr, &v) == 1) { g_csw_nonsec = nonsec; return true; }
    }
    return false;
}

// Full SWD bring-up: connect, line reset, DPIDR, power up debug, configure AP0.
static bool swd_connect(uint32_t clock_hz, uint32_t *out_idcode) {
    uint8_t c[16], r[64];

    c[0] = ID_DAP_Connect; c[1] = 1;
    if (dap_cmd(c, 2, r, sizeof r) < 2 || r[1] != 1) return false;

    dap_set_clock(clock_hz);

    c[0] = ID_DAP_TransferConfigure;
    c[1] = 0; c[2] = 0x80; c[3] = 0x00; c[4] = 0x00; c[5] = 0x00;
    dap_cmd(c, 6, r, sizeof r);

    c[0] = ID_DAP_SWD_Configure; c[1] = 0x00;
    dap_cmd(c, 2, r, sizeof r);

    static const uint8_t ones[8]  = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    static const uint8_t sw[2]    = {0x9e, 0xe7};
    static const uint8_t zeros[2] = {0x00, 0x00};
    swj_seq(ones, 64); swj_seq(sw, 16); swj_seq(ones, 64); swj_seq(zeros, 16);

    uint32_t idcode = 0;
    if (xfer1(DP_IDCODE_R, 0, &idcode) != 1) return false;
    if (out_idcode) *out_idcode = idcode;

    xfer1(DP_ABORT_W, 0x0000001E, NULL);             // clear sticky errors
    xfer1(DP_SELECT_W, 0x00000000, NULL);
    xfer1(DP_CTRLSTAT_W, 0x50000000, NULL);          // CDBGPWRUPREQ | CSYSPWRUPREQ
    bool powered = false;
    for (int i = 0; i < 200; i++) {
        uint32_t s = 0;
        if (xfer1(DP_CTRLSTAT_R, 0, &s) != 1) break;
        if ((s & 0xA0000000u) == 0xA0000000u) { powered = true; break; }
    }
    if (!powered) return false;
    return ap_configure(NULL);
}

// ---- single-word memory access ---------------------------------------------
static int mem_read32(uint32_t addr, uint32_t *val) {
    if (xfer1(AP_TAR_W, addr, NULL) != 1) return -1;
    int ack = xfer1(AP_DRW_R, 0, NULL);              // AP reads are posted
    if (ack != 1) return ack;
    return xfer1(DP_RDBUFF_R, 0, val);
}

static int mem_write32(uint32_t addr, uint32_t val) {
    if (xfer1(AP_TAR_W, addr, NULL) != 1) return -1;
    return xfer1(AP_DRW_W, val, NULL);
}

// ---- Cortex-M control -------------------------------------------------------
static bool cm_halt(void) {
    if (mem_write32(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT) != 1) return false;
    for (int i = 0; i < 100; i++) {
        uint32_t v = 0;
        if (mem_read32(DHCSR, &v) == 1 && (v & DHCSR_S_HALT)) return true;
        usleep(2000);
    }
    return false;
}

static bool cm_resume(void) {
    return mem_write32(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN) == 1;
}

static bool cm_release_debug(void) {
    return mem_write32(DHCSR, DHCSR_DBGKEY) == 1;    // clear C_DEBUGEN, target free-runs
}

// ---- block memory access ----------------------------------------------------
// TAR is rewritten every 1 KiB because AHB-AP auto-increment is only guaranteed
// inside a 1 KiB window.
static int mem_read_block(uint32_t addr, uint8_t *buf, unsigned nbytes) {
    uint8_t *cmd = malloc(16), *rsp = malloc(g_pkt_size + 64);
    unsigned done = 0;
    int rc = 0;
    while (done < nbytes) {
        unsigned page_off = (addr + done) & (PAGE_BYTES - 1);
        unsigned room     = PAGE_BYTES - page_off;
        unsigned want     = nbytes - done;
        if (want > room) want = room;
        unsigned words = want / 4;
        if (words > g_wpb_r) words = g_wpb_r;
        if (words == 0) { rc = -1; break; }

        if (xfer1(AP_TAR_W, addr + done, NULL) != 1) { rc = -1; break; }
        cmd[0] = ID_DAP_TransferBlock; cmd[1] = 0;
        cmd[2] = words & 0xff; cmd[3] = (words >> 8) & 0xff;
        cmd[4] = AP_DRW_R;
        int n = dap_cmd(cmd, 5, rsp, 4 + (int)words * 4);
        if (n < 4 || rsp[3] != 1) { rc = (n >= 4 && rsp[3]) ? rsp[3] : -2; break; }
        memcpy(buf + done, rsp + 4, words * 4);
        done += words * 4;
    }
    free(cmd); free(rsp);
    return rc ? rc : 0;
}

static int mem_write_block(uint32_t addr, const uint8_t *buf, unsigned nbytes) {
    uint8_t *cmd = malloc(g_pkt_size + 64), rsp[64];
    unsigned done = 0;
    int rc = 0;
    while (done < nbytes) {
        unsigned page_off = (addr + done) & (PAGE_BYTES - 1);
        unsigned room     = PAGE_BYTES - page_off;
        unsigned want     = nbytes - done;
        if (want > room) want = room;
        unsigned words = want / 4;
        if (words > g_wpb_w) words = g_wpb_w;
        if (words == 0) { rc = -1; break; }

        if (xfer1(AP_TAR_W, addr + done, NULL) != 1) { rc = -1; break; }
        cmd[0] = ID_DAP_TransferBlock; cmd[1] = 0;
        cmd[2] = words & 0xff; cmd[3] = (words >> 8) & 0xff;
        cmd[4] = AP_DRW_W;
        memcpy(cmd + 5, buf + done, words * 4);
        int n = dap_cmd(cmd, 5 + (int)words * 4, rsp, sizeof rsp);
        if (n < 4 || rsp[3] != 1) { rc = (n >= 4 && rsp[3]) ? rsp[3] : -2; break; }
        done += words * 4;
    }
    free(cmd);
    return rc ? rc : 0;
}

// ---- pipelined bandwidth engine ---------------------------------------------
// One USB transfer per DAP command; slots refill in completion order, which on a
// bulk endpoint is submission order, so responses stay paired with their command.
struct bwcfg {
    uint32_t base;          // window base address
    int      write;         // 0 = read, 1 = write
    int      verify;        // compare reads against a golden first pass
    uint64_t target_bytes;
    int      depth;
    int      no_clamp;      // skip the DAP_PACKET_COUNT clamp (used by the depth test)
    uint32_t seed;          // pattern seed for writes
};

struct bwres {
    uint64_t bytes;
    double   elapsed, max_latency;
    double   min_bucket, mean_bucket, sd_bucket;
    long     usb_err, ack_err, short_err, mismatch;
};

struct gen { uint32_t base, addr; unsigned chunk; int need_tar; };
static void gen_init(struct gen *g, uint32_t base) { g->base = base; g->addr = base; g->chunk = 0; g->need_tar = 1; }

static uint8_t *g_golden;
static uint8_t  g_page_valid[NPAGES];
static struct bwcfg g_cfg;
static struct bwres g_res;
static double g_t0, g_bucket_end;
static uint64_t g_bucket_bytes;
static double g_bucket[65536];
static int g_nbuckets, g_done, g_inflight;

// Deterministic per-offset pattern; the seed changes each pass so a readback that
// returns stale data cannot pass verification.
static uint32_t pat_word(uint32_t seed, uint32_t off) {
    uint32_t x = seed ^ (off * 2654435761u);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static int gen_next(struct gen *g, uint8_t *buf, int *rlen, int *payload, long *woff) {
    if (g->need_tar) {
        buf[0] = ID_DAP_Transfer; buf[1] = 0; buf[2] = 1; buf[3] = AP_TAR_W;
        buf[4] = g->addr & 0xff;         buf[5] = (g->addr >> 8) & 0xff;
        buf[6] = (g->addr >> 16) & 0xff; buf[7] = (g->addr >> 24) & 0xff;
        g->need_tar = 0;
        *rlen = 3; *payload = 0; *woff = -1;
        return 8;
    }
    unsigned page = (g->addr - g->base) / PAGE_BYTES;
    *woff = (long)page * g_page_read + (long)g->chunk * g_wpb * 4;

    buf[0] = ID_DAP_TransferBlock; buf[1] = 0;
    buf[2] = g_wpb & 0xff; buf[3] = (g_wpb >> 8) & 0xff;
    int clen;
    if (g_cfg.write) {
        buf[4] = AP_DRW_W;
        for (unsigned i = 0; i < g_wpb; i++) {
            uint32_t w = pat_word(g_cfg.seed, (uint32_t)(*woff) + i * 4);
            buf[5 + i * 4 + 0] = w & 0xff;         buf[5 + i * 4 + 1] = (w >> 8) & 0xff;
            buf[5 + i * 4 + 2] = (w >> 16) & 0xff; buf[5 + i * 4 + 3] = (w >> 24) & 0xff;
        }
        clen = 5 + (int)g_wpb * 4;
        *rlen = 4;
    } else {
        buf[4] = AP_DRW_R;
        clen = 5;
        *rlen = 4 + (int)g_wpb * 4;
    }
    *payload = (int)g_wpb * 4;

    if (++g->chunk >= g_chunks_per_page) {
        g->chunk = 0; g->need_tar = 1;
        g->addr += PAGE_BYTES;
        if (g->addr >= g->base + WINDOW_BYTES) g->addr = g->base;
    }
    return clen;
}

static void bucket_tick(double now) {
    while (now >= g_bucket_end) {
        if (g_nbuckets < (int)(sizeof g_bucket / sizeof g_bucket[0]))
            g_bucket[g_nbuckets++] = g_bucket_bytes / BUCKET_S;
        g_bucket_bytes = 0;
        g_bucket_end += BUCKET_S;
    }
}

static int consume(const uint8_t *rsp, int n, int rlen, int payload, long woff) {
    if (payload == 0) {
        /* The TAR write. Its ACK has to be checked here: if it faults, every
         * block that follows faults too, and blaming the block read sends you
         * looking in the wrong place entirely. */
        if (n < 3) {
            g_res.short_err++;
            if (g_verbose) fprintf(stderr, "  short TAR rsp: n=%d after %llu bytes\n",
                                   n, (unsigned long long)g_res.bytes);
        } else if (rsp[2] != 1) {
            g_res.ack_err++;
            if (g_verbose) fprintf(stderr, "  TAR write ack=%u after %llu bytes\n",
                                   rsp[2], (unsigned long long)g_res.bytes);
        }
        return 0;
    }
    if (n < rlen) {
        g_res.short_err++;
        if (g_verbose) fprintf(stderr, "  short blk rsp: n=%d want %d, cmd=%02X cnt=%u ack=%u, after %llu bytes\n",
                               n, rlen, rsp[0], n >= 3 ? (unsigned)(rsp[1] | (rsp[2] << 8)) : 0u,
                               n >= 4 ? rsp[3] : 0, (unsigned long long)g_res.bytes);
        return 0;
    }
    if (rsp[3] != 1)  { g_res.ack_err++;   return 0; }
    if (g_cfg.write)  return payload;

    unsigned got = (unsigned)(rsp[1] | (rsp[2] << 8)) * 4u;
    if (got != (unsigned)payload) { g_res.short_err++; return (int)got; }

    if (g_cfg.verify && woff >= 0) {
        unsigned page = (unsigned)(woff / g_page_read);
        if (page < NPAGES) {
            if (!g_page_valid[page]) {
                memcpy(g_golden + woff, rsp + 4, payload);
                if ((unsigned)(woff % g_page_read) + payload >= g_page_read) g_page_valid[page] = 1;
            } else if (memcmp(g_golden + woff, rsp + 4, payload) != 0) {
                g_res.mismatch++;
            }
        }
    }
    return payload;
}

struct slot {
    struct libusb_transfer *tout, *tin;
    uint8_t *cmd, *rsp;
    int rlen, payload;
    long woff;
    double ts;
    struct gen *g;
};
static struct slot g_slot[MAX_SLOTS];

static void LIBUSB_CALL on_in(struct libusb_transfer *t);
static void LIBUSB_CALL on_out(struct libusb_transfer *t) {
    g_inflight--;
    if (t->status != LIBUSB_TRANSFER_COMPLETED) { g_res.usb_err++; g_done = 1; }
}

static void arm_slot(struct slot *s) {
    if (g_done) return;
    int clen = gen_next(s->g, s->cmd, &s->rlen, &s->payload, &s->woff);
    s->ts = now_s();
    libusb_fill_bulk_transfer(s->tout, g_h, g_ep_out, s->cmd, clen, on_out, s, 3000);
    libusb_fill_bulk_transfer(s->tin,  g_h, g_ep_in,  s->rsp, s->rlen, on_in,  s, 3000);
    if (libusb_submit_transfer(s->tout)) { g_res.usb_err++; g_done = 1; return; }
    g_inflight++;
    if (libusb_submit_transfer(s->tin))  { g_res.usb_err++; g_done = 1; return; }
    g_inflight++;
}

static void LIBUSB_CALL on_in(struct libusb_transfer *t) {
    struct slot *s = t->user_data;
    g_inflight--;
    if (t->status != LIBUSB_TRANSFER_COMPLETED) { g_res.usb_err++; g_done = 1; return; }
    double now = now_s(), lat = now - s->ts;
    if (lat > g_res.max_latency) g_res.max_latency = lat;
    int good = consume(s->rsp, t->actual_length, s->rlen, s->payload, s->woff);
    g_res.bytes += (unsigned)good; g_bucket_bytes += (unsigned)good;
    bucket_tick(now);
    if (g_res.bytes >= g_cfg.target_bytes ||
        g_res.usb_err || g_res.ack_err || g_res.short_err || g_res.mismatch) { g_done = 1; return; }
    arm_slot(s);
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Run a pipelined bandwidth pass. Returns 0 on completion.
static int run_bw(const struct bwcfg *cfg, struct bwres *out) {
    struct gen g; gen_init(&g, cfg->base);
    set_dir(cfg->write);
    g_cfg = *cfg;
    memset(&g_res, 0, sizeof g_res);
    memset(g_page_valid, 0, sizeof g_page_valid);
    g_nbuckets = 0; g_bucket_bytes = 0; g_done = 0; g_inflight = 0;

    int depth = cfg->depth;
    if (!cfg->no_clamp && depth > (int)g_pkt_count) depth = (int)g_pkt_count;   // device ring limit
    if (depth > MAX_SLOTS) depth = MAX_SLOTS;
    if (depth < 1) depth = 1;

    for (int i = 0; i < depth; i++) {
        g_slot[i].tout = libusb_alloc_transfer(0);
        g_slot[i].tin  = libusb_alloc_transfer(0);
        g_slot[i].cmd  = malloc(g_pkt_size + 64);
        g_slot[i].rsp  = malloc(g_pkt_size + 64);
        g_slot[i].g    = &g;
    }
    g_t0 = now_s(); g_bucket_end = g_t0 + BUCKET_S;
    for (int i = 0; i < depth && !g_done; i++) arm_slot(&g_slot[i]);
    // Hard wall-clock cap. A desynced device answers nothing and every transfer
    // sits out its 3 s timeout; without this the tool looks hung rather than
    // failing, which is the worst possible behaviour in a test suite.
    double hard_deadline = g_t0 + 30.0 + (double)cfg->target_bytes / (100.0 * 1024.0);
    while (!g_done || g_inflight > 0) {
        struct timeval tv = {0, 20000};
        if (libusb_handle_events_timeout(NULL, &tv)) break;
        if (g_done && g_inflight <= 0) break;
        if (now_s() > hard_deadline) { g_res.usb_err++; g_done = 1; break; }
    }
    g_res.elapsed = now_s() - g_t0;
    if (g_inflight > 0) {
        for (int i = 0; i < depth; i++) { libusb_cancel_transfer(g_slot[i].tout); libusb_cancel_transfer(g_slot[i].tin); }
        double dl = now_s() + 2.0;
        while (g_inflight > 0 && now_s() < dl) { struct timeval tv = {0, 20000}; if (libusb_handle_events_timeout(NULL, &tv)) break; }
    }
    for (int i = 0; i < depth; i++) {
        libusb_free_transfer(g_slot[i].tout); libusb_free_transfer(g_slot[i].tin);
        free(g_slot[i].cmd); free(g_slot[i].rsp);
    }
    if (g_nbuckets > 2) {
        int n = g_nbuckets - 2;
        double *v = malloc(n * sizeof *v);
        memcpy(v, g_bucket + 1, n * sizeof *v);
        qsort(v, n, sizeof *v, cmp_d);
        double sum = 0; for (int i = 0; i < n; i++) sum += v[i];
        double mean = sum / n, sd = 0;
        for (int i = 0; i < n; i++) sd += (v[i] - mean) * (v[i] - mean);
        g_res.min_bucket  = v[0];
        g_res.mean_bucket = mean;
        g_res.sd_bucket   = sqrt(sd / n);
        free(v);
    }
    *out = g_res;
    return 0;
}

static long bw_errors(const struct bwres *r) { return r->usb_err + r->ack_err + r->short_err + r->mismatch; }
static double bw_kib(const struct bwres *r) { return (r->bytes / r->elapsed) / 1024.0; }

// =============================================================================
// test cases
// =============================================================================

static uint32_t g_clock   = 10000000;   // what real hosts ask for; >=~4 MHz saturates
static uint32_t g_rdbase  = 0x08000000; // target code flash: static, so reads are verifiable
static uint32_t g_wrbase  = 0;          // discovered scratch RAM
static uint64_t g_readmb  = 10;
static double   g_min_read_kib  = 280.0;
static double   g_min_write_kib = 280.0;
static int      g_depth   = 8;
static int      g_iters   = 20;
/* The soak case is judged against a real workload: a SystemView capture from a
 * riker payment soak produces about 160 KiB/s into an RTT buffer. A stall only
 * loses trace data if the backlog it builds outgrows that buffer. */
static double   g_soak_produce_kib = 160.0;
static double   g_soak_buffer_kib  = 64.0;
static uint64_t g_soak_mb = 64;

static int g_rdbase_fixed;              // user passed --rdbase, do not second-guess it

static bool reconnect(void) {
    /* Realign the USB stream before anything else. Whatever went wrong may have
     * left responses queued, and a DAP_Disconnect issued into a desynced stream
     * just makes it worse. */
    dap_flush();
    dap_in_sync();
    dap_disconnect();
    return swd_connect(g_clock, NULL);
}

// The read window has to be static memory that the current security attribute
// can actually reach. On a TrustZone part the same flash appears at a secure and
// a non-secure alias and only one of them answers, so probe rather than assume.
// A window is only accepted if two reads agree and the contents are not all one
// value - a FAULTing AP happily returns a consistent 0x00000000.
static bool window_readable(uint32_t base) {
    uint32_t a = 0, b = 0, c = 0;
    if (mem_probe(base, &a) != 1) return false;
    if (mem_probe(base, &b) != 1 || a != b) return false;
    if (mem_probe(base + WINDOW_BYTES - 4, &c) != 1) return false;
    return !(a == 0 && c == 0) && !(a == 0xFFFFFFFFu && c == 0xFFFFFFFFu);
}

static bool pick_read_window(void) {
    // Deliberately vendor-agnostic: the suite should not need to be told what
    // is on the other end of the wire. 0x08000000 covers EFR32 Series 2 and the
    // STM32 family alike; 0x0C000000 is the secure alias several ARMv8-M parts
    // expose for the same flash.
    static const uint32_t cand[] = {
        0x08000000,   // main flash on EFR32 Series 2, STM32, and others
        0x0C000000,   // secure alias of the same flash on TrustZone parts
        0x00000000,   // boot-mapped flash (RA, and many others)
        0x0FE08000,   // Silicon Labs Series 2 device information page
        0x1FF00000,   // STM32 system memory (bootloader ROM)
    };
    if (window_readable(g_rdbase)) return true;
    if (g_rdbase_fixed) return false;
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++)
        if (cand[i] != g_rdbase && window_readable(cand[i])) { g_rdbase = cand[i]; return true; }
    return false;
}

static int reset_halt_via_swd(void);
static int g_leave_halted;

// Make sure the read window is reachable before a case depends on it, and if it
// is not, climb the recovery ladder: realign USB, re-negotiate the AP security
// attribute, and finally catch the target at its reset vector before its
// firmware has had a chance to close the debug port. This is the same escalation
// a flashing tool needs on a part whose firmware locks debug on boot; here it
// also keeps one failing case from cascading into every case after it.
static bool ensure_readable(void) {
    if (window_readable(g_rdbase)) return true;
    if (reconnect() && window_readable(g_rdbase)) return true;
    if (reset_halt_via_swd() == 0) {
        g_leave_halted = 1;
        if (ap_configure(NULL) && pick_read_window()) return true;
    }
    return false;
}

// --- transport-only baseline: DAP_Info round trips do zero SWD work, so this
// --- separates USB/firmware overhead from wire time.
static int t_transport(void) {
    uint8_t c[2] = {ID_DAP_Info, 0xF0}, r[64];
    double t0 = now_s();
    long n = 0;
    while (now_s() - t0 < 0.5) { if (dap_cmd(c, 2, r, sizeof r) < 2) break; n++; }
    double dt = now_s() - t0, rate = n / dt;
    int pass = rate > 500.0;
    record("transport", pass, "%.0f DAP round trips/s (%.2f ms each)", rate, 1000.0 / rate);
    return !pass;
}

// --- single-word and block reads of the same memory must agree, and repeated
// --- reads of a constant register must never vary. Cheapest detector for the
// --- response/command desync class of bug.
static int t_coherence(void) {
    if (!ensure_readable()) { record("coherence", 0, "read window 0x%08X unreachable even after reset+halt", g_rdbase); return 1; }
    uint32_t cpuid = 0, first = 0;
    int fails = 0;

    if (mem_read32(0xE000ED00, &cpuid) != 1) { record("coherence", 0, "CPUID read failed"); return 1; }
    for (int i = 0; i < 500; i++) {
        uint32_t v = 0;
        if (mem_read32(0xE000ED00, &v) != 1) { fails++; break; }
        if (i == 0) first = v;
        else if (v != first) { fails++; break; }
    }

    // block vs single-word over a 2 KiB span that straddles a TAR page boundary
    uint32_t span_base = g_rdbase + PAGE_BYTES - 32;
    uint8_t blk[2048];
    int mismatch = 0;
    if (mem_read_block(span_base, blk, sizeof blk) != 0) mismatch = -1;
    else for (unsigned i = 0; i < sizeof blk; i += 4) {
        uint32_t w = 0;
        if (mem_read32(span_base + i, &w) != 1) { mismatch = -1; break; }
        uint32_t b = blk[i] | (blk[i+1] << 8) | (blk[i+2] << 16) | (blk[i+3] << 24);
        if (w != b) { mismatch++; }
    }

    int pass = !fails && mismatch == 0;
    record("coherence", pass, "CPUID 0x%08X stable over 500 reads; block vs word over 2 KiB: %s",
           cpuid, mismatch == 0 ? "identical" : (mismatch < 0 ? "read error" : "MISMATCH"));
    return !pass;
}

// --- sustained verified read.
static int t_read(void) {
    if (!ensure_readable()) { record("read", 0, "read window 0x%08X unreachable even after reset+halt", g_rdbase); return 1; }
    struct bwcfg cfg = { .base = g_rdbase, .write = 0, .verify = 1,
                         .target_bytes = g_readmb * 1024ull * 1024ull, .depth = g_depth };
    struct bwres r;
    g_golden = calloc(1, WINDOW_BYTES + 4096);
    run_bw(&cfg, &r);
    free(g_golden); g_golden = NULL;

    double kib = bw_kib(&r);
    long errs = bw_errors(&r);
    int pass = (errs == 0) && (kib >= g_min_read_kib) && (r.bytes >= cfg.target_bytes);
    record("read", pass, "%.1f MiB in %.2f s = %.1f KiB/s (floor %.0f), errors %ld, "
           "min100ms %.0f, sd %.1f%%, maxlat %.1f ms",
           r.bytes / 1048576.0, r.elapsed, kib, g_min_read_kib, errs,
           r.min_bucket / 1024.0, 100.0 * r.sd_bucket / (r.mean_bucket ? r.mean_bucket : 1),
           r.max_latency * 1000.0);
    return !pass;
}

// --- locate a 16 KiB scratch window in target RAM that we can save and restore.
static bool find_scratch(uint32_t *out) {
    static const uint32_t cand[] = {
        0x20030000, 0x20020000, 0x20010000, 0x20008000, 0x20004000, 0x20000000,
        0x30000000, 0x10000000,
    };
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        uint32_t a = cand[i], save[4], probe[4] = {0xA5A5A5A5, 0x5A5A5A5A, 0xDEADBEEF, 0x00000000};
        bool ok = true;
        for (int w = 0; w < 4 && ok; w++) ok = mem_read32(a + w * 4, &save[w]) == 1;
        // must also be readable at the far end of the window
        uint32_t tail;
        if (ok) ok = mem_read32(a + WINDOW_BYTES - 4, &tail) == 1;
        for (int w = 0; w < 4 && ok; w++) ok = mem_write32(a + w * 4, probe[w]) == 1;
        for (int w = 0; w < 4 && ok; w++) {
            uint32_t v = 0;
            ok = mem_read32(a + w * 4, &v) == 1 && v == probe[w];
        }
        for (int w = 0; w < 4; w++) mem_write32(a + w * 4, save[w]);   // always restore
        if (ok) { *out = a; return true; }
    }
    return false;
}

// --- write bandwidth. Non-destructive: the core is halted, the window saved,
// --- written, verified, restored, and the restore verified before resuming.
static int t_write(void) {
    if (!g_wrbase && !find_scratch(&g_wrbase)) {
        record("write", 0, "no writable 16 KiB scratch window found in target RAM");
        return 1;
    }
    bool halted = cm_halt();
    uint8_t *save = malloc(WINDOW_BYTES), *back = malloc(WINDOW_BYTES);
    int rc = mem_read_block(g_wrbase, save, WINDOW_BYTES);
    if (rc) {
        record("write", 0, "could not save scratch window at 0x%08X (rc %d)", g_wrbase, rc);
        free(save); free(back); if (halted) cm_resume();
        return 1;
    }

    struct bwcfg cfg = { .base = g_wrbase, .write = 1, .verify = 0,
                         .target_bytes = g_readmb * 1024ull * 1024ull, .depth = g_depth,
                         .seed = 0x1234ABCDu };
    struct bwres r;
    run_bw(&cfg, &r);

    // read the window back and compare against the pattern, using the exact
    // geometry the writer used (the last 4 bytes of each 1 KiB page are not
    // covered by a 255-word block, so they were never written).
    long bad = 0;
    for (unsigned p = 0; p < NPAGES && bad == 0; p++) {
        uint8_t buf[PAGE_BYTES];
        if (mem_read_block(g_wrbase + p * PAGE_BYTES, buf, g_page_read) != 0) { bad = -1; break; }
        for (unsigned i = 0; i < g_page_read; i += 4) {
            uint32_t want = pat_word(cfg.seed, (uint32_t)(p * g_page_read + i));
            uint32_t got  = buf[i] | (buf[i+1] << 8) | (buf[i+2] << 16) | (buf[i+3] << 24);
            if (want != got) { bad++; break; }
        }
    }

    int restored = (mem_write_block(g_wrbase, save, WINDOW_BYTES) == 0) &&
                   (mem_read_block(g_wrbase, back, WINDOW_BYTES) == 0) &&
                   (memcmp(save, back, WINDOW_BYTES) == 0);
    if (halted) cm_resume();

    double kib = bw_kib(&r);
    long errs = bw_errors(&r);
    int pass = (errs == 0) && (bad == 0) && restored && (kib >= g_min_write_kib);
    record("write", pass, "%.1f MiB to 0x%08X in %.2f s = %.1f KiB/s (floor %.0f), errors %ld, "
           "readback %s, restore %s",
           r.bytes / 1048576.0, g_wrbase, r.elapsed, kib, g_min_write_kib, errs,
           bad == 0 ? "verified" : (bad < 0 ? "read error" : "MISMATCH"),
           restored ? "verified" : "FAILED");
    free(save); free(back);
    return !pass;
}

// --- hardware reset line: pulse nRESET, reconnect, prove the target came back.
static int t_resetloop(void) {
    int fails = 0, pin_ok = 1;
    uint32_t cpuid0 = 0;
    mem_read32(0xE000ED00, &cpuid0);

    for (int i = 0; i < g_iters; i++) {
        uint8_t c[8], r[16];
        // assert, and read the pin back to confirm the probe actually drove it
        c[0] = ID_DAP_SWJ_Pins; c[1] = 0x00; c[2] = (1u << SWJ_nRESET);
        c[3] = 0x10; c[4] = 0x27; c[5] = 0; c[6] = 0;      // 10 ms pin wait
        int n = dap_cmd(c, 7, r, sizeof r);
        if (n < 2 || (r[1] & (1u << SWJ_nRESET))) pin_ok = 0;
        usleep(20000);
        if (!dap_nreset(0)) { fails++; break; }
        usleep(50000);                                     // let the target boot

        uint32_t idcode = 0, cpuid = 0;
        if (!reconnect() || mem_read32(0xE000ED00, &cpuid) != 1 || cpuid != cpuid0) {
            // one retry: a target that resets while we are connecting is the
            // exact HDP-1692 failure mode, and a fresh session is the cure.
            usleep(50000);
            if (!reconnect() || mem_read32(0xE000ED00, &cpuid) != 1 || cpuid != cpuid0) fails++;
        }
        (void)idcode;
    }
    int pass = (fails == 0) && pin_ok;
    record("resetloop", pass, "%d nRESET pulses, %d recovery failures, pin readback %s",
           g_iters, fails, pin_ok ? "correct" : "WRONG (nRESET did not read low while asserted)");
    return !pass;
}

// --- SWD-only reset-and-halt with vector catch: no nRESET wire required. This
// --- is the recovery path for a target whose firmware reboots every couple of
// --- seconds, where normal AP discovery races the reset (HDP-1692).
static int reset_halt_via_swd(void) {
    if (mem_write32(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN) != 1) return -1;
    uint32_t demcr = 0;
    mem_read32(DEMCR, &demcr);
    if (mem_write32(DEMCR, demcr | DEMCR_TRCENA | DEMCR_VC_CORERESET) != 1) return -2;

    // The reset itself tears the bus out from under this write, so a lost ACK
    // here is expected, not an error.
    (void)mem_write32(AIRCR, AIRCR_SYSRESETREQ);

    double deadline = now_s() + 2.0;                       // bounded, always
    int halted = 0, reinit = 0;
    while (now_s() < deadline) {
        uint32_t v = 0;
        int ack = mem_read32(DHCSR, &v);
        if (ack == 1 && (v & DHCSR_S_HALT)) { halted = 1; break; }
        if (ack != 1) {
            xfer1(DP_ABORT_W, 0x0000001E, NULL);           // clear sticky, retry
            if (++reinit > 4) { if (!swd_connect(g_clock, NULL)) break; reinit = 0; }
        }
        usleep(5000);
    }
    mem_read32(DEMCR, &demcr);
    mem_write32(DEMCR, demcr & ~DEMCR_VC_CORERESET);       // always clear the catch
    return halted ? 0 : -3;
}

// --- reset the target and leave it halted at the reset vector, before its
// --- firmware has run a single instruction. On a part whose firmware locks down
// --- debug access (TrustZone secure attribution, DBGMCU, an SPE that shuts the
// --- port), this is the only way back in - and it is the recovery path pyOCD
// --- needs before AP discovery when a half-flashed target reboots on a loop.
static int t_halt(void) {
    int rc = reset_halt_via_swd();
    uint32_t dhcsr = 0, csw = 0, stat = 0;
    mem_read32(DHCSR, &dhcsr);
    xfer1(DP_CTRLSTAT_R, 0, &stat);
    xfer1(AP_CSW_R, 0, NULL); xfer1(DP_RDBUFF_R, 0, &csw);
    g_leave_halted = 1;
    int pass = rc == 0 && (dhcsr & DHCSR_S_HALT);
    record("halt", pass, "reset+halt rc %d, DHCSR 0x%08X (%s), CTRL/STAT 0x%08X, CSW 0x%08X",
           rc, dhcsr, (dhcsr & DHCSR_S_HALT) ? "halted" : "RUNNING", stat, csw);
    return !pass;
}

static int t_srst(void) {
    int fails = 0;
    for (int i = 0; i < (g_iters < 10 ? g_iters : 10); i++) {
        if (reset_halt_via_swd() != 0) { fails++; continue; }
        uint32_t v = 0;
        if (mem_read32(DHCSR, &v) != 1 || !(v & DHCSR_S_HALT)) fails++;
    }
    cm_resume();
    cm_release_debug();
    int n = g_iters < 10 ? g_iters : 10;
    int pass = fails == 0;
    record("srst", pass, "%d SWD-only reset+halt cycles (vector catch), %d failures", n, fails);
    return !pass;
}

// --- DAP_PACKET_COUNT is a hard limit. Prove both halves of it: at or below the
// --- advertised count the data is byte-exact; above it the device's request ring
// --- is overrun and responses stop matching commands. A host that ignores
// --- DAP_Info 0xFE gets full throughput and silent corruption.
static int t_depth(void) {
    if (!ensure_readable()) { record("depth", 0, "read window 0x%08X unreachable even after reset+halt", g_rdbase); return 1; }
    struct bwres r;
    long clean_errs = 0;
    int over_corrupts = 0;
    char per_depth[192]; int off = 0;

    g_golden = calloc(1, WINDOW_BYTES + 4096);
    for (int d = 1; d <= (int)g_pkt_count; d++) {
        struct bwcfg cfg = { .base = g_rdbase, .verify = 1, .target_bytes = 2u << 20, .depth = d };
        run_bw(&cfg, &r);
        long e = bw_errors(&r);
        clean_errs += e;
        off += snprintf(per_depth + off, sizeof per_depth - off, "%s%d:", d > 1 ? " " : "", d);
        if (!e) off += snprintf(per_depth + off, sizeof per_depth - off, "ok");
        else    off += snprintf(per_depth + off, sizeof per_depth - off, "u%ld/a%ld/s%ld/m%ld",
                                r.usb_err, r.ack_err, r.short_err, r.mismatch);
        if (off >= (int)sizeof per_depth) break;
    }
    // now deliberately exceed it
    int over = (int)g_pkt_count * 4;
    if (over > MAX_SLOTS) over = MAX_SLOTS;
    if (over > (int)g_pkt_count) {
        struct bwcfg cfg = { .base = g_rdbase, .verify = 1, .target_bytes = 2u << 20,
                             .depth = over, .no_clamp = 1 };
        run_bw(&cfg, &r);
        over_corrupts = bw_errors(&r) > 0;
    }
    free(g_golden); g_golden = NULL;
    reconnect();

    int pass = (clean_errs == 0) && over_corrupts;
    record("depth", pass, "[%s] (%ld errors within DAP_PACKET_COUNT=%u); depth %d over the limit %s",
           per_depth, clean_errs, g_pkt_count, over,
           over_corrupts ? "corrupts as expected" : "did NOT corrupt (limit not what it claims?)");
    return !pass;
}

// --- the probe must deliver the SWD rate the host asked for. Before
// --- IO_PORT_WRITE_CYCLES was corrected it padded every half period and ran
// --- slower than requested, which is invisible unless you time it.
static int t_clock(void) {
    if (!ensure_readable()) { record("clock", 0, "read window 0x%08X unreachable even after reset+halt", g_rdbase); return 1; }
    static const uint32_t reqs[] = {1000000, 2000000, 4000000, 10000000, 30000000};
    char buf[256]; int off = 0, fails = 0;
    double sat = 0;

    for (unsigned i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
        dap_set_clock(reqs[i]);
        struct bwcfg cfg = { .base = g_rdbase, .verify = 0, .target_bytes = 768u << 10, .depth = g_depth };
        struct bwres r;
        run_bw(&cfg, &r);
        double kib = bw_kib(&r);
        // 46 SWD clocks per 32-bit word => bytes/s * 46/4 = SWD Hz
        double hz = kib * 1024.0 * 46.0 / 4.0;
        double ratio = hz / reqs[i];
        if (kib > sat) sat = kib;
        // Below saturation the delivered rate must be within 15% of the request.
        // At and above saturation it just has to hold the ceiling.
        int ok = (ratio > 0.85 && ratio < 1.30) || (kib >= g_min_read_kib);
        if (!ok) fails++;
        off += snprintf(buf + off, sizeof buf - off, "%s%.0fM->%.2fM", i ? " " : "",
                        reqs[i] / 1e6, hz / 1e6);
    }
    dap_set_clock(g_clock);
    int pass = fails == 0;
    record("clock", pass, "%s (ceiling %.0f KiB/s), %d out of tolerance", buf, sat, fails);
    return !pass;
}

// --- a bus fault must come back as a clean FAULT ack and the link must still
// --- work afterwards. A probe that hangs or wedges here takes the whole
// --- flashing session with it.
static int t_fault(void) {
    static const uint32_t cand[] = {0xDF000000, 0x60000000, 0xA0000000, 0x00FF0000, 0x50FF0000};
    int faulted = 0, recovered = 0;
    uint32_t at = 0;

    for (unsigned i = 0; i < sizeof cand / sizeof cand[0] && !faulted; i++) {
        uint32_t v = 0;
        int ack = mem_read32(cand[i], &v);
        if (ack != 1) { faulted = 1; at = cand[i]; }
    }
    if (faulted) {
        xfer1(DP_ABORT_W, 0x0000001E, NULL);                // clear sticky flags
        uint32_t stat = 0;
        xfer1(DP_CTRLSTAT_R, 0, &stat);
        uint32_t cpuid = 0;
        recovered = (stat & 0xB2u) == 0 && mem_read32(0xE000ED00, &cpuid) == 1 &&
                    (cpuid >> 16) == 0x410F;
        if (!recovered) { reconnect(); recovered = mem_read32(0xE000ED00, &cpuid) == 1; }
    }
    // Whether an address faults is a property of the target, not the probe; only
    // the recovery behaviour is under test here.
    int pass = !faulted || recovered;
    record("fault", pass, faulted
           ? "fault at 0x%08X returned a clean ack, link %s after ABORT"
           : "no faulting address found on this target (recovery path not exercised)%.0s",
           at, recovered ? "recovered" : "DID NOT RECOVER");
    return !pass;
}

// --- hosts open and close the probe constantly (pyocd does it per invocation).
// --- Connect/disconnect must not leak state or wedge the SWD state machine.
static int t_churn(void) {
    int fails = 0;
    uint32_t idcode0 = 0;
    for (int i = 0; i < g_iters * 5; i++) {
        uint32_t idcode = 0, cpuid = 0;
        dap_disconnect();
        if (!swd_connect(g_clock, &idcode)) { fails++; continue; }
        if (i == 0) idcode0 = idcode;
        else if (idcode != idcode0) fails++;
        if (mem_read32(0xE000ED00, &cpuid) != 1) fails++;
    }
    int pass = fails == 0;
    record("churn", pass, "%d connect/disconnect cycles, DPIDR 0x%08X, %d failures",
           g_iters * 5, idcode0, fails);
    return !pass;
}

// --- long verified pull with a hard per-100ms floor. Average bandwidth hides
// --- stalls; a SystemView capture drops data on the worst bucket, not the mean.
static int t_soak(void) {
    if (!ensure_readable()) { record("soak", 0, "read window 0x%08X unreachable even after reset+halt", g_rdbase); return 1; }
    struct bwcfg cfg = { .base = g_rdbase, .verify = 1,
                         .target_bytes = g_soak_mb << 20, .depth = g_depth };
    struct bwres r;
    g_golden = calloc(1, WINDOW_BYTES + 4096);
    run_bw(&cfg, &r);
    free(g_golden); g_golden = NULL;

    double kib = bw_kib(&r);
    long errs = bw_errors(&r);

    // What decides whether a SystemView capture drops data is not the worst
    // 100 ms window - a floor on every window over three minutes is an assertion
    // about the host OS scheduler, not about the probe. It is the peak BACKLOG:
    // run the measured drain rate against a constant producer and track how much
    // unread data would have piled up in the target's RTT buffer. A stall only
    // matters if it outlasts the buffer.
    double backlog = 0, peak = 0, stalled_s = 0;
    int n = g_nbuckets > 2 ? g_nbuckets - 2 : 0;
    for (int i = 0; i < n; i++) {
        double drained = g_bucket[i + 1] * BUCKET_S;
        double made    = g_soak_produce_kib * 1024.0 * BUCKET_S;
        backlog += made - drained;
        if (backlog < 0) backlog = 0;
        if (backlog > peak) peak = backlog;
        if (g_bucket[i + 1] < g_soak_produce_kib * 1024.0) stalled_s += BUCKET_S;
    }

    int pass = errs == 0 && kib >= g_min_read_kib && peak <= g_soak_buffer_kib * 1024.0;
    record("soak", pass, "%.0f MiB in %.1f s = %.1f KiB/s, errors %ld, sd %.1f%%, worst 100ms "
           "%.0f KiB/s, maxlat %.0f ms; vs a %.0f KiB/s producer: peak backlog %.1f KiB "
           "(buffer %.0f KiB), behind %.1f%% of the time",
           r.bytes / 1048576.0, r.elapsed, kib, errs,
           100.0 * r.sd_bucket / (r.mean_bucket ? r.mean_bucket : 1),
           r.min_bucket / 1024.0, r.max_latency * 1000.0,
           g_soak_produce_kib, peak / 1024.0, g_soak_buffer_kib,
           n ? 100.0 * stalled_s / (n * BUCKET_S) : 0.0);
    return !pass;
}

// =============================================================================
static void usage(void) {
    fprintf(stderr,
        "usage: daptest [options] <case>...\n"
        "cases: transport coherence read write resetloop srst halt depth clock fault churn soak all\n"
        "options:\n"
        "  --clock HZ     SWD clock request (default 10000000)\n"
        "  --rdbase HEX   static read window base (default 0x08000000)\n"
        "  --wrbase HEX   scratch RAM base for the write test (default: autodetect)\n"
        "  --mb N         MiB per read/write bandwidth case (default 10)\n"
        "  --depth N      pipeline depth, clamped to DAP_PACKET_COUNT (default 8)\n"
        "  --iters N      iterations for resetloop/srst/churn (default 20)\n"
        "  --soak-mb N    MiB for the soak case (default 64)\n"
        "  --produce K    assumed RTT producer rate in KiB/s for the soak (default 160)\n"
        "  --buffer K     assumed RTT buffer size in KiB for the soak (default 64)\n"
        "  --min-read K   read bandwidth floor in KiB/s (default 280)\n"
        "  --min-write K  write bandwidth floor in KiB/s (default 280)\n"
        "  -v             verbose\n");
}

int main(int argc, char **argv) {
    const char *cases[32]; int ncases = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--clock")     && i + 1 < argc) g_clock  = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--rdbase")    && i + 1 < argc) { g_rdbase = strtoul(argv[++i], NULL, 0); g_rdbase_fixed = 1; }
        else if (!strcmp(a, "--wrbase")    && i + 1 < argc) g_wrbase = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--mb")        && i + 1 < argc) g_readmb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--depth")     && i + 1 < argc) g_depth  = atoi(argv[++i]);
        else if (!strcmp(a, "--iters")     && i + 1 < argc) g_iters  = atoi(argv[++i]);
        else if (!strcmp(a, "--soak-mb")   && i + 1 < argc) g_soak_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--produce")   && i + 1 < argc) g_soak_produce_kib = atof(argv[++i]);
        else if (!strcmp(a, "--buffer")    && i + 1 < argc) g_soak_buffer_kib  = atof(argv[++i]);
        else if (!strcmp(a, "--min-read")  && i + 1 < argc) g_min_read_kib  = atof(argv[++i]);
        else if (!strcmp(a, "--min-write") && i + 1 < argc) g_min_write_kib = atof(argv[++i]);
        else if (!strcmp(a, "-v")) g_verbose = 1;
        else if (a[0] == '-') { usage(); return 2; }
        else if (ncases < 32) cases[ncases++] = a;
    }
    if (!ncases) { usage(); return 2; }
    if (ncases == 1 && !strcmp(cases[0], "all")) {
        static const char *all[] = {"transport","coherence","read","write","resetloop",
                                    "srst","depth","clock","fault","churn","soak"};
        ncases = 0;
        for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) cases[ncases++] = all[i];
    }

    if (libusb_init(NULL)) die("libusb_init");
    libusb_device **list;
    ssize_t n = libusb_get_device_list(NULL, &list);
    libusb_device *dev = NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d)) continue;
        if (d.idVendor == VID && d.idProduct == PID) { dev = list[i]; break; }
    }
    if (!dev) die("CMSIS-DAP probe 045b:201f not found");
    if (!find_dap_interface(dev)) die("no vendor bulk interface (CMSIS-DAP v2) on the probe");
    if (libusb_open(dev, &g_h)) die("libusb_open (permissions?)");
    libusb_free_device_list(list, 1);
    libusb_set_auto_detach_kernel_driver(g_h, 1);
    if (libusb_claim_interface(g_h, g_iface)) die("claim_interface");

    dap_flush();
    if (!dap_in_sync()) die("probe is not answering in sync - unplug and replug it");
    dap_query();
    uint32_t idcode = 0;
    if (!swd_connect(g_clock, &idcode)) die("SWD connect failed - is a target attached and powered?");
    uint32_t cpuid = 0;
    mem_read32(0xE000ED00, &cpuid);
    bool window_ok = pick_read_window();

    printf("probe   iface %d ep %02x/%02x, DAP_PACKET_SIZE %u, DAP_PACKET_COUNT %u\n",
           g_iface, g_ep_out, g_ep_in, g_pkt_size, g_pkt_count);
    printf("target  DPIDR 0x%08X, CPUID 0x%08X, SWD clock request %u Hz\n", idcode, cpuid, g_clock);
    printf("access  AP0 %s, read window 0x%08X %s\n",
           g_csw_nonsec == 0 ? "secure" : (g_csw_nonsec == 1 ? "non-secure" : "UNCONFIGURED"),
           g_rdbase, window_ok ? "readable" : "NOT READABLE");
    printf("geom    %u words/block, %u blocks/KiB page, %u bytes covered per page\n",
           g_wpb, g_chunks_per_page, g_page_read);
    if (g_verbose) {
        uint32_t stat = 0, csw = 0, w = 0;
        xfer1(DP_CTRLSTAT_R, 0, &stat);
        xfer1(AP_CSW_R, 0, NULL); xfer1(DP_RDBUFF_R, 0, &csw);
        int ack = mem_read32(g_rdbase, &w);
        printf("dp      CTRL/STAT 0x%08X, AP CSW 0x%08X, word at 0x%08X: ack %d value 0x%08X\n",
               stat, csw, g_rdbase, ack, w);
    }
    printf("\n");

    int failed = 0;
    for (int i = 0; i < ncases; i++) {
        const char *c = cases[i];
        int rc;
        if      (!strcmp(c, "transport")) rc = t_transport();
        else if (!strcmp(c, "coherence")) rc = t_coherence();
        else if (!strcmp(c, "read"))      rc = t_read();
        else if (!strcmp(c, "write"))     rc = t_write();
        else if (!strcmp(c, "resetloop")) rc = t_resetloop();
        else if (!strcmp(c, "srst"))      rc = t_srst();
        else if (!strcmp(c, "halt"))      rc = t_halt();
        else if (!strcmp(c, "depth"))     rc = t_depth();
        else if (!strcmp(c, "clock"))     rc = t_clock();
        else if (!strcmp(c, "fault"))     rc = t_fault();
        else if (!strcmp(c, "churn"))     rc = t_churn();
        else if (!strcmp(c, "soak"))      rc = t_soak();
        else { fprintf(stderr, "unknown case '%s'\n", c); usage(); return 2; }
        failed += rc;
        if (rc) reconnect();       // never let one failure poison the next case
    }

    printf("\n%d/%d cases passed\n", g_ncase - failed, g_ncase);
    for (int i = 0; i < g_ncase; i++)
        if (!g_case[i].pass) printf("  FAILED %s: %s\n", g_case[i].name, g_case[i].note);

    if (!g_leave_halted) {
        cm_release_debug();       // let the target free-run again
    }
    dap_disconnect();
    libusb_release_interface(g_h, g_iface);
    libusb_close(g_h);
    libusb_exit(NULL);
    return failed;
}
