/*
 * Copyright (C) 2019-2023 by Sukchan Lee <acetcom@gmail.com>
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

#include "event.h"
#include "context.h"
#include "gtp-path.h"
#include "fd-path.h"
#include "metrics.h"
#include "s5c-build.h"
#include "s5c-handler.h"
#include "pfcp-path.h"

#include "ipfw/ipfw2.h"

static void pfcp_sess_timeout(ogs_pfcp_xact_t *xact, void *data)
{
    smf_sess_t *sess = NULL;
    ogs_pool_id_t sess_id = OGS_INVALID_POOL_ID;
    uint8_t type;

    ogs_assert(xact);
    type = xact->seq[0].type;

    ogs_assert(data);
    sess_id = OGS_POINTER_TO_UINT(data);
    ogs_assert(sess_id >= OGS_MIN_POOL_ID && sess_id <= OGS_MAX_POOL_ID);

    sess = smf_sess_find_by_id(sess_id);
    if (!sess) {
        ogs_error("Session has already been removed [%d]", type);
        return;
    }

    switch (type) {
    case OGS_PFCP_SESSION_ESTABLISHMENT_REQUEST_TYPE:
        ogs_error("No PFCP session establishment response");
        break;
    case OGS_PFCP_SESSION_MODIFICATION_REQUEST_TYPE:
        ogs_error("No PFCP session modification response");
        break;
    case OGS_PFCP_SESSION_DELETION_REQUEST_TYPE:
        ogs_error("No PFCP session deletion response");
        break;
    default:
        ogs_error("Not implemented [type:%d]", type);
        break;
    }
}

static ogs_inline uint32_t get_sender_f_teid(
        smf_sess_t *sess, ogs_gtp2_sender_f_teid_t *sender_f_teid)
{
    return sess ? sess->sgw_s5c_teid :
                sender_f_teid && sender_f_teid->teid_presence == true ?
                    sender_f_teid->teid : 0;
}

void smf_s5c_handle_echo_request(
        ogs_gtp_xact_t *xact, ogs_gtp2_echo_request_t *req)
{
    ogs_assert(xact);
    ogs_assert(req);

    ogs_debug("[PGW] Receiving Echo Request");
    /* FIXME : Before implementing recovery counter correctly,
     *         I'll re-use the recovery value in request message */
    ogs_gtp2_send_echo_response(xact, req->recovery.u8, 0);
}

void smf_s5c_handle_echo_response(
        ogs_gtp_xact_t *xact, ogs_gtp2_echo_response_t *req)
{
    /* Not Implemented */
}

