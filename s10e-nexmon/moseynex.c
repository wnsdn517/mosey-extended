/*
 * moseynex: LD_PRELOAD shim that lets mosey_server's "radiotap0" backend (radio=1) run on the
 * nexmon-patched BCM4375B1 firmware.
 *
 *   mosey expects                        shim does
 *   interface "radiotap0"                maps it to wlan0 (nexmon delivers radiotap there)
 *   radiotap0 in an nl80211 dump         appends a copy of the wlan0 entry named radiotap0
 *   ART_* private commands               answered with nexmon ioctls
 *   packet-socket TX on the interface    turned into NEX_INJECT_FRAME
 *
 * Monitor mode is armed on ART_SET_CHAN (after a settle delay) and on a capture bind. It is
 * disarmed by one timer thread once no capture socket has been open for a while. An armed
 * firmware keeps the station off 5 GHz, so the disarm matters. service.sh also disarms when
 * mosey_server exits.
 *
 * Environment (all optional):
 *   MOSEYNEX_IF        nexmon interface                   (default wlan0)
 *   MOSEYNEX_ALIAS     name mosey opens                   (default radiotap0)
 *   MOSEYNEX_MAC       reply to ART_GET_IF_ADDR           (default: MAC of MOSEYNEX_IF)
 *   MOSEYNEX_MONITOR   nexmon monitor value, 0 = never    (default 2, radiotap)
 *   MOSEYNEX_SETTLE_MS delay before the first arm         (default 2000)
 *   MOSEYNEX_TX_RATE   0 = keep mosey's 12M radiotap rate  (default 1: ART_TX_RATE for data)
 *   MOSEYNEX_DATAPATH  0 ioctl only, 2 data frames only    (default 1: all frames on the data path)
 *   MOSEYNEX_MIN_VHT_MCS  floor for mosey's VHT data MCS   (default 5, 0 = mosey's choice)
 *   MOSEYNEX_FFI       fallback libmosey_daemon_ffi.so    (default: none)
 *   /data/adb/mosey-extended/tx_rate  optional "VHT <mcs> <nss> [sgi]" override, re-read live
 *   MOSEYNEX_LOG       log file                           (default /data/local/tmp/moseynex.log)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <linux/genetlink.h>
#include <linux/if_arp.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/sockios.h>

/* ---- constants and driver structures ------------------------------------ */

#define WLC_IOCTL_MAGIC        0x14e46c77
#define WLC_SET_VAR            263
#define NEX_INJECT_FRAME       408
#define NEX_SET_MONITOR_CHSPEC 0x613   /* bcm4375b1 nexmon: monitor on the wlc owning the chanspec */
#define PRIV_CMD_IOCTL         (SIOCDEVPRIVATE + 1)

/* d11ac chanspec fields */
#define CHSPEC_5G              0xC000
#define CHSPEC_BW20            0x1000
#define CHSPEC_BW80            0x2000
#define CHSPEC_SB_SHIFT        8

#define MAX_FD                 1024
#define MAX_FRAME              2048
#define FFI_VENDOR_PATH        "/vendor/lib64/libmosey_daemon_ffi.so"

/* Disarm delays. The app re-enables autojoin just before it stops mosey, so the close-disarm
 * must beat the reconnect scan. libpcap reopens its socket during activation, inside this window.
 * The start-disarm covers a start that never opens a capture socket. */
#define CLOSE_DISARM_MS        300
#define START_DISARM_MS        3000

struct nex_ioctl {
    unsigned int cmd;
    void *buf;
    unsigned int len;
    bool set;
    unsigned int used;
    unsigned int needed;
    unsigned int driver;
};

struct android_wifi_priv_cmd {
    char *buf;
    int used_len;
    int total_len;
};

struct inject_frame {
    unsigned short len;
    unsigned char pad;
    unsigned char type;
    char data[];
};

/* What the shim knows about a file descriptor */
enum fd_kind {
    FD_OTHER,
    FD_PACKET,          /* AF_PACKET, not bound to the monitor interface */
    FD_CAPTURE,         /* AF_PACKET bound to the monitor interface: sends are injected */
    FD_GENL,            /* generic netlink: interface dumps get the alias */
};

/* ---- state -------------------------------------------------------------- */

