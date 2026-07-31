/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
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

#include "mme-event.h"
#include "mme-timer.h"
#include "mme-sm.h"
#include "mme-fd-path.h"
#include "emm-handler.h"
#include "esm-build.h"
#include "esm-handler.h"
#include "mme-s11-handler.h"
#include "s1ap-path.h"
#include "nas-path.h"
#include "mme-gtp-path.h"

#undef OGS_LOG_DOMAIN
#define OGS_LOG_DOMAIN __esm_log_domain

static uint8_t gtp_cause_from_esm(uint8_t esm_cause)
{
    switch (esm_cause) {
    case OGS_NAS_ESM_CAUSE_SEMANTIC_ERROR_IN_THE_TFT_OPERATION:
        return OGS_GTP2_CAUSE_SEMANTIC_ERROR_IN_THE_TFT_OPERATION;
    case OGS_NAS_ESM_CAUSE_SYNTACTICAL_ERROR_IN_THE_TFT_OPERATION:
        return OGS_GTP2_CAUSE_SYNTACTIC_ERROR_IN_THE_TFT_OPERATION;
    case OGS_NAS_ESM_CAUSE_SYNTACTICAL_ERROR_IN_PACKET_FILTERS:
        return OGS_GTP2_CAUSE_SYNTACTIC_ERRORS_IN_PACKET_FILTER;
    case OGS_NAS_ESM_CAUSE_SEMANTIC_ERRORS_IN_PACKET_FILTERS:
        return OGS_GTP2_CAUSE_SEMANTIC_ERRORS_IN_PACKET_FILTER;
    default:
        break;
    }

    return OGS_GTP2_CAUSE_SYSTEM_FAILURE;
}

void esm_state_initial(ogs_fsm_t *s, mme_event_t *e)
{
    ogs_assert(s);

    mme_sm_debug(e);

    OGS_FSM_TRAN(s, &esm_state_inactive);
}

void esm_state_final(ogs_fsm_t *s, mme_event_t *e)
{
    ogs_assert(s);

    mme_sm_debug(e);
}

