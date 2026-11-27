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
 *                      (the automatic controller, when active, chooses the MCS instead)
 *   /data/adb/mosey-extended/amsdu    "auto" (default), "off", or frames per A-MSDU (2-7)
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
#define WLC_SET_SCANSUPPRESS   116
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

static void suppress_scan(uint32_t on);

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
    suppress_scan(0);                    /* left on if the previous daemon died in a session */
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

/* Android keeps scanning while the station is down for a session. Each scan takes the radio off
 * the AWDL channel and nothing is received until it is tuned again (S10e: the peer stayed
 * invisible, transfers moved once a second). Scans are suppressed while the monitor is armed.
 * module/monwatch.sh and service.sh clear this too if the daemon dies. */
static void suppress_scan(uint32_t on)
{
    if (nex_ioctl(WLC_SET_SCANSUPPRESS, &on, sizeof(on), true) < 0)
        logf_("scan suppress %u failed (errno=%d)", on, errno);
}

static void arm_monitor(void)
{
    /* bcm4375 is RSDB: nexmon's 0x613 binds monitor mode to the core owning the chanspec. It also
     * avoids bcmdhd's own WLC_SET_MONITOR handling, so radiotap frames keep arriving on wlan0. */
    uint32_t mon = (uint32_t)g_monitor;
    pthread_mutex_lock(&g_radio_lock);
    suppress_scan(1);
    int ret = nex_ioctl(NEX_SET_MONITOR_CHSPEC, &mon, sizeof(mon), true);
    int err = errno;
    if (ret >= 0) {
        g_armed = true;
        dp_enable();
    } else if (!g_armed) {
        suppress_scan(0);
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
        suppress_scan(0);
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
static _Atomic int g_auto_mcs;           /* chosen by the rate controller; 0: not in use */
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
        int mcs = g_auto_mcs ? g_auto_mcs : g_rate_mcs;
        if (g_bw == 20 && mcs == 9)      /* VHT MCS 9 does not exist at 20 MHz with 1 or 2 streams */
            mcs = 8;
        out[12] = (unsigned char)(mcs << 4 | g_rate_nss);
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

/* ---- A-MSDU ----------------------------------------------------------------
 * Unicast QoS data frames to the same peer go out together as one A-MSDU: one preamble and one
 * acknowledgement for all of them. The driver refuses frames over 1600 bytes, so the A-MSDU is
 * handed to the firmware as several packets that it chains together (marked in the radiotap pad
 * byte). Used when the firmware reports support, unless the file AMSDU_ON says "off". A frame
 * waits at most AMSDU_WAIT_MS for more. */
#define AMSDU_ON        "/data/adb/mosey-extended/amsdu"
#define NEX_AMSDU_STAT  0x61a
#define AMSDU_MAGIC_N   0x414d5333u       /* firmware joins any number */
#define AMSDU_MAX_N     7                 /* frames per A-MSDU */
#define AMSDU_PEER_MAX  11400             /* Apple devices accept 11454 byte frames */
#define AMSDU_WAIT_MS   2
#define AMSDU_PART_MAX  1514              /* one netdev packet */
#define AMSDU_SPLIT     1512              /* the firmware just concatenates: split anywhere */
#define AMSDU_NEXT      (AMSDU_PART_MAX - 8)  /* payload of a following packet */
#define AMSDU_TOTAL_MAX(n) ((size_t)(AMSDU_SPLIT + ((n) - 1) * AMSDU_NEXT))
#define DOT11_QOS_LEN   26

static _Atomic bool g_amsdu;
static pthread_mutex_t g_am_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_am_cond = PTHREAD_COND_INITIALIZER;
static unsigned char g_am_buf[AMSDU_PART_MAX];
static size_t g_am_len, g_am_rl;          /* held frame; 0: none */
static unsigned long g_am_gen;            /* changes with every held frame */
static bool g_am_thread;
static unsigned char g_am_agg[AMSDU_MAX_N * AMSDU_PART_MAX];  /* A-MSDU being built */
static size_t g_am_agg_len, g_am_last_sf; /* its length; length of its last subframe */
static int g_am_count, g_am_n = 2;        /* frames held; frames per A-MSDU */
static unsigned long s_pairs, s_single, s_joined;

static bool am_send(const unsigned char *d, size_t len)
{
    if (real_send(g_tx_fd, d, len, 0) == (ssize_t)len)
        return true;
    s_fail++;
    return false;
}

/* Sends the held frames: a single one unchanged, more as one A-MSDU cut into packets the driver
 * accepts (the firmware chains them back together). Caller holds g_am_lock. */
static void am_flush(void)
{
    if (!g_am_len)
        return;
    if (g_am_count < 2) {
        am_send(g_am_buf, g_am_len);
        s_single++;
    } else {
        unsigned char part[AMSDU_PART_MAX];
        size_t n = g_am_agg_len;
        size_t off = n > AMSDU_SPLIT + 16 ? AMSDU_SPLIT : n - 16;
        g_am_agg[1] = 0xa1;
        /* without one part the frame would be garbled: stop there, the firmware drops the
         * unfinished chain when the next one starts */
        bool sent = am_send(g_am_agg, off);
        while (sent && off < n) {
            size_t rem = n - off, m = rem > AMSDU_NEXT ? AMSDU_NEXT : rem;
            if (rem > m && rem - m < 16)
                m = rem - 16;
            memset(part, 0, 8);
            part[1] = off + m < n ? 0xa3 : 0xa2;
            part[2] = 8;
            memcpy(part + 8, g_am_agg + off, m);
            sent = am_send(part, 8 + m);
            off += m;
        }
        s_pairs++;
        s_joined += (unsigned long)g_am_count;
    }
    g_am_len = 0;
    g_am_count = 0;
}

static void *am_timer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_am_lock);
    for (;;) {
        while (!g_am_len)
            pthread_cond_wait(&g_am_cond, &g_am_lock);
        unsigned long gen = g_am_gen;
        struct timespec due;
        clock_gettime(CLOCK_REALTIME, &due);
        due.tv_nsec += AMSDU_WAIT_MS * 1000000L;
        if (due.tv_nsec >= 1000000000L) {
            due.tv_sec++;
            due.tv_nsec -= 1000000000L;
        }
        while (g_am_len && g_am_gen == gen &&
               pthread_cond_timedwait(&g_am_cond, &g_am_lock, &due) != ETIMEDOUT) {
        }
        if (g_am_len && g_am_gen == gen)
            am_flush();
    }
    return NULL;
}

