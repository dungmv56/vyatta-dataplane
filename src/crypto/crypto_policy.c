/*-
 * Copyright (c) 2017-2021, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2015-2017 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include <arpa/inet.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <linux/if_ether.h>
#include <linux/snmp.h>
#include <linux/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <rte_atomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <urcu/uatomic.h>
#include <values.h>
#include <linux/xfrm.h>
#include <netinet/icmp6.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_funcs.h>
#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_ether.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_per_lcore.h>

#include "compiler.h"
#include "capture.h"
#include "compat.h"
#include "control.h"
#include "crypto/crypto.h"
#include "crypto/crypto_forward.h"
#include "crypto/crypto_internal.h"
#include "crypto/crypto_main.h"
#include "crypto/crypto_policy.h"
#include "crypto/crypto_sadb.h"
#include "crypto/esp.h"
#include "if_var.h"
#include "ip_funcs.h"
#include "ip_icmp.h"
#include "json_writer.h"
#include "lcore_sched.h"
#include "nh_common.h"
#include "pipeline/nodes/pl_nodes_common.h"
#include "pktmbuf_internal.h"
#include "pl_node.h"
#include "rldb.h"
#include "route.h"
#include "route_flags.h"
#include "route_v6.h"
#include "shadow.h"
#include "snmp_mib.h"
#include "urcu.h"
#include "vplane_debug.h"
#include "vplane_log.h"
#include "vrf_internal.h"
#include "flow_cache.h"
#include "xfrm_client.h"

#include "protobuf.h"
#include "protobuf_util.h"
#include "protobuf/CryptoPolicyConfig.pb-c.h"
#include "protobuf/IPAddress.pb-c.h"

struct cds_lfht;
struct rte_timer;

#define POLICY_DEBUG(args...)				\
	DP_DEBUG(CRYPTO, DEBUG, POLICY, args)

#define POLICY_ERR(args...)				\
	DP_DEBUG(CRYPTO, ERR, POLICY, args)

#define POLICY_NOTICE(args...)				\
	DP_DEBUG(CRYPTO, NOTICE, POLICY, args)

#define POLICY_INFO(args...)				\
	DP_DEBUG(CRYPTO, INFO, POLICY, args)

/*
 * A binding between a policy rule and a feature attachment point, which
 * is a dummy interface used to attach input and output features that
 * need to be run on packets matching the policy.
 *
 * The interface is stored in a next hop for ease of use in the pipeline
 * path.
 */
struct pr_feat_attach {
	struct next_hop nh;
	struct rcu_head pr_feat_rcu;
};

#define POLICY_F_PENDING_ADD	0x0001
#define POLICY_F_PENDING_DEL	0x0002
#define POLICY_F_PENDING_UPDATE 0x0004
/*
 * struct policy_rule
 *
 * This is the type for entries in the policy rule
 * database. Each entry tracks a single rldb rule handle.
 * A given rldb rule can be used by both an input and
 * output policy since their selectors can overlap.
 *
 * The policy database is indexd by the rule_index
 * value and also by a subset of the fields in the
 * selector.
 */
struct policy_rule {
	int action;
	struct cds_lfht_node sel_ht_node;
	struct xfrm_selector sel;
	struct xfrm_mark mark;
	xfrm_address_t output_peer;
	uint16_t output_peer_af;
	uint32_t reqid;
	struct crypto_overhead overhead;
	int dir;
	vrfid_t vrfid;
	struct rcu_head policy_rule_rcu;
	uint32_t policy_priority;
	uint32_t rule_index;
	bool vti_tunnel_policy;
	uint8_t flags;
	struct pr_feat_attach *feat_attach;
	struct rldb_rule_handle *rh;
};

struct policy_rule_key {
	const struct xfrm_selector *sel;
	const struct xfrm_mark *mark;
};

bool flow_cache_disabled;

uint32_t crypto_rekey_requests;

#define POLICY_RULE_BUFSIZE (1024 * sizeof(char))
#define ATTACH_GROUP_BUFSIZE 32

#define CRYPTO_FLOW_CACHE_MAX_COUNT  8192

static struct flow_cache *flow_cache;

union crypto_ctx {
	uint16_t context;
	struct {
		uint8_t in_rule_checked:1,
			in_rule_drop:1,
			no_rule_fwd:1,
			PR_UNUSED:5;
		char SPARE[7];
	};
};

/*
 * A binding between a s2s policy and a feature attachment point.
 */
struct s2s_binding {
	struct cds_lfht_node bind_ht_node;
	struct rcu_head bind_rcu_head;
	struct xfrm_selector sel;
	vrfid_t vrfid;
	uint ifindex;
};

static struct flow_cache_entry *
crypto_flow_cache_lookup(struct rte_mbuf *m, bool v4)
{
	struct flow_cache_entry *entry;
	int err;

	/* Any host generated packet don't make use of the flow cache table*/
	if (flow_cache_disabled)
		return NULL;

	err = flow_cache_lookup(flow_cache, m,
				v4 ? FLOW_CACHE_IPV4 : FLOW_CACHE_IPV6,
				&entry);
	if (err)
		return NULL;

	return entry;
}

static void crypto_flow_cache_add(struct flow_cache *flow_cache,
				  struct policy_rule *pr, struct rte_mbuf *m,
				  bool v4, bool seen_by_crypto,
				  int dir)
{
	union crypto_ctx ctx = { .context = 0 }; /* SA Fix */
	enum flow_cache_ftype af = v4 ? FLOW_CACHE_IPV4 : FLOW_CACHE_IPV6;
	struct flow_cache_entry *cache_entry;

	if (!flow_cache || flow_cache_disabled)
		return;

	if (pr) {
		if (!seen_by_crypto) {
			ctx.in_rule_checked = 1;
			ctx.in_rule_drop = (pr->action == XFRM_POLICY_BLOCK);
		}
	} else {
		if (dir == XFRM_POLICY_OUT) {
			ctx.in_rule_checked = 0;
			ctx.in_rule_drop = 0;
			ctx.no_rule_fwd = 1;
		}
	}

	/*
	 * In case this is an input policy match, check to see if the
	 * cache exists based on a previous output policy match and if so
	 * update the cache to indicate that the input policy has been checked
	 * for unencrypted packets
	 */
	if (dir == XFRM_POLICY_IN) {
		cache_entry = crypto_flow_cache_lookup(m, v4);
		if (cache_entry) {
			flow_cache_entry_set_info(cache_entry, pr,
						  ctx.context);
			return;
		}
	}

	IPSEC_CNT_INC(FLOW_CACHE_MISS);
	if (flow_cache_add(flow_cache, pr, ctx.context, m, af) != 0)
		IPSEC_CNT_INC(FLOW_CACHE_ADD_FAIL);
	else
		IPSEC_CNT_INC(FLOW_CACHE_ADD);
}

int crypto_flow_cache_init_lcore(unsigned int lcore_id)
{
	int err;

	err = flow_cache_init_lcore(flow_cache, lcore_id);
	return err;
}

int crypto_flow_cache_teardown_lcore(unsigned int lcore_id)
{
	int err;

	err = flow_cache_teardown_lcore(flow_cache, lcore_id);
	return err;
}

int crypto_flow_cache_init(void)
{
	flow_cache = flow_cache_init(CRYPTO_FLOW_CACHE_MAX_COUNT);
	if (!flow_cache)
		return -ENOMEM;

	return 0;
}

void
crypto_flow_cache_timer_handler(struct rte_timer *tmr __rte_unused,
				void *arg __rte_unused)
{
	flow_cache_age(flow_cache);
}

static unsigned long policy_rule_sel_hash(const struct policy_rule_key *key)
{
	unsigned long h;

	/* sel->family is zero for a VTI tunnel, use AF_INET in this case */
	h = hash_xfrm_address(&key->sel->daddr,
			      key->sel->family ?: AF_INET);
	h += hash_xfrm_address(&key->sel->saddr,
			       key->sel->family ?: AF_INET);
	h += key->mark ? key->mark->v : 0;
	h += key->sel->ifindex;
	return (h + key->sel->proto);
}

static int policy_rule_sel_eq(const struct xfrm_selector *sel1,
			      const struct xfrm_selector *sel2)
{
	/*
	 * Note that sel->family is zero for VTI tunnels, in which
	 * case we use AF_INET for address comparisons.
	 */
	return (sel1->family == sel2->family &&
		sel1->proto == sel2->proto &&
		sel1->prefixlen_d == sel2->prefixlen_d &&
		sel1->prefixlen_s == sel2->prefixlen_s &&
		xfrm_addr_eq(&sel1->daddr, &sel2->daddr,
			     sel1->family ?: AF_INET) &&
		xfrm_addr_eq(&sel1->saddr, &sel2->saddr,
			     sel1->family ?: AF_INET) &&
		sel1->dport == sel2->dport &&
		sel1->sport == sel2->sport &&
		sel1->ifindex == sel2->ifindex);
}

static int policy_rule_sel_match(struct cds_lfht_node *node, const void *key)
{
	const struct policy_rule_key *search_key;
	const struct policy_rule *pr;

	pr = caa_container_of(node, const struct policy_rule, sel_ht_node);

	search_key = (const struct policy_rule_key *)key;

	/*
	 * Match if and only if the all the fields used to build the
	 * rldb rule are the same (see policy_prepare_rldb_rule()).
	 *
	 */
	return (policy_rule_sel_eq(&pr->sel, search_key->sel) &&
		((search_key->mark && (pr->mark.v == search_key->mark->v)) ||
		 (!search_key->mark && (pr->mark.v == 0))));
}

static bool policy_rule_add_to_selector_ht(struct policy_rule *pr)
{
	struct cds_lfht_node *ret_node;
	struct cds_lfht *hash_table;
	struct policy_rule_key key;
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_get(pr->vrfid);
	if (!vrf_ctx)
		return false;

	switch (pr->dir) {
	case XFRM_POLICY_IN:
		hash_table = vrf_ctx->input_policy_rule_sel_ht;
		break;
	case XFRM_POLICY_OUT:
		hash_table = vrf_ctx->output_policy_rule_sel_ht;
		break;
	default:
		POLICY_ERR(
			"Failed to add policy rule to hash table: Bad direction\n");
		return false;
	}

	key.sel = &pr->sel;
	key.mark = &pr->mark;

	ret_node = cds_lfht_add_unique(hash_table,
				       policy_rule_sel_hash(&key),
				       policy_rule_sel_match,
				       &key, &pr->sel_ht_node);
	if (ret_node != &pr->sel_ht_node) {
		POLICY_ERR("Failed to add rule to selector hash table\n");
		return false;
	}

	return true;
}

static void policy_rule_remove_from_selector_ht(struct policy_rule *pr)
{
	struct cds_lfht *hash_table;
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_find(pr->vrfid);
	if (!vrf_ctx)
		return;

	switch (pr->dir) {
	case XFRM_POLICY_IN:
		hash_table = vrf_ctx->input_policy_rule_sel_ht;
		break;
	case XFRM_POLICY_OUT:
		hash_table = vrf_ctx->output_policy_rule_sel_ht;
		break;
	default:
		POLICY_ERR(
			"Failed to remove policy rule from hash table: Bad direction\n");
		return;
	}

	cds_lfht_del(hash_table, &pr->sel_ht_node);
}

static struct policy_rule*
policy_rule_find_by_selector(vrfid_t vrfid,
			     const struct xfrm_selector *sel,
			     const struct xfrm_mark *mark,
			     int policy_direction)
{
	struct cds_lfht *hash_table;
	struct policy_rule_key key;
	struct cds_lfht_node *node;
	struct cds_lfht_iter iter;
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_find(vrfid);
	if (!vrf_ctx)
		return NULL;

	switch (policy_direction) {
	case XFRM_POLICY_IN:
		hash_table = vrf_ctx->input_policy_rule_sel_ht;
		break;
	case XFRM_POLICY_OUT:
		hash_table = vrf_ctx->output_policy_rule_sel_ht;
		break;
	default:
		POLICY_ERR(
			"Failed to find policy rule in hash table: Bad direction\n");
		return NULL;
	}

	key.sel = sel;
	key.mark = mark;

	cds_lfht_lookup(hash_table,
			policy_rule_sel_hash(&key),
			policy_rule_sel_match, &key, &iter);

	node = cds_lfht_iter_get_node(&iter);

	return node ? caa_container_of(node, struct policy_rule, sel_ht_node)
		    : NULL;
}

