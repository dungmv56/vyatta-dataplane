/*-
 * Copyright (c) 2020-2021, AT&T Intellectual Property.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Generalised Packet Classification (GPF) op-mode command handling
 */

#include <errno.h>
#include <stdio.h>
#include <urcu/list.h>
#include <vplane_log.h>
#include <vplane_debug.h>

#include "commands.h"
#include "include/fal_plugin.h"
#include "fal.h"
#include "gpc_pb.h"
#include "gpc_util.h"
#include "json_writer.h"
#include "npf/config/gpc_db_query.h"
#include "urcu.h"
#include "util.h"

#define PREFIX_STRLEN (INET6_ADDRSTRLEN + sizeof("/128"))

/*
 * Structure definitions
 */
struct gpc_show_context {
	json_writer_t *wr;
};

static int
gpc_ip_prefix_str(struct ip_prefix *ip_prefix, char *outstr, size_t outstr_len)
{
	char buf[INET6_ADDRSTRLEN];

	if (inet_ntop(ip_prefix->addr.type, &ip_prefix->addr.address, buf,
		      sizeof(buf)) == NULL) {
		RTE_LOG(ERR, GPC, "inet_ntop error: %d, af: %u\n",
			errno, ip_prefix->addr.type);
	} else {
		snprintf(outstr, outstr_len, "%s/%d", &buf[0],
			 ip_prefix->prefix_length);
		return 0;
	}
	return 1;
}

static gpc_pb_rule_match_walker_cb gpc_op_show_match;
static bool
gpc_op_show_match(struct gpc_pb_match *match, struct gpc_walk_context *walk_ctx)
{
	struct gpc_show_context *show_ctx =
		(struct gpc_show_context *)walk_ctx->data;
	json_writer_t *wr = show_ctx->wr;
	char prefix_str[PREFIX_STRLEN];

	jsonw_start_object(wr);
	switch (match->match_type) {
	case GPC_RULE_MATCH_VALUE_NOT_SET:
		break;
	case GPC_RULE_MATCH_VALUE_SRC_IP:
		if (!gpc_ip_prefix_str(&match->match_value.src_ip,
				       prefix_str, sizeof(prefix_str)))
			jsonw_string_field(wr, "src-ip", prefix_str);
		break;
	case GPC_RULE_MATCH_VALUE_DEST_IP:
		if (!gpc_ip_prefix_str(&match->match_value.dest_ip,
				       prefix_str, sizeof(prefix_str)))
			jsonw_string_field(wr, "dest-ip", prefix_str);
		break;
	case GPC_RULE_MATCH_VALUE_SRC_PORT:
		jsonw_uint_field(wr, "src-port", match->match_value.src_port);
		break;
	case GPC_RULE_MATCH_VALUE_DEST_PORT:
		jsonw_uint_field(wr, "dest-port", match->match_value.dest_port);
		break;
	case GPC_RULE_MATCH_VALUE_FRAGMENT:
		jsonw_uint_field(wr, "fragment", match->match_value.fragment);
		break;
	case GPC_RULE_MATCH_VALUE_DSCP:
		jsonw_uint_field(wr, "dscp", match->match_value.dscp);
		break;
	case GPC_RULE_MATCH_VALUE_TTL:
		jsonw_uint_field(wr, "ttl", match->match_value.ttl);
		break;
	case GPC_RULE_MATCH_VALUE_ICMPV4:
		jsonw_uint_field(wr, "icmp-type",
				 match->match_value.icmpv4.typenum);
		jsonw_uint_field(wr, "icmp-code",
				 match->match_value.icmpv4.code);
		break;
	case GPC_RULE_MATCH_VALUE_ICMPV6:
		jsonw_uint_field(wr, "icmpv6-type",
				 match->match_value.icmpv6.typenum);
		jsonw_uint_field(wr, "icmpv6-code",
				 match->match_value.icmpv6.code);
		break;
	case GPC_RULE_MATCH_VALUE_ICMPV6_CLASS:
		jsonw_uint_field(wr, "icmpv6-class",
				 match->match_value.icmpv6_class);
		break;
	case GPC_RULE_MATCH_VALUE_PROTO_BASE:
		jsonw_uint_field(wr, "base-protocol",
				 match->match_value.proto_base);
		break;
	case GPC_RULE_MATCH_VALUE_PROTO_FINAL:
		jsonw_uint_field(wr, "final-protocol",
				 match->match_value.proto_final);
		break;
	default:
		RTE_LOG(ERR, GPC, "Unknown GPC Match match-type %u\n",
			match->match_type);
		break;
	}
	jsonw_end_object(wr);
	return true;
}