/* Writes the A-MSDU subframe of the 802.11 frame h (len bytes): DA, SA, length, payload. */
static size_t am_subframe(unsigned char *out, const unsigned char *h, size_t len)
{
    size_t body = len - DOT11_QOS_LEN;
    memcpy(out, h + 4, 12);               /* addr1 = DA, addr2 = SA */
    out[12] = (unsigned char)(body >> 8);
    out[13] = (unsigned char)body;
    memcpy(out + 14, h + DOT11_QOS_LEN, body);
    return 14 + body;
}

/* Takes a frame (radiotap of rl bytes + 802.11) bound for the data path. Returns true if it was
 * sent or held here; false: send it as usual (any held frame has already gone first). */
static bool am_take(const unsigned char *d, size_t len, size_t rl)
{
    const unsigned char *h = d + rl;
    size_t hlen = len - rl;
    bool fits = rl >= 8 && len <= AMSDU_PART_MAX && hlen > DOT11_QOS_LEN + 8 &&
                h[0] == 0x88 && h[1] == 0 && !(h[4] & 1) && !(h[24] & 0x80) && d[1] == 0;

    pthread_mutex_lock(&g_am_lock);
    if (!g_am_thread) {
        pthread_t t;
        if (pthread_create(&t, NULL, am_timer, NULL) != 0) {
            pthread_mutex_unlock(&g_am_lock);
            return false;
        }
        pthread_detach(t);
        g_am_thread = true;
    }
    if (!fits) {
        am_flush();
        pthread_mutex_unlock(&g_am_lock);
        return false;
    }
    /* join the held frames if the rate header, the three addresses and the QoS TID match and
     * the result still fits */
    size_t body = hlen - DOT11_QOS_LEN;
    if (fits && g_am_len && g_am_rl == rl && memcmp(g_am_buf, d, rl) == 0 &&
        memcmp(g_am_buf + rl + 4, h + 4, 18) == 0 && g_am_buf[rl + 24] == h[24]) {
        if (g_am_count == 1) {
            size_t n = rl + DOT11_QOS_LEN;
            memcpy(g_am_agg, g_am_buf, n);
            g_am_agg[rl + 24] |= 0x80;     /* QoS control: A-MSDU present */
            g_am_last_sf = am_subframe(g_am_agg + n, g_am_buf + rl, g_am_len - rl);
            g_am_agg_len = n + g_am_last_sf;
        }
        size_t pad = (4 - g_am_last_sf % 4) % 4;
        if (g_am_agg_len + pad + 14 + body <= AMSDU_TOTAL_MAX(g_am_n) &&
            g_am_agg_len - rl + pad + 14 + body <= AMSDU_PEER_MAX) {
            memset(g_am_agg + g_am_agg_len, 0, pad);
            g_am_agg_len += pad;
            g_am_last_sf = am_subframe(g_am_agg + g_am_agg_len, h, hlen);
            g_am_agg_len += g_am_last_sf;
            if (++g_am_count >= g_am_n)
                am_flush();
            pthread_mutex_unlock(&g_am_lock);
            return true;
        }
    }
    am_flush();
    memcpy(g_am_buf, d, len);
    g_am_len = len;
    g_am_rl = rl;
    g_am_count = 1;
    g_am_gen++;
    pthread_cond_signal(&g_am_cond);
    pthread_mutex_unlock(&g_am_lock);
    return true;
}

