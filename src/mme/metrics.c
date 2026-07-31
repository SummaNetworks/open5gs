#include "ogs-app.h"
#include "mme-context.h"
#include "ogs-gtp.h"
#include "ogs-nas-eps.h"

#include "metrics.h"

typedef struct mme_metrics_spec_def_s {
    unsigned int type;
    const char *name;
    const char *description;
    int initial_val;
    unsigned int num_labels;
    const char **labels;
    ogs_metrics_histogram_params_t histogram_params;
} mme_metrics_spec_def_t;

/* Helper generic functions: */
static int mme_metrics_init_inst(ogs_metrics_inst_t **inst, ogs_metrics_spec_t **specs,
        unsigned int len, unsigned int num_labels, const char **labels)
{
    unsigned int i;
    for (i = 0; i < len; i++)
        inst[i] = ogs_metrics_inst_new(specs[i], num_labels, labels);
    return OGS_OK;
}

static int mme_metrics_free_inst(ogs_metrics_inst_t **inst,
        unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++)
        ogs_metrics_inst_free(inst[i]);
    memset(inst, 0, sizeof(inst[0]) * len);
    return OGS_OK;
}

static int mme_metrics_init_spec(ogs_metrics_context_t *ctx,
        ogs_metrics_spec_t **dst, mme_metrics_spec_def_t *src, unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++) {
        dst[i] = ogs_metrics_spec_new(ctx, src[i].type,
                src[i].name, src[i].description,
                src[i].initial_val, src[i].num_labels, src[i].labels,
                &src[i].histogram_params);
    }
    return OGS_OK;
}

/* GLOBAL */
ogs_metrics_spec_t *mme_metrics_spec_global[_MME_METR_GLOB_MAX];
ogs_metrics_inst_t *mme_metrics_inst_global[_MME_METR_GLOB_MAX];
mme_metrics_spec_def_t mme_metrics_spec_def_global[_MME_METR_GLOB_MAX] = {
/* Global Gauges (existing) */
[MME_METR_GLOB_GAUGE_ENB_UE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "enb_ue",
    .description = "Number of UEs connected to eNodeBs",
},
[MME_METR_GLOB_GAUGE_MME_SESS] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "mme_session",
    .description = "MME Sessions",
},
[MME_METR_GLOB_GAUGE_ENB] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "enb",
    .description = "eNodeBs",
},
/* Open5GS implementation-specific counters (Phase 4 / Phase 4.1 hotfix). */
[MME_METR_GLOB_CTR_HO_TYPE_RESCUE_FIRED] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_ho_type_rescue_fired_total",
    .description = "Phase 4 PR6-B HO-type reject relaxation fired "
            "(TS 24.301 6.5.1.6(a)(2) rescue path)",
},
[MME_METR_GLOB_CTR_HO_TYPE_RESCUE_SKIPPED] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_ho_type_rescue_skipped_total",
    .description = "Phase 4.1 hotfix PR6-B multi-retry guard skipped a "
            "second rescue while teardown was still in flight",
},
[MME_METR_GLOB_CTR_HI_RESET] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_hi_reset_total",
    .description = "Phase 4.1 hotfix MBR Handover Indication reset to "
            "INITIAL after a successful Modify Bearer Response "
            "(per-session reset count)",
},
[MME_METR_GLOB_CTR_DBRESP_PROACTIVE_FLUSH] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_dbresp_proactive_flush_total",
    .description = "Phase 4 PR3b Delete Bearer Response proactive flush "
            "on idle-bound action",
},
[MME_METR_GLOB_CTR_E_RAB_SETUP_RELEASE_RACE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_e_rab_setup_release_race_total",
    .description = "Phase 4 PR5 E-RAB Setup Response received while UE "
            "Context Release was already in progress",
},
};

int mme_metrics_init_inst_global(void)
{
    return mme_metrics_init_inst(mme_metrics_inst_global, mme_metrics_spec_global,
                _MME_METR_GLOB_MAX, 0, NULL);
}
int mme_metrics_free_inst_global(void)
{
    return mme_metrics_free_inst(mme_metrics_inst_global, _MME_METR_GLOB_MAX);
}