static int (*real_ioctl)(int, int, ...);
static int (*real_socket)(int, int, int);
static int (*real_bind)(int, const struct sockaddr *, socklen_t);
static int (*real_close)(int);
static ssize_t (*real_send)(int, const void *, size_t, int);
static ssize_t (*real_sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
static unsigned int (*real_if_nametoindex)(const char *);
static void *(*real_dlopen)(const char *, int);

/* configuration, fixed after init */
static const char *g_if = "wlan0";
static const char *g_alias = "radiotap0";
static const char *g_mac;
static const char *g_ffi;
static int g_monitor = 2;
static int g_settle_ms = 2000;
static bool g_rate_override = true;
static bool g_dp_allowed = true;                  /* MOSEYNEX_DATAPATH != 0 */
static int g_dp_mode = 1;                         /* MOSEYNEX_DATAPATH: 1 all frames, 2 data only */
static int g_min_vht_mcs = 5;                     /* MOSEYNEX_MIN_VHT_MCS */
static _Atomic int g_bw = 20;                     /* bandwidth of the last chanspec, MHz */

static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic unsigned char g_fd_kind[MAX_FD];  /* enum fd_kind, written by hooks on any thread */

/* radio state: g_armed and g_want_monitor are guarded by g_radio_lock */
static pthread_mutex_t g_radio_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_armed;
static bool g_want_monitor;              /* a channel was set: a later capture bind re-arms */

/* disarm timer: one thread, deadline g_timer_due, guarded by g_timer_lock */
static pthread_mutex_t g_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_timer_cond = PTHREAD_COND_INITIALIZER;
static bool g_timer_started;
static bool g_timer_pending;
static struct timespec g_timer_due;

/* ---- logging and init --------------------------------------------------- */

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void logf_(const char *fmt, ...)
{
    if (!g_log)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_mutex_lock(&g_log_lock);
    fprintf(g_log, "[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_lock);
}

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

static void moseynex_init_once(void)
{
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_socket = dlsym(RTLD_NEXT, "socket");
    real_bind = dlsym(RTLD_NEXT, "bind");
    real_close = dlsym(RTLD_NEXT, "close");
    real_send = dlsym(RTLD_NEXT, "send");
    real_sendto = dlsym(RTLD_NEXT, "sendto");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
    real_write = dlsym(RTLD_NEXT, "write");
    real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
    real_if_nametoindex = dlsym(RTLD_NEXT, "if_nametoindex");
    real_dlopen = dlsym(RTLD_NEXT, "dlopen");

    g_if = env_or("MOSEYNEX_IF", g_if);
    g_alias = env_or("MOSEYNEX_ALIAS", g_alias);
    g_mac = env_or("MOSEYNEX_MAC", NULL);
    g_ffi = env_or("MOSEYNEX_FFI", NULL);
    g_monitor = atoi(env_or("MOSEYNEX_MONITOR", "2"));
    g_settle_ms = atoi(env_or("MOSEYNEX_SETTLE_MS", "2000"));
    g_rate_override = atoi(env_or("MOSEYNEX_TX_RATE", "1")) != 0;
    g_dp_mode = atoi(env_or("MOSEYNEX_DATAPATH", "1"));
    g_dp_allowed = g_dp_mode != 0;
    g_min_vht_mcs = atoi(env_or("MOSEYNEX_MIN_VHT_MCS", "5"));

    g_log = fopen(env_or("MOSEYNEX_LOG", "/data/local/tmp/moseynex.log"), "a");
}

/* Hooks can run before the constructor (another library's initializer). Nothing inside the once
 * may reach a hook: bionic stdio flushes through write(), so no logging there. */
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static inline void ensure_init(void)
{
    pthread_once(&g_init_once, moseynex_init_once);
}

__attribute__((constructor)) static void moseynex_init(void)
{
    ensure_init();
    logf_("moseynex loaded pid=%d if=%s alias=%s", getpid(), g_if, g_alias);
}

static enum fd_kind fd_kind(int fd)
{
    return fd >= 0 && fd < MAX_FD ? (enum fd_kind)g_fd_kind[fd] : FD_OTHER;
}

static void set_fd_kind(int fd, enum fd_kind kind)
{
    if (fd >= 0 && fd < MAX_FD)
        g_fd_kind[fd] = (unsigned char)kind;
}

/* ---- ioctls on the real interface --------------------------------------- */

/* Runs one interface ioctl on g_if through a control socket kept open for the process lifetime
 * (opening one per frame was most of the injection overhead), preserving errno. */
static int g_ctl_fd = -1;
static pthread_mutex_t g_ctl_lock = PTHREAD_MUTEX_INITIALIZER;

static int if_ioctl(int request, struct ifreq *ifr)
{
    strncpy(ifr->ifr_name, g_if, IFNAMSIZ - 1);
    pthread_mutex_lock(&g_ctl_lock);
    if (g_ctl_fd < 0)
        g_ctl_fd = real_socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int ret = g_ctl_fd < 0 ? -1 : real_ioctl(g_ctl_fd, request, ifr);
    int err = errno;
    pthread_mutex_unlock(&g_ctl_lock);
    errno = err;
    return ret;
}

static int nex_ioctl(int cmd, void *buf, unsigned int len, bool set)
{
    struct nex_ioctl ioc = { .cmd = cmd, .buf = buf, .len = len, .set = set, .driver = WLC_IOCTL_MAGIC };
    struct ifreq ifr = { 0 };
    ifr.ifr_data = (void *)&ioc;
    return if_ioctl(SIOCDEVPRIVATE, &ifr);
}

/* ---- monitor mode ------------------------------------------------------- */

static void dp_enable(void);
static void dp_disable(void);
static unsigned int real_index(void);

static bool capture_open(void)
{
    for (int fd = 0; fd < MAX_FD; fd++)
        if (g_fd_kind[fd] == FD_CAPTURE)
            return true;
    return false;
}

static void arm_monitor(void)
{
    /* bcm4375 is RSDB: nexmon's 0x613 binds monitor mode to the core owning the chanspec. It also
     * avoids bcmdhd's own WLC_SET_MONITOR handling, so radiotap frames keep arriving on wlan0. */
    uint32_t mon = (uint32_t)g_monitor;
    pthread_mutex_lock(&g_radio_lock);
    int ret = nex_ioctl(NEX_SET_MONITOR_CHSPEC, &mon, sizeof(mon), true);
    int err = errno;
    if (ret >= 0) {
        g_armed = true;
        dp_enable();
    }
    pthread_mutex_unlock(&g_radio_lock);
    if (ret < 0)
        logf_("arm monitor %d failed (errno=%d)", g_monitor, err);
}

/* The firmware only ACKs unicast frames addressed to its own MAC. mosey uses a separate AWDL
 * address, so without this the peer retransmits every frame (about 6x seen) and TCP stalls on
 * the delayed ACKs. While a session runs the firmware MAC is set to the AWDL address. Guarded
 * by g_radio_lock. */
static unsigned char g_fw_mac[6];
static bool g_fw_mac_set;

static int set_fw_mac(const unsigned char *mac)
{
    char buf[sizeof("cur_etheraddr") + 6] = "cur_etheraddr";
    memcpy(buf + sizeof("cur_etheraddr"), mac, 6);
    return nex_ioctl(WLC_SET_VAR, buf, sizeof(buf), true);
}

/* Called with the source address of each injected frame. */
static void track_tx_mac(const unsigned char *sa)
{
    if (sa[0] & 1 || (g_fw_mac_set && memcmp(sa, g_fw_mac, 6) == 0))
        return;
    pthread_mutex_lock(&g_radio_lock);
    if (g_armed && (!g_fw_mac_set || memcmp(sa, g_fw_mac, 6) != 0)) {
        int ret = set_fw_mac(sa);
        if (ret == 0) {
            memcpy(g_fw_mac, sa, 6);
            g_fw_mac_set = true;
        }
        logf_("firmware mac -> %02x:%02x:%02x:%02x:%02x:%02x ret=%d errno=%d",
              sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], ret, ret ? errno : 0);
    }
    pthread_mutex_unlock(&g_radio_lock);
}

/* Puts the interface's own MAC back into the firmware. Caller holds g_radio_lock. */
static void restore_fw_mac(void)
{
    if (!g_fw_mac_set)
        return;
    struct ifreq ifr = { 0 };
    int ret = if_ioctl(SIOCGIFHWADDR, &ifr);
    if (ret == 0)
        ret = set_fw_mac((const unsigned char *)ifr.ifr_hwaddr.sa_data);
    if (ret == 0)
        g_fw_mac_set = false;
    logf_("firmware mac restored ret=%d", ret);
}

/* Turns the monitor off if it is still armed and no capture socket is open. */
static void disarm_if_idle(void)
{
    pthread_mutex_lock(&g_radio_lock);
    if (g_armed && !capture_open()) {
        uint32_t off = 0;
        int ret = nex_ioctl(NEX_SET_MONITOR_CHSPEC, &off, sizeof(off), true);
        if (ret >= 0)
            g_armed = false;
        logf_("no capture socket left, disarm monitor -> ret=%d", ret);
        dp_disable();
        restore_fw_mac();
    }
    pthread_mutex_unlock(&g_radio_lock);
}

/* Single timer thread. Sleeps until g_timer_due, then runs disarm_if_idle(). A newer schedule
 * request replaces the deadline (latest wins), so there is no per-call thread or allocation. */
static void *disarm_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_timer_lock);
        while (!g_timer_pending)
            pthread_cond_wait(&g_timer_cond, &g_timer_lock);
        int rc = pthread_cond_timedwait(&g_timer_cond, &g_timer_lock, &g_timer_due);
        if (rc != ETIMEDOUT) {
            /* signalled or spurious: re-read the deadline */
            pthread_mutex_unlock(&g_timer_lock);
            continue;
        }
        g_timer_pending = false;
        pthread_mutex_unlock(&g_timer_lock);
        disarm_if_idle();
    }
    return NULL;
}