static bool policy_prepare_rldb_rule(struct policy_rule *pr,
				     struct rldb_rule_spec *rule)
{
	rule->rldb_user_data = (uintptr_t)pr;
	rule->rldb_priority = pr->policy_priority;

	if (pr->sel.proto) {
		rule->rldb_flags |= NPFRL_FLAG_PROTO;
		rule->rldb_proto.npfrl_proto = pr->sel.proto;
	}

	if (pr->sel.family == AF_INET) {
		struct rldb_v4_prefix *pfx;
		rule->rldb_flags |= NPFRL_FLAG_V4_PFX;

		/* src */
		if (pr->sel.prefixlen_s) {
			rule->rldb_flags |= NPFRL_FLAG_SRC_PFX;
			pfx = &rule->rldb_src_addr.v4_pfx;
			memcpy(pfx->npfrl_bytes, &pr->sel.saddr.a4,
			       sizeof(pfx->npfrl_bytes));
			pfx->npfrl_plen = pr->sel.prefixlen_s;
		}

		/* dst */
		if (pr->sel.prefixlen_d) {
			rule->rldb_flags |= NPFRL_FLAG_DST_PFX;
			pfx = &rule->rldb_dst_addr.v4_pfx;
			memcpy(pfx->npfrl_bytes, &pr->sel.daddr.a4,
			       sizeof(pfx->npfrl_bytes));
			pfx->npfrl_plen = pr->sel.prefixlen_d;
		}
	} else if (pr->sel.family == AF_INET6) {
		struct rldb_v6_prefix *pfx;
		rule->rldb_flags |= NPFRL_FLAG_V6_PFX;

		/* src */
		if (pr->sel.prefixlen_s) {
			rule->rldb_flags |= NPFRL_FLAG_SRC_PFX;
			pfx = &rule->rldb_src_addr.v6_pfx;
			memcpy(pfx->npfrl_bytes, &pr->sel.saddr.a6,
			       sizeof(pfx->npfrl_bytes));
			pfx->npfrl_plen = pr->sel.prefixlen_s;
		}

		/* dst */
		if (pr->sel.prefixlen_d) {
			rule->rldb_flags |= NPFRL_FLAG_DST_PFX;
			pfx = &rule->rldb_dst_addr.v6_pfx;
			memcpy(pfx->npfrl_bytes, &pr->sel.daddr.a6,
			       sizeof(pfx->npfrl_bytes));
			pfx->npfrl_plen = pr->sel.prefixlen_d;
		}
	} else {
		POLICY_ERR(
			"Failed to add policy rule: AF not supported\n");
		return false;
	}

	/*
	 * Port ranges are not yet supported.
	 * This mimics the pre-RLDB behavior.
	 */
	if (pr->sel.sport) {
		rule->rldb_flags |= NPFRL_FLAG_SRC_PORT_RANGE;
		rule->rldb_src_port_range.npfrl_loport = pr->sel.sport;
		rule->rldb_src_port_range.npfrl_hiport = pr->sel.sport;
	}

	if (pr->sel.dport) {
		rule->rldb_flags |= NPFRL_FLAG_DST_PORT_RANGE;
		rule->rldb_dst_port_range.npfrl_loport = pr->sel.dport;
		rule->rldb_dst_port_range.npfrl_hiport = pr->sel.dport;
	}

	return true;
}

static
struct rldb_db_handle *policy_rule_get_rldb(struct crypto_vrf_ctx *vrf_ctx,
					    struct policy_rule *pr)
{
	struct rldb_db_handle *db = NULL;

	switch (pr->dir) {
	case XFRM_POLICY_IN:
		if (pr->sel.family == AF_INET)
			db = vrf_ctx->input_policy_v4_rldb;
		else
			db = vrf_ctx->input_policy_v6_rldb;
		break;
	case XFRM_POLICY_OUT:
		if (pr->sel.family == AF_INET)
			db = vrf_ctx->output_policy_v4_rldb;
		else
			db = vrf_ctx->output_policy_v6_rldb;
		break;
	default:
		POLICY_ERR(
			"Failed to find rule database: Bad direction\n");
		break;
	}

	return db;
}

static bool policy_rule_add_to_rldb(struct crypto_vrf_ctx *vrf_ctx,
				    struct policy_rule *pr)
{
	int rc;
	struct rldb_db_handle *db = NULL;
	struct rldb_rule_spec rule = { 0 };

	/*
	 * Packets are routed into VTI tunnels, so we
	 * don't need to create RLDB rules for them.
	 */
	if (pr->vti_tunnel_policy)
		return true;

	rc = policy_prepare_rldb_rule(pr, &rule);
	if (rc < 0)
		return false;

	db = policy_rule_get_rldb(vrf_ctx, pr);
	if (!db)
		return false;

	rc = rldb_add_rule(db, pr->rule_index, &rule, &pr->rh);
	if (rc < 0) {
		POLICY_ERR("Failed to add policy rule to rule database\n");
		return false;
	}

	if (pr->sel.family == AF_INET) {
		++vrf_ctx->crypto_total_ipv4_policies;
		POLICY_DEBUG("Active IPv4 policies: %d\n",
			     vrf_ctx->crypto_total_ipv4_policies);
	} else {
		++vrf_ctx->crypto_total_ipv6_policies;
		POLICY_DEBUG("Active IPv6 policies: %d\n",
			     vrf_ctx->crypto_total_ipv6_policies);
	}

	return true;
}

static void
policy_rule_set_peer_info(struct policy_rule *pr,
			  const struct xfrm_user_tmpl *tmpl,
			  const xfrm_address_t *dst)
{
	struct ifnet *ifp;

	ifp = pr->feat_attach ?
		dp_nh_get_ifp(&pr->feat_attach->nh) : NULL;
	pr->reqid = tmpl->reqid;
	pr->output_peer_af = tmpl->family;
	memcpy(&pr->output_peer, dst, sizeof(pr->output_peer));
	crypto_sadb_feat_attach_in(pr->reqid, ifp);

	if (pr->vti_tunnel_policy)
		vti_reqid_set(&pr->output_peer, pr->output_peer_af,
			      pr->mark.v, pr->reqid);
	else
		crypto_sadb_tunl_overhead_subscribe(pr->reqid, &pr->overhead,
						    pr->vrfid);
}

static void
policy_rule_set_mark(struct policy_rule *pr, const struct xfrm_mark *mark)
{
	if (mark) {
		/*
		 * This policy is for a VTI tunnel so we inhibit
		 * the creation of an rldb rule as these are only
		 * required for site-to site tunnels.
		 */
		pr->vti_tunnel_policy = true;
		pr->mark.v = mark->v;
		pr->mark.m = mark->m;
	} else  {
		pr->vti_tunnel_policy = false;
		pr->mark.v =  0;
		pr->mark.m = 0;
	}
}

static struct policy_rule *
policy_rule_create(const struct xfrm_userpolicy_info *usr_policy,
		   const struct xfrm_user_tmpl *tmpl,
		   const struct xfrm_mark *mark,
		   const xfrm_address_t *dst,
		   vrfid_t vrfid)
{
	struct policy_rule *pr;

	pr = zmalloc_aligned(sizeof(*pr));
	if (!pr) {
		POLICY_ERR("Policy rule allocation failed\n");
		return NULL;
	}

	/*
	 * INPUT policies are programmed as DROP for unencrypted
	 * traffic and not checked for encrypted traffic except
	 * for passthrough policies.
	 */
	pr->action = (((usr_policy->dir == XFRM_POLICY_IN) && (tmpl != NULL)) ?
		      XFRM_POLICY_BLOCK : usr_policy->action);
	pr->dir = usr_policy->dir;
	pr->policy_priority = usr_policy->priority;
	pr->rule_index = usr_policy->index;
	memcpy(&pr->sel, &usr_policy->sel, sizeof(pr->sel));

	policy_rule_set_mark(pr, mark);
	pr->vrfid = vrfid;

	if ((usr_policy->dir == XFRM_POLICY_OUT) &&
	    (usr_policy->action == XFRM_POLICY_ALLOW)) {
		/*
		 * dst and tmpl should be both NULL or both not NULL
		 */
		if (!!dst != !!tmpl) {
			POLICY_ERR(
				"Failed to create policy rule: "
				"Mismatch of tmpl and dst\n");

			free(pr);
			return NULL;
		}
		if (tmpl && dst)
			policy_rule_set_peer_info(pr, tmpl, dst);
	}

	cds_lfht_node_init(&pr->sel_ht_node);
	return pr;
}

static void policy_feat_attach_free(struct rcu_head *head)
{
	struct pr_feat_attach *attach =
		caa_container_of(head, struct pr_feat_attach, pr_feat_rcu);

	free(attach);
}

static void policy_feat_attach_destroy(struct policy_rule *pr)
{
	struct pr_feat_attach *attach;

	attach = pr->feat_attach;

	if (attach) {
		rcu_assign_pointer(pr->feat_attach, NULL);
		call_rcu(&attach->pr_feat_rcu, policy_feat_attach_free);
	}
}

static void policy_rule_rcu_free(struct rcu_head *head)
{
	struct policy_rule *pr;

	pr = caa_container_of(head, struct policy_rule, policy_rule_rcu);

	free(pr);
}

static void policy_rule_rcu_invalidate(struct rcu_head *head)
{
	/*
	 * The callback to invalidate a policy rule. At this stage
	 * we need to make sure that the policy rule is no longer in
	 * any of the pr caches.
	 */
	flow_cache_invalidate(flow_cache, true, true);
	/*
	 * Now the PR is gone from the cache, but other threads
	 * may still hold references to it, so wait for another
	 * grace period before freeing the PR.
	 */
	call_rcu(head, policy_rule_rcu_free);
}

static void policy_rule_destroy(struct policy_rule *pr)
{
	if ((pr->dir == XFRM_POLICY_OUT) &&
	    (pr->action == XFRM_POLICY_ALLOW)) {
		if (pr->vti_tunnel_policy)
			vti_reqid_clear(&pr->output_peer, pr->output_peer_af,
					pr->mark.v);
		else
			crypto_sadb_tunl_overhead_unsubscribe(
				pr->reqid,
				&pr->overhead,
				pr->vrfid);
	}

	policy_feat_attach_destroy(pr);
	pr->flags |= POLICY_F_PENDING_DEL;
	call_rcu(&pr->policy_rule_rcu, policy_rule_rcu_invalidate);
}

static bool policy_rule_add_to_hash_tables(struct policy_rule *pr)
{
	if (!policy_rule_add_to_selector_ht(pr))
		return false;

	return true;
}

static void policy_rule_remove_from_hash_tables(struct policy_rule *pr)
{
	policy_rule_remove_from_selector_ht(pr);
}

static bool policy_rule_update_rldb(struct policy_rule *pr)
{
	int rc;
	struct rldb_db_handle *db;
	struct rldb_rule_spec rule = { 0 };
	struct crypto_vrf_ctx *vrf_ctx;


	/*
	 * Packets are routed into VTI tunnels,
	 * so we don't have an rldb rule to update.
	 */
	if (pr->vti_tunnel_policy)
		return true;

	vrf_ctx = crypto_vrf_get(pr->vrfid);
	if (!vrf_ctx)
		return false;

	db = policy_rule_get_rldb(vrf_ctx, pr);
	if (!db)
		return false;

	rc = rldb_del_rule(db, pr->rh);
	if (rc < 0) {
		POLICY_ERR("Failed to update rule %u: %d\n",
			   pr->rule_index, -rc);
		return false;
	}

	rc = policy_prepare_rldb_rule(pr, &rule);
	if (rc < 0)
		return false;

	rc = rldb_add_rule(db, pr->rule_index, &rule, &pr->rh);
	if (rc < 0) {
		POLICY_ERR("Failed to update rule %u: %d\n",
			   pr->rule_index, -rc);
		return false;
	}

	return true;
}

