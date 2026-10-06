/* wlaf: send one 802.11 action frame through the bcmdhd "actframe" iovar.
 *
 * This is the firmware's own control-path TX for off-channel / management
 * action frames (the path cfg80211 mgmt_tx uses). Unlike the monitor
 * netdev TX (which bcmdhd drops) and unlike the data path (which needs a
 * per-association flowring), actframe needs neither, so it is the natural
 * injection route for AWDL sync/discovery frames (vendor action frames,
 * category 0x7f, Apple OUI 00:17:f2) for mosey/AirDrop interop research.
 *
 * It auto-detects the actframe iovar version: newer firmware (e.g. BCM4375
 * 18.41.117) uses wl_af_params v2; older firmware uses the legacy struct.
 *
 * usage: wlaf <chanspec_hex> <dst_mac> <hexpayload>
 *   e.g. 5GHz ch149, broadcast, Apple vendor action test body:
 *     wlaf 0xd095 ff:ff:ff:ff:ff:ff 7f0017f208deadbeef
 *   payload = action-frame body from the Category octet.
 *   Run on the normal STA firmware, radio UP; capture on a 2nd monitor
 *   device on the same channel to confirm it went on air.
 */
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/sockios.h>

#define WLC_GET_VAR 262
#define WLC_SET_VAR 263
#define ACTION_FRAME_SIZE 1800
#define WL_ACTFRAME_VERSION_MAJOR_2 2

typedef struct { uint32_t cmd; void *buf; uint32_t len; uint8_t set; uint32_t used; uint32_t needed; } wl_ioctl_t;
typedef struct { wl_ioctl_t ioc; uint32_t driver; } wl_req_t;

struct ether_addr { uint8_t o[6]; };

/* legacy wl_af_params */
struct wl_action_frame { struct ether_addr da; uint16_t len; uint32_t packetId; uint8_t data[ACTION_FRAME_SIZE]; };
struct wl_af_params { uint32_t channel; int32_t dwell_time; struct ether_addr BSSID; uint8_t PAD[2]; struct wl_action_frame action_frame; };

/* versioned wl_af_params v2 */
struct wl_action_frame_v2 {
	uint16_t version, len_total, data_offset;
	struct ether_addr da;
	uint32_t packetId;
	struct ether_addr rand_mac_addr, rand_mac_mask;
	uint16_t flags, len_data;
	uint8_t data[ACTION_FRAME_SIZE];
};
struct wl_af_params_v2 {
	uint16_t version, length;
	uint32_t channel; int32_t dwell_time;
	struct ether_addr BSSID; uint8_t PAD[2];
	struct wl_action_frame_v2 action_frame;
};
struct wl_actframe_version_v1 { uint16_t version, length, actframe_ver_major; };

static int wl_ioc(const char *ifn, uint32_t cmd, void *buf, uint32_t len, int set)
{
	struct ifreq ifr; wl_req_t req;
	int s = socket(AF_INET, SOCK_DGRAM, 0), r;
	if (s < 0) { perror("socket"); return -1; }
	memset(&ifr, 0, sizeof ifr); memset(&req, 0, sizeof req);
	strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
	req.ioc.cmd = cmd; req.ioc.buf = buf; req.ioc.len = len; req.ioc.set = set;
	ifr.ifr_data = (void *)&req;
	r = ioctl(s, SIOCDEVPRIVATE, &ifr);
	close(s);
	return r;
}

/* iovar set: buffer = name\0 + value */
static int iovar_set(const char *ifn, const char *name, const void *val, uint32_t vlen)
{
	uint32_t nlen = strlen(name) + 1, blen = nlen + vlen;
	uint8_t *buf = calloc(1, blen < 256 ? 256 : blen);
	memcpy(buf, name, nlen);
	if (val && vlen) memcpy(buf + nlen, val, vlen);
	int r = wl_ioc(ifn, WLC_SET_VAR, buf, blen, 1);
	if (r < 0) fprintf(stderr, "iovar set %s failed: %s\n", name, strerror(errno));
	free(buf);
	return r;
}

/* returns actframe major version (0 = legacy / iovar absent) */
static int actframe_ver(const char *ifn)
{
	uint8_t buf[256];
	uint32_t nlen = strlen("actframe_ver") + 1;
	memset(buf, 0, sizeof buf);
	memcpy(buf, "actframe_ver", nlen);
	if (wl_ioc(ifn, WLC_GET_VAR, buf, sizeof buf, 0) < 0) return 0;
	struct wl_actframe_version_v1 *v = (void *)buf;
	if (v->version == 1) return v->actframe_ver_major;
	return 0;
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
		if (s[0] == ' ') { s++; continue; }
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
	struct ether_addr da;
	if (parse_mac(argv[2], &da) < 0) { fprintf(stderr, "bad mac\n"); return 2; }
	uint8_t body[ACTION_FRAME_SIZE];
	int plen = parse_hex(argv[3], body, ACTION_FRAME_SIZE);
	if (plen < 1) { fprintf(stderr, "bad payload\n"); return 2; }

	int ver = actframe_ver(ifn);
	printf("actframe iovar version: %s\n", ver == WL_ACTFRAME_VERSION_MAJOR_2 ? "v2" : "legacy");

	int rc;
	if (ver == WL_ACTFRAME_VERSION_MAJOR_2) {
		struct wl_af_params_v2 af; memset(&af, 0, sizeof af);
		af.version = WL_ACTFRAME_VERSION_MAJOR_2;
		af.channel = chanspec;
		af.dwell_time = 400;
		memset(&af.BSSID, 0xff, 6);
		af.action_frame.version = WL_ACTFRAME_VERSION_MAJOR_2;
		af.action_frame.data_offset = offsetof(struct wl_action_frame_v2, data);
		af.action_frame.len_total = offsetof(struct wl_action_frame_v2, data) + plen;
		af.action_frame.len_data = plen;
		af.action_frame.da = da;
		af.action_frame.packetId = 0x1234;
		memcpy(af.action_frame.data, body, plen);
		uint32_t vlen = offsetof(struct wl_af_params_v2, action_frame)
			+ offsetof(struct wl_action_frame_v2, data) + plen;
		af.length = vlen;
		rc = iovar_set(ifn, "actframe", &af, vlen);
	} else {
		struct wl_af_params af; memset(&af, 0, sizeof af);
		af.channel = chanspec;
		af.dwell_time = 400;
		memset(&af.BSSID, 0xff, 6);
		af.action_frame.da = da;
		af.action_frame.len = plen;
		af.action_frame.packetId = 0x1234;
		memcpy(af.action_frame.data, body, plen);
		uint32_t vlen = offsetof(struct wl_af_params, action_frame)
			+ offsetof(struct wl_action_frame, data) + plen;
		rc = iovar_set(ifn, "actframe", &af, vlen);
	}
	if (rc < 0) return 1;
	printf("actframe TX issued: chanspec=0x%04x da=%s payload=%dB\n", chanspec, argv[2], plen);
	printf("(check dmesg for 'TX AF: ACK' / 'TX actfrm : NO ACK'; capture on a 2nd device)\n");
	return 0;
}