/* ---- automatic rate and A-MSDU size ---------------------------------------
 * Large frames go out as RTS, CTS, data, ACK. A CTS shows the peer was listening, so the ACKs
 * received per CTS tell how well data frames get through at the current rate and size, whatever
 * share of the time the peer spends on this channel. Both are raised step by step while that
 * ratio is high and lowered when it drops; a level that failed is not retried for a while. */
#define WLC_GET_VAR      262
#define CNT_MAC_XTLV     0x900            /* "counters": MAC statistics block */
#define CNT_RXCTS        23               /* CTS frames addressed to us */
#define CNT_RXACK        24               /* ACK frames addressed to us */
#define AUTO_PERIOD_US   300000
#define AUTO_MIN_CTS     15               /* fewer in a period: not enough to judge */
#define AUTO_MCS_MIN     3
#define AUTO_MCS_MAX     9
#define AUTO_HOLD        20               /* good periods before a failed level is tried again */

static _Atomic bool g_auto;
static bool g_auto_thread;
static int g_auto_rate = 7, g_auto_size = 3;     /* kept between sessions */

static bool read_cts_ack(uint32_t *cts, uint32_t *ack)
{
    static unsigned char b[4096];
    memset(b, 0, sizeof(b));
    memcpy(b, "counters", sizeof("counters"));
    if (nex_ioctl(WLC_GET_VAR, b, sizeof(b), false) < 0)
        return false;
    size_t end = 4 + (size_t)(b[2] | b[3] << 8);
    for (size_t p = 4; p + 4 <= end && end <= sizeof(b);) {
        unsigned int id = b[p] | b[p + 1] << 8, len = b[p + 2] | b[p + 3] << 8;
        if (id == CNT_MAC_XTLV && len >= (CNT_RXACK + 1) * 4 && p + 4 + len <= end) {
            memcpy(cts, b + p + 4 + CNT_RXCTS * 4, 4);
            memcpy(ack, b + p + 4 + CNT_RXACK * 4, 4);
            return true;
        }
        p += 4 + ((len + 3) & ~3u);
    }
    return false;
}

static void am_set_size(int n)
{
    pthread_mutex_lock(&g_am_lock);
    am_flush();
    g_am_n = n;
    pthread_mutex_unlock(&g_am_lock);
}

static void *auto_thread(void *arg)
{
    (void)arg;
    uint32_t cts0 = 0, ack0 = 0;
    bool have = false, raise_rate = false;
    int good = 0, hold_rate = 0, hold_size = 0, max_rate = AUTO_MCS_MAX, max_size = AMSDU_MAX_N;
    for (;;) {
        usleep(AUTO_PERIOD_US);
        if (!g_auto) {
            have = false;
            continue;
        }
        uint32_t cts, ack;
        if (!read_cts_ack(&cts, &ack)) {
            have = false;
            continue;
        }
        uint32_t dc = cts - cts0, da = ack - ack0;
        cts0 = cts;
        ack0 = ack;
        if (!have || dc < AUTO_MIN_CTS || dc > 100000) {
            have = true;
            continue;
        }
        unsigned int pct = da >= dc ? 100 : da * 100 / dc;
        int rate = g_auto_rate, size = g_auto_size;
        if (pct < 75) {
            /* undo the last kind of step first; shrink as well when almost nothing arrives */
            if ((raise_rate || size <= 2) && rate > AUTO_MCS_MIN) {
                max_rate = --rate;
                hold_rate = 0;
            } else if (size > 2) {
                max_size = --size;
                hold_size = 0;
            }
            if (pct < 30) {
                if (rate > AUTO_MCS_MIN)
                    max_rate = --rate;
                if (size > 2)
                    max_size = --size;
                hold_rate = hold_size = 0;
            }
            raise_rate = !raise_rate;
            good = 0;
        } else if (pct >= 90) {
            if (++hold_rate >= AUTO_HOLD)
                max_rate = AUTO_MCS_MAX;
            if (++hold_size >= AUTO_HOLD)
                max_size = AMSDU_MAX_N;
            if (++good >= 2) {
                good = 0;
                /* take turns, so that a failure points at one of the two */
                raise_rate = !raise_rate;
                if (raise_rate && rate >= max_rate)
                    raise_rate = false;
                else if (!raise_rate && size >= max_size)
                    raise_rate = true;
                if (raise_rate && rate < max_rate)
                    rate++;
                else if (!raise_rate && size < max_size)
                    size++;
            }
        } else {
            good = 0;
        }
        if (rate != g_auto_rate || size != g_auto_size) {
            logf_("auto: MCS %d, %d frames (acked %u of %u)", rate, size, da, dc);
            g_auto_rate = rate;
            g_auto_mcs = rate;
            if (size != g_auto_size) {
                g_auto_size = size;
                am_set_size(size);
            }
        }
    }
    return NULL;
}