/* BY CAUSE (scope, cause) */
static const char *labels_scope_cause[] = { "scope", "cause" };
ogs_metrics_spec_t *mme_metrics_spec_by_cause[_MME_METR_BY_CAUSE_MAX];
static ogs_hash_t *metrics_hash_by_cause = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_cause[_MME_METR_BY_CAUSE_MAX] = {
[MME_METR_BY_CAUSE_GTPV2_RESPONSE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_gtpv2_response_cause_total",
    .description = "GTPv2 response Cause distribution (bucketed)",
    .num_labels = OGS_ARRAY_SIZE(labels_scope_cause),
    .labels = labels_scope_cause,
},
};

typedef struct mme_metric_key_by_cause_s {
    char scope[16];
    char cause[32];
    mme_metric_type_by_cause_t t;
} mme_metric_key_by_cause_t;

void mme_metrics_inst_by_cause_add(const char *scope, const char *cause,
        mme_metric_type_by_cause_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_cause_t *key;

    if (!scope) scope = "other";
    if (!cause) cause = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->scope, scope, sizeof(key->scope));
    ogs_cpystrn(key->cause, cause, sizeof(key->cause));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_cause, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_cause[t],
                mme_metrics_spec_def_by_cause->num_labels,
                (const char *[]){ key->scope, key->cause });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_cause, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY DIRECTION (direction) */
static const char *labels_direction[] = { "direction" };
ogs_metrics_spec_t *mme_metrics_spec_by_direction[_MME_METR_BY_DIRECTION_MAX];
static ogs_hash_t *metrics_hash_by_direction = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_direction[_MME_METR_BY_DIRECTION_MAX] = {
[MME_METR_BY_DIRECTION_HO_ATTEMPT] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_handover_attempt_total",
    .description = "Handover attempts observed by the MME "
            "(per TS 32.455 KPI family)",
    .num_labels = OGS_ARRAY_SIZE(labels_direction),
    .labels = labels_direction,
},
[MME_METR_BY_DIRECTION_HO_SUCCESS] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_handover_success_total",
    .description = "Handover control-plane successes "
            "(MBR Cause=ACCEPTED with Handover Indication)",
    .num_labels = OGS_ARRAY_SIZE(labels_direction),
    .labels = labels_direction,
},
};

typedef struct mme_metric_key_by_direction_s {
    char direction[24];
    mme_metric_type_by_direction_t t;
} mme_metric_key_by_direction_t;

void mme_metrics_inst_by_direction_add(const char *direction,
        mme_metric_type_by_direction_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_direction_t *key;

    if (!direction) direction = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->direction, direction, sizeof(key->direction));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_direction, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_direction[t],
                mme_metrics_spec_def_by_direction->num_labels,
                (const char *[]){ key->direction });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_direction, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY REQUEST_TYPE (request_type) */
static const char *labels_request_type[] = { "request_type" };
ogs_metrics_spec_t *mme_metrics_spec_by_request_type[_MME_METR_BY_REQUEST_TYPE_MAX];
static ogs_hash_t *metrics_hash_by_request_type = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_request_type[_MME_METR_BY_REQUEST_TYPE_MAX] = {
[MME_METR_BY_REQUEST_TYPE_PDN_CONN] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_pdn_connectivity_total",
    .description = "PDN Connectivity Request received, "
            "bucketed by NAS request_type",
    .num_labels = OGS_ARRAY_SIZE(labels_request_type),
    .labels = labels_request_type,
},
};

typedef struct mme_metric_key_by_request_type_s {
    char request_type[16];
    mme_metric_type_by_request_type_t t;
} mme_metric_key_by_request_type_t;

void mme_metrics_inst_by_request_type_add(const char *request_type,
        mme_metric_type_by_request_type_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_request_type_t *key;

    if (!request_type) request_type = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->request_type, request_type, sizeof(key->request_type));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_request_type, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_request_type[t],
                mme_metrics_spec_def_by_request_type->num_labels,
                (const char *[]){ key->request_type });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_request_type, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY REQUEST_TYPE + CAUSE (2-label) */
static const char *labels_req_type_cause[] = { "request_type", "esm_cause" };
ogs_metrics_spec_t *mme_metrics_spec_by_req_type_cause[_MME_METR_BY_REQ_TYPE_CAUSE_MAX];
static ogs_hash_t *metrics_hash_by_req_type_cause = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_req_type_cause[_MME_METR_BY_REQ_TYPE_CAUSE_MAX] = {
[MME_METR_BY_REQ_TYPE_CAUSE_PDN_CONN_REJECT] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_pdn_connectivity_reject_total",
    .description = "PDN Connectivity Request rejected, "
            "bucketed by request_type and esm_cause",
    .num_labels = OGS_ARRAY_SIZE(labels_req_type_cause),
    .labels = labels_req_type_cause,
},
};

