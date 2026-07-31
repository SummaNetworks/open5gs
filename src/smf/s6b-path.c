/* 3GPP TS 29.273 section 9
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

#include "fd-path.h"
#include "metrics.h"

static struct session_handler *smf_s6b_reg = NULL;
static struct disp_hdl *hdl_s6b_fb = NULL;

struct sess_state {
    /*
     * Pool id, not a raw pointer: the callbacks run long after the request
     * was sent and the smf_sess_t may have been removed meanwhile (the
     * TS 29.274 7.2.1 collision path does exactly that). A stale pointer
     * would survive ogs_assert(sess) and be dereferenced; a stale id simply
     * fails to resolve.
     */
    ogs_pool_id_t sess_id;
    os0_t       s6b_sid;             /* S6B Session-Id */

    ogs_pool_id_t xact_id;

    struct timespec ts; /* Time of sending the message */
};

static OGS_POOL(sess_state_pool, struct sess_state);
static ogs_thread_mutex_t sess_state_mutex;

static void smf_s6b_aaa_cb(void *data, struct msg **msg);
static void smf_s6b_sta_cb(void *data, struct msg **msg);

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

    new->s6b_sid = (os0_t)ogs_strdup((char *)sid);
    if (!new->s6b_sid) {
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

    if (sess_data->s6b_sid)
        ogs_free(sess_data->s6b_sid);

    ogs_thread_mutex_lock(&sess_state_mutex);
    ogs_pool_free(&sess_state_pool, sess_data);
    ogs_thread_mutex_unlock(&sess_state_mutex);
}

/*
 * Re-attach a state that fd_sess_state_retrieve() detached, tolerating the
 * case where a newer state already owns this handler's slot.
 *
 * fd_sess_state_store() returns EALREADY - leaving *sess_data untouched -
 * when an entry for smf_s6b_reg is already linked, which happens when the
 * SMF main thread sent a new S6b request while this callback held the state
 * detached. Asserting ret == 0 there would abort the whole process for what
 * is a per-session race, so treat EALREADY as "we are holding a redundant
 * state": a newer one owns the slot and smf_sess_t keeps its own copy of
 * the Session-Id, which makes state_cleanup() the correct disposal.
 *
 * A NULL *sess_data is a no-op: nothing was detached, and storing NULL
 * would link a NULL-payload entry that steals the slot from whoever really
 * owns the state (libfdproto does not special-case NULL).
 */
static bool s6b_state_reattach(struct session *session,
        struct sess_state **sess_data)
{
    int ret;

    ogs_assert(sess_data);

    if (!*sess_data)
        return true;

    ret = fd_sess_state_store(smf_s6b_reg, session, sess_data);
    if (ret != 0) {
        ogs_warn("fd_sess_state_store() failed (%d) - a newer S6b state owns "
                "the slot, releasing the orphaned state [%s]", ret,
                (*sess_data)->s6b_sid ?
                    (char *)(*sess_data)->s6b_sid : "(null)");
        state_cleanup(*sess_data, NULL, NULL);
        *sess_data = NULL;
        return false;
    }

    ogs_assert(*sess_data == NULL);
    return true;
}

/*
 * Dispose of a detached state when an answer cannot be processed.
 *
 * Never just re-attach: the Session-Id may have no user left, in which case
 * re-attaching would leave behind exactly the orphan this series reclaims.
 * The owner is identified by comparing Session-Id strings, since each side
 * keeps its own copy.
 *
 * Always leaves *sess_data NULL, so the caller can return without stranding
 * anything.
 */
static void s6b_state_dispose(struct session *session,
        struct sess_state **sess_data)
{
    smf_sess_t *sess = NULL;

    ogs_assert(sess_data);

    if (!*sess_data)
        return;

    sess = smf_sess_find_by_id((*sess_data)->sess_id);
    if (sess && sess->s6b_sid && (*sess_data)->s6b_sid &&
            strcmp(sess->s6b_sid, (char *)(*sess_data)->s6b_sid) == 0) {
        s6b_state_reattach(session, sess_data);
        return;
    }

    ogs_warn("S6b Session-Id [%s] no longer used - releasing state",
            (*sess_data)->s6b_sid ?
                (char *)(*sess_data)->s6b_sid : "(null)");
    state_cleanup(*sess_data, NULL, NULL);
    *sess_data = NULL;
}

/*
 * Report an S6b request that could not be sent, as if the AAA server had
 * answered it with a failure.
 *
 * The caller sets sm_data.s6b_aar_in_flight around the send and the GSM
 * state machine only leaves smf_gsm_state_wait_epc_auth_initial through
 * test_can_proceed(), which is driven purely by sm_data and has no S6b
 * timeout. Returning silently would therefore wedge the session on an
 * answer that was never asked for - no Create Session Response, no error.
 *
 * Mirrors gx_report_ccr_send_failure(): synthesise the failure so the
 * existing, already-tested handling clears the flag, records the error and
 * runs test_can_proceed(). When no GTP transaction can be resolved the
 * event is not posted - the receiving branches and test_can_proceed()
 * dereference it - and the failure is recorded in sm_data directly.
 *
 * Used for both directions: the AAR gates
 * smf_gsm_state_wait_epc_auth_initial via s6b_aar_in_flight, and the STR
 * gates smf_gsm_state_wait_epc_auth_release via s6b_str_in_flight.
 */
