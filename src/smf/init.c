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

#include "context.h"
#include "fd-path.h"
#include "gtp-path.h"
#include "pfcp-path.h"
#include "sbi-path.h"
#include "metrics.h"

//-----------------------------------------
// Eureka - ktsubouc: start
//-----------------------------------------
#include <ulfius.h>
#include <jansson.h>

#define REST_PORT 8881

int callback_get_ue(const struct _u_request *request, struct _u_response *response, void *user_data);
int callback_get_ue_by_id(const struct _u_request *request, struct _u_response *response, void *user_data);

int callback_get_ue(const struct _u_request *request, struct _u_response *response, void *user_data)
{
    smf_ue_t *smf_ue = NULL;

    json_t *json_response = json_array();

    ogs_list_for_each(&smf_self()->smf_ue_list, smf_ue) {
        json_array_append_new(json_response, json_string(smf_ue->imsi_bcd));
    }

    ulfius_set_json_body_response(response, 200, json_response);
    json_decref(json_response);

    return U_CALLBACK_CONTINUE;
}

int callback_get_ue_by_id(const struct _u_request *request, struct _u_response *response, void *user_data)
{
    smf_ue_t *smf_ue = NULL;

    json_t *json_response = json_object();
    json_t *session_array = json_array();

    const char *imsi_bcd = u_map_get(request->map_url, "imsi");

    ogs_list_for_each(&smf_self()->smf_ue_list, smf_ue) {
        smf_sess_t *sess = NULL;

        if (strcmp(imsi_bcd, smf_ue->imsi_bcd)==0) {
            char buf1[OGS_ADDRSTRLEN];

            json_object_set_new(json_response, "imsi", json_string(smf_ue->imsi_bcd));
            ogs_list_for_each(&smf_ue->sess_list, sess) {
                json_t *sess_obj = json_object();
                json_object_set_new(sess_obj, "apn", json_string(sess->session.name));
//                json_object_set_new(sess_obj, "ue_ip", json_string(ogs_ipv4_to_string(sess->session.ue_ip.addr)));
                json_object_set_new(sess_obj, "ue_ip", json_string(sess->ipv4 ? OGS_INET_NTOP(&sess->ipv4->addr, buf1) : ""));
                json_object_set_new(sess_obj, "dl_octet", json_integer(sess->gy.dl_octets));
                json_object_set_new(sess_obj, "ul_octet", json_integer(sess->gy.ul_octets));

                json_object_set_new(sess_obj, "ambr_downlink", json_integer(sess->session.ambr.downlink));
                json_object_set_new(sess_obj, "ambr_uplink", json_integer(sess->session.ambr.uplink));

                json_object_set_new(sess_obj, "gbr_downlink", json_integer(sess->session.qos.gbr.downlink));
                json_object_set_new(sess_obj, "gbr_uplink", json_integer(sess->session.qos.gbr.uplink));
                json_object_set_new(sess_obj, "mbr_downlink", json_integer(sess->session.qos.mbr.downlink));
                json_object_set_new(sess_obj, "mbr_uplink", json_integer(sess->session.qos.mbr.uplink));

                json_array_append_new(session_array, sess_obj);
            }
            json_object_set_new(json_response, "session", session_array);
            break;
        }
    }

    ulfius_set_json_body_response(response, 200, json_response);
    json_decref(json_response);

    return U_CALLBACK_CONTINUE;
}

static void rest_handler(void *arg)
{
    struct _u_instance instance;

    if (ulfius_init_instance(&instance, REST_PORT, NULL, NULL) != U_OK) {
        ogs_error("Error ulfius_init_instance, abort");
        return;
    }

    // CRUD Endpoint Registrations
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/ue", 0, &callback_get_ue, NULL);
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/ue/:imsi", 0, &callback_get_ue_by_id, NULL);

    if (ulfius_start_framework(&instance) == U_OK) {
        ogs_info("Starting OAM Rest-API server on port %d", instance.port);
        while(1) {
            //getchar(); // Wait for user input to terminate the program
            // -> seems it does not work in open5gs env. Thus, pause() is used instead.
            pause();
        }
    } else {
        ogs_error("Error starting OAM Rest-API server");
    }

    ulfius_stop_framework(&instance);
    ulfius_clean_instance(&instance);

}
//-----------------------------------------
// Eureka - ktsubouc: end
//-----------------------------------------


static ogs_thread_t *thread;
static void smf_main(void *data);

static int initialized = 0;

