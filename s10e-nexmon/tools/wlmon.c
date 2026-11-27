/*
 * wlmon: minimal freestanding "wl" ioctl client for bcmdhd (BCM4375), used by the module scripts.
 * Built without libc (raw aarch64 syscalls):
 *   aarch64-linux-gnu-gcc -O2 -s -static -nostdlib -ffreestanding -fno-builtin \
 *       -fno-stack-protector -fno-tree-loop-distribute-patterns -o wlmon wlmon.c
 *
 *   wlmon [-i ifname] get <cmd>           WLC_* get, prints int result
 *   wlmon [-i ifname] set <cmd> <int>     WLC_* set with int arg
 *   wlmon [-i ifname] var <name>          iovar get (WLC_GET_VAR), prints string
 *
 * WLC_GET_MONITOR=107, WLC_GET_VAR=262, nexmon monitor on the chanspec's core=0x613
 */
typedef unsigned long size_t;

#define SYS_ioctl  29
#define SYS_close  57
#define SYS_write  64
#define SYS_exit   93
#define SYS_socket 198

static long sc3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void scpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void mset(void *p, int v, size_t n) { unsigned char *c = p; while (n--) *c++ = (unsigned char)v; }

static long atol_(const char *s)
{
    long v = 0, neg = 0, base = 10;
    if (*s == '-') neg = 1, s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16, s += 2;
    for (; *s; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
        if (d < 0 || d >= base) break;
        v = v * base + d;
    }
    return neg ? -v : v;
}

static void outnum(long v)
{
    char b[24]; int i = 23; int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    b[i] = 0;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (neg) b[--i] = '-';
    out(b + i);
}

struct wl_ioctl { unsigned int cmd; void *buf; unsigned int len; unsigned char set; unsigned int used, needed, driver; };
struct ifreq_ { char name[16]; void *data; char pad[16]; };

static const char *g_if = "wlan0";

static long wl(unsigned cmd, void *buf, unsigned len, int set)
{
    struct wl_ioctl io; struct ifreq_ ifr;
    long s = sc3(SYS_socket, 2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0), r;
    if (s < 0) return s;
    mset(&io, 0, sizeof io); mset(&ifr, 0, sizeof ifr);
    io.cmd = cmd; io.buf = buf; io.len = len; io.set = (unsigned char)set;
    scpy(ifr.name, g_if, sizeof ifr.name);
    ifr.data = &io;
    r = sc3(SYS_ioctl, s, 0x89F0 /*SIOCDEVPRIVATE*/, (long)&ifr);
    sc3(SYS_close, s, 0, 0);
    return r;
}

static void res(long r) { out(r ? "err " : "ok "); outnum(r); out("\n"); }

static int run(int argc, char **argv)
{
    static char buf[1024];
    int i = 1;
    while (argc - i > 1 && seq(argv[i], "-i")) { g_if = argv[i + 1]; i += 2; }
    if (argc - i < 1) { out("usage: wlmon [-i if] get|set|var ...\n"); return 2; }
    const char *op = argv[i];
    if (seq(op, "get") && argc - i >= 2) {
        int v = 0; long r = wl((unsigned)atol_(argv[i + 1]), &v, 4, 0);
        if (!r) { outnum(v); out("\n"); } else res(r);
        return r != 0;
    }
    if (seq(op, "set") && argc - i >= 3) {
        int v = (int)atol_(argv[i + 2]); long r = wl((unsigned)atol_(argv[i + 1]), &v, 4, 1);
        res(r); return r != 0;
    }
    if (seq(op, "var") && argc - i >= 2) {
        mset(buf, 0, sizeof buf); scpy(buf, argv[i + 1], sizeof buf);
        long r = wl(262, buf, sizeof buf, 0);
        if (!r) { buf[sizeof buf - 1] = 0; out(buf); out("\n"); } else res(r);
        return r != 0;
    }
    out("bad args\n");
    return 2;
}

__attribute__((noreturn, used)) void cstart(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    sc3(SYS_exit, run(argc, argv), 0, 0);
    __builtin_unreachable();
}

__asm__(".global _start\n_start:\n mov x0, sp\n bl cstart\n");