typedef struct mme_metric_key_by_req_type_cause_s {
    char request_type[16];
    char esm_cause[32];
    mme_metric_type_by_req_type_cause_t t;
} mme_metric_key_by_req_type_cause_t;

void mme_metrics_inst_by_req_type_cause_add(const char *request_type,
        const char *esm_cause, mme_metric_type_by_req_type_cause_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_req_type_cause_t *key;

    if (!request_type) request_type = "other";
    if (!esm_cause) esm_cause = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->request_type, request_type, sizeof(key->request_type));
    ogs_cpystrn(key->esm_cause, esm_cause, sizeof(key->esm_cause));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_req_type_cause, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_req_type_cause[t],
                mme_metrics_spec_def_by_req_type_cause->num_labels,
                (const char *[]){ key->request_type, key->esm_cause });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_req_type_cause, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY RESULT + CAUSE (2-label) */
static const char *labels_result_cause[] = { "result", "emm_cause" };
ogs_metrics_spec_t *mme_metrics_spec_by_result_cause[_MME_METR_BY_RESULT_CAUSE_MAX];
static ogs_hash_t *metrics_hash_by_result_cause = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_result_cause[_MME_METR_BY_RESULT_CAUSE_MAX] = {
[MME_METR_BY_RESULT_CAUSE_ATTACH] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_attach_total",
    .description = "EPS attach outcomes bucketed by result and emm_cause",
    .num_labels = OGS_ARRAY_SIZE(labels_result_cause),
    .labels = labels_result_cause,
},
};

typedef struct mme_metric_key_by_result_cause_s {
    char result[16];
    char emm_cause[32];
    mme_metric_type_by_result_cause_t t;
} mme_metric_key_by_result_cause_t;

void mme_metrics_inst_by_result_cause_add(const char *result,
        const char *emm_cause, mme_metric_type_by_result_cause_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_result_cause_t *key;

    if (!result) result = "other";
    if (!emm_cause) emm_cause = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->result, result, sizeof(key->result));
    ogs_cpystrn(key->emm_cause, emm_cause, sizeof(key->emm_cause));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_result_cause, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_result_cause[t],
                mme_metrics_spec_def_by_result_cause->num_labels,
                (const char *[]){ key->result, key->emm_cause });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_result_cause, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY REASON + OUTCOME (2-label) */
static const char *labels_reason_outcome[] = { "reason", "outcome" };
ogs_metrics_spec_t *mme_metrics_spec_by_reason_outcome[_MME_METR_BY_REASON_OUTCOME_MAX];
static ogs_hash_t *metrics_hash_by_reason_outcome = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_reason_outcome[_MME_METR_BY_REASON_OUTCOME_MAX] = {
[MME_METR_BY_REASON_OUTCOME_ICS_FILTER] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_ics_filter_total",
    .description = "Phase 4 PR1+PR2 ICS deletion_in_progress filter "
            "activations bucketed by reason and outcome",
    .num_labels = OGS_ARRAY_SIZE(labels_reason_outcome),
    .labels = labels_reason_outcome,
},
};

typedef struct mme_metric_key_by_reason_outcome_s {
    char reason[24];
    char outcome[16];
    mme_metric_type_by_reason_outcome_t t;
} mme_metric_key_by_reason_outcome_t;

void mme_metrics_inst_by_reason_outcome_add(const char *reason,
        const char *outcome, mme_metric_type_by_reason_outcome_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_reason_outcome_t *key;

    if (!reason) reason = "other";
    if (!outcome) outcome = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->reason, reason, sizeof(key->reason));
    ogs_cpystrn(key->outcome, outcome, sizeof(key->outcome));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_reason_outcome, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_reason_outcome[t],
                mme_metrics_spec_def_by_reason_outcome->num_labels,
                (const char *[]){ key->reason, key->outcome });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_reason_outcome, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* HISTOGRAM: HO duration (direction label, milliseconds observation).
 *
 * The bucket boundaries are chosen to cover the empirical 50ms .. 25.6s
 * range observed across the Phase 4 verification PCAPs (typical HO is
 * ~350ms, but PR6-B-cascaded HO can run multi-second). Note that
 * ogs_metrics_inst_add() takes an int, so observations are stored as
 * integer milliseconds. The exposition therefore reports buckets in
 * milliseconds (le="50", le="100", ...). */
