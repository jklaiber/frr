/**
 * bgp_updgrp_adv.c: BGP update group advertisement and adjacency
 *                   maintenance
 *
 *
 * @copyright Copyright (C) 2014 Cumulus Networks, Inc.
 *
 * @author Avneesh Sachdev <avneesh@sproute.net>
 * @author Rajesh Varadarajan <rajesh@sproute.net>
 * @author Pradosh Mohapatra <pradosh@sproute.net>
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "command.h"
#include "memory.h"
#include "prefix.h"
#include "hash.h"
#include "thread.h"
#include "queue.h"
#include "routemap.h"
#include "filter.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_table.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_advertise.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_aspath.h"
#include "bgpd/bgp_packet.h"
#include "bgpd/bgp_fsm.h"
#include "bgpd/bgp_mplsvpn.h"
#include "bgpd/bgp_updgrp.h"
#include "bgpd/bgp_advertise.h"
#include "bgpd/bgp_addpath.h"


/********************
 * PRIVATE FUNCTIONS
 ********************/
static int bgp_adj_out_compare(const struct bgp_adj_out *o1,
			       const struct bgp_adj_out *o2)
{
	if (o1->subgroup < o2->subgroup)
		return -1;

	if (o1->subgroup > o2->subgroup)
		return 1;

	if (o1->addpath_tx_id < o2->addpath_tx_id)
		return -1;

	if (o1->addpath_tx_id > o2->addpath_tx_id)
		return 1;

	return 0;
}
RB_GENERATE(bgp_adj_out_rb, bgp_adj_out, adj_entry, bgp_adj_out_compare);

static inline struct bgp_adj_out *adj_lookup(struct bgp_dest *dest,
					     struct update_subgroup *subgrp,
					     uint32_t addpath_tx_id)
{
	struct bgp_adj_out lookup;

	if (!dest || !subgrp)
		return NULL;

	/* update-groups that do not support addpath will pass 0 for
	 * addpath_tx_id. */
	lookup.subgroup = subgrp;
	lookup.addpath_tx_id = addpath_tx_id;

	return RB_FIND(bgp_adj_out_rb, &dest->adj_out, &lookup);
}

static void adj_free(struct bgp_adj_out *adj)
{
	TAILQ_REMOVE(&(adj->subgroup->adjq), adj, subgrp_adj_train);
	SUBGRP_DECR_STAT(adj->subgroup, adj_count);
	XFREE(MTYPE_BGP_ADJ_OUT, adj);
}

static void subgrp_withdraw_stale_addpath(struct updwalk_context *ctx,
					  struct update_subgroup *subgrp)
{
	struct bgp_adj_out *adj, *adj_next;
	uint32_t id;
	struct bgp_path_info *pi;
	afi_t afi = SUBGRP_AFI(subgrp);
	safi_t safi = SUBGRP_SAFI(subgrp);
	struct peer *peer = SUBGRP_PEER(subgrp);

	/* Look through all of the paths we have advertised for this rn and send
	 * a withdraw for the ones that are no longer present */
	RB_FOREACH_SAFE (adj, bgp_adj_out_rb, &ctx->dest->adj_out, adj_next) {

		if (adj->subgroup == subgrp) {
			for (pi = bgp_dest_get_bgp_path_info(ctx->dest); pi;
			     pi = pi->next) {
				id = bgp_addpath_id_for_peer(peer, afi, safi,
					&pi->tx_addpath);

				if (id == adj->addpath_tx_id) {
					break;
				}
			}

			if (!pi) {
				subgroup_process_announce_selected(
					subgrp, NULL, ctx->dest,
					adj->addpath_tx_id);
			}
		}
	}
}