uint8_t smf_s5c_handle_create_session_request(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_create_session_request_t *req)
{
    char buf1[OGS_ADDRSTRLEN];
    char buf2[OGS_ADDRSTRLEN];

    int i, rv;
    uint8_t cause_value = 0;

    ogs_gtp2_uli_t uli;

    smf_ue_t *smf_ue = NULL;

    ogs_gtp2_f_teid_t *sgw_s5c_teid, *sgw_s5u_teid;
    ogs_paa_t *paa = NULL;
    smf_bearer_t *bearer = NULL;
    ogs_gtp2_bearer_qos_t bearer_qos;
    ogs_gtp2_ambr_t *ambr = NULL;
    uint16_t decoded = 0;

    ogs_assert(sess);
    ogs_assert(xact);
    ogs_assert(req);

    ogs_debug("Create Session Request");

    cause_value = OGS_GTP2_CAUSE_REQUEST_ACCEPTED;

    if (req->imsi.presence == 0) {
        ogs_error("No IMSI");
        cause_value = OGS_GTP2_CAUSE_CONDITIONAL_IE_MISSING;
    }
    if (req->sender_f_teid_for_control_plane.presence == 0) {
        ogs_error("No TEID");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts_to_be_created[0].presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts_to_be_created[0].eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts_to_be_created[0].bearer_level_qos.presence == 0) {
        ogs_error("No EPS Bearer QoS");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->pdn_address_allocation.presence == 0) {
        ogs_error("No PAA");
        cause_value = OGS_GTP2_CAUSE_CONDITIONAL_IE_MISSING;
    }
    if (req->serving_network.presence == 0) {
        ogs_error("No Serving Network");
        cause_value = OGS_GTP2_CAUSE_CONDITIONAL_IE_MISSING;
    }
    if (req->serving_network.data == NULL) {
        ogs_error("No Data in Serving Network");
        cause_value = OGS_GTP2_CAUSE_CONDITIONAL_IE_MISSING;
    }
    if (req->serving_network.len != OGS_PLMN_ID_LEN) {
        ogs_error("Invalid Len[%d] in Serving Network",
                req->serving_network.len);
        cause_value = OGS_GTP2_CAUSE_CONDITIONAL_IE_MISSING;
    }

    if (!ogs_diam_is_relay_or_app_advertised(OGS_DIAM_GX_APPLICATION_ID)) {
        ogs_error("No Gx Diameter Peer");
        cause_value = OGS_GTP2_CAUSE_REMOTE_PEER_NOT_RESPONDING;
    }
    switch (sess->gtp_rat_type) {
    case OGS_GTP2_RAT_TYPE_EUTRAN:
        if (req->bearer_contexts_to_be_created[0].
                s5_s8_u_sgw_f_teid.presence == 0) {
            ogs_error("No S5/S8 SGW GTP-U TEID");
            cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        }
        if (req->user_location_information.presence == 0) {
            ogs_error("No UE Location Information");
            cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        }
        break;
    case OGS_GTP2_RAT_TYPE_WLAN:
        if (!ogs_diam_is_relay_or_app_advertised(OGS_DIAM_S6B_APPLICATION_ID)) {
            ogs_error("No S6b Diameter Peer");
            cause_value = OGS_GTP2_CAUSE_REMOTE_PEER_NOT_RESPONDING;
        }
        if (req->bearer_contexts_to_be_created[0].
                s2b_u_epdg_f_teid_5.presence == 0) {
            ogs_error("No S2b ePDG GTP-U TEID");
            cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        }
        break;
    default:
        ogs_error("Unknown RAT Type [%d]", req->rat_type.u8);
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED)
        return cause_value;

    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);

    /* Set MSISDN: */
    /* TS 29.274 sec 8.11, TS 29.002 ISDN-AddressString  */
    if (req->msisdn.presence == 1) {
        if (req->msisdn.len > sizeof(smf_ue->msisdn))  {
            ogs_error("MSISDN wrong size %u > %zu",
                req->msisdn.len, sizeof(smf_ue->msisdn));
            return OGS_GTP2_CAUSE_MANDATORY_IE_INCORRECT;
        }
        smf_ue->msisdn_len = req->msisdn.len;
        if (smf_ue->msisdn_len > 0) {
            memcpy(smf_ue->msisdn, req->msisdn.data, smf_ue->msisdn_len);
            ogs_buffer_to_bcd(
                smf_ue->msisdn, smf_ue->msisdn_len, smf_ue->msisdn_bcd);
        }
    }

    /* Set Selection Mode, TS 29.274 8.58 */
    if (req->selection_mode.presence == 1) {
        sess->gtp.selection_mode = req->selection_mode.u8 & 0x03;
        if (sess->gtp.selection_mode > 2)
            sess->gtp.selection_mode = 2;
    }

    if (sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_EUTRAN) {
        /* User Location Inforation is mandatory only for E-UTRAN */
        ogs_assert(req->user_location_information.presence);
        if (req->user_location_information.presence == 0) {
            ogs_error("No User Location Information(ULI)");
            return OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        }
        decoded = ogs_gtp2_parse_uli(&uli, &req->user_location_information);
        if (req->user_location_information.len != decoded) {
            ogs_error("Invalid User Location Information(ULI)");
            return OGS_GTP2_CAUSE_MANDATORY_IE_INCORRECT;
        }
        memcpy(&sess->e_tai, &uli.tai, sizeof(sess->e_tai));
        memcpy(&sess->e_cgi, &uli.e_cgi, sizeof(sess->e_cgi));

    }

    /* Serving Network */
    ogs_nas_to_plmn_id(&sess->serving_plmn_id, req->serving_network.data);

    /* Select PGW based on UE Location Information */
    smf_sess_select_upf(sess);

    /* Check if selected PGW is associated with SMF */
    ogs_assert(sess->pfcp_node);
    if (!OGS_FSM_CHECK(&sess->pfcp_node->sm, smf_pfcp_state_associated))
        return OGS_GTP2_CAUSE_REMOTE_PEER_NOT_RESPONDING;

    /* Handover Indication detection (bidirectional) */
    if (req->indication_flags.presence &&
        req->indication_flags.data && req->indication_flags.len) {
        ogs_gtp2_indication_t *indication = req->indication_flags.data;
        if (indication->handover_indication) {
            uint8_t peer_rat = 0;

            if (sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_WLAN) {
                peer_rat = OGS_GTP2_RAT_TYPE_EUTRAN;
            } else if (sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_EUTRAN) {
                peer_rat = OGS_GTP2_RAT_TYPE_WLAN;
            }

            if (peer_rat) {
                smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
                if (smf_ue) {
                    smf_sess_t *peer_sess = smf_sess_find_by_apn(
                            smf_ue, sess->session.name, peer_rat);
                    if (peer_sess) {
                        ogs_info("%s->%s HO detected: "
                                "linking [0x%x] <-> [0x%x]",
                                peer_rat == OGS_GTP2_RAT_TYPE_EUTRAN ?
                                    "VoLTE" : "VoWiFi",
                                sess->gtp_rat_type ==
                                    OGS_GTP2_RAT_TYPE_WLAN ?
                                    "VoWiFi" : "VoLTE",
                                (int)peer_sess->smf_n4_teid,
                                (int)sess->smf_n4_teid);

                        /* Set bidirectional peer link */
                        sess->epc_handover.peer_sess_id = peer_sess->id;
                        peer_sess->epc_handover.peer_sess_id = sess->id;

                        /* Set HO state */
                        sess->epc_handover.state =
                            SMF_HO_STATE_TARGET_ACTIVE;
                        peer_sess->epc_handover.state =
                            SMF_HO_STATE_SOURCE_HOLD;

                        /* KPI: record HO start. Direction is determined
                         * by the *target* side (sess), not the source. */
                        sess->epc_handover.kpi_start_us =
                                ogs_get_monotonic_time();
                        sess->epc_handover.kpi_direction =
                                (sess->gtp_rat_type ==
                                        OGS_GTP2_RAT_TYPE_WLAN) ?
                                    "volte_to_vowifi" :
                                    "vowifi_to_volte";
                        smf_metrics_inst_by_direction_inc(
                                sess->epc_handover.kpi_direction,
                                SMF_METR_BY_DIRECTION_CTR_HO_ATTEMPT);

                        /*
                         * The source-release hold timer is deliberately
                         * NOT armed here.
                         *
                         * Arming it at HO detection makes it a
                         * speculative kill switch: if the target then
                         * fails to come up - e.g. the PCRF never answers
                         * the CCR-U, for which there is no Gx timeout -
                         * the timer still fires and deletes the source
                         * session, destroying the very session the UE
                         * would otherwise have fallen back to. Observed
                         * in production: CCR-U at T+0.1s, timer fired at
                         * T+5.0s, old LTE session removed, and the UE
                         * ended up re-attaching with a different IP (no
                         * IP preservation, no voice bearer) because the
                         * retransmitted CSReq no longer found a peer.
                         *
                         * Releasing the old access is only correct once
                         * the target has actually been accepted, so the
                         * timer is armed on the success edge instead -
                         * right after smf_gtp2_send_create_session_
                         * response() succeeds, next to the existing
                         * peer PFCP deactivation in gsm-sm.c. That turns
                         * it into a post-success old-access release.
                         *
                         * Still only for a non-3GPP target (VoLTE→VoWiFi):
                         * VoWiFi→VoLTE has its own cleanup path via
                         * MBR(HI=1) on S5/S8 (Phase 1 deferred DBReq ->
                         * ePDG). The actual Delete Bearer Request remains
                         * behind the `enable_sgw_cleanup` feature flag.
                         */

                        /* Transfer Gx session ownership */
                        if (peer_sess->gx_sid) {
                            /*
                             * Ownership move, so anything already held here
                             * has to be released first. The target is a
                             * freshly created session and normally has none,
                             * but overwriting without freeing would leak the
                             * string now that each session owns its copy.
                             */
                            if (sess->gx_sid)
                                ogs_free(sess->gx_sid);
                            sess->gx_sid = peer_sess->gx_sid;
                            peer_sess->gx_sid = NULL;
                            ogs_info("Gx session transferred: "
                                    "[%s]", sess->gx_sid);

                            /*
                             * The freeDiameter sess_state.sess_id back-
                             * reference is NOT rewritten here. It is
                             * refreshed by smf_gx_send_ccr() (which sets
                             * sess_data->sess_id unconditionally for every
                             * CC-Request-Type), and until that CCR-U goes
                             * out an incoming RAR follows the peer link in
                             * smf_gx_rar_cb() instead. Doing the rewrite
                             * from this (main) thread would require a
                             * fd_sess_state_retrieve()/store() pair that
                             * races the freeDiameter dispatch threads, and
                             * fd_sess_fromsid_msg() would leak a session
                             * msg reference on every handover.
                             */
                        }

                        /* Restore session info from peer */
                        /* QoS: always copy from peer for HO */
                        memcpy(&sess->session.qos,
                                &peer_sess->session.qos,
                                sizeof(sess->session.qos));
                        ogs_info("HO: QoS restored (QCI=%d)",
                                sess->session.qos.index);

                        /* Copy all dedicated PCC rules from peer
                         * session's policy for bearer_binding() to
                         * re-establish via CBReq after PFCP setup
                         * (TS 23.402). Iterate policy.pcc_rule[]
                         * directly so multiple rules bound to the same
                         * dedicated bearer (e.g. RTP + RTCP) are all
                         * copied. Default-bearer rules are excluded by
                         * QCI comparison (Phase 3 interim - TS 23.203
                         * 6.3.1 full QCI+ARP binding deferred). */
                        {
                            smf_bearer_t *peer_default;
                            uint8_t default_qci;
                            int pi;
                            int copied = 0;

                            peer_default =
                                smf_default_bearer_in_sess(peer_sess);
                            default_qci = peer_default ?
                                peer_default->qos.index :
                                peer_sess->session.qos.index;

                            /* Phase 4.2b #15 detector: Open5GS does not
                             * support multi-dedicated-bearer per session.
                             * If the source session has >1 dedicated
                             * bearer (e.g. call-waiting created a 2nd
                             * bearer via RAR overwrite side-effect), HO
                             * will lose bearer #2+ because policy.
                             * pcc_rule[] reflects only one bearer's
                             * rules. Pure observation: no behavior
                             * change to the HO copy below. */
                            if (peer_default) {
                                int dedicated_count = 0;
                                smf_bearer_t *db = NULL;
                                ogs_list_for_each(
                                        &peer_sess->bearer_list, db) {
                                    if (db != peer_default)
                                        dedicated_count++;
                                }
                                if (dedicated_count > 1) {
                                    ogs_warn("[Phase 4.2b #15 detector] "
                                            "HO source sess has %d "
                                            "dedicated bearers but "
                                            "Open5GS supports only 1 "
                                            "per session; bearer #2+ "
                                            "will be lost on target "
                                            "(call-waiting / multi-"
                                            "stream HO drop expected, "
                                            "future-work.md section "
                                            "2.2 issue 15)",
                                            dedicated_count);
                                    smf_metrics_inst_by_direction_inc(
                                        sess->epc_handover.kpi_direction,
                                        SMF_METR_BY_DIRECTION_CTR_HO_MULTI_DEDICATED_BEARER_LOSS);
                                }
                            }

                            for (pi = 0;
                                 pi < peer_sess->policy.num_of_pcc_rule;
                                 pi++) {
                                ogs_pcc_rule_t *src_rule =
                                    &peer_sess->policy.pcc_rule[pi];

                                if (src_rule->type !=
                                        OGS_PCC_RULE_TYPE_INSTALL)
                                    continue;
                                if (!src_rule->name)
                                    continue;
                                /* Skip default-bearer rules */
                                if (src_rule->qos.index == default_qci)
                                    continue;

                                if (sess->policy.num_of_pcc_rule >=
                                        OGS_MAX_NUM_OF_PCC_RULE) {
                                    ogs_warn("HO: PCC rule copy limit "
                                            "reached");
                                    break;
                                }

                                OGS_STORE_PCC_RULE(
                                    &sess->policy.pcc_rule[
                                        sess->policy.num_of_pcc_rule],
                                    src_rule);
                                /* Ensure type is INSTALL for
                                 * bearer_binding() */
                                sess->policy.pcc_rule[
                                    sess->policy.num_of_pcc_rule].type =
                                        OGS_PCC_RULE_TYPE_INSTALL;
                                sess->policy.num_of_pcc_rule++;
                                copied++;

                                ogs_info("HO: Copied PCC rule [%s] "
                                        "QCI=%d (%d flows) from peer",
                                        src_rule->name,
                                        src_rule->qos.index,
                                        src_rule->num_of_flow);
                            }

                            if (copied > 0) {
                                ogs_info("HO: %d dedicated PCC rule(s) "
                                        "copied for CBReq "
                                        "re-establishment", copied);
                            }
                        }

                        /* PAA=0: Restore IP from peer session.
                         * Use peer_sess->ipv4->addr (actual allocated IP),
                         * not peer_sess->session.ue_ip (may be 0 if
                         * original PAA was also 0). */
                        if (req->pdn_address_allocation.presence) {
                            ogs_paa_t *check_paa =
                                req->pdn_address_allocation.data;
                            if (check_paa && peer_sess->ipv4 &&
                                check_paa->addr == 0) {
                                char buf[OGS_ADDRSTRLEN];
                                sess->session.session_type =
                                    peer_sess->session.session_type;
                                sess->session.ue_ip.addr =
                                    peer_sess->ipv4->addr[0];
                                sess->session.ue_ip.ipv4 = 1;
                                ogs_info("HO: PAA=0, restoring IP [%s] "
                                        "from peer session",
                                        OGS_INET_NTOP(
                                            &sess->session.ue_ip.addr,
                                            buf));
                            }
                        }
                    } else {
                        ogs_warn("HO: peer session not found "
                                "for APN[%s] RAT[%d]",
                                sess->session.name, peer_rat);
                    }
                }
            }
        }
    }

    /* UE IP Address */
    paa = req->pdn_address_allocation.data;
    ogs_assert(paa);

    if (sess->epc_handover.state == SMF_HO_STATE_TARGET_ACTIVE &&
        sess->session.ue_ip.addr) {
        /* HO with PAA=0: IP already restored from peer session.
         * Use restored session_type and ue_ip.
         * smf_sess_set_ue_ip() will allocate via static IP path. */
        sess->ue_session_type = sess->session.session_type;
    } else {
        /* Store UE Session Type (IPv4, IPv6, IPv4v6) */
        sess->ue_session_type = paa->session_type;

        /* Initially Set Session Type from UE */
        sess->session.session_type = sess->ue_session_type;
        rv = ogs_paa_to_ip(paa, &sess->session.ue_ip);
        ogs_assert(rv == OGS_OK);
    }

    /* Set UE IP Address */
    rv = smf_sess_set_ue_ip(sess);
    if (rv != OGS_PFCP_CAUSE_REQUEST_ACCEPTED) {
        /* only two possibilities are:
         * OGS_PFCP_CAUSE_ALL_DYNAMIC_ADDRESS_ARE_OCCUPIED
         * OGS_PFCP_CAUSE_NO_RESOURCES_AVAILABLE
         */
        ogs_error("Failed to set UE IP Address");
        switch(rv) {
        case OGS_PFCP_CAUSE_ALL_DYNAMIC_ADDRESS_ARE_OCCUPIED:
            cause_value = OGS_GTP2_CAUSE_ALL_DYNAMIC_ADDRESSES_ARE_OCCUPIED;
            break;
        case OGS_PFCP_CAUSE_NO_RESOURCES_AVAILABLE:
            cause_value = OGS_GTP2_CAUSE_NO_RESOURCES_AVAILABLE;
            break;
        default:
            cause_value = OGS_GTP2_CAUSE_REQUEST_REJECTED_REASON_NOT_SPECIFIED;
            break;
        }
        return cause_value;
    }

    ogs_info("UE IMSI[%s] APN[%s] IPv4[%s] IPv6[%s]",
        smf_ue->imsi_bcd,
        sess->session.name,
        sess->ipv4 ? OGS_INET_NTOP(&sess->ipv4->addr, buf1) : "",
        sess->ipv6 ? OGS_INET6_NTOP(&sess->ipv6->addr, buf2) : "");

    /* Control Plane(DL) : SGW-S5C */
    sgw_s5c_teid = req->sender_f_teid_for_control_plane.data;
    ogs_assert(sgw_s5c_teid);
    /* sess->sgw_s5c_teid has already been updated in SMF-SM */
    ogs_assert(sess->sgw_s5c_teid == be32toh(sgw_s5c_teid->teid));
    rv = ogs_gtp2_f_teid_to_ip(sgw_s5c_teid, &sess->sgw_s5c_ip);
    ogs_assert(rv == OGS_OK);

    ogs_debug("    SGW_S5C_TEID[0x%x] SMF_N4_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    /* Remove all previous bearer */
    smf_bearer_remove_all(sess);

    /* Setup Bearer */
    for (i = 0; i < OGS_BEARER_PER_UE; i++) {
        if (req->bearer_contexts_to_be_created[i].presence == 0)
            break;
        if (req->bearer_contexts_to_be_created[i].eps_bearer_id.presence == 0) {
            ogs_error("No EPS Bearer ID");
            break;
        }
        if (req->bearer_contexts_to_be_created[i].
                bearer_level_qos.presence == 0) {
            ogs_error("No Bearer QoS");
            break;
        }

        decoded = ogs_gtp2_parse_bearer_qos(&bearer_qos,
                &req->bearer_contexts_to_be_created[i].bearer_level_qos);
        ogs_assert(decoded ==
                req->bearer_contexts_to_be_created[i].bearer_level_qos.len);

        bearer = smf_bearer_add(sess);
        if (!bearer) {
            ogs_error("smf_bearer_add() failed (PFCP resource exhausted) "
                    "in Create Session Request");
            return OGS_GTP2_CAUSE_NO_RESOURCES_AVAILABLE;
        }

        /* Set Bearer EBI */
        bearer->ebi = req->bearer_contexts_to_be_created[i].eps_bearer_id.u8;

        switch (sess->gtp_rat_type) {
        case OGS_GTP2_RAT_TYPE_EUTRAN:
            sgw_s5u_teid = req->bearer_contexts_to_be_created[i].
                s5_s8_u_sgw_f_teid.data;
            ogs_assert(sgw_s5u_teid);
            bearer->sgw_s5u_teid = be32toh(sgw_s5u_teid->teid);
            rv = ogs_gtp2_f_teid_to_ip(sgw_s5u_teid, &bearer->sgw_s5u_ip);
            ogs_assert(rv == OGS_OK);

            break;
        case OGS_GTP2_RAT_TYPE_WLAN:
            sgw_s5u_teid = req->bearer_contexts_to_be_created[i].
                s2b_u_epdg_f_teid_5.data;
            ogs_assert(sgw_s5u_teid);
            bearer->sgw_s5u_teid = be32toh(sgw_s5u_teid->teid);
            rv = ogs_gtp2_f_teid_to_ip(sgw_s5u_teid, &bearer->sgw_s5u_ip);
            ogs_assert(rv == OGS_OK);
            break;
        default:
            ogs_error("Unknown RAT Type [%d]", sess->gtp_rat_type);
            ogs_assert_if_reached();
        }

        ogs_debug("    SGW_S5U_TEID[0x%x] PGW_S5U_TEID[0x%x]",
                bearer->sgw_s5u_teid, bearer->pgw_s5u_teid);

        /* Set Session QoS from Default Bearer Level QoS */
        if (i == 0) {
            sess->session.qos.index = bearer_qos.qci;
            sess->session.qos.arp.priority_level = bearer_qos.priority_level;
            sess->session.qos.arp.pre_emption_capability =
                            bearer_qos.pre_emption_capability;
            sess->session.qos.arp.pre_emption_vulnerability =
                            bearer_qos.pre_emption_vulnerability;
        }
    }

    /* Set AMBR if available */
    if (req->aggregate_maximum_bit_rate.presence) {
        /*
         * Ch 8.7. Aggregate Maximum Bit Rate(AMBR) in TS 29.274 V15.9.0
         *
         * AMBR is defined in clause 9.9.4.2 of 3GPP TS 24.301 [23],
         * but it shall be encoded as shown in Figure 8.7-1 as
         * Unsigned32 binary integer values in kbps (1000 bits per second).
         */
        ambr = req->aggregate_maximum_bit_rate.data;
        sess->session.ambr.downlink = be32toh(ambr->downlink) * 1000;
        sess->session.ambr.uplink = be32toh(ambr->uplink) * 1000;
    }

    /* ePCO */
    if (req->extended_protocol_configuration_options.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.ue_epco,
                &req->extended_protocol_configuration_options);
        
        /* Check for MS support of Local address in TFT indicator PCO */
        ogs_pco_t pco;
        if (ogs_pco_parse(&pco, (unsigned char *)req->extended_protocol_configuration_options.data, 
                req->extended_protocol_configuration_options.len) > 0) {
            for (int i = 0; i < pco.num_of_id; i++) {
                if (pco.ids[i].id == OGS_PCO_ID_MS_SUPPORT_LOCAL_ADDR_TFT_INDICATOR) {
                    /* PCO ID 0x0011 actually indicates UE does NOT support local address in TFT */
                    sess->gtp.ue_local_addr_in_tft = false;
                    ogs_info("UE explicitly indicates NO support for local address in TFT in ePCO");
                    break;
                }
            }
        }

    /* PCO */
    } else if (req->protocol_configuration_options.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.ue_pco,
                &req->protocol_configuration_options);
                
        /* Check for MS support of Local address in TFT indicator PCO */
        ogs_pco_t pco;
        if (ogs_pco_parse(&pco, (unsigned char *)req->protocol_configuration_options.data,
                req->protocol_configuration_options.len) > 0) {
            for (int i = 0; i < pco.num_of_id; i++) {
                if (pco.ids[i].id == OGS_PCO_ID_MS_SUPPORT_LOCAL_ADDR_TFT_INDICATOR) {
                    /* PCO ID 0x0011 actually indicates UE does NOT support local address in TFT */
                    sess->gtp.ue_local_addr_in_tft = false;
                    ogs_info("UE explicitly indicates NO support for local address in TFT in PCO");
                    break;
                }
            }
        }
    }

    /* APCO */
    if (req->additional_protocol_configuration_options.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.ue_apco,
                &req->additional_protocol_configuration_options);
    }

    /* Set User Location Information */
    if (req->user_location_information.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.user_location_information,
                &req->user_location_information);
    }

    /* Set UE Timezone */
    if (req->ue_time_zone.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.ue_timezone, &req->ue_time_zone);
    }

    if (req->charging_characteristics.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.charging_characteristics, &req->charging_characteristics);
    }

    /* Set MSISDN */
    if (req->msisdn.presence && req->msisdn.len && req->msisdn.data) {
        smf_ue->msisdn_len = ogs_min(req->msisdn.len, OGS_MAX_MSISDN_LEN);
        memcpy(smf_ue->msisdn, req->msisdn.data, smf_ue->msisdn_len);
        ogs_buffer_to_bcd(smf_ue->msisdn,
                smf_ue->msisdn_len, smf_ue->msisdn_bcd);
    }

    /* Set IMEI(SV) */
    if (req->me_identity.presence && req->me_identity.len > 0) {
        smf_ue->imeisv_len = ogs_min(req->me_identity.len, OGS_MAX_IMEISV_LEN);
        memcpy(smf_ue->imeisv,
            (uint8_t*)req->me_identity.data, smf_ue->imeisv_len);
        ogs_buffer_to_bcd(
            smf_ue->imeisv, smf_ue->imeisv_len, smf_ue->imeisv_bcd);
    }

    return OGS_GTP2_CAUSE_REQUEST_ACCEPTED;
}

