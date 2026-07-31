/* Gx Interface, 3GPP TS 29.212 section 4
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

#include "fd-path.h"
#include "metrics.h"

/* KPI helper: map CC-Request-Type and result_code to a Diameter
 * lifecycle event label. Used by gx-path.c / gy-path.c. */
static inline const char *smf_diameter_event_label(
        uint32_t cc_request_type, uint32_t result_code)
{
    if (result_code != ER_DIAMETER_SUCCESS)
        return "error";
    switch (cc_request_type) {
    case OGS_DIAM_GX_CC_REQUEST_TYPE_INITIAL_REQUEST:
        return "init";
    case OGS_DIAM_GX_CC_REQUEST_TYPE_UPDATE_REQUEST:
        return "update";
    case OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST:
        return "term";
    default:
        return "other";
    }
}

static struct session_handler *smf_gx_reg = NULL;
static struct disp_hdl *hdl_gx_fb = NULL;
static struct disp_hdl *hdl_gx_rar = NULL;

struct sess_state {
    os0_t       gx_sid;             /* Gx Session-Id */

    os0_t       peer_host;          /* Peer Host */

#define NUM_CC_REQUEST_SLOT 4
    ogs_pool_id_t sess_id;
    struct {
        uint32_t cc_req_no;
        ogs_pool_id_t id;
    } xact_data[NUM_CC_REQUEST_SLOT];

    uint32_t cc_request_type;
    uint32_t cc_request_number;

    struct timespec ts; /* Time of sending the message */
};

static OGS_POOL(sess_state_pool, struct sess_state);
static ogs_thread_mutex_t sess_state_mutex;

static int decode_pcc_rule_definition(
        ogs_pcc_rule_t *pcc_rule, struct avp *avpch1, int *perror);
static void smf_gx_cca_cb(void *data, struct msg **msg);

static __inline__ struct sess_state *new_state(os0_t sid)
{
    struct sess_state *new = NULL;

    ogs_thread_mutex_lock(&sess_state_mutex);
    ogs_pool_alloc(&sess_state_pool, &new);
    if (!new) {
        ogs_error("ogs_pool_alloc() failed");
        ogs_thread_mutex_unlock(&sess_state_mutex);
        return NULL;
    }
    memset(new, 0, sizeof(*new));

    new->gx_sid = (os0_t)ogs_strdup((char *)sid);
    if (!new->gx_sid) {
        ogs_error("ogs_strdup() failed");
        ogs_pool_free(&sess_state_pool, new);
        ogs_thread_mutex_unlock(&sess_state_mutex);
        return NULL;
    }

    ogs_thread_mutex_unlock(&sess_state_mutex);

    return new;
}

static void state_cleanup(struct sess_state *sess_data, os0_t sid, void *opaque)
{
    if (!sess_data) {
        ogs_error("No session state");
        return;
    }

    ogs_debug("[GX_DANGLING] state_cleanup: sess_data=%p gx_sid_ptr=%p "
              "gx_sid=%s peer_host_ptr=%p",
              sess_data, sess_data->gx_sid,
              sess_data->gx_sid ? (char *)sess_data->gx_sid : "(null)",
              sess_data->peer_host);

    if (sess_data->gx_sid) {
        /*
         * This copy belongs to sess_data alone: smf_sess_t keeps its own
         * ogs_strdup() of the Session-Id, so freeing this one cannot dangle
         * a live session.
         *
         * Callers must still check that no session is USING the Session-Id
         * before disposing of the state - see smf_sess_find_by_gx_sid() -
         * because another in-flight message for the same Session-Id would
         * otherwise be able to act on a recycled sess_state.
         */
        ogs_free(sess_data->gx_sid);
        sess_data->gx_sid = NULL;
    }

    if (sess_data->peer_host) {
        ogs_free(sess_data->peer_host);
        sess_data->peer_host = NULL;
    }

    ogs_thread_mutex_lock(&sess_state_mutex);
    ogs_pool_free(&sess_state_pool, sess_data);
    ogs_thread_mutex_unlock(&sess_state_mutex);
}

/*
 * Re-attach a state that fd_sess_state_retrieve() detached, tolerating the
 * case where a newer state already owns this handler's slot.
 *
 * fd_sess_state_store() returns EALREADY -- leaving *sess_data untouched --
 * when an entry for smf_gx_reg is already linked. That happens when the SMF
 * main thread ran smf_gx_send_ccr() while this callback held the state
 * detached: it saw an empty slot and allocated a replacement with
 * new_state(). Asserting ret == 0 there would abort the SMF, so treat
 * EALREADY as "we are holding a redundant state": a newer one owns the slot,
 * and smf_sess_t keeps its own copy of the Session-Id, so state_cleanup() is
 * the correct disposal.
 *
 * Passing a NULL *sess_data is a no-op: nothing was detached, and storing
 * NULL would link a NULL-payload entry that steals the slot from whoever
 * really owns the state (libfdproto does not special-case NULL).
 */
static void gx_state_reattach(struct session *session,
        struct sess_state **sess_data)
{
    int ret;

    ogs_assert(sess_data);

    if (!*sess_data)
        return;

    ret = fd_sess_state_store(smf_gx_reg, session, sess_data);
    if (ret != 0) {
        ogs_warn("fd_sess_state_store() failed (%d) - a newer Gx state owns "
                "the slot, releasing the orphaned state [%s]", ret,
                (*sess_data)->gx_sid ?
                    (char *)(*sess_data)->gx_sid : "(null)");
        state_cleanup(*sess_data, NULL, NULL);
        *sess_data = NULL;
        return;
    }

    ogs_assert(*sess_data == NULL);
}

/*
 * Dispose of a detached state when an answer cannot be processed.
 *
 * Never just re-attach: the Session-Id may have no user left, in which case
 * re-attaching would leave behind exactly the orphan this series exists to
 * reclaim. Ask who owns it and act accordingly - re-point the
 * back-reference and re-attach for a live owner, release the state when
 * there is none.
 *
 * Always leaves *sess_data NULL, so the caller can return without stranding
 * anything.
 */
static void gx_state_dispose(struct session *session,
        struct sess_state **sess_data)
{
    smf_sess_t *owner = NULL;

    ogs_assert(sess_data);

    if (!*sess_data)
        return;

    owner = smf_sess_find_by_gx_sid((char *)(*sess_data)->gx_sid);
    if (owner) {
        (*sess_data)->sess_id = owner->id;
        gx_state_reattach(session, sess_data);
        return;
    }

    ogs_warn("Gx Session-Id [%s] no longer used - releasing state",
            (*sess_data)->gx_sid ? (char *)(*sess_data)->gx_sid : "(null)");
    state_cleanup(*sess_data, NULL, NULL);
    *sess_data = NULL;
}

/*
 * Report a CCR that could not be sent, as if the PCRF had answered with a
 * failed CCA.
 *
 * Also used from smf_gx_cca_cb() when an answer arrives but cannot be
 * processed - see gx_report_dropped_answer() below, which is a thin wrapper
 * that recovers the request type and transaction from the state, since the
 * malformed-answer cases get there precisely because the message could not
 * be parsed.
 *
 * The GSM state machine only ever leaves smf_gsm_state_wait_epc_auth_initial
 * (and ..._release) through its test_can_proceed convergence point, which is
 * driven purely by sm_data: it waits for the *_in_flight flags to clear, picks
 * the error out of the *_err fields, skips the Gx/Gy teardown for a session
 * that never came up, and answers the peer with a mapped GTP cause. Silently
 * returning from smf_gx_send_ccr() would leave gx_ccr_*_in_flight set and hang
 * the FSM on a CCA that will never arrive, so synthesise the failure instead
 * and let the existing, already-tested handling run.
 *
 * The receiving branches assert on the GTP transaction (gsm-sm.c INITIAL /
 * UPDATE cases and smf_gx_handle_cca_initial_request()), and test_can_proceed
 * dereferences it for the cause mapping. Posting an event without a resolvable
 * xact would only swap one abort for another, so in that case just record the
 * failure in sm_data and let whatever else drives this session converge.
 *
 * gx_message is zeroed: cmd_code / cc_request_type / result_code are what the
 * handler reads, err stays NULL so it falls back to
 * ER_DIAMETER_AUTHENTICATION_REJECTED, and the untouched session_data is safe
 * to OGS_SESSION_DATA_FREE().
 */
static void gx_report_ccr_send_failure(smf_sess_t *sess,
        ogs_pool_id_t xact_id, uint32_t cc_request_type)
{
    ogs_diam_gx_message_t *gx_message = NULL;
    smf_event_t *e = NULL;
    int rv;

    ogs_assert(sess);

    if (xact_id < OGS_MIN_POOL_ID || xact_id > OGS_MAX_POOL_ID ||
            !ogs_gtp_xact_find_by_id(xact_id)) {
        ogs_error("Gx CCR(type:%d) not sent and no GTP transaction to report "
                "it on - recording the failure only", cc_request_type);
        if (cc_request_type ==
                OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST) {
            sess->sm_data.gx_ccr_term_in_flight = false;
            sess->sm_data.gx_cca_term_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        } else {
            sess->sm_data.gx_ccr_init_in_flight = false;
            sess->sm_data.gx_cca_init_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        }
        return;
    }

    gx_message = ogs_calloc(1, sizeof(ogs_diam_gx_message_t));
    if (!gx_message) {
        /* This function exists to make test_can_proceed() converge, so its
         * own failure must not leave the in-flight flag set. */
        ogs_error("ogs_calloc() failed");
        goto record_only;
    }
    gx_message->cmd_code = OGS_DIAM_GX_CMD_CODE_CREDIT_CONTROL;
    gx_message->cc_request_type = cc_request_type;
    gx_message->result_code = ER_DIAMETER_UNABLE_TO_COMPLY;

    e = smf_event_new(SMF_EVT_GX_MESSAGE);
    ogs_assert(e);
    e->sess_id = sess->id;
    e->gx_message = gx_message;
    e->gtp_xact_id = xact_id;

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed:%d", (int)rv);
        OGS_SESSION_DATA_FREE(&gx_message->session_data);
        ogs_free(gx_message);
        ogs_event_free(e);
        goto record_only;
    }

    ogs_pollset_notify(ogs_app()->pollset);
    return;

record_only:
    /* Could not deliver the synthesised answer - record the failure directly
     * so the FSM still converges. */
    if (cc_request_type == OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST) {
        sess->sm_data.gx_ccr_term_in_flight = false;
        sess->sm_data.gx_cca_term_err = ER_DIAMETER_UNABLE_TO_COMPLY;
    } else {
        sess->sm_data.gx_ccr_init_in_flight = false;
        sess->sm_data.gx_cca_init_err = ER_DIAMETER_UNABLE_TO_COMPLY;
    }
}