static const char *labels_histogram_direction[] = { "direction" };
ogs_metrics_spec_t *mme_metrics_spec_histogram[_MME_METR_HISTOGRAM_MAX];
static ogs_hash_t *metrics_hash_histogram = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_histogram[_MME_METR_HISTOGRAM_MAX] = {
[MME_METR_HISTOGRAM_HO_DURATION] = {
    .type = OGS_METRICS_METRIC_TYPE_HISTOGRAM,
    .name = "epc_mme_handover_duration_milliseconds",
    .description = "Handover duration observed by the MME, in "
            "milliseconds. From PDN Connectivity Request "
            "(request_type=HANDOVER) reception to Modify Bearer "
            "Response Cause=ACCEPTED.",
    .num_labels = OGS_ARRAY_SIZE(labels_histogram_direction),
    .labels = labels_histogram_direction,
    .histogram_params = {
        .type = OGS_METRICS_HISTOGRAM_BUCKET_TYPE_VARIABLE,
        .count = 10,
        .var.buckets = {
            50.0, 100.0, 200.0, 400.0, 800.0,
            1600.0, 3200.0, 6400.0, 12800.0, 25600.0,
        },
    },
},
};

typedef struct mme_metric_key_histogram_s {
    char direction[24];
    mme_metric_type_histogram_t t;
} mme_metric_key_histogram_t;

void mme_metrics_inst_histogram_observe(const char *direction,
        mme_metric_type_histogram_t t, int milliseconds)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_histogram_t *key;

    if (!direction) direction = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->direction, direction, sizeof(key->direction));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_histogram, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_histogram[t],
                mme_metrics_spec_def_histogram->num_labels,
                (const char *[]){ key->direction });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_histogram, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, milliseconds);
}

/* =====================================================================
 * Tier 1 / Tier 2 KPI extensions (next batch)
 *
 * Three-point observation (Paging + Service Request + UEContextRelease)
 * for stealth radio-link failure detection (e.g. CM-CONNECTED but radio
 * dead), plus ECM state gauge and supporting histograms.
 * =====================================================================
 */

/* GLOBAL_EXT counters (no labels). */
ogs_metrics_spec_t *mme_metrics_spec_global_ext[_MME_METR_GLOB_EXT_MAX];
ogs_metrics_inst_t *mme_metrics_inst_global_ext[_MME_METR_GLOB_EXT_MAX];
mme_metrics_spec_def_t mme_metrics_spec_def_global_ext[_MME_METR_GLOB_EXT_MAX] = {
[MME_METR_GLOB_CTR_PAGING_SUCCESS] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_paging_success_total",
    .description = "Paging requests that elicited a Service Request "
            "from the UE (TS 32.426 MM.PagingEpsSucc analogue)",
},
[MME_METR_GLOB_CTR_PAGING_TIMEOUT] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_paging_timeout_total",
    .description = "Paging requests that did not receive a Service "
            "Request within the T3413 window (TS 32.426 MM.PagingEpsFail)",
},
[MME_METR_GLOB_CTR_SUSPECTED_STALE_CONNECTED] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_mme_suspected_stale_connected_total",
    .description = "UE contexts that remained in CM-CONNECTED past the "
            "warning threshold without observable activity (implementation-"
            "specific heuristic, distinct from the standard RLF counter)",
},
};

int mme_metrics_init_inst_global_ext(void)
{
    return mme_metrics_init_inst(mme_metrics_inst_global_ext,
            mme_metrics_spec_global_ext, _MME_METR_GLOB_EXT_MAX, 0, NULL);
}
int mme_metrics_free_inst_global_ext(void)
{
    return mme_metrics_free_inst(mme_metrics_inst_global_ext,
            _MME_METR_GLOB_EXT_MAX);
}

/* BY PAGING_CAUSE (cause label only). */
static const char *labels_paging_cause[] = { "cause" };
ogs_metrics_spec_t *mme_metrics_spec_by_paging_cause[_MME_METR_BY_PAGING_CAUSE_MAX];
static ogs_hash_t *metrics_hash_by_paging_cause = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_paging_cause[_MME_METR_BY_PAGING_CAUSE_MAX] = {
[MME_METR_BY_PAGING_CAUSE_ATTEMPT] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_paging_attempt_total",
    .description = "Paging Requests sent by the MME, bucketed by trigger "
            "cause (TS 32.426 MM.PagingEpsAtt analogue)",
    .num_labels = OGS_ARRAY_SIZE(labels_paging_cause),
    .labels = labels_paging_cause,
},
};