static int group_announce_route_walkcb(struct update_group *updgrp, void *arg)
{
	struct updwalk_context *ctx = arg;
	struct update_subgroup *subgrp;
	struct bgp_path_info *pi;
	afi_t afi;
	safi_t safi;
	struct peer *peer;
	struct bgp_adj_out *adj, *adj_next;
	int addpath_capable;

	afi = UPDGRP_AFI(updgrp);
	safi = UPDGRP_SAFI(updgrp);
	peer = UPDGRP_PEER(updgrp);
	addpath_capable = bgp_addpath_encode_tx(peer, afi, safi);

	if (BGP_DEBUG(update, UPDATE_OUT))
		zlog_debug("%s: afi=%s, safi=%s, p=%pRN", __func__,
			   afi2str(afi), safi2str(safi),
			   bgp_dest_to_rnode(ctx->dest));

	UPDGRP_FOREACH_SUBGRP (updgrp, subgrp) {

		/*
		 * Skip the subgroups that have coalesce timer running. We will
		 * walk the entire prefix table for those subgroups when the
		 * coalesce timer fires.
		 */
		if (!subgrp->t_coalesce) {
			/* An update-group that uses addpath */
			if (addpath_capable) {
				subgrp_withdraw_stale_addpath(ctx, subgrp);

				for (pi = bgp_dest_get_bgp_path_info(ctx->dest);
				     pi; pi = pi->next) {
					/* Skip the bestpath for now */
					if (pi == ctx->pi)
						continue;

					subgroup_process_announce_selected(
						subgrp, pi, ctx->dest,
						bgp_addpath_id_for_peer(
							peer, afi, safi,
							&pi->tx_addpath));
				}

				/* Process the bestpath last so the "show [ip]
				 * bgp neighbor x.x.x.x advertised"
				 * output shows the attributes from the bestpath
				 */
				if (ctx->pi)
					subgroup_process_announce_selected(
						subgrp, ctx->pi, ctx->dest,
						bgp_addpath_id_for_peer(
							peer, afi, safi,
							&ctx->pi->tx_addpath));
			}

			/* An update-group that does not use addpath */
			else {
				if (ctx->pi) {
					subgroup_process_announce_selected(
						subgrp, ctx->pi, ctx->dest,
						bgp_addpath_id_for_peer(
							peer, afi, safi,
							&ctx->pi->tx_addpath));
				} else {
					/* Find the addpath_tx_id of the path we
					 * had advertised and
					 * send a withdraw */
					RB_FOREACH_SAFE (adj, bgp_adj_out_rb,
							 &ctx->dest->adj_out,
							 adj_next) {
						if (adj->subgroup == subgrp) {
							subgroup_process_announce_selected(
								subgrp, NULL,
								ctx->dest,
								adj->addpath_tx_id);
						}
					}
				}
			}
		}

		/* Notify BGP Conditional advertisement */
		bgp_notify_conditional_adv_scanner(subgrp);
	}

	return UPDWALK_CONTINUE;
}

static void subgrp_show_adjq_vty(struct update_subgroup *subgrp,
				 struct vty *vty, uint8_t flags)
{
	struct bgp_table *table;
	struct bgp_adj_out *adj;
	unsigned long output_count;
	struct bgp_dest *dest;
	int header1 = 1;
	struct bgp *bgp;
	int header2 = 1;

	bgp = SUBGRP_INST(subgrp);
	if (!bgp)
		return;

	table = bgp->rib[SUBGRP_AFI(subgrp)][SUBGRP_SAFI(subgrp)];

	output_count = 0;