uint8_t smf_s5c_handle_delete_session_request(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_delete_session_request_t *req)
{
    ogs_debug("Delete Session Request");

    ogs_assert(xact);
    ogs_assert(req);

    /*
     * Phase 4 PR3a (Step 4-3a):
     * If the UE-initiated PDN Disconnect arrived in time for the
     * SOURCE_HOLD session, cancel the source-release hold timer so
     * that the active cleanup path (PR3b) does not duplicate work
     * after the regular delete completes. Generation is bumped to
     * invalidate any callback that may already be queued.
     */
    if (sess->epc_handover.state == SMF_HO_STATE_SOURCE_HOLD &&
        sess->epc_handover.hold_timer) {
        ogs_info("HO: hold timer cancelled by UE Delete Session "
                "(gen=%u -> %u)",
                sess->epc_handover.hold_generation,
                sess->epc_handover.hold_generation + 1);
        ogs_timer_stop(sess->epc_handover.hold_timer);
        sess->epc_handover.hold_generation++;
    }

    /* Step 1-1: Protect new WLAN session during HO.
     * If this session is TARGET_ACTIVE, the Delete Session is meant for
     * the old EUTRAN session. Accept it but skip actual deletion. */
    if (sess->epc_handover.state == SMF_HO_STATE_TARGET_ACTIVE) {
        ogs_warn("Delete Session Request received for TARGET_ACTIVE "
                "session - protecting VoWiFi session");
        return OGS_GTP2_CAUSE_REQUEST_ACCEPTED;
    }

    if (!ogs_diam_is_relay_or_app_advertised(OGS_DIAM_GX_APPLICATION_ID)) {
        ogs_error("No Gx Diameter Peer");
        return OGS_GTP2_CAUSE_REMOTE_PEER_NOT_RESPONDING;
    }

    if (sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_WLAN) {
        if (!ogs_diam_is_relay_or_app_advertised(OGS_DIAM_S6B_APPLICATION_ID)) {
            ogs_error("No S6b Diameter Peer");
            return OGS_GTP2_CAUSE_REMOTE_PEER_NOT_RESPONDING;
        }
    }

    /* PCO
     * 3GPP TS 29.274 version 10.5.0, Table 7.2.9.1-1
     * If the UE includes the PCO IE, then the MME/SGSN shall copy
     * the content of this IE transparently from the PCO IE included by the UE.
     * If SGW receives the PCO IE, SGW shall forward it to PGW.
     */
    if (req->protocol_configuration_options.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.ue_pco,
                &req->protocol_configuration_options);
    } else {
        /*
         * Clear contents to reflect whether PCO IE was included or not as part
         * of session deletion procedure
         */
        OGS_TLV_CLEAR_DATA(&sess->gtp.ue_pco);
    }

    /* APCO not present in Session deletion procedure, hence no need to clear it here. */

    if (req->extended_protocol_configuration_options.presence) {
        OGS_TLV_STORE_DATA(&sess->gtp.ue_epco,
                &req->extended_protocol_configuration_options);
    } else {
        /*
         * Clear contents to reflect whether PCO IE was included or not as part
         * of session deletion procedure
         */
        OGS_TLV_CLEAR_DATA(&sess->gtp.ue_epco);
    }

    ogs_debug("    SGW_S5C_TEID[0x%x] SMF_N4_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    return OGS_GTP2_CAUSE_REQUEST_ACCEPTED;
}