typedef struct mme_metric_key_by_paging_cause_s {
    char cause[16];
    mme_metric_type_by_paging_cause_t t;
} mme_metric_key_by_paging_cause_t;

void mme_metrics_inst_by_paging_cause_add(const char *cause,
        mme_metric_type_by_paging_cause_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_paging_cause_t *key;

    if (!cause) cause = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->cause, cause, sizeof(key->cause));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_paging_cause, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_paging_cause[t],
                mme_metrics_spec_def_by_paging_cause->num_labels,
                (const char *[]){ key->cause });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_paging_cause, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY RESULT+CAUSE EXT (2-label) — Service Request + ICS. */
static const char *labels_result_cause_ext[] = { "result", "cause" };
ogs_metrics_spec_t *mme_metrics_spec_by_result_cause_ext[_MME_METR_BY_RESULT_CAUSE_EXT_MAX];
static ogs_hash_t *metrics_hash_by_result_cause_ext = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_result_cause_ext[_MME_METR_BY_RESULT_CAUSE_EXT_MAX] = {
[MME_METR_BY_RESULT_CAUSE_EXT_SERVICE_REQUEST] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_service_request_total",
    .description = "Service Request outcomes bucketed by result "
            "(accept/reject) and reject cause",
    .num_labels = OGS_ARRAY_SIZE(labels_result_cause_ext),
    .labels = labels_result_cause_ext,
},
[MME_METR_BY_RESULT_CAUSE_EXT_ICS] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_initial_context_setup_total",
    .description = "Initial Context Setup outcomes bucketed by result "
            "(success/failure) and S1AP cause",
    .num_labels = OGS_ARRAY_SIZE(labels_result_cause_ext),
    .labels = labels_result_cause_ext,
},
};

typedef struct mme_metric_key_by_result_cause_ext_s {
    char result[16];
    char cause[32];
    mme_metric_type_by_result_cause_ext_t t;
} mme_metric_key_by_result_cause_ext_t;

void mme_metrics_inst_by_result_cause_ext_add(const char *result,
        const char *cause, mme_metric_type_by_result_cause_ext_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_result_cause_ext_t *key;

    if (!result) result = "other";
    if (!cause) cause = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->result, result, sizeof(key->result));
    ogs_cpystrn(key->cause, cause, sizeof(key->cause));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_result_cause_ext, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_result_cause_ext[t],
                mme_metrics_spec_def_by_result_cause_ext->num_labels,
                (const char *[]){ key->result, key->cause });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_result_cause_ext, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY CAUSE_GROUP+CAUSE (2-label) — UEContextRelease. */
static const char *labels_cause_group[] = { "cause_group", "cause" };
ogs_metrics_spec_t *mme_metrics_spec_by_cause_group[_MME_METR_BY_CAUSE_GROUP_MAX];
static ogs_hash_t *metrics_hash_by_cause_group = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_cause_group[_MME_METR_BY_CAUSE_GROUP_MAX] = {
[MME_METR_BY_CAUSE_GROUP_UE_CONTEXT_RELEASE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_mme_ue_context_release_total",
    .description = "UEContextRelease events bucketed by high-level "
            "cause_group and specific S1AP cause",
    .num_labels = OGS_ARRAY_SIZE(labels_cause_group),
    .labels = labels_cause_group,
},
};

typedef struct mme_metric_key_by_cause_group_s {
    char cause_group[16];
    char cause[32];
    mme_metric_type_by_cause_group_t t;
} mme_metric_key_by_cause_group_t;

void mme_metrics_inst_by_cause_group_add(const char *cause_group,
        const char *cause, mme_metric_type_by_cause_group_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_cause_group_t *key;

    if (!cause_group) cause_group = "other";
    if (!cause) cause = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->cause_group, cause_group, sizeof(key->cause_group));
    ogs_cpystrn(key->cause, cause, sizeof(key->cause));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_cause_group, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_cause_group[t],
                mme_metrics_spec_def_by_cause_group->num_labels,
                (const char *[]){ key->cause_group, key->cause });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_cause_group, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY STATE (state label, gauge) — ECM-CONNECTED / ECM-IDLE UE count. */
