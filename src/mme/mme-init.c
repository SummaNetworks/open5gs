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

#include "ogs-sctp.h"
#include "ogs-gtp.h"

#include "mme-context.h"
#include "mme-sm.h"
#include "mme-event.h"
#include "mme-timer.h"

#include "mme-fd-path.h"
#include "s1ap-path.h"
#include "sgsap-path.h"
#include "mme-gtp-path.h"
#include "metrics.h"

//-----------------------------------------
// Eureka - ktsubouc: start
//-----------------------------------------
#include <ulfius.h>
#include <jansson.h>

#define REST_PORT 8880

int callback_get_enb(const struct _u_request *request, struct _u_response *response, void *user_data);
int callback_get_enb_by_id(const struct _u_request *request, struct _u_response *response, void *user_data);
int callback_get_ue(const struct _u_request *request, struct _u_response *response, void *user_data);
int callback_get_ue_by_id(const struct _u_request *request, struct _u_response *response, void *user_data);
int callback_get_log_level(const struct _u_request *request, struct _u_response *response, void *user_data);
int callback_set_log_level_fatal(const struct _u_request *request, struct _u_response *response, void *user_data);

int callback_get_enb(const struct _u_request *request, struct _u_response *response, void *user_data)
{
	mme_enb_t *mme_enb = NULL;

	json_t *json_response = json_array();

	ogs_list_for_each(&mme_self()->enb_list, mme_enb) {
        if (mme_enb->state.s1_setup_success) {
            json_t *jobj = json_object();
            char buf[OGS_ADDRSTRLEN];

            json_object_set_new(jobj, "enb_id", json_integer(mme_enb->enb_id));
            if (mme_enb->sctp.addr && mme_enb->sctp.addr->ogs_sa_family != 0) {
                json_object_set_new(jobj, "address", json_string(OGS_ADDR(mme_enb->sctp.addr, buf)));
            } else {
                json_object_set_new(jobj, "address", json_string("unknown"));
            }
            // assume number of tac is 1. needs to be modified as necessary.
            json_object_set_new(jobj, "tac", json_integer(mme_enb->supported_ta_list[0].tac));

            json_array_append_new(json_response, jobj);
        }
	}

	ulfius_set_json_body_response(response, 200, json_response);
	json_decref(json_response);

	return U_CALLBACK_CONTINUE;
}

int callback_get_enb_by_id(const struct _u_request *request, struct _u_response *response, void *user_data)
{
    mme_enb_t *mme_enb = NULL;
    enb_ue_t *enb_ue = NULL;
	char buf[OGS_ADDRSTRLEN];

    json_t *json_response = json_object();
    json_t *imsi_array = json_array();

    uint32_t enb_id = atoi(u_map_get(request->map_url, "enb_id"));

    mme_enb = mme_enb_find_by_enb_id(enb_id);

    if (mme_enb == NULL) {
        ulfius_set_json_body_response(response, 200, json_response);
        json_decref(json_response);
    	return U_CALLBACK_CONTINUE;
    }

    json_object_set_new(json_response, "enb_id", json_integer(enb_id));
    if (mme_enb->sctp.addr && mme_enb->sctp.addr->ogs_sa_family != 0) {
        json_object_set_new(json_response, "address", json_string(OGS_ADDR(mme_enb->sctp.addr, buf)));
    } else {
        json_object_set_new(json_response, "address", json_string("unknown"));
    }
    json_object_set_new(json_response, "tac", json_integer(mme_enb->supported_ta_list[0].tac));

	ogs_list_for_each(&mme_enb->enb_ue_list, enb_ue) {
        mme_ue_t *mme_ue = NULL;
        
        mme_ue = mme_ue_find_by_id(enb_ue->mme_ue_id);
        json_array_append_new(imsi_array, json_string(mme_ue->imsi_bcd));
	}
    json_object_set_new(json_response, "imsi", imsi_array);

	ulfius_set_json_body_response(response, 200, json_response);
	json_decref(json_response);
	
	return U_CALLBACK_CONTINUE;
}


// return imsi list
int callback_get_ue(const struct _u_request *request, struct _u_response *response, void *user_data)
{
	mme_ue_t *mme_ue = NULL;

	json_t *json_response = json_array();

	ogs_list_for_each(&mme_self()->mme_ue_list, mme_ue) {
        json_array_append_new(json_response, json_string(mme_ue->imsi_bcd));
	}

	ulfius_set_json_body_response(response, 200, json_response);
	json_decref(json_response);

	return U_CALLBACK_CONTINUE;
}