void smf_s5c_handle_modify_bearer_request(
        smf_sess_t *sess, ogs_gtp_xact_t *gtp_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp2_modify_bearer_request_t *req,
        ogs_gtp2_sender_f_teid_t *sender_f_teid)
{
    int rv, i;
    uint8_t cause_value = 0;
    uint64_t flags = 0;

    smf_ue_t *smf_ue = NULL;
    smf_bearer_t *bearer = NULL;

    ogs_debug("Modify Bearer Request");

    ogs_assert(gtp_xact);
    ogs_assert(req);
    ogs_assert(sender_f_teid);

    /************************
     * Check Session Context
     ************************/
    cause_value = OGS_GTP2_CAUSE_REQUEST_ACCEPTED;

    if (!sess) {
        ogs_error("No Context");
        cause_value = OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp2_send_error_message(
                gtp_xact, get_sender_f_teid(sess, sender_f_teid),
                OGS_GTP2_MODIFY_BEARER_RESPONSE_TYPE, cause_value);
        return;
    }

    /********************
     * Check ALL Context
     ********************/
    ogs_assert(sess);
    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);

    /* Control Plane(DL) : SGW-S5C */
    if (req->sender_f_teid_for_control_plane.presence == 1) {
        ogs_gtp2_f_teid_t *sgw_s5c_teid =
            req->sender_f_teid_for_control_plane.data;
        ogs_assert(sgw_s5c_teid);
        sess->sgw_s5c_teid = be32toh(sgw_s5c_teid->teid);
        rv = ogs_gtp2_f_teid_to_ip(sgw_s5c_teid, &sess->sgw_s5c_ip);
        ogs_assert(rv == OGS_OK);

        OGS_SETUP_GTP_NODE(sess, gtp_xact->gnode);

        ogs_debug("    SGW_S5C_TEID[0x%x] SMF_N4_TEID[0x%x]",
                sess->sgw_s5c_teid, sess->smf_n4_teid);
    }

    /* Check Modify Bearer */
    ogs_list_init(&sess->qos_flow_to_modify_list);

    for (i = 0; i < OGS_BEARER_PER_UE; i++) {
        if (req->bearer_contexts_to_be_modified[i].presence == 0)
            break;
        if (req->bearer_contexts_to_be_modified[i].
                eps_bearer_id.presence == 0) {
            ogs_error("No EPS Bearer ID");
            break;
        }
        /* EPS Bearer ID */
        bearer = smf_bearer_find_by_ebi(sess,
                    req->bearer_contexts_to_be_modified[i].eps_bearer_id.u8);
        if (!bearer) {
            ogs_error("No Bearer Context");
            break;
        }

        if (req->bearer_contexts_to_be_modified[i].s4_u_sgsn_f_teid.presence) {
            ogs_pfcp_far_t *far = NULL;
            ogs_gtp2_f_teid_t *sgw_s5u_teid = NULL;

            ogs_ip_t remote_ip;
            ogs_ip_t zero_ip;

            /* Data Plane(DL) : SGW-S5U */
            sgw_s5u_teid = req->bearer_contexts_to_be_modified[i].
                            s4_u_sgsn_f_teid.data;
            ogs_assert(sgw_s5u_teid);
            bearer->sgw_s5u_teid = be32toh(sgw_s5u_teid->teid);
            ogs_assert(OGS_OK ==
                    ogs_gtp2_f_teid_to_ip(sgw_s5u_teid, &remote_ip));

            memset(&zero_ip, 0, sizeof(ogs_ip_t));
            if (memcmp(&bearer->sgw_s5u_ip, &zero_ip, sizeof(ogs_ip_t)) != 0 &&
                memcmp(&bearer->sgw_s5u_ip,
                    &remote_ip, sizeof(ogs_ip_t)) != 0) {

                ogs_assert(sess->pfcp_node);

                /* eNB IP is changed during handover */
                if (sess->pfcp_node->up_function_features.empu) {
                    flags |= OGS_PFCP_MODIFY_END_MARKER;
                } else {
                    ogs_error("UPF does not support End Marker");
                }
            }

            memcpy(&bearer->sgw_s5u_ip, &remote_ip, sizeof(ogs_ip_t));

            far = bearer->dl_far;
            ogs_assert(far);

            far->apply_action = OGS_PFCP_APPLY_ACTION_FORW;

            ogs_assert(OGS_OK ==
                ogs_pfcp_ip_to_outer_header_creation(
                    &bearer->sgw_s5u_ip,
                    &far->outer_header_creation,
                    &far->outer_header_creation_len));
            far->outer_header_creation.teid = bearer->sgw_s5u_teid;

            ogs_list_add(&sess->qos_flow_to_modify_list,
                            &bearer->to_modify_node);

            ogs_debug("    SGW_S5U_TEID[0x%x] PGW_S5U_TEID[0x%x]",
                    bearer->sgw_s5u_teid, bearer->pgw_s5u_teid);
        }
    }

    if (ogs_list_count(&sess->qos_flow_to_modify_list)) {
        ogs_pfcp_xact_t *pfcp_xact = NULL;

        /* Need to modify SGW-S5U */
        pfcp_xact = ogs_pfcp_xact_local_create(
                        sess->pfcp_node, pfcp_sess_timeout, sess);
        ogs_assert(pfcp_xact);

        pfcp_xact->epc = true; /* EPC PFCP transaction */
        pfcp_xact->assoc_xact_id = gtp_xact->id;
        pfcp_xact->modify_flags =
            flags|OGS_PFCP_MODIFY_DL_ONLY|OGS_PFCP_MODIFY_ACTIVATE;

        pfcp_xact->gtp_pti = OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED;
        pfcp_xact->gtp_cause = OGS_GTP2_CAUSE_UNDEFINED_VALUE;
        pfcp_xact->local_seid = sess->smf_n4_seid;

        ogs_assert(gtpbuf);
        pfcp_xact->gtpbuf = ogs_pkbuf_copy(gtpbuf);
        ogs_assert(pfcp_xact->gtpbuf);

        ogs_assert(OGS_OK == smf_pfcp_send_modify_list(
                sess, smf_n4_build_qos_flow_to_modify_list, pfcp_xact, 0));

    } else {

        /* Check if Handover from Non-3GPP to 3GPP */
        ogs_gtp2_indication_t *indication = NULL;

        ogs_assert(OGS_OK ==
            smf_gtp2_send_modify_bearer_response(sess, gtp_xact, req, false));

        if (req->indication_flags.presence &&
            req->indication_flags.data && req->indication_flags.len) {
            indication = req->indication_flags.data;
        }

        if (indication && indication->handover_indication) {
            /* KPI: VoWiFi -> VoLTE HO control-plane success on the SMF
             * side. The TARGET (EUTRAN) session received an MBR with
             * the Handover Indication, mirroring the MME-side
             * epc_mme_handover_success_total. */
            if (sess->epc_handover.kpi_start_us &&
                    sess->epc_handover.kpi_direction) {
                ogs_time_t elapsed_us = ogs_get_monotonic_time() -
                        sess->epc_handover.kpi_start_us;
                smf_metrics_inst_by_direction_inc(
                        sess->epc_handover.kpi_direction,
                        SMF_METR_BY_DIRECTION_CTR_HO_SUCCESS);
                smf_metrics_inst_histogram_observe(
                        sess->epc_handover.kpi_direction,
                        SMF_METR_HISTOGRAM_HO_DURATION,
                        (int)(elapsed_us / 1000));
                sess->epc_handover.kpi_start_us = 0;
            }
            ogs_assert(OGS_OK == smf_epc_pfcp_send_deactivation(sess,
                    OGS_GTP2_CAUSE_ACCESS_CHANGED_FROM_NON_3GPP_TO_3GPP));

            /* Send Delete Bearer Request to ePDG to release
             * the old WLAN session.
             * If CBReq is pending (dedicated bearer being created),
             * defer until CBResp to avoid deleting the old voice
             * bearer before the new one is established (TS 23.402
             * 8.6.1.1: old access release is the final step). */
            if (sess->epc_handover.peer_sess_id !=
                    OGS_INVALID_POOL_ID) {
                smf_bearer_t *b;
                bool has_pending_cbr = false;
                int pending_cbr_count = 0; /* Phase 4.2b #13 detector */

                ogs_list_for_each(&sess->bearer_list, b) {
                    if (b->create_pending) {
                        has_pending_cbr = true;
                        pending_cbr_count++;
                    }
                }

                if (has_pending_cbr) {
                    /*
                     * Phase 4.2b #13 detector: the deferred_
                     * deactivation flag is single-bool, so the first
                     * CBResp fires the DBReq to ePDG regardless of
                     * how many CBReq remain pending. This is safe for
                     * single-dedicated-bearer UEs (the only case
                     * observed in our test env) but breaks with
                     * multi-dedicated-bearer UEs (e.g. voice QCI=1 +
                     * IMS sig QCI=5, video QCI=2) because parallel
                     * CBResps would prematurely release the old WLAN
                     * session. Phase 5 will replace the bool with a
                     * pending-count or EBI-set; until then, warn so
                     * the misfire condition is logged on appearance.
                     */
                    if (pending_cbr_count > 1) {
                        ogs_warn("[Phase 4.2b #13 detector] MBR(HI=1) "
                                "with %d pending CBReq during HO - "
                                "deferred_deactivation single-bool "
                                "granularity may misfire on first "
                                "CBResp (TS 23.402, future-work.md "
                                "§2 #13)", pending_cbr_count);
                    }
                    ogs_info("MBR(HI=1): CBReq pending, deferring "
                            "DBReq to ePDG until CBResp");
                    sess->epc_handover.deferred_deactivation = true;
                    smf_metrics_inst_global_inc(
                        SMF_METR_GLOB_GAUGE_DEFERRED_DEACTIVATION_PENDING);
                } else {
                    smf_sess_t *wlan_sess = smf_sess_find_by_id(
                            sess->epc_handover.peer_sess_id);
                    if (wlan_sess) {
                        smf_bearer_t *linked_bearer =
                            ogs_list_first(&wlan_sess->bearer_list);
                        if (linked_bearer) {
                            ogs_info("MBR(HI=1): sending DBReq to ePDG "
                                    "for old WLAN session");
                            smf_gtp2_send_delete_bearer_request(
                                    linked_bearer,
                                OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
                                OGS_GTP2_CAUSE_ACCESS_CHANGED_FROM_NON_3GPP_TO_3GPP);
                        }
                    }
                }
            }
        }
    }
}