static const char *labels_state[] = { "state" };
ogs_metrics_spec_t *mme_metrics_spec_by_state[_MME_METR_BY_STATE_MAX];
static ogs_hash_t *metrics_hash_by_state = NULL;
mme_metrics_spec_def_t mme_metrics_spec_def_by_state[_MME_METR_BY_STATE_MAX] = {
[MME_METR_BY_STATE_UES_IN_STATE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "epc_mme_ues_in_state",
    .description = "UEs currently in the given ECM state. Only "
            "state=\"connected\" is tracked directly; idle count "
            "can be derived as (ues_active - ues_in_state{connected}).",
    .num_labels = OGS_ARRAY_SIZE(labels_state),
    .labels = labels_state,
},
};

typedef struct mme_metric_key_by_state_s {
    char state[16];
    mme_metric_type_by_state_t t;
} mme_metric_key_by_state_t;

void mme_metrics_inst_by_state_add(const char *state,
        mme_metric_type_by_state_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    mme_metric_key_by_state_t *key;

    if (!state) state = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->state, state, sizeof(key->state));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_state, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(mme_metrics_spec_by_state[t],
                mme_metrics_spec_def_by_state->num_labels,
                (const char *[]){ key->state });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_state, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* HISTOGRAM (seconds, unlabeled): connected-state duration + paging
 * response time. Bucket choice (10..1800s) per Codex MCP review —
 * warning at 120s, critical at 300s for the connected-state metric. */
ogs_metrics_spec_t *mme_metrics_spec_histogram_seconds[_MME_METR_HIST_SECONDS_MAX];
ogs_metrics_inst_t *mme_metrics_inst_histogram_seconds[_MME_METR_HIST_SECONDS_MAX];
mme_metrics_spec_def_t mme_metrics_spec_def_histogram_seconds[_MME_METR_HIST_SECONDS_MAX] = {
[MME_METR_HIST_CONNECTED_STATE_DURATION] = {
    .type = OGS_METRICS_METRIC_TYPE_HISTOGRAM,
    .name = "epc_mme_connected_state_duration_seconds",
    .description = "Duration each UE spent in CM-CONNECTED state, in "
            "seconds (from Initial Context Setup to Context Release). "
            "Long-tail values point at stealth radio-link failures.",
    .histogram_params = {
        .type = OGS_METRICS_HISTOGRAM_BUCKET_TYPE_VARIABLE,
        .count = 8,
        .var.buckets = {
            10.0, 20.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0,
        },
    },
},
[MME_METR_HIST_PAGING_RESPONSE_DURATION] = {
    .type = OGS_METRICS_METRIC_TYPE_HISTOGRAM,
    .name = "epc_mme_paging_response_duration_seconds",
    .description = "Latency from MME Paging Request to UE Service "
            "Request, in seconds.",
    .histogram_params = {
        .type = OGS_METRICS_HISTOGRAM_BUCKET_TYPE_VARIABLE,
        .count = 8,
        .var.buckets = {
            1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0,
        },
    },
},
};

static int mme_metrics_init_inst_histogram_seconds(void)
{
    return mme_metrics_init_inst(mme_metrics_inst_histogram_seconds,
            mme_metrics_spec_histogram_seconds,
            _MME_METR_HIST_SECONDS_MAX, 0, NULL);
}

void mme_metrics_inst_histogram_seconds_observe(
        mme_metric_type_histogram_seconds_t t, int seconds)
{
    if (t >= _MME_METR_HIST_SECONDS_MAX)
        return;
    if (mme_metrics_inst_histogram_seconds[t])
        ogs_metrics_inst_add(mme_metrics_inst_histogram_seconds[t], seconds);
}

/* Cause bucket helpers. Unknown values always return "other" so that
 * silent cardinality growth is impossible. */

const char *mme_gtpv2_cause_bucket(uint8_t cause)
{
    switch (cause) {
    case OGS_GTP2_CAUSE_REQUEST_ACCEPTED:
        return "accepted";
    case OGS_GTP2_CAUSE_CONTEXT_NOT_FOUND:
        return "context_not_found";
    case OGS_GTP2_CAUSE_USER_AUTHENTICATION_FAILED:
        return "user_auth_failed";
    case OGS_GTP2_CAUSE_TIMED_OUT_REQUEST:
        return "timed_out";
    case OGS_GTP2_CAUSE_REQUEST_REJECTED_REASON_NOT_SPECIFIED:
        return "request_rejected";
    case OGS_GTP2_CAUSE_TEMPORARILY_REJECTED_DUE_TO_HANDOVER_IN_PROGRESS:
        return "ho_in_progress";
    default:
        return "other";
    }
}

