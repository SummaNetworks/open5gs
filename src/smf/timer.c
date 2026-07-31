/*
 * Copyright (C) 2019-2022 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "context.h"
#include "gtp-path.h"
#include "metrics.h"

const char *smf_timer_get_name(int timer_id)
{
    switch (timer_id) {
    case OGS_TIMER_NF_INSTANCE_REGISTRATION_INTERVAL:
        return OGS_TIMER_NAME_NF_INSTANCE_REGISTRATION_INTERVAL;
    case OGS_TIMER_NF_INSTANCE_HEARTBEAT_INTERVAL:
        return OGS_TIMER_NAME_NF_INSTANCE_HEARTBEAT_INTERVAL;
    case OGS_TIMER_NF_INSTANCE_NO_HEARTBEAT:
        return OGS_TIMER_NAME_NF_INSTANCE_NO_HEARTBEAT;
    case OGS_TIMER_NF_INSTANCE_VALIDITY:
        return OGS_TIMER_NAME_NF_INSTANCE_VALIDITY;
    case OGS_TIMER_SUBSCRIPTION_VALIDITY:
        return OGS_TIMER_NAME_SUBSCRIPTION_VALIDITY;
    case OGS_TIMER_SUBSCRIPTION_PATCH:
        return OGS_TIMER_NAME_SUBSCRIPTION_PATCH;
    case OGS_TIMER_SBI_CLIENT_WAIT:
        return OGS_TIMER_NAME_SBI_CLIENT_WAIT;
    case SMF_TIMER_PFCP_ASSOCIATION:
        return "SMF_TIMER_PFCP_ASSOCIATION";
    case SMF_TIMER_PFCP_NO_HEARTBEAT:
        return "SMF_TIMER_PFCP_NO_HEARTBEAT";
    case SMF_TIMER_PFCP_NO_ESTABLISHMENT_RESPONSE:
        return "SMF_TIMER_PFCP_NO_ESTABLISHMENT_RESPONSE";
    case SMF_TIMER_PFCP_NO_DELETION_RESPONSE:
        return "SMF_TIMER_PFCP_NO_DELETION_RESPONSE";
    case SMF_TIMER_GTP_NODE_CLEANUP:
        return "SMF_TIMER_GTP_NODE_CLEANUP";
    case SMF_TIMER_HANDOVER_HOLD:
        return "SMF_TIMER_HANDOVER_HOLD";
    default:
       break;
    }

    ogs_error("Unknown Timer[%d]", timer_id);
    return "UNKNOWN_TIMER";
}

static void timer_send_event(int timer_id, void *data)
{
    int rv;
    smf_event_t *e = NULL;
    ogs_assert(data);

    switch (timer_id) {
    case SMF_TIMER_PFCP_ASSOCIATION:
    case SMF_TIMER_PFCP_NO_HEARTBEAT:
        e = smf_event_new(SMF_EVT_N4_TIMER);
        ogs_assert(e);
        e->h.timer_id = timer_id;
        e->pfcp_node = data;
        break;
    default:
        ogs_fatal("Unknown timer id[%d]", timer_id);
        ogs_assert_if_reached();
        break;
    }

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed [%d] in %s",
                (int)rv, smf_timer_get_name(timer_id));
        ogs_event_free(e);
    }
}

void smf_timer_pfcp_association(void *data)
{
    timer_send_event(SMF_TIMER_PFCP_ASSOCIATION, data);
}

void smf_timer_pfcp_no_heartbeat(void *data)
{
    timer_send_event(SMF_TIMER_PFCP_NO_HEARTBEAT, data);
}

void smf_timer_gtp_node_cleanup(void *data)
{
    ogs_gtp_node_t *gnode = NULL, *next_gnode = NULL;
    char buf[OGS_ADDRSTRLEN];
    int removed_count = 0;
    int total_count = 0;

    /* Check if cleanup is enabled */
    if (!smf_self()->gtp_node_cleanup_enabled) {
        ogs_debug("GTP node cleanup is disabled - timer stopped");
        return;
    }

    ogs_info("GTP node cleanup cycle starting...");

    /* SMF-specific cleanup: free wrapper nodes first */
    ogs_time_t now = ogs_get_monotonic_time();
    ogs_time_t idle_threshold = 2 * 60 * 1000000; /* 2 minutes in microseconds (reduced from 5 min) */
    
    ogs_list_for_each_safe(&smf_self()->sgw_s5c_list, next_gnode, gnode) {
        total_count++;

        int local_count = ogs_list_count(&gnode->local_list);
        int remote_count = ogs_list_count(&gnode->remote_list);
        ogs_time_t idle_time = now - gnode->last_activity;

        /* Remove nodes with no active transactions AND idle for more than 2 minutes */
        if (local_count == 0 && remote_count == 0 && idle_time > idle_threshold) {
            smf_gtp_node_t *smf_gnode = gnode->data_ptr;
            smf_ue_t *smf_ue = NULL;
            smf_sess_t *sess = NULL;

            ogs_info("Removing idle SMF GTP node [%s]:%d (idle for %ld seconds, no transactions)",
                    OGS_ADDR(&gnode->addr, buf), OGS_PORT(&gnode->addr),
                    (long)(idle_time / 1000000));

            /* Invalidate session references before removing gnode */
            ogs_list_for_each(&smf_self()->smf_ue_list, smf_ue) {
                ogs_list_for_each(&smf_ue->sess_list, sess) {
                    if (sess->gnode == gnode) {
                        ogs_warn("Detaching session from idle GTP node [%s]:%d",
                                OGS_ADDR(&gnode->addr, buf),
                                OGS_PORT(&gnode->addr));
                        sess->gnode = NULL;
                    }
                }
            }

            /* Free SMF wrapper first */
            if (smf_gnode) {
                smf_gtp_node_free(smf_gnode);
            }

            /* Then remove the underlying GTP node */
            ogs_gtp_node_remove(&smf_self()->sgw_s5c_list, gnode);
            smf_metrics_inst_global_dec(SMF_METR_GLOB_GAUGE_GTP_PEERS_ACTIVE);
            removed_count++;
        } else {
            ogs_debug("Keeping GTP node [%s]:%d (idle:%ld sec, local:%d, remote:%d transactions)", 
                    OGS_ADDR(&gnode->addr, buf), OGS_PORT(&gnode->addr),
                    (long)(idle_time / 1000000), local_count, remote_count);
        }
    }

    if (removed_count > 0) {
        ogs_info("SMF GTP node cleanup: removed %d idle nodes, %d nodes remaining", 
                removed_count, total_count - removed_count);
    } else if (total_count > 0) {
        ogs_debug("SMF GTP node cleanup: no idle nodes found, %d nodes active", total_count);
    }
    
    /* Restart the timer for next cleanup cycle (reduced from 60s to 30s) */
    ogs_timer_start(smf_self()->t_gtp_node_cleanup, ogs_time_from_sec(30));
}