/* Phase 4 Step 4-5: Helper to execute deferred DBReq->ePDG.
 *
 * The Phase 3 C feature defers the DBReq to ePDG until CBResp arrives.
 * When CBResp is a failure (e.g. eNB invalid-qos-combination), the SMF
 * still needs to flush this deferred work so the old WLAN session is not
 * leaked. This helper is called on every CBResp return path (success and
 * failure) so the deferred DBReq is not stuck. */
static void smf_ho_flush_deferred_deactivation(smf_sess_t *sess)
{
    smf_sess_t *wlan_sess = NULL;
    smf_bearer_t *linked_bearer = NULL;

    if (!sess->epc_handover.deferred_deactivation)
        return;

    sess->epc_handover.deferred_deactivation = false;
    smf_metrics_inst_global_dec(
            SMF_METR_GLOB_GAUGE_DEFERRED_DEACTIVATION_PENDING);

    if (sess->epc_handover.peer_sess_id == OGS_INVALID_POOL_ID)
        return;

    wlan_sess = smf_sess_find_by_id(sess->epc_handover.peer_sess_id);
    if (!wlan_sess)
        return;

    linked_bearer = ogs_list_first(&wlan_sess->bearer_list);
    if (!linked_bearer)
        return;

    ogs_info("HO: CBResp processed, sending deferred DBReq to ePDG");
    smf_gtp2_send_delete_bearer_request(
            linked_bearer,
            OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
            OGS_GTP2_CAUSE_ACCESS_CHANGED_FROM_NON_3GPP_TO_3GPP);
}

void smf_s5c_handle_create_bearer_response(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_create_bearer_response_t *rsp)
{
    int rv;
    uint8_t cause_value;
    ogs_gtp2_cause_t *cause = NULL;
    ogs_gtp2_f_teid_t *sgw_s5u_teid = NULL, *pgw_s5u_teid = NULL;
    smf_bearer_t *bearer = NULL;
    ogs_pool_id_t bearer_id = OGS_INVALID_POOL_ID;
    ogs_pfcp_far_t *dl_far = NULL;

    ogs_assert(sess);
    ogs_assert(rsp);

    ogs_debug("Create Bearer Response");

    /********************
     * Check Transaction
     ********************/
    ogs_assert(xact);

    bearer_id = OGS_POINTER_TO_UINT(xact->data);
    ogs_assert(bearer_id >= OGS_MIN_POOL_ID && bearer_id <= OGS_MAX_POOL_ID);

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);

    /********************
     * Check ALL Context
     ********************/
    bearer = smf_bearer_find_by_id(bearer_id);
    if (!bearer) {
        ogs_error("Bearer has already been removed");
        return;
    }

    /*****************************************
     * Check Mandatory/Conditional IE Missing
     *
     * Phase 4 Step 4-5 (Codex-validated): TEIDs in Bearer Context are
     * Conditional (TS 29.274 7.2.4) - required only when bearer-level
     * Cause is "Request accepted". On failure (e.g. eNB
     * invalid-qos-combination), MME omits TEIDs and includes only
     * EBI + Cause. The bearer is already located via xact->data above.
     *****************************************/
    cause_value = OGS_GTP2_CAUSE_REQUEST_ACCEPTED;

    if (rsp->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->cause.presence == 0) {
        ogs_error("No Cause in Create Bearer Response");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    } else {
        cause = rsp->cause.data;
        ogs_info("Create Bearer Response - Main Cause: %d", cause->value);
    }
    if (rsp->bearer_contexts.cause.presence == 0) {
        ogs_error("No Bearer Cause in Create Bearer Response");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    } else {
        ogs_gtp2_cause_t *bearer_cause = rsp->bearer_contexts.cause.data;
        ogs_info("Create Bearer Response - Bearer Cause: %d", bearer_cause->value);
    }

    /* Reject early on missing mandatory IEs */
    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_assert(OGS_OK ==
            smf_epc_pfcp_send_one_bearer_modification_request(
                bearer, OGS_INVALID_POOL_ID, OGS_PFCP_MODIFY_REMOVE,
                OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
                OGS_GTP2_CAUSE_UNDEFINED_VALUE));
        smf_ho_flush_deferred_deactivation(sess);
        return;
    }

    /********************
     * Check Cause Value
     ********************/
    cause = rsp->bearer_contexts.cause.data;
    ogs_assert(cause);
    cause_value = cause->value;
    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_warn("GTP Bearer Cause [VALUE:%d] - cleaning up new bearer "
                "and flushing any deferred HO deactivation",
                cause_value);
        ogs_assert(OGS_OK ==
            smf_epc_pfcp_send_one_bearer_modification_request(
                bearer, OGS_INVALID_POOL_ID, OGS_PFCP_MODIFY_REMOVE,
                OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
                OGS_GTP2_CAUSE_UNDEFINED_VALUE));
        smf_ho_flush_deferred_deactivation(sess);
        return;
    }

    cause = rsp->cause.data;
    ogs_assert(cause);
    cause_value = cause->value;
    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_warn("GTP Cause [Value:%d] - cleaning up new bearer and "
                "flushing any deferred HO deactivation", cause_value);
        ogs_assert(OGS_OK ==
            smf_epc_pfcp_send_one_bearer_modification_request(
                bearer, OGS_INVALID_POOL_ID, OGS_PFCP_MODIFY_REMOVE,
                OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
                OGS_GTP2_CAUSE_UNDEFINED_VALUE));
        smf_ho_flush_deferred_deactivation(sess);
        return;
    }

    /***********************************************
     * Cause = Request accepted -> require TEIDs now
     ***********************************************/
    if (rsp->bearer_contexts.s5_s8_u_pgw_f_teid.presence &&
        rsp->bearer_contexts.s5_s8_u_sgw_f_teid.presence) {
        pgw_s5u_teid = rsp->bearer_contexts.s5_s8_u_pgw_f_teid.data;
        sgw_s5u_teid = rsp->bearer_contexts.s5_s8_u_sgw_f_teid.data;
    }

    if (rsp->bearer_contexts.s2b_u_pgw_f_teid.presence &&
        rsp->bearer_contexts.s2b_u_epdg_f_teid_8.presence) {
        pgw_s5u_teid = rsp->bearer_contexts.s2b_u_pgw_f_teid.data;
        sgw_s5u_teid = rsp->bearer_contexts.s2b_u_epdg_f_teid_8.data;
    }

    if (!pgw_s5u_teid || !sgw_s5u_teid) {
        ogs_error("Missing F-TEID on accepted CBResp (PGW=%p SGW=%p) - "
                "cleaning up", pgw_s5u_teid, sgw_s5u_teid);
        ogs_assert(OGS_OK ==
            smf_epc_pfcp_send_one_bearer_modification_request(
                bearer, OGS_INVALID_POOL_ID, OGS_PFCP_MODIFY_REMOVE,
                OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
                OGS_GTP2_CAUSE_UNDEFINED_VALUE));
        smf_ho_flush_deferred_deactivation(sess);
        return;
    }

    /********************
     * Check ALL Context
     ********************/
    ogs_assert(bearer);
    ogs_assert(sess);

    /* Find the Bearer by PGW-S5U-TEID */
    ogs_assert(pgw_s5u_teid);
    bearer = smf_bearer_find_by_pgw_s5u_teid(sess, be32toh(pgw_s5u_teid->teid));
    if (!bearer) {
        ogs_error("smf_bearer_find_by_pgw_s5u_teid() failed");
        return;
    }

    /* Set EBI */
    bearer->ebi = rsp->bearer_contexts.eps_bearer_id.u8;

    /* Clear create_pending flag */
    bearer->create_pending = false;

    /* Check if delete was pending during Create Bearer Response wait */
    if (bearer->delete_pending) {
        ogs_warn("Bearer[EBI:%d] has pending delete, sending Delete Bearer Request",
                 bearer->ebi);
        bearer->delete_pending = false;
        ogs_assert(OGS_OK ==
            smf_gtp2_send_delete_bearer_request(
                bearer, bearer->delete_pti, bearer->delete_cause_value));
        return;
    }

    /* Data Plane(DL) : SGW-S5U */
    ogs_assert(sgw_s5u_teid);
    bearer->sgw_s5u_teid = be32toh(sgw_s5u_teid->teid);
    rv = ogs_gtp2_f_teid_to_ip(sgw_s5u_teid, &bearer->sgw_s5u_ip);
    ogs_assert(rv == OGS_OK);

    ogs_debug("Create Bearer Response : SGW[0x%x] --> SMF[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    /* Setup FAR */
    dl_far = bearer->dl_far;
    ogs_assert(dl_far);

    dl_far->apply_action = OGS_PFCP_APPLY_ACTION_FORW;

    ogs_assert(OGS_OK ==
        ogs_pfcp_ip_to_outer_header_creation(
            &bearer->sgw_s5u_ip,
            &dl_far->outer_header_creation,
            &dl_far->outer_header_creation_len));
    dl_far->outer_header_creation.teid = bearer->sgw_s5u_teid;

    ogs_assert(OGS_OK ==
        smf_epc_pfcp_send_one_bearer_modification_request(
            bearer, OGS_INVALID_POOL_ID, OGS_PFCP_MODIFY_ACTIVATE,
            OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
            OGS_GTP2_CAUSE_UNDEFINED_VALUE));

    /* HO: Send deferred DBReq to ePDG now that dedicated bearer
     * is established (CBResp received with Cause=accepted). */
    smf_ho_flush_deferred_deactivation(sess);
}