const char *mme_esm_cause_bucket(uint8_t cause)
{
    switch (cause) {
    case 0:
        return "accepted";
    case OGS_NAS_ESM_CAUSE_MISSING_OR_UNKNOWN_APN:
        return "missing_apn";
    case OGS_NAS_ESM_CAUSE_PDN_TYPE_IPV4_ONLY_ALLOWED:
        return "pdn_type_v4";
    case OGS_NAS_ESM_CAUSE_PDN_TYPE_IPV6_ONLY_ALLOWED:
        return "pdn_type_v6";
    case OGS_NAS_ESM_CAUSE_MULTIPLE_PDN_CONNECTIONS_FOR_A_GIVEN_APN_NOT_ALLOWED:
        return "multiple_pdn";
    case OGS_NAS_ESM_CAUSE_REQUEST_REJECTED_UNSPECIFIED:
        return "rejected_unspecified";
    default:
        return "other";
    }
}

const char *mme_emm_cause_bucket(uint8_t cause)
{
    switch (cause) {
    case 0:
        return "accepted";
    case OGS_NAS_EMM_CAUSE_ILLEGAL_UE:
        return "illegal_ue";
    case OGS_NAS_EMM_CAUSE_NO_SUITABLE_CELLS_IN_TRACKING_AREA:
        return "no_suitable_cell";
    case OGS_NAS_EMM_CAUSE_NETWORK_FAILURE:
        return "network_failure";
    case OGS_NAS_EMM_CAUSE_EPS_SERVICES_NOT_ALLOWED:
        return "eps_not_allowed";
    case OGS_NAS_EMM_CAUSE_UE_IDENTITY_CANNOT_BE_DERIVED_BY_THE_NETWORK:
        return "identity_unknown";
    default:
        return "other";
    }
}

const char *mme_request_type_bucket(uint8_t request_type)
{
    switch (request_type) {
    case OGS_NAS_EPS_REQUEST_TYPE_INITIAL:
        return "initial";
    case OGS_NAS_EPS_REQUEST_TYPE_HANDOVER:
        return "handover";
    case OGS_NAS_EPS_REQUEST_TYPE_EMERGENCY:
        return "emergency";
    default:
        return "other";
    }
}

/* S1AP UEContextRelease cause bucket. Splits the cause into a
 * high-level cause_group and a bucketed specific cause string. Unknown
 * combinations fall through to "other" / "other".
 *
 * Inputs come from S1AP_Cause_PR (cause_present) and the corresponding
 * S1AP_Cause<Group>_* enum value. Constants from
 * lib/asn1c/s1ap/S1AP_Cause*.h:
 *   S1AP_Cause_PR_radioNetwork = 1
 *   S1AP_Cause_PR_transport    = 2
 *   S1AP_Cause_PR_nas          = 3
 *   S1AP_Cause_PR_protocol     = 4
 *   S1AP_Cause_PR_misc         = 5
 *   S1AP_CauseRadioNetwork_user_inactivity                 = 20
 *   S1AP_CauseRadioNetwork_radio_connection_with_ue_lost   = 21
 *   S1AP_CauseRadioNetwork_handover_*                      = 1..7 (HO family)
 *   S1AP_CauseNas_normal_release                           = 0
 *   S1AP_CauseNas_detach                                   = 2
 */
void mme_s1ap_release_cause_bucket(
        int cause_present, int cause_value,
        const char **cause_group_out, const char **cause_out)
{
    if (!cause_group_out || !cause_out)
        return;

    switch (cause_present) {
    case 1: /* radioNetwork */
        switch (cause_value) {
        case 20: /* user_inactivity */
            *cause_group_out = "radio";
            *cause_out = "user_inactivity";
            return;
        case 21: /* radio_connection_with_ue_lost */
            *cause_group_out = "radio";
            *cause_out = "radio_lost";
            return;
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            /* handover_* family */
            *cause_group_out = "mobility";
            *cause_out = "handover";
            return;
        default:
            *cause_group_out = "radio";
            *cause_out = "other";
            return;
        }
    case 2: /* transport */
        *cause_group_out = "transport";
        *cause_out = "other";
        return;
    case 3: /* nas */
        switch (cause_value) {
        case 0: /* normal_release */
            *cause_group_out = "normal";
            *cause_out = "normal_release";
            return;
        case 2: /* detach */
            *cause_group_out = "normal";
            *cause_out = "detach";
            return;
        default:
            *cause_group_out = "nas";
            *cause_out = "other";
            return;
        }
    case 4: /* protocol */
        *cause_group_out = "protocol";
        *cause_out = "other";
        return;
    case 5: /* misc */
        *cause_group_out = "misc";
        *cause_out = "other";
        return;
    default:
        *cause_group_out = "other";
        *cause_out = "other";
        return;
    }
}