/* Requests a disarm check delay_ms from now. */
static void schedule_disarm(unsigned int delay_ms)
{
    struct timespec due;
    clock_gettime(CLOCK_REALTIME, &due);
    long ns = due.tv_nsec + (long)delay_ms * 1000000L;
    due.tv_sec += ns / 1000000000L;
    due.tv_nsec = ns % 1000000000L;

    pthread_mutex_lock(&g_timer_lock);
    if (!g_timer_started) {
        pthread_t t;
        if (pthread_create(&t, NULL, disarm_thread, NULL) != 0) {
            pthread_mutex_unlock(&g_timer_lock);
            logf_("cannot start disarm timer");
            return;
        }
        pthread_detach(t);
        g_timer_started = true;
    }
    g_timer_due = due;
    g_timer_pending = true;
    pthread_cond_signal(&g_timer_cond);
    pthread_mutex_unlock(&g_timer_lock);
}

/* center channel | ctl sideband | bandwidth | band */
static uint32_t make_chanspec(int ctl, bool is5g, int bw)
{
    uint32_t band = is5g ? CHSPEC_5G : 0;
    if (bw == 80 && is5g) {
        static const int blocks[] = { 36, 52, 100, 116, 132, 149 };
        for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
            int lo = blocks[i];
            if (ctl >= lo && ctl <= lo + 12 && (ctl - lo) % 4 == 0)
                return band | CHSPEC_BW80 | (uint32_t)(((ctl - lo) / 4) << CHSPEC_SB_SHIFT) |
                       (uint32_t)(lo + 6);
        }
    }
    /* mosey only asks for 149/44 @80 and 6 @20; anything else falls back to 20 MHz */
    return band | CHSPEC_BW20 | (uint32_t)ctl;
}