int callback_get_ue_by_id(const struct _u_request *request, struct _u_response *response, void *user_data)
{
	mme_ue_t *mme_ue;

	json_t *json_response = json_object();

    const char *imsi_bcd = u_map_get(request->map_url, "imsi");

    mme_ue = mme_ue_find_by_imsi_bcd(imsi_bcd);
    if (mme_ue != NULL) {
        json_object_set_new(json_response, "imsi", json_string(mme_ue->imsi_bcd));
        json_object_set_new(json_response, "imeisv", json_string(mme_ue->imeisv_bcd));
        json_object_set_new(json_response, "msisdn", json_string(mme_ue->msisdn_bcd));
    }

	ulfius_set_json_body_response(response, 200, json_response);
	json_decref(json_response);

	return U_CALLBACK_CONTINUE;
}

int callback_get_log_level(const struct _u_request *request, struct _u_response *response, void *user_data)
{
	json_t *json_response = json_array();

    ogs_log_level_e log_level_ogs_sctp_domain = ogs_log_get_domain_level(__ogs_sctp_domain);
    ogs_log_level_e log_level_ogs_s1ap_domain = ogs_log_get_domain_level(__ogs_s1ap_domain);
    ogs_log_level_e log_level_ogs_nas_domain = ogs_log_get_domain_level(__ogs_nas_domain);
    ogs_log_level_e log_level_ogs_diam_domain = ogs_log_get_domain_level(__ogs_diam_domain);
    ogs_log_level_e log_level_mme_log_domain = ogs_log_get_domain_level(__mme_log_domain);
    ogs_log_level_e log_level_emm_log_domain = ogs_log_get_domain_level(__emm_log_domain);
    ogs_log_level_e log_level_esm_log_domain = ogs_log_get_domain_level(__esm_log_domain);

    json_array_append_new(json_response, json_pack("{s:i}", "ogs_sctp_domain", log_level_ogs_sctp_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "ogs_s1ap_domain", log_level_ogs_s1ap_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "ogs_nas_domain", log_level_ogs_nas_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "ogs_diam_domain", log_level_ogs_diam_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "mme_log_domain", log_level_mme_log_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "emm_log_domain", log_level_emm_log_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "esm_log_domain", log_level_esm_log_domain));

	ulfius_set_json_body_response(response, 200, json_response);

	json_decref(json_response);

	return U_CALLBACK_CONTINUE;
}

int callback_set_log_level_fatal(const struct _u_request *request, struct _u_response *response, void *user_data)
{
	json_t *json_response = json_array();

    ogs_log_level_e log_level_ogs_sctp_domain = ogs_log_get_domain_level(__ogs_sctp_domain);
    ogs_log_level_e log_level_ogs_s1ap_domain = ogs_log_get_domain_level(__ogs_s1ap_domain);
    ogs_log_level_e log_level_ogs_nas_domain = ogs_log_get_domain_level(__ogs_nas_domain);
    ogs_log_level_e log_level_ogs_diam_domain = ogs_log_get_domain_level(__ogs_diam_domain);
    ogs_log_level_e log_level_mme_log_domain = ogs_log_get_domain_level(__mme_log_domain);
    ogs_log_level_e log_level_emm_log_domain = ogs_log_get_domain_level(__emm_log_domain);
    ogs_log_level_e log_level_esm_log_domain = ogs_log_get_domain_level(__esm_log_domain);
    
    ogs_log_level_e log_level_new = OGS_LOG_FATAL;

    /*    
    printf("%d", log_level_ogs_sctp_domain);
    printf("%d", log_level_ogs_s1ap_domain);
    printf("%d", log_level_ogs_nas_domain);
    printf("%d", log_level_ogs_diam_domain);
    printf("%d", log_level_mme_log_domain);
    printf("%d", log_level_emm_log_domain);
    printf("%d", log_level_esm_log_domain);
    fflush(stdout);
    */

    ogs_log_set_domain_level(__ogs_sctp_domain, OGS_LOG_FATAL);
    ogs_log_set_domain_level(__ogs_s1ap_domain, OGS_LOG_FATAL);
    ogs_log_set_domain_level(__ogs_nas_domain, OGS_LOG_FATAL);
    ogs_log_set_domain_level(__ogs_diam_domain, OGS_LOG_FATAL);
    ogs_log_set_domain_level(__mme_log_domain, OGS_LOG_FATAL);
    ogs_log_set_domain_level(__emm_log_domain, OGS_LOG_FATAL);
    ogs_log_set_domain_level(__esm_log_domain, OGS_LOG_FATAL);
    
    log_level_ogs_sctp_domain = log_level_new;
    log_level_ogs_s1ap_domain = log_level_new;
    log_level_ogs_nas_domain = log_level_new;
    log_level_ogs_diam_domain = log_level_new;
    log_level_mme_log_domain = log_level_new;
    log_level_emm_log_domain = log_level_new;
    log_level_esm_log_domain = log_level_new;

    json_array_append_new(json_response, json_pack("{s:i}", "ogs_sctp_domain", log_level_ogs_sctp_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "ogs_s1ap_domain", log_level_ogs_s1ap_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "ogs_nas_domain", log_level_ogs_nas_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "ogs_diam_domain", log_level_ogs_diam_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "mme_log_domain", log_level_mme_log_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "emm_log_domain", log_level_emm_log_domain));
    json_array_append_new(json_response, json_pack("{s:i}", "esm_log_domain", log_level_esm_log_domain));

	ulfius_set_json_body_response(response, 200, json_response);

	json_decref(json_response);

	return U_CALLBACK_CONTINUE;
}