	for (dest = bgp_table_top(table); dest; dest = bgp_route_next(dest)) {
		const struct prefix *dest_p = bgp_dest_get_prefix(dest);

		RB_FOREACH (adj, bgp_adj_out_rb, &dest->adj_out)
			if (adj->subgroup == subgrp) {
				if (header1) {
					vty_out(vty,
						"BGP table version is %" PRIu64
						", local router ID is %pI4\n",
						table->version,
						&bgp->router_id);
					vty_out(vty, BGP_SHOW_SCODE_HEADER);
					vty_out(vty, BGP_SHOW_OCODE_HEADER);
					header1 = 0;
				}
				if (header2) {
					vty_out(vty, BGP_SHOW_HEADER);
					header2 = 0;
				}
				if ((flags & UPDWALK_FLAGS_ADVQUEUE) && adj->adv
				    && adj->adv->baa) {
					route_vty_out_tmp(vty, dest_p,
							  adj->adv->baa->attr,
							  SUBGRP_SAFI(subgrp),
							  0, NULL, false);
					output_count++;
				}
				if ((flags & UPDWALK_FLAGS_ADVERTISED)
				    && adj->attr) {
					route_vty_out_tmp(vty, dest_p,
							  adj->attr,
							  SUBGRP_SAFI(subgrp),
							  0, NULL, false);
					output_count++;
				}
			}
	}
	if (output_count != 0)
		vty_out(vty, "\nTotal number of prefixes %ld\n", output_count);
}

static int updgrp_show_adj_walkcb(struct update_group *updgrp, void *arg)
{
	struct updwalk_context *ctx = arg;
	struct update_subgroup *subgrp;
	struct vty *vty;

	vty = ctx->vty;
	UPDGRP_FOREACH_SUBGRP (updgrp, subgrp) {
		if (ctx->subgrp_id && (ctx->subgrp_id != subgrp->id))
			continue;
		vty_out(vty, "update group %" PRIu64 ", subgroup %" PRIu64 "\n",
			updgrp->id, subgrp->id);
		subgrp_show_adjq_vty(subgrp, vty, ctx->flags);
	}
	return UPDWALK_CONTINUE;
}

static void updgrp_show_adj(struct bgp *bgp, afi_t afi, safi_t safi,
			    struct vty *vty, uint64_t id, uint8_t flags)
{
	struct updwalk_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.vty = vty;
	ctx.subgrp_id = id;
	ctx.flags = flags;

	update_group_af_walk(bgp, afi, safi, updgrp_show_adj_walkcb, &ctx);
}

static int subgroup_coalesce_timer(struct thread *thread)
{
	struct update_subgroup *subgrp;

	subgrp = THREAD_ARG(thread);
	if (bgp_debug_update(NULL, NULL, subgrp->update_group, 0))
		zlog_debug("u%" PRIu64 ":s%" PRIu64" announcing routes upon coalesce timer expiry(%u ms)",
			   (SUBGRP_UPDGRP(subgrp))->id, subgrp->id,
			   subgrp->v_coalesce);
	subgrp->t_coalesce = NULL;
	subgrp->v_coalesce = 0;
	subgroup_announce_route(subgrp);


	/* While the announce_route() may kick off the route advertisement timer
	 * for
	 * the members of the subgroup, we'd like to send the initial updates
	 * much
	 * faster (i.e., without enforcing MRAI). Also, if there were no routes
	 * to
	 * announce, this is the method currently employed to trigger the EOR.
	 */
	if (!bgp_update_delay_active(SUBGRP_INST(subgrp))) {
		struct peer_af *paf;
		struct peer *peer;

		SUBGRP_FOREACH_PEER (subgrp, paf) {
			peer = PAF_PEER(paf);
			BGP_TIMER_OFF(peer->t_routeadv);
			BGP_TIMER_ON(peer->t_routeadv, bgp_routeadv_timer, 0);
		}
	}

	return 0;
}

static int update_group_announce_walkcb(struct update_group *updgrp, void *arg)
{
	struct update_subgroup *subgrp;

	UPDGRP_FOREACH_SUBGRP (updgrp, subgrp) {
		subgroup_announce_all(subgrp);
	}

	return UPDWALK_CONTINUE;
}

static int update_group_announce_rrc_walkcb(struct update_group *updgrp,
					    void *arg)
{
	struct update_subgroup *subgrp;
	afi_t afi;
	safi_t safi;
	struct peer *peer;

	afi = UPDGRP_AFI(updgrp);
	safi = UPDGRP_SAFI(updgrp);
	peer = UPDGRP_PEER(updgrp);

	/* Only announce if this is a group of route-reflector-clients */
	if (CHECK_FLAG(peer->af_flags[afi][safi], PEER_FLAG_REFLECTOR_CLIENT)) {
		UPDGRP_FOREACH_SUBGRP (updgrp, subgrp) {
			subgroup_announce_all(subgrp);
		}
	}

	return UPDWALK_CONTINUE;
}