static int write_chanspec(uint32_t cs)
{
    char buf[sizeof("chanspec") + sizeof(cs)] = "chanspec";
    memcpy(buf + sizeof("chanspec"), &cs, sizeof(cs));
    return nex_ioctl(WLC_SET_VAR, buf, sizeof(buf), true);
}

/* Tunes the radio and arms monitor mode. */
static int set_channel(int ch, bool is5g, int bw)
{
    /* Arming while the firmware is still tearing the link down leaves it delivering mostly
     * garbage frames (S10e: 0.15-0.7 s after the disconnect was bad, 1.5 s and more was clean).
     * Delay before each arm while monitor is off. */
    if (g_monitor && !g_armed && g_settle_ms > 0)
        usleep((useconds_t)g_settle_ms * 1000);

    uint32_t cs = make_chanspec(ch, is5g, bw);
    int ret = write_chanspec(cs);
    if (ret < 0 && bw != 20) {
        /* firmware in monitor mode may reject wide chanspecs */
        logf_("chanspec 0x%04x rejected (errno=%d), retry 20MHz", cs, errno);
        cs = make_chanspec(ch, is5g, 20);
        ret = write_chanspec(cs);
    }
    logf_("set chanspec ch=%d %s bw=%d -> 0x%04x ret=%d", ch, is5g ? "5g" : "2g", bw, cs, ret);

    if (ret == 0)
        g_bw = (cs & 0x3800) == CHSPEC_BW80 ? 80 : (cs & 0x3800) == 0x1800 ? 40 : 20;
    if (ret == 0 && g_monitor) {
        g_want_monitor = true;
        arm_monitor();
        schedule_disarm(START_DISARM_MS);
    }
    return ret;
}

/* ---- data rate ------------------------------------------------------------
 * mosey marks every frame with a radiotap RATE of 12 Mbit/s and sends the real data rate, from its
 * own rate control, as "ART_TX_RATE VHT <mcs> <nss>" or "ART_TX_RATE HT <mcs>". Data frames get a
 * radiotap VHT/HT field with that rate, which the nexmon firmware (inject-rate.patch) applies. */

enum { RATE_LEGACY, RATE_HT, RATE_VHT };
static _Atomic int g_rate_kind = RATE_LEGACY;
static _Atomic int g_rate_mcs, g_rate_nss;

/* Optional override of mosey's rate: "VHT <mcs> <nss> [sgi]" or "HT <mcs> [sgi]" in this file.
 * Re-read on ART_TX_RATE and every STAT_EVERY injected frames, so it can change mid-transfer. */
#define TX_RATE_OVERRIDE "/data/adb/mosey-extended/tx_rate"
static _Atomic bool g_rate_sgi;
static pthread_mutex_t g_rate_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_rate_req[64];            /* mosey's last request; guarded by g_rate_lock */
static char g_rate_cur[64];            /* the rate string in effect; guarded by g_rate_lock */

static void parse_tx_rate(const char *arg);

/* Applies the override file if present, else mosey's request. Logs only changes. */
static void reload_tx_rate(void)
{
    char ov[64];
    const char *arg;
    pthread_mutex_lock(&g_rate_lock);
    int fd = open(TX_RATE_OVERRIDE, O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, ov, sizeof(ov) - 1) : 0;
    if (fd >= 0)
        real_close(fd);
    if (r > 0) {
        ov[r] = 0;
        ov[strcspn(ov, "\n")] = 0;
        arg = ov;
    } else {
        arg = g_rate_req;
    }
    if (strcmp(arg, g_rate_cur) != 0) {
        snprintf(g_rate_cur, sizeof(g_rate_cur), "%s", arg);
        if (arg == ov)
            logf_("tx rate override \"%s\" (mosey asked \"%s\")", ov, g_rate_req);
        parse_tx_rate(arg);
    }
    pthread_mutex_unlock(&g_rate_lock);
}