static bool all_other_policies_can_be_cached(struct crypto_vrf_ctx *vrf_ctx,
					     const struct policy_rule *pr)
{
	bool result = true;

	const struct policy_rule *check_pr;
	struct cds_lfht_iter iter;
	struct cds_lfht *ht;

	ht = vrf_ctx->output_policy_rule_sel_ht;

	cds_lfht_for_each_entry(ht, &iter, check_pr, sel_ht_node) {
		if (check_pr == pr)
			continue;
		if ((check_pr->sel.sport > 0) || (check_pr->sel.dport > 0)) {
			result = false;
			break;
		}
	}

	return result;
}

static int policy_rule_remove_from_rldb(struct policy_rule *pr,
					struct crypto_vrf_ctx *vrf_ctx,
					bool vti_tunnel_policy,
					struct rldb_rule_handle *rh)
{
	int rc;
	struct rldb_db_handle *db;

	/* We don't create rldb rules for VTI tunnel policies */
	if (vti_tunnel_policy)
		return 0;

	db = policy_rule_get_rldb(vrf_ctx, pr);
	if (!db) {
		POLICY_ERR("Failed to delete policy\n");
		return -1;
	}

	rc = rldb_del_rule(db, rh);
	if (rc < 0) {
		POLICY_ERR("Failed to delete policy from rule database\n");
		return -1;
	}

	if (pr->sel.family == AF_INET) {
		--vrf_ctx->crypto_total_ipv4_policies;
		POLICY_DEBUG("Remaining IPv4 policies: %d\n",
			     vrf_ctx->crypto_total_ipv4_policies);
	} else {
		--vrf_ctx->crypto_total_ipv6_policies;
		POLICY_DEBUG("Remaining IPv6 policies: %d\n",
			     vrf_ctx->crypto_total_ipv6_policies);
	}

	return 0;
}

static unsigned long policy_bind_sel_hash(const struct xfrm_selector *sel)
{
	unsigned long h;

	h = hash_xfrm_address(&sel->daddr, sel->family);
	h += hash_xfrm_address(&sel->saddr, sel->family);

	return (h + sel->proto);
}

static int policy_bind_sel_match(struct cds_lfht_node *node, const void *key)
{
	const struct s2s_binding *bind;
	const struct xfrm_selector *sel;

	bind = caa_container_of(node, const struct s2s_binding, bind_ht_node);
	sel = (const struct xfrm_selector *) key;

	return policy_rule_sel_eq(sel, &bind->sel);
}

static void bind_table_vrf_inc(vrfid_t vrfid)
{
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_get(vrfid);
	if (!vrf_ctx)
		return;

	vrf_ctx->s2s_bindings++;
}

static void bind_table_vrf_dec(vrfid_t vrfid)
{
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_get(vrfid);
	if (!vrf_ctx)
		return;

	vrf_ctx->s2s_bindings--;
}

static struct cds_lfht *bind_table_vrf_get(vrfid_t vrfid)
{
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_get(vrfid);
	if (!vrf_ctx)
		return NULL;

	return vrf_ctx->s2s_bind_hash_table;
}

static void policy_bind_free(struct rcu_head *rcu_head)
{
	struct s2s_binding *bind = caa_container_of(rcu_head,
						    struct s2s_binding,
						    bind_rcu_head);
	free(bind);
}

static void policy_bind_del(struct s2s_binding *bind)
{
	struct cds_lfht *bind_table;

	bind_table = bind_table_vrf_get(bind->vrfid);

	if (!bind_table) {
		POLICY_ERR("Failed to get binding table for del\n");
		return;
	}

	cds_lfht_del(bind_table, &bind->bind_ht_node);
	bind_table_vrf_dec(bind->vrfid);
	call_rcu(&bind->bind_rcu_head, policy_bind_free);
}

static struct s2s_binding *policy_bind_lookup(vrfid_t vrfid,
					      const struct xfrm_selector *sel)
{
	struct cds_lfht *bind_table;
	struct cds_lfht_node *node;
	struct cds_lfht_iter iter;

	bind_table = bind_table_vrf_get(vrfid);

	if (!bind_table) {
		POLICY_ERR("Failed to get binding table for lookup\n");
		return NULL;
	}

	cds_lfht_lookup(bind_table,
			policy_bind_sel_hash(sel),
			policy_bind_sel_match, sel, &iter);

	node = cds_lfht_iter_get_node(&iter);

	if (node)
		return caa_container_of(node, struct s2s_binding, bind_ht_node);

	return NULL;
}

static void policy_bind_feat_attach(vrfid_t vrfid,
				    const struct xfrm_selector *sel,
				    uint ifindex)
{
	struct policy_rule *pr;
	struct xfrm_mark mark;
	struct ifnet *ifp;
	struct pr_feat_attach *attach;

	mark.v = mark.m = 0;
	pr = policy_rule_find_by_selector(vrfid, sel, &mark, XFRM_POLICY_OUT);

	if (!pr) {
		POLICY_DEBUG("Failed bind lookup for policy\n");
		return;
	}

	if (!pr->feat_attach) {
		attach = calloc(1, sizeof(*attach));
		if (!attach) {
			POLICY_ERR("Failed to alloc nh for s2s bind\n");
			return;
		}
		rcu_assign_pointer(pr->feat_attach, attach);
	}

	ifp = dp_ifnet_byifindex(ifindex);
	if (!ifp) {
		POLICY_DEBUG("Failed bind lookup for ifi %u\n", ifindex);
		return;
	}

	if (!is_s2s_feat_attach(ifp)) {
		POLICY_ERR("Bind failed, %s not an s2s vfp\n", ifp->if_name);
		return;
	}

	nh_set_ifp(&pr->feat_attach->nh, ifp);

	/*
	 * If there are any SAs already present for this policy, we
	 * need to find them via the policy reqid and bind them to
	 * the virtual feature point.
	 */
	crypto_sadb_feat_attach_in(pr->reqid, ifp);
}

/*
 * Do we have a binding already matching this newly created
 * policy rule?
 */
static void policy_update_pending_vfp_bind(vrfid_t vrfid,
					   struct policy_rule *pr)
{
	struct s2s_binding *bind;

	bind = policy_bind_lookup(vrfid, &pr->sel);
	if (!bind)
		return;

	policy_bind_feat_attach(vrfid, &bind->sel, bind->ifindex);
}

static uint32_t crypto_npf_cfg_commit_count;
static struct rte_timer crypto_npf_cfg_commit_all_timer;

#define CRYPTO_NPF_CFG_COMMIT_FORCE_COUNT 2000

static uint32_t batch_seq[CRYPTO_NPF_CFG_COMMIT_FORCE_COUNT];
static struct policy_rule *batch_pr[CRYPTO_NPF_CFG_COMMIT_FORCE_COUNT];

static int crypto_policy_rldb_commit(struct crypto_vrf_ctx *vrf_ctx)
{
	int rc = 0;

	if (rldb_commit_transaction(vrf_ctx->input_policy_v4_rldb) < 0) {
		POLICY_ERR("Policy commit for IPv4 inbound failed\n");
		rc = -1;
	}
	rldb_start_transaction(vrf_ctx->input_policy_v4_rldb);

	if (rldb_commit_transaction(vrf_ctx->output_policy_v4_rldb) < 0) {
		POLICY_ERR("Policy commit for IPv4 outbound failed\n");
		rc = -1;
	}
	rldb_start_transaction(vrf_ctx->output_policy_v4_rldb);

	if (rldb_commit_transaction(vrf_ctx->input_policy_v6_rldb) < 0) {
		POLICY_ERR("Policy commit for IPv6 inbound failed\n");
		rc = -1;
	}
	rldb_start_transaction(vrf_ctx->input_policy_v6_rldb);

	if (rldb_commit_transaction(vrf_ctx->output_policy_v6_rldb) < 0) {
		POLICY_ERR("Policy commit for IPv6 outbound failed\n");
		rc = -1;
	}
	rldb_start_transaction(vrf_ctx->output_policy_v6_rldb);

	return rc;
}

void crypto_npf_cfg_commit_flush(void)
{
	vrfid_t vrf_id;
	struct vrf *vrf;
	struct crypto_vrf_ctx *vrf_ctx;
	uint32_t i;
	int rc = MNL_CB_OK;

	VRF_FOREACH(vrf, vrf_id) {
		vrf_ctx = crypto_vrf_find(vrf_id);
		if (!vrf_ctx)
			continue;

		if (crypto_policy_rldb_commit(vrf_ctx) < 0)
			rc = MNL_CB_ERROR;

		/* Enable IPsec in IPv4 feature pipeline and
		 * update live count.
		 */

		if (vrf_ctx->crypto_live_ipv4_policies == 0 &&
		    vrf_ctx->crypto_total_ipv4_policies > 0) {

			pl_node_add_feature_by_inst(&ipv4_ipsec_out_feat, vrf);

		} else if (vrf_ctx->crypto_live_ipv4_policies > 0 &&
			   vrf_ctx->crypto_total_ipv4_policies == 0) {

			pl_node_remove_feature_by_inst(&ipv4_ipsec_out_feat,
					       vrf);
		}

		vrf_ctx->crypto_live_ipv4_policies =
			vrf_ctx->crypto_total_ipv4_policies;


		/* Enable IPsec in IPv6 feature pipeline and
		 * update live count.
		 */

		if (vrf_ctx->crypto_live_ipv6_policies == 0 &&
		    vrf_ctx->crypto_total_ipv6_policies > 0) {

			pl_node_add_feature_by_inst(&ipv6_ipsec_out_feat, vrf);

		} else if (vrf_ctx->crypto_live_ipv6_policies > 0 &&
			   vrf_ctx->crypto_total_ipv6_policies == 0) {

			pl_node_remove_feature_by_inst(&ipv6_ipsec_out_feat,
						       vrf);
		}

		vrf_ctx->crypto_live_ipv6_policies =
			vrf_ctx->crypto_total_ipv6_policies;
	}

	/*
	 * There is an assumption that npf_cfg_commit_all completed
	 * successfully as there is no return value. Any issues should
	 * have been caught when the individual policies were added
	 * at which point an error should have been returned to the
	 * xfrm source.
	 */
	for (i = 0; i < crypto_npf_cfg_commit_count ; i++) {
		if (batch_pr[i])
			batch_pr[i]->flags &=
				~(POLICY_F_PENDING_ADD |
				  POLICY_F_PENDING_UPDATE);
		if (xfrm_direct)
			xfrm_client_send_ack(batch_seq[i], rc);
	}

	crypto_npf_cfg_commit_count = 0;
	flow_cache_invalidate(flow_cache, flow_cache_disabled,
			      false);

}

static void crypto_npf_cfg_commit_all_timer_handler(
	struct rte_timer *timer __rte_unused,
	void *arg __rte_unused)
{
	ASSERT_MAIN();
	if (crypto_npf_cfg_commit_count)
		crypto_npf_cfg_commit_flush();
}

/*
 * As the npf commit is slow and does a rebuild of the entire state
 * batch up the calls to it. This can possibly delay the application of
 * a rule, but overall will be much faster.
 */
static void crypto_npf_cfg_commit_all(struct policy_rule *pr,
				      uint32_t seq)
{
	ASSERT_MAIN();

	/*
	 * If the xfrm_direct path is not programming the classifier
	 * then no batch completed will be signal and so the existing
	 * timer based mechanism is required to commit the policies.
	 */
	if (!xfrm_direct && crypto_npf_cfg_commit_count == 0) {
		rte_timer_reset(&crypto_npf_cfg_commit_all_timer,
				rte_get_timer_hz(),
				SINGLE, rte_get_master_lcore(),
				crypto_npf_cfg_commit_all_timer_handler, NULL);
	}

	if (xfrm_direct)
		batch_seq[crypto_npf_cfg_commit_count] = seq;
	else
		batch_seq[crypto_npf_cfg_commit_count] = 0;

	if (!(pr->flags & POLICY_F_PENDING_DEL))
		batch_pr[crypto_npf_cfg_commit_count] = pr;
	else
		batch_pr[crypto_npf_cfg_commit_count] = NULL;
	crypto_npf_cfg_commit_count++;

	/* Force the commit if we have batched up too many */
	if (crypto_npf_cfg_commit_count == CRYPTO_NPF_CFG_COMMIT_FORCE_COUNT)
		crypto_npf_cfg_commit_flush();
}

