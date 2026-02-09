/*
 * Copyright (C) 2019-2024 by Sukchan Lee <acetcom@gmail.com>
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
#include "fd-path.h"
#include "pfcp-path.h"
#include "sbi-path.h"
#include "s5c-handler.h"
#include "gn-handler.h"
#include "gx-handler.h"
#include "gy-handler.h"
#include "nnrf-handler.h"
#include "namf-handler.h"
#include "nsmf-handler.h"
#include "npcf-handler.h"

void smf_state_initial(ogs_fsm_t *s, smf_event_t *e)
{
    smf_sm_debug(e);

    ogs_assert(s);

    OGS_FSM_TRAN(s, &smf_state_operational);
}

void smf_state_final(ogs_fsm_t *s, smf_event_t *e)
{
    smf_sm_debug(e);

    ogs_assert(s);
}

void smf_state_operational(ogs_fsm_t *s, smf_event_t *e)
{
    int rv;
    const char *api_version = NULL;

    ogs_pkbuf_t *recvbuf = NULL;
    smf_sess_t *sess = NULL;
    smf_ue_t *smf_ue = NULL;
    smf_gtp_node_t *smf_gnode = NULL;

    ogs_gtp_xact_t *gtp_xact = NULL;
    ogs_gtp2_message_t gtp2_message;
    ogs_gtp2_sender_f_teid_t gtp2_sender_f_teid;
    ogs_gtp1_message_t gtp1_message;

    ogs_diam_gx_message_t *gx_message = NULL;
    ogs_diam_gy_message_t *gy_message = NULL;
    ogs_diam_s6b_message_t *s6b_message = NULL;

    ogs_pfcp_node_t *pfcp_node = NULL;
    ogs_pfcp_xact_t *pfcp_xact = NULL;
    ogs_pfcp_message_t *pfcp_message = NULL;

    ogs_sbi_stream_t *stream = NULL;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;
    ogs_sbi_request_t *sbi_request = NULL;

    ogs_sbi_nf_instance_t *nf_instance = NULL;
    ogs_sbi_subscription_data_t *subscription_data = NULL;
    ogs_sbi_response_t *sbi_response = NULL;
    ogs_sbi_message_t sbi_message;
    ogs_sbi_xact_t *sbi_xact = NULL;
    ogs_pool_id_t sbi_xact_id = OGS_INVALID_POOL_ID;
    ogs_pool_id_t sbi_object_id = OGS_INVALID_POOL_ID;

    ogs_nas_5gs_message_t nas_message;
    ogs_pkbuf_t *pkbuf = NULL;

    smf_sm_debug(e);

    ogs_assert(s);

    switch (e->h.id) {
    case OGS_FSM_ENTRY_SIG:
        break;

    case OGS_FSM_EXIT_SIG:
        break;

    case SMF_EVT_S5C_MESSAGE:
        ogs_assert(e);
        recvbuf = e->pkbuf;
        ogs_assert(recvbuf);

        smf_gnode = e->gnode;
        ogs_assert(smf_gnode);

        if (ogs_gtp2_parse_msg(&gtp2_message, recvbuf) != OGS_OK) {
            ogs_error("ogs_gtp2_parse_msg() failed for message type %d", 
                     recvbuf->len > 1 ? recvbuf->data[1] : 0);
            smf_metrics_inst_global_inc(SMF_METR_GLOB_CTR_S5C_RX_PARSE_FAILED);
            smf_metrics_inst_gtp_node_inc(smf_gnode->metrics, SMF_METR_GTP_NODE_CTR_S5C_RX_PARSE_FAILED);
            ogs_pkbuf_free(recvbuf);
            break;
        }
        
        ogs_info("SMF PARSED GTP message type: %d, TEID: 0x%x", 
                 gtp2_message.h.type, gtp2_message.h.teid);
        e->gtp2_message = &gtp2_message;

        ogs_gtp2_sender_f_teid(&gtp2_sender_f_teid, &gtp2_message);

        rv = ogs_gtp_xact_receive(smf_gnode->gnode, &gtp2_message.h, &gtp_xact);
        if (rv == OGS_RETRY) {
            /* This is a duplicate message that was already handled */
            ogs_debug("Duplicate message type %d - already processed", gtp2_message.h.type);
            ogs_pkbuf_free(recvbuf);
            break;
        }
        if (rv != OGS_OK) {
            ogs_error("ogs_gtp_xact_receive() failed with rv=%d for message type %d", rv, gtp2_message.h.type);
            ogs_pkbuf_free(recvbuf);
            break;
        }
        e->gtp_xact_id = gtp_xact ? gtp_xact->id : OGS_INVALID_POOL_ID;

        if (gtp2_message.h.teid_presence && gtp2_message.h.teid != 0) {
            ogs_debug("Message type %d has TEID: 0x%x, looking up session", 
                     gtp2_message.h.type, gtp2_message.h.teid);
            sess = smf_sess_find_by_teid(gtp2_message.h.teid);
            if (!sess) {
                ogs_debug("No session found for TEID: 0x%x, message type: %d", 
                         gtp2_message.h.teid, gtp2_message.h.type);
            }
        } else if (gtp_xact->local_teid) { /* rx no TEID or TEID=0 */
            /* 3GPP TS 29.274 5.5.2: we receive TEID=0 under some
             * conditions, such as cause "Session context not found". In those
             * cases, we still want to identify the local session which
             * originated the message, so try harder by using the TEID we
             * locally stored in xact when sending the original request: */
            ogs_debug("Message type %d has no TEID, trying local_teid: 0x%x", 
                     gtp2_message.h.type, gtp_xact->local_teid);
            sess = smf_sess_find_by_teid(gtp_xact->local_teid);
        } else {
            ogs_debug("Message type %d has no TEID and no local_teid", gtp2_message.h.type);
        }

        ogs_info("SMF PROCESSING GTP message type: %d", gtp2_message.h.type);
        
        switch(gtp2_message.h.type) {
        case OGS_GTP2_ECHO_REQUEST_TYPE:
            smf_s5c_handle_echo_request(gtp_xact, &gtp2_message.echo_request);
            break;
        case OGS_GTP2_ECHO_RESPONSE_TYPE:
            smf_s5c_handle_echo_response(gtp_xact, &gtp2_message.echo_response);
            break;
        case OGS_GTP2_CREATE_SESSION_REQUEST_TYPE:
            smf_metrics_inst_global_inc(SMF_METR_GLOB_CTR_S5C_RX_CREATESESSIONREQ);
            smf_metrics_inst_gtp_node_inc(smf_gnode->metrics, SMF_METR_GTP_NODE_CTR_S5C_RX_CREATESESSIONREQ);
            if (gtp2_message.h.teid == 0) {
                ogs_expect(!sess);
                sess = smf_sess_add_by_gtp2_message(&gtp2_message);
                if (sess)
                    OGS_SETUP_GTP_NODE(sess, smf_gnode->gnode);
            }
            if (!sess) {
                ogs_error("No Session");
                ogs_gtp2_send_error_message(gtp_xact,
                        gtp2_sender_f_teid.teid_presence == true ?
                            gtp2_sender_f_teid.teid : 0,
                        OGS_GTP2_CREATE_SESSION_RESPONSE_TYPE,
                        OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND);
                break;
            }

            if (gtp2_sender_f_teid.teid_presence == true)
                sess->sgw_s5c_teid = gtp2_sender_f_teid.teid;

            ogs_debug("    SGW_S5C_TEID[0x%x], Sender F-TEID(%d)[0x%x]",
                    sess->sgw_s5c_teid,
                    gtp2_sender_f_teid.teid_presence, gtp2_sender_f_teid.teid);

            e->sess_id = sess->id;
            ogs_fsm_dispatch(&sess->sm, e);
            break;
        case OGS_GTP2_DELETE_SESSION_REQUEST_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            smf_metrics_inst_global_inc(SMF_METR_GLOB_CTR_S5C_RX_DELETESESSIONREQ);
            smf_metrics_inst_gtp_node_inc(smf_gnode->metrics, SMF_METR_GTP_NODE_CTR_S5C_RX_DELETESESSIONREQ);
            if (!sess) {
                ogs_error("No Session");
                ogs_gtp2_send_error_message(gtp_xact,
                        gtp2_sender_f_teid.teid_presence == true ?
                            gtp2_sender_f_teid.teid : 0,
                        OGS_GTP2_DELETE_SESSION_RESPONSE_TYPE,
                        OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND);
                break;
            }
            if (gtp2_sender_f_teid.teid_presence == true) {
                if (sess->sgw_s5c_teid != gtp2_sender_f_teid.teid) {
                    ogs_error("Invalid Sender F-TEID [0x%x != 0x%x]",
                        sess->sgw_s5c_teid, gtp2_sender_f_teid.teid);
                    ogs_gtp2_send_error_message(gtp_xact,
                            gtp2_sender_f_teid.teid_presence == true ?
                                gtp2_sender_f_teid.teid : 0,
                            OGS_GTP2_DELETE_SESSION_RESPONSE_TYPE,
                            OGS_GTP2_CAUSE_INVALID_MESSAGE_FORMAT);
                    break;
                }
            }
            e->sess_id = sess->id;
            ogs_fsm_dispatch(&sess->sm, e);
            break;
        case OGS_GTP2_MODIFY_BEARER_REQUEST_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            smf_s5c_handle_modify_bearer_request(
                sess, gtp_xact, recvbuf,
                &gtp2_message.modify_bearer_request, &gtp2_sender_f_teid);
            break;
        case OGS_GTP2_CREATE_BEARER_RESPONSE_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            if (!sess) {
                uint8_t gtp_cause = OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND;
                ogs_warn("Create Bearer Response for removed session - SGW may have removed the session");
                /* Get cause from response if available */
                if (gtp2_message.create_bearer_response.cause.presence && 
                    gtp2_message.create_bearer_response.cause.len > 0 &&
                    gtp2_message.create_bearer_response.cause.data) {
                    gtp_cause = *((uint8_t*)gtp2_message.create_bearer_response.cause.data);
                }
                ogs_info("Received cause [%d] from SGW for removed session", gtp_cause);
                
                /* Complete the transaction to avoid leaks */
                rv = ogs_gtp_xact_commit(gtp_xact);
                ogs_expect(rv == OGS_OK);
                break;
            }
            smf_s5c_handle_create_bearer_response(
                sess, gtp_xact, &gtp2_message.create_bearer_response);
            break;
        case OGS_GTP2_UPDATE_BEARER_RESPONSE_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            if (!sess) {
                ogs_error("No Session");
                rv = ogs_gtp_xact_commit(gtp_xact);
                ogs_expect(rv == OGS_OK);
                break;
            }
            smf_s5c_handle_update_bearer_response(
                sess, gtp_xact, &gtp2_message.update_bearer_response);
            break;
        case OGS_GTP2_DELETE_BEARER_RESPONSE_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            if (!sess) {
                ogs_error("No Session");
                rv = ogs_gtp_xact_commit(gtp_xact);
                ogs_expect(rv == OGS_OK);
                break;
            }
            e->sess_id = sess->id;
            ogs_fsm_dispatch(&sess->sm, e);
            break;
        case OGS_GTP2_BEARER_RESOURCE_COMMAND_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            smf_s5c_handle_bearer_resource_command(
                sess, gtp_xact,
                &gtp2_message.bearer_resource_command, &gtp2_sender_f_teid);
            break;
        case OGS_GTP2_DELETE_BEARER_COMMAND_TYPE:
            if (!gtp2_message.h.teid_presence) ogs_error("No TEID");
            if (!sess) {
                ogs_error("No Session for Delete Bearer Command (TEID: 0x%x)",
                        gtp2_message.h.teid);
                ogs_gtp2_send_error_message(gtp_xact,
                        gtp2_sender_f_teid.teid_presence == true ?
                            gtp2_sender_f_teid.teid : 0,
                        OGS_GTP2_DELETE_BEARER_FAILURE_INDICATION_TYPE,
                        OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND);
                break;
            }
            smf_s5c_handle_delete_bearer_command(
                sess, gtp_xact, &gtp2_message.delete_bearer_command);
            break;
        case OGS_GTP2_CHANGE_NOTIFICATION_REQUEST_TYPE:
            ogs_info("Received Change Notification Request in session-dependent section");
            
            /* Build Change Notification Response (Type 39) */
            {
                ogs_gtp2_header_t h;
                ogs_pkbuf_t *pkbuf = NULL;
                uint8_t *p;
                int payload_len = 5; /* Cause IE: 5 bytes */
                
                ogs_info("Building Change Notification Response header");
                memset(&h, 0, sizeof(ogs_gtp2_header_t));
                h.type = OGS_GTP2_CHANGE_NOTIFICATION_RESPONSE_TYPE;
                h.teid = gtp2_sender_f_teid.teid_presence == true ? gtp2_sender_f_teid.teid : 0;
                
                ogs_info("Response TEID: 0x%x, Sender TEID present: %s", 
                         h.teid, gtp2_sender_f_teid.teid_presence ? "YES" : "NO");
                
                /* Allocate packet buffer with proper headroom for GTP headers */
                ogs_info("Allocating packet buffer with headroom, payload size: %d bytes", payload_len);
                pkbuf = ogs_pkbuf_alloc(NULL, OGS_GTPV2C_HEADER_LEN + payload_len);
                if (!pkbuf) {
                    ogs_error("ogs_pkbuf_alloc() failed for Change Notification Response");
                    break;
                }
                
                /* Reserve headroom for GTP headers that will be added by ogs_gtp_xact_update_tx() */
                ogs_pkbuf_reserve(pkbuf, OGS_GTPV2C_HEADER_LEN);
                ogs_pkbuf_put(pkbuf, payload_len);
                p = pkbuf->data;
                
                ogs_info("Adding Cause IE with REQUEST_ACCEPTED");
                /* Add Cause IE: Type=2, Length=1, Value=REQUEST_ACCEPTED(16) */
                *p++ = 2;   /* Cause IE type */
                *(uint16_t*)p = htobe16(1); p += 2; /* Length = 1 */
                *p++ = 0;   /* Instance/flags */
                *p++ = OGS_GTP2_CAUSE_REQUEST_ACCEPTED; /* Cause value = 16 */
                
                ogs_info("Payload built successfully, calling ogs_gtp_xact_update_tx()");
                rv = ogs_gtp_xact_update_tx(gtp_xact, &h, pkbuf);
                if (rv != OGS_OK) {
                    ogs_error("ogs_gtp_xact_update_tx() failed with rv=%d", rv);
                    ogs_pkbuf_free(pkbuf);
                    break;
                }
                
                ogs_info("Response packet stored in transaction, calling ogs_gtp_xact_commit()");
                rv = ogs_gtp_xact_commit(gtp_xact);
                if (rv != OGS_OK) {
                    ogs_error("ogs_gtp_xact_commit() failed with rv=%d", rv);
                } else {
                    ogs_info("Change Notification Response sent successfully!");
                }
            }
            break;
        default:
            ogs_warn("Not implemented(type:%d)", gtp2_message.h.type);
            break;
        }
        ogs_pkbuf_free(recvbuf);
        break;

    case SMF_EVT_GN_MESSAGE:
        ogs_assert(e);
        recvbuf = e->pkbuf;
        ogs_assert(recvbuf);

        smf_gnode = e->gnode;
        ogs_assert(smf_gnode);

        if (ogs_gtp1_parse_msg(&gtp1_message, recvbuf) != OGS_OK) {
            ogs_error("ogs_gtp2_parse_msg() failed");
            smf_metrics_inst_global_inc(SMF_METR_GLOB_CTR_GN_RX_PARSE_FAILED);
            smf_metrics_inst_gtp_node_inc(smf_gnode->metrics, SMF_METR_GTP_NODE_CTR_GN_RX_PARSE_FAILED);
            ogs_pkbuf_free(recvbuf);
            break;
        }
        e->gtp1_message = &gtp1_message;

        if (gtp1_message.h.teid != 0) {
            sess = smf_sess_find_by_teid(gtp1_message.h.teid);
        }

        rv = ogs_gtp1_xact_receive(smf_gnode->gnode, &gtp1_message.h, &gtp_xact);
        if (rv != OGS_OK) {
            ogs_pkbuf_free(recvbuf);
            break;
        }
        e->gtp_xact_id = gtp_xact ? gtp_xact->id : OGS_INVALID_POOL_ID;

        switch(gtp1_message.h.type) {
        case OGS_GTP1_ECHO_REQUEST_TYPE:
            smf_gn_handle_echo_request(gtp_xact, &gtp1_message.echo_request);
            break;
        case OGS_GTP1_ECHO_RESPONSE_TYPE:
            smf_gn_handle_echo_response(gtp_xact, &gtp1_message.echo_response);
            break;
        case OGS_GTP1_CREATE_PDP_CONTEXT_REQUEST_TYPE:
            smf_metrics_inst_global_inc(SMF_METR_GLOB_CTR_GN_RX_CREATEPDPCTXREQ);
            smf_metrics_inst_gtp_node_inc(smf_gnode->metrics, SMF_METR_GTP_NODE_CTR_GN_RX_CREATEPDPCTXREQ);
            if (gtp1_message.h.teid == 0) {
                ogs_expect(!sess);
                sess = smf_sess_add_by_gtp1_message(&gtp1_message);
                if (sess)
                    OGS_SETUP_GTP_NODE(sess, smf_gnode->gnode);
            }
            if (!sess) {
                ogs_gtp1_send_error_message(gtp_xact, 0,
                        OGS_GTP1_CREATE_PDP_CONTEXT_RESPONSE_TYPE,
                        OGS_GTP1_CAUSE_CONTEXT_NOT_FOUND);
                break;
            }
            e->sess_id = sess->id;
            ogs_fsm_dispatch(&sess->sm, e);
            break;
        case OGS_GTP1_DELETE_PDP_CONTEXT_REQUEST_TYPE:
            smf_metrics_inst_global_inc(SMF_METR_GLOB_CTR_GN_RX_DELETEPDPCTXREQ);
            smf_metrics_inst_gtp_node_inc(smf_gnode->metrics, SMF_METR_GTP_NODE_CTR_GN_RX_DELETEPDPCTXREQ);
            if (!sess) {
                ogs_gtp1_send_error_message(gtp_xact, 0,
                        OGS_GTP1_DELETE_PDP_CONTEXT_RESPONSE_TYPE,
                        OGS_GTP1_CAUSE_NON_EXISTENT);
                break;
            }
            e->sess_id = sess->id;
            ogs_fsm_dispatch(&sess->sm, e);
            break;
        case OGS_GTP1_UPDATE_PDP_CONTEXT_REQUEST_TYPE:
            smf_gn_handle_update_pdp_context_request(
                sess, gtp_xact, &gtp1_message.update_pdp_context_request);
            break;
        case OGS_GTP1_ERROR_INDICATION_TYPE:
            /* TS 29.060 10.1.1.4 dst port shall be the userplane port (2152) */
            ogs_error("Rx unexpected Error Indication in GTPC port");
            break;
        default:
            ogs_warn("Not implemented(type:%d)", gtp1_message.h.type);
            break;
        }
        ogs_pkbuf_free(recvbuf);
        break;

    case SMF_EVT_GX_MESSAGE:
        ogs_assert(e);
        gx_message = e->gx_message;
        ogs_assert(gx_message);

        sess = smf_sess_find_by_id(e->sess_id);
        ogs_assert(sess);

        switch(gx_message->cmd_code) {
        case OGS_DIAM_GX_CMD_CODE_CREDIT_CONTROL:
            switch(gx_message->cc_request_type) {
            case OGS_DIAM_GX_CC_REQUEST_TYPE_INITIAL_REQUEST:
                ogs_fsm_dispatch(&sess->sm, e);
                break;
            case OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST:
                ogs_fsm_dispatch(&sess->sm, e);
                break;
            default:
                ogs_error("Not implemented(%d)", gx_message->cc_request_type);
                break;
            }

            break;
        case OGS_DIAM_GX_CMD_RE_AUTH:
            smf_gx_handle_re_auth_request(sess, gx_message);
            break;
        default:
            ogs_error("Invalid type(%d)", gx_message->cmd_code);
            break;
        }

        OGS_SESSION_DATA_FREE(&gx_message->session_data);
        ogs_free(gx_message);
        break;

    case SMF_EVT_GY_MESSAGE:
        ogs_assert(e);
        gy_message = e->gy_message;
        ogs_assert(gy_message);

        sess = smf_sess_find_by_id(e->sess_id);
        ogs_assert(sess);

        switch(gy_message->cmd_code) {
        case OGS_DIAM_GY_CMD_CODE_CREDIT_CONTROL:
             ogs_fsm_dispatch(&sess->sm, e);
            break;
        case OGS_DIAM_GY_CMD_RE_AUTH:
            smf_gy_handle_re_auth_request(sess, gy_message);
            break;
        default:
            ogs_error("Invalid type(%d)", gy_message->cmd_code);
            break;
        }

        ogs_free(gy_message);
        break;

    case SMF_EVT_S6B_MESSAGE:
        ogs_assert(e);
        s6b_message = e->s6b_message;
        ogs_assert(s6b_message);
        sess = smf_sess_find_by_id(e->sess_id);
        if (!sess) {
            /* Session might have been deleted by previous Gx/Gy message handling */
            ogs_debug("Session not found for S6B message (session may have been deleted)");
            ogs_free(s6b_message);
            break;
        }

        /* Check if session is in a valid state for S6B messages */
        ogs_fsm_handler_t current_state = sess->sm.state;
        if (current_state == (ogs_fsm_handler_t)smf_gsm_state_epc_session_will_release ||
            current_state == (ogs_fsm_handler_t)smf_gsm_state_exception) {
            /* If this is a termination answer and we have a stored transaction, send DSResp */
            if (s6b_message->cmd_code == OGS_DIAM_S6B_CMD_SESSION_TERMINATION &&
                sess->gtp.stored_xact) {
                ogs_debug("Session[%d] in release state - sending delayed DSResp", e->sess_id);
                smf_gtp2_send_delete_session_response(sess, sess->gtp.stored_xact);
                sess->gtp.stored_xact = NULL;
            } else {
                ogs_debug("Session[%d] in release/exception state - ignoring S6B message", e->sess_id);
            }
            ogs_free(s6b_message);
            break;
        }

        switch(s6b_message->cmd_code) {
        case OGS_DIAM_S6B_CMD_AUTHENTICATION_AUTHORIZATION:
        case OGS_DIAM_S6B_CMD_SESSION_TERMINATION:
            ogs_fsm_dispatch(&sess->sm, e);
            break;
        default:
            ogs_error("Invalid type(%d)", s6b_message->cmd_code);
            break;
        }

        ogs_free(s6b_message);
        break;

    case SMF_EVT_N4_MESSAGE:
        ogs_assert(e);
        recvbuf = e->pkbuf;
        ogs_assert(recvbuf);
        pfcp_node = e->pfcp_node;
        ogs_assert(pfcp_node);
        ogs_assert(OGS_FSM_STATE(&pfcp_node->sm));

        /*
         * Issue #1911
         *
         * Because ogs_pfcp_message_t is over 80kb in size,
         * it can cause stack overflow.
         * To avoid this, the pfcp_message structure uses heap memory.
         */
        if ((pfcp_message = ogs_pfcp_parse_msg(recvbuf)) == NULL) {
            ogs_error("ogs_pfcp_parse_msg() failed");
            ogs_pkbuf_free(recvbuf);
            break;
        }

        rv = ogs_pfcp_xact_receive(pfcp_node, &pfcp_message->h, &pfcp_xact);
        if (rv != OGS_OK) {
            ogs_pkbuf_free(recvbuf);
            ogs_pfcp_message_free(pfcp_message);
            break;
        }

        e->pfcp_message = pfcp_message;
        e->pfcp_xact_id = pfcp_xact ? pfcp_xact->id : OGS_INVALID_POOL_ID;

        e->gtp2_message = NULL;
        if (pfcp_xact->gtpbuf) {
            rv = ogs_gtp2_parse_msg(&gtp2_message, pfcp_xact->gtpbuf);
            e->gtp2_message = &gtp2_message;
        }

        ogs_fsm_dispatch(&pfcp_node->sm, e);
        if (OGS_FSM_CHECK(&pfcp_node->sm, smf_pfcp_state_exception)) {
            ogs_error("PFCP state machine exception");
        }

        ogs_pkbuf_free(recvbuf);
        ogs_pfcp_message_free(pfcp_message);
        break;
    case SMF_EVT_N4_TIMER:
    case SMF_EVT_N4_NO_HEARTBEAT:
        ogs_assert(e);
        pfcp_node = e->pfcp_node;
        ogs_assert(pfcp_node);
        ogs_assert(OGS_FSM_STATE(&pfcp_node->sm));

        ogs_fsm_dispatch(&pfcp_node->sm, e);
        break;

    case OGS_EVENT_SBI_SERVER:
        sbi_request = e->h.sbi.request;
        ogs_assert(sbi_request);

        stream_id = OGS_POINTER_TO_UINT(e->h.sbi.data);
        ogs_assert(stream_id >= OGS_MIN_POOL_ID &&
                stream_id <= OGS_MAX_POOL_ID);

        stream = ogs_sbi_stream_find_by_id(stream_id);
        if (!stream) {
            ogs_error("STREAM has already been removed [%d]", stream_id);
            break;
        }

        rv = ogs_sbi_parse_request(&sbi_message, sbi_request);
        if (rv != OGS_OK) {
            /* 'sbi_message' buffer is released in ogs_sbi_parse_request() */
            ogs_error("cannot parse HTTP sbi_message");
            ogs_assert(true ==
                ogs_sbi_server_send_error(
                    stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                    NULL, "cannot parse HTTP sbi_message", NULL,
                    NULL));
            break;
        }

        SWITCH(sbi_message.h.service.name)
        CASE(OGS_SBI_SERVICE_NAME_NUDM_SDM)
            api_version = OGS_SBI_API_V2;
            break;
        DEFAULT
            api_version = OGS_SBI_API_V1;
        END

        ogs_assert(api_version);
        if (strcmp(sbi_message.h.api.version, api_version) != 0) {
            ogs_error("Not supported version [%s]", sbi_message.h.api.version);
            ogs_assert(true ==
                ogs_sbi_server_send_error(
                    stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                    &sbi_message, "Not supported version", NULL, NULL));
            ogs_sbi_message_free(&sbi_message);
            break;
        }

        SWITCH(sbi_message.h.service.name)
        CASE(OGS_SBI_SERVICE_NAME_NNRF_NFM)

            SWITCH(sbi_message.h.resource.component[0])
            CASE(OGS_SBI_RESOURCE_NAME_NF_STATUS_NOTIFY)
                SWITCH(sbi_message.h.method)
                CASE(OGS_SBI_HTTP_METHOD_POST)
                    ogs_nnrf_nfm_handle_nf_status_notify(stream, &sbi_message);
                    break;

                DEFAULT
                    ogs_error("Invalid HTTP method [%s]", sbi_message.h.method);
                    ogs_assert(true ==
                        ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_FORBIDDEN, &sbi_message,
                            "Invalid HTTP method", sbi_message.h.method, NULL));
                END
                break;

            DEFAULT
                ogs_error("Invalid resource name [%s]",
                        sbi_message.h.resource.component[0]);
                ogs_assert(true ==
                    ogs_sbi_server_send_error(stream,
                        OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                        "Invalid resource name",
                        sbi_message.h.resource.component[0], NULL));
            END
            break;

        CASE(OGS_SBI_SERVICE_NAME_NSMF_PDUSESSION)
            SWITCH(sbi_message.h.resource.component[0])
            CASE(OGS_SBI_RESOURCE_NAME_SM_CONTEXTS)
                SWITCH(sbi_message.h.method)
                CASE(OGS_SBI_HTTP_METHOD_POST)
                    SWITCH(sbi_message.h.resource.component[2])
                    CASE(OGS_SBI_RESOURCE_NAME_MODIFY)
                    CASE(OGS_SBI_RESOURCE_NAME_RELEASE)
                        if (!sbi_message.h.resource.component[1]) {
                            ogs_error("No smContextRef [%s]",
                                    sbi_message.h.resource.component[1]);
                            smf_sbi_send_sm_context_update_error_log(
                                    stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                                    "No smContextRef",
                                    sbi_message.h.resource.component[1]);
                            break;
                        }

                        sess = smf_sess_find_by_sm_context_ref(
                                sbi_message.h.resource.component[1]);

                        if (!sess) {
                            ogs_warn("Not found [%s]", sbi_message.h.uri);
                            smf_sbi_send_sm_context_update_error_log(
                                    stream, OGS_SBI_HTTP_STATUS_NOT_FOUND,
                                    "Not found", sbi_message.h.uri);
                        }
                        break;

                    DEFAULT
                        sess = smf_sess_add_by_sbi_message(&sbi_message);
                        if (!sess) {
                            ogs_error("smf_sess_add_by_sbi_message() failed");
                            smf_sbi_send_sm_context_create_error(stream,
                                    OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                                    OGS_SBI_APP_ERRNO_NULL,
                                    "smf_sess_add_by_sbi_message() failed",
                                    NULL, NULL);
                            break;
                        }

                        smf_metrics_inst_by_slice_add(NULL, NULL,
                                SMF_METR_CTR_SM_PDUSESSIONCREATIONREQ, 1);
                    END
                    break;

                DEFAULT
                    ogs_error("Invalid HTTP method [%s]", sbi_message.h.method);
                    ogs_assert(true ==
                        ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                            "Invalid HTTP method", sbi_message.h.method,
                            NULL));
                    break;
                END

                if (sess) {
                    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
                    ogs_assert(smf_ue);
                    ogs_assert(OGS_FSM_STATE(&sess->sm));

                    e->sess_id = sess->id;
                    e->h.sbi.message = &sbi_message;
                    ogs_fsm_dispatch(&sess->sm, e);
                }
                break;

            DEFAULT
                ogs_error("Invalid resource name [%s]",
                        sbi_message.h.resource.component[0]);
                ogs_assert(true ==
                    ogs_sbi_server_send_error(stream,
                        OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                        "Invalid resource name",
                        sbi_message.h.resource.component[0], NULL));
            END
            break;

        CASE(OGS_SBI_SERVICE_NAME_NSMF_CALLBACK)
            SWITCH(sbi_message.h.resource.component[0])
            CASE(OGS_SBI_RESOURCE_NAME_N1_N2_FAILURE_NOTIFY)
                smf_namf_comm_handle_n1_n2_message_transfer_failure_notify(
                        stream, &sbi_message);
                break;
            CASE(OGS_SBI_RESOURCE_NAME_SM_POLICY_NOTIFY)
                if (!sbi_message.h.resource.component[1]) {
                    ogs_error("No smContextRef [%s]",
                            sbi_message.h.resource.component[1]);
                    ogs_assert(true ==
                        ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                            "No smContextRef",
                            sbi_message.h.resource.component[1], NULL));
                    break;
                }

                sess = smf_sess_find_by_sm_context_ref(
                        sbi_message.h.resource.component[1]);

                if (!sess) {
                    ogs_warn("Not found [%s]", sbi_message.h.uri);
                    ogs_assert(true ==
                        ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_NOT_FOUND, &sbi_message,
                            "Not found",
                            sbi_message.h.resource.component[1], NULL));
                    break;
                }

                SWITCH(sbi_message.h.resource.component[2])
                CASE(OGS_SBI_RESOURCE_NAME_UPDATE)
                    smf_npcf_smpolicycontrol_handle_update_notify(
                            sess, stream, &sbi_message);
                    break;
                CASE(OGS_SBI_RESOURCE_NAME_TERMINATE)
                    smf_npcf_smpolicycontrol_handle_terminate_notify(
                            sess, stream, &sbi_message);
                    break;
                DEFAULT
                    ogs_error("Invalid resource name [%s]",
                            sbi_message.h.resource.component[0]);
                    ogs_assert(true ==
                        ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                            "Invalid resource name",
                            sbi_message.h.resource.component[0], NULL));
                END
                break;
            CASE(OGS_SBI_RESOURCE_NAME_SDMSUBSCRIPTION_NOTIFY)
                smf_nsmf_callback_handle_sdm_data_change_notify(
                        stream, &sbi_message);
                break;
            DEFAULT
                ogs_error("Invalid resource name [%s]",
                        sbi_message.h.resource.component[0]);
                ogs_assert(true ==
                    ogs_sbi_server_send_error(stream,
                        OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                        "Invalid resource name",
                        sbi_message.h.resource.component[0], NULL));
            END
            break;

        DEFAULT
            ogs_error("Invalid API name [%s]", sbi_message.h.service.name);
            ogs_assert(true ==
                ogs_sbi_server_send_error(stream,
                    OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                    "Invalid API name", sbi_message.h.service.name, NULL));
        END

        /* In lib/sbi/server.c, notify_completed() releases 'request' buffer. */
        ogs_sbi_message_free(&sbi_message);
        break;

    case OGS_EVENT_SBI_CLIENT:
        ogs_assert(e);

        sbi_response = e->h.sbi.response;
        ogs_assert(sbi_response);
        rv = ogs_sbi_parse_response(&sbi_message, sbi_response);
        if (rv != OGS_OK) {
            ogs_error("cannot parse HTTP response");
            ogs_sbi_message_free(&sbi_message);
            ogs_sbi_response_free(sbi_response);
            break;
        }

        SWITCH(sbi_message.h.service.name)
        CASE(OGS_SBI_SERVICE_NAME_NUDM_SDM)
            api_version = OGS_SBI_API_V2;
            break;
        DEFAULT
            api_version = OGS_SBI_API_V1;
        END

        ogs_assert(api_version);
        if (strcmp(sbi_message.h.api.version, api_version) != 0) {
            ogs_error("Not supported version [%s]", sbi_message.h.api.version);
            ogs_sbi_message_free(&sbi_message);
            ogs_sbi_response_free(sbi_response);
            break;
        }

        SWITCH(sbi_message.h.service.name)
        CASE(OGS_SBI_SERVICE_NAME_NNRF_NFM)

            SWITCH(sbi_message.h.resource.component[0])
            CASE(OGS_SBI_RESOURCE_NAME_NF_INSTANCES)
                nf_instance = e->h.sbi.data;
                ogs_assert(nf_instance);
                ogs_assert(OGS_FSM_STATE(&nf_instance->sm));

                e->h.sbi.message = &sbi_message;
                ogs_fsm_dispatch(&nf_instance->sm, e);
                break;

            CASE(OGS_SBI_RESOURCE_NAME_SUBSCRIPTIONS)
                subscription_data = e->h.sbi.data;
                ogs_assert(subscription_data);

                SWITCH(sbi_message.h.method)
                CASE(OGS_SBI_HTTP_METHOD_POST)
                    if (sbi_message.res_status == OGS_SBI_HTTP_STATUS_CREATED ||
                        sbi_message.res_status == OGS_SBI_HTTP_STATUS_OK) {
                        ogs_nnrf_nfm_handle_nf_status_subscribe(
                                subscription_data, &sbi_message);
                    } else {
                        ogs_error("HTTP response error : %d",
                                sbi_message.res_status);
                    }
                    break;

                CASE(OGS_SBI_HTTP_METHOD_PATCH)
                    if (sbi_message.res_status == OGS_SBI_HTTP_STATUS_OK ||
                        sbi_message.res_status ==
                            OGS_SBI_HTTP_STATUS_NO_CONTENT) {
                        ogs_nnrf_nfm_handle_nf_status_update(
                                subscription_data, &sbi_message);
                    } else {
                        ogs_error("[%s] HTTP response error [%d]",
                                subscription_data->id ?
                                    subscription_data->id : "Unknown",
                                sbi_message.res_status);
                    }
                    break;

                CASE(OGS_SBI_HTTP_METHOD_DELETE)
                    if (sbi_message.res_status ==
                            OGS_SBI_HTTP_STATUS_NO_CONTENT) {
                        ogs_sbi_subscription_data_remove(subscription_data);
                    } else {
                        ogs_error("[%s] HTTP response error [%d]",
                                subscription_data->id ?
                                    subscription_data->id : "Unknown",
                                sbi_message.res_status);
                    }
                    break;

                DEFAULT
                    ogs_error("Invalid HTTP method [%s]", sbi_message.h.method);
                    ogs_assert_if_reached();
                END
                break;

            DEFAULT
                ogs_error("Invalid resource name [%s]",
                        sbi_message.h.resource.component[0]);
                ogs_assert_if_reached();
            END
            break;

        CASE(OGS_SBI_SERVICE_NAME_NNRF_DISC)
            SWITCH(sbi_message.h.resource.component[0])
            CASE(OGS_SBI_RESOURCE_NAME_NF_INSTANCES)
                sbi_xact_id = OGS_POINTER_TO_UINT(e->h.sbi.data);
                ogs_assert(sbi_xact_id >= OGS_MIN_POOL_ID &&
                        sbi_xact_id <= OGS_MAX_POOL_ID);

                sbi_xact = ogs_sbi_xact_find_by_id(sbi_xact_id);
                if (!sbi_xact) {
                    /* CLIENT_WAIT timer could remove SBI transaction
                     * before receiving SBI message */
                    ogs_error("SBI transaction has already been removed [%d]",
                            sbi_xact_id);
                    break;
                }

                SWITCH(sbi_message.h.method)
                CASE(OGS_SBI_HTTP_METHOD_GET)
                    if (sbi_message.res_status == OGS_SBI_HTTP_STATUS_OK)
                        smf_nnrf_handle_nf_discover(sbi_xact, &sbi_message);
                    else
                        ogs_error("HTTP response error [%d]",
                                sbi_message.res_status);
                    break;

                DEFAULT
                    ogs_error("Invalid HTTP method [%s]", sbi_message.h.method);
                    ogs_assert_if_reached();
                END
                break;

            DEFAULT
                ogs_error("Invalid resource name [%s]",
                        sbi_message.h.resource.component[0]);
                ogs_assert_if_reached();
            END
            break;

        CASE(OGS_SBI_SERVICE_NAME_NUDM_SDM)
        CASE(OGS_SBI_SERVICE_NAME_NPCF_SMPOLICYCONTROL)
        CASE(OGS_SBI_SERVICE_NAME_NAMF_COMM)
            sbi_xact_id = OGS_POINTER_TO_UINT(e->h.sbi.data);
            ogs_assert(sbi_xact_id >= OGS_MIN_POOL_ID &&
                    sbi_xact_id <= OGS_MAX_POOL_ID);

            sbi_xact = ogs_sbi_xact_find_by_id(sbi_xact_id);
            if (!sbi_xact) {
                /* CLIENT_WAIT timer could remove SBI transaction
                 * before receiving SBI message */
                ogs_error("SBI transaction has already been removed [%d]",
                        sbi_xact_id);
                break;
            }

            sbi_object_id = sbi_xact->sbi_object_id;
            ogs_assert(sbi_object_id >= OGS_MIN_POOL_ID &&
                    sbi_object_id <= OGS_MAX_POOL_ID);

            if (sbi_xact->assoc_stream_id >= OGS_MIN_POOL_ID &&
                sbi_xact->assoc_stream_id <= OGS_MAX_POOL_ID)
                e->h.sbi.data = OGS_UINT_TO_POINTER(sbi_xact->assoc_stream_id);

            e->h.sbi.state = sbi_xact->state;

            ogs_sbi_xact_remove(sbi_xact);

            sess = smf_sess_find_by_id(sbi_object_id);
            if (!sess) {
                ogs_error("Session has already been removed");
                break;
            }
            smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
            ogs_assert(smf_ue);
            ogs_assert(OGS_FSM_STATE(&sess->sm));

            e->sess_id = sess->id;
            e->h.sbi.message = &sbi_message;

            ogs_fsm_dispatch(&sess->sm, e);
            break;

        CASE(OGS_SBI_SERVICE_NAME_NUDM_UECM)
            int state = 0;
            bool unknown_res_status = false;

            sbi_xact_id = OGS_POINTER_TO_UINT(e->h.sbi.data);
            ogs_assert(sbi_xact_id >= OGS_MIN_POOL_ID &&
                    sbi_xact_id <= OGS_MAX_POOL_ID);

            sbi_xact = ogs_sbi_xact_find_by_id(sbi_xact_id);
            if (!sbi_xact) {
                /* CLIENT_WAIT timer could remove SBI transaction
                 * before receiving SBI message */
                ogs_error("SBI transaction has already been removed [%d]",
                        sbi_xact_id);
                break;
            }

            sbi_object_id = sbi_xact->sbi_object_id;
            ogs_assert(sbi_object_id >= OGS_MIN_POOL_ID &&
                    sbi_object_id <= OGS_MAX_POOL_ID);

            if (sbi_xact->assoc_stream_id >= OGS_MIN_POOL_ID &&
                sbi_xact->assoc_stream_id <= OGS_MAX_POOL_ID)
                stream = ogs_sbi_stream_find_by_id(sbi_xact->assoc_stream_id);

            state = sbi_xact->state;
            ogs_assert(state);

            ogs_sbi_xact_remove(sbi_xact);

            sess = smf_sess_find_by_id(sbi_object_id);
            if (!sess) {
                ogs_error("Session has already been removed");
                break;
            }
            smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
            ogs_assert(smf_ue);

            if (state == SMF_UECM_STATE_REGISTERED) {
                /* SMF Registration */
                if (sbi_message.res_status != OGS_SBI_HTTP_STATUS_OK &&
                    sbi_message.res_status != OGS_SBI_HTTP_STATUS_CREATED)
                    unknown_res_status = true;
            } else if (state == SMF_UECM_STATE_DEREGISTERED_BY_AMF ||
                        state == SMF_UECM_STATE_DEREGISTERED_BY_N1_N2_RELEASE) {
                /* SMF Deregistration */
                if (sbi_message.res_status != OGS_SBI_HTTP_STATUS_NO_CONTENT)
                    unknown_res_status = true;
            } else {
                ogs_fatal("Unknown state [%d]", state);
                ogs_assert_if_reached();
            }

            if (unknown_res_status == true) {
                char *strerror = ogs_msprintf(
                    "[%s:%d] HTTP response error [%d] state [%d]",
                    smf_ue->supi, sess->psi, sbi_message.res_status, state);
                ogs_assert(strerror);

                ogs_error("%s", strerror);
                if (stream)
                    ogs_assert(true ==
                        ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                            NULL, strerror, NULL, NULL));
                ogs_free(strerror);
                break;
            }

            if (state == SMF_UECM_STATE_REGISTERED) {
                /* SMF Registration */
                ogs_assert(stream);
                ogs_assert(true == ogs_sbi_send_http_status_no_content(stream));
            } else if (state == SMF_UECM_STATE_DEREGISTERED_BY_AMF) {
                /* SMF Deregistration */
                ogs_assert(stream);
                ogs_assert(true == ogs_sbi_send_http_status_no_content(stream));
                SMF_SESS_CLEAR(sess);
            } else if (state == SMF_UECM_STATE_DEREGISTERED_BY_N1_N2_RELEASE) {
                /* SMF Deregistration */
                ogs_assert(true == smf_sbi_send_sm_context_status_notify(sess));
                SMF_SESS_CLEAR(sess);
            }

            break;

        DEFAULT
            ogs_error("Invalid service name [%s]", sbi_message.h.service.name);
            ogs_assert_if_reached();
        END

        ogs_sbi_message_free(&sbi_message);
        ogs_sbi_response_free(sbi_response);
        break;

    case OGS_EVENT_SBI_TIMER:
        ogs_assert(e);

        switch(e->h.timer_id) {
        case OGS_TIMER_NF_INSTANCE_REGISTRATION_INTERVAL:
        case OGS_TIMER_NF_INSTANCE_HEARTBEAT_INTERVAL:
        case OGS_TIMER_NF_INSTANCE_NO_HEARTBEAT:
        case OGS_TIMER_NF_INSTANCE_VALIDITY:
            nf_instance = e->h.sbi.data;
            ogs_assert(nf_instance);
            ogs_assert(OGS_FSM_STATE(&nf_instance->sm));

            ogs_sbi_self()->nf_instance->load = smf_instance_get_load();

            ogs_fsm_dispatch(&nf_instance->sm, e);
            if (OGS_FSM_CHECK(&nf_instance->sm, ogs_sbi_nf_state_exception))
                ogs_error("[%s:%s] State machine exception [%d]",
                        OpenAPI_nf_type_ToString(nf_instance->nf_type),
                        nf_instance->id, e->h.timer_id);
            break;

        case OGS_TIMER_SUBSCRIPTION_VALIDITY:
            subscription_data = e->h.sbi.data;
            ogs_assert(subscription_data);

            ogs_assert(true ==
                ogs_nnrf_nfm_send_nf_status_subscribe(
                    ogs_sbi_self()->nf_instance->nf_type,
                    subscription_data->req_nf_instance_id,
                    subscription_data->subscr_cond.nf_type,
                    subscription_data->subscr_cond.service_name));

            ogs_error("[%s] Subscription validity expired",
                subscription_data->id);
            ogs_sbi_subscription_data_remove(subscription_data);
            break;

        case OGS_TIMER_SUBSCRIPTION_PATCH:
            subscription_data = e->h.sbi.data;
            ogs_assert(subscription_data);

            ogs_assert(true ==
                ogs_nnrf_nfm_send_nf_status_update(subscription_data));

            ogs_info("[%s] Need to update Subscription",
                    subscription_data->id);
            break;

        case OGS_TIMER_SBI_CLIENT_WAIT:
            /*
             * ogs_pollset_poll() receives the time of the expiration
             * of next timer as an argument. If this timeout is
             * in very near future (1 millisecond), and if there are
             * multiple events that need to be processed by ogs_pollset_poll(),
             * these could take more than 1 millisecond for processing,
             * resulting in the timer already passed the expiration.
             *
             * In case that another NF is under heavy load and responds
             * to an SBI request with some delay of a few seconds,
             * it can happen that ogs_pollset_poll() adds SBI responses
             * to the event list for further processing,
             * then ogs_timer_mgr_expire() is called which will add
             * an additional event for timer expiration. When all events are
             * processed one-by-one, the SBI xact would get deleted twice
             * in a row, resulting in a crash.
             *
             * 1. ogs_pollset_poll()
             *    message was received and put into an event list,
             * 2. ogs_timer_mgr_expire()
             *    add an additional event for timer expiration
             * 3. message event is processed. (free SBI xact)
             * 4. timer expiration event is processed. (double-free SBI xact)
             *
             * To avoid double-free SBI xact,
             * we need to check ogs_sbi_xact_find_by_id()
             */
            sbi_xact_id = OGS_POINTER_TO_UINT(e->h.sbi.data);
            ogs_assert(sbi_xact_id >= OGS_MIN_POOL_ID &&
                    sbi_xact_id <= OGS_MAX_POOL_ID);

            sbi_xact = ogs_sbi_xact_find_by_id(sbi_xact_id);
            if (!sbi_xact) {
                ogs_error("SBI transaction has already been removed [%d]",
                        sbi_xact_id);
                break;
            }

            if (sbi_xact->assoc_stream_id >= OGS_MIN_POOL_ID &&
                sbi_xact->assoc_stream_id <= OGS_MAX_POOL_ID)
                stream = ogs_sbi_stream_find_by_id(sbi_xact->assoc_stream_id);
            /* Here, we should not use ogs_assert(stream)
             * since 'namf-comm' service has no an associated stream. */

            ogs_sbi_xact_remove(sbi_xact);

            ogs_error("Cannot receive SBI message");
            if (stream) {
                ogs_assert(true ==
                    ogs_sbi_server_send_error(stream,
                        OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT, NULL,
                        "Cannot receive SBI message", NULL, NULL));
            }
            break;

        default:
            ogs_error("Unknown timer[%s:%d]",
                    ogs_timer_get_name(e->h.timer_id), e->h.timer_id);
        }
        break;

    case SMF_EVT_5GSM_MESSAGE:
        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);

        if (ogs_nas_5gsm_decode(&nas_message, pkbuf) != OGS_OK) {
            ogs_error("ogs_nas_5gsm_decode() failed");
            ogs_pkbuf_free(pkbuf);
            return;
        }

        sess = smf_sess_find_by_id(e->sess_id);
        if (!sess) {
            ogs_error("Session has already been removed");
            ogs_pkbuf_free(pkbuf);
            break;
        }

        e->nas.message = &nas_message;
        ogs_fsm_dispatch(&sess->sm, e);

        ogs_pkbuf_free(pkbuf);
        break;

    case SMF_EVT_NGAP_MESSAGE:
        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);
        ogs_assert(e->ngap.type);

        sess = smf_sess_find_by_id(e->sess_id);
        if (!sess) {
            ogs_error("Session has already been removed");
            ogs_pkbuf_free(pkbuf);
            break;
        }

        ogs_fsm_dispatch(&sess->sm, e);

        ogs_pkbuf_free(pkbuf);
        break;

    default:
        ogs_error("No handler for event %s", smf_event_get_name(e));
        break;
    }
}