/*
 * An answer arrived but could not be processed. Dropping it silently is not
 * enough: the sender set an in-flight flag, the GSM state machine leaves
 * smf_gsm_state_wait_epc_auth_initial / _release only through
 * test_can_proceed(), and there is no Gx CCA timeout - so if this is the
 * answer being waited on, nothing else will ever clear that flag.
 *
 * The request type and transaction come from the state, not the message:
 * the malformed-answer cases reach here precisely because the message could
 * not be parsed.
 */
static void gx_report_dropped_answer(struct sess_state *sess_data,
        uint32_t cc_request_number)
{
    smf_sess_t *sess = NULL;
    uint32_t slot;

    ogs_assert(sess_data);

    /*
     * Only report when the answer is provably the one being waited on.
     *
     * sess_data->cc_request_number and ->cc_request_type are overwritten by
     * every send, so they describe the LATEST request. Reporting blindly
     * would fail a CCR that is still in flight: test_can_proceed() would
     * take its failure branch, consume the GTP transaction, and the real
     * answer arriving afterwards would hit ogs_assert(gtp_xact) on a stale
     * id - reintroducing exactly the abort class this series removes.
     *
     * So the caller has to have recovered a CC-Request-Number, and it has to
     * match the latest one. When it cannot be identified at all the answer
     * is dropped silently; the residual wedge is confined to an answer that
     * carries neither a CC-Request-Number nor an originating request.
     */
    if (cc_request_number != sess_data->cc_request_number)
        return;

    sess = smf_sess_find_by_id(sess_data->sess_id);
    if (!sess)
        return;     /* nobody is waiting on this any more */

    slot = sess_data->cc_request_number % NUM_CC_REQUEST_SLOT;
    gx_report_ccr_send_failure(sess, sess_data->xact_data[slot].id,
            sess_data->cc_request_type);
}


/*
 * The request never got an answer. freeDiameter has already removed it from
 * its sent-request table, so a late answer will not find a query and is
 * discarded - this is the last chance to unblock whoever is waiting.
 *
 * Runs on a freeDiameter thread, like the answer callbacks, and takes the
 * same care: `data` is only ever compared, never dereferenced, and the state
 * is recovered from the session rather than trusted from the cookie.
 *
 * Leaves *req in place; freeDiameter frees it after this returns.
 */
static void smf_gx_ccr_expire_cb(void *data, DiamId_t peer_id, size_t peer_len,
        struct msg **req)
{
    struct session *session = NULL;
    struct sess_state *sess_data = NULL;
    struct avp *avp = NULL;
    struct avp_hdr *hdr = NULL;
    uint32_t cc_request_number = 0;
    int ret;

    if (!req || !*req)
        return;

    ret = fd_msg_sess_get(fd_g_config->cnf_dict, *req, &session, NULL);
    if (ret != 0 || !session) {
        ogs_error("Gx CCR timed out but its Session could not be resolved");
        return;
    }

    ret = fd_sess_state_retrieve(smf_gx_reg, session, &sess_data);
    if (ret != 0 || !sess_data) {
        /* The answer path got there first and already converged. */
        return;
    }
    if ((void *)sess_data != data) {
        /* A newer request owns the slot - put it back untouched. */
        gx_state_reattach(session, &sess_data);
        return;
    }

    /*
     * Report against the request that actually timed out. If a newer CCR has
     * been sent since, the numbers differ and gx_report_dropped_answer()
     * declines - failing the newer, still-live request here would consume its
     * GTP transaction and strand its answer.
     */
    ret = fd_msg_search_avp(*req, ogs_diam_gx_cc_request_number, &avp);
    if (ret == 0 && avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        if (ret == 0 && hdr)
            cc_request_number = hdr->avp_value->i32;
    }

    ogs_error("Gx CCR(number:%d) timed out after %ds [%s]",
            cc_request_number,
            (int)ogs_time_to_sec(
                ogs_local_conf()->time.message.diameter.timeout_duration),
            sess_data->gx_sid ? (char *)sess_data->gx_sid : "(null)");

    gx_report_dropped_answer(sess_data, cc_request_number);
    gx_state_dispose(session, &sess_data);
}