static void s6b_report_send_failure(smf_sess_t *sess,
        ogs_pool_id_t xact_id, uint32_t cmd_code)
{
    ogs_diam_s6b_message_t *s6b_message = NULL;
    smf_event_t *e = NULL;
    int rv;
    bool is_str = (cmd_code == OGS_DIAM_S6B_CMD_SESSION_TERMINATION);

    ogs_assert(sess);

    if (xact_id < OGS_MIN_POOL_ID || xact_id > OGS_MAX_POOL_ID ||
            !ogs_gtp_xact_find_by_id(xact_id)) {
        ogs_error("S6b request(cmd:%d) not sent and no GTP transaction to "
                "report it on - recording the failure only", cmd_code);
        if (is_str) {
            sess->sm_data.s6b_str_in_flight = false;
            sess->sm_data.s6b_sta_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        } else {
            sess->sm_data.s6b_aar_in_flight = false;
            sess->sm_data.s6b_aaa_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        }
        return;
    }

    s6b_message = ogs_calloc(1, sizeof(ogs_diam_s6b_message_t));
    if (!s6b_message) {
        /* Fall back to recording it directly, or the in-flight flag stays
         * set and test_can_proceed() never converges. */
        ogs_error("ogs_calloc() failed");
        if (is_str) {
            sess->sm_data.s6b_str_in_flight = false;
            sess->sm_data.s6b_sta_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        } else {
            sess->sm_data.s6b_aar_in_flight = false;
            sess->sm_data.s6b_aaa_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        }
        return;
    }
    s6b_message->cmd_code = cmd_code;
    s6b_message->result_code = ER_DIAMETER_UNABLE_TO_COMPLY;

    e = smf_event_new(SMF_EVT_S6B_MESSAGE);
    ogs_assert(e);
    e->sess_id = sess->id;
    e->s6b_message = s6b_message;
    e->gtp_xact_id = xact_id;

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        /* The report is what makes test_can_proceed() converge, so its own
         * failure must not leave the flag set. */
        ogs_error("ogs_queue_push() failed:%d", (int)rv);
        ogs_free(s6b_message);
        ogs_event_free(e);
        if (is_str) {
            sess->sm_data.s6b_str_in_flight = false;
            sess->sm_data.s6b_sta_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        } else {
            sess->sm_data.s6b_aar_in_flight = false;
            sess->sm_data.s6b_aaa_err = ER_DIAMETER_UNABLE_TO_COMPLY;
        }
        return;
    }

    ogs_pollset_notify(ogs_app()->pollset);
}

static int smf_s6b_fb_cb(struct msg **msg, struct avp *avp,
        struct session *sess, void *opaque, enum disp_action *act)
{
    /* This CB should never be called */
    ogs_warn("Unexpected message received!");

    return ENOTSUP;
}


/*
 * No answer came back. freeDiameter has dropped the request from its
 * sent-request table by now, so a late answer finds no query and is
 * discarded; this is the last chance to release whatever is waiting.
 *
 * Runs on a freeDiameter thread. `data` is compared, never dereferenced, and
 * the state is recovered from the session - the same discipline the answer
 * callbacks use. *req is left in place for freeDiameter to free.
 */
static void s6b_request_expire_cb(void *data, DiamId_t peer_id,
        size_t peer_len, struct msg **req, uint32_t cmd_code)
{
    struct session *session = NULL;
    struct sess_state *sess_data = NULL;
    smf_sess_t *sess = NULL;
    ogs_pool_id_t xact_id = OGS_INVALID_POOL_ID;
    bool is_str = (cmd_code == OGS_DIAM_S6B_CMD_SESSION_TERMINATION);
    int ret;

    if (!req || !*req)
        return;

    ret = fd_msg_sess_get(fd_g_config->cnf_dict, *req, &session, NULL);
    if (ret != 0 || !session) {
        ogs_error("S6b request timed out but its Session could not be "
                "resolved");
        return;
    }

    ret = fd_sess_state_retrieve(smf_s6b_reg, session, &sess_data);
    if (ret != 0 || !sess_data) {
        /* The answer path got there first and already converged. */
        return;
    }
    if ((void *)sess_data != data) {
        /* A newer request owns the slot - put it back untouched. */
        s6b_state_reattach(session, &sess_data);
        return;
    }

    xact_id = sess_data->xact_id;
    sess = smf_sess_find_by_id(sess_data->sess_id);

    ogs_error("S6b %s timed out after %ds [%s]",
            is_str ? "STR" : "AAR",
            (int)ogs_time_to_sec(
                ogs_local_conf()->time.message.diameter.timeout_duration),
            sess_data->s6b_sid ? (char *)sess_data->s6b_sid : "(null)");

    /*
     * Only report while the flag this request set is still up. If it has
     * already been cleared the procedure moved on without us, and failing it
     * now would consume a GTP transaction that belongs to something else.
     */
    if (sess &&
            ((is_str && sess->sm_data.s6b_str_in_flight) ||
             (!is_str && sess->sm_data.s6b_aar_in_flight)))
        s6b_report_send_failure(sess, xact_id, cmd_code);

    s6b_state_dispose(session, &sess_data);
}

static void smf_s6b_aar_expire_cb(void *data, DiamId_t peer_id,
        size_t peer_len, struct msg **req)
{
    s6b_request_expire_cb(data, peer_id, peer_len, req,
            OGS_DIAM_S6B_CMD_AUTHENTICATION_AUTHORIZATION);
}

