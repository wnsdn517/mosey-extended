/* wlaf: send an 802.11 action frame via the bcmdhd "actframe" iovar.
 *
 * This is the OS-supported TX path the Wi-Fi stack itself uses for
 * P2P / off-channel action frames. AWDL sync & discovery frames are
 * vendor-specific action frames (category 0x7f, Apple OUI 00:17:f2),
 * so this is the natural injection path for mosey/AirDrop interop
 * without replacing firmware.
 *
 * Usage:
 *   wlaf <chanspec_hex> <dst_mac> <hexpayload>
 * e.g. ch149/20MHz 5GHz, broadcast, minimal Apple vendor action body:
 *   wlaf 0xd095 ff:ff:ff:ff:ff:ff 7f0017f208...
 *
 * payload = the action-frame body starting at the Category octet.
 * Interface must be UP and in managed (STA) mode with the normal
 * bcmdhd_sta firmware (NOT the _mon monitor firmware).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/sockios.h>

#define WLC_SET_VAR 263
#define ACTION_FRAME_SIZE 1800

typedef struct { uint32_t cmd; void *buf; uint32_t len; uint8_t set; uint32_t used; uint32_t needed; } wl_ioctl_t;
typedef struct { wl_ioctl_t ioc; uint32_t driver; } wl_req_t;

struct ether_addr { uint8_t o[6]; };
struct wl_action_frame { struct ether_addr da; uint16_t len; uint32_t packetId; uint8_t data[ACTION_FRAME_SIZE]; };
struct wl_af_params { uint32_t channel; int32_t dwell_time; struct ether_addr BSSID; uint8_t PAD[2]; struct wl_action_frame action_frame; };

static int wl_setvar(const char *ifn, const char *name, void *val, uint32_t vlen)
{
	/* iovar buffer = name\0 + value */
	uint32_t nlen = strlen(name) + 1;
	uint32_t blen = nlen + vlen;
	uint8_t *buf = calloc(1, blen);
	memcpy(buf, name, nlen);
	memcpy(buf + nlen, val, vlen);

	struct ifreq ifr; wl_req_t req;
	int s = socket(AF_INET, SOCK_DGRAM, 0), r;
	if (s < 0) { perror("socket"); free(buf); return -1; }
	memset(&ifr, 0, sizeof ifr); memset(&req, 0, sizeof req);
	strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
	req.ioc.cmd = WLC_SET_VAR; req.ioc.buf = buf; req.ioc.len = blen; req.ioc.set = 1;
	ifr.ifr_data = (void *)&req;
	r = ioctl(s, SIOCDEVPRIVATE, &ifr);
	if (r < 0) fprintf(stderr, "actframe ioctl failed: %s\n", strerror(errno));
	close(s); free(buf);
	return r;
}

static int parse_mac(const char *s, struct ether_addr *m)
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return -1;
	for (int i = 0; i < 6; i++) m->o[i] = (uint8_t)v[i];
	return 0;
}

static int parse_hex(const char *s, uint8_t *out, int max)
{
	int n = 0;
	while (s[0] && s[1] && n < max) {
		if (s[0]==' '){s++;continue;}
		unsigned b; if (sscanf(s, "%2x", &b) != 1) return -1;
		out[n++] = (uint8_t)b; s += 2;
	}
	return n;
}

int main(int argc, char **argv)
{
	const char *ifn = getenv("WLIF") ? getenv("WLIF") : "wlan0";
	if (argc < 4) {
		fprintf(stderr, "usage: wlaf <chanspec_hex> <dst_mac> <hexpayload>\n");
		return 2;
	}
	uint32_t chanspec = (uint32_t)strtoul(argv[1], NULL, 0);
	struct wl_af_params af;
	memset(&af, 0, sizeof af);
	af.channel = chanspec;              /* newer FW (>=v14) wants chanspec here */
	af.dwell_time = 400;
	memset(&af.BSSID, 0xff, 6);         /* wildcard */
	if (parse_mac(argv[2], &af.action_frame.da) < 0) { fprintf(stderr, "bad mac\n"); return 2; }
	int plen = parse_hex(argv[3], af.action_frame.data, ACTION_FRAME_SIZE);
	if (plen < 1) { fprintf(stderr, "bad payload\n"); return 2; }
	af.action_frame.len = (uint16_t)plen;
	af.action_frame.packetId = 0x1234;

	/* length sent = header through used data only */
	uint32_t vlen = (uint32_t)((uint8_t*)af.action_frame.data - (uint8_t*)&af) + plen;
	if (wl_setvar(ifn, "actframe", &af, vlen) < 0) return 1;
	printf("actframe TX ok: chanspec=0x%04x da=%s payload=%dB\n", chanspec, argv[2], plen);
	return 0;
}