/* 3GPP TS 29.212 5.6.2 Credit-Control-Request */
void smf_gx_send_ccr(smf_sess_t *sess, ogs_pool_id_t xact_id,
        uint32_t cc_request_type)
{
    int ret;
    smf_ue_t *smf_ue = NULL;

    struct msg *req = NULL;
    struct avp *avp;
    struct avp *avpch1, *avpch2;
    struct avp_hdr *ahdr;
    union avp_value val;
    struct sess_state *sess_data = NULL, *svg;
    struct session *session = NULL;
    int new;
    ogs_paa_t paa; /* For changing Framed-IPv6-Prefix Length to 64 */
    char buf[OGS_PLMNIDSTRLEN];
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    uint32_t charging_id;
    uint32_t req_slot;

    ogs_assert(sess);

    ogs_assert(sess->ipv4 || sess->ipv6);
    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);

    ogs_debug("[Credit-Control-Request]");

    /* Create the request */
    ret = fd_msg_new(ogs_diam_gx_cmd_ccr, MSGFL_ALLOC_ETEID, &req);
    ogs_assert(ret == 0);
    {
        struct msg_hdr *h;
        ret = fd_msg_hdr(req, &h);
        ogs_assert(ret == 0);
        h->msg_appl = OGS_DIAM_GX_APPLICATION_ID;
    }

    /* Find Diameter Gx Session */
    if (sess->gx_sid) {
        /* Retrieve session by Session-Id */
        size_t sidlen = strlen(sess->gx_sid);
        ret = fd_sess_fromsid_msg((os0_t)sess->gx_sid, sidlen, &session, &new);
        ogs_assert(ret == 0);
        ogs_assert(new == 0);

        ogs_debug("    Found Gx Session-Id: [%s]", sess->gx_sid);

        /* Add Session-Id to the message */
        ret = ogs_diam_message_session_id_set(req, (os0_t)sess->gx_sid, sidlen);
        ogs_assert(ret == 0);
        /* Save the session associated with the message */
        ret = fd_msg_sess_set(req, session);
    } else {
        /* Create a new session */
        #define OGS_DIAM_GX_APP_SID_OPT  "app_gx"
        ret = fd_msg_new_session(req, (os0_t)OGS_DIAM_GX_APP_SID_OPT,
                CONSTSTRLEN(OGS_DIAM_GX_APP_SID_OPT));
        ogs_assert(ret == 0);
        ret = fd_msg_sess_get(fd_g_config->cnf_dict, req, &session, NULL);
        ogs_assert(ret == 0);
        ogs_debug("    Create a New Session");
    }

    /* Retrieve session state in this session */
    ret = fd_sess_state_retrieve(smf_gx_reg, session, &sess_data);
    if (!sess_data) {
        os0_t sid;
        size_t sidlen;

        ret = fd_sess_getsid(session, &sid, &sidlen);
        ogs_assert(ret == 0);

        /* Allocate new session state memory */
        sess_data = new_state(sid);
        if (!sess_data) {
            /*
             * Returning silently would wedge the session: the caller has
             * already set gx_ccr_*_in_flight, smf_gx_send_ccr() is void, and
             * the GSM state machine has no Gx CCA timeout - so
             * test_can_proceed() would wait forever for an answer that was
             * never asked for, and neither a Create Session Response nor an
             * error would ever be sent. Report it as a failed CCA instead.
             */
            ogs_error("new_state() failed: Gx sess_state_pool exhausted "
                    "- abandoning CCR(type:%d)", cc_request_type);
            fd_msg_free(req);
            gx_report_ccr_send_failure(sess, xact_id, cc_request_type);
            return;
        }

        ogs_debug("    Allocate new session: [%s]", sess_data->gx_sid);

        /*
         * Do NOT publish sess->gx_sid yet.
         *
         * The state stays detached across the whole CCR assembly below, so
         * the closing fd_sess_state_store() can still fail with EALREADY if
         * a freeDiameter dispatch thread re-attached a state meanwhile.
         * Publishing now would leave smf_sess_t.gx_sid holding a
         * Session-Id whose state we then have to discard. It is published
         * only after the store succeeds, at the end of this function.
         */
    } else
        ogs_debug("    Retrieve session: [%s]", sess_data->gx_sid);

    /*
     * 8.2.  CC-Request-Number AVP
     *
     *  The CC-Request-Number AVP (AVP Code 415) is of type Unsigned32 and
     *  identifies this request within one session.  As Session-Id AVPs are
     * globally unique, the combination of Session-Id and CC-Request-Number
     * AVPs is also globally unique and can be used in matching credit-
     * control messages with confirmations.  An easy way to produce unique
     * numbers is to set the value to 0 for a credit-control request of type
     * INITIAL_REQUEST and EVENT_REQUEST and to set the value to 1 for the
     * first UPDATE_REQUEST, to 2 for the second, and so on until the value
     * for TERMINATION_REQUEST is one more than for the last UPDATE_REQUEST.
     */

    sess_data->cc_request_type = cc_request_type;
    if (cc_request_type == OGS_DIAM_GX_CC_REQUEST_TYPE_INITIAL_REQUEST ||
        cc_request_type == OGS_DIAM_GX_CC_REQUEST_TYPE_EVENT_REQUEST)
        sess_data->cc_request_number = 0;
    else
        sess_data->cc_request_number++;

    ogs_debug("    CC Request Type[%d] Number[%d]",
        sess_data->cc_request_type, sess_data->cc_request_number);

    /* Update session state */
    sess_data->sess_id = sess->id;
    req_slot = sess_data->cc_request_number % NUM_CC_REQUEST_SLOT;
    sess_data->xact_data[req_slot].id = xact_id;
    sess_data->xact_data[req_slot].cc_req_no = sess_data->cc_request_number;

    /* Set Origin-Host & Origin-Realm */
    ret = fd_msg_add_origin(req, 0);
    ogs_assert(ret == 0);

    /* Set the Destination-Realm AVP */
    ret = fd_msg_avp_new(ogs_diam_destination_realm, 0, &avp);
    ogs_assert(ret == 0);
    val.os.data = (unsigned char *)(fd_g_config->cnf_diamrlm);
    val.os.len = strlen(fd_g_config->cnf_diamrlm);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Auth-Application-Id AVP */
    ret = fd_msg_avp_new(ogs_diam_auth_application_id, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = OGS_DIAM_GX_APPLICATION_ID;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set CC-Request-Type, CC-Request-Number */
    ret = fd_msg_avp_new(ogs_diam_gx_cc_request_type, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = sess_data->cc_request_type;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    ret = fd_msg_avp_new(ogs_diam_gx_cc_request_number, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = sess_data->cc_request_number;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Destination-Host AVP */
    if (sess_data->peer_host) {
        ret = fd_msg_avp_new(ogs_diam_destination_host, 0, &avp);
        ogs_assert(ret == 0);
        val.os.data = sess_data->peer_host;
        val.os.len  = strlen((char *)sess_data->peer_host);
        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);
    }

    /* Set Subscription-Id */
    ret = fd_msg_avp_new(ogs_diam_subscription_id, 0, &avp);
    ogs_assert(ret == 0);

    ret = fd_msg_avp_new(ogs_diam_subscription_id_type, 0, &avpch1);
    ogs_assert(ret == 0);
    val.i32 = OGS_DIAM_SUBSCRIPTION_ID_TYPE_END_USER_IMSI;
    ret = fd_msg_avp_setvalue (avpch1, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
    ogs_assert(ret == 0);

    ret = fd_msg_avp_new(ogs_diam_subscription_id_data, 0, &avpch1);
    ogs_assert(ret == 0);
    val.os.data = (uint8_t *)smf_ue->imsi_bcd;
    val.os.len = strlen(smf_ue->imsi_bcd);
    ret = fd_msg_avp_setvalue (avpch1, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
    ogs_assert(ret == 0);

    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Subscription-Id (MSISDN) */
    if (smf_ue->msisdn_len > 0) {
        ret = fd_msg_avp_new(ogs_diam_subscription_id, 0, &avp);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_subscription_id_type, 0, &avpch1);
        ogs_assert(ret == 0);
        val.i32 = OGS_DIAM_SUBSCRIPTION_ID_TYPE_END_USER_E164;
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_subscription_id_data, 0, &avpch1);
        ogs_assert(ret == 0);
        val.os.data = (uint8_t *)smf_ue->msisdn_bcd;
        val.os.len = strlen(smf_ue->msisdn_bcd);
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);
    }
    
    if (cc_request_type != OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST) {
        /* Set Supported-Features */
        ret = fd_msg_avp_new(ogs_diam_gx_supported_features, 0, &avp);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_vendor_id, 0, &avpch1);
        ogs_assert(ret == 0);
        val.i32 = OGS_3GPP_VENDOR_ID;
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_gx_feature_list_id, 0, &avpch1);
        ogs_assert(ret == 0);
        val.i32 = 1;
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_gx_feature_list, 0, &avpch1);
        ogs_assert(ret == 0);
        val.u32 = 0x0000000b;
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set Network-Request-Support */
        ret = fd_msg_avp_new(ogs_diam_gx_network_request_support, 0, &avp);
        ogs_assert(ret == 0);
        val.i32 = 1;
        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set Framed-IP-Address */
        if (sess->ipv4) {
            ret = fd_msg_avp_new(ogs_diam_gx_framed_ip_address, 0, &avp);
            ogs_assert(ret == 0);
            val.os.data = (uint8_t*)&sess->ipv4->addr;
            val.os.len = OGS_IPV4_LEN;
            ret = fd_msg_avp_setvalue(avp, &val);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }

        /* Set Framed-IPv6-Prefix */
        if (sess->ipv6) {
            ret = fd_msg_avp_new(ogs_diam_gx_framed_ipv6_prefix, 0, &avp);
            ogs_assert(ret == 0);
            /* As per 3GPP TS 23.401 version 15.12.0, section 5.3.1.2.2
             * The PDN GW allocates a globally unique /64
             * IPv6 prefix via Router Advertisement to a given UE.
             *
             * After the UE has received the Router Advertisement message, it
             * constructs a full IPv6 address via IPv6 Stateless Address
             * autoconfiguration in accordance with RFC 4862 using the interface
             * identifier assigned by PDN GW.
             *
             * For stateless address autoconfiguration however, the UE can
             * choose any interface identifier to generate IPv6 addresses, other
             * than link-local, without involving the network.
             *
             * And, from section 5.3.1.1, Both EPS network elements and UE shall
             * support the following mechanisms:
             *
             * /64 IPv6 prefix allocation via IPv6 Stateless Address
             * autoconfiguration according to RFC 4862 [18], if IPv6 is
             * supported.
             */
            memset(&paa, 0 , sizeof(paa));
            memcpy(&paa.addr6, &sess->ipv6->addr,
                    OGS_IPV6_DEFAULT_PREFIX_LEN >> 3);
#define FRAMED_IPV6_PREFIX_LENGTH 64  /* from spec document */
            paa.len = FRAMED_IPV6_PREFIX_LENGTH;
            val.os.data = (uint8_t*)&paa;
            /* Reserved (1 byte) + Prefix length (1 byte) +
             * IPv6 Prefix (8 bytes)
             */
            val.os.len = (OGS_IPV6_DEFAULT_PREFIX_LEN >> 3) + 2;
            ret = fd_msg_avp_setvalue(avp, &val);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }

        /* Set IP-Can-Type */
        ret = fd_msg_avp_new(ogs_diam_gx_ip_can_type, 0, &avp);
        ogs_assert(ret == 0);

        switch (sess->gtp_rat_type) {
        case OGS_GTP2_RAT_TYPE_UTRAN:
        case OGS_GTP2_RAT_TYPE_GERAN:
        case OGS_GTP2_RAT_TYPE_HSPA_EVOLUTION:
        case OGS_GTP2_RAT_TYPE_EUTRAN:
            val.i32 = OGS_DIAM_GX_IP_CAN_TYPE_3GPP_EPS;
            break;
        case OGS_GTP2_RAT_TYPE_WLAN:
        case OGS_GTP2_RAT_TYPE_VIRTUAL:
            val.i32 = OGS_DIAM_GX_IP_CAN_TYPE_NON_3GPP_EPS;
            break;
        default:
            ogs_error("Unknown RAT Type [%d]", sess->gtp_rat_type);
            ogs_assert_if_reached();
        }

        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set RAT-Type */
        ret = fd_msg_avp_new(ogs_diam_rat_type, 0, &avp);
        ogs_assert(ret == 0);

        switch (sess->gtp_rat_type) {
        case OGS_GTP2_RAT_TYPE_UTRAN:
            val.i32 = OGS_DIAM_RAT_TYPE_UTRAN;
            break;
        case OGS_GTP2_RAT_TYPE_GERAN:
            val.i32 = OGS_DIAM_RAT_TYPE_GERAN;
            break;
        case OGS_GTP2_RAT_TYPE_HSPA_EVOLUTION:
            val.i32 = OGS_DIAM_RAT_TYPE_HSPA_EVOLUTION;
            break;
        case OGS_GTP2_RAT_TYPE_EUTRAN:
            val.i32 = OGS_DIAM_RAT_TYPE_EUTRAN;
            break;
        case OGS_GTP2_RAT_TYPE_WLAN:
            val.i32 = OGS_DIAM_RAT_TYPE_WLAN;
            break;
        default:
            ogs_error("Unknown RAT Type [%d]", sess->gtp_rat_type);
            ogs_assert_if_reached();
        }

        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set QoS-Information */
        if (sess->session.ambr.downlink || sess->session.ambr.uplink) {
            ret = fd_msg_avp_new(ogs_diam_gx_qos_information, 0, &avp);
            ogs_assert(ret == 0);

            if (sess->session.ambr.uplink) {
                ret = fd_msg_avp_new(ogs_diam_gx_apn_aggregate_max_bitrate_ul,
                        0, &avpch1);
                ogs_assert(ret == 0);
                val.u32 = sess->session.ambr.uplink;
                ret = fd_msg_avp_setvalue (avpch1, &val);
                ogs_assert(ret == 0);
                ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
                ogs_assert(ret == 0);
            }

            if (sess->session.ambr.downlink) {
                ret = fd_msg_avp_new(
                        ogs_diam_gx_apn_aggregate_max_bitrate_dl, 0, &avpch1);
                ogs_assert(ret == 0);
                val.u32 = sess->session.ambr.downlink;
                ret = fd_msg_avp_setvalue (avpch1, &val);
                ogs_assert(ret == 0);
                ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
                ogs_assert(ret == 0);
            }

            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }

        /* Set Default-EPS-Bearer-QoS */
        ret = fd_msg_avp_new(ogs_diam_gx_default_eps_bearer_qos, 0, &avp);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_gx_qos_class_identifier, 0, &avpch1);
        ogs_assert(ret == 0);
        val.u32 = sess->session.qos.index;
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(
                ogs_diam_gx_allocation_retention_priority, 0, &avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_gx_priority_level, 0, &avpch2);
        ogs_assert(ret == 0);
        val.u32 = sess->session.qos.arp.priority_level;
        ret = fd_msg_avp_setvalue (avpch2, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avpch1, MSG_BRW_LAST_CHILD, avpch2);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_gx_pre_emption_capability, 0, &avpch2);
        ogs_assert(ret == 0);
        val.u32 = sess->session.qos.arp.pre_emption_capability;
        ret = fd_msg_avp_setvalue (avpch2, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avpch1, MSG_BRW_LAST_CHILD, avpch2);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(ogs_diam_gx_pre_emption_vulnerability, 0, &avpch2);
        ogs_assert(ret == 0);
        val.u32 = sess->session.qos.arp.pre_emption_vulnerability;
        ret = fd_msg_avp_setvalue (avpch2, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avpch1, MSG_BRW_LAST_CHILD, avpch2);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* 3GPP-User-Location-Info, 3GPP TS 29.061 16.4.7.2 22 */
        smf_fd_msg_avp_add_3gpp_uli(sess, req);

        /* Set 3GPP-MS-Timezone */
        if (sess->gtp.ue_timezone.presence &&
                sess->gtp.ue_timezone.len && sess->gtp.ue_timezone.data) {
            ret = fd_msg_avp_new(ogs_diam_gx_3gpp_ms_timezone, 0, &avp);
            ogs_assert(ret == 0);
            val.os.data = sess->gtp.ue_timezone.data;
            val.os.len = sess->gtp.ue_timezone.len;
            ret = fd_msg_avp_setvalue(avp, &val);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }

        /* Set 3GPP-SGSN-MCC-MNC */
        ret = fd_msg_avp_new(ogs_diam_gx_3gpp_sgsn_mcc_mnc, 0, &avp);
        ogs_assert(ret == 0);
        val.os.data = (uint8_t *)ogs_plmn_id_to_string(
                &sess->serving_plmn_id, buf);
        val.os.len = strlen(buf);
        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set AN-GW-Address - Upto 2 address */
        if (sess->sgw_s5c_ip.ipv4) {
            ret = fd_msg_avp_new(ogs_diam_gx_an_gw_address, 0, &avp);
            ogs_assert(ret == 0);
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = sess->sgw_s5c_ip.addr;
            ret = fd_msg_avp_value_encode(&sin, avp);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }
        if (sess->sgw_s5c_ip.ipv6) {
            ret = fd_msg_avp_new(ogs_diam_gx_an_gw_address, 0, &avp);
            ogs_assert(ret == 0);
            sin6.sin6_family = AF_INET6;
            memcpy(sin6.sin6_addr.s6_addr,
                    sess->sgw_s5c_ip.addr6, OGS_IPV6_LEN);
            ret = fd_msg_avp_value_encode(&sin6, avp);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }
    }

    /* 3GPP-Charging-Characteristics, 3GPP TS 29.061 16.4.7.2 13 */
    if (sess->gtp.charging_characteristics.presence &&
        sess->gtp.charging_characteristics.len > 0) {
        uint8_t oct1, oct2;
        char digits[5];
        ret = fd_msg_avp_new(ogs_diam_gx_3gpp_charging_characteristics, 0, &avp);
        ogs_assert(ret == 0);
        oct1 = ((uint8_t*)sess->gtp.charging_characteristics.data)[0];
        oct2 = (sess->gtp.charging_characteristics.len > 1) ?
                        ((uint8_t*)sess->gtp.charging_characteristics.data)[1] : 0;
        ogs_snprintf(digits, sizeof(digits), "%02x%02x", oct1, oct2);
        val.os.data = (uint8_t*)&digits[0];
        val.os.len = 4;
        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);
    }

    /* Set Called-Station-Id */
    ret = fd_msg_avp_new(ogs_diam_gx_called_station_id, 0, &avp);
    ogs_assert(ret == 0);
    ogs_assert(sess->session.name);
    val.os.data = (uint8_t*)sess->session.name;
    val.os.len = strlen(sess->session.name);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    if (cc_request_type != OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST) {
        /* Set Online to DISABLE */
        ret = fd_msg_avp_new(ogs_diam_gx_online, 0, &avp);
        ogs_assert(ret == 0);
        val.u32 = OGS_DIAM_GX_DISABLE_ONLINE;
        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set Offline to ENABLE */
        ret = fd_msg_avp_new(ogs_diam_gx_offline, 0, &avp);
        ogs_assert(ret == 0);
        val.u32 = OGS_DIAM_GX_ENABLE_OFFLINE;
        ret = fd_msg_avp_setvalue(avp, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /* Set Access-Network-Charging-Address - Only 1 address */
        if (ogs_gtp_self()->gtpc_addr) {
            ret = fd_msg_avp_new(
                    ogs_diam_gx_access_network_charging_address, 0, &avp);
            ogs_assert(ret == 0);
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr =
                ogs_gtp_self()->gtpc_addr->sin.sin_addr.s_addr;
            ret = fd_msg_avp_value_encode(&sin, avp);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        } else if (ogs_gtp_self()->gtpc_addr6) {
            ret = fd_msg_avp_new(
                    ogs_diam_gx_access_network_charging_address, 0, &avp);
            ogs_assert(ret == 0);
            sin6.sin6_family = AF_INET6;
            memcpy(sin6.sin6_addr.s6_addr,
                    ogs_gtp_self()->gtpc_addr6->sin6.sin6_addr.s6_addr,
                    OGS_IPV6_LEN);
            ret = fd_msg_avp_value_encode(&sin6, avp);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }

        /* Set Access-Network-Charging-Identitifer-Gx */
        ret = fd_msg_avp_new(
                ogs_diam_gx_access_network_charging_identifier_gx, 0, &avp);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_new(
                ogs_diam_gx_access_network_charging_identifier_value, 0,
                &avpch1);
        ogs_assert(ret == 0);
        charging_id = htobe32(sess->charging.id);
        val.os.data = (uint8_t *)&charging_id;
        val.os.len = sizeof(charging_id);
        ret = fd_msg_avp_setvalue (avpch1, &val);
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add (avp, MSG_BRW_LAST_CHILD, avpch1);
        ogs_assert(ret == 0);

        ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
        ogs_assert(ret == 0);

        /*
         * TS 29.274 version 16.11.0, Table 7.2.1-1,
         * GTPv2 RAT Type: The ePDG may use the access technology type of the
         * untrusted non-3GPP access network if it is able to acquire
         * it; otherwise it shall indicate Virtual as the RAT Type.
         * The TWAN shall set the RAT Type value to "WLAN" on the
         * S2a interface.
         */
        if (sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_WLAN ||
            sess->gtp_rat_type == OGS_GTP2_RAT_TYPE_VIRTUAL) {
            ret = fd_msg_avp_new(ogs_diam_gx_an_trusted, 0, &avp);
            ogs_assert(ret == 0);
            /*
             * TS 29.212 version 16.4.0, Table 5.4.0.1,
             * AN-Trusted: This AVP shall have the 'M' bit cleared.
             */
            ret = fd_msg_avp_hdr(avp, &ahdr);
            ogs_assert(ret == 0);
            ahdr->avp_flags = ahdr->avp_flags & AVP_FLAG_VENDOR;

            /*
             * Currently, only Untrusted non-3GPP access via ePDG is supported.
             */
            val.u32 = OGS_DIAM_GX_AN_UNTRUSTED;
            ret = fd_msg_avp_setvalue (avp, &val);
            ogs_assert(ret == 0);
            ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
            ogs_assert(ret == 0);
        }
    }

    ret = clock_gettime(CLOCK_REALTIME, &sess_data->ts);

    /* Keep a pointer to the session data for debug purpose,
     * in real life we would not need it */
    svg = sess_data;

    /* Store this value in the session */
    ret = fd_sess_state_store(smf_gx_reg, session, &sess_data);
    if (ret != 0) {
        /*
         * EALREADY: a freeDiameter dispatch thread re-attached a state while
         * we were assembling this CCR. Aborting here would take the whole
         * SMF down for what is a per-session race, so abandon the CCR and
         * report it as a failed CCA (see gx_report_ccr_send_failure()).
         *
         * Disposing of the state is unconditionally safe here. Whether this
         * call allocated it or retrieved an existing one, a newer state now
         * owns the freeDiameter slot, so ours is redundant; smf_sess_t keeps
         * its own copy of the Session-Id, so freeing this one dangles
         * nothing; and the request is discarded unsent, so no callback holds
         * a pointer to it.
         */
        ogs_error("fd_sess_state_store() failed (%d) for Gx Session-Id [%s] "
                "- abandoning CCR(type:%d)", ret,
                svg->gx_sid ? (char *)svg->gx_sid : "(null)",
                cc_request_type);
        state_cleanup(svg, NULL, NULL);

        fd_msg_free(req);
        gx_report_ccr_send_failure(sess, xact_id, cc_request_type);
        return;
    }
    ogs_assert(sess_data == NULL);

    /*
     * Publish the Session-Id now that the state is attached.
     *
     * smf_sess_t owns its own copy: sess_data and smf_sess_t have
     * independent lifetimes (a handover moves the session's copy to the
     * target, and either side can be torn down first), and sharing one
     * allocation between them is what used to let state_cleanup() free a
     * string a live session was still pointing at.
     *
     * Only (re)allocate when the value actually changes, so the common case
     * of a retrieved or handover-inherited state is a no-op.
     */
    if (!sess->gx_sid || strcmp(sess->gx_sid, (char *)svg->gx_sid) != 0) {
        if (sess->gx_sid)
            ogs_free(sess->gx_sid);
        sess->gx_sid = ogs_strdup((char *)svg->gx_sid);
        ogs_assert(sess->gx_sid);
    }

    /* Send the request */
    {
        struct timespec ts;
        struct timeval tv;

        ogs_gettimeofday(&tv);
        ts.tv_sec = tv.tv_sec +
            ogs_time_to_sec(
                ogs_local_conf()->time.message.diameter.timeout_duration);
        ts.tv_nsec = tv.tv_usec * 1000;
        ret = fd_msg_send_timeout(&req, smf_gx_cca_cb, svg,
                smf_gx_ccr_expire_cb, &ts);
    }
    ogs_assert(ret == 0);

    /* Increment the counter */
    ogs_assert(pthread_mutex_lock(&ogs_diam_stats_self()->stats_lock) == 0);
    ogs_diam_stats_self()->stats.nb_sent++;
    ogs_assert(pthread_mutex_unlock(&ogs_diam_stats_self()->stats_lock) == 0);
}