static bool
policy_rule_update(struct policy_rule *pr,
		   struct crypto_vrf_ctx *vrf_ctx,
		   const struct xfrm_userpolicy_info *usr_policy,
		   const xfrm_address_t *dst,
		   const struct xfrm_user_tmpl *tmpl,
		   const struct xfrm_mark *mark,
		   uint32_t seq,
		   bool *send_ack)
{
	bool was_vti_policy = pr->vti_tunnel_policy;
	bool changed = false;

	*send_ack = true;

	if (usr_policy->dir == XFRM_POLICY_OUT) {

		if ((usr_policy->action == XFRM_POLICY_ALLOW) &&
		    (pr->action == XFRM_POLICY_BLOCK)) {
			/*
			 * For a change from BLOCK to ALLOW for an output policy
			 * we need to populate the peer, peer_af and reqid,
			 * which come from the TMPL attribute.
			 */
			if (!tmpl || !dst) {
				POLICY_ERR(
					"Policy update to allow ignored: missing TMPL or destination\n");
				return false;
			}

			policy_rule_set_mark(pr, mark);
			policy_rule_set_peer_info(pr, tmpl, dst);
			changed = true;

		} else if ((usr_policy->action == XFRM_POLICY_BLOCK) &&
			   (pr->action == XFRM_POLICY_ALLOW)) {
			/*
			 * For a change from ALLOW to BLOCK, we need to
			 * unsubscribe from crypto overhead updates.
			 */
			if (pr->vti_tunnel_policy)
				vti_reqid_clear(&pr->output_peer,
						pr->output_peer_af,
						pr->mark.v);
			else
				crypto_sadb_tunl_overhead_unsubscribe(
					pr->reqid,
					&pr->overhead,
					pr->vrfid);

			policy_rule_set_mark(pr, mark);
			changed = true;
		}
		pr->action = usr_policy->action;
	} else {
		/*
		 * Non-passthrough INPUT policies are always treated as BLOCK
		 * for unencrypted traffic and not checked for encrypted
		 * traffic
		 */
		if (tmpl != NULL)
			pr->action = XFRM_POLICY_BLOCK;
	}

	if (pr->policy_priority != usr_policy->priority) {
		struct rldb_rule_handle *old_rh = pr->rh;
		/*
		 * Since the priority of the policy has changed, we must
		 * insert a new rldb rule with an index that is based on
		 * the new priority and remove the old rule.
		 */
		pr->policy_priority = usr_policy->priority;
		pr->rule_index = usr_policy->index;

		policy_rule_remove_from_rldb(pr, vrf_ctx, was_vti_policy,
					     old_rh);

		if (!policy_rule_add_to_rldb(vrf_ctx, pr)) {
			POLICY_ERR(
				"Failed to add updated policy rule to rldb\n");
			return false;
		}

		*send_ack = false;
		pr->flags |= POLICY_F_PENDING_UPDATE;
		crypto_npf_cfg_commit_all(pr, seq);
		if ((pr->dir == XFRM_POLICY_OUT) &&
		    (!was_vti_policy || !pr->vti_tunnel_policy))
			flow_cache_invalidate(flow_cache, flow_cache_disabled,
					      false);
	} else if (changed) {
		if (!policy_rule_update_rldb(pr)) {
			POLICY_ERR("Failed to update rldb rule %u\n",
				   pr->rule_index);
			return false;
		}
		pr->flags |= POLICY_F_PENDING_UPDATE;
		crypto_npf_cfg_commit_all(pr, seq);
		*send_ack = false;
	}

	/* Check if this update means we need to rebind */
	policy_update_pending_vfp_bind(pr->vrfid, pr);
	return true;
}

/*
 * crypto_policy_add()
 *
 * Add a new IPsec policy to the policy DB.
 *
 * MUST be called from the main thread
 */
int crypto_policy_add(const struct xfrm_userpolicy_info *usr_policy,
		      const xfrm_address_t *dst,
		      const struct xfrm_user_tmpl *tmpl,
		      const struct xfrm_mark *mark,
		      vrfid_t vrfid,
		      uint32_t seq,
		      bool *send_ack)
{
	struct policy_rule *pr;
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_get(vrfid);
	if (!vrf_ctx)
		return -1;

	*send_ack = true;

	pr = policy_rule_find_by_selector(vrfid, &usr_policy->sel, mark,
					  usr_policy->dir);
	if (pr) {
		if (!policy_rule_update(pr, vrf_ctx, usr_policy, dst, tmpl,
					mark, seq, send_ack)) {
			POLICY_ERR(
				"Policy add failed to update existing policy\n");
			return -1;
		}
		return 1;
	}

	pr = policy_rule_create(usr_policy, tmpl, mark, dst, vrfid);
	if (!pr) {
		POLICY_ERR("Failed to create policy rule\n");
		return -1;
	}

	pr->flags = POLICY_F_PENDING_ADD;

	if (!policy_rule_add_to_hash_tables(pr)) {
		POLICY_ERR("Failed to add policy rule to hash tables\n");
		policy_rule_destroy(pr);
		return -1;
	}

	if (!policy_rule_add_to_rldb(vrf_ctx, pr)) {
		POLICY_ERR("Failed to add policy rule to rldb\n");
		policy_rule_remove_from_hash_tables(pr);
		policy_rule_destroy(pr);
		return -1;
	}

	*send_ack = false;
	crypto_npf_cfg_commit_all(pr, seq);
	/*
	 * Any policy rule added, where the port is specified as part of the
	 * selection criteria, the cache is disabled.
	 */
	if (pr->dir == XFRM_POLICY_OUT) {
		if (!flow_cache_disabled) {
			flow_cache_disabled = ((pr->sel.sport > 0) ||
					       (pr->sel.dport > 0));
			flow_cache_invalidate(flow_cache, flow_cache_disabled,
					      false);
		}

		/*
		 * There may already be a pending binding to a feature
		 * attachment point for this policy.
		 */
		policy_update_pending_vfp_bind(vrfid, pr);
	}

	return 1;
}

/*
 * crypto_policy_update()
 *
 * Update an IPsec policy that is already in the policy DB.
 *
 * MUST be called from the main thread
 */
int crypto_policy_update(const struct xfrm_userpolicy_info *usr_policy,
			 const xfrm_address_t *dst,
			 const struct xfrm_user_tmpl *tmpl,
			 const struct xfrm_mark *mark,
			 vrfid_t vrfid,
			 uint32_t seq,
			 bool *send_ack)
{
	struct policy_rule *pr;
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_get(vrfid);
	if (!vrf_ctx)
		return -1;

	pr = policy_rule_find_by_selector(vrfid, &usr_policy->sel, mark,
					  usr_policy->dir);
	if (!pr) {
		POLICY_INFO("Could not update policy: Not found\n");

		/*
		 * This failure could have been due to a dataplane
		 * restart and the controller collapsed the add, so
		 * treat it like an add now.
		 */
		return crypto_policy_add(usr_policy, dst, tmpl, mark, vrfid,
					 seq, send_ack);
	}

	if (!policy_rule_update(pr, vrf_ctx, usr_policy, dst, tmpl, mark,
				seq, send_ack)) {
		POLICY_ERR("Failed to update existing policy\n");
		return -1;
	}

	return 1;
}

/*
 * crypto_policy_delete()
 *
 * Remove an IPsec policy from the policy DB.
 *
 * MUST be called from the main thread
 */
static void crypto_policy_delete_internal(struct policy_rule *pr,
					  struct crypto_vrf_ctx *vrf_ctx,
					  uint32_t seq, bool ack)
{
	policy_rule_remove_from_rldb(pr, vrf_ctx, pr->vti_tunnel_policy,
				     pr->rh);

	/*
	 * Is the policy is being purged as the result of a flush
	 * style event?.  If so no ack needs to be generated, as the
	 * event is not called due to the reception of a policy delete
	 * from strongswan.
	 */
	if (ack) {
		pr->flags |= POLICY_F_PENDING_DEL;
		crypto_npf_cfg_commit_all(pr, seq);
	}

	if (pr->dir == XFRM_POLICY_OUT) {
		/*
		 * The cache is disabled any time there is a policy that
		 * specifies a source or destination port. If the policy
		 * we're deleting is one such, we may be able to enable
		 * it if it was the only one.
		 */
		if (flow_cache_disabled &&
		    ((pr->sel.sport > 0) || (pr->sel.dport > 0)) &&
		    all_other_policies_can_be_cached(vrf_ctx, pr))
			flow_cache_disabled = false;

		if (!flow_cache_disabled)
			flow_cache_invalidate(flow_cache, flow_cache_disabled,
					      false);
	}

	policy_rule_remove_from_hash_tables(pr);
	policy_rule_destroy(pr);

	crypto_vrf_check_remove(vrf_ctx);
}

void crypto_policy_delete(const struct xfrm_userpolicy_id *id,
			  const struct xfrm_mark *mark,
			  vrfid_t vrfid,
			  uint32_t seq, bool *send_ack)
{
	struct policy_rule *pr;
	struct crypto_vrf_ctx *vrf_ctx;

	vrf_ctx = crypto_vrf_find(vrfid);
	if (!vrf_ctx)
		return;

	pr = policy_rule_find_by_selector(vrfid, &id->sel, mark, id->dir);
	if (!pr) {
		/*
		 * Might have been removed by a flush,
		 * or never received if there was a dp restart
		 */
		*send_ack = true;
		return;
	}

	crypto_policy_delete_internal(pr, vrf_ctx, seq, true);
}

void crypto_policy_flush_vrf(struct crypto_vrf_ctx *vrf_ctx)
{
	struct cds_lfht_iter iter;
	struct policy_rule *pr;

	POLICY_DEBUG("Flush all policies for VRF %d\n", vrf_ctx->vrfid);

	cds_lfht_for_each_entry(vrf_ctx->input_policy_rule_sel_ht,
				&iter, pr, sel_ht_node) {
		crypto_policy_delete_internal(pr, vrf_ctx, 0, false);
	}

	cds_lfht_for_each_entry(vrf_ctx->output_policy_rule_sel_ht,
				&iter, pr, sel_ht_node) {
		crypto_policy_delete_internal(pr, vrf_ctx, 0, false);
	}
}

int crypto_policy_get_vti_reqid(vrfid_t vrfid,
				const xfrm_address_t *peer, uint8_t family,
				uint32_t mark_value, uint32_t *reqid)
{
	struct xfrm_selector sel;
	struct policy_rule *pr;
	struct xfrm_mark mark;

	if (!peer || !reqid) {
		POLICY_ERR("Bad parameters on VTI reqid lookup\n");
		return -1;
	}

	/* The selector for a VTI tunnel is all zeros. */
	memset(&sel, 0, sizeof(sel));
	mark.v = mark_value;
	mark.m = 0;

	pr = policy_rule_find_by_selector(vrfid, &sel, &mark, XFRM_POLICY_OUT);
	if (!pr) {
		POLICY_DEBUG("Policy not found for VTI reqid lookup\n");
		return -1;
	}

	if ((family != pr->output_peer_af) ||
	    !xfrm_addr_eq(&pr->output_peer, peer, pr->output_peer_af)) {
		POLICY_ERR("Wrong peer address in VTI reqid lookup\n");
		return -1;
	}

	*reqid = pr->reqid;
	return 1;
}

/*
 * crypto_enqueue_fragment()
 *
 * Callback to en-queue a packet generated by ip_fragment() for encryption.
 */
