/* Kernel module to match packet length. */
/* (C) 1999-2001 James Morris <jmorros@intercode.com.au>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ipv6.h>
#include <net/ip.h>

#include <linux/netfilter/xt_length.h>
#include <linux/netfilter/x_tables.h>

MODULE_AUTHOR("James Morris <jmorris@intercode.com.au>");
MODULE_DESCRIPTION("Xtables: Packet length (Layer3,4,5) match");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_length");
MODULE_ALIAS("ip6t_length");

static bool
length_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_length_info *info = par->matchinfo;
	u_int16_t pktlen = ntohs(ip_hdr(skb)->tot_len);

#if defined(CONFIG_BCM_KF_BLOG) && defined(CONFIG_BLOG_FEATURE)
	struct sk_buff *skb_p;
	skb_p = (struct sk_buff *)skb;
	skb_p->ipt_check |= IPT_MATCH_LENGTH;
	skb_p->ipt_log.u32[BLOG_MIN_LEN_INDEX] = info->min;
	skb_p->ipt_log.u32[BLOG_MAX_LEN_INDEX] = info->max;
#endif
	return (pktlen >= info->min && pktlen <= info->max) ^ info->invert;
}

static bool
length_mt6(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_length_info *info = par->matchinfo;
	const u_int16_t pktlen = ntohs(ipv6_hdr(skb)->payload_len) +
				 sizeof(struct ipv6hdr);

#if defined(CONFIG_BCM_KF_BLOG) && defined(CONFIG_BLOG_FEATURE)
	struct sk_buff *skb_p;
	skb_p = (struct sk_buff *)skb;
	skb_p->ipt_check |= IPT_MATCH_LENGTH;
	skb_p->ipt_log.u32[BLOG_MIN_LEN_INDEX] = info->min;
	skb_p->ipt_log.u32[BLOG_MAX_LEN_INDEX] = info->max;
#endif
	return (pktlen >= info->min && pktlen <= info->max) ^ info->invert;
}

static struct xt_match length_mt_reg[] __read_mostly = {
	{
		.name		= "length",
		.family		= NFPROTO_IPV4,
		.match		= length_mt,
		.matchsize	= sizeof(struct xt_length_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "length",
		.family		= NFPROTO_IPV6,
		.match		= length_mt6,
		.matchsize	= sizeof(struct xt_length_info),
		.me		= THIS_MODULE,
	},
};

static int __init length_mt_init(void)
{
	return xt_register_matches(length_mt_reg, ARRAY_SIZE(length_mt_reg));
}

static void __exit length_mt_exit(void)
{
	xt_unregister_matches(length_mt_reg, ARRAY_SIZE(length_mt_reg));
}

module_init(length_mt_init);
module_exit(length_mt_exit);