/********************
 * PUBLIC FUNCTIONS
 ********************/

/**
 * Allocate an adj-out object. Do proper initialization of its fields,
 * primarily its association with the subgroup and the prefix.
 */
struct bgp_adj_out *bgp_adj_out_alloc(struct update_subgroup *subgrp,
				      struct bgp_dest *dest,
				      uint32_t addpath_tx_id)
{
	struct bgp_adj_out *adj;

	adj = XCALLOC(MTYPE_BGP_ADJ_OUT, sizeof(struct bgp_adj_out));
	adj->subgroup = subgrp;
	adj->addpath_tx_id = addpath_tx_id;

	if (dest) {
		RB_INSERT(bgp_adj_out_rb, &dest->adj_out, adj);
		bgp_dest_lock_node(dest);
		adj->dest = dest;
	}

	TAILQ_INSERT_TAIL(&(subgrp->adjq), adj, subgrp_adj_train);
	SUBGRP_INCR_STAT(subgrp, adj_count);
	return adj;
}


struct bgp_advertise *
bgp_advertise_clean_subgroup(struct update_subgroup *subgrp,
			     struct bgp_adj_out *adj)
{
	struct bgp_advertise *adv;
	struct bgp_advertise_attr *baa;
	struct bgp_advertise *next;
	struct bgp_adv_fifo_head *fhead;

	adv = adj->adv;
	baa = adv->baa;
	next = NULL;

	if (baa) {
		fhead = &subgrp->sync->update;

		/* Unlink myself from advertise attribute FIFO.  */
		bgp_advertise_delete(baa, adv);

		/* Fetch next advertise candidate. */
		next = baa->adv;

		/* Unintern BGP advertise attribute.  */
		bgp_advertise_unintern(subgrp->hash, baa);
	} else
		fhead = &subgrp->sync->withdraw;


	/* Unlink myself from advertisement FIFO.  */
	bgp_adv_fifo_del(fhead, adv);

	/* Free memory.  */
	bgp_advertise_free(adj->adv);
	adj->adv = NULL;

	return next;
}

void bgp_adj_out_set_subgroup(struct bgp_dest *dest,
			      struct update_subgroup *subgrp, struct attr *attr,
			      struct bgp_path_info *path)
{
	struct bgp_adj_out *adj = NULL;
	struct bgp_advertise *adv;
	struct peer *peer;
	afi_t afi;
	safi_t safi;

	peer = SUBGRP_PEER(subgrp);
	afi = SUBGRP_AFI(subgrp);
	safi = SUBGRP_SAFI(subgrp);

	if (DISABLE_BGP_ANNOUNCE)
		return;

	/* Look for adjacency information. */
	adj = adj_lookup(
		dest, subgrp,
		bgp_addpath_id_for_peer(peer, afi, safi, &path->tx_addpath));

	if (!adj) {
		adj = bgp_adj_out_alloc(
			subgrp, dest,
			bgp_addpath_id_for_peer(peer, afi, safi,
						&path->tx_addpath));
		if (!adj)
			return;
	}

	if (adj->adv)
		bgp_advertise_clean_subgroup(subgrp, adj);
	adj->adv = bgp_advertise_new();

	adv = adj->adv;
	adv->dest = dest;
	assert(adv->pathi == NULL);
	/* bgp_path_info adj_out reference */
	adv->pathi = bgp_path_info_lock(path);

	if (attr)
		adv->baa = bgp_advertise_intern(subgrp->hash, attr);
	else
		adv->baa = baa_new();
	adv->adj = adj;

	/* Add new advertisement to advertisement attribute list. */
	bgp_advertise_add(adv->baa, adv);