void esm_state_inactive(ogs_fsm_t *s, mme_event_t *e)
{
    int r, rv;
    mme_ue_t *mme_ue = NULL;
    enb_ue_t *enb_ue = NULL;
    sgw_ue_t *sgw_ue = NULL;
    mme_sess_t *sess = NULL;
    mme_bearer_t *bearer = NULL;
    ogs_nas_eps_message_t *message = NULL;
    ogs_nas_security_header_type_t h;

    ogs_nas_eps_activate_dedicated_eps_bearer_context_reject_t
        *activate_dedicated_eps_bearer_context_reject = NULL;

    ogs_assert(s);
    ogs_assert(e);

    mme_sm_debug(e);

    bearer = mme_bearer_find_by_id(e->bearer_id);
    ogs_assert(bearer);
    sess = mme_sess_find_by_id(bearer->sess_id);
    ogs_assert(sess);
    mme_ue = mme_ue_find_by_id(sess->mme_ue_id);
    ogs_assert(mme_ue);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        CLEAR_BEARER_ALL_TIMERS(bearer);
        break;
    case OGS_FSM_EXIT_SIG:
        /*
         * T3485 (Activate-default retransmit) is only meaningful while the
         * bearer is stuck in esm_state_inactive. Clear it on every exit so it
         * cannot fire in a state that does not handle it (e.g. a stuck bearer
         * receiving PDN_DISCONNECT_REQUEST transitions to
         * esm_state_pdn_will_disconnect, which has no T3485 handler). Only
         * t3485 is cleared here: a deactivate sent just before the transition
         * arms t3495, which must be preserved (EXIT runs after the handler).
         */
        CLEAR_BEARER_TIMER(bearer->t3485);
        break;
    case MME_EVENT_ESM_MESSAGE:
        message = e->nas_message;
        ogs_assert(message);

        enb_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
        if (!enb_ue) {
            ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) - "
                    "S1 context released mid-procedure (no crash)",
                    mme_ue->imsi_bcd, message->esm.h.message_type);
            OGS_FSM_TRAN(s, esm_state_exception);
            break;
        }

        switch (message->esm.h.message_type) {
        case OGS_NAS_EPS_PDN_CONNECTIVITY_REQUEST:
            ogs_debug("PDN Connectivity request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            rv = esm_handle_pdn_connectivity_request(
                    enb_ue, bearer, &message->esm.pdn_connectivity_request,
                    e->create_action);
            if (rv != OGS_OK) {
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }
            break;
        case OGS_NAS_EPS_PDN_DISCONNECT_REQUEST:
            ogs_debug("PDN disconnect request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            if (MME_HAVE_SGW_S1U_PATH(sess)) {
                sgw_ue = sgw_ue_find_by_id(mme_ue->sgw_ue_id);
                ogs_assert(sgw_ue);

                ogs_assert(OGS_OK ==
                    mme_gtp_send_delete_session_request(enb_ue, sgw_ue, sess,
                        OGS_GTP_DELETE_SEND_DEACTIVATE_BEARER_CONTEXT_REQUEST));
                /* Mark the core delete as issued (DSReq sent, DSResp not yet
                 * received) so the pdn_will_disconnect stuck-recovery gate
                 * can later confirm intent + core-delete completion. */
                sess->deletion_in_progress = true;
                sess->core_delete_done = false;
            } else {
                r = nas_eps_send_deactivate_bearer_context_request(bearer);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
            }

            CLEAR_SGW_S1U_PATH(sess);

            OGS_FSM_TRAN(s, esm_state_pdn_will_disconnect);
            break;

        case OGS_NAS_EPS_ESM_INFORMATION_RESPONSE:
            ogs_debug("ESM information response");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);

            CLEAR_BEARER_TIMER(bearer->t3489);

            h.type = e->nas_type;
            enb_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);

            if (h.integrity_protected == 0) {
                ogs_error("[%s] No Integrity Protected", mme_ue->imsi_bcd);

                if (!enb_ue) {
                ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) "
                        "(no crash)", mme_ue->imsi_bcd,
                        message->esm.h.message_type);
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }

                r = nas_eps_send_attach_reject(enb_ue, mme_ue,
                        OGS_NAS_EMM_CAUSE_SECURITY_MODE_REJECTED_UNSPECIFIED,
                        OGS_NAS_ESM_CAUSE_PROTOCOL_ERROR_UNSPECIFIED);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
                r = s1ap_send_ue_context_release_command(enb_ue,
                        S1AP_Cause_PR_nas, S1AP_CauseNas_normal_release,
                        S1AP_UE_CTX_REL_UE_CONTEXT_REMOVE, 0);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
                OGS_FSM_TRAN(s, &esm_state_exception);
                break;
            }

            if (!SECURITY_CONTEXT_IS_VALID(mme_ue)) {
                ogs_warn("[%s] No Security Context", mme_ue->imsi_bcd);

                if (!enb_ue) {
                ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) "
                        "(no crash)", mme_ue->imsi_bcd,
                        message->esm.h.message_type);
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }

                r = nas_eps_send_attach_reject(enb_ue, mme_ue,
                        OGS_NAS_EMM_CAUSE_SECURITY_MODE_REJECTED_UNSPECIFIED,
                        OGS_NAS_ESM_CAUSE_PROTOCOL_ERROR_UNSPECIFIED);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
                r = s1ap_send_ue_context_release_command(enb_ue,
                        S1AP_Cause_PR_nas, S1AP_CauseNas_normal_release,
                        S1AP_UE_CTX_REL_UE_CONTEXT_REMOVE, 0);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
                OGS_FSM_TRAN(s, &esm_state_exception);
                break;
            }

            rv = esm_handle_information_response(
                    enb_ue, sess, &message->esm.esm_information_response);
            if (rv != OGS_OK) {
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }
            break;
        case OGS_NAS_EPS_ACTIVATE_DEFAULT_EPS_BEARER_CONTEXT_ACCEPT:
            ogs_debug("Activate default EPS bearer context accept");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            /* T3485 safety net: activation confirmed -> stop retransmit timer
             * and clear the stuck marker (no longer a recovery candidate). */
            CLEAR_BEARER_TIMER(bearer->t3485);
            bearer->reactivation_stuck = 0;
            /* Check if Initial Context Setup Response or
             *          E-RAB Setup Response is received */
            if (MME_HAVE_ENB_S1U_PATH(bearer)) {
                ogs_list_init(&mme_ue->bearer_to_modify_list);
                ogs_list_add(&mme_ue->bearer_to_modify_list,
                                &bearer->to_modify_node);
                ogs_assert(OGS_OK ==
                    mme_gtp_send_modify_bearer_request(enb_ue, mme_ue, 0, 0));
            }

            nas_eps_send_activate_all_dedicated_bearers(bearer);
            OGS_FSM_TRAN(s, esm_state_active);
            break;
        case OGS_NAS_EPS_ACTIVATE_DEDICATED_EPS_BEARER_CONTEXT_ACCEPT:
            ogs_debug("Activate dedicated EPS bearer context accept");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            /* Check if Initial Context Setup Response or 
             *          E-RAB Setup Response is received */
            if (MME_HAVE_ENB_S1U_PATH(bearer)) {
                ogs_assert(OGS_OK ==
                    mme_gtp_send_create_bearer_response(
                        bearer, OGS_GTP2_CAUSE_REQUEST_ACCEPTED));
            }

            OGS_FSM_TRAN(s, esm_state_active);
            break;
        case OGS_NAS_EPS_ACTIVATE_DEDICATED_EPS_BEARER_CONTEXT_REJECT:
            ogs_error("Activate dedicated EPS bearer context reject");
            ogs_error("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            activate_dedicated_eps_bearer_context_reject =
                &message->esm.activate_dedicated_eps_bearer_context_reject;
            ogs_assert(activate_dedicated_eps_bearer_context_reject);
            ogs_assert(OGS_OK ==
                mme_gtp_send_create_bearer_response(bearer,
                gtp_cause_from_esm(
                    activate_dedicated_eps_bearer_context_reject->esm_cause)));
            OGS_FSM_TRAN(s, esm_state_bearer_deactivated);
            break;
        default:
            ogs_error("Unknown message(type:%d)", message->esm.h.message_type);
            break;
        }
        break;
    case MME_EVENT_ESM_TIMER:
        switch (e->timer_id) {
        case MME_TIMER_T3485:
            /*
             * Activate Default Bearer Context Request retransmission timer
             * (stuck IMS default-bearer safety net). The connection is still
             * up (S1 release would have cleared T3485 via CLEAR_BEARER_ALL_
             * TIMERS). Retransmit the saved ESM PDU via Downlink NAS Transport
             * (NOT a fresh E-RAB Setup); this recovers the "ACCEPT lost on a
             * live connection / UE PTI still pending" sub-case.
             *
             * On exhaustion, the bearer is genuinely stuck. Do NOT send a
             * network-initiated Deactivate (the UE rejects it with PTI mismatch
             * since it is not running a matching procedure). Instead release the
             * stuck PDN with an MME-driven Delete Session (leak-free SGW/SMF/UPF
             * teardown); the UE's next PDN connectivity request then creates a
             * fresh session (see mme_bearer_find_or_add_by_message()).
             */
            if (bearer->t3485.retry_count >=
                    mme_timer_cfg(MME_TIMER_T3485)->max_count) {
                enb_ue_t *enb_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
                sgw_ue_t *sgw_ue = sgw_ue_find_by_id(mme_ue->sgw_ue_id);
                ogs_warn("[%s] T3485 exhausted for EBI[%d]; releasing stuck "
                        "PDN session", mme_ue->imsi_bcd, bearer->ebi);
                CLEAR_BEARER_TIMER(bearer->t3485);
                bearer->reactivation_stuck = 0;
                if (sgw_ue && MME_HAVE_SGW_S1U_PATH(sess) &&
                        !sess->deletion_in_progress) {
                    sess->deletion_in_progress = true;
                    ogs_expect(OGS_OK ==
                        mme_gtp_send_delete_session_request(
                            enb_ue, sgw_ue, sess, OGS_GTP_DELETE_NO_ACTION));
                }
            } else if (ECM_CONNECTED(mme_ue)) {
                enb_ue_t *enb_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
                bearer->t3485.retry_count++;
                if (enb_ue && bearer->t3485.pkbuf) {
                    ogs_pkbuf_t *copy = ogs_pkbuf_copy(bearer->t3485.pkbuf);
                    if (copy) {
                        r = nas_eps_send_to_downlink_nas_transport(
                                enb_ue, copy);
                        ogs_expect(r == OGS_OK);
                    }
                    ogs_timer_start(bearer->t3485.timer,
                            mme_timer_cfg(MME_TIMER_T3485)->duration);
                }
            }
            break;
        case MME_TIMER_T3489:
            if (bearer->t3489.retry_count >=
                    mme_timer_cfg(MME_TIMER_T3489)->max_count) {
                ogs_warn("Retransmission of IMSI[%s] failed. "
                        "Stop retransmission", mme_ue->imsi_bcd);
                OGS_FSM_TRAN(&bearer->sm, &esm_state_exception);

                r = nas_eps_send_pdn_connectivity_reject(sess,
                        OGS_NAS_ESM_CAUSE_ESM_INFORMATION_NOT_RECEIVED,
                        e->create_action);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
            } else {
                bearer->t3489.retry_count++;
                r = nas_eps_send_esm_information_request(bearer);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
            }
            break;
        default:
            ogs_error("Unknown timer[%s:%d]",
                    mme_timer_get_name(e->timer_id), e->timer_id);
            break;
        }
        break;
    default:
        ogs_error("Unknown event %s", mme_event_get_name(e));
        break;
    }
}