static void set_tx_rate(const char *arg)
{
    pthread_mutex_lock(&g_rate_lock);
    snprintf(g_rate_req, sizeof(g_rate_req), "%s", arg + strspn(arg, " "));
    g_rate_cur[0] = 0;                 /* force a re-parse */
    pthread_mutex_unlock(&g_rate_lock);
    reload_tx_rate();
}

/* Caller holds g_rate_lock. */
static void parse_tx_rate(const char *arg)
{
    char kind[8] = "", extra[8] = "";
    int mcs = -1, nss = 1;
    int n = sscanf(arg, " %7s %d %d %7s", kind, &mcs, &nss, extra);
    g_rate_sgi = strstr(arg, "sgi") != NULL;
    if (n >= 2 && !strcmp(kind, "VHT") && mcs >= 0 && mcs <= 9 && nss >= 1 && nss <= 2) {
        /* Injected frames are not aggregated, so per-frame overhead dominates: on an S10e ->
         * iPhone transfer MCS 5 gave +23%, MCS 7 only +26%, short GI nothing (measured). */
        if (mcs < g_min_vht_mcs)
            mcs = g_min_vht_mcs;
        g_rate_mcs = mcs;
        g_rate_nss = nss;
        g_rate_kind = RATE_VHT;
    } else if (n >= 2 && !strcmp(kind, "HT") && mcs >= 0 && mcs <= 15) {
        g_rate_mcs = mcs;
        g_rate_kind = RATE_HT;
    } else {
        g_rate_kind = RATE_LEGACY;
    }
    logf_("tx rate %s mcs=%d nss=%d sgi=%d -> kind=%d", kind, mcs, nss, (int)g_rate_sgi,
          (int)g_rate_kind);
}

/* Writes a radiotap header carrying the current data rate to out. Returns its length, or 0 to
 * keep the frame's own header. */
static size_t rate_radiotap(unsigned char *out)
{
    int kind = g_rate_kind;
    if (!g_rate_override || kind == RATE_LEGACY)
        return 0;
    memset(out, 0, 20);
    if (kind == RATE_VHT) {
        uint32_t present = 1u << 21;                        /* VHT: u16 known, u8 flags, u8 bw, ... */
        out[2] = 20;
        memcpy(out + 4, &present, 4);
        out[8] = 0x44;                                      /* known: bandwidth, GI */
        out[10] = g_rate_sgi ? 0x04 : 0;                    /* flags: short GI */
        out[11] = g_bw == 80 ? 4 : g_bw == 40 ? 1 : 0;
        out[12] = (unsigned char)(g_rate_mcs << 4 | g_rate_nss);
        return 20;
    }
    uint32_t present = 1u << 19;                            /* MCS: u8 known, u8 flags, u8 mcs */
    out[2] = 11;
    memcpy(out + 4, &present, 4);
    out[8] = 0x07;                                          /* known: bandwidth, mcs, GI */
    out[9] = (g_bw >= 40 ? 1 : 0) | (g_rate_sgi ? 0x04 : 0);
    out[10] = (unsigned char)g_rate_mcs;
    return 11;
}

/* ---- ART_* private commands --------------------------------------------- */