void smf_s5c_handle_update_bearer_response(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_update_bearer_response_t *rsp)
{
    int rv;
    uint8_t cause_value;
    ogs_gtp2_cause_t *cause = NULL;
    uint64_t gtp_flags = 0;
    uint64_t pfcp_flags = 0;
    smf_bearer_t *bearer = NULL;
    ogs_pool_id_t bearer_id = OGS_INVALID_POOL_ID;

    ogs_assert(sess);
    ogs_assert(rsp);

    ogs_debug("Update Bearer Response");

    /********************
     * Check Transaction
     ********************/
    ogs_assert(xact);
    gtp_flags = xact->update_flags;
    ogs_assert(gtp_flags);

    bearer_id = OGS_POINTER_TO_UINT(xact->data);
    ogs_assert(bearer_id >= OGS_MIN_POOL_ID && bearer_id <= OGS_MAX_POOL_ID);

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);

    /*****************************************
     * Check Mandatory/Conditional IE Missing
     *****************************************/
    cause_value = OGS_GTP2_CAUSE_REQUEST_ACCEPTED;

    if (rsp->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }

    if (rsp->cause.presence == 0) {
        ogs_error("No Cause");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts.cause.presence == 0) {
        ogs_error("No Bearer Cause");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        return;
    }

    /********************
     * Check Cause Value
     ********************/
    ogs_assert(cause_value == OGS_GTP2_CAUSE_REQUEST_ACCEPTED);

    cause = rsp->bearer_contexts.cause.data;
    ogs_assert(cause);
    cause_value = cause->value;
    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_error("GTP Bearer Cause [VALUE:%d]", cause_value);
        return;
    }

    cause = rsp->cause.data;
    ogs_assert(cause);
    cause_value = cause->value;
    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_error("GTP Cause [Value:%d]", cause_value);
        return;
    }

    /********************
     * Check ALL Context
     ********************/
    bearer = smf_bearer_find_by_id(bearer_id);
    if (!bearer) {
        ogs_error("Bearer has already been removed");
        return;
    }

    ogs_debug("    SGW_S5C_TEID[0x%x] SMF_N4_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    ogs_debug("Update Bearer Response : SGW[0x%x] --> SMF[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    if (gtp_flags & OGS_GTP_MODIFY_TFT_UPDATE) {
        pfcp_flags |= OGS_PFCP_MODIFY_EPC_TFT_UPDATE;
        smf_bearer_tft_update(bearer);
    }

    if (gtp_flags & OGS_GTP_MODIFY_QOS_UPDATE) {
        pfcp_flags |= OGS_PFCP_MODIFY_EPC_QOS_UPDATE;
        smf_bearer_qos_update(bearer);
    }

    if (pfcp_flags)
        ogs_assert(OGS_OK ==
            smf_epc_pfcp_send_one_bearer_modification_request(
                bearer, OGS_INVALID_POOL_ID, pfcp_flags,
                OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
                OGS_GTP2_CAUSE_UNDEFINED_VALUE));
}

/* return true if entire session must be released */
/* Check if there's another session for the same UE with the same IP but different RAT type */
static smf_sess_t *smf_sess_find_by_ip_and_different_rat(
        smf_sess_t *current_sess) 
{
    smf_ue_t *smf_ue = NULL;
    smf_sess_t *sess = NULL;

    ogs_assert(current_sess);
    smf_ue = smf_ue_find_by_id(current_sess->smf_ue_id);
    ogs_assert(smf_ue);

    /*
     * Phase 4 case Y (now Phase 4.2a): match the actually-allocated UE
     * IP rather than session.ue_ip — for Initial-Attach sessions, MME
     * sends CSR with PAA=0.0.0.0 and the real address is only kept in
     * sess->ipv4. The helper smf_sess_match_ue_ip() (context.c) is
     * shared with smf_find_s2b_session_for_ue() in pfcp-path.c so the
     * comparison logic stays consistent across both call sites.
     *
     * s5c-handler.c around line 407 documents the same gotcha for the
     * HO peer-link path: "Use peer_sess->ipv4->addr (actual allocated
     * IP), not peer_sess->session.ue_ip (may be 0 if original PAA was
     * also 0)".
     */
    ogs_list_for_each(&smf_ue->sess_list, sess) {
        if (sess == current_sess)
            continue;
        if (sess->gtp_rat_type == current_sess->gtp_rat_type)
            continue;
        if (!smf_sess_match_ue_ip(sess, current_sess))
            continue;

        ogs_debug("Found session with same IP but different RAT type: "
                "Current[RAT:%d] Other[RAT:%d]",
                current_sess->gtp_rat_type, sess->gtp_rat_type);
        return sess;
    }

    return NULL;
}

bool smf_s5c_handle_delete_bearer_response(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_delete_bearer_response_t *rsp)
{
    int rv;
    uint8_t cause_value;
    smf_bearer_t *bearer = NULL;
    ogs_pool_id_t bearer_id = OGS_INVALID_POOL_ID;
    smf_sess_t *other_sess = NULL;

    ogs_assert(sess);
    ogs_assert(rsp);

    ogs_debug("Delete Bearer Response");

    /********************
     * Check Transaction
     ********************/
    ogs_assert(xact);

    bearer_id = OGS_POINTER_TO_UINT(xact->data);
    ogs_assert(bearer_id >= OGS_MIN_POOL_ID && bearer_id <= OGS_MAX_POOL_ID);

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);

    bearer = smf_bearer_find_by_id(bearer_id);
    if (!bearer) {
        ogs_error("Bearer has already been removed");
        /* Release entire session: */
        return true;
    }


    /* Check if there's another active session for the same UE with the same IP
     * but different RAT type. This is critical for VoLTE to VoWiFi handover. */
    other_sess = smf_sess_find_by_ip_and_different_rat(sess);
    if (other_sess) {
        ogs_debug("    Found another session for UE with same IP but different RAT:");
        ogs_debug("    Current[RAT:%d] Other[RAT:%d]",
                sess->gtp_rat_type, other_sess->gtp_rat_type);
    }

    if (rsp->linked_eps_bearer_id.presence) {
        /*
         * 1. SMF sends Delete Bearer Request(DEFAULT BEARER) to SGW/MME.
         * 2. MME sends Delete Bearer Response to SGW/SMF.
         *
         * OR
         *
         * 1. SMF sends Delete Bearer Request(DEFAULT BEARER) to ePDG.
         * 2. ePDG sends Delete Bearer Response(DEFAULT BEARER) to SMF.
         */
        if (rsp->cause.presence) {
            ogs_gtp2_cause_t *cause = rsp->cause.data;
            ogs_assert(cause);

            cause_value = cause->value;
            if (cause->value == OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
                /* If this is a VoLTE to VoWiFi handover with same IP address */
                if (other_sess && 
                    sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_EUTRAN && 
                    other_sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_WLAN) {
                    ogs_debug("    VoLTE to VoWiFi handover detected, ensuring "
                            "packets flow through WLAN interface");
                    
                    /* No need to delete the session, forwarding rules will be 
                     * updated when the S2b interface is established */
                    return false;
                }
            } else {
                ogs_error("GTP Cause [Value:%d]", cause_value);
            }
        } else {
            ogs_error("No Cause");
        }
        
        /* Release entire session: */
        return true;
    }

    /*
     * 1. MME sends Bearer Resource Command to SGW/SMF.
     * 2. SMF sends Delete Bearer Request(DEDICATED BEARER) to SGW/MME
     * 3. MME sends Delete Bearer Response(DEDICATED BEARER) to SGW/SMF
     *
     * OR
     *
     * 1. SMF sends Delete Bearer Request(DEDICATED BEARER) to SGW/MME
     * 2. MME sends Delete Bearer Response(DEDICATED BEARER) to SGW/SMF
     */
    if (rsp->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }

    if (rsp->cause.presence) {
        ogs_gtp2_cause_t *cause = rsp->cause.data;
        ogs_assert(cause);

        cause_value = cause->value;
        if (cause_value == OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
            if (rsp->bearer_contexts.cause.presence) {
                cause = rsp->bearer_contexts.cause.data;
                ogs_assert(cause);

                if (cause_value == OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
                    /* If this is a VoLTE to VoWiFi handover with same IP address */
                    if (other_sess && 
                        sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_EUTRAN && 
                        other_sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_WLAN) {
                        ogs_debug("    VoLTE to VoWiFi handover detected, ensuring "
                                "packets flow through WLAN interface");
                        /* No need for special handling here, forwarding rules are 
                         * maintained by the PFCP session */
                    }
                } else {
                    ogs_error("GTP Cause [Value:%d]", cause_value);
                }
            } else {
                ogs_error("No Cause");
            }
        } else {
            ogs_error("GTP Cause [Value:%d]", cause_value);
        }
    } else {
        ogs_error("No Cause");
    }

    ogs_debug("    SGW_S5C_TEID[0x%x] SMF_N4_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    ogs_debug("Delete Bearer Response : SGW[0x%x] --> SMF[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    ogs_assert(OGS_OK ==
        smf_epc_pfcp_send_one_bearer_modification_request(
            bearer, OGS_INVALID_POOL_ID, OGS_PFCP_MODIFY_REMOVE,
            OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
            OGS_GTP2_CAUSE_UNDEFINED_VALUE));
    return false;
}

static int reconfigure_packet_filter(smf_pf_t *pf, ogs_gtp2_tft_t *tft, int i)
{
    int j;

    ogs_assert(pf);
    ogs_assert(tft);

    memset(&pf->ipfw_rule, 0, sizeof(ogs_ipfw_rule_t));
    pf->direction = tft->pf[i].direction;
    for (j = 0; j < tft->pf[i].content.num_of_component; j++) {
        switch(tft->pf[i].content.component[j].type) {
        case OGS_PACKET_FILTER_PROTOCOL_IDENTIFIER_NEXT_HEADER_TYPE:
            pf->ipfw_rule.proto = tft->pf[i].content.component[j].proto;
            break;
        case OGS_PACKET_FILTER_IPV4_REMOTE_ADDRESS_TYPE:
            pf->ipfw_rule.ipv4_dst = 1;
            pf->ipfw_rule.ip.dst.addr[0] =
                tft->pf[i].content.component[j].ipv4.addr;
            pf->ipfw_rule.ip.dst.mask[0] =
                tft->pf[i].content.component[j].ipv4.mask;
            break;
        case OGS_PACKET_FILTER_IPV4_LOCAL_ADDRESS_TYPE:
            pf->ipfw_rule.ipv4_src = 1;
            pf->ipfw_rule.ip.src.addr[0] =
                tft->pf[i].content.component[j].ipv4.addr;
            pf->ipfw_rule.ip.src.mask[0] =
                tft->pf[i].content.component[j].ipv4.mask;
            break;
        case OGS_PACKET_FILTER_IPV6_REMOTE_ADDRESS_TYPE:
            pf->ipfw_rule.ipv6_dst = 1;
            memcpy(pf->ipfw_rule.ip.dst.addr,
                tft->pf[i].content.component[j].ipv6_mask.addr,
                sizeof(pf->ipfw_rule.ip.dst.addr));
            memcpy(pf->ipfw_rule.ip.dst.mask,
                tft->pf[i].content.component[j].ipv6_mask.mask,
                sizeof(pf->ipfw_rule.ip.dst.mask));
            break;
        case OGS_PACKET_FILTER_IPV6_REMOTE_ADDRESS_PREFIX_LENGTH_TYPE:
            pf->ipfw_rule.ipv6_dst = 1;
            memcpy(pf->ipfw_rule.ip.dst.addr,
                tft->pf[i].content.component[j].ipv6_mask.addr,
                sizeof(pf->ipfw_rule.ip.dst.addr));
            n2mask((struct in6_addr *)pf->ipfw_rule.ip.dst.mask,
                tft->pf[i].content.component[j].ipv6.prefixlen);
            break;
        case OGS_PACKET_FILTER_IPV6_LOCAL_ADDRESS_TYPE:
            pf->ipfw_rule.ipv6_src = 1;
            memcpy(pf->ipfw_rule.ip.src.addr,
                tft->pf[i].content.component[j].ipv6_mask.addr,
                sizeof(pf->ipfw_rule.ip.src.addr));
            memcpy(pf->ipfw_rule.ip.src.mask,
                tft->pf[i].content.component[j].ipv6_mask.mask,
                sizeof(pf->ipfw_rule.ip.src.mask));
            break;
        case OGS_PACKET_FILTER_IPV6_LOCAL_ADDRESS_PREFIX_LENGTH_TYPE:
            pf->ipfw_rule.ipv6_src = 1;
            memcpy(pf->ipfw_rule.ip.src.addr,
                tft->pf[i].content.component[j].ipv6_mask.addr,
                sizeof(pf->ipfw_rule.ip.src.addr));
            n2mask((struct in6_addr *)pf->ipfw_rule.ip.src.mask,
                tft->pf[i].content.component[j].ipv6.prefixlen);
            break;
        case OGS_PACKET_FILTER_SINGLE_LOCAL_PORT_TYPE:
            pf->ipfw_rule.port.src.low = pf->ipfw_rule.port.src.high =
                    tft->pf[i].content.component[j].port.low;
            break;
        case OGS_PACKET_FILTER_SINGLE_REMOTE_PORT_TYPE:
            pf->ipfw_rule.port.dst.low = pf->ipfw_rule.port.dst.high =
                    tft->pf[i].content.component[j].port.low;
            break;
        case OGS_PACKET_FILTER_LOCAL_PORT_RANGE_TYPE:
            pf->ipfw_rule.port.src.low =
                tft->pf[i].content.component[j].port.low;
            pf->ipfw_rule.port.src.high =
                tft->pf[i].content.component[j].port.high;
            break;
        case OGS_PACKET_FILTER_REMOTE_PORT_RANGE_TYPE:
            pf->ipfw_rule.port.dst.low =
                tft->pf[i].content.component[j].port.low;
            pf->ipfw_rule.port.dst.high =
                tft->pf[i].content.component[j].port.high;
            break;
        default:
            ogs_error("Unknown Packet Filter Type(%d)",
                    tft->pf[i].content.component[j].type);
            return OGS_ERROR;
        }
    }

    return j;
}

void smf_s5c_handle_bearer_resource_command(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_bearer_resource_command_t *cmd,
        ogs_gtp2_sender_f_teid_t *sender_f_teid)
{
    int rv;
    uint8_t cause_value = 0;
    int i;

    ogs_gtp2_header_t h;
    ogs_pkbuf_t *pkbuf = NULL;

    smf_bearer_t *bearer = NULL;
    smf_pf_t *pf = NULL;

    int16_t decoded;
    ogs_gtp2_tft_t tft;

    int tft_update = 0;
    int qos_update = 0;
    int tft_delete = 0;

    ogs_assert(xact);
    ogs_assert(cmd);
    ogs_assert(sender_f_teid);

    ogs_debug("Bearer Resource Command");

    /************************
     * Check Session Context
     ************************/
    cause_value = OGS_GTP2_CAUSE_REQUEST_ACCEPTED;

    if (!sess) {
        ogs_error("No Context");
        cause_value = OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND;
    } else {
        if (cmd->linked_eps_bearer_id.presence == 0) {
            ogs_error("No EPS Bearer ID");
            cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        }

        if (cause_value == OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
            uint8_t ebi = cmd->linked_eps_bearer_id.u8;

            if (cmd->eps_bearer_id.presence)
                ebi = cmd->eps_bearer_id.u8;

            bearer = smf_bearer_find_by_ebi(sess, ebi);
            if (!bearer) {
                ogs_error("No Context for Linked EPS Bearer ID[%d:%d]",
                        cmd->linked_eps_bearer_id.u8, ebi);
                cause_value = OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND;
            }
        }
    }

    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp2_send_error_message(
                xact, get_sender_f_teid(sess, sender_f_teid),
                OGS_GTP2_BEARER_RESOURCE_FAILURE_INDICATION_TYPE, cause_value);
        return;
    }

    /*****************************************
     * Check Mandatory/Conditional IE Missing
     *****************************************/
    ogs_assert(cause_value == OGS_GTP2_CAUSE_REQUEST_ACCEPTED);

    if (cmd->procedure_transaction_id.presence == 0) {
        ogs_error("No PTI");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }
    if (cmd->traffic_aggregate_description.presence == 0) {
        ogs_error("No Traffic aggregate description(TAD)");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cmd->linked_eps_bearer_id.presence == 0) {
        ogs_error("No Linked EPS Bearer ID");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP2_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp2_send_error_message(
                xact, get_sender_f_teid(sess, sender_f_teid),
                OGS_GTP2_BEARER_RESOURCE_FAILURE_INDICATION_TYPE, cause_value);
        return;
    }

    /********************
     * Check ALL Context
     ********************/
    ogs_assert(bearer);
    ogs_assert(sess);

    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->smf_n4_teid);

    decoded = ogs_gtp2_parse_tft(&tft, &cmd->traffic_aggregate_description);
    if (cmd->traffic_aggregate_description.len != decoded) {
        ogs_error("ogs_gtp2_parse_tft() failed");
        ogs_log_hexdump(OGS_LOG_ERROR,
            cmd->traffic_aggregate_description.data,
            cmd->traffic_aggregate_description.len);
        ogs_gtp2_send_error_message(
                xact, get_sender_f_teid(sess, sender_f_teid),
                OGS_GTP2_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                OGS_GTP2_CAUSE_INVALID_MESSAGE_FORMAT);
        return;
    }

    ogs_assert(cmd->traffic_aggregate_description.len == decoded);

    if (tft.code == OGS_GTP2_TFT_CODE_NO_TFT_OPERATION) {
        /* No operation */
    } else if (tft.code == OGS_GTP2_TFT_CODE_DELETE_EXISTING_TFT) {
        smf_pf_remove_all(bearer);
        tft_delete = 1;
    } else if (tft.code ==
            OGS_GTP2_TFT_CODE_REPLACE_PACKET_FILTERS_IN_EXISTING) {
        for (i = 0; i < tft.num_of_packet_filter &&
                    i < OGS_MAX_NUM_OF_FLOW_IN_GTP; i++) {
            pf = smf_pf_find_by_identifier(bearer, tft.pf[i].identifier+1);
            if (pf) {
                if (reconfigure_packet_filter(pf, &tft, i) < 0) {
                    ogs_gtp2_send_error_message(
                        xact, get_sender_f_teid(sess, sender_f_teid),
                        OGS_GTP2_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                        OGS_GTP2_CAUSE_SEMANTIC_ERRORS_IN_PACKET_FILTER);
                    return;
                }
/*
 * Refer to lib/ipfw/ogs-ipfw.h
 * Issue #338
 *
 * <DOWNLINK/BI-DIRECTIONAL>
 * TFT : Local <UE_IP> <UE_PORT> REMOTE <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 * -->
 * RULE : Source <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> Destination <UE_IP> <UE_PORT>
 *
 * <UPLINK>
 * TFT : Local <UE_IP> <UE_PORT> REMOTE <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 * -->
 * RULE : Source <UE_IP> <UE_PORT> Destination <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 */
                if (pf->direction == OGS_FLOW_DOWNLINK_ONLY)
                    ogs_ipfw_rule_swap(&pf->ipfw_rule);

                if (pf->flow_description)
                    ogs_free(pf->flow_description);
/*
 * Issue #338
 *
 * <DOWNLINK/BI-DIRECTIONAL>
 * RULE : Source <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> Destination <UE_IP> <UE_PORT>
 * -->
 * GX : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 * PFCP : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 *
 * <UPLINK>
 * RULE : Source <UE_IP> <UE_PORT> Destination <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 * -->
 * GX : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 * PFCP : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 */
                if (pf->direction == OGS_FLOW_UPLINK_ONLY) {
                    ogs_ipfw_rule_t tmp;
                    ogs_ipfw_copy_and_swap(&tmp, &pf->ipfw_rule);
                    pf->flow_description =
                        ogs_ipfw_encode_flow_description(&tmp);
                    ogs_assert(pf->flow_description);
                } else {
                    pf->flow_description =
                        ogs_ipfw_encode_flow_description(&pf->ipfw_rule);
                    ogs_assert(pf->flow_description);
                }

                tft_update = 1;
            }
        }
    } else if (tft.code ==
                OGS_GTP2_TFT_CODE_ADD_PACKET_FILTERS_TO_EXISTING_TFT ||
                tft.code == OGS_GTP2_TFT_CODE_CREATE_NEW_TFT) {
        if (tft.code == OGS_GTP2_TFT_CODE_CREATE_NEW_TFT)
            smf_pf_remove_all(bearer);

        for (i = 0; i < tft.num_of_packet_filter &&
                    i < OGS_MAX_NUM_OF_FLOW_IN_GTP; i++) {
            pf = smf_pf_find_by_identifier(bearer, tft.pf[i].identifier+1);
            if (!pf)
                pf = smf_pf_add(bearer);
            ogs_assert(pf);

            if (reconfigure_packet_filter(pf, &tft, i) < 0) {
                ogs_gtp2_send_error_message(
                    xact, get_sender_f_teid(sess, sender_f_teid),
                    OGS_GTP2_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                    OGS_GTP2_CAUSE_SEMANTIC_ERRORS_IN_PACKET_FILTER);
                return;
            }
/*
 * Refer to lib/ipfw/ogs-ipfw.h
 * Issue #338
 *
 * <DOWNLINK/BI-DIRECTIONAL>
 * TFT : Local <UE_IP> <UE_PORT> REMOTE <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 * -->
 * RULE : Source <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> Destination <UE_IP> <UE_PORT>
 *
 * <UPLINK>
 * TFT : Local <UE_IP> <UE_PORT> REMOTE <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 * -->
 * RULE : Source <UE_IP> <UE_PORT> Destination <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 */
            if (pf->direction == OGS_FLOW_DOWNLINK_ONLY)
                ogs_ipfw_rule_swap(&pf->ipfw_rule);

            if (pf->flow_description)
                ogs_free(pf->flow_description);

/*
 * Issue #338
 *
 * <DOWNLINK/BI-DIRECTIONAL>
 * RULE : Source <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> Destination <UE_IP> <UE_PORT>
 * -->
 * GX : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 * PFCP : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 *
 * <UPLINK>
 * RULE : Source <UE_IP> <UE_PORT> Destination <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT>
 * -->
 * GX : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 * PFCP : permit out from <P-CSCF_RTP_IP> <P-CSCF_RTP_PORT> to <UE_IP> <UE_PORT>
 */
            if (pf->direction == OGS_FLOW_UPLINK_ONLY) {
                ogs_ipfw_rule_t tmp;
                ogs_ipfw_copy_and_swap(&tmp, &pf->ipfw_rule);
                pf->flow_description =
                    ogs_ipfw_encode_flow_description(&tmp);
                ogs_assert(pf->flow_description);
            } else {
                pf->flow_description =
                    ogs_ipfw_encode_flow_description(&pf->ipfw_rule);
                ogs_assert(pf->flow_description);
            }

            tft_update = 1;
        }
    } else if (tft.code ==
            OGS_GTP2_TFT_CODE_DELETE_PACKET_FILTERS_FROM_EXISTING) {
        for (i = 0; i < tft.num_of_packet_filter &&
                    i <= OGS_MAX_NUM_OF_FLOW_IN_GTP; i++) {
            pf = smf_pf_find_by_identifier(bearer, tft.pf[i].identifier+1);
            if (pf)
                smf_pf_remove(pf);
        }

        if (ogs_list_count(&bearer->pf_list))
            tft_update = 1;
        else
            tft_delete = 1;
    }

    if (cmd->flow_quality_of_service.presence) {
        ogs_gtp2_flow_qos_t flow_qos;

        decoded = ogs_gtp2_parse_flow_qos(
                &flow_qos, &cmd->flow_quality_of_service);
        ogs_assert(cmd->flow_quality_of_service.len == decoded);

        bearer->qos.mbr.uplink = flow_qos.ul_mbr;
        bearer->qos.mbr.downlink = flow_qos.dl_mbr;
        bearer->qos.gbr.uplink = flow_qos.ul_gbr;
        bearer->qos.gbr.downlink = flow_qos.dl_gbr;

        qos_update = 1;
    }

    if (tft_update == 0 && tft_delete == 0 && qos_update == 0) {
        /* No modification */
        ogs_gtp2_send_error_message(
                xact, get_sender_f_teid(sess, sender_f_teid),
                OGS_GTP2_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                OGS_GTP2_CAUSE_SERVICE_NOT_SUPPORTED);
        return;
    }

    if (tft_delete) {
        /*
         * TS23.214
         * 6.3.1.7 Procedures with modification of bearer
         * p50
         * 2.  ...
         * For "PGW/MME initiated bearer deactivation procedure",
         * PGW-C shall indicate PGW-U to stop counting and stop
         * forwarding downlink packets for the affected bearer(s).
         */
        ogs_assert(OGS_OK ==
            smf_epc_pfcp_send_one_bearer_modification_request(
                bearer, xact->id,
                OGS_PFCP_MODIFY_DL_ONLY|OGS_PFCP_MODIFY_DEACTIVATE,
                cmd->procedure_transaction_id.u8,
                OGS_GTP2_CAUSE_UNDEFINED_VALUE));

    } else {
        memset(&h, 0, sizeof(ogs_gtp2_header_t));
        h.teid = sess->sgw_s5c_teid;
        h.type = OGS_GTP2_UPDATE_BEARER_REQUEST_TYPE;

        pkbuf = smf_s5c_build_update_bearer_request(
                h.type, bearer, cmd->procedure_transaction_id.u8,
                tft_update ? &tft : NULL, qos_update);
        if (!pkbuf) {
            ogs_error("smf_s5c_build_update_bearer_request() failed");
            return;
        }

        rv = ogs_gtp_xact_update_tx(xact, &h, pkbuf);
        if (rv != OGS_OK) {
            ogs_error("ogs_gtp_xact_update_tx() failed");
            return;
        }

        if (tft_update)
            xact->update_flags |= OGS_GTP_MODIFY_TFT_UPDATE;
        if (qos_update)
            xact->update_flags |= OGS_GTP_MODIFY_QOS_UPDATE;

        /* IMPORTANT:
         *
         * When initiaited by Bearer Resource Command, there must be bearer context
         * in the Transaction. Otherwise, the bearer context cannot be found
         * in GTP response message.
         *
         * For example,
         * 1. MME sends Bearer Resource Command to SGW-C, SMF.
         * 2. SMF sends Update/Delete Bearer Request to the SGW-C, MME.
         * 3. MME sends Update/Delete Bearer Response to thw SGW-C, SMF.
         *
         * On number 3 step, if MME sends Response without Bearer Context,
         * we need a way to find Bearer context.
         *
         * To do this, I saved Bearer Context in Transaction Context.
         */
        xact->data = OGS_UINT_TO_POINTER(bearer->id);

        rv = ogs_gtp_xact_commit(xact);
        ogs_expect(rv == OGS_OK);
    }
}