	/*
	 * If the update adv list is empty, trigger the member peers'
	 * mrai timers so the socket writes can happen.
	 */
	if (!bgp_adv_fifo_count(&subgrp->sync->update)) {
		struct peer_af *paf;

		SUBGRP_FOREACH_PEER (subgrp, paf) {
			bgp_adjust_routeadv(PAF_PEER(paf));
		}
	}

	bgp_adv_fifo_add_tail(&subgrp->sync->update, adv);

	subgrp->version = max(subgrp->version, dest->version);
}

/* The only time 'withdraw' will be false is if we are sending
 * the "neighbor x.x.x.x default-originate" default and need to clear
 * bgp_adj_out for the 0.0.0.0/0 route in the BGP table.
 */
void bgp_adj_out_unset_subgroup(struct bgp_dest *dest,
				struct update_subgroup *subgrp, char withdraw,
				uint32_t addpath_tx_id)
{
	struct bgp_adj_out *adj;
	struct bgp_advertise *adv;
	bool trigger_write;

	if (DISABLE_BGP_ANNOUNCE)
		return;

	/* Lookup existing adjacency */
	if ((adj = adj_lookup(dest, subgrp, addpath_tx_id)) != NULL) {
		/* Clean up previous advertisement.  */
		if (adj->adv)
			bgp_advertise_clean_subgroup(subgrp, adj);

		/* If default originate is enabled and the route is default
		 * route, do not send withdraw. This will prevent deletion of
		 * the default route at the peer.
		 */
		if (CHECK_FLAG(subgrp->sflags, SUBGRP_STATUS_DEFAULT_ORIGINATE)
		    && is_default_prefix(bgp_dest_get_prefix(dest)))
			return;

		if (adj->attr && withdraw) {
			/* We need advertisement structure.  */
			adj->adv = bgp_advertise_new();
			adv = adj->adv;
			adv->dest = dest;
			adv->adj = adj;

			/* Note if we need to trigger a packet write */
			trigger_write =
				!bgp_adv_fifo_count(&subgrp->sync->withdraw);

			/* Add to synchronization entry for withdraw
			 * announcement.  */
			bgp_adv_fifo_add_tail(&subgrp->sync->withdraw, adv);

			if (trigger_write)
				subgroup_trigger_write(subgrp);
		} else {
			/* Remove myself from adjacency. */
			RB_REMOVE(bgp_adj_out_rb, &dest->adj_out, adj);

			/* Free allocated information.  */
			adj_free(adj);

			bgp_dest_unlock_node(dest);
		}
	}

	subgrp->version = max(subgrp->version, dest->version);
}

void bgp_adj_out_remove_subgroup(struct bgp_dest *dest, struct bgp_adj_out *adj,
				 struct update_subgroup *subgrp)
{
	if (adj->attr)
		bgp_attr_unintern(&adj->attr);

	if (adj->adv)
		bgp_advertise_clean_subgroup(subgrp, adj);

	RB_REMOVE(bgp_adj_out_rb, &dest->adj_out, adj);
	adj_free(adj);
}

/*
 * Go through all the routes and clean up the adj/adv structures corresponding
 * to the subgroup.
 */
void subgroup_clear_table(struct update_subgroup *subgrp)
{
	struct bgp_adj_out *aout, *taout;

	SUBGRP_FOREACH_ADJ_SAFE (subgrp, aout, taout) {
		struct bgp_dest *dest = aout->dest;
		bgp_adj_out_remove_subgroup(dest, aout, subgrp);
		bgp_dest_unlock_node(dest);
	}
}

/*
 * subgroup_announce_table
 */
void subgroup_announce_table(struct update_subgroup *subgrp,
			     struct bgp_table *table)
{
	struct bgp_dest *dest;
	struct bgp_path_info *ri;
	struct attr attr;
	struct peer *peer;
	afi_t afi;
	safi_t safi;
	int addpath_capable;