static void get_mac_str(char *out, size_t n)
{
    if (g_mac) {
        snprintf(out, n, "%s", g_mac);
        return;
    }
    unsigned char m[6] = { 0 };
    struct ifreq ifr = { 0 };
    if (if_ioctl(SIOCGIFHWADDR, &ifr) == 0)
        memcpy(m, ifr.ifr_hwaddr.sa_data, sizeof(m));
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* Returns true if the command was handled (reply written into pc->buf). */
static bool handle_art(struct android_wifi_priv_cmd *pc)
{
    if (!pc || !pc->buf || pc->total_len <= 0 || strncmp(pc->buf, "ART_", 4) != 0)
        return false;

    char cmd[256];
    int n = pc->used_len > 0 && pc->used_len < (int)sizeof(cmd) ? pc->used_len : (int)sizeof(cmd) - 1;
    memcpy(cmd, pc->buf, n);
    cmd[n] = 0;

    logf_("art: %s", cmd);
    char reply[64] = "OK";
    int ch, bw;
    char band[8];
    if (sscanf(cmd, "ART_SET_CHAN %d %7s %d", &ch, band, &bw) == 3) {
        if (set_channel(ch, band[0] == '5', bw) < 0)
            snprintf(reply, sizeof(reply), "FAIL");
    } else if (!strncmp(cmd, "ART_GET_IF_ADDR", 15)) {
        get_mac_str(reply, sizeof(reply));
    } else if (!strncmp(cmd, "ART_TX_RATE", 11)) {
        set_tx_rate(cmd + 11);
    }
    /* ART_BSSID needs nothing: monitor mode receives all frames, and the ACK address is set from
     * the injected frames (track_tx_mac) */

    memset(pc->buf, 0, pc->total_len);
    snprintf(pc->buf, pc->total_len, "%s", reply);
    return true;
}

/* ---- hook: dlopen --------------------------------------------------------
 * mosey_server dlopens the ffi library by absolute path. KernelSU Next in "Meta" mount mode
 * without a metamodule does not overlay /vendor, so use the module copy when that path is missing. */

void *dlopen(const char *filename, int flags)
{
    ensure_init();
    if (filename && g_ffi && strcmp(filename, FFI_VENDOR_PATH) == 0 && access(filename, F_OK) != 0) {
        logf_("dlopen %s -> %s", filename, g_ffi);
        filename = g_ffi;
    }
    return real_dlopen(filename, flags);
}

/* ---- hooks: interface name and ioctl mapping ---------------------------- */

static bool is_alias(const char *name)
{
    return name && strncmp(name, g_alias, IFNAMSIZ) == 0;
}

static unsigned int real_index(void)
{
    return real_if_nametoindex(g_if);
}

unsigned int if_nametoindex(const char *name)
{
    ensure_init();
    return is_alias(name) ? real_index() : real_if_nametoindex(name);
}

/* An interface ioctl addressed to the alias, executed on the real interface. */
static int alias_ioctl(int fd, int request, struct ifreq *ifr)
{
    unsigned int req = (unsigned int)request;
    if (req == SIOCSIFFLAGS)      /* never let mosey bring the real Wi-Fi interface down */
        return 0;
    strncpy(ifr->ifr_name, g_if, IFNAMSIZ - 1);
    int ret = real_ioctl(fd, request, ifr);
    strncpy(ifr->ifr_name, g_alias, IFNAMSIZ - 1);
    if (ret == 0 && req == SIOCGIFHWADDR)
        ifr->ifr_hwaddr.sa_family = ARPHRD_IEEE80211_RADIOTAP;
    return ret;
}

int ioctl(int fd, int request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    ensure_init();

    unsigned int req = (unsigned int)request;
    if (req >= 0x8900 && req <= 0x89ff && arg) {     /* socket/interface ioctls carry an ifreq */
        struct ifreq *ifr = arg;
        if (req == PRIV_CMD_IOCTL && strncmp(ifr->ifr_name, g_if, IFNAMSIZ) == 0 &&
            handle_art((struct android_wifi_priv_cmd *)ifr->ifr_data))
            return 0;
        if (is_alias(ifr->ifr_name))
            return alias_ioctl(fd, request, ifr);
    }
    return real_ioctl(fd, request, arg);
}

/* ---- hooks: socket tracking --------------------------------------------- */

int socket(int domain, int type, int protocol)
{
    ensure_init();
    int fd = real_socket(domain, type, protocol);
    enum fd_kind kind = FD_OTHER;
    if (domain == AF_PACKET)
        kind = FD_PACKET;
    else if (domain == AF_NETLINK && protocol == NETLINK_GENERIC)
        kind = FD_GENL;
    set_fd_kind(fd, kind);
    return fd;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    ensure_init();
    enum fd_kind kind = fd_kind(fd);
    if ((kind == FD_PACKET || kind == FD_CAPTURE) && addr && addr->sa_family == AF_PACKET &&
        len >= sizeof(struct sockaddr_ll)) {
        struct sockaddr_ll sll;
        memcpy(&sll, addr, sizeof(sll));
        unsigned int idx = real_index();
        if (idx && (unsigned int)sll.sll_ifindex == idx) {
            int ret = real_bind(fd, addr, len);
            if (ret == 0) {
                set_fd_kind(fd, FD_CAPTURE);
                if (g_want_monitor && !g_armed)
                    arm_monitor();
            }
            return ret;
        }
    }
    return real_bind(fd, addr, len);
}

int close(int fd)
{
    ensure_init();
    bool capture = fd_kind(fd) == FD_CAPTURE;
    set_fd_kind(fd, FD_OTHER);
    int ret = real_close(fd);
    if (capture) {
        int err = errno;
        schedule_disarm(CLOSE_DISARM_MS);
        errno = err;
    }
    return ret;
}

/* ---- hooks: frame injection --------------------------------------------- */

/* Two injection paths:
 *   data path  the frame (radiotap + 802.11) is sent as a raw packet on g_if. bcmdhd queues it
 *              on a flow ring (mosey_dhd.ko forces the link up so the ring exists while
 *              unassociated); the nexmon datapath hook fetches the host part of the packet
 *              (hnd_pktfetch) and injects it. A few us per frame.
 *   ioctl      one NEX_INJECT_FRAME per frame, a ~2.3 ms round trip. Requests are truncated at
 *              ~1.8 KB, so frames cannot be batched. Used when the data path is unavailable and
 *              for any frame the data path refuses. */
#define STAT_EVERY 500
#define DP_LINK_PARAM "/sys/module/mosey_dhd/parameters/link"
#define DP_BUSY       "/data/adb/mosey-extended/dp_busy"
#define NEX_DP_STAT   0x614             /* datapath firmware: diag counters, absent otherwise */

static unsigned long s_frames, s_fail, s_bytes, s_dp;
static long long s_win_ns, s_call_ns;

static _Atomic bool g_dp_on;            /* changed with g_radio_lock held */
static int g_tx_fd = -1;

static bool write_dp_link(int v)
{
    int fd = open(DP_LINK_PARAM, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char c = v ? '1' : '0';
    bool ok = real_write(fd, &c, 1) == 1;
    real_close(fd);
    return ok;
}

/* On disk while the data path is on. If the phone goes down in that time the file survives, and
 * the module's service.sh pins slow mode. */
static void mark_dp_busy(bool busy)
{
    if (!busy) {
        unlink(DP_BUSY);
        return;
    }
    int fd = open(DP_BUSY, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        fsync(fd);
        real_close(fd);
    }
}

/* Turns the data path on after a successful arm, if firmware and module support it.
 * Caller holds g_radio_lock. */
static void dp_enable(void)
{
    if (!g_dp_allowed || g_dp_on)
        return;
    uint32_t st[8];
    if (nex_ioctl(NEX_DP_STAT, st, sizeof(st), false) < 0) {
        logf_("datapath: firmware has no hook, ioctl injection");
        return;
    }
    if (g_tx_fd < 0) {
        struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_ifindex = (int)real_index() };
        int fd = real_socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);   /* protocol 0: no RX */
        if (fd >= 0 && real_bind(fd, (struct sockaddr *)&sll, sizeof(sll)) != 0) {
            real_close(fd);
            fd = -1;
        }
        if (fd >= 0) {
            /* Blocking sends: bcmdhd flow control reaches mosey as a short wait instead of a
             * deep queue. A stuck driver times out and the frame goes by ioctl. */
            struct timeval tmo = { .tv_sec = 0, .tv_usec = 20000 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
        }
        g_tx_fd = fd;
    }
    if (g_tx_fd >= 0)
        mark_dp_busy(true);
    if (g_tx_fd < 0 || !write_dp_link(1)) {
        mark_dp_busy(false);
        logf_("datapath: unavailable (tx fd %d, errno=%d), ioctl injection", g_tx_fd, errno);
        return;
    }
    g_dp_on = true;
    logf_("datapath: on (fw stat calls=%u injected=%u)", st[0], st[7]);
}

/* Caller holds g_radio_lock. */
static void dp_disable(void)
{
    if (!g_dp_on)
        return;
    g_dp_on = false;
    write_dp_link(0);
    mark_dp_busy(false);
    uint32_t st[8] = { 0 };
    nex_ioctl(NEX_DP_STAT, st, sizeof(st), false);
    logf_("datapath: off (fw calls=%u fetched=%u fetch_fail=%u done=%u cancelled=%u "
          "non-radiotap dropped=%u injected=%u)", st[0], st[1], st[2], st[3], st[4], st[6], st[7]);
}

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void tx_stats(size_t bytes, long long t0, long long t1, bool ok)
{
    if (s_frames == 0)
        s_win_ns = t0;
    s_frames++;
    s_bytes += bytes;
    s_call_ns += t1 - t0;
    if (!ok)
        s_fail++;
    if (s_frames == STAT_EVERY) {
        double secs = (t1 - s_win_ns) / 1e9;
        logf_("inject %d frames in %.2fs: %.1f fps, %.1f KB/s, avg len=%lu, call avg=%lluus, "
              "datapath=%lu, fail=%lu",
              STAT_EVERY, secs, STAT_EVERY / secs, s_bytes / secs / 1024.0, s_bytes / STAT_EVERY,
              (unsigned long long)(s_call_ns / STAT_EVERY / 1000), s_dp, s_fail);
        s_frames = s_fail = s_bytes = s_dp = 0;
        reload_tx_rate();
        s_call_ns = 0;
    }
}

static ssize_t inject(const void *buf, size_t len)
{
    if (len > MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }
    unsigned char tmp[sizeof(struct inject_frame) + 20 + MAX_FRAME];
    struct inject_frame *f = (struct inject_frame *)tmp;
    const unsigned char *b = buf;
    size_t rl = len >= 4 ? (size_t)(b[2] | b[3] << 8) : len, hl = 0;
    bool is_data = false;
    if (rl + 24 <= len) {
        track_tx_mac(b + rl + 10);                   /* 802.11 addr2 */
        is_data = ((b[rl] >> 2) & 3) == 2;
        if (is_data)                                 /* use mosey's data rate */
            hl = rate_radiotap((unsigned char *)f->data);
    }
    size_t flen = len;
    if (hl) {
        memcpy(f->data + hl, b + rl, len - rl);
        flen = hl + len - rl;
    } else {
        memcpy(f->data, buf, len);
    }

    long long t0 = now_ns();
    /* MOSEYNEX_DATAPATH=2 keeps management/action frames on the ioctl path (with that
     * split the peer never accepted the TCP connect, so the default sends everything one way). */
    if (g_dp_on && (g_dp_mode != 2 || is_data)) {
        if (real_send(g_tx_fd, f->data, flen, 0) == (ssize_t)flen) {
            s_dp++;
            tx_stats(len, t0, now_ns(), true);
            return (ssize_t)len;
        }
    }
    f->len = (unsigned short)(sizeof(*f) + flen);
    f->pad = 0;
    f->type = 1;                                     /* data starts with a radiotap header */
    bool ok = nex_ioctl(NEX_INJECT_FRAME, f, f->len, true) >= 0;
    tx_stats(len, t0, now_ns(), ok);
    return ok ? (ssize_t)len : -1;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    ensure_init();
    return fd_kind(fd) == FD_CAPTURE ? inject(buf, len) : real_send(fd, buf, len, flags);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    ensure_init();
    return fd_kind(fd) == FD_CAPTURE ? inject(buf, len) : real_sendto(fd, buf, len, flags, to, tolen);
}

ssize_t write(int fd, const void *buf, size_t len)
{
    ensure_init();
    return fd_kind(fd) == FD_CAPTURE ? inject(buf, len) : real_write(fd, buf, len);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    ensure_init();
    if (fd_kind(fd) != FD_CAPTURE)
        return real_sendmsg(fd, msg, flags);

    unsigned char frame[MAX_FRAME];
    size_t off = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        size_t l = msg->msg_iov[i].iov_len;
        if (l > sizeof(frame) - off) {
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(frame + off, msg->msg_iov[i].iov_base, l);
        off += l;
    }
    return inject(frame, off);
}

/* ---- hooks: nl80211 interface dump ---------------------------------------
 * The daemon finds the interface in an NL80211 GET_INTERFACE dump, which only lists real wireless
 * interfaces. The copy keeps the real ifindex, matching what if_nametoindex(alias) reports. */

#define NL_HDRS (NLMSG_HDRLEN + GENL_HDRLEN)

/* If m is the NEW_INTERFACE entry of g_if, writes a copy named g_alias to out (cap bytes).
 * Returns the size of the copy, or 0. */
static size_t make_alias_msg(const unsigned char *m, size_t mlen, unsigned char *out, size_t cap)
{
    struct nlmsghdr h;
    memcpy(&h, m, sizeof(h));
    if (h.nlmsg_type <= GENL_ID_CTRL || mlen < NL_HDRS || cap < NL_HDRS ||
        m[NLMSG_HDRLEN] != NL80211_CMD_NEW_INTERFACE)
        return 0;
    memcpy(out, m, NL_HDRS);

    size_t in = NL_HDRS, o = NL_HDRS;
    bool match = false;
    while (in + NLA_HDRLEN <= mlen) {
        struct nlattr a;
        memcpy(&a, m + in, sizeof(a));
        if (a.nla_len < NLA_HDRLEN || in + a.nla_len > mlen)
            return 0;
        const void *data = m + in + NLA_HDRLEN;
        size_t dlen = a.nla_len - NLA_HDRLEN;
        in += NLA_ALIGN(a.nla_len);

        if ((a.nla_type & NLA_TYPE_MASK) == NL80211_ATTR_IFNAME) {
            if (dlen != strlen(g_if) + 1 || memcmp(data, g_if, dlen) != 0)
                return 0;
            match = true;
            data = g_alias;
            dlen = strlen(g_alias) + 1;
        }
        size_t need = NLA_ALIGN(NLA_HDRLEN + dlen);
        if (o + need > cap)
            return 0;
        a.nla_len = (unsigned short)(NLA_HDRLEN + dlen);
        memset(out + o, 0, need);
        memcpy(out + o, &a, sizeof(a));
        memcpy(out + o + NLA_HDRLEN, data, dlen);
        o += need;
    }
    if (!match)
        return 0;
    h.nlmsg_len = (uint32_t)o;
    memcpy(out, &h, sizeof(h));
    return o;
}

/* Inserts the alias entry after the real one if the buffer has room. Returns the new length. */
static size_t add_alias_iface(unsigned char *buf, size_t len, size_t cap)
{
    size_t off = 0;
    while (off + NLMSG_HDRLEN <= len) {
        struct nlmsghdr h;
        memcpy(&h, buf + off, sizeof(h));
        if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > len - off)
            break;
        size_t step = NLMSG_ALIGN(h.nlmsg_len);
        unsigned char copy[1024];
        size_t n = make_alias_msg(buf + off, h.nlmsg_len, copy, sizeof(copy));
        if (n && off + step <= len && len + n <= cap) {
            memmove(buf + off + step + n, buf + off + step, len - off - step);
            memcpy(buf + off + step, copy, n);
            return len + n;
        }
        off += step;
    }
    return len;
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    ensure_init();
    ssize_t ret = real_recvfrom(fd, buf, len, flags, from, fromlen);
    if (ret > 0 && (size_t)ret <= len && fd_kind(fd) == FD_GENL && !(flags & (MSG_PEEK | MSG_TRUNC))) {
        int err = errno;
        ret = (ssize_t)add_alias_iface(buf, (size_t)ret, len);
        errno = err;
    }
    return ret;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}