void esm_state_active(ogs_fsm_t *s, mme_event_t *e)
{
    int r, rv;
    mme_ue_t *mme_ue = NULL;
    enb_ue_t *enb_ue = NULL;
    sgw_ue_t *sgw_ue = NULL;
    mme_sess_t *sess = NULL;
    mme_bearer_t *bearer = NULL;
    ogs_nas_eps_message_t *message = NULL;

    ogs_assert(s);
    ogs_assert(e);

    mme_sm_debug(e);

    bearer = mme_bearer_find_by_id(e->bearer_id);
    ogs_assert(bearer);
    sess = mme_sess_find_by_id(bearer->sess_id);
    ogs_assert(sess);
    mme_ue = mme_ue_find_by_id(sess->mme_ue_id);
    ogs_assert(mme_ue);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        break;
    case OGS_FSM_EXIT_SIG:
        break;
    case MME_EVENT_ESM_MESSAGE:
        message = e->nas_message;
        ogs_assert(message);

        enb_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);

        switch (message->esm.h.message_type) {
        case OGS_NAS_EPS_PDN_CONNECTIVITY_REQUEST:
            ogs_debug("PDN Connectivity request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            if (!enb_ue) {
                ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) "
                        "(no crash)", mme_ue->imsi_bcd,
                        message->esm.h.message_type);
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }
            rv = esm_handle_pdn_connectivity_request(
                    enb_ue, bearer, &message->esm.pdn_connectivity_request,
                    e->create_action);
            if (rv != OGS_OK) {
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }

            OGS_FSM_TRAN(s, esm_state_inactive);
            break;
        case OGS_NAS_EPS_PDN_DISCONNECT_REQUEST:
            ogs_debug("PDN disconnect request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);

            if (!enb_ue) {
                ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) "
                        "(no crash)", mme_ue->imsi_bcd,
                        message->esm.h.message_type);
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }
            if (MME_HAVE_SGW_S1U_PATH(sess)) {
                sgw_ue = sgw_ue_find_by_id(mme_ue->sgw_ue_id);
                ogs_assert(sgw_ue);

                ogs_assert(OGS_OK ==
                    mme_gtp_send_delete_session_request(enb_ue, sgw_ue, sess,
                    OGS_GTP_DELETE_SEND_DEACTIVATE_BEARER_CONTEXT_REQUEST));
                /* Mark the core delete as issued (DSReq sent, DSResp not yet
                 * received) so the pdn_will_disconnect stuck-recovery gate
                 * can later confirm intent + core-delete completion. */
                sess->deletion_in_progress = true;
                sess->core_delete_done = false;
            } else {
                r = nas_eps_send_deactivate_bearer_context_request(bearer);
                ogs_expect(r == OGS_OK);
                ogs_assert(r != OGS_ERROR);
            }

            CLEAR_SGW_S1U_PATH(sess);

            OGS_FSM_TRAN(s, esm_state_pdn_will_disconnect);
            break;

        case OGS_NAS_EPS_MODIFY_EPS_BEARER_CONTEXT_ACCEPT:
            ogs_debug("Modify EPS bearer context accept");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);

            ogs_assert(OGS_OK ==
                mme_gtp_send_update_bearer_response(
                    bearer, OGS_GTP2_CAUSE_REQUEST_ACCEPTED));
            break;
        case OGS_NAS_EPS_DEACTIVATE_EPS_BEARER_CONTEXT_ACCEPT:
            ogs_debug("Deactivate EPS bearer "
                    "context accept");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            CLEAR_BEARER_TIMER(bearer->t3495);
            ogs_assert(OGS_OK ==
                mme_gtp_send_delete_bearer_response(
                    bearer, OGS_GTP2_CAUSE_REQUEST_ACCEPTED));
            OGS_FSM_TRAN(s, esm_state_bearer_deactivated);
            break;
        case OGS_NAS_EPS_BEARER_RESOURCE_ALLOCATION_REQUEST:
            ogs_debug("Bearer resource allocation request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            if (!enb_ue) {
                ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) "
                        "(no crash)", mme_ue->imsi_bcd,
                        message->esm.h.message_type);
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }
            esm_handle_bearer_resource_allocation_request(
                    enb_ue, bearer, message);
            break;
        case OGS_NAS_EPS_BEARER_RESOURCE_MODIFICATION_REQUEST:
            ogs_debug("Bearer resource modification request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            if (!enb_ue) {
                ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) "
                        "(no crash)", mme_ue->imsi_bcd,
                        message->esm.h.message_type);
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }
            esm_handle_bearer_resource_modification_request(
                    enb_ue, bearer, message);
            break;
        default:
            ogs_error("Unknown message(type:%d)", 
                    message->esm.h.message_type);
            break;
        }
        break;
    case MME_EVENT_ESM_TIMER:
        switch (e->timer_id) {
        case MME_TIMER_T3495:
            if (bearer->t3495.retry_count >=
                    mme_timer_cfg(MME_TIMER_T3495)->max_count) {
                ogs_warn("[%s] Retransmission of Deactivate EPS Bearer "
                        "Context Request for EBI[%d] failed. "
                        "Stop retransmission",
                        mme_ue->imsi_bcd, bearer->ebi);
                ogs_assert(OGS_OK ==
                    mme_gtp_send_delete_bearer_response(
                        bearer, OGS_GTP2_CAUSE_UE_NOT_RESPONDING));
                OGS_FSM_TRAN(s, esm_state_bearer_deactivated);
            } else {
                bearer->t3495.retry_count++;
                if (ECM_CONNECTED(mme_ue)) {
                    r = nas_eps_send_deactivate_bearer_context_request(
                            bearer);
                    ogs_expect(r == OGS_OK);
                    ogs_assert(r != OGS_ERROR);
                } else {
                    ogs_timer_start(bearer->t3495.timer,
                            mme_timer_cfg(MME_TIMER_T3495)->duration);
                }
            }
            break;
        default:
            ogs_error("Unknown timer[%s:%d]",
                    mme_timer_get_name(e->timer_id), e->timer_id);
            break;
        }
        break;
    default:
        ogs_error("Unknown event %s", mme_event_get_name(e));
        break;
    }
}