static gpc_pb_rule_action_walker_cb gpc_op_show_action;
static bool
gpc_op_show_action(struct gpc_pb_action *action,
		   struct gpc_walk_context *walk_ctx)
{
	struct gpc_show_context *show_ctx =
		(struct gpc_show_context *)walk_ctx->data;
	json_writer_t *wr = show_ctx->wr;
	struct gpc_pb_policer *policer;

	jsonw_start_object(wr);
	switch (action->action_type) {
	case GPC_RULE_ACTION_VALUE_NOT_SET:
		break;
	case GPC_RULE_ACTION_VALUE_DECISION:
		jsonw_string_field(wr, "decision",
		     gpc_get_pkt_decision_str(action->action_value.decision));
		break;
	case GPC_RULE_ACTION_VALUE_DESIGNATION:
		jsonw_uint_field(wr, "designation",
				 action->action_value.designation);
		break;
	case GPC_RULE_ACTION_VALUE_COLOUR:
		jsonw_string_field(wr, "colour",
			gpc_get_pkt_colour_str(action->action_value.colour));
		break;
	case GPC_RULE_ACTION_VALUE_POLICER:
		policer = &action->action_value.policer;

		jsonw_uint_field(wr, "flags", policer->flags);
		if (policer->flags & POLICER_HAS_BW)
			jsonw_uint_field(wr, "bandwidth", policer->bw);
		if (policer->flags & POLICER_HAS_BURST)
			jsonw_uint_field(wr, "burst", policer->burst);
		if (policer->flags & POLICER_HAS_EXCESS_BW)
			jsonw_uint_field(wr, "excess-bandwidth",
					 policer->excess_bw);
		if (policer->flags & POLICER_HAS_EXCESS_BURST)
			jsonw_uint_field(wr, "excess-burst",
					 policer->excess_burst);
		if (policer->flags & POLICER_HAS_AWARENESS) {
			uint32_t val = policer->awareness;
			const char *aware = gpc_get_policer_awareness_str(val);
			jsonw_string_field(wr, "awareness", aware);
		}
		jsonw_uint_field(wr, "packets", 0);
		jsonw_uint_field(wr, "drops", 0);
		break;
	default:
		RTE_LOG(ERR, GPC, "Unknown GPC Action action-type %u\n",
			action->action_type);
		break;
	}
	jsonw_end_object(wr);
	return true;
}

static gpc_pb_table_rule_walker_cb gpc_op_show_rule;
static bool
gpc_op_show_rule(struct gpc_pb_rule *rule, struct gpc_walk_context *walk_ctx)
{
	struct gpc_show_context *show_ctx =
		(struct gpc_show_context *)walk_ctx->data;
	json_writer_t *wr = show_ctx->wr;

	jsonw_start_object(wr);
	jsonw_uint_field(wr, "rule-number", rule->number);

	jsonw_name(wr, "matches");
	jsonw_start_array(wr);
	gpc_pb_rule_match_walk(rule, gpc_op_show_match, walk_ctx);
	jsonw_end_array(wr);

	jsonw_name(wr, "actions");
	jsonw_start_array(wr);
	gpc_pb_rule_action_walk(rule, gpc_op_show_action, walk_ctx);
	jsonw_end_array(wr);

	if (rule->counter.counter_type != GPC_COUNTER_TYPE_UNKNOWN ||
	    rule->counter.name) {
		jsonw_name(wr, "counter");
		jsonw_start_object(wr);
		if (rule->counter.counter_type != GPC_COUNTER_TYPE_UNKNOWN)
			jsonw_uint_field(wr, "counter-type",
					 rule->counter.counter_type);
		if (rule->counter.name)
			jsonw_string_field(wr, "counter-name",
					   rule->counter.name);
		jsonw_uint_field(wr, "packets", 0);
		jsonw_uint_field(wr, "bytes", 0);
		jsonw_end_object(wr);
	}

	jsonw_uint_field(wr, "table-index", rule->table_index);
	jsonw_uint_field(wr, "orig-number", rule->orig_number);
	if (rule->result)
		jsonw_string_field(wr, "result", rule->result);
	jsonw_end_object(wr);
	return true;
}

static gpc_pb_feature_table_walker_cb gpc_op_show_table;
static bool
gpc_op_show_table(struct gpc_pb_table *table,
		  struct gpc_walk_context *walk_ctx)
{
	struct gpc_show_context *show_ctx =
		(struct gpc_show_context *)walk_ctx->data;
	json_writer_t *wr = show_ctx->wr;
	uint32_t i;