void smf_s5c_handle_delete_bearer_command(
        smf_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp2_delete_bearer_command_t *cmd,
        ogs_gtp2_sender_f_teid_t *sender_f_teid)
{
    int rv;
    uint8_t cause_value = OGS_GTP2_CAUSE_REQUEST_ACCEPTED;
    smf_bearer_t *bearer = NULL;

    ogs_assert(xact);
    ogs_assert(cmd);
    ogs_assert(sess);

    ogs_debug("Delete Bearer Command received");

    /* Check if we have bearer contexts to delete */
    if (cmd->bearer_contexts.presence == 0) {
        ogs_error("No Bearer Contexts in Delete Bearer Command");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        goto cleanup;
    }

    /* Check if EPS Bearer ID is present */
    if (cmd->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID in Delete Bearer Command");
        cause_value = OGS_GTP2_CAUSE_MANDATORY_IE_MISSING;
        goto cleanup;
    }

    /* Find and delete the specified bearer */
    bearer = smf_bearer_find_by_ebi(
                sess, cmd->bearer_contexts.eps_bearer_id.u8);
    if (!bearer) {
        ogs_warn("Bearer EBI[%d] not found",
                cmd->bearer_contexts.eps_bearer_id.u8);
        cause_value = OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND;
        goto cleanup;
    }

    ogs_debug("Processing Delete Bearer Command for Bearer EBI[%d]",
            bearer->ebi);

    /* Send Delete Bearer Request to SGW/MME to initiate bearer deactivation */
    rv = smf_gtp2_send_delete_bearer_request(bearer,
        OGS_NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED,
        OGS_GTP2_CAUSE_REQUEST_ACCEPTED);
    if (rv != OGS_OK) {
        ogs_error("Failed to send Delete Bearer Request");
        cause_value = OGS_GTP2_CAUSE_SYSTEM_FAILURE;
        goto cleanup;
    }

    /*
     * The Command xact must stay alive until Delete Bearer Response arrives.
     * Commit the xact here — ogs_gtp2_send_error_message would also commit,
     * but on the success path we commit explicitly so the xact is not
     * silently destroyed by the GTP stack's timeout.
     *
     * NOTE: Ideally we would associate this cmd_xact with the local
     * Delete Bearer Request xact via ogs_gtp_xact_associate() so that
     * when Delete Bearer Response arrives the cmd_xact is automatically
     * handled. However, smf_gtp2_send_delete_bearer_request() currently
     * creates and commits the local xact internally, so association would
     * require a larger refactor. For now, commit the Command xact to
     * acknowledge receipt.
     */
    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);

    return;

cleanup:
    ogs_gtp2_send_error_message(
            xact, get_sender_f_teid(sess, sender_f_teid),
            OGS_GTP2_DELETE_BEARER_FAILURE_INDICATION_TYPE, cause_value);
}