/* Caller holds g_radio_lock, data path just turned on. */
static void am_enable(void)
{
    uint32_t r[4] = { 0 };
    if (nex_ioctl(NEX_AMSDU_STAT, r, sizeof(r), false) < 0 || r[0] != AMSDU_MAGIC_N)
        return;
    /* the file: "auto" (also when missing), "off", or a fixed number of frames per A-MSDU */
    char word[8] = "auto";
    FILE *f = fopen(AMSDU_ON, "re");
    if (f) {
        if (fscanf(f, "%7s", word) != 1)
            strcpy(word, "auto");
        fclose(f);
    }
    bool automatic = strcmp(word, "auto") == 0;
    int n = automatic ? g_auto_size : atoi(word);
    if (n < 2)
        return;
    if (automatic) {
        if (!g_auto_thread) {
            pthread_t t;
            if (pthread_create(&t, NULL, auto_thread, NULL) == 0) {
                pthread_detach(t);
                g_auto_thread = true;
            }
        }
        automatic = g_auto_thread;
    }
    if (n > AMSDU_MAX_N)
        n = AMSDU_MAX_N;
    pthread_mutex_lock(&g_am_lock);
    g_am_n = n;
    pthread_mutex_unlock(&g_am_lock);
    g_auto_mcs = automatic ? g_auto_rate : 0;
    g_auto = automatic;
    g_amsdu = true;
    logf_("amsdu: on, %d frames%s", n, automatic ? ", automatic rate and size" : "");
}

static void am_disable(void)
{
    if (!g_amsdu)
        return;
    g_auto = false;
    g_auto_mcs = 0;
    g_amsdu = false;
    pthread_mutex_lock(&g_am_lock);
    g_am_len = 0;
    g_am_count = 0;
    pthread_mutex_unlock(&g_am_lock);
    uint32_t r[4] = { 0 };
    nex_ioctl(NEX_AMSDU_STAT, r, sizeof(r), false);
    logf_("amsdu: off (host sent=%lu singles=%lu joined=%lu; fw sent=%u dropped heads=%u tails=%u)",
          s_pairs, s_single, s_joined, r[1], r[2], r[3]);
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
    am_enable();
    logf_("datapath: on (fw stat calls=%u injected=%u)", st[0], st[7]);
}

/* Caller holds g_radio_lock. */
static void dp_disable(void)
{
    if (!g_dp_on)
        return;
    g_dp_on = false;
    am_disable();
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

static long long s_big_ns, s_gap_ns, s_gap_max;
static unsigned int s_gaps;

static void tx_stats(size_t bytes, long long t0, long long t1, bool ok)
{
    if (bytes >= 1000) {                 /* pauses of 50 ms to 2 s between large frames */
        long long gap = t0 - s_big_ns;
        if (s_big_ns && gap > 50000000LL && gap < 2000000000LL) {
            s_gaps++;
            s_gap_ns += gap;
            if (gap > s_gap_max)
                s_gap_max = gap;
        }
        s_big_ns = t1;
    }
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
              "datapath=%lu, fail=%lu, pauses=%u (%lld ms, max %lld ms)",
              STAT_EVERY, secs, STAT_EVERY / secs, s_bytes / secs / 1024.0, s_bytes / STAT_EVERY,
              (unsigned long long)(s_call_ns / STAT_EVERY / 1000), s_dp, s_fail, s_gaps,
              s_gap_ns / 1000000, s_gap_max / 1000000);
        s_gaps = 0;
        s_gap_ns = s_gap_max = 0;
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
        /* the data rate is for unicast: group frames (discovery) keep mosey's robust rate */
        if (is_data && !(b[rl + 4] & 1))
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
    if (g_dp_on && g_amsdu && hl && am_take((const unsigned char *)f->data, flen, hl)) {
        s_dp++;
        tx_stats(len, t0, now_ns(), true);
        return (ssize_t)len;
    }
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