static int callback_get_tenant_control(const struct _u_request *request,
                                       struct _u_response *response, 
                                       void *user_data)
{
    json_t *json_response = mme_tenant_control_to_json();
    
    ulfius_set_json_body_response(response, 200, json_response);
    json_decref(json_response);
    
    return U_CALLBACK_CONTINUE;
}

static int callback_reload_tenant_control(const struct _u_request *request,
                                          struct _u_response *response, 
                                          void *user_data)
{
    json_t *json_response = mme_reload_tenant_control_with_diff();
    
    const char *status = json_string_value(json_object_get(json_response, "status"));
    int http_code = (status && strcmp(status, "success") == 0) ? 200 : 500;
    
    ulfius_set_json_body_response(response, http_code, json_response);
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
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/enb", 0, &callback_get_enb, NULL);
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/enb/:enb_id", 0, &callback_get_enb_by_id, NULL);
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/ue", 0, &callback_get_ue, NULL);
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/ue/:imsi", 0, &callback_get_ue_by_id, NULL);
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/log_level", 0, &callback_get_log_level, NULL);
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/log_level_fatal", 0, &callback_set_log_level_fatal, NULL);
    
    // Tenant Control Endpoints
    ulfius_add_endpoint_by_val(&instance, "GET", NULL, "/tenant_control", 0, &callback_get_tenant_control, NULL);
    ulfius_add_endpoint_by_val(&instance, "POST", NULL, "/tenant_control/reload", 0, &callback_reload_tenant_control, NULL);

    if (ulfius_start_framework(&instance) == U_OK) {
        ogs_info("Starting OAM Rest-API server on port %d\n", instance.port);
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
static void mme_main(void *data);

static int initialized = 0;

int mme_initialize(void)
{
    int rv;

#define APP_NAME "mme"
    rv = ogs_app_parse_local_conf(APP_NAME);
    if (rv != OGS_OK) return rv;

    mme_metrics_init();

    ogs_gtp_context_init(OGS_MAX_NUM_OF_GTPU_RESOURCE);
    mme_context_init();

    rv = ogs_gtp_xact_init();
    if (rv != OGS_OK) return rv;

    rv = ogs_log_config_domain(
            ogs_app()->logger.domain, ogs_app()->logger.level);
    if (rv != OGS_OK) return rv;

    rv = ogs_gtp_context_parse_config(APP_NAME, "sgwc");
    if (rv != OGS_OK) return rv;

    rv = ogs_metrics_context_parse_config(APP_NAME);
    if (rv != OGS_OK) return rv;

    rv = mme_context_parse_config();
    if (rv != OGS_OK) return rv;

    /* Initialize timer configuration from parsed YAML */
    mme_timer_cfg_init();

    ogs_metrics_context_open(ogs_metrics_self());

    rv = mme_fd_init();
    if (rv != OGS_OK) return OGS_ERROR;

    rv = mme_gtp_open();
    if (rv != OGS_OK) return OGS_ERROR;

    rv = sgsap_open();
    if (rv != OGS_OK) return OGS_ERROR;

    rv = s1ap_open();
    if (rv != OGS_OK) return OGS_ERROR;

    thread = ogs_thread_create(mme_main, NULL);
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

void mme_terminate(void)
{
    if (!initialized) return;

    mme_event_term();

    ogs_thread_destroy(thread);

    mme_gtp_close();
    sgsap_close();
    s1ap_close();

    ogs_metrics_context_close(ogs_metrics_self());

    mme_fd_final();

    mme_context_final();

    ogs_gtp_context_final();

    ogs_gtp_xact_final();

    mme_metrics_final();
}

static void mme_main(void *data)
{
    ogs_fsm_t mme_sm;
    int rv;

    ogs_fsm_init(&mme_sm, mme_state_initial, mme_state_final, 0);

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
            mme_event_t *e = NULL;

            rv = ogs_queue_trypop(ogs_app()->queue, (void**)&e);
            ogs_assert(rv != OGS_ERROR);

            if (rv == OGS_DONE)
                goto done;

            if (rv == OGS_RETRY)
                break;

            ogs_assert(e);
            ogs_fsm_dispatch(&mme_sm, e);
            mme_event_free(e);
        }
    }
done:

    ogs_fsm_fini(&mme_sm, 0);
}