void crypto_enqueue_fragment(struct ifnet *ifp,
			     struct rte_mbuf *mbuf,
			     void *ctx)
{
	const struct crypto_fragment_ctx *frag_ctx = ctx;

	crypto_enqueue_outbound(mbuf, frag_ctx->orig_family,
				frag_ctx->family, frag_ctx->dst,
				frag_ctx->in_ifp, ifp, frag_ctx->reqid,
				frag_ctx->pmd_dev_id, frag_ctx->spi);
}

/*
 * Do the checks to make sure we will be able to send a packet after
 * encrypting. Return the results so that the caller can take
 * appropriate action depending on the address family.
 */
static void
crypto_policy_handle_packet_outbound_checks(struct rte_mbuf *mbuf,
					    uint32_t tbl_id,
					    struct policy_rule *pr,
					    bool *no_next_hop,
					    bool *bh_or_bc,
					    bool *reject,
					    bool *not_slowpath,
					    struct ifnet **nxt_ifp)
{
	struct vrf *vrf = vrf_get_rcu_fast(VRF_DEFAULT_ID);
	struct next_hop *nxt = NULL;

	/* Currently only support underlay in default vrf */
	if (pr->output_peer_af == AF_INET) {
		nxt = rt_lookup_fast(vrf, (in_addr_t)(pr->output_peer.a4),
				tbl_id, mbuf);
	} else {
		nxt = rt6_lookup_fast(vrf,
				      (struct in6_addr *)(&pr->output_peer.a6),
				      tbl_id, mbuf);
	}

	/*
	 * If the IPsec peer's address matches a blackholed or directed
	 * broadcast route, the encrypted packet would be dropped in
	 * ip_lookup_and_originate so drop it early here.
	 */
	if (!nxt) {
		*no_next_hop = true;
		return;
	}
	if (nxt->flags & (RTF_BLACKHOLE | RTF_BROADCAST)) {
		*bh_or_bc = true;
		return;
	}

	/*
	 * Filter reject routes out now. If we hit this post encryption
	 * we won't be able to send the ICMP error back to the source.
	 */
	if (unlikely(nxt->flags & RTF_REJECT)) {
		*reject = true;
		return;
	}

	/*
	 * If we're able to forward to this destination in the
	 * dataplane, check whether we need to fragment the
	 * packet before encryption. For other destinations
	 * we allow the packet to be fragmented post encryption.
	 */
	*nxt_ifp = dp_nh_get_ifp(nxt);
	if (!*nxt_ifp)
		return;

	if (!(nxt->flags & RTF_SLOWPATH)) {
		*not_slowpath = true;
		return;
	}
}

/*
 * crypto_policy_handle_packet_outbound()
 *
 * Handle a packet that has matched the rldb rule for an IPsec output policy.
 *
 * This function always consumes the packet, either dropping it on an error
 * or queuing it to the crypto thread for encryption.
 */
static void
crypto_policy_handle_packet_outbound(struct ifnet *vfp_ifp,
				     struct ifnet *in_ifp,
				     struct rte_mbuf *mbuf,
				     uint32_t tbl_id,
				     struct policy_rule *pr)
{
	struct ifnet *nxt_ifp = NULL;
	bool no_next_hop = false;
	bool blackhole_or_broadcast = false;
	bool reject = false;
	bool not_slowpath = false;
	struct ifnet *icmp_ifp = in_ifp;

	if (in_ifp == get_lo_ifp(CONT_SRC_MAIN) && vfp_ifp)
		icmp_ifp = vfp_ifp;

	/*
	 * Lookup the egress interface for the encrypted packet
	 * and pre-fragment the original packet if necessary.
	 */
	crypto_policy_handle_packet_outbound_checks(mbuf, tbl_id,
						    pr, &no_next_hop,
						    &blackhole_or_broadcast,
						    &reject, &not_slowpath,
						    &nxt_ifp);

	if (unlikely(no_next_hop)) {
		IPSTAT_INC_VRF(if_vrf(in_ifp), IPSTATS_MIB_INNOROUTES);
		icmp_error(icmp_ifp, mbuf,
			   ICMP_DEST_UNREACH, ICMP_NET_UNREACH, 0);
		IPSEC_CNT_INC(DROPPED_NO_NEXT_HOP);
		goto drop;
	}

	if (blackhole_or_broadcast) {
		IPSEC_CNT_INC(DROPPED_BLACKHOLE_OR_BROADCAST);
		goto drop;
	}

	if (reject) {
		icmp_error(icmp_ifp, mbuf,
			   ICMP_DEST_UNREACH, ICMP_HOST_UNREACH, 0);
		IPSEC_CNT_INC(DROPPED_FILTER_REJECT);
		goto drop;
	}

	if (likely(nxt_ifp && not_slowpath)) {
		const struct iphdr *ip = iphdr(mbuf);
		unsigned int ip_len =
			esp_payload_padded_len(&pr->overhead,
					       ntohs(ip->tot_len));
		unsigned int effective_mtu;

		if (pr->overhead.bytes >= nxt_ifp->if_mtu) {
			IPSEC_CNT_INC(DROPPED_OVERHEAD_TOO_BIG);
			goto drop;
		}

		effective_mtu = nxt_ifp->if_mtu - pr->overhead.bytes;
		if (unlikely((effective_mtu < ip_len))) {
			struct crypto_fragment_ctx frag_ctx;

			/*
			 * For efficient padding lets lower the effective MTU
			 * to be a multiple of block_size - 2.
			 */
			effective_mtu =
				RTE_ALIGN_FLOOR(effective_mtu + 2,
						pr->overhead.block_size) - 2;

			/* In future catch at vfp check for NAT scenario */
			if (ip->frag_off & htons(IP_DF)) {
				IPSTAT_INC_MBUF(mbuf, IPSTATS_MIB_FRAGFAILS);
				icmp_error(icmp_ifp, mbuf,
					   ICMP_DEST_UNREACH,
					   ICMP_FRAG_NEEDED,
					   htons(effective_mtu));
				IPSEC_CNT_INC(DROPPED_DF);
				goto drop;
			}

			/* Frag code needs l3_len set to avoid
			 * malformed fragments
			 */
			mbuf->l3_len = ip->ihl << 2;
			frag_ctx.orig_family = AF_INET;
			frag_ctx.family = pr->output_peer_af;
			frag_ctx.dst = &pr->output_peer;
			frag_ctx.in_ifp = in_ifp;
			frag_ctx.reqid = pr->reqid;
			frag_ctx.pmd_dev_id = pr->overhead.pmd_dev_id;
			frag_ctx.spi = pr->overhead.spi;
			ip_fragment_mtu(nxt_ifp, effective_mtu,
					mbuf, &frag_ctx,
					crypto_enqueue_fragment);
			return;
		}
	}

	crypto_enqueue_outbound(mbuf, AF_INET, pr->output_peer_af,
				&pr->output_peer, in_ifp, NULL,
				pr->reqid, pr->overhead.pmd_dev_id,
				pr->overhead.spi);
	return;

drop:
	rte_pktmbuf_free(mbuf);
}

/*
 * crypto_policy_handle_packet6_outbound()
 *
 * Handle a packet that has matched the rldb rule for an IPsec output policy.
 *
 * This function always consumes the packet, either dropping it on an error
 * or queuing it to the crypto thread for encryption.
 */
static void
crypto_policy_handle_packet6_outbound(struct ifnet *vfp_ifp,
				      struct ifnet *in_ifp,
				      struct rte_mbuf *mbuf,
				      uint32_t tbl_id,
				      struct policy_rule *pr)
{
	struct ifnet *nxt_ifp = NULL;
	bool no_next_hop = false;
	bool blackhole_or_broadcast = false;
	bool reject = false;
	bool not_slowpath = false;
	struct ifnet *icmp_ifp = in_ifp;

	if (in_ifp == get_lo_ifp(CONT_SRC_MAIN) && vfp_ifp)
		icmp_ifp = vfp_ifp;

	crypto_policy_handle_packet_outbound_checks(mbuf, tbl_id,
						    pr, &no_next_hop,
						    &blackhole_or_broadcast,
						    &reject, &not_slowpath,
						    &nxt_ifp);

	if (unlikely(no_next_hop)) {
		IP6STAT_INC_VRF(if_vrf(in_ifp), IPSTATS_MIB_INNOROUTES);
		icmp6_error(icmp_ifp, mbuf,
			    ICMP6_DST_UNREACH, ICMP6_DST_UNREACH_NOROUTE, 0);
		IPSEC_CNT_INC(DROPPED_NO_NEXT_HOP);
		return;
	}

	if (blackhole_or_broadcast) {
		IPSEC_CNT_INC(DROPPED_BLACKHOLE_OR_BROADCAST);
		goto drop;
	}

	if (reject) {
		icmp6_error(icmp_ifp, mbuf,
			    ICMP6_DST_UNREACH, ICMP6_DST_UNREACH_ADDR, 0);
		IPSEC_CNT_INC(DROPPED_FILTER_REJECT);
		return;
	}

	if (likely(nxt_ifp && not_slowpath)) {
		const struct ip6_hdr *ip6 = ip6hdr(mbuf);
		unsigned int ip6_len =
			esp_payload_padded_len(&pr->overhead,
					       ntohs(ip6->ip6_plen) +
					       sizeof(*ip6));
		unsigned int effective_mtu;

		if (pr->overhead.bytes >= nxt_ifp->if_mtu) {
			IPSEC_CNT_INC(DROPPED_OVERHEAD_TOO_BIG);
			goto drop;
		}

		/*
		 * MTU check.
		 */
		effective_mtu = nxt_ifp->if_mtu - pr->overhead.bytes;
		if (unlikely((effective_mtu < ip6_len))) {
			struct crypto_fragment_ctx frag_ctx;

			/*
			 * For efficient padding lets lower the effective MTU
			 * to be a multiple of block_size - 2.
			 */
			effective_mtu =
				RTE_ALIGN_FLOOR(effective_mtu + 2,
						pr->overhead.block_size) - 2;
			/*
			 * Don't fragment transit packets
			 */
			if (in_ifp != get_lo_ifp(CONT_SRC_MAIN)) {
				IPSTAT_INC_MBUF(mbuf, IPSTATS_MIB_FRAGFAILS);
				icmp6_error(icmp_ifp, mbuf,
					    ICMP6_PACKET_TOO_BIG, 0,
					    htonl(effective_mtu));
				IPSEC_CNT_INC(DROPPED_DF);
				return;
			}
			frag_ctx.orig_family = AF_INET6;
			frag_ctx.family = pr->output_peer_af;
			frag_ctx.dst = &pr->output_peer;
			frag_ctx.in_ifp = in_ifp;
			frag_ctx.reqid = pr->reqid;
			frag_ctx.pmd_dev_id = pr->overhead.pmd_dev_id;
			frag_ctx.spi = pr->overhead.spi;

			ip6_fragment_mtu(nxt_ifp, effective_mtu, mbuf,
					 &frag_ctx, crypto_enqueue_fragment);
			return;
		}
	}

	crypto_enqueue_outbound(mbuf, AF_INET6, pr->output_peer_af,
				&pr->output_peer, in_ifp, NULL,
				pr->reqid, pr->overhead.pmd_dev_id,
				pr->overhead.spi);
	return;

drop:
	rte_pktmbuf_free(mbuf);
}

#define PREFIX_STRLEN (INET6_ADDRSTRLEN + sizeof("/128"))

static const char *prefix_str(uint16_t family, const xfrm_address_t *addr,
			      int prefix_len, char *buf, size_t blen)
{
	char addrbuf[INET6_ADDRSTRLEN];
	const char *addrstr;
	uint32_t count;

	switch (family) {
	case AF_INET:
		addrstr = inet_ntop(family, &addr->a4, addrbuf,
				    sizeof(addrbuf));
		break;
	case AF_INET6:
		addrstr = inet_ntop(family, &addr->a6, addrbuf,
				    sizeof(addrbuf));
		break;
	default:
		addrstr = NULL;
	}

	count = snprintf(buf, blen, "%s", addrstr ?: "[bad address]");
	if (prefix_len >= 0)
		snprintf(buf + count, blen - count, "/%d", prefix_len);

	return buf;
}