void smf_timer_handover_hold(void *data)
{
    ogs_pool_id_t sess_id = (ogs_pool_id_t)(uintptr_t)data;
    smf_sess_t *sess = NULL;
    smf_sess_t *peer = NULL;
    smf_bearer_t *bearer = NULL;

    /*
     * Phase 4 PR3b (Step 4-3b): hold timer fired on a SOURCE_HOLD
     * (old-access, EUTRAN) session that the SMF was holding open
     * while waiting for a UE-initiated PDN Disconnect after a
     * VoLTE→VoWiFi handover. If we get here the UE has not torn the
     * old session down within `hold_duration_sec`, so the SMF
     * proactively releases the old access by sending a Delete Bearer
     * Request to the old SGW (TS 23.402 8.6.1.1, TS 29.274 7.2.9.2).
     *
     * Race guards (kept narrow on purpose - open5gs is single
     * threaded so the heavy lifting is done in smf_sess_remove() via
     * the cleanup order in context.c and the generation bump):
     *
     *   #5  re-fetch sess by pool id; if it has been freed since the
     *       timer was queued, bail out (dangling-pointer guard).
     *   #1  sess must still be in SOURCE_HOLD.
     *   #2  peer link must still point at us; if a fresh HI=1 came
     *       in and swapped the peer pointer to a different new
     *       session we abort to avoid stomping on it.
     *
     * Race guard #4 (per-arming generation snapshot) is intentionally
     * deferred; with the single-threaded event loop a stale firing
     * after ogs_timer_stop() would only happen if expire() and stop()
     * land in the same tick, in which case guards #1-#2 already
     * cover us.
     *
     * A former guard #3 (dbreq_xact_id != OGS_INVALID_POOL_ID) was
     * removed: the xact id was never assigned, so the guard was
     * structurally dead code. GTP-C retransmit semantics plus Phase 4
     * PR2 (CBResp failure handling) already cover the recovery path.
     */
    sess = smf_sess_find_by_id(sess_id);
    if (!sess) {
        ogs_warn("HO: hold timer expired but session id[%d] gone - "
                "abort (race guard #5)", sess_id);
        smf_metrics_inst_by_outcome_inc("no_op",
                SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);
        return;
    }

    if (sess->epc_handover.state != SMF_HO_STATE_SOURCE_HOLD) {
        ogs_info("HO: hold timer fired on sess[0x%x] but state=%d "
                "(not SOURCE_HOLD) - abort (race guard #1)",
                (int)sess->smf_n4_teid, sess->epc_handover.state);
        smf_metrics_inst_by_outcome_inc("no_op",
                SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);
        return;
    }

    if (sess->epc_handover.peer_sess_id == OGS_INVALID_POOL_ID) {
        ogs_info("HO: hold timer fired on sess[0x%x] but peer link "
                "cleared - abort (race guard #2)",
                (int)sess->smf_n4_teid);
        smf_metrics_inst_by_outcome_inc("no_op",
                SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);
        return;
    }
    peer = smf_sess_find_by_id(sess->epc_handover.peer_sess_id);
    if (!peer) {
        ogs_info("HO: hold timer fired on sess[0x%x] but peer "
                "session gone - abort (race guard #2)",
                (int)sess->smf_n4_teid);
        smf_metrics_inst_by_outcome_inc("no_op",
                SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);
        return;
    }
    if (peer->epc_handover.peer_sess_id != sess->id) {
        ogs_info("HO: hold timer fired on sess[0x%x] but peer's "
                "peer_sess_id no longer points back at us "
                "(new HI=1 in flight?) - abort (race guard #2)",
                (int)sess->smf_n4_teid);
        smf_metrics_inst_by_outcome_inc("no_op",
                SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);
        return;
    }

    /*
     * Feature flag: keep PR3b dormant in production until the
     * operator explicitly opts in. With the flag OFF we still want
     * the guards above to run so the no-op path is exercised in
     * regression tests (= a sanity check that Phase 1-3 are not
     * regressing).
     */
    if (!smf_self()->handover_config.enable_sgw_cleanup) {
        ogs_info("HO: hold timer expired (sess[0x%x], gen=%u) - "
                "active cleanup disabled by feature flag, no-op",
                (int)sess->smf_n4_teid,
                sess->epc_handover.hold_generation);
        smf_metrics_inst_by_outcome_inc("no_op",
                SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);
        return;
    }

    bearer = smf_default_bearer_in_sess(sess);
    if (!bearer) {
        ogs_error("HO: hold timer fired on sess[0x%x] but no default "
                "bearer - abort", (int)sess->smf_n4_teid);
        return;
    }

    /*
     * State transition: SOURCE_HOLD -> SOURCE_DBREQ_TX so that any
     * UE-initiated Delete Session arriving after we send DBReq is
     * handled as a duplicate by the existing idempotent path in
     * s5c-handler.c (returns OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND once
     * the bearer has been removed).
     */
    sess->epc_handover.state = SMF_HO_STATE_SOURCE_DBREQ_TX;

    ogs_warn("HO: hold timer fired - SMF-initiated cleanup of old "
            "LTE session [sess:0x%x, EBI:%d, gen:%u] via Delete "
            "Bearer Request to SGW",
            (int)sess->smf_n4_teid, bearer->ebi,
            sess->epc_handover.hold_generation);
    smf_metrics_inst_by_outcome_inc("expired",
            SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER);

    smf_gtp2_send_delete_bearer_request(
            bearer,
            OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
            OGS_GTP2_CAUSE_ACCESS_CHANGED_FROM_NON_3GPP_TO_3GPP);
}
