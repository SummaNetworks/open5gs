#ifndef MME_METRICS_H
#define MME_METRICS_H

#include "ogs-metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GLOBAL */
typedef enum mme_metric_type_global_s {
    /* Existing gauges */
    MME_METR_GLOB_GAUGE_ENB_UE,
    MME_METR_GLOB_GAUGE_MME_SESS,
    MME_METR_GLOB_GAUGE_ENB,
    /*
     * Bearer occupancy, and the capacities the two are measured against.
     *
     * mme_sess_pool and mme_bearer_pool are shared by every UE, so running
     * either dry stops new sessions node-wide and does not recover until the
     * leaked ones are freed. mme_session already tracked the first; this adds
     * the second, which nothing observed before - a dedicated-bearer leak
     * moves mme_bearer without moving mme_session at all.
     *
     * The capacities are exported so an alert can be written as a ratio and
     * survive a change to max.ue, which is where both pool sizes come from.
     */
    MME_METR_GLOB_GAUGE_MME_BEARER,
    MME_METR_GLOB_GAUGE_MME_SESS_CAPACITY,
    MME_METR_GLOB_GAUGE_MME_BEARER_CAPACITY,
    /* Phase 4 / Phase 4.1 hotfix instrumentation (Open5GS implementation-
     * specific counters; prefixed open5gs_mme_* in the exposition). */
    MME_METR_GLOB_CTR_HO_TYPE_RESCUE_FIRED,
    MME_METR_GLOB_CTR_HO_TYPE_RESCUE_SKIPPED,
    MME_METR_GLOB_CTR_HI_RESET,
    MME_METR_GLOB_CTR_DBRESP_PROACTIVE_FLUSH,
    MME_METR_GLOB_CTR_E_RAB_SETUP_RELEASE_RACE,
    _MME_METR_GLOB_MAX,
} mme_metric_type_global_t;
extern ogs_metrics_inst_t *mme_metrics_inst_global[_MME_METR_GLOB_MAX];

int mme_metrics_init_inst_global(void);
int mme_metrics_free_inst_global(void);

static inline void mme_metrics_inst_global_set(mme_metric_type_global_t t, int val)
{ ogs_metrics_inst_set(mme_metrics_inst_global[t], val); }
static inline void mme_metrics_inst_global_add(mme_metric_type_global_t t, int val)
{ ogs_metrics_inst_add(mme_metrics_inst_global[t], val); }
static inline void mme_metrics_inst_global_inc(mme_metric_type_global_t t)
{ ogs_metrics_inst_inc(mme_metrics_inst_global[t]); }
static inline void mme_metrics_inst_global_dec(mme_metric_type_global_t t)
{ ogs_metrics_inst_dec(mme_metrics_inst_global[t]); }

/* BY CAUSE (scope + cause bucket): GTPv2 response Cause distribution. */
typedef enum mme_metric_type_by_cause_s {
    MME_METR_BY_CAUSE_GTPV2_RESPONSE,
    _MME_METR_BY_CAUSE_MAX,
} mme_metric_type_by_cause_t;
void mme_metrics_inst_by_cause_add(
        const char *scope, const char *cause,
        mme_metric_type_by_cause_t t, int val);
static inline void mme_metrics_inst_by_cause_inc(
        const char *scope, const char *cause,
        mme_metric_type_by_cause_t t)
{ mme_metrics_inst_by_cause_add(scope, cause, t, 1); }

/* BY DIRECTION (direction label): handover attempt/success counters. */
typedef enum mme_metric_type_by_direction_s {
    MME_METR_BY_DIRECTION_HO_ATTEMPT,
    MME_METR_BY_DIRECTION_HO_SUCCESS,
    _MME_METR_BY_DIRECTION_MAX,
} mme_metric_type_by_direction_t;
void mme_metrics_inst_by_direction_add(
        const char *direction,
        mme_metric_type_by_direction_t t, int val);
static inline void mme_metrics_inst_by_direction_inc(
        const char *direction, mme_metric_type_by_direction_t t)
{ mme_metrics_inst_by_direction_add(direction, t, 1); }