void esm_state_pdn_will_disconnect(ogs_fsm_t *s, mme_event_t *e)
{
    int rv;
    mme_ue_t *mme_ue = NULL;
    enb_ue_t *enb_ue = NULL;
    mme_sess_t *sess = NULL;
    mme_bearer_t *bearer = NULL;
    ogs_nas_eps_message_t *message = NULL;

    ogs_assert(s);
    ogs_assert(e);

    mme_sm_debug(e);

    bearer = mme_bearer_find_by_id(e->bearer_id);
    ogs_assert(bearer);
    sess = mme_sess_find_by_id(bearer->sess_id);
    ogs_assert(sess);
    mme_ue = mme_ue_find_by_id(sess->mme_ue_id);
    ogs_assert(mme_ue);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        break;
    case OGS_FSM_EXIT_SIG:
        break;
    case MME_EVENT_ESM_MESSAGE:
        message = e->nas_message;
        ogs_assert(message);

        enb_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
        if (!enb_ue) {
            ogs_warn("[%s] No eNB-UE context; abort ESM message(type:%d) - "
                    "S1 context released mid-procedure (no crash)",
                    mme_ue->imsi_bcd, message->esm.h.message_type);
            OGS_FSM_TRAN(s, esm_state_exception);
            break;
        }

        switch (message->esm.h.message_type) {
        case OGS_NAS_EPS_DEACTIVATE_EPS_BEARER_CONTEXT_ACCEPT:
            ogs_debug("[D] Deactivate EPS bearer "
                    "context accept");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            /* Stop T3495 retransmission timer on normal completion.
             * Mirrors the explicit clear in esm_state_active (line above),
             * preventing spurious retransmission between Accept and the
             * eventual bearer free (CLEAR_BEARER_ALL_TIMERS via
             * mme-context.c bearer remove path). */
            CLEAR_BEARER_TIMER(bearer->t3495);
            OGS_FSM_TRAN(s, esm_state_pdn_did_disconnect);
            break;
        case OGS_NAS_EPS_PDN_CONNECTIVITY_REQUEST:
            ogs_debug("PDN Connectivity request");
            ogs_debug("    IMSI[%s] PTI[%d] EBI[%d]",
                    mme_ue->imsi_bcd, sess->pti, bearer->ebi);
            rv = esm_handle_pdn_connectivity_request(
                    enb_ue, bearer, &message->esm.pdn_connectivity_request,
                    e->create_action);
            if (rv != OGS_OK) {
                OGS_FSM_TRAN(s, esm_state_exception);
                break;
            }

            OGS_FSM_TRAN(s, esm_state_inactive);
            break;
        case OGS_NAS_EPS_PDN_DISCONNECT_REQUEST:
            /*
             * Retransmitted PDN disconnect while still waiting for the UE's
             * Deactivate EPS Bearer Context Accept. Normally the Accept
             * drives us to esm_state_pdn_did_disconnect; but if the eNB
             * idled the radio (E-RAB Release failed with user-inactivity)
             * that Accept never arrives, and the UE keeps retransmitting the
             * PDN disconnect. Previously these fell into the default branch
             * ("Unknown message") and were dropped, leaving the bearer stuck
             * here until a later TAU bearer-status mismatch (~tens of
             * seconds), long enough to drop an in-progress call.
             *
             * Treat the retransmit as a recovery trigger, but ONLY under the
             * shared safe condition (identical to the E-RAB Release failure
             * cleanup path): the core (SGW/SMF) delete is genuinely complete.
             *   - sess->deletion_in_progress  (we did issue the DSReq)
             *   - sess->core_delete_done      (DSResp received, ownership-
             *     guard-verified; not a timed-out xact)
             *   - this bearer is the session default bearer
             * When satisfied, stop T3495 and transition to
             * esm_state_pdn_did_disconnect; the outer ESM dispatcher
             * (mme-sm.c) then performs MME_SESS_CLEAR() safely -- we do NOT
             * free in place here (the dispatcher still dereferences
             * bearer/sess after this returns). If the condition does not
             * hold (core delete not yet confirmed), fall through to the
             * default "Unknown message" handling so a genuine in-flight
             * Accept handshake is never broken.
             */
            {
                mme_bearer_t *def = mme_default_bearer_in_sess(sess);
                if (def && def == bearer &&
                        sess->deletion_in_progress &&
                        sess->core_delete_done) {
                    ogs_warn("[%s] PDN disconnect retransmit; core delete "
                            "complete, no Deactivate Accept expected - "
                            "completing local PDN[%s] teardown (EBI:%d)",
                            mme_ue->imsi_bcd,
                            sess->session ? sess->session->name : "unknown",
                            bearer->ebi);
                    CLEAR_BEARER_TIMER(bearer->t3495);
                    OGS_FSM_TRAN(s, esm_state_pdn_did_disconnect);
                    break;
                }
            }
            ogs_error("Unknown message(type:%d)",
                    message->esm.h.message_type);
            break;
        default:
            ogs_error("Unknown message(type:%d)",
                    message->esm.h.message_type);
            break;
        }
        break;

    default:
        ogs_error("Unknown event %s", mme_event_get_name(e));
        break;
    }
}

void esm_state_pdn_did_disconnect(ogs_fsm_t *s, mme_event_t *e)
{
    ogs_assert(e);
    mme_sm_debug(e);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        break;
    case OGS_FSM_EXIT_SIG:
        break;
    default:
        ogs_error("Unknown event %s", mme_event_get_name(e));
        break;
    }
}

void esm_state_bearer_deactivated(ogs_fsm_t *s, mme_event_t *e)
{
    ogs_assert(e);
    mme_sm_debug(e);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        break;
    case OGS_FSM_EXIT_SIG:
        break;
    default:
        ogs_error("Unknown event %s", mme_event_get_name(e));
        break;
    }
}

void esm_state_exception(ogs_fsm_t *s, mme_event_t *e)
{
    mme_bearer_t *bearer = NULL;
    ogs_assert(e);
    mme_sm_debug(e);

    bearer = mme_bearer_find_by_id(e->bearer_id);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        CLEAR_BEARER_ALL_TIMERS(bearer);
        break;
    case OGS_FSM_EXIT_SIG:
        break;
    default:
        ogs_error("Unknown event %s", mme_event_get_name(e));
        break;
    }
}