	peer = SUBGRP_PEER(subgrp);
	afi = SUBGRP_AFI(subgrp);
	safi = SUBGRP_SAFI(subgrp);
	addpath_capable = bgp_addpath_encode_tx(peer, afi, safi);

	if (safi == SAFI_LABELED_UNICAST)
		safi = SAFI_UNICAST;

	if (!table)
		table = peer->bgp->rib[afi][safi];

	if (safi != SAFI_MPLS_VPN && safi != SAFI_ENCAP && safi != SAFI_EVPN
	    && CHECK_FLAG(peer->af_flags[afi][safi],
			  PEER_FLAG_DEFAULT_ORIGINATE))
		subgroup_default_originate(subgrp, 0);

	for (dest = bgp_table_top(table); dest; dest = bgp_route_next(dest)) {
		const struct prefix *dest_p = bgp_dest_get_prefix(dest);

		for (ri = bgp_dest_get_bgp_path_info(dest); ri; ri = ri->next)

			if (CHECK_FLAG(ri->flags, BGP_PATH_SELECTED)
			    || (addpath_capable
				&& bgp_addpath_tx_path(
					   peer->addpath_type[afi][safi],
					   ri))) {
				if (subgroup_announce_check(dest, ri, subgrp,
							    dest_p, &attr,
							    false))
					bgp_adj_out_set_subgroup(dest, subgrp,
								 &attr, ri);
				else {
					/* If default originate is enabled for
					 * the peer, do not send explicit
					 * withdraw. This will prevent deletion
					 * of default route advertised through
					 * default originate
					 */
					if (CHECK_FLAG(
						    peer->af_flags[afi][safi],
						    PEER_FLAG_DEFAULT_ORIGINATE)
					    && is_default_prefix(bgp_dest_get_prefix(dest)))
						break;

					bgp_adj_out_unset_subgroup(
						dest, subgrp, 1,
						bgp_addpath_id_for_peer(
							peer, afi, safi,
							&ri->tx_addpath));
				}
			}
	}

	/*
	 * We walked through the whole table -- make sure our version number
	 * is consistent with the one on the table. This should allow
	 * subgroups to merge sooner if a peer comes up when the route node
	 * with the largest version is no longer in the table. This also
	 * covers the pathological case where all routes in the table have
	 * now been deleted.
	 */
	subgrp->version = max(subgrp->version, table->version);

	/*
	 * Start a task to merge the subgroup if necessary.
	 */
	update_subgroup_trigger_merge_check(subgrp, 0);
}

/*
 * subgroup_announce_route
 *
 * Refresh all routes out to a subgroup.
 */
void subgroup_announce_route(struct update_subgroup *subgrp)
{
	struct bgp_dest *dest;
	struct bgp_table *table;
	struct peer *onlypeer;

	if (update_subgroup_needs_refresh(subgrp)) {
		update_subgroup_set_needs_refresh(subgrp, 0);
	}

	/*
	 * First update is deferred until ORF or ROUTE-REFRESH is received
	 */
	onlypeer = ((SUBGRP_PCOUNT(subgrp) == 1) ? (SUBGRP_PFIRST(subgrp))->peer
						 : NULL);
	if (onlypeer && CHECK_FLAG(onlypeer->af_sflags[SUBGRP_AFI(subgrp)]
						      [SUBGRP_SAFI(subgrp)],
				   PEER_STATUS_ORF_WAIT_REFRESH))
		return;

	if (SUBGRP_SAFI(subgrp) != SAFI_MPLS_VPN
	    && SUBGRP_SAFI(subgrp) != SAFI_ENCAP
	    && SUBGRP_SAFI(subgrp) != SAFI_EVPN)
		subgroup_announce_table(subgrp, NULL);
	else
		for (dest = bgp_table_top(update_subgroup_rib(subgrp)); dest;
		     dest = bgp_route_next(dest)) {
			table = bgp_dest_get_bgp_table_info(dest);
			if (!table)
				continue;
			subgroup_announce_table(subgrp, table);
		}
}