int smf_initialize(void)
{
    int rv;

#define APP_NAME "smf"
    rv = ogs_app_parse_local_conf(APP_NAME);
    if (rv != OGS_OK) return rv;

    smf_metrics_init();

    ogs_gtp_context_init(ogs_app()->pool.nf * OGS_MAX_NUM_OF_GTPU_RESOURCE);
    ogs_pfcp_context_init();
    ogs_sbi_context_init(OpenAPI_nf_type_SMF);

    smf_context_init();

    rv = ogs_gtp_xact_init();
    if (rv != OGS_OK) return rv;

    rv = ogs_pfcp_xact_init();
    if (rv != OGS_OK) return rv;

    rv = ogs_log_config_domain(
            ogs_app()->logger.domain, ogs_app()->logger.level);
    if (rv != OGS_OK) return rv;

    rv = ogs_gtp_context_parse_config(APP_NAME, "upf");
    if (rv != OGS_OK) return rv;

    rv = ogs_pfcp_context_parse_config(APP_NAME, "upf");
    if (rv != OGS_OK) return rv;

    rv = ogs_sbi_context_parse_config(APP_NAME, "nrf", "scp");
    if (rv != OGS_OK) return rv;

    rv = ogs_metrics_context_parse_config(APP_NAME);
    if (rv != OGS_OK) return rv;

    rv = smf_context_parse_config();
    if (rv != OGS_OK) return rv;

    rv = ogs_pfcp_ue_pool_generate();
    if (rv != OGS_OK) return rv;

    ogs_metrics_context_open(ogs_metrics_self());

    rv = smf_fd_init();
    if (rv != 0) return OGS_ERROR;

    rv = smf_gtp_open();
    if (rv != 0) return OGS_ERROR;

    rv = smf_pfcp_open();
    if (rv != 0) return OGS_ERROR;

    rv = smf_sbi_open();
    if (rv != 0) return OGS_ERROR;

    thread = ogs_thread_create(smf_main, NULL);
    if (!thread) return OGS_ERROR;

    initialized = 1;

    //-----------------------------------------------
    // Eureka - ktsubouc
    //-----------------------------------------------
    thread = ogs_thread_create(rest_handler, NULL);
    if (!thread) return OGS_ERROR;
    // Eureka - ktsubouc - end

    return OGS_OK;
}

static ogs_timer_t *t_termination_holding = NULL;

static void event_termination(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    /* Sending NF Instance De-registeration to NRF */
    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance)
        ogs_sbi_nf_fsm_fini(nf_instance);

    /* Starting holding timer */
    t_termination_holding = ogs_timer_add(ogs_app()->timer_mgr, NULL, NULL);
    ogs_assert(t_termination_holding);
#define TERMINATION_HOLDING_TIME ogs_time_from_msec(300)
    ogs_timer_start(t_termination_holding, TERMINATION_HOLDING_TIME);

    /* Sending termination event to the queue */
    ogs_queue_term(ogs_app()->queue);
    ogs_pollset_notify(ogs_app()->pollset);
}

void smf_terminate(void)
{
    if (!initialized) return;

    /* Daemon terminating */
    event_termination();
    ogs_thread_destroy(thread);
    ogs_timer_delete(t_termination_holding);

    smf_gtp_close();
    smf_pfcp_close();
    smf_sbi_close();

    ogs_metrics_context_close(ogs_metrics_self());

    smf_fd_final();

    smf_context_final();

    ogs_pfcp_context_final();
    ogs_sbi_context_final();
    ogs_gtp_context_final();

    ogs_pfcp_xact_final();
    ogs_gtp_xact_final();

    smf_metrics_final();
}

static void smf_main(void *data)
{
    ogs_fsm_t smf_sm;
    int rv;

    ogs_fsm_init(&smf_sm, smf_state_initial, smf_state_final, 0);

    for ( ;; ) {
        ogs_pollset_poll(ogs_app()->pollset,
                ogs_timer_mgr_next(ogs_app()->timer_mgr));

        /*
         * After ogs_pollset_poll(), ogs_timer_mgr_expire() must be called.
         *
         * The reason is why ogs_timer_mgr_next() can get the corrent value
         * when ogs_timer_stop() is called internally in ogs_timer_mgr_expire().
         *
         * You should not use event-queue before ogs_timer_mgr_expire().
         * In this case, ogs_timer_mgr_expire() does not work
         * because 'if rv == OGS_DONE' statement is exiting and
         * not calling ogs_timer_mgr_expire().
         */
        ogs_timer_mgr_expire(ogs_app()->timer_mgr);

        for ( ;; ) {
            smf_event_t *e = NULL;

            rv = ogs_queue_trypop(ogs_app()->queue, (void**)&e);
            ogs_assert(rv != OGS_ERROR);

            if (rv == OGS_DONE)
                goto done;

            if (rv == OGS_RETRY)
                break;

            ogs_assert(e);
            ogs_fsm_dispatch(&smf_sm, e);
            ogs_event_free(e);
        }
    }
done:

    ogs_fsm_fini(&smf_sm, 0);
}