static void smf_s6b_str_expire_cb(void *data, DiamId_t peer_id,
        size_t peer_len, struct msg **req)
{
    s6b_request_expire_cb(data, peer_id, peer_len, req,
            OGS_DIAM_S6B_CMD_SESSION_TERMINATION);
}

void smf_s6b_send_aar(smf_sess_t *sess, ogs_gtp_xact_t *xact)
{
    int ret;

    struct msg *req = NULL;
    struct avp *avp;
    union avp_value val;
    struct sess_state *sess_data = NULL, *svg;
    struct session *session = NULL;
    int new;

    smf_ue_t *smf_ue = NULL;
    char *user_name = NULL;
    char *visited_network_identifier = NULL;

    struct avp *mip6_agent_info, *mip_home_agent_address;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;

    ogs_assert(xact);
    ogs_assert(sess);
    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);

    ogs_debug("[AA-Request]");

    /* Create the request */
    ret = fd_msg_new(ogs_diam_rx_cmd_aar, MSGFL_ALLOC_ETEID, &req);
    ogs_assert(ret == 0);
    {
        struct msg_hdr * h;
        ret = fd_msg_hdr( req, &h );
        ogs_assert(ret == 0);
        h->msg_appl = OGS_DIAM_S6B_APPLICATION_ID;
    }

    /* Find Diameter S6b Session */
    if (sess->s6b_sid) {
        /* Retrieve session by Session-Id */
        size_t sidlen = strlen(sess->s6b_sid);
        ret = fd_sess_fromsid_msg((os0_t)sess->s6b_sid, sidlen, &session, &new);
        ogs_assert(ret == 0);
        ogs_assert(new == 0);

        ogs_debug("    Found S6b Session-Id: [%s]", sess->s6b_sid);

        /* Add Session-Id to the message */
        ret = ogs_diam_message_session_id_set(
                req, (os0_t)sess->s6b_sid, sidlen);
        ogs_assert(ret == 0);
        /* Save the session associated with the message */
        ret = fd_msg_sess_set(req, session);
    } else {
        /* Create a new session */
        #define OGS_DIAM_S6B_APP_SID_OPT  "app_s6b"
        ret = fd_msg_new_session(req, (os0_t)OGS_DIAM_S6B_APP_SID_OPT,
                CONSTSTRLEN(OGS_DIAM_S6B_APP_SID_OPT));
        ogs_assert(ret == 0);
        ret = fd_msg_sess_get(fd_g_config->cnf_dict, req, &session, NULL);
        ogs_assert(ret == 0);
    }

    /* Retrieve session state in this session */
    ret = fd_sess_state_retrieve(smf_s6b_reg, session, &sess_data);
    if (!sess_data) {
        os0_t sid;
        size_t sidlen;

        ret = fd_sess_getsid(session, &sid, &sidlen);
        ogs_assert(ret == 0);

        /* Allocate new session state memory */
        sess_data = new_state(sid);
        if (!sess_data) {
            /*
             * Same wedge as on Gx: the caller has set s6b_aar_in_flight and
             * there is no S6b timeout, so a silent return would leave
             * test_can_proceed() waiting forever. Report it as a failed AAA.
             */
            ogs_error("new_state() failed: S6b sess_state_pool exhausted "
                    "- abandoning AAR");
            fd_msg_free(req);
            s6b_report_send_failure(sess,
                    xact ? xact->id : OGS_INVALID_POOL_ID,
                    OGS_DIAM_S6B_CMD_AUTHENTICATION_AUTHORIZATION);
            return;
        }

        ogs_debug("    Allocate new session: [%s]", sess_data->s6b_sid);

        /*
         * sess->s6b_sid is NOT published here: it is set after the state has
         * been stored successfully, at the end of this function. Publishing
         * now would advertise ownership of a Session-Id whose state may lose
         * the slot, and the assignment would overwrite - and leak - any copy
         * already held.
         */
    } else
        ogs_debug("    Retrieve session: [%s]", sess_data->s6b_sid);

    /* Update session state */
    sess_data->sess_id = sess->id;
    sess_data->xact_id = xact ? xact->id : OGS_INVALID_POOL_ID;

    /* Set Origin-Host & Origin-Realm */
    ret = fd_msg_add_origin(req, 0);
    ogs_assert(ret == 0);

    /* Set the Destination-Realm AVP */
    ret = fd_msg_avp_new(ogs_diam_destination_realm, 0, &avp);
    ogs_assert(ret == 0);
    val.os.data = (unsigned char *)(fd_g_config->cnf_diamrlm);
    val.os.len  = strlen(fd_g_config->cnf_diamrlm);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Auth-Application-Id AVP */
    ret = fd_msg_avp_new(ogs_diam_auth_application_id, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = OGS_DIAM_S6B_APPLICATION_ID;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Auth-Request-Type AVP */
    ret = fd_msg_avp_new(ogs_diam_auth_request_type, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = OGS_DIAM_AUTH_REQUEST_TYPE_AUTHORIZE_ONLY;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set RAT-Type */
    ret = fd_msg_avp_new(ogs_diam_rat_type, 0, &avp);
    ogs_assert(ret == 0);

    switch (sess->gtp_rat_type) {
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

    /* Set the User-Name AVP */
    user_name = ogs_msprintf("0%s@nai.epc.mnc%03d.mcc%03d.3gppnetwork.org",
                    smf_ue->imsi_bcd,
                    ogs_plmn_id_mnc(&sess->serving_plmn_id),
                    ogs_plmn_id_mcc(&sess->serving_plmn_id));
    ogs_assert(user_name);

    ret = fd_msg_avp_new(ogs_diam_user_name, 0, &avp);
    ogs_assert(ret == 0);
    val.os.data = (uint8_t *)user_name;
    val.os.len = strlen(user_name);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set MIP6-Feature-Vector */
    ret = fd_msg_avp_new(ogs_diam_s6b_mip6_feature_vector, 0, &avp);
    ogs_assert(ret == 0);
    val.u64 = 0x0000400000000000LL; /* GTPv2_SUPPORTED: Set */
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set MIP6-Agent-Info */
    ret = fd_msg_avp_new(ogs_diam_mip6_agent_info, 0, &mip6_agent_info);
    ogs_assert(ret == 0);

    if (ogs_gtp_self()->gtpc_addr) {
        ret = fd_msg_avp_new(ogs_diam_mip_home_agent_address, 0,
                    &mip_home_agent_address);
        ogs_assert(ret == 0);
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr =
            ogs_gtp_self()->gtpc_addr->sin.sin_addr.s_addr;
        ret = fd_msg_avp_value_encode (
                    &sin, mip_home_agent_address );
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(mip6_agent_info,
                MSG_BRW_LAST_CHILD, mip_home_agent_address);
        ogs_assert(ret == 0);
    }

    if (ogs_gtp_self()->gtpc_addr6) {
        ret = fd_msg_avp_new(ogs_diam_mip_home_agent_address, 0,
                    &mip_home_agent_address);
        ogs_assert(ret == 0);
        sin6.sin6_family = AF_INET6;
        memcpy(sin6.sin6_addr.s6_addr,
                ogs_gtp_self()->gtpc_addr6->sin6.sin6_addr.s6_addr,
                OGS_IPV6_LEN);
        ret = fd_msg_avp_value_encode (
                    &sin6, mip_home_agent_address );
        ogs_assert(ret == 0);
        ret = fd_msg_avp_add(mip6_agent_info,
                MSG_BRW_LAST_CHILD, mip_home_agent_address);
        ogs_assert(ret == 0);
    }

    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, mip6_agent_info);
    ogs_assert(ret == 0);

    /* Set the Visited-Network-Identifier AVP */
    visited_network_identifier =
                ogs_msprintf("mnc%03d.mcc%03d.3gppnetwork.org",
                    ogs_plmn_id_mnc(&sess->serving_plmn_id),
                    ogs_plmn_id_mcc(&sess->serving_plmn_id));
    ogs_assert(visited_network_identifier);

    ret = fd_msg_avp_new(ogs_diam_visited_network_identifier, 0, &avp);
    ogs_assert(ret == 0);
    val.os.data = (unsigned char *)(visited_network_identifier);
    val.os.len  = strlen(visited_network_identifier);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set Service-Selection */
    ret = fd_msg_avp_new(ogs_diam_service_selection, 0, &avp);
    ogs_assert(ret == 0);
    ogs_assert(sess->session.name);
    val.os.data = (uint8_t*)sess->session.name;
    val.os.len = strlen(sess->session.name);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Keep a pointer to the session data for debug purpose,
     * in real life we would not need it */
    svg = sess_data;

    /* Store this value in the session */
    if (s6b_state_reattach(session, &sess_data) == false) {
        /*
         * A newer state took the slot, so ours has been released. Sending
         * now would hand freeDiameter a callback cookie pointing at freed
         * state, and the answer would be discarded on the stale-data path -
         * leaving s6b_aar_in_flight set forever. Abandon the request and
         * report it, exactly as the Gx sender does.
         */
        ogs_error("S6b AAR: state lost the slot - abandoning request");
        fd_msg_free(req);
        s6b_report_send_failure(sess, xact ? xact->id : OGS_INVALID_POOL_ID,
                OGS_DIAM_S6B_CMD_AUTHENTICATION_AUTHORIZATION);
        return;
    }

    /*
     * Publish the Session-Id only now that the state is attached, and only
     * when the value actually changes - mirroring the Gx sender. Publishing
     * earlier would advertise ownership of a Session-Id whose state may not
     * have made it into the slot, and overwriting without freeing would leak
     * the previous copy.
     */
    if (!sess->s6b_sid || strcmp(sess->s6b_sid, (char *)svg->s6b_sid) != 0) {
        if (sess->s6b_sid)
            ogs_free(sess->s6b_sid);
        sess->s6b_sid = ogs_strdup((char *)svg->s6b_sid);
        ogs_assert(sess->s6b_sid);
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
        ret = fd_msg_send_timeout(&req, smf_s6b_aaa_cb, svg, smf_s6b_aar_expire_cb, &ts);
    }
    if (ret != 0) {
        ogs_error("fd_msg_send() failed (%d) - abandoning S6b AAR", ret);
        s6b_report_send_failure(sess, xact ? xact->id : OGS_INVALID_POOL_ID,
                OGS_DIAM_S6B_CMD_AUTHENTICATION_AUTHORIZATION);
        return;
    }

    /* Increment the counter */
    ogs_assert(pthread_mutex_lock(&ogs_diam_stats_self()->stats_lock) == 0);
    ogs_diam_stats_self()->stats.nb_sent++;
    ogs_assert(pthread_mutex_unlock(&ogs_diam_stats_self()->stats_lock) == 0);

    ogs_free(user_name);
    ogs_free(visited_network_identifier);
}

static void smf_s6b_aaa_cb(void *data, struct msg **msg)
{
    int ret;

    struct sess_state *sess_data = NULL;
    struct timespec ts;
    struct session *session;
    struct avp *avp, *avpch1;
    struct avp_hdr *hdr;
    unsigned long dur;
    int error = 0;
    int new;

    smf_sess_t *sess = NULL;
    smf_event_t *e = NULL;
    ogs_diam_s6b_message_t *s6b_message = NULL;

    ogs_debug("[AA-Answer]");

    ret = clock_gettime(CLOCK_REALTIME, &ts);
    ogs_assert(ret == 0);

    /* Search the session, retrieve its data */
    ret = fd_msg_sess_get(fd_g_config->cnf_dict, *msg, &session, &new);
    ogs_assert(ret == 0);
    ogs_assert(new == 0);

    ogs_debug("    Search the session");

    ret = fd_sess_state_retrieve(smf_s6b_reg, session, &sess_data);
    ogs_assert(ret == 0);
    if (!sess_data) {
        /* Consume the answer here rather than relying on freeDiameter's
         * fallback-handler drop path. */
        ogs_error("No Session Data");
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }
    if ((void *)sess_data != data) {
        /*
         * The state this request was sent with is no longer the one attached
         * to the Session-Id - the same detached-state/replacement race that
         * s6b_state_reattach() handles from the other side. The answer
         * belongs to the old request, so put the current state back and drop
         * it rather than acting on the wrong transaction.
         *
         * `data` is only compared, never dereferenced: it may already have
         * been returned to sess_state_pool.
         */
        ogs_warn("S6b answer for a stale sess_state - dropping [%s]",
                sess_data->s6b_sid ? (char *)sess_data->s6b_sid : "(null)");
        s6b_state_dispose(session, &sess_data);
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    ogs_debug("    Retrieve its data: [%s]", sess_data->s6b_sid);

    /*
     * Resolve the session that owns this S6b Session-Id. The one named when
     * the request was sent may have been removed since - the TS 29.274
     * 7.2.1 collision path removes sessions locally - so verify the
     * Session-Id still matches rather than trusting the back-reference.
     * When nobody owns it the state is a genuine orphan and is released
     * here; nothing else would ever reclaim it.
     */
    sess = smf_sess_find_by_id(sess_data->sess_id);
    if (!sess || !sess->s6b_sid || !sess_data->s6b_sid ||
            strcmp(sess->s6b_sid, (char *)sess_data->s6b_sid) != 0) {
        ogs_warn("S6b: no session owns Session-Id [%s] - releasing state",
                sess_data->s6b_sid ?
                    (char *)sess_data->s6b_sid : "(null)");
        state_cleanup(sess_data, NULL, NULL);
        sess_data = NULL;
        /* This callback owns the answer; the shared exit is not reached. */
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    s6b_message = ogs_calloc(1, sizeof(ogs_diam_s6b_message_t));
    ogs_assert(s6b_message);
    /* Set Session Termination Command */
    s6b_message->cmd_code = OGS_DIAM_S6B_CMD_AUTHENTICATION_AUTHORIZATION;

    /* Phase 4 PR8: parse IETF Result-Code (RFC 6733 §7.1.3) and
     * 3GPP Experimental-Result-Code (TS 29.230) in parallel. Either
     * may be absent, but they may also be present together — keep
     * both so the GTP-C v2 Cause mapper can disambiguate value 5001
     * (IETF AVP_UNSUPPORTED vs 3GPP USER_UNKNOWN). */

    /* Value of Result Code */
    ret = fd_msg_search_avp(*msg, ogs_diam_result_code, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        s6b_message->result_code = hdr->avp_value->i32;
        /* KPI: S6b AAA result. The AA request initialises the S2b
         * session, so a successful AAA maps to "init". Failures roll
         * up to "error" regardless of the underlying experimental
         * code so the time series stays simple to chart. */
        smf_metrics_inst_by_app_event_inc("s6b",
                s6b_message->result_code == ER_DIAMETER_SUCCESS ?
                    "init" : "error",
                SMF_METR_BY_APP_EVENT_CTR_DIAMETER_LIFECYCLE);
        if (s6b_message->result_code != ER_DIAMETER_SUCCESS) {
            ogs_error("Result Code: %d", s6b_message->result_code);
            error++;
        }
    }

    /* Value of Experimental-Result (vendor + code) */
    ret = fd_msg_search_avp(*msg, ogs_diam_experimental_result, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        struct avp *avpch_vendor = NULL;

        ret = fd_avp_search_avp(avp, ogs_diam_vendor_id, &avpch_vendor);
        ogs_assert(ret == 0);
        if (avpch_vendor) {
            ret = fd_msg_avp_hdr(avpch_vendor, &hdr);
            ogs_assert(ret == 0);
            s6b_message->experimental_vendor_id = hdr->avp_value->u32;
        }

        ret = fd_avp_search_avp(
                avp, ogs_diam_experimental_result_code, &avpch1);
        ogs_assert(ret == 0);
        if (avpch1) {
            ret = fd_msg_avp_hdr(avpch1, &hdr);
            ogs_assert(ret == 0);
            s6b_message->experimental_result_code = hdr->avp_value->u32;
            ogs_error("Experimental Result Code: %u (Vendor-Id: %u)",
                    s6b_message->experimental_result_code,
                    s6b_message->experimental_vendor_id);
            error++;
        }
    }

    if (!s6b_message->result_code &&
        !s6b_message->experimental_result_code) {
        ogs_error("no Result-Code");
        error++;
    }

    /* Value of Origin-Host */
    ret = fd_msg_search_avp(*msg, ogs_diam_origin_host, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        ogs_debug("From '%.*s' ",
                (int)hdr->avp_value->os.len, hdr->avp_value->os.data);
    } else {
        ogs_error("no_Origin-Host ");
        error++;
    }

    /* Value of Origin-Realm */
    ret = fd_msg_search_avp(*msg, ogs_diam_origin_realm, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        ogs_debug("('%.*s') ",
                (int)hdr->avp_value->os.len, hdr->avp_value->os.data);
    } else {
        ogs_error("no_Origin-Realm ");
        error++;
    }

    e = smf_event_new(SMF_EVT_S6B_MESSAGE);
    ogs_assert(e);

    /* Phase 4 PR8: only fall back to the local error counter if neither
     * Result-Code nor Experimental-Result-Code provided a value. With
     * Experimental-Result-Code now stored separately, downstream
     * gtp_cause_from_diameter() can inspect both fields directly. */
    if (error && s6b_message->result_code == ER_DIAMETER_SUCCESS &&
            !s6b_message->experimental_result_code)
            s6b_message->result_code = error;

    e->sess_id = sess->id;
    e->gtp_xact_id = sess_data->xact_id;
    e->s6b_message = s6b_message;
    ret = ogs_queue_push(ogs_app()->queue, e);
    if (ret != OGS_OK) {
        ogs_error("ogs_queue_push() failed:%d", (int)ret);
        ogs_free(s6b_message);
        ogs_event_free(e);
    } else {
        ogs_pollset_notify(ogs_app()->pollset);
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
        ogs_debug("in %d.%06ld sec",
                (int)(ts.tv_sec - sess_data->ts.tv_sec),
                (long)(ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);
    else
        ogs_debug("in %d.%06ld sec",
                (int)(ts.tv_sec + 1 - sess_data->ts.tv_sec),
                (long)(1000000000 + ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);

    s6b_state_reattach(session, &sess_data);

    ret = fd_msg_free(*msg);
    ogs_assert(ret == 0);
    *msg = NULL;

    return;
}

void smf_s6b_send_str(smf_sess_t *sess, ogs_gtp_xact_t *xact, uint32_t cause)
{
    int ret;

    struct msg *req = NULL;
    struct avp *avp;
    union avp_value val;
    struct sess_state *sess_data = NULL, *svg;
    struct session *session = NULL;
    int new;
    size_t sidlen;

    smf_ue_t *smf_ue = NULL;
    char *user_name = NULL;

    ogs_assert(sess);
    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);

    ogs_debug("[Session-Termination-Request]");

    /* Create the request */
    ret = fd_msg_new(ogs_diam_rx_cmd_str, MSGFL_ALLOC_ETEID, &req);
    ogs_assert(ret == 0);
    {
        struct msg_hdr * h;
        ret = fd_msg_hdr( req, &h );
        ogs_assert(ret == 0);
        h->msg_appl = OGS_DIAM_S6B_APPLICATION_ID;
    }

    ogs_assert(sess->s6b_sid);

    /* Retrieve session by Session-Id */
    sidlen = strlen(sess->s6b_sid);
    ret = fd_sess_fromsid_msg((os0_t)sess->s6b_sid, sidlen, &session, &new);
    ogs_assert(ret == 0);
    ogs_assert(new == 0);

    ogs_debug("    Found S6b Session-Id: [%s]", sess->s6b_sid);

    /* Add Session-Id to the message */
    ret = ogs_diam_message_session_id_set(req, (os0_t)sess->s6b_sid, sidlen);
    ogs_assert(ret == 0);
    /* Save the session associated with the message */
    ret = fd_msg_sess_set(req, session);

    /* Retrieve session state in this session */
    ret = fd_sess_state_retrieve(smf_s6b_reg, session, &sess_data);
    ogs_assert(ret == 0);
    if (!sess_data) {
        /*
         * The S6b state is already gone - the orphan-release paths added
         * with the ownership split can reclaim it before a later teardown
         * gets here. Returning silently would leak the request AND wedge
         * smf_gsm_state_wait_epc_auth_release, which waits on
         * s6b_str_in_flight and only ever clears it on STA receipt.
         */
        ogs_error("No Session Data - abandoning STR");
        fd_msg_free(req);
        s6b_report_send_failure(sess,
                xact ? xact->id : OGS_INVALID_POOL_ID,
                OGS_DIAM_S6B_CMD_SESSION_TERMINATION);
        return;
    }
    ogs_debug("    Retrieve session: [%s]", sess_data->s6b_sid);

    /* Update session state */
    sess_data->sess_id = sess->id;
    sess_data->xact_id = xact ? xact->id : OGS_INVALID_POOL_ID;

    /* Set Origin-Host & Origin-Realm */
    ret = fd_msg_add_origin(req, 0);
    ogs_assert(ret == 0);

    /* Set the Destination-Realm AVP */
    ret = fd_msg_avp_new(ogs_diam_destination_realm, 0, &avp);
    ogs_assert(ret == 0);
    val.os.data = (unsigned char *)(fd_g_config->cnf_diamrlm);
    val.os.len  = strlen(fd_g_config->cnf_diamrlm);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Auth-Application-Id AVP */
    ret = fd_msg_avp_new(ogs_diam_auth_application_id, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = OGS_DIAM_S6B_APPLICATION_ID;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the Termination-Cause AVP */
    ret = fd_msg_avp_new(ogs_diam_termination_cause, 0, &avp);
    ogs_assert(ret == 0);
    val.i32 = cause;
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Set the User-Name AVP */
    user_name = ogs_msprintf("0%s@nai.epc.mnc%03d.mcc%03d.3gppnetwork.org",
                    smf_ue->imsi_bcd,
                    ogs_plmn_id_mnc(&sess->serving_plmn_id),
                    ogs_plmn_id_mcc(&sess->serving_plmn_id));
    ogs_assert(user_name);

    ret = fd_msg_avp_new(ogs_diam_user_name, 0, &avp);
    ogs_assert(ret == 0);
    val.os.data = (uint8_t *)user_name;
    val.os.len = strlen(user_name);
    ret = fd_msg_avp_setvalue(avp, &val);
    ogs_assert(ret == 0);
    ret = fd_msg_avp_add(req, MSG_BRW_LAST_CHILD, avp);
    ogs_assert(ret == 0);

    /* Keep a pointer to the session data for debug purpose,
     * in real life we would not need it */
    svg = sess_data;

    /* Store this value in the session */
    if (s6b_state_reattach(session, &sess_data) == false) {
        ogs_error("S6b STR: state lost the slot - abandoning request");
        fd_msg_free(req);
        s6b_report_send_failure(sess, xact ? xact->id : OGS_INVALID_POOL_ID,
                OGS_DIAM_S6B_CMD_SESSION_TERMINATION);
        return;
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
        ret = fd_msg_send_timeout(&req, smf_s6b_sta_cb, svg, smf_s6b_str_expire_cb, &ts);
    }
    if (ret != 0) {
        ogs_error("fd_msg_send() failed (%d) - abandoning S6b STR", ret);
        s6b_report_send_failure(sess, xact ? xact->id : OGS_INVALID_POOL_ID,
                OGS_DIAM_S6B_CMD_SESSION_TERMINATION);
        return;
    }

    /* Increment the counter */
    ogs_assert(pthread_mutex_lock(&ogs_diam_stats_self()->stats_lock) == 0);
    ogs_diam_stats_self()->stats.nb_sent++;
    ogs_assert(pthread_mutex_unlock(&ogs_diam_stats_self()->stats_lock) == 0);

    ogs_free(user_name);
}

static void smf_s6b_sta_cb(void *data, struct msg **msg)
{
    int ret;
    int rv;

    struct sess_state *sess_data = NULL;
    struct timespec ts;
    struct session *session;
    struct avp *avp, *avpch1;
    struct avp_hdr *hdr;
    unsigned long dur;
    int error = 0;
    int new;

    smf_event_t *e = NULL;
    smf_sess_t *sess = NULL;
    ogs_diam_s6b_message_t *s6b_message = NULL;

    ogs_debug("[Session-Termination-Answer]");

    ret = clock_gettime(CLOCK_REALTIME, &ts);
    ogs_assert(ret == 0);

    /* Search the session, retrieve its data */
    ret = fd_msg_sess_get(fd_g_config->cnf_dict, *msg, &session, &new);
    ogs_assert(ret == 0);
    ogs_assert(new == 0);

    ogs_debug("    Search the session");

    ret = fd_sess_state_retrieve(smf_s6b_reg, session, &sess_data);
    ogs_assert(ret == 0);
    if (!sess_data) {
        /* Consume the answer here rather than relying on freeDiameter's
         * fallback-handler drop path. */
        ogs_error("No Session Data");
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }
    if ((void *)sess_data != data) {
        /*
         * The state this request was sent with is no longer the one attached
         * to the Session-Id - the same detached-state/replacement race that
         * s6b_state_reattach() handles from the other side. The answer
         * belongs to the old request, so put the current state back and drop
         * it rather than acting on the wrong transaction.
         *
         * `data` is only compared, never dereferenced: it may already have
         * been returned to sess_state_pool.
         */
        ogs_warn("S6b answer for a stale sess_state - dropping [%s]",
                sess_data->s6b_sid ? (char *)sess_data->s6b_sid : "(null)");
        s6b_state_dispose(session, &sess_data);
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    ogs_debug("    Retrieve its data: [%s]", sess_data->s6b_sid);

    /*
     * Resolve the session that owns this S6b Session-Id. The one named when
     * the request was sent may have been removed since - the TS 29.274
     * 7.2.1 collision path removes sessions locally - so verify the
     * Session-Id still matches rather than trusting the back-reference.
     * When nobody owns it the state is a genuine orphan and is released
     * here; nothing else would ever reclaim it.
     */
    sess = smf_sess_find_by_id(sess_data->sess_id);
    if (!sess || !sess->s6b_sid || !sess_data->s6b_sid ||
            strcmp(sess->s6b_sid, (char *)sess_data->s6b_sid) != 0) {
        ogs_warn("S6b: no session owns Session-Id [%s] - releasing state",
                sess_data->s6b_sid ?
                    (char *)sess_data->s6b_sid : "(null)");
        state_cleanup(sess_data, NULL, NULL);
        sess_data = NULL;
        /* This callback owns the answer; the shared exit is not reached. */
        fd_msg_free(*msg);
        *msg = NULL;
        return;
    }

    s6b_message = ogs_calloc(1, sizeof(ogs_diam_s6b_message_t));
    ogs_assert(s6b_message);
    /* Set Session Termination Command */
    s6b_message->cmd_code = OGS_DIAM_S6B_CMD_SESSION_TERMINATION;

    /* Phase 4 PR8: parse IETF Result-Code and 3GPP Experimental-
     * Result-Code in parallel (see AAA callback for rationale). */

    /* Value of Result Code */
    ret = fd_msg_search_avp(*msg, ogs_diam_result_code, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        s6b_message->result_code = hdr->avp_value->i32;
        s6b_message->err = &s6b_message->result_code;
        if (s6b_message->result_code != ER_DIAMETER_SUCCESS) {
            ogs_error("Result Code: %d", s6b_message->result_code);
            error++;
        }
    }

    /* Value of Experimental-Result (vendor + code) */
    ret = fd_msg_search_avp(*msg, ogs_diam_experimental_result, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        struct avp *avpch_vendor = NULL;

        ret = fd_avp_search_avp(avp, ogs_diam_vendor_id, &avpch_vendor);
        ogs_assert(ret == 0);
        if (avpch_vendor) {
            ret = fd_msg_avp_hdr(avpch_vendor, &hdr);
            ogs_assert(ret == 0);
            s6b_message->experimental_vendor_id = hdr->avp_value->u32;
        }

        ret = fd_avp_search_avp(
                avp, ogs_diam_experimental_result_code, &avpch1);
        ogs_assert(ret == 0);
        if (avpch1) {
            ret = fd_msg_avp_hdr(avpch1, &hdr);
            ogs_assert(ret == 0);
            s6b_message->experimental_result_code = hdr->avp_value->u32;
            s6b_message->exp_err =
                &s6b_message->experimental_result_code;
            ogs_error("Experimental Result Code: %u (Vendor-Id: %u)",
                    s6b_message->experimental_result_code,
                    s6b_message->experimental_vendor_id);
            error++;
        }
    }

    if (!s6b_message->result_code &&
        !s6b_message->experimental_result_code) {
        ogs_error("no Result-Code");
        error++;
    }

    /* Value of Origin-Host */
    ret = fd_msg_search_avp(*msg, ogs_diam_origin_host, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        ogs_debug("From '%.*s' ",
                (int)hdr->avp_value->os.len, hdr->avp_value->os.data);
    } else {
        ogs_error("no_Origin-Host ");
        error++;
    }

    /* Value of Origin-Realm */
    ret = fd_msg_search_avp(*msg, ogs_diam_origin_realm, &avp);
    ogs_assert(ret == 0);
    if (avp) {
        ret = fd_msg_avp_hdr(avp, &hdr);
        ogs_assert(ret == 0);
        ogs_debug("('%.*s') ",
                (int)hdr->avp_value->os.len, hdr->avp_value->os.data);
    } else {
        ogs_error("no_Origin-Realm ");
        error++;
    }

    if (!error) {
        e = smf_event_new(SMF_EVT_S6B_MESSAGE);
        ogs_assert(e);

        e->sess_id = sess->id;
        /*
         * Carry the GTP transaction the STR was sent for, exactly as the AAA
         * callback does. Without it a release FSM that converges on this STA
         * has no transaction to answer, and the Delete Session Response is
         * never sent.
         */
        e->gtp_xact_id = sess_data->xact_id;
        e->s6b_message = s6b_message;
        rv = ogs_queue_push(ogs_app()->queue, e);
        if (rv != OGS_OK) {
            ogs_error("ogs_queue_push() failed:%d", (int)rv);
            ogs_free(s6b_message);
            ogs_event_free(e);
        } else {
            ogs_pollset_notify(ogs_app()->pollset);
        }
    } else {
        ogs_free(s6b_message);
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
        ogs_debug("in %d.%06ld sec",
                (int)(ts.tv_sec - sess_data->ts.tv_sec),
                (long)(ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);
    else
        ogs_debug("in %d.%06ld sec",
                (int)(ts.tv_sec + 1 - sess_data->ts.tv_sec),
                (long)(1000000000 + ts.tv_nsec - sess_data->ts.tv_nsec) / 1000);

    s6b_state_reattach(session, &sess_data);

    ret = fd_msg_free(*msg);
    ogs_assert(ret == 0);
    *msg = NULL;

    return;
}

int smf_s6b_init(void)
{
    int ret;
    struct disp_when data;

    ogs_thread_mutex_init(&sess_state_mutex);
    ogs_pool_init(&sess_state_pool, ogs_app()->pool.sess);

    /* Install objects definitions for this application */
    ret = ogs_diam_s6b_init();
    ogs_assert(ret == 0);

    /* Create handler for sessions */
    ret = fd_sess_handler_create(&smf_s6b_reg, state_cleanup, NULL, NULL);
    ogs_assert(ret == 0);

    memset(&data, 0, sizeof(data));
    data.app = ogs_diam_s6b_application;

    ret = fd_disp_register(smf_s6b_fb_cb, DISP_HOW_APPID, &data, NULL,
                &hdl_s6b_fb);
    ogs_assert(ret == 0);

    /* Advertise the support for the application in the peer */
    ret = fd_disp_app_support(ogs_diam_s6b_application, ogs_diam_vendor, 1, 0);
    ogs_assert(ret == 0);

    return OGS_OK;
}

void smf_s6b_final(void)
{
    int ret;

    ret = fd_sess_handler_destroy(&smf_s6b_reg, NULL);
    ogs_assert(ret == 0);

    if (hdl_s6b_fb)
        (void) fd_disp_unregister(&hdl_s6b_fb, NULL);

    ogs_pool_final(&sess_state_pool);
    ogs_thread_mutex_destroy(&sess_state_mutex);
}