/* BY REQUEST_TYPE (request_type label): PDN connectivity request bucket. */
typedef enum mme_metric_type_by_request_type_s {
    MME_METR_BY_REQUEST_TYPE_PDN_CONN,
    _MME_METR_BY_REQUEST_TYPE_MAX,
} mme_metric_type_by_request_type_t;
void mme_metrics_inst_by_request_type_add(
        const char *request_type,
        mme_metric_type_by_request_type_t t, int val);
static inline void mme_metrics_inst_by_request_type_inc(
        const char *request_type, mme_metric_type_by_request_type_t t)
{ mme_metrics_inst_by_request_type_add(request_type, t, 1); }

/* BY REQUEST_TYPE + CAUSE (2-label): PDN connectivity reject. */
typedef enum mme_metric_type_by_req_type_cause_s {
    MME_METR_BY_REQ_TYPE_CAUSE_PDN_CONN_REJECT,
    _MME_METR_BY_REQ_TYPE_CAUSE_MAX,
} mme_metric_type_by_req_type_cause_t;
void mme_metrics_inst_by_req_type_cause_add(
        const char *request_type, const char *esm_cause,
        mme_metric_type_by_req_type_cause_t t, int val);
static inline void mme_metrics_inst_by_req_type_cause_inc(
        const char *request_type, const char *esm_cause,
        mme_metric_type_by_req_type_cause_t t)
{ mme_metrics_inst_by_req_type_cause_add(request_type, esm_cause, t, 1); }

/* BY RESULT + CAUSE (2-label): Attach accept/reject + emm_cause. */
typedef enum mme_metric_type_by_result_cause_s {
    MME_METR_BY_RESULT_CAUSE_ATTACH,
    _MME_METR_BY_RESULT_CAUSE_MAX,
} mme_metric_type_by_result_cause_t;
void mme_metrics_inst_by_result_cause_add(
        const char *result, const char *emm_cause,
        mme_metric_type_by_result_cause_t t, int val);
static inline void mme_metrics_inst_by_result_cause_inc(
        const char *result, const char *emm_cause,
        mme_metric_type_by_result_cause_t t)
{ mme_metrics_inst_by_result_cause_add(result, emm_cause, t, 1); }

/* BY REASON + OUTCOME (2-label): ICS deletion_in_progress filter. */
typedef enum mme_metric_type_by_reason_outcome_s {
    MME_METR_BY_REASON_OUTCOME_ICS_FILTER,
    _MME_METR_BY_REASON_OUTCOME_MAX,
} mme_metric_type_by_reason_outcome_t;
void mme_metrics_inst_by_reason_outcome_add(
        const char *reason, const char *outcome,
        mme_metric_type_by_reason_outcome_t t, int val);
static inline void mme_metrics_inst_by_reason_outcome_inc(
        const char *reason, const char *outcome,
        mme_metric_type_by_reason_outcome_t t)
{ mme_metrics_inst_by_reason_outcome_add(reason, outcome, t, 1); }

/* HISTOGRAM (direction label): handover duration in milliseconds. */
typedef enum mme_metric_type_histogram_s {
    MME_METR_HISTOGRAM_HO_DURATION,
    _MME_METR_HISTOGRAM_MAX,
} mme_metric_type_histogram_t;
void mme_metrics_inst_histogram_observe(
        const char *direction,
        mme_metric_type_histogram_t t, int milliseconds);

/* Tier 1 KPI extensions (next batch) — Paging / SR / UEContextRelease
 * three-point observation for stealth radio-link failure detection. */

/* BY PAGING_CAUSE (cause label): TS 32.426 MM.PagingEpsAtt analogue. */
typedef enum mme_metric_type_by_paging_cause_s {
    MME_METR_BY_PAGING_CAUSE_ATTEMPT,
    _MME_METR_BY_PAGING_CAUSE_MAX,
} mme_metric_type_by_paging_cause_t;
void mme_metrics_inst_by_paging_cause_add(
        const char *cause,
        mme_metric_type_by_paging_cause_t t, int val);
static inline void mme_metrics_inst_by_paging_cause_inc(
        const char *cause, mme_metric_type_by_paging_cause_t t)
{ mme_metrics_inst_by_paging_cause_add(cause, t, 1); }