	jsonw_start_object(wr);
	jsonw_string_field(wr, "interface", table->ifname);
	jsonw_string_field(wr, "location",
			   gpc_get_table_location_str(table->location));

	jsonw_name(wr, "rules");
	jsonw_start_array(wr);
	gpc_pb_table_rule_walk(table, gpc_op_show_rule, walk_ctx);
	jsonw_end_array(wr);

	jsonw_string_field(wr, "traffic-type",
			   gpc_get_traffic_type_str(table->traffic_type));

	jsonw_name(wr, "table-names");
	jsonw_start_array(wr);
	for (i = 0; i < table->n_table_names; i++) {
		jsonw_start_object(wr);
		/*
		 * i + 1 because table-index starts at one, but we place the
		 * first table-name in table_names[0]
		 */
		jsonw_uint_field(wr, "table-index", i + 1);
		jsonw_string_field(wr, "name", table->table_names[i]);
		jsonw_end_object(wr);
	}
	jsonw_end_array(wr);
	jsonw_end_object(wr);
	return true; // keep walking
}

static gpc_pb_feature_counter_walker_cb gpc_op_show_counter;
static bool
gpc_op_show_counter(struct gpc_pb_counter *counter,
		    struct gpc_walk_context *walk_ctx)
{
	struct gpc_show_context *show_ctx =
		(struct gpc_show_context *)walk_ctx->data;
	json_writer_t *wr = show_ctx->wr;

	jsonw_start_object(wr);
	jsonw_string_field(wr, "name", counter->name);
	jsonw_string_field(wr, "format",
			   gpc_get_cntr_format_str(counter->format));
	jsonw_end_object(wr);
	return true; // keep walking
}

static gpc_pb_feature_walker_cb gpc_op_show_feature;
static bool
gpc_op_show_feature(struct gpc_pb_feature *feature,
		    struct gpc_walk_context *walk_ctx)
{
	struct gpc_show_context *show_ctx =
		(struct gpc_show_context *)walk_ctx->data;
	json_writer_t *wr = show_ctx->wr;

	jsonw_start_object(wr);
	jsonw_string_field(wr, "type", gpc_get_feature_type_str(feature->type));
	jsonw_name(wr, "tables");
	jsonw_start_array(wr);
	gpc_pb_feature_table_walk(feature, gpc_op_show_table, walk_ctx);
	jsonw_end_array(wr);
	jsonw_name(wr, "counters");
	jsonw_start_array(wr);
	gpc_pb_feature_counter_walk(feature, gpc_op_show_counter, walk_ctx);
	jsonw_end_array(wr);
	jsonw_end_object(wr);
	return true; // keep walking
}

/*
 * Handle: "gpc show [<feature-name> [<ifname> [<location> [<traffic-type>]]]]"
 * Output in Yang compatible JSON.
 */
static int
gpc_show(FILE *f, int argc __unused, char **argv __unused)
{
	struct gpc_show_context show_ctx;
	struct gpc_walk_context walk_ctx;

	--argc, ++argv;		/* skip "show" */
	show_ctx.wr = jsonw_new(f);
	if (!show_ctx.wr)
		return -ENOMEM;

	walk_ctx.data = (void *)&show_ctx;
	walk_ctx.feature_type = 0;
	walk_ctx.ifname = NULL;
	walk_ctx.location = 0;
	walk_ctx.traffic_type = 0;

	if (argc > 0)
		walk_ctx.feature_type = gpc_feature_str_to_type(argv[0]);

	if (argc > 1)
		walk_ctx.ifname = argv[1];

	if (argc > 2)
		walk_ctx.location = gpc_table_location_str_to_value(argv[2]);

	if (argc > 3)
		walk_ctx.traffic_type = gpc_traffic_type_str_to_value(argv[3]);

	jsonw_pretty(show_ctx.wr, true);

	jsonw_name(show_ctx.wr, "gpc");
	jsonw_start_array(show_ctx.wr);
	gpc_pb_feature_walk(gpc_op_show_feature, &walk_ctx);
	jsonw_end_array(show_ctx.wr);
	jsonw_destroy(&show_ctx.wr);
	return 0;
}

int
cmd_gpc_op(FILE *f, int argc, char **argv)
{
	--argc, ++argv;		/* skip "gpc" */
	if (argc < 1) {
		fprintf(f, "usage: missing qos command\n");
		return -1;
	}

	/* Check for op-mode commands first */
	if (strcmp(argv[0], "show") == 0)
		return gpc_show(f, argc, argv);

	return 0;
}