void subgroup_default_originate(struct update_subgroup *subgrp, int withdraw)
{
	struct bgp *bgp;
	struct attr attr;
	struct attr *new_attr = &attr;
	struct aspath *aspath;
	struct prefix p;
	struct peer *from;
	struct bgp_dest *dest;
	struct bgp_path_info *pi;
	struct peer *peer;
	struct bgp_adj_out *adj;
	route_map_result_t ret = RMAP_DENYMATCH;
	afi_t afi;
	safi_t safi;

	if (!subgrp)
		return;

	peer = SUBGRP_PEER(subgrp);
	afi = SUBGRP_AFI(subgrp);
	safi = SUBGRP_SAFI(subgrp);

	if (!(afi == AFI_IP || afi == AFI_IP6))
		return;

	bgp = peer->bgp;
	from = bgp->peer_self;

	bgp_attr_default_set(&attr, BGP_ORIGIN_IGP);
	aspath = attr.aspath;

	attr.local_pref = bgp->default_local_pref;

	if ((afi == AFI_IP6) || peer_cap_enhe(peer, afi, safi)) {
		/* IPv6 global nexthop must be included. */
		attr.mp_nexthop_len = BGP_ATTR_NHLEN_IPV6_GLOBAL;

		/* If the peer is on shared nextwork and we have link-local
		   nexthop set it. */
		if (peer->shared_network
		    && !IN6_IS_ADDR_UNSPECIFIED(&peer->nexthop.v6_local))
			attr.mp_nexthop_len = BGP_ATTR_NHLEN_IPV6_GLOBAL_AND_LL;
	}

	if (peer->default_rmap[afi][safi].name) {
		struct attr attr_tmp = attr;
		struct bgp_path_info bpi_rmap = {0};

		bpi_rmap.peer = bgp->peer_self;
		bpi_rmap.attr = &attr_tmp;

		SET_FLAG(bgp->peer_self->rmap_type, PEER_RMAP_TYPE_DEFAULT);

		/* Iterate over the RIB to see if we can announce
		 * the default route. We announce the default
		 * route only if route-map has a match.
		 */
		for (dest = bgp_table_top(bgp->rib[afi][safi]); dest;
		     dest = bgp_route_next(dest)) {
			if (!bgp_dest_has_bgp_path_info_data(dest))
				continue;

			ret = route_map_apply(peer->default_rmap[afi][safi].map,
					      bgp_dest_get_prefix(dest),
					      RMAP_BGP, &bpi_rmap);

			if (ret != RMAP_DENYMATCH)
				break;
		}
		bgp->peer_self->rmap_type = 0;
		new_attr = bgp_attr_intern(&attr_tmp);

		if (ret == RMAP_DENYMATCH) {
			bgp_attr_flush(&attr_tmp);
			withdraw = 1;
		}
	}

	/* Check if the default route is in local BGP RIB which is
	 * installed through redistribute or network command
	 */
	memset(&p, 0, sizeof(p));
	p.family = afi2family(afi);
	p.prefixlen = 0;
	dest = bgp_afi_node_lookup(bgp->rib[afi][safi], afi, safi, &p, NULL);

	if (withdraw) {
		/* Withdraw the default route advertised using default
		 * originate
		 */
		if (CHECK_FLAG(subgrp->sflags, SUBGRP_STATUS_DEFAULT_ORIGINATE))
			subgroup_default_withdraw_packet(subgrp);
		UNSET_FLAG(subgrp->sflags, SUBGRP_STATUS_DEFAULT_ORIGINATE);

		/* If default route is present in the local RIB, advertise the
		 * route
		 */
		if (dest != NULL) {
			for (pi = bgp_dest_get_bgp_path_info(dest); pi;
			     pi = pi->next) {
				if (CHECK_FLAG(pi->flags, BGP_PATH_SELECTED))
					if (subgroup_announce_check(
						    dest, pi, subgrp,
						    bgp_dest_get_prefix(dest),
						    &attr, false))
						bgp_adj_out_set_subgroup(
							dest, subgrp, &attr,
							pi);
			}
		}
	} else {
		if (!CHECK_FLAG(subgrp->sflags,
				SUBGRP_STATUS_DEFAULT_ORIGINATE)) {

			/* The 'neighbor x.x.x.x default-originate' default will
			 * act as an
			 * implicit withdraw for any previous UPDATEs sent for
			 * 0.0.0.0/0 so
			 * clear adj_out for the 0.0.0.0/0 prefix in the BGP
			 * table.
			 */
			if (dest != NULL) {
				/* Remove the adjacency for the previously
				 * advertised default route
				 */
				adj = adj_lookup(
				       dest, subgrp,
				       BGP_ADDPATH_TX_ID_FOR_DEFAULT_ORIGINATE);
				if (adj != NULL) {
					/* Clean up previous advertisement.  */
					if (adj->adv)
						bgp_advertise_clean_subgroup(
							subgrp, adj);

					/* Remove  from adjacency. */
					RB_REMOVE(bgp_adj_out_rb,
						  &dest->adj_out, adj);

					/* Free allocated information.  */
					adj_free(adj);

					bgp_dest_unlock_node(dest);
				}
			}

			/* Advertise the default route */
			if (bgp_in_graceful_shutdown(bgp))
				bgp_attr_add_gshut_community(new_attr);

			SET_FLAG(subgrp->sflags,
				 SUBGRP_STATUS_DEFAULT_ORIGINATE);
			subgroup_default_update_packet(subgrp, new_attr, from);
		}
	}

	aspath_unintern(&aspath);
}