/* New GLOBAL counters for Tier 1 (no labels). */
typedef enum mme_metric_type_global_ext_s {
    MME_METR_GLOB_CTR_PAGING_SUCCESS,
    MME_METR_GLOB_CTR_PAGING_TIMEOUT,
    MME_METR_GLOB_CTR_SUSPECTED_STALE_CONNECTED, /* reserved Tier 3 */
    _MME_METR_GLOB_EXT_MAX,
} mme_metric_type_global_ext_t;
extern ogs_metrics_inst_t *mme_metrics_inst_global_ext[_MME_METR_GLOB_EXT_MAX];
int mme_metrics_init_inst_global_ext(void);
int mme_metrics_free_inst_global_ext(void);
static inline void mme_metrics_inst_global_ext_inc(
        mme_metric_type_global_ext_t t)
{ ogs_metrics_inst_inc(mme_metrics_inst_global_ext[t]); }

/* BY RESULT+CAUSE for Service Request and ICS (2-label). */
typedef enum mme_metric_type_by_result_cause_ext_s {
    MME_METR_BY_RESULT_CAUSE_EXT_SERVICE_REQUEST,
    MME_METR_BY_RESULT_CAUSE_EXT_ICS,
    _MME_METR_BY_RESULT_CAUSE_EXT_MAX,
} mme_metric_type_by_result_cause_ext_t;
void mme_metrics_inst_by_result_cause_ext_add(
        const char *result, const char *cause,
        mme_metric_type_by_result_cause_ext_t t, int val);
static inline void mme_metrics_inst_by_result_cause_ext_inc(
        const char *result, const char *cause,
        mme_metric_type_by_result_cause_ext_t t)
{ mme_metrics_inst_by_result_cause_ext_add(result, cause, t, 1); }

/* BY CAUSE_GROUP+CAUSE for UEContextRelease (2-label). */
typedef enum mme_metric_type_by_cause_group_s {
    MME_METR_BY_CAUSE_GROUP_UE_CONTEXT_RELEASE,
    _MME_METR_BY_CAUSE_GROUP_MAX,
} mme_metric_type_by_cause_group_t;
void mme_metrics_inst_by_cause_group_add(
        const char *cause_group, const char *cause,
        mme_metric_type_by_cause_group_t t, int val);
static inline void mme_metrics_inst_by_cause_group_inc(
        const char *cause_group, const char *cause,
        mme_metric_type_by_cause_group_t t)
{ mme_metrics_inst_by_cause_group_add(cause_group, cause, t, 1); }

/* BY ECM STATE (state label, gauge). */
typedef enum mme_metric_type_by_state_s {
    MME_METR_BY_STATE_UES_IN_STATE,
    _MME_METR_BY_STATE_MAX,
} mme_metric_type_by_state_t;
void mme_metrics_inst_by_state_add(
        const char *state, mme_metric_type_by_state_t t, int val);
static inline void mme_metrics_inst_by_state_inc(
        const char *state, mme_metric_type_by_state_t t)
{ mme_metrics_inst_by_state_add(state, t, 1); }
static inline void mme_metrics_inst_by_state_dec(
        const char *state, mme_metric_type_by_state_t t)
{ mme_metrics_inst_by_state_add(state, t, -1); }

/* Histograms (unlabeled, seconds): connected-state duration and
 * paging-response time. */
typedef enum mme_metric_type_histogram_seconds_s {
    MME_METR_HIST_CONNECTED_STATE_DURATION,
    MME_METR_HIST_PAGING_RESPONSE_DURATION,
    _MME_METR_HIST_SECONDS_MAX,
} mme_metric_type_histogram_seconds_t;
void mme_metrics_inst_histogram_seconds_observe(
        mme_metric_type_histogram_seconds_t t, int seconds);

/* Cause-to-bucket helpers. The bucket strings are stable enum-like
 * values; unknown causes always fall through to "other" so silent
 * cardinality growth cannot happen. */
const char *mme_gtpv2_cause_bucket(uint8_t cause);
const char *mme_esm_cause_bucket(uint8_t cause);
const char *mme_emm_cause_bucket(uint8_t cause);
const char *mme_request_type_bucket(uint8_t request_type);
/* S1AP UEContextRelease cause -> (cause_group, cause). Both pointers
 * are mandatory (the helper writes through them). */
void mme_s1ap_release_cause_bucket(
        int cause_present, int cause_value,
        const char **cause_group_out, const char **cause_out);

void mme_metrics_init(void);
void mme_metrics_final(void);

#ifdef __cplusplus
}
#endif

#endif /* MME_METRICS_H */