/* Init / final */

static void mme_metrics_init_hashes(void)
{
    metrics_hash_by_cause = ogs_hash_make();
    ogs_assert(metrics_hash_by_cause);
    metrics_hash_by_direction = ogs_hash_make();
    ogs_assert(metrics_hash_by_direction);
    metrics_hash_by_request_type = ogs_hash_make();
    ogs_assert(metrics_hash_by_request_type);
    metrics_hash_by_req_type_cause = ogs_hash_make();
    ogs_assert(metrics_hash_by_req_type_cause);
    metrics_hash_by_result_cause = ogs_hash_make();
    ogs_assert(metrics_hash_by_result_cause);
    metrics_hash_by_reason_outcome = ogs_hash_make();
    ogs_assert(metrics_hash_by_reason_outcome);
    metrics_hash_histogram = ogs_hash_make();
    ogs_assert(metrics_hash_histogram);
    /* Tier 1 / Tier 2 KPI extension hashes. */
    metrics_hash_by_paging_cause = ogs_hash_make();
    ogs_assert(metrics_hash_by_paging_cause);
    metrics_hash_by_result_cause_ext = ogs_hash_make();
    ogs_assert(metrics_hash_by_result_cause_ext);
    metrics_hash_by_cause_group = ogs_hash_make();
    ogs_assert(metrics_hash_by_cause_group);
    metrics_hash_by_state = ogs_hash_make();
    ogs_assert(metrics_hash_by_state);
}

void mme_metrics_init(void)
{
    ogs_metrics_context_t *ctx = ogs_metrics_self();
    ogs_metrics_context_init();

    mme_metrics_init_spec(ctx, mme_metrics_spec_global,
            mme_metrics_spec_def_global, _MME_METR_GLOB_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_cause,
            mme_metrics_spec_def_by_cause, _MME_METR_BY_CAUSE_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_direction,
            mme_metrics_spec_def_by_direction, _MME_METR_BY_DIRECTION_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_request_type,
            mme_metrics_spec_def_by_request_type, _MME_METR_BY_REQUEST_TYPE_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_req_type_cause,
            mme_metrics_spec_def_by_req_type_cause,
            _MME_METR_BY_REQ_TYPE_CAUSE_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_result_cause,
            mme_metrics_spec_def_by_result_cause,
            _MME_METR_BY_RESULT_CAUSE_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_reason_outcome,
            mme_metrics_spec_def_by_reason_outcome,
            _MME_METR_BY_REASON_OUTCOME_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_histogram,
            mme_metrics_spec_def_histogram, _MME_METR_HISTOGRAM_MAX);

    /* Tier 1 / Tier 2 KPI extension specs. */
    mme_metrics_init_spec(ctx, mme_metrics_spec_global_ext,
            mme_metrics_spec_def_global_ext, _MME_METR_GLOB_EXT_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_paging_cause,
            mme_metrics_spec_def_by_paging_cause,
            _MME_METR_BY_PAGING_CAUSE_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_result_cause_ext,
            mme_metrics_spec_def_by_result_cause_ext,
            _MME_METR_BY_RESULT_CAUSE_EXT_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_cause_group,
            mme_metrics_spec_def_by_cause_group,
            _MME_METR_BY_CAUSE_GROUP_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_by_state,
            mme_metrics_spec_def_by_state, _MME_METR_BY_STATE_MAX);
    mme_metrics_init_spec(ctx, mme_metrics_spec_histogram_seconds,
            mme_metrics_spec_def_histogram_seconds,
            _MME_METR_HIST_SECONDS_MAX);

    mme_metrics_init_inst_global();
    mme_metrics_init_inst_global_ext();
    mme_metrics_init_inst_histogram_seconds();
    mme_metrics_init_hashes();
}

void mme_metrics_final(void)
{
    ogs_metrics_context_final();
}