/* 3GPP TS 29.212 5b.6.5 Credit-Control-Answer */
static void smf_gx_cca_cb(void *data, struct msg **msg)
{
    int rv;
    int ret;

    struct sess_state *sess_data = NULL;
    struct timespec ts;
    struct session *session;
    struct avp *avp, *avpch1, *avpch2;
    struct avp_hdr *hdr;
    unsigned long dur;
    int error = 0;
    int new;
    struct msg *req = NULL;
    smf_event_t *e = NULL;
    smf_sess_t *sess = NULL;
    ogs_diam_gx_message_t *gx_message = NULL;
    uint32_t req_slot, cc_request_number = 0;

    ogs_debug("[Credit-Control-Answer]");

    ret = clock_gettime(CLOCK_REALTIME, &ts);
    ogs_assert(ret == 0);

    /* Get originating request of received message, if any */
    ret = fd_msg_answ_getq(*msg, &req);
    ogs_assert(ret == 0);

    /* Search the session, retrieve its data */
    ret = fd_msg_sess_get(fd_g_config->cnf_dict, *msg, &session, &new);
    ogs_assert(ret == 0);
    ogs_assert(new == 0);

    ogs_debug("    Search the session");

    ret = fd_sess_state_retrieve(smf_gx_reg, session, &sess_data);
    ogs_assert(ret == 0);
    if (!sess_data) {
        /*
         * Upstream behaviour: the answer is left for freeDiameter to
         * dispatch or drop rather than consumed here.
         *
         * Known residual risk: now that the orphan paths release the state
         * instead of re-attaching it, a second answer in flight for the
         * same Session-Id lands here and its message is not freed. The
         * window needs two requests outstanding on one Session-Id, so it is
         * narrow, but it is the same class as the leaks just fixed.
         */
        /*
         * Not a leak as such - leaving *msg set lets freeDiameter fall
         * through to the fallback handler and drop it - but that route is
         * indirect, logs a misleading "Unexpected message received!", and
         * depends on the fallback staying registered. Consume it here like
         * every other exit of this callback.
         */
        ogs_error("No Session Data");
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }
    if ((void *)sess_data != data) {
        /*
         * The state this request was sent with is no longer the one attached
         * to the Session-Id. That is the race gx_state_reattach() handles
         * from the other side: while the request was in flight the slot sat
         * empty, the main thread allocated a replacement, and our original
         * state was released as redundant when it tried to store.
         *
         * The answer belongs to that old request, so it must not be
         * processed against the current state - that would attribute it to
         * the wrong transaction. Put the current state back and drop it.
         *
         * `data` is only compared, never dereferenced: it may already have
         * been returned to sess_state_pool.
         */
        ogs_warn("Gx answer for a stale sess_state - dropping [%s]",
                sess_data->gx_sid ? (char *)sess_data->gx_sid : "(null)");
        gx_state_dispose(session, &sess_data);
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    ogs_debug("    Retrieve its data: [%s]", sess_data->gx_sid);

    /* Value of CC-Request-Number */
    ret = fd_msg_search_avp(*msg, ogs_diam_gx_cc_request_number, &avp);
    ogs_assert(ret == 0);
    if (!avp && req) {
        /* Attempt searching for CC-Request-* in original request. Error
         * messages (like DIAMETER_UNABLE_TO_DELIVER) crafted internally may not
         * have them. */
        ret = fd_msg_search_avp(req, ogs_diam_gx_cc_request_number, &avp);
        ogs_assert(ret == 0);
    }
    if (!avp) {
        /*
         * Neither the answer nor an originating request carries a
         * CC-Request-Number, so there is nothing to match this answer to.
         *
         * Narrow: it needs an answer that omits the AVP AND no query
         * attached to it, since the SMF's own CCRs always carry one. But it
         * is driven entirely by what the peer sends, and the disposal is the
         * same one the two checks around it already use, so there is no
         * reason for the last unmatched-answer case to be the one that
         * aborts.
         */
        ogs_error("no_CC-Request-Number - dropping Gx answer [%s]",
                sess_data->gx_sid ? (char *)sess_data->gx_sid : "(null)");
        /*
         * No report: with no CC-Request-Number and no originating request
         * there is nothing to prove which CCR this answers, and failing the
         * wrong one is worse than dropping it.
         */
        gx_state_dispose(session, &sess_data);
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }
    ret = fd_msg_avp_hdr(avp, &hdr);
    ogs_assert(ret == 0);
    cc_request_number = hdr->avp_value->i32;
    req_slot = cc_request_number % NUM_CC_REQUEST_SLOT;

    ogs_debug("    CC-Request-Number[%d]", cc_request_number);

    sess = smf_sess_find_by_id(sess_data->sess_id);

    if (!sess) {
        /*
         * The session named by sess_data->sess_id is gone, but that does not
         * mean the Gx Session-Id is unused: a VoLTE<->VoWiFi handover moves
         * it to the target session and the back-reference is only refreshed
         * by the next CCR. Ask who owns the Session-Id now.
         *
         * Owner found -> the state belongs to a live session: re-point the
         * back-reference and re-attach it. Discarding it would strand a
         * session that is still using this Gx session, and freeing the state
         * while another CCR for the same Session-Id is in flight would let a
         * late callback act on a recycled sess_state.
         *
         * No owner -> nobody is using this Session-Id, so the state is a
         * genuine orphan and can be released. This is the path that reclaims
         * a Gx session terminated at the TS 29.274 7.2.1 collision.
         *
         * Note this branch returns instead of falling through to a shared
         * exit, so whichever action is taken must leave nothing detached.
         */
        smf_sess_t *owner =
                smf_sess_find_by_gx_sid((char *)sess_data->gx_sid);

        if (owner) {
            ogs_warn("Session [%d] gone but Gx Session-Id now owned by [%d] "
                    "- keeping state [%s]", sess_data->sess_id, owner->id,
                    sess_data->gx_sid ?
                        (char *)sess_data->gx_sid : "(null)");
            sess_data->sess_id = owner->id;
            gx_state_reattach(session, &sess_data);
            /* This callback owns the answer; the shared exit is not
             * reached from here. */
            fd_msg_free(*msg);
            *msg = NULL;
            return;
        }

        ogs_warn("Session not found for ID [%d] and Gx Session-Id [%s] "
                "unused - releasing Gx state", sess_data->sess_id,
                sess_data->gx_sid ? (char *)sess_data->gx_sid : "(null)");
        state_cleanup(sess_data, NULL, NULL);
        sess_data = NULL;
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    if (sess_data->xact_data[req_slot].cc_req_no != cc_request_number) {
        /*
         * Reachable independently of the stale-state check earlier: there
         * are only NUM_CC_REQUEST_SLOT slots and the send path overwrites
         * xact_data[cc_request_number % NUM_CC_REQUEST_SLOT], so a late
         * answer can arrive after its slot was reused. The transaction
         * mapping it needs is gone, so drop it rather than acting on another
         * request's xact.
         *
         * Checked here, after the owner/orphan resolution above, rather than
         * before it: for an answer whose session is gone AND whose slot was
         * reused, running this first would skip that resolution and leave an
         * unused Gx state attached - the very orphan this series reclaims.
         */
        ogs_warn("Gx stale answer: CC-Request-Number[%d] no longer owns "
                "slot[%d] (now %d) - dropping", cc_request_number, req_slot,
                sess_data->xact_data[req_slot].cc_req_no);
        gx_state_dispose(session, &sess_data);
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    gx_message = ogs_calloc(1, sizeof(ogs_diam_gx_message_t));
    ogs_assert(gx_message);

    /* Set Credit Control Command */
    gx_message->cmd_code = OGS_DIAM_GX_CMD_CODE_CREDIT_CONTROL;

    /* Value of Result Code */
    ret = fd_msg_search_avp(*msg, ogs_diam_result_code, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        gx_message->result_code = hdr->avp_value->i32;
        gx_message->err = &gx_message->result_code;
        ogs_debug("    Result Code: %d", hdr->avp_value->i32);
    } else {
        ret = fd_msg_search_avp(*msg, ogs_diam_experimental_result, &avp);
        ogs_assert(ret == 0);
        if (avp) {
            ret = fd_avp_search_avp(
                    avp, ogs_diam_experimental_result_code, &avpch1);
            ogs_assert(ret == 0);
            if (avpch1) {
                ret = fd_msg_avp_hdr(avpch1, &hdr);
                ogs_assert(ret == 0);
                gx_message->result_code = hdr->avp_value->i32;
                gx_message->exp_err = &gx_message->result_code;
                ogs_debug("    Experimental Result Code: %d",
                        gx_message->result_code);
            }
        } else {
            ogs_error("no Result-Code");
            error++;
        }
    }

    /* Value of Origin-Host */
    ret = fd_msg_search_avp(*msg, ogs_diam_origin_host, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        ogs_debug("    From '%.*s'",
                (int)hdr->avp_value->os.len, hdr->avp_value->os.data);
    } else {
        ogs_error("no_Origin-Host");
        error++;
    }

    /* Value of Origin-Realm */
    ret = fd_msg_search_avp(*msg, ogs_diam_origin_realm, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        ogs_debug("         ('%.*s')",
                (int)hdr->avp_value->os.len, hdr->avp_value->os.data);
    } else {
        ogs_error("no_Origin-Realm");
        error++;
    }

    /* Value of CC-Request-Type */
    ret = fd_msg_search_avp(*msg, ogs_diam_gx_cc_request_type, &avp);
    ogs_assert(ret == 0);
    if (!avp && req) {
        /* Attempt searching for CC-Request-* in original request. Error
         * messages (like DIAMETER_UNABLE_TO_DELIVER) crafted internally may not
         * have them. */
        ret = fd_msg_search_avp(req, ogs_diam_gx_cc_request_type, &avp);
        ogs_assert(ret == 0);
    }
    if (!avp) {
        /*
         * Same class as the missing CC-Request-Number above, but this one
         * only counted the error and then called fd_msg_avp_hdr(NULL, ...),
         * which fails CHECK_AVP and returns EINVAL - so the ogs_assert()
         * below aborted anyway, two lines later. Drop the answer instead.
         */
        ogs_error("no_CC-Request-Type - dropping Gx answer [%s]",
                sess_data->gx_sid ? (char *)sess_data->gx_sid : "(null)");
        OGS_SESSION_DATA_FREE(&gx_message->session_data);
        ogs_free(gx_message);
        gx_report_dropped_answer(sess_data, cc_request_number);
        gx_state_dispose(session, &sess_data);
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }
    ret = fd_msg_avp_hdr(avp, &hdr);
    ogs_assert(ret == 0);
    gx_message->cc_request_type = hdr->avp_value->i32;

    /* KPI: Gx CCA lifecycle event. Emitted once per CCA reception. */
    smf_metrics_inst_by_app_event_inc("gx",
            smf_diameter_event_label(gx_message->cc_request_type,
                    gx_message->result_code),
            SMF_METR_BY_APP_EVENT_CTR_DIAMETER_LIFECYCLE);

    if (gx_message->result_code != ER_DIAMETER_SUCCESS) {
        ogs_warn("ERROR DIAMETER Result Code(%d)", gx_message->result_code);
        goto out;
    }

    ret = fd_msg_search_avp(*msg, ogs_diam_gx_qos_information, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_avp_search_avp(
                avp, ogs_diam_gx_apn_aggregate_max_bitrate_ul, &avpch1);
        ogs_assert(ret == 0);
        if (avpch1) {
            ret = fd_msg_avp_hdr(avpch1, &hdr);
            ogs_assert(ret == 0);
            gx_message->session_data.session.ambr.uplink = hdr->avp_value->u32;
        }
        ret = fd_avp_search_avp(
                avp, ogs_diam_gx_apn_aggregate_max_bitrate_dl, &avpch1);
        ogs_assert(ret == 0);
        if (avpch1) {
            ret = fd_msg_avp_hdr(avpch1, &hdr);
            ogs_assert(ret == 0);
            gx_message->session_data.session.ambr.downlink =
                hdr->avp_value->u32;
        }
    }

    ret = fd_msg_search_avp(*msg, ogs_diam_gx_default_eps_bearer_qos, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_avp_search_avp(avp, ogs_diam_gx_qos_class_identifier, &avpch1);
        ogs_assert(ret == 0);
        if (avpch1) {
            ret = fd_msg_avp_hdr(avpch1, &hdr);
            ogs_assert(ret == 0);
            gx_message->session_data.session.qos.index = hdr->avp_value->u32;
        }

        ret = fd_avp_search_avp(
                avp, ogs_diam_gx_allocation_retention_priority, &avpch1);
        ogs_assert(ret == 0);
        if (avpch1) {
            ret = fd_avp_search_avp(
                    avpch1, ogs_diam_gx_priority_level, &avpch2);
            ogs_assert(ret == 0);
            if (avpch2) {
                ret = fd_msg_avp_hdr(avpch2, &hdr);
                ogs_assert(ret == 0);
                gx_message->session_data.session.qos.arp.priority_level =
                    hdr->avp_value->u32;
            }

            /*
             * Ch 7.3.40 Allocation-Retenion-Proirty in TS 29.272 V15.9.0
             *
             * If the Pre-emption-Capability AVP is not present in the
             * Allocation-Retention-Priority AVP, the default value shall be
             * PRE-EMPTION_CAPABILITY_DISABLED (1).
             *
             * If the Pre-emption-Vulnerability AVP is not present in the
             * Allocation-Retention-Priority AVP, the default value shall be
             * PRE-EMPTION_VULNERABILITY_ENABLED (0).
             *
             * However, to easily set up VoLTE service,
             * enable Pre-emption Capability/Vulnerablility
             * in Default Bearer
             */
            ret = fd_avp_search_avp(
                    avpch1, ogs_diam_gx_pre_emption_capability, &avpch2);
            ogs_assert(ret == 0);
            if (avpch2) {
                ret = fd_msg_avp_hdr(avpch2, &hdr);
                ogs_assert(ret == 0);
                gx_message->session_data.
                    session.qos.arp.pre_emption_capability =
                        hdr->avp_value->u32;
            } else {
                gx_message->session_data.
                    session.qos.arp.pre_emption_capability =
                        OGS_EPC_PRE_EMPTION_DISABLED;
            }

            ret = fd_avp_search_avp(avpch1,
                        ogs_diam_gx_pre_emption_vulnerability, &avpch2);
            ogs_assert(ret == 0);
            if (avpch2) {
                ret = fd_msg_avp_hdr(avpch2, &hdr);
                ogs_assert(ret == 0);
                gx_message->session_data.
                    session.qos.arp.pre_emption_vulnerability =
                        hdr->avp_value->u32;
            } else {
                gx_message->session_data.
                    session.qos.arp.pre_emption_vulnerability =
                        OGS_EPC_PRE_EMPTION_ENABLED;
            }
        }
    }

    ret = fd_msg_browse(*msg, MSG_BRW_FIRST_CHILD, &avp, NULL);
    ogs_assert(ret == 0);
    while (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        switch (hdr->avp_code) {
        case AC_SESSION_ID:
        case AC_ORIGIN_HOST:
            if (sess_data->peer_host)
                ogs_free(sess_data->peer_host);
            sess_data->peer_host =
                (os0_t)ogs_strdup((char *)hdr->avp_value->os.data);
            ogs_assert(sess_data->peer_host);
            break;
        case AC_ORIGIN_REALM:
        case AC_DESTINATION_REALM:
        case AC_RESULT_CODE:
        case AC_ROUTE_RECORD:
        case AC_PROXY_INFO:
        case AC_AUTH_APPLICATION_ID:
            break;
        case OGS_DIAM_GX_AVP_CODE_CC_REQUEST_TYPE:
        case OGS_DIAM_GX_AVP_CODE_CC_REQUEST_NUMBER:
        case OGS_DIAM_GX_AVP_CODE_SUPPORTED_FEATURES:
            break;
        case OGS_DIAM_GX_AVP_CODE_QOS_INFORMATION:
        case OGS_DIAM_GX_AVP_CODE_DEFAULT_EPS_BEARER_QOS:
            break;
        case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_INSTALL:
            ret = fd_msg_browse(avp, MSG_BRW_FIRST_CHILD, &avpch1, NULL);
            ogs_assert(ret == 0);
            while (avpch1) {
                ret = fd_msg_avp_hdr(avpch1, &hdr);
                ogs_assert(ret == 0);
                switch (hdr->avp_code) {
                case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_DEFINITION:
                    if (gx_message->session_data.num_of_pcc_rule <
                            OGS_MAX_NUM_OF_PCC_RULE) {
                        ogs_pcc_rule_t *pcc_rule = NULL;
                        smf_bearer_t *bearer = NULL;
                        int num_of_flow = 0;

                        pcc_rule = &gx_message->session_data.pcc_rule
                                [gx_message->session_data.num_of_pcc_rule];

                        rv = decode_pcc_rule_definition(
                                pcc_rule, avpch1, &error);
                        ogs_assert(rv == OGS_OK);

                        num_of_flow = pcc_rule->num_of_flow;

                        bearer = smf_bearer_find_by_pcc_rule_name(
                                sess, pcc_rule->name);
                        if (bearer)
                            num_of_flow += ogs_list_count(&bearer->pf_list);

                        if (num_of_flow < OGS_MAX_NUM_OF_FLOW_IN_BEARER) {
                            pcc_rule->type = OGS_PCC_RULE_TYPE_INSTALL;
                            gx_message->session_data.num_of_pcc_rule++;
                        } else {
                            ogs_error("Overflow : Num Of Flow %d", num_of_flow);
                            OGS_PCC_RULE_FREE(pcc_rule);
                            error++;
                        }
                    } else {
                        ogs_error("Overflow: Number of PCCRule");
                        error++;
                    }
                    break;
                default:
                    ogs_error("Not supported(%d)", hdr->avp_code);
                    break;
                }
                fd_msg_browse(avpch1, MSG_BRW_NEXT, &avpch1, NULL);
            }
            break;
        default:
            ogs_warn("Not supported(%d)", hdr->avp_code);
            break;
        }
        fd_msg_browse(avp, MSG_BRW_NEXT, &avp, NULL);
    }

out:
    if (!error) {
        e = smf_event_new(SMF_EVT_GX_MESSAGE);
        ogs_assert(e);

        e->sess_id = sess->id;
        e->gx_message = gx_message;
        e->gtp_xact_id = sess_data->xact_data[req_slot].id;
        rv = ogs_queue_push(ogs_app()->queue, e);
        if (rv != OGS_OK) {
            ogs_error("ogs_queue_push() failed:%d", (int)rv);
            OGS_SESSION_DATA_FREE(&gx_message->session_data);
            ogs_free(gx_message);
            ogs_event_free(e);
        } else {
            ogs_pollset_notify(ogs_app()->pollset);
        }
    } else {
        OGS_SESSION_DATA_FREE(&gx_message->session_data);
        ogs_free(gx_message);
    }

    /* Free the message */
    ogs_assert(pthread_mutex_lock(&ogs_diam_stats_self()->stats_lock) == 0);
    dur = ((ts.tv_sec - sess_data->ts.tv_sec) * 1000000) +
        ((ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);
    if (ogs_diam_stats_self()->stats.nb_recv) {
        /* Ponderate in the avg */
        ogs_diam_stats_self()->stats.avg = (ogs_diam_stats_self()->stats.avg *
            ogs_diam_stats_self()->stats.nb_recv + dur) /
            (ogs_diam_stats_self()->stats.nb_recv + 1);
        /* Min, max */
        if (dur < ogs_diam_stats_self()->stats.shortest)
            ogs_diam_stats_self()->stats.shortest = dur;
        if (dur > ogs_diam_stats_self()->stats.longest)
            ogs_diam_stats_self()->stats.longest = dur;
    } else {
        ogs_diam_stats_self()->stats.shortest = dur;
        ogs_diam_stats_self()->stats.longest = dur;
        ogs_diam_stats_self()->stats.avg = dur;
    }
    if (error)
        ogs_diam_stats_self()->stats.nb_errs++;
    else
        ogs_diam_stats_self()->stats.nb_recv++;

    ogs_assert(pthread_mutex_unlock(&ogs_diam_stats_self()->stats_lock) == 0);

    /* Display how long it took */
    if (ts.tv_nsec > sess_data->ts.tv_nsec)
        ogs_trace("in %d.%06ld sec",
                (int)(ts.tv_sec - sess_data->ts.tv_sec),
                (long)(ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);
    else
        ogs_trace("in %d.%06ld sec",
                (int)(ts.tv_sec + 1 - sess_data->ts.tv_sec),
                (long)(1000000000 + ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);

    ogs_debug("    CC-Request-Type[%d] Number[%d] in Session Data",
        sess_data->cc_request_type, sess_data->cc_request_number);
    ogs_debug("    Current CC-Request-Number[%d]", cc_request_number);
    
    if (sess_data->cc_request_type ==
            OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST &&
        sess_data->cc_request_number <= cc_request_number) {
        smf_sess_t *owner =
                smf_sess_find_by_gx_sid((char *)sess_data->gx_sid);

        if (!owner) {
            /* Nobody is using this Session-Id any more: this really is the
             * last message for it, so drop the state. */
            ogs_debug("    [LAST] state_cleanup(): [%s]", sess_data->gx_sid);
            state_cleanup(sess_data, NULL, NULL);
            sess_data = NULL;
        } else if (owner == sess) {
            /*
             * Reclaim the Diameter state here, but leave sess->gx_sid alone.
             *
             * This runs on a freeDiameter dispatch thread; smf_sess_remove()
             * frees the same copy on the main thread and neither side is
             * synchronised, so having both free it is a genuine double-free
             * race (clearing before freeing only narrows one interleaving).
             * One side has to own it, and that has to be the main thread.
             *
             * The CCA-T is delivered to the main loop as an SMF_EVT_GX_MESSAGE
             * anyway, so its handler in gsm-sm.c releases the session's copy
             * there - which also keeps the session from reusing a Session-Id
             * whose Diameter session has just been torn down.
             */
            ogs_debug("    [LAST] state_cleanup(): [%s]", sess_data->gx_sid);
            state_cleanup(sess_data, NULL, NULL);
            sess_data = NULL;
        } else {
            /*
             * Somebody else owns this Session-Id now - a VoLTE<->VoWiFi
             * handover moved it to the target while this CCR-T was in
             * flight. Terminating the Diameter state would pull it out from
             * under a live session, so keep it and re-point the
             * back-reference at the real owner instead.
             *
             * Abnormal path: warn, so an unexpected hit on the normal
             * teardown route shows up immediately rather than as
             * sess_state_pool pressure days later.
             */
            ogs_warn("Gx CCA-T: Session-Id now owned by another session "
                    "- keeping state [sess_id:%d -> %d gx_sid:%s]",
                    sess_data->sess_id, owner->id,
                    sess_data->gx_sid ?
                        (char *)sess_data->gx_sid : "(null)");
            sess_data->sess_id = owner->id;
            gx_state_reattach(session, &sess_data);
        }
    } else {
        ogs_debug("    fd_sess_state_store(): [%s]", sess_data->gx_sid);
        gx_state_reattach(session, &sess_data);
    }

    ret = fd_msg_free(*msg);
    ogs_assert(ret == 0);
    *msg = NULL;

    return;
}

static int smf_gx_fb_cb(struct msg **msg, struct avp *avp,
        struct session *sess, void *opaque, enum disp_action *act)
{
    /* This CB should never be called */
    ogs_warn("Unexpected message received!");

    return ENOTSUP;
}

static int smf_gx_rar_cb( struct msg **msg, struct avp *avp,
        struct session *session, void *opaque, enum disp_action *act)
{
    int rv;
    int ret;

    struct msg *ans, *qry;
    struct avp *avpch1;
    struct avp_hdr *hdr;
    union avp_value val;
    struct sess_state *sess_data = NULL;

    smf_event_t *e = NULL;
    smf_sess_t *sess = NULL;
    ogs_diam_gx_message_t *gx_message = NULL;

    uint32_t result_code = OGS_DIAM_UNKNOWN_SESSION_ID;
    int error = 0;

    ogs_assert(msg);

    ogs_debug("Re-Auth-Request");

    gx_message = ogs_calloc(1, sizeof(ogs_diam_gx_message_t));
    ogs_assert(gx_message);

    /* Set Credit Control Command */
    gx_message->cmd_code = OGS_DIAM_GX_CMD_RE_AUTH;

    /* Create answer header */
    qry = *msg;
    ret = fd_msg_new_answer_from_req(fd_g_config->cnf_dict, msg, 0);
    ogs_assert(ret == 0);
    ans = *msg;

    ret = fd_sess_state_retrieve(smf_gx_reg, session, &sess_data);
    ogs_assert(ret == 0);
    if (!sess_data) {
        ogs_debug("[GX_DANGLING] !sess_data path: session=%p "
                  "(already released)", session);
        ogs_warn("No Session Data - session already released, sending UNKNOWN_SESSION_ID");
        result_code = OGS_DIAM_UNKNOWN_SESSION_ID;
        goto out;
    }

    /* Get Session Information */
    sess = smf_sess_find_by_id(sess_data->sess_id);

    /*
     * VoLTE<->VoWiFi handover: smf_s5c_handle_create_session_request() hands
     * the Gx Session-Id over to the target session and clears it on the
     * source, but sess_data->sess_id is only refreshed when the next CCR-U
     * is sent. For a WLAN target that CCR-U waits on the S6b AAR/AAA round
     * trip, so the stale window is milliseconds, not microseconds. Follow
     * the peer link (set up in the same handler) to reach the session that
     * actually owns this Session-Id now.
     *
     * The Session-Id strings are compared by value: each side owns its own
     * copy, so the peer merely having *a* gx_sid is not enough - it has to
     * be this one.
     */
    if (sess && !sess->gx_sid &&
            sess->epc_handover.peer_sess_id != OGS_INVALID_POOL_ID) {
        smf_sess_t *peer =
                smf_sess_find_by_id(sess->epc_handover.peer_sess_id);
        if (peer && peer->gx_sid && sess_data->gx_sid &&
                strcmp(peer->gx_sid, (char *)sess_data->gx_sid) == 0) {
            ogs_info("Gx RAR: following HO peer link [%d] -> [%d]",
                    sess->id, peer->id);
            sess = peer;
            /* Make the correction sticky so later RARs need no lookup. */
            sess_data->sess_id = sess->id;
        }
    }

    if (!sess) {
        /*
         * The session named by sess_data->sess_id is gone. Before giving up,
         * ask whether anybody else owns this Gx Session-Id - the peer chase
         * above only covers the case where the named session still exists.
         */
        smf_sess_t *owner =
                smf_sess_find_by_gx_sid((char *)sess_data->gx_sid);

        if (owner) {
            ogs_info("Gx RAR: session [%d] gone, Session-Id now owned by "
                    "[%d]", sess_data->sess_id, owner->id);
            sess_data->sess_id = owner->id;
            sess = owner;
        } else {
            ogs_warn("No Session ID [%d] and Gx Session-Id [%s] unused - "
                    "releasing Gx state, answering UNKNOWN_SESSION_ID",
                    sess_data->sess_id,
                    sess_data->gx_sid ?
                        (char *)sess_data->gx_sid : "(null)");
            /*
             * Nobody is using this Session-Id, so the state is a genuine
             * orphan: release it rather than leaving it attached until the
             * freeDiameter expiry thread runs. sess_data is NULLed so the
             * shared "out:" path stores nothing.
             */
            state_cleanup(sess_data, NULL, NULL);
            sess_data = NULL;
            result_code = OGS_DIAM_UNKNOWN_SESSION_ID;
            goto out;
        }
    }

    ret = fd_msg_browse(qry, MSG_BRW_FIRST_CHILD, &avp, NULL);
    ogs_assert(ret == 0);
    while (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        switch(hdr->avp_code) {
        case AC_SESSION_ID:
        case AC_ORIGIN_HOST:
        case AC_ORIGIN_REALM:
        case AC_DESTINATION_REALM:
        case AC_DESTINATION_HOST:
        case AC_ROUTE_RECORD:
        case AC_PROXY_INFO:
        case AC_AUTH_APPLICATION_ID:
            break;
        case OGS_DIAM_GX_AVP_CODE_RE_AUTH_REQUEST_TYPE:
            break;
        case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_INSTALL:
            ret = fd_msg_browse(avp, MSG_BRW_FIRST_CHILD, &avpch1, NULL);
            ogs_assert(ret == 0);
            while(avpch1) {
                ret = fd_msg_avp_hdr(avpch1, &hdr);
                ogs_assert(ret == 0);
                switch(hdr->avp_code) {
                case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_DEFINITION:
                    if (gx_message->session_data.num_of_pcc_rule <
                            OGS_MAX_NUM_OF_PCC_RULE) {
                        ogs_pcc_rule_t *pcc_rule = NULL;
                        smf_bearer_t *bearer = NULL;
                        int num_of_flow = 0;

                        pcc_rule = &gx_message->session_data.pcc_rule
                                [gx_message->session_data.num_of_pcc_rule];

                        rv = decode_pcc_rule_definition(
                                pcc_rule, avpch1, &error);
                        ogs_assert(rv == OGS_OK);

                        if (error) {
                            ogs_error("decode_pcc_rule_definition() failed");
                            OGS_PCC_RULE_FREE(pcc_rule);
                            result_code = OGS_DIAM_GX_DIAMETER_PCC_RULE_EVENT;
                            goto out;
                        }

                        num_of_flow = pcc_rule->num_of_flow;

                        bearer = smf_bearer_find_by_pcc_rule_name(
                                sess, pcc_rule->name);
                        if (bearer)
                            num_of_flow += ogs_list_count(&bearer->pf_list);

                        if (num_of_flow < OGS_MAX_NUM_OF_FLOW_IN_BEARER) {
                            pcc_rule->type = OGS_PCC_RULE_TYPE_INSTALL;
                            gx_message->session_data.num_of_pcc_rule++;
                        } else {
                            ogs_error("Overflow : Num Of Flow %d", num_of_flow);
                            OGS_PCC_RULE_FREE(pcc_rule);
                            result_code = OGS_DIAM_GX_DIAMETER_PCC_RULE_EVENT;
                            goto out;
                        }
                    } else {
                        ogs_error("Overflow: Number of PCCRule");
                    }
                    break;
                default:
                    ogs_debug("Ignoring unsupported AVP(%d) in charging rule install", hdr->avp_code);
                    break;
                }
                fd_msg_browse(avpch1, MSG_BRW_NEXT, &avpch1, NULL);
            }
            break;
        case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_REMOVE:
            ret = fd_msg_browse(avp, MSG_BRW_FIRST_CHILD, &avpch1, NULL);
            ogs_assert(ret == 0);
            while (avpch1) {
                ret = fd_msg_avp_hdr(avpch1, &hdr);
                ogs_assert(ret == 0);
                switch (hdr->avp_code) {
                case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_NAME:
                    if (gx_message->session_data.num_of_pcc_rule <
                            OGS_MAX_NUM_OF_PCC_RULE) {
                        ogs_pcc_rule_t *pcc_rule =
                            &gx_message->session_data.pcc_rule
                                [gx_message->session_data.num_of_pcc_rule];

                        pcc_rule->name = ogs_strdup(
                                (char*)hdr->avp_value->os.data);
                        ogs_assert(pcc_rule->name);

                        pcc_rule->type = OGS_PCC_RULE_TYPE_REMOVE;
                        gx_message->session_data.num_of_pcc_rule++;
                    } else {
                        ogs_error("Overflow: Number of PCCRule");
                    }
                    break;
                default:
                    ogs_debug("Ignoring unsupported AVP(%d) in charging rule remove", hdr->avp_code);
                    break;
                }
                fd_msg_browse(avpch1, MSG_BRW_NEXT, &avpch1, NULL);
            }
            break;
        default:
            ogs_debug("Ignoring unsupported AVP(%d) in main context", hdr->avp_code);
            break;
        }
        fd_msg_browse(avp, MSG_BRW_NEXT, &avp, NULL);
    }

    /* Send Gx Event to SMF State Machine */
    e = smf_event_new(SMF_EVT_GX_MESSAGE);
    ogs_assert(e);

    e->sess_id = sess->id;
    e->gx_message = gx_message;
    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed:%d", (int)rv);
        OGS_SESSION_DATA_FREE(&gx_message->session_data);
        ogs_free(gx_message);
        ogs_event_free(e);
    } else {
        ogs_pollset_notify(ogs_app()->pollset);
    }

    /* Set the Auth-Application-Id AVP */
    ret = fd_msg_avp_new(ogs_diam_auth_application_id, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = OGS_DIAM_GX_APPLICATION_ID;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(ans, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Origin-Host, Origin-Realm, andResult-Code AVPs */
    ret = fd_msg_rescode_set(ans, (char *)"DIAMETER_SUCCESS", NULL, NULL, 1);
    ogs_assert(ret == 0);

    /* Store this value in the session */
    gx_state_reattach(session, &sess_data);

    /* Send the answer */
    ret = fd_msg_send(msg, NULL, NULL);
    ogs_assert(ret == 0);

    ogs_debug("Re-Auth-Answer");

    /* Add this value to the stats */
    ogs_assert(pthread_mutex_lock(&ogs_diam_stats_self()->stats_lock) == 0);
    ogs_diam_stats_self()->stats.nb_echoed++;
    ogs_assert(pthread_mutex_unlock(&ogs_diam_stats_self()->stats_lock) == 0);

    return 0;

out:
    ogs_debug("[GX_DANGLING] out: result_code=%u sess_data=%p",
              result_code, sess_data);

    if (result_code == OGS_DIAM_UNKNOWN_SESSION_ID) {
        ret = fd_msg_rescode_set(ans,
                    (char *)"DIAMETER_UNKNOWN_SESSION_ID", NULL, NULL, 1);
        ogs_assert(ret == 0);
    } else {
        ret = ogs_diam_message_experimental_rescode_set(ans, result_code);
        ogs_assert(ret == 0);
    }

    /*
     * Only store when we actually own a state to give back.
     *
     * fd_sess_state_retrieve() DETACHES the state from the session, so a
     * NULL here means either (a) the state is genuinely gone, or (b) another
     * thread is holding it right now -- the SMF main thread runs
     * fd_sess_state_retrieve()/store() pairs in smf_gx_send_ccr() (the whole
     * CCR AVP assembly sits between them) while this callback runs on a
     * freeDiameter dispatch thread.
     *
     * fd_sess_state_store() does NOT special-case NULL: it allocates a state
     * wrapper, assigns new->state = NULL and links it into session->states
     * (libfdproto/sessions.c). That occupies the slot for smf_gx_reg, so the
     * owning thread's subsequent store() finds an entry, returns EALREADY and
     * trips its ogs_assert(ret == 0) -> ogs_abort(). It also strands the real
     * sess_state and makes every later retrieve() return NULL, permanently
     * breaking that Gx session.
     *
     * Note the !sess path above deliberately does NOT state_cleanup(): it
     * arrives here with sess_data still valid precisely so the state gets
     * re-attached (see the ownership comment there). The only way to reach
     * this point with sess_data == NULL is the !sess_data branch, where
     * nothing was detached in the first place.
     *
     * This mirrors the discipline smf_gx_cca_cb() follows on its own
     * retrieve-returned-NULL path.
     */
    gx_state_reattach(session, &sess_data);

    ret = fd_msg_send(msg, NULL, NULL);
    ogs_assert(ret == 0);

    OGS_SESSION_DATA_FREE(&gx_message->session_data);
    ogs_free(gx_message);

    return 0;
}

int smf_gx_init(void)
{
    int ret;
    struct disp_when data;

    ogs_thread_mutex_init(&sess_state_mutex);
    ogs_pool_init(&sess_state_pool, ogs_app()->pool.sess);

    /* Install objects definitions for this application */
    ret = ogs_diam_gx_init();
    ogs_assert(ret == 0);

    /* Create handler for sessions */
    ret = fd_sess_handler_create(&smf_gx_reg, state_cleanup, NULL, NULL);
    ogs_assert(ret == 0);

    memset(&data, 0, sizeof(data));
    data.app = ogs_diam_gx_application;

    ret = fd_disp_register(smf_gx_fb_cb, DISP_HOW_APPID, &data, NULL,
                &hdl_gx_fb);
    ogs_assert(ret == 0);

    data.command = ogs_diam_gx_cmd_rar;
    ret = fd_disp_register(smf_gx_rar_cb, DISP_HOW_CC, &data, NULL,
                &hdl_gx_rar);
    ogs_assert(ret == 0);

    /* Advertise the support for the application in the peer */
    ret = fd_disp_app_support(ogs_diam_gx_application, ogs_diam_vendor, 1, 0);
    ogs_assert(ret == 0);

    return OGS_OK;
}

void smf_gx_final(void)
{
    int ret;

    ret = fd_sess_handler_destroy(&smf_gx_reg, NULL);
    ogs_assert(ret == 0);

    if (hdl_gx_fb)
        (void) fd_disp_unregister(&hdl_gx_fb, NULL);
    if (hdl_gx_rar)
        (void) fd_disp_unregister(&hdl_gx_rar, NULL);

    ogs_pool_final(&sess_state_pool);
    ogs_thread_mutex_destroy(&sess_state_mutex);
}

static int decode_pcc_rule_definition(
        ogs_pcc_rule_t *pcc_rule, struct avp *avpch1, int *perror)
{
    int ret = 0, error = 0;
    struct avp *avpch2, *avpch3, *avpch4;
    struct avp_hdr *hdr;

    ogs_assert(pcc_rule);
    ogs_assert(avpch1);

    ret = fd_msg_browse(avpch1, MSG_BRW_FIRST_CHILD, &avpch2, NULL);
    ogs_assert(ret == 0);
    while (avpch2) {
        ogs_flow_t *flow = NULL;

        ret = fd_msg_avp_hdr(avpch2, &hdr);
        ogs_assert(ret == 0);
        switch (hdr->avp_code) {
        case OGS_DIAM_GX_AVP_CODE_CHARGING_RULE_NAME:
            if (pcc_rule->name) {
                ogs_error("PCC Rule Name has already been defined");
                ogs_free(pcc_rule->name);
            }
            pcc_rule->name = ogs_strdup((char*)hdr->avp_value->os.data);
            ogs_assert(pcc_rule->name);
            break;
        case OGS_DIAM_GX_AVP_CODE_FLOW_INFORMATION:
            if (pcc_rule->num_of_flow < OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE) {
                flow = &pcc_rule->flow[pcc_rule->num_of_flow];

                ret = fd_avp_search_avp(
                        avpch2, ogs_diam_gx_flow_direction, &avpch3);
                ogs_assert(ret == 0);
                if (avpch3) {
                    ret = fd_msg_avp_hdr( avpch3, &hdr);
                    ogs_assert(ret == 0);
                    flow->direction = hdr->avp_value->i32;
                }

                ret = fd_avp_search_avp(
                        avpch2, ogs_diam_gx_flow_description, &avpch3);
                ogs_assert(ret == 0);
                if (avpch3) {
                    ret = fd_msg_avp_hdr(avpch3, &hdr);
                    ogs_assert(ret == 0);
                    flow->description = ogs_strndup(
                        (char*)hdr->avp_value->os.data, hdr->avp_value->os.len);
                    ogs_assert(flow->description);
                }

                pcc_rule->num_of_flow++;
            } else {
                ogs_error("Overflow: Num of Flow [%d]", pcc_rule->num_of_flow);
                error++;
            }
            break;
        case OGS_DIAM_GX_AVP_CODE_FLOW_STATUS:
            pcc_rule->flow_status = hdr->avp_value->i32;
            break;
        case OGS_DIAM_GX_AVP_CODE_QOS_INFORMATION:
            ret = fd_avp_search_avp(avpch2,
                ogs_diam_gx_qos_class_identifier, &avpch3);
            ogs_assert(ret == 0);
            if (avpch3) {
                ret = fd_msg_avp_hdr(avpch3, &hdr);
                ogs_assert(ret == 0);
                pcc_rule->qos.index = hdr->avp_value->u32;
            } else {
                ogs_error("no_QCI");
                error++;
            }

            ret = fd_avp_search_avp(avpch2,
                ogs_diam_gx_allocation_retention_priority, &avpch3);
            ogs_assert(ret == 0);
            if (avpch3) {
                ret = fd_avp_search_avp(
                        avpch3, ogs_diam_gx_priority_level, &avpch4);
                ogs_assert(ret == 0);
                if (avpch4) {
                    ret = fd_msg_avp_hdr(avpch4, &hdr);
                    ogs_assert(ret == 0);
                    pcc_rule->qos.arp.priority_level = hdr->avp_value->u32;
                } else {
                    ogs_error("no_Priority-Level");
                    error++;
                }

                /*
                 * Ch 7.3.40 Allocation-Retenion-Proirty in TS 29.272 V15.9.0
                 *
                 * If the Pre-emption-Capability AVP is not present in the
                 * Allocation-Retention-Priority AVP, the default value shall be
                 * PRE-EMPTION_CAPABILITY_DISABLED (1).
                 *
                 * If the Pre-emption-Vulnerability AVP is not present in the
                 * Allocation-Retention-Priority AVP, the default value shall be
                 * PRE-EMPTION_VULNERABILITY_ENABLED (0).
                 *
                 * However, to easily set up VoLTE service,
                 * enable Pre-emption Capability/Vulnerablility
                 * in Default Bearer
                 */
                ret = fd_avp_search_avp(avpch3,
                    ogs_diam_gx_pre_emption_capability, &avpch4);
                ogs_assert(ret == 0);
                if (avpch4) {
                    ret = fd_msg_avp_hdr(avpch4, &hdr);
                    ogs_assert(ret == 0);
                    pcc_rule->qos.arp.pre_emption_capability =
                            hdr->avp_value->u32;
                } else {
                    pcc_rule->qos.arp.pre_emption_capability =
                        OGS_EPC_PRE_EMPTION_DISABLED;
                }

                ret = fd_avp_search_avp(avpch3,
                        ogs_diam_gx_pre_emption_vulnerability, &avpch4);
                ogs_assert(ret == 0);
                if (avpch4) {
                    ret = fd_msg_avp_hdr(avpch4, &hdr);
                    ogs_assert(ret == 0);
                    pcc_rule->qos.arp.pre_emption_vulnerability =
                            hdr->avp_value->u32;
                } else {
                    pcc_rule->qos.arp.pre_emption_vulnerability =
                        OGS_EPC_PRE_EMPTION_ENABLED;

                }
            } else {
                ogs_error("no_ARP");
                error++;
            }

            ret = fd_avp_search_avp(avpch2,
                    ogs_diam_gx_max_requested_bandwidth_ul, &avpch3);
            ogs_assert(ret == 0);
            if (avpch3) {
                ret = fd_msg_avp_hdr(avpch3, &hdr);
                ogs_assert(ret == 0);
                pcc_rule->qos.mbr.uplink = hdr->avp_value->u32;
            }
            ret = fd_avp_search_avp(avpch2,
                ogs_diam_gx_max_requested_bandwidth_dl, &avpch3);
            ogs_assert(ret == 0);
            if (avpch3) {
                ret = fd_msg_avp_hdr(avpch3, &hdr);
                ogs_assert(ret == 0);
                pcc_rule->qos.mbr.downlink = hdr->avp_value->u32;
            }
            ret = fd_avp_search_avp(avpch2,
                    ogs_diam_gx_guaranteed_bitrate_ul, &avpch3);
            ogs_assert(ret == 0);
            if (avpch3) {
                ret = fd_msg_avp_hdr(avpch3, &hdr);
                ogs_assert(ret == 0);
                pcc_rule->qos.gbr.uplink = hdr->avp_value->u32;
            }
            ret = fd_avp_search_avp(avpch2,
                ogs_diam_gx_guaranteed_bitrate_dl, &avpch3);
            ogs_assert(ret == 0);
            if (avpch3) {
                ret = fd_msg_avp_hdr(avpch3, &hdr);
                ogs_assert(ret == 0);
                pcc_rule->qos.gbr.downlink = hdr->avp_value->u32;
            }
            break;
        case OGS_DIAM_GX_AVP_CODE_PRECEDENCE:
            pcc_rule->precedence = hdr->avp_value->i32;
            break;
        case OGS_DIAM_GX_AVP_CODE_RATING_GROUP:
            pcc_rule->rating_group = hdr->avp_value->i32;
            break;
        default:
            ogs_debug("Ignoring unsupported AVP(%d) in PCC rule definition", hdr->avp_code);
            break;
        }
        fd_msg_browse(avpch2, MSG_BRW_NEXT, &avpch2, NULL);
    }

    if (perror)
        *perror = error;

    return OGS_OK;
}