static void policy_selector_to_json(json_writer_t *wr,
				    const struct xfrm_selector *sel,
				    bool show_ifi)
{
	char prefix_buf[PREFIX_STRLEN];

	if (sel->family == AF_INET)
		jsonw_string_field(wr, "af", "IPv4");
	else
		jsonw_string_field(wr, "af", "IPv6");

	jsonw_string_field(wr, "dst", prefix_str(sel->family, &sel->daddr,
						 sel->prefixlen_d,
						 prefix_buf,
						 sizeof(prefix_buf)));
	jsonw_string_field(wr, "src", prefix_str(sel->family, &sel->saddr,
						 sel->prefixlen_s,
						 prefix_buf,
						 sizeof(prefix_buf)));
	jsonw_uint_field(wr, "dport", sel->dport);
	jsonw_uint_field(wr, "sport", sel->sport);
	jsonw_uint_field(wr, "proto", sel->proto);
	if (show_ifi)
		jsonw_uint_field(wr, "ifindex", sel->ifindex);
}

static void policy_rule_to_json(json_writer_t *wr,
				const struct policy_rule *pr)
{
	char spi_as_hexstring[SPI_LEN_IN_HEXCHARS];
	char prefix_buf[PREFIX_STRLEN];
	struct ifnet *ifp;

	jsonw_start_object(wr);

	policy_selector_to_json(wr, &pr->sel, true);

	if (pr->dir == XFRM_POLICY_OUT) {
		jsonw_string_field(wr, "direction", "encryption-out");
		if (pr->action == XFRM_POLICY_ALLOW)
			jsonw_string_field(wr, "action", "allow");
		else
			jsonw_string_field(wr, "action", "block");

	} else {
		jsonw_string_field(wr, "direction", "in-rp-check");
		jsonw_string_field(wr, "action", "allow-decrypted-traffic");
	}

	jsonw_uint_field(wr, "priority", pr->policy_priority);

	if (pr->dir == XFRM_POLICY_OUT) {
		if (pr->action == XFRM_POLICY_ALLOW)
			jsonw_string_field(wr, "peer",
					   prefix_str(pr->output_peer_af,
						      &pr->output_peer,
						      -1, prefix_buf,
						      sizeof(prefix_buf)));
		else
			jsonw_string_field(wr, "peer", "blocked");
	} else {
		jsonw_string_field(wr, "peer", "local");
	}
	jsonw_uint_field(wr, "reqid", pr->reqid);

	spi_to_hexstr(spi_as_hexstring, pr->overhead.spi);
	jsonw_string_field(wr, "spi", spi_as_hexstring);

	jsonw_uint_field(wr, "pmd dev id", pr->overhead.pmd_dev_id);
	jsonw_bool_field(wr, "vti_tunnel", pr->vti_tunnel_policy);
	jsonw_uint_field(wr, "mark_v", pr->mark.v);
	jsonw_uint_field(wr, "mark_m", pr->mark.m);
	jsonw_uint_field(wr, "index", pr->rule_index);

	if (pr->feat_attach) {
		ifp = dp_nh_get_ifp(&pr->feat_attach->nh);
		if (ifp)
			jsonw_string_field(wr, "virtual-feature-point",
					   ifp->if_name);
	}
	jsonw_end_object(wr);
}

void crypto_policy_bind_show_summary(FILE *f, vrfid_t vrfid)
{
	json_writer_t *wr;
	struct cds_lfht_iter iter;
	struct crypto_vrf_ctx *vrf_ctx;
	struct s2s_binding *bind;

	vrf_ctx = crypto_vrf_find_external(vrfid);
	if (!vrf_ctx)
		return;

	wr = jsonw_new(f);
	if (!wr)
		return;

	jsonw_pretty(wr, true);
	jsonw_name(wr, "ipsec_s2s_bindings");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "vrf", vrfid);

	cds_lfht_for_each_entry(vrf_ctx->s2s_bind_hash_table, &iter, bind,
				bind_ht_node) {
		struct ifnet *ifp;

		policy_selector_to_json(wr, &bind->sel, false);
		jsonw_uint_field(wr, "virtual-feature-point_ifi",
				 bind->ifindex);

		ifp = dp_ifnet_byifindex(bind->ifindex);
		if (ifp)
			jsonw_string_field(wr, "virtual-feature-point_name",
					   ifp->if_name);
	}
	jsonw_end_object(wr);
	jsonw_destroy(&wr);
}

void crypto_policy_show_summary(FILE *f, vrfid_t vrfid, bool brief)
{
	json_writer_t *wr;
	struct crypto_vrf_ctx *vrf_ctx;
	const struct policy_rule *pr;
	struct cds_lfht *ht;
	struct cds_lfht_iter iter;

	vrf_ctx = crypto_vrf_find_external(vrfid);

	wr = jsonw_new(f);
	if (!wr)
		return;

	jsonw_pretty(wr, true);

	jsonw_name(wr, "ipsec_policies");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "vrf", vrfid);
	jsonw_name(wr, "policy_statistics");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "rekey_requests", crypto_rekey_requests);
	jsonw_end_object(wr);
	jsonw_name(wr, "total_policy_count");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "ipv4", vrf_ctx ?
			 vrf_ctx->crypto_total_ipv4_policies : 0);
	jsonw_uint_field(wr, "ipv6", vrf_ctx ?
			 vrf_ctx->crypto_total_ipv6_policies : 0);
	jsonw_end_object(wr);
	jsonw_name(wr, "live_policy_count");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "ipv4", vrf_ctx ?
			 vrf_ctx->crypto_live_ipv4_policies : 0);
	jsonw_uint_field(wr, "ipv6", vrf_ctx ?
			 vrf_ctx->crypto_live_ipv6_policies : 0);
	jsonw_end_object(wr);

	if (!brief) {
		jsonw_name(wr, "policies");
		jsonw_start_array(wr);

		ht = vrf_ctx ? vrf_ctx->output_policy_rule_sel_ht : NULL;
		if (ht) {
			cds_lfht_for_each_entry(ht, &iter, pr, sel_ht_node) {
				if (pr->flags & POLICY_F_PENDING_ADD)
					continue;
				if (dp_vrf_get_external_id(pr->vrfid) == vrfid)
					policy_rule_to_json(wr, pr);
			}
		}

		ht = vrf_ctx ? vrf_ctx->input_policy_rule_sel_ht : NULL;
		if (ht) {
			cds_lfht_for_each_entry(ht, &iter, pr, sel_ht_node) {
				if (pr->flags & POLICY_F_PENDING_ADD)
					continue;
				if (dp_vrf_get_external_id(pr->vrfid) == vrfid)
					policy_rule_to_json(wr, pr);
			}
		}
		jsonw_end_array(wr);
	}

	jsonw_end_object(wr);
	jsonw_destroy(&wr);
}

static void
crypto_flow_cache_dump_entry(struct flow_cache_entry *entry,
			     bool detail, json_writer_t *wr)
{
	struct policy_rule *pr = NULL;
	union crypto_ctx ctx;

	if (!detail)
		return;

	flow_cache_entry_get_info(entry,
				  (void **)&pr,
				  &ctx.context);
	if (pr) {
		jsonw_uint_field(wr, "PR_index",
				 pr->rule_index);
		jsonw_uint_field(wr, "PR_Tag", 0);
	}
	jsonw_uint_field(wr, "IN_rule_checked",
			 ctx.in_rule_checked);
	jsonw_uint_field(wr, "IN_rule_drop",
			 ctx.in_rule_drop);
	jsonw_uint_field(wr, "NO_rule_fwd",
			 ctx.no_rule_fwd);
}

void crypto_show_cache(FILE *f, const char *str)
{
	json_writer_t *wr = jsonw_new(f);
	bool detail = (str ? strcmp(str, "detail") == 0 : 0);

	if (!wr)
		return;

	jsonw_pretty(wr, true);
	jsonw_name(wr, "IPsec-Cache");
	flow_cache_dump(flow_cache, wr, detail, crypto_flow_cache_dump_entry);
	jsonw_destroy(&wr);
}

/*
 * crypto_policy_init()
 *
 * Initialise the SADB's hash table and counters.
 */
int crypto_policy_init(void)
{
	rte_timer_init(&crypto_npf_cfg_commit_all_timer);

	return 0;
}

/*
 * Encrypt and output a packet on a s2s virtual feature point interface.
 */
void
crypto_policy_post_features_outbound(struct ifnet *vfp_ifp,
				     struct ifnet *in_ifp,
				     struct rte_mbuf *m,
				     uint16_t proto)
{
	struct pktmbuf_mdata *mdata = pktmbuf_mdata(m);
	struct policy_rule *pr = mdata->pr;

	if (likely(pktmbuf_mdata_exists(m, PKT_MDATA_CRYPTO_PR))) {
		/*
		 * This packet previously matched a policy in
		 * crypto_policy_check_outbound but was returned to have
		 * output features applied. The policy rule was cached in
		 * packet metadata.
		 */
		pktmbuf_mdata_clear(m, PKT_MDATA_CRYPTO_PR);
		if_incr_out(vfp_ifp, m);
		if (proto == ETH_P_IP)
			crypto_policy_handle_packet_outbound(vfp_ifp, in_ifp, m,
							     RT_TABLE_MAIN,
							     pr);
		else
			crypto_policy_handle_packet6_outbound(vfp_ifp, in_ifp,
							      m, RT_TABLE_MAIN,
							      pr);
		return;
	}

	/*
	 * Alternatively, the packet might have been directed to
	 * the virtual feature point, eg. by a PBR rule when
	 * pre-NAT it didn't match the output policy. If we are now
	 * post-NAT, it might match so check again.
	 */
	if (!crypto_policy_check_outbound(in_ifp, &m, RT_TABLE_MAIN,
					  htons(proto), NULL)) {
		POLICY_ERR("Packet on vfp with no policy rule\n");
		rte_pktmbuf_free(m);
		IPSEC_CNT_INC(DROPPED_ON_FP_NO_PR);
		if_incr_dropped(vfp_ifp);
		return;
	}
}

/*
 * Check for a match on an IPsec policy and if one matches then either
 * drop the packet,  or queue it to the crypto thread as appropriate.
 * If a matching policy has a virtual feature point bound to it,
 * then return the packet so output features can be run.
 */
bool crypto_policy_check_outbound(struct ifnet *in_ifp, struct rte_mbuf **mbuf,
				  uint32_t tbl_id, uint16_t eth_type,
				  struct next_hop **nh)
{
	struct policy_rule *pr = NULL;
	struct flow_cache_entry *cache_entry;
	vrfid_t vrfid;
	bool v4 = (eth_type == htons(RTE_ETHER_TYPE_IPV4));
	bool freed = false;
	bool seen_by_crypto;
	int err;
	union crypto_ctx ctx;

	seen_by_crypto = ((*mbuf)->ol_flags & PKT_RX_SEEN_BY_CRYPTO);

	/*
	 * Do we have a cached lookup result for this policy?
	 */
	cache_entry = crypto_flow_cache_lookup(*mbuf, v4);
	if (cache_entry)
		flow_cache_entry_get_info(cache_entry, (void **)&pr,
					  &ctx.context);

