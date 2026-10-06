// wlmon: bcmdhd monitor-mode helper (WLC_SET_MONITOR / chanspec)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/sockios.h>

#define WLC_GET_MONITOR 107
#define WLC_SET_MONITOR 108
#define WLC_SET_VAR     263

typedef struct {
	uint32_t cmd; void *buf; uint32_t len; uint8_t set; uint32_t used; uint32_t needed;
} wl_ioctl_t;
typedef struct { wl_ioctl_t ioc; uint32_t driver; } wl_req_t;  /* driver=0 -> WL ioctl */

static int wl_ioctl(const char *ifn, uint32_t cmd, void *buf, uint32_t len, int set)
{
	struct ifreq ifr; wl_req_t req;
	int s = socket(AF_INET, SOCK_DGRAM, 0), r;
	if (s < 0) { perror("socket"); return -1; }
	memset(&ifr, 0, sizeof(ifr)); memset(&req, 0, sizeof(req));
	strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
	req.ioc.cmd = cmd; req.ioc.buf = buf; req.ioc.len = len; req.ioc.set = set;
	ifr.ifr_data = (void *)&req;
	r = ioctl(s, SIOCDEVPRIVATE, &ifr);
	if (r < 0) perror("ioctl");
	close(s);
	return r;
}

int main(int argc, char **argv)
{
	const char *ifn = getenv("WLIF") ? getenv("WLIF") : "wlan0";
	if (argc >= 2 && !strcmp(argv[1], "get")) {
		int32_t v = -1;
		if (wl_ioctl(ifn, WLC_GET_MONITOR, &v, sizeof(v), 0) < 0) return 1;
		printf("monitor=%d\n", v); return 0;
	}
	if (argc >= 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
		int32_t v = !strcmp(argv[1], "on") ? 1 : 0;
		if (wl_ioctl(ifn, WLC_SET_MONITOR, &v, sizeof(v), 1) < 0) return 1;
		printf("monitor set to %d\n", v); return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "chanspec")) {
		/* e.g. 0xd095 = 5GHz ch149 20MHz, 0x1006 = 2.4GHz ch6 20MHz */
		char buf[16] = "chanspec";
		uint32_t cs = (uint32_t)strtoul(argv[2], NULL, 0);
		memcpy(buf + 9, &cs, sizeof(cs));
		if (wl_ioctl(ifn, WLC_SET_VAR, buf, 9 + sizeof(cs), 1) < 0) return 1;
		printf("chanspec set to 0x%04x\n", cs); return 0;
	}
	fprintf(stderr, "usage: wlmon get|on|off|chanspec <hex>   (WLIF=wlan0)\n");
	return 2;
}