/*
 * Announce the BGP table to a subgroup.
 *
 * At startup, we try to optimize route announcement by coalescing the
 * peer-up events. This is done only the first time - from then on,
 * subgrp->v_coalesce will be set to zero and the normal logic
 * prevails.
 */
void subgroup_announce_all(struct update_subgroup *subgrp)
{
	if (!subgrp)
		return;

	/*
	 * If coalesce timer value is not set, announce routes immediately.
	 */
	if (!subgrp->v_coalesce) {
		if (bgp_debug_update(NULL, NULL, subgrp->update_group, 0))
			zlog_debug("u%" PRIu64 ":s%" PRIu64" announcing all routes",
				   subgrp->update_group->id, subgrp->id);
		subgroup_announce_route(subgrp);
		return;
	}

	/*
	 * We should wait for the coalesce timer. Arm the timer if not done.
	 */
	if (!subgrp->t_coalesce) {
		thread_add_timer_msec(bm->master, subgroup_coalesce_timer,
				      subgrp, subgrp->v_coalesce,
				      &subgrp->t_coalesce);
	}
}

/*
 * Go through all update subgroups and set up the adv queue for the
 * input route.
 */
void group_announce_route(struct bgp *bgp, afi_t afi, safi_t safi,
			  struct bgp_dest *dest, struct bgp_path_info *pi)
{
	struct updwalk_context ctx;
	ctx.pi = pi;
	ctx.dest = dest;
	update_group_af_walk(bgp, afi, safi, group_announce_route_walkcb, &ctx);
}

void update_group_show_adj_queue(struct bgp *bgp, afi_t afi, safi_t safi,
				 struct vty *vty, uint64_t id)
{
	updgrp_show_adj(bgp, afi, safi, vty, id, UPDWALK_FLAGS_ADVQUEUE);
}

void update_group_show_advertised(struct bgp *bgp, afi_t afi, safi_t safi,
				  struct vty *vty, uint64_t id)
{
	updgrp_show_adj(bgp, afi, safi, vty, id, UPDWALK_FLAGS_ADVERTISED);
}

void update_group_announce(struct bgp *bgp)
{
	update_group_walk(bgp, update_group_announce_walkcb, NULL);
}

void update_group_announce_rrclients(struct bgp *bgp)
{
	update_group_walk(bgp, update_group_announce_rrc_walkcb, NULL);
}