	/*
	 * Use the flow cache under following conditions:
	 * - received an encrypted packet
	 * - received an UNencrypted packet and we have cached the input
	 *   policy check result.
	 */
	if (cache_entry) {
		IPSEC_CNT_INC(FLOW_CACHE_HIT);
		if (!pr) {
			/*
			 * cleartext packet found in cache. Forward as-is
			 */
			if (ctx.no_rule_fwd)
				return false;
		}
	} else {
		struct rldb_result result;
		struct crypto_vrf_ctx *vrf_ctx;
		struct rldb_db_handle *db_in, *db_out;
		int dir = XFRM_POLICY_OUT;

		vrfid = pktmbuf_get_vrf(*mbuf);
		vrf_ctx = crypto_vrf_find(vrfid);

		/* no crypto fo this VRF */
		if (!vrf_ctx)
			return false;

		if (v4) {
			db_in = vrf_ctx->input_policy_v4_rldb;
			db_out = vrf_ctx->output_policy_v4_rldb;
		} else {
			db_in = vrf_ctx->input_policy_v6_rldb;
			db_out = vrf_ctx->output_policy_v6_rldb;
		}

		/*
		 * If this packet was received encrypted,  then we don't need to
		 * check the input policy.  Otherwise check the policy to see if
		 * it should have been received encrypted,  and so now needs to
		 * be dropped.
		 */

		if (likely(!seen_by_crypto)) {
			err = rldb_match(db_in, mbuf, 1, &result);
			if (likely(err == -ENOENT))
				;
			else if (err == 0) {

				dir = XFRM_POLICY_IN;
				pr = (struct policy_rule *)
					result.rldb_user_data;
			}
		}

		/*
		 * Packets matching an input policy must be dropped if
		 * they were not encrypted when originally received,
		 * and this routine is only called for such unencrypted
		 * packets.
		 *
		 * If no policy matches we find -ENOENT.
		 * Otherwise one a ALLOW policy or a BLOCK polic.
		 *
		 * Only block rules are currently used in the input policy.
		 */

		if (likely(!pr)) {

			err = rldb_match(db_out, mbuf, 1, &result);
			if (likely(err == -ENOENT))
				;
			else if (err == 0) {

				dir = XFRM_POLICY_OUT;
				pr = (struct policy_rule *)
					result.rldb_user_data;
			}
		}

		/*
		 * No input and no output policy matched,  allow normal
		 * processing
		 */
		if (likely(!pr && err == -ENOENT)) {
			crypto_flow_cache_add(flow_cache, NULL, *mbuf, v4,
					      seen_by_crypto, XFRM_POLICY_OUT);
			return false;
		}

		/*
		 * We found a policy. If it has a selector
		 * with an ifindex set, then check we match.
		 */
		if (pr && pr->sel.ifindex && nh) {
			struct ifnet *ifp = NULL;

			if (v4 && *nh)
				ifp = dp_nh_get_ifp(*nh);
			else if (*nh)
				ifp = dp_nh_get_ifp(*nh);

			if (!ifp || pr->sel.ifindex != (int)ifp->if_index)
				/* We don't have a match */
				return false;
		}

		crypto_flow_cache_add(flow_cache, pr, *mbuf, v4,
				      seen_by_crypto, dir);
	}

	if (pr && !(pr->flags & POLICY_F_PENDING_DEL)) {
		if (pr->action != XFRM_POLICY_BLOCK) {
			struct pr_feat_attach *attach;
			struct pktmbuf_mdata *mdata;

			/*
			 * If the policy has a virtual feature point
			 * applied, cache the policy rule and return the
			 * packet to the caller to run output features with
			 * a next hop pointing to the feature point.
			 *
			 * Transiently, the binding might be ahead of the
			 * virtual feature point interface,
			 * drop in the meantime.
			 */
			attach = rcu_dereference(pr->feat_attach);
			struct ifnet *vfp_ifp = NULL;

			if (attach) {
				vfp_ifp = dp_nh_get_ifp(&attach->nh);

				if (!vfp_ifp) {
					IPSEC_CNT_INC(DROPPED_NO_BIND);
					goto drop;
				}

				if (nh) {
					*nh = &attach->nh;
					mdata = pktmbuf_mdata(*mbuf);
					mdata->pr = pr;
					pktmbuf_mdata_set(*mbuf,
							  PKT_MDATA_CRYPTO_PR);
					return false;
				}
				if_incr_out(vfp_ifp, *mbuf);
			}

			if (v4)
				crypto_policy_handle_packet_outbound(vfp_ifp,
								     in_ifp,
								     *mbuf,
								     tbl_id,
								     pr);
			else
				crypto_policy_handle_packet6_outbound(vfp_ifp,
								      in_ifp,
								      *mbuf,
								      tbl_id,
								      pr);
			return true;
		}
	} else {
		IPSEC_CNT_INC(DROPPED_NO_POLICY_RULE);
	}
	/* An Input or Output policy blocked this */
drop:
	if (in_ifp) {
		if (v4) {
			IPSTAT_INC_IFP(in_ifp, IPSTATS_MIB_INNOROUTES);
			icmp_error(in_ifp, *mbuf, ICMP_DEST_UNREACH,
				   ICMP_NET_UNREACH, 0);
		} else {
			IP6STAT_INC_IFP(in_ifp, IPSTATS_MIB_INNOROUTES);
			icmp6_error(in_ifp, *mbuf, ICMP6_DST_UNREACH,
				    ICMP6_DST_UNREACH_ADMIN, 0);
			freed = true;
		}
	}

	IPSEC_CNT_INC(DROPPED_POLICY_BLOCK);
	if (!freed)
		rte_pktmbuf_free(*mbuf);
	return true;
}

/*
 * Check for a match on an IPsec input policy. If one matches and the mbuf
 * was not already decrypted then drop the packet. If the mbuf has already
 * been decrypted then there is no need to check the policy as we know it
 * has already matched.
 *
 * Return false if already decrypted or does not match a policy.
 * Return true if dropped by this func.
 */
static bool
crypto_policy_check_inbound(struct ifnet *in_ifp, struct rte_mbuf **mbuf,
			    uint16_t eth_type)
{
	struct policy_rule *pr = NULL;
	struct flow_cache_entry *cache_entry;
	bool v4 = (eth_type == htons(RTE_ETHER_TYPE_IPV4));
	bool freed = false;
	vrfid_t vrfid;
	union crypto_ctx ctx;
	struct crypto_vrf_ctx *vrf_ctx;

	if ((*mbuf)->ol_flags & PKT_RX_SEEN_BY_CRYPTO)
		return false;

	/*
	 * Use the flow cache only if we have already cached the input check.
	 */
	cache_entry = crypto_flow_cache_lookup(*mbuf, v4);
	if (cache_entry)
		flow_cache_entry_get_info(cache_entry, (void **)&pr,
					  &ctx.context);

	if (cache_entry && pr && ctx.in_rule_checked) {
		IPSEC_CNT_INC(FLOW_CACHE_HIT);
		if (pr->action == XFRM_POLICY_BLOCK)
			goto drop;

	} else {
		struct rldb_result result;
		struct rldb_db_handle *db;
		int err;

		vrfid = pktmbuf_get_vrf(*mbuf);
		vrf_ctx = crypto_vrf_find(vrfid);

		/* no crypto fo this VRF */
		if (!vrf_ctx)
			return false;

		/*
		 * Packets matching an input policy must be dropped if
		 * they were not encrypted when originally received,
		 * and this routine is only called for such unencrypted
		 * packets.
		 *
		 * If no policy matches we find -ENOENT.
		 * Otherwise one a ALLOW policy or a BLOCK policy.
		 *
		 * Only block rules are currently used in the input policy.
		 */

		db = v4 ? vrf_ctx->input_policy_v4_rldb
		       : vrf_ctx->input_policy_v6_rldb;

		err = rldb_match(db, mbuf, 1, &result);
		if (err)
			return false;

		pr = (struct policy_rule *)result.rldb_user_data;

		if (pr) {
			/*
			 * We found an input policy. If it has a
			 * selector with an ifindex set, then
			 * check we match.
			 */
			if (pr->sel.ifindex) {
				if (pr->sel.ifindex !=
				    (int)in_ifp->if_index) {
					/* We don't have a match */
					return false;
				}
			}

			/*
			 * We found an input policy, add it to the
			 * flow cache and drop the packet.
			 */
			crypto_flow_cache_add(flow_cache, pr, *mbuf, v4,
					      false, XFRM_POLICY_IN);

			if (pr->action == XFRM_POLICY_BLOCK)
				goto drop;
		}
	}
	return false;

drop:
	if (in_ifp) {
		if (v4) {
			icmp_error(in_ifp, *mbuf, ICMP_DEST_UNREACH,
				   ICMP_NET_UNREACH, 0);
			IPSTAT_INC_IFP(in_ifp, IPSTATS_MIB_INNOROUTES);
		} else {
			icmp6_error(in_ifp, *mbuf, ICMP6_DST_UNREACH,
				    ICMP6_DST_UNREACH_ADMIN, 0);
			freed = true;
			IP6STAT_INC_IFP(in_ifp, IPSTATS_MIB_INNOROUTES);
		}
	}

	IPSEC_CNT_INC(DROPPED_POLICY_BLOCK);
	if (!freed)
		rte_pktmbuf_free(*mbuf);
	return true;
}

bool crypto_policy_check_inbound_terminating(struct ifnet *in_ifp,
					     struct rte_mbuf **mbuf,
					     uint16_t eth_type)
{
	uint8_t proto;

	if (eth_type == htons(RTE_ETHER_TYPE_IPV4)) {
		struct iphdr *ip = iphdr(*mbuf);

		proto = ip->protocol;
	} else {
		struct ip6_hdr *ip6 = ip6hdr(*mbuf);

		proto = ip6->ip6_nxt;
	}

	if (proto == IPPROTO_UDP) {
		struct udphdr *udp = dp_pktmbuf_mtol4(*mbuf, struct udphdr *);

		if (udp->uh_dport == htons(IKE_PORT))
			return false;
	}

	return (crypto_policy_check_inbound(in_ifp, mbuf, eth_type));
}

/*
 * For a given reqid, find the matching output policy and retrieve the
 * virtual feature point interface, if any.
 */
struct ifnet *crypto_policy_feat_attach_by_reqid(struct crypto_vrf_ctx *vrf_ctx,
						 uint32_t reqid)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct cds_lfht *ht;

	ht = vrf_ctx->output_policy_rule_sel_ht;

	cds_lfht_first(ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct policy_rule *pr;

		pr = caa_container_of(node, struct policy_rule, sel_ht_node);

		if (pr->reqid == reqid)
			return pr->feat_attach ?
				dp_nh_get_ifp(&pr->feat_attach->nh) : NULL;
		cds_lfht_next(ht, &iter);
	}

	return NULL;
}

/* Do we have any bindings already matching this newly created interface? */
void crypto_policy_update_pending_if(struct ifnet *ifp)
{
	vrfid_t vrfid = if_vrfid(ifp);
	struct cds_lfht *bind_table;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	bind_table = bind_table_vrf_get(vrfid);

	if (!bind_table) {
		POLICY_ERR("Failed to get binding table for if walk\n");
		return;
	}

	cds_lfht_first(bind_table, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct s2s_binding *bind;

		bind = caa_container_of(node, struct s2s_binding, bind_ht_node);

		if (ifp->if_index == bind->ifindex) {
			policy_bind_feat_attach(vrfid,
						&bind->sel,
						bind->ifindex);
		}
		cds_lfht_next(bind_table, &iter);
	}
}

static int policy_feat_detach_internal(vrfid_t vrfid,
				       const struct xfrm_selector *sel,
				       struct s2s_binding *bind)
{
	struct policy_rule *pr;
	struct xfrm_mark mark;

	if (bind)
		policy_bind_del(bind);

	mark.v = mark.m = 0;
	pr = policy_rule_find_by_selector(vrfid, sel, &mark, XFRM_POLICY_OUT);

	if (pr) {
		crypto_sadb_feat_attach_in(pr->reqid, NULL);
		policy_feat_attach_destroy(pr);
	}
	return 0;
}

/* Unbind a policy and virtual feature point. */
static int policy_feat_detach(vrfid_t vrfid, const struct xfrm_selector *sel,
			      uint ifindex __unused)
{
	struct s2s_binding *bind;

	bind = policy_bind_lookup(vrfid, sel);

	return policy_feat_detach_internal(vrfid, sel, bind);
}

