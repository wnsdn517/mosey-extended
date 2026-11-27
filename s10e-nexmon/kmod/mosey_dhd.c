// SPDX-License-Identifier: GPL-2.0
/*
 * mosey_dhd: lets the built-in bcmdhd create TX flow rings while the STA is unassociated.
 *
 * bcmdhd only allocates a flow ring when if_flow_lkup[ifidx].status is set, which happens on a
 * WLC_E_LINK up event. With the STA disconnected for AirDrop, packets sent on wlan0 are freed
 * before they reach the dongle. Writing 1 to /sys/module/mosey_dhd/parameters/link marks ifidx 0
 * as linked through the driver's own dhd_update_interface_link_status(); 0 clears it again. No
 * kernel code is modified.
 */
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

typedef int (*link_fn_t)(void *dhdp, u8 ifindex, u8 status);

static link_fn_t link_fn;
static void **g_dhd_pub_p;

static int link_set(const char *val, const struct kernel_param *kp)
{
	int v, ret;

	if (kstrtoint(val, 0, &v))
		return -EINVAL;
	if (!link_fn || !g_dhd_pub_p || !*g_dhd_pub_p)
		return -ENODEV;
	ret = link_fn(*g_dhd_pub_p, 0, v ? 1 : 0);
	pr_info("mosey_dhd: ifidx 0 link status %d -> %d\n", v ? 1 : 0, ret);
	return 0;
}

static const struct kernel_param_ops link_ops = {
	.set = link_set,
};
module_param_cb(link, &link_ops, NULL, 0200);

static int __init mosey_dhd_init(void)
{
	link_fn = (link_fn_t)kallsyms_lookup_name("dhd_update_interface_link_status");
	g_dhd_pub_p = (void **)kallsyms_lookup_name("g_dhd_pub");
	if (!link_fn || !g_dhd_pub_p) {
		pr_err("mosey_dhd: bcmdhd symbols not found\n");
		return -ENOENT;
	}
	pr_info("mosey_dhd: loaded (dhd_pub %p)\n", *g_dhd_pub_p);
	return 0;
}

static void __exit mosey_dhd_exit(void)
{
}

module_init(mosey_dhd_init);
module_exit(mosey_dhd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("bcmdhd TX flow rings while unassociated (mosey AirDrop)");