void policy_feat_flush_vrf(struct crypto_vrf_ctx *vrf_ctx)
{
	struct s2s_binding *bind;
	struct cds_lfht_iter iter;

	POLICY_DEBUG("Flush all feature bindings for VRF %d\n",
		     vrf_ctx->vrfid);

	cds_lfht_for_each_entry(vrf_ctx->s2s_bind_hash_table,
				&iter, bind, bind_ht_node) {
		policy_feat_detach_internal(vrf_ctx->vrfid, &bind->sel, bind);
	}
}

/* Bind a policy and virtual feature point. */
static int policy_feat_attach(vrfid_t vrfid, const struct xfrm_selector *sel,
			      uint ifindex)
{
	struct s2s_binding *bind;
	struct cds_lfht_node *node;
	struct cds_lfht *bind_table;

	bind = malloc(sizeof(*bind));

	if (!bind) {
		POLICY_ERR("Failed to create policy bind\n");
		return -ENOMEM;
	}

	cds_lfht_node_init(&bind->bind_ht_node);

	bind_table = bind_table_vrf_get(vrfid);

	if (!bind_table) {
		POLICY_ERR("Failed to get binding table for add\n");
		free(bind);
		return -ENOENT;
	}

	node = cds_lfht_add_unique(bind_table,
				   policy_bind_sel_hash(sel),
				   policy_bind_sel_match,
				   sel, &bind->bind_ht_node);
	if (node != &bind->bind_ht_node) {
		/* existing binding, use it instead of the created one */
		free(bind);
		bind = caa_container_of(node, struct s2s_binding, bind_ht_node);
	} else
		bind_table_vrf_inc(vrfid);

	bind->sel = *sel;
	bind->ifindex = ifindex;
	bind->vrfid = vrfid;

	policy_bind_feat_attach(vrfid, sel, ifindex);

	return 0;
}

/*
 * Attach/detach an IPSec site to site policy to/from a virtual feature point
 * interface used as a feature hook.
 *
 * s2s attach
 *    <ifindex> <vrf> <dst> <dlen> <src> <slen> <dport> <sport> <prot> [sel if]
 * s2s detach
 *    <ifindex> <vrf> <dst> <dlen> <src> <slen> <dport> <sport> <prot> [sel if]
 *
 * The [sel if] is the ifindex in the selector, if set.  If not set then this
 * value will be 0. If it is the ifindex of a vrf, then we will use 0
 * instead (like we do when creating policies). If this arg does not exist then
 * we will assume it is 0.
 */
static int crypto_policy_cmd_handler(struct pb_msg *msg)
{
	struct xfrm_selector sel;
	CryptoPolicyConfig *cp_msg;
	int rc = 0;
	vrfid_t vrf_id;
	struct ifnet *ifp;
	struct vrf *vrf;

	cp_msg = crypto_policy_config__unpack(NULL, msg->msg_len, msg->msg);
	if (!cp_msg) {
		RTE_LOG(ERR, DATAPLANE,
			"failed to read crypto policy protobuf command\n");
		return -1;
	}

	if (cp_msg->vrf < VRF_DEFAULT_ID) {
		rc = -1;
		goto done;
	}

	vrf_id = cp_msg->vrf;
	vrf = dp_vrf_get_rcu_from_external(vrf_id);
	if (vrf)
		vrf_id = vrf->v_id;

	memset(&sel, 0, sizeof(sel));

	struct ip_addr daddr, saddr;
	if (dp_protobuf_get_ipaddr(cp_msg->sel_daddr, &daddr)) {
		rc = -1;
		goto done;
	}
	if (dp_protobuf_get_ipaddr(cp_msg->sel_saddr, &saddr)) {
		rc = -1;
		goto done;
	}

	sel.family = daddr.type;

	if (sel.family == AF_INET)
		memcpy(&sel.daddr.a4,
		       &daddr.address.ip_v4,
		       sizeof(sel.daddr.a4));
	else
		memcpy(&sel.daddr.a6,
		       &daddr.address.ip_v6,
		       sizeof(sel.daddr.a6));

	if (sel.family == AF_INET)
		memcpy(&sel.saddr.a4,
		       &saddr.address.ip_v4,
		       sizeof(sel.saddr.a4));
	else
		memcpy(&sel.saddr.a6,
		       &saddr.address.ip_v6,
		       sizeof(sel.saddr.a6));

	sel.prefixlen_d = cp_msg->sel_dprefix_len;
	if (sel.prefixlen_d > (sel.family == AF_INET ? 32 : 128)) {
		rc = -1;
		goto done;
	}

	sel.prefixlen_s = cp_msg->sel_sprefix_len;
	if (sel.prefixlen_s > (sel.family == AF_INET ? 32 : 128)) {
		rc = -1;
		goto done;
	}

	if (cp_msg->sel_dport > USHRT_MAX) {
		rc = -1;
		goto done;
	}
	sel.dport = cp_msg->sel_dport;

	if (cp_msg->sel_sport > USHRT_MAX) {
		rc = -1;
		goto done;
	}
	sel.sport = cp_msg->sel_sport;

	if (cp_msg->sel_proto > USHRT_MAX) {
		rc = -1;
		goto done;
	}
	sel.proto = cp_msg->sel_proto;

	if (cp_msg->has_sel_ifindex) {
		sel.ifindex = cp_msg->sel_ifindex;
		ifp = dp_ifnet_byifindex(sel.ifindex);
		if (ifp && ifp->if_type == IFT_VRF)
			sel.ifindex = 0;
	} else
		sel.ifindex = 0;

	if (cp_msg->action == CRYPTO_POLICY_CONFIG__ACTION__ATTACH)
		rc = policy_feat_attach(vrf_id, &sel, cp_msg->ifindex);
	else
		rc = policy_feat_detach(vrf_id, &sel, cp_msg->ifindex);

done:
	crypto_policy_config__free_unpacked(cp_msg, NULL);
	return rc;
}

struct crypto_incmpl_xfrm_pol_stats {
	uint64_t pol_add;
	uint64_t pol_update;
	uint64_t pol_del;
	uint64_t pol_missing;
	uint64_t if_complete;
	uint64_t mem_fails;
};

#define CRYPTO_INCMPL_XFRM_HASH_MIN 2
#define CRYPTO_INCMPL_XFRM_HASH_MAX 64

struct cds_lfht *crypto_incmpl_policy;
static struct crypto_incmpl_xfrm_pol_stats crypto_incmpl_xfrm_pol_stats;

struct crypto_incmpl_xfrm_policy {
	struct cds_lfht_node hash_node;
	struct rcu_head rcu;

	/* keys */
	struct xfrm_selector sel;
	struct xfrm_mark mark;
	/* Key here holds pointers to the sel and mark */
	struct policy_rule_key key;

	/* netlink message */
	struct nlmsghdr *nlh;
};


void crypto_incmpl_policy_init(void)
{
	crypto_incmpl_policy = cds_lfht_new(CRYPTO_INCMPL_XFRM_HASH_MIN,
					    CRYPTO_INCMPL_XFRM_HASH_MAX,
					    CRYPTO_INCMPL_XFRM_HASH_MAX,
					    CDS_LFHT_AUTO_RESIZE |
					    CDS_LFHT_ACCOUNTING,
					    NULL);
	if (!crypto_incmpl_policy)
		rte_panic("Can't allocate hash for incomplete xfrm policies\n");
}

static int crypto_incmpl_pol_match_fn(struct cds_lfht_node *node,
				      const void *key)
{
	const struct crypto_incmpl_xfrm_policy *pol;
	const struct crypto_incmpl_xfrm_policy *search_key = key;

	pol = caa_container_of(node,
			       const struct crypto_incmpl_xfrm_policy,
			       hash_node);

	return (policy_rule_sel_eq(pol->key.sel, search_key->key.sel) &&
		((search_key->key.mark &&
		  (pol->mark.v == search_key->key.mark->v)) ||
		 (!search_key->key.mark && (pol->mark.v == 0))));
}

static void
crypto_incmpl_xfrm_pol_free(struct rcu_head *head)
{
	struct crypto_incmpl_xfrm_policy *pol;

	pol = caa_container_of(head, struct crypto_incmpl_xfrm_policy, rcu);
	free(pol->nlh);
	free(pol);
}

/*
 * Add an incomplete policy (waiting on the vrf). If we already have
 * an entry for the key (selector + mark) then update the message.
 */
void crypto_incmpl_xfrm_policy_add(uint32_t ifindex __unused,
				   const struct nlmsghdr *nlh,
				   const struct xfrm_selector *sel,
				   const struct xfrm_mark *mark)
{
	struct crypto_incmpl_xfrm_policy *pol;
	struct cds_lfht_node *ret_node;

	pol = calloc(1, sizeof(*pol));
	if (!pol) {
		crypto_incmpl_xfrm_pol_stats.mem_fails++;
		return;
	}
	pol->sel = *sel;
	pol->key.sel = &pol->sel;
	if (mark) {
		pol->mark = *mark;
		pol->key.mark = &pol->mark;
	}

	pol->nlh = malloc(nlh->nlmsg_len);
	if (!pol->nlh) {
		free(pol);
		crypto_incmpl_xfrm_pol_stats.mem_fails++;
		return;
	}
	memcpy(pol->nlh, nlh, nlh->nlmsg_len);

	ret_node = cds_lfht_add_replace(crypto_incmpl_policy,
					policy_rule_sel_hash(&pol->key),
					crypto_incmpl_pol_match_fn,
					pol,
					&pol->hash_node);
	if (ret_node == NULL) {
		/* added, but was no old entry */
		crypto_incmpl_xfrm_pol_stats.pol_add++;
	} else if (ret_node != &pol->hash_node) {
		/* replaced, so free old one */
		crypto_incmpl_xfrm_pol_stats.pol_update++;
		pol = caa_container_of(ret_node,
				       struct crypto_incmpl_xfrm_policy,
				       hash_node);
		call_rcu(&pol->rcu, crypto_incmpl_xfrm_pol_free);
	}
}

void crypto_incmpl_xfrm_policy_del(uint32_t ifindex __unused,
				   const struct nlmsghdr *nlh __unused,
				   const struct xfrm_selector *sel,
				   const struct xfrm_mark *mark)

{
	struct crypto_incmpl_xfrm_policy pol;
	struct crypto_incmpl_xfrm_policy *found;
	struct policy_rule_key key;
	struct cds_lfht_node *node;
	struct cds_lfht_iter iter;

	memset(&pol, 0, sizeof(pol));

	key.sel = sel;
	key.mark = mark;

	pol.sel = *sel;
	pol.key.sel = &pol.sel;
	if (mark) {
		pol.mark = *mark;
		pol.key.mark = &pol.mark;
	}

	cds_lfht_lookup(crypto_incmpl_policy,
			policy_rule_sel_hash(&key),
			crypto_incmpl_pol_match_fn,
			&pol,
			&iter);

	node = cds_lfht_iter_get_node(&iter);
	if (!node) {
		crypto_incmpl_xfrm_pol_stats.pol_missing++;
		return;
	}
	cds_lfht_del(crypto_incmpl_policy, node);
	found = caa_container_of(node, struct crypto_incmpl_xfrm_policy,
				 hash_node);
	call_rcu(&found->rcu, crypto_incmpl_xfrm_pol_free);
	crypto_incmpl_xfrm_pol_stats.pol_del++;
}

void crypto_incmpl_policy_make_complete(void)
{
	struct cds_lfht_iter iter;
	struct crypto_incmpl_xfrm_policy *pol;
	struct xfrm_client_aux_data aux;

	vrfid_t vrf_id = VRF_DEFAULT_ID;
	aux.vrf = &vrf_id;

	crypto_incmpl_xfrm_pol_stats.if_complete++;

	cds_lfht_for_each_entry(crypto_incmpl_policy, &iter,
				pol, hash_node) {
		rtnl_process_xfrm(pol->nlh, &aux);
	}
	crypto_npf_cfg_commit_flush();
}

PB_REGISTER_CMD(crypto_policy_cmd) = {
	.cmd = "vyatta:crypto-policy",
	.handler = crypto_policy_cmd_handler,
};
