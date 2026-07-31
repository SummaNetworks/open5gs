#include "ogs-app.h"
#include "ogs-gtp.h"
#include "context.h"

#include "metrics.h"

typedef struct smf_metrics_spec_def_s {
    unsigned int type;
    const char *name;
    const char *description;
    int initial_val;
    unsigned int num_labels;
    const char **labels;
    ogs_metrics_histogram_params_t histogram_params;
} smf_metrics_spec_def_t;

/* Helper generic functions: */
static int smf_metrics_init_inst(ogs_metrics_inst_t **inst, ogs_metrics_spec_t **specs,
        unsigned int len, unsigned int num_labels, const char **labels)
{
    unsigned int i;
    for (i = 0; i < len; i++)
        inst[i] = ogs_metrics_inst_new(specs[i], num_labels, labels);
    return OGS_OK;
}

static int smf_metrics_free_inst(ogs_metrics_inst_t **inst,
        unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++)
        ogs_metrics_inst_free(inst[i]);
    memset(inst, 0, sizeof(inst[0]) * len);
    return OGS_OK;
}

static int smf_metrics_init_spec(ogs_metrics_context_t *ctx,
        ogs_metrics_spec_t **dst, smf_metrics_spec_def_t *src, unsigned int len)
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
ogs_metrics_spec_t *smf_metrics_spec_global[_SMF_METR_GLOB_MAX];
ogs_metrics_inst_t *smf_metrics_inst_global[_SMF_METR_GLOB_MAX];
smf_metrics_spec_def_t smf_metrics_spec_def_global[_SMF_METR_GLOB_MAX] = {
/* Global Counters: */
[SMF_METR_GLOB_CTR_GTP_NEW_NODE_FAILED] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "gtp_new_node_failed",
    .description = "Unable to allocate new GTP (peer) Node",
},
[SMF_METR_GLOB_CTR_GN_RX_PARSE_FAILED] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "gn_rx_parse_failed",
    .description = "Received GTPv1C messages discarded due to parsing failure",
},
[SMF_METR_GLOB_CTR_GN_RX_CREATEPDPCTXREQ] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "gn_rx_createpdpcontextreq",
    .description = "Received GTPv1C CreatePDPContextRequest messages",
},
[SMF_METR_GLOB_CTR_GN_RX_DELETEPDPCTXREQ] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "gn_rx_deletepdpcontextreq",
    .description = "Received GTPv1C DeletePDPContextRequest messages",
},
[SMF_METR_GLOB_CTR_S5C_RX_PARSE_FAILED] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "s5c_rx_parse_failed",
    .description = "Received GTPv2C messages discarded due to parsing failure",
},
[SMF_METR_GLOB_CTR_S5C_RX_CREATESESSIONREQ] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "s5c_rx_createsession",
    .description = "Received GTPv2C CreateSessionRequest messages",
},
[SMF_METR_GLOB_CTR_S5C_RX_DELETESESSIONREQ] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "s5c_rx_deletesession",
    .description = "Received GTPv2C DeleteSessionRequest messages",
},
[SMF_METR_GLOB_CTR_SM_N4SESSIONESTABREQ] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "fivegs_smffunction_sm_n4sessionestabreq",
    .description = "Number of requested N4 session establishments evidented by SMF",
},
[SMF_METR_GLOB_CTR_SM_N4SESSIONREPORT] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "fivegs_smffunction_sm_n4sessionreport",
    .description = "Number of requested N4 session reports evidented by SMF",
},
[SMF_METR_GLOB_CTR_SM_N4SESSIONREPORTSUCC] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "fivegs_smffunction_sm_n4sessionreportsucc",
    .description = "Number of successful N4 session reports evidented by SMF",
},
/* Global Gauges: */
[SMF_METR_GLOB_GAUGE_UES_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "ues_active",
    .description = "Active User Equipments",
},
[SMF_METR_GLOB_GAUGE_BEARERS_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "bearers_active",
    .description = "Active Bearers",
},
[SMF_METR_GLOB_GAUGE_GTP1_PDPCTXS_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "gtp1_pdpctxs_active",
    .description = "Active GTPv1 PDP Contexts (GGSN)",
},
[SMF_METR_GLOB_GAUGE_GTP2_SESSIONS_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "gtp2_sessions_active",
    .description = "Active GTPv2 Sessions (PGW)",
},
[SMF_METR_GLOB_GAUGE_GTP_PEERS_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "gtp_peers_active",
    .description = "Active GTP peers",
},
/* Open5GS implementation-specific (Phase 4 / Phase 4.1 hotfix). */
[SMF_METR_GLOB_CTR_PCC_RULE_SYNC_MISMATCH] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_smf_pcc_rule_sync_mismatch_total",
    .description = "PCC rule copy vs PCRF expected value mismatch "
            "detected during CCA handling",
},
[SMF_METR_GLOB_GAUGE_DEFERRED_DEACTIVATION_PENDING] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "open5gs_smf_deferred_deactivation_pending",
    .description = "Phase 3 deferred deactivation pending bearer count "
            "(concern 13 observability)",
},
};
int smf_metrics_init_inst_global(void)
{
    return smf_metrics_init_inst(smf_metrics_inst_global, smf_metrics_spec_global,
                _SMF_METR_GLOB_MAX, 0, NULL);
}
int smf_metrics_free_inst_global(void)
{
    return smf_metrics_free_inst(smf_metrics_inst_global, _SMF_METR_GLOB_MAX);
}

/* GTP NODE: */
const char *labels_gtp_node[] = {
    "addr"
};
#define SMF_METR_GTP_NODE_CTR_ENTRY(_id, _name, _desc) \
    [_id] = { \
        .type = OGS_METRICS_METRIC_TYPE_COUNTER, \
        .name = _name, \
        .description = _desc, \
        .num_labels = OGS_ARRAY_SIZE(labels_gtp_node), \
        .labels = labels_gtp_node, \
    },
ogs_metrics_spec_t *smf_metrics_spec_gtp_node[_SMF_METR_GTP_NODE_MAX];
smf_metrics_spec_def_t smf_metrics_spec_def_gtp_node[_SMF_METR_GTP_NODE_MAX] = {
/* Global Counters: */
SMF_METR_GTP_NODE_CTR_ENTRY(
    SMF_METR_GTP_NODE_CTR_GN_RX_PARSE_FAILED,
    "gtp_node_gn_rx_parse_failed",
    "Received GTPv1C messages discarded due to parsing failure")
SMF_METR_GTP_NODE_CTR_ENTRY(
    SMF_METR_GTP_NODE_CTR_GN_RX_CREATEPDPCTXREQ,
    "gtp_node_gn_rx_createpdpcontextreq",
    "Received GTPv1C CreatePDPContextRequest messages")
SMF_METR_GTP_NODE_CTR_ENTRY(
    SMF_METR_GTP_NODE_CTR_GN_RX_DELETEPDPCTXREQ,
    "gtp_node_gn_rx_deletepdpcontextreq",
    "Received GTPv1C DeletePDPContextRequest messages")
SMF_METR_GTP_NODE_CTR_ENTRY(
    SMF_METR_GTP_NODE_CTR_S5C_RX_PARSE_FAILED,
    "gtp_node_s5c_rx_parse_failed",
    "Received GTPv2C messages discarded due to parsing failure")
SMF_METR_GTP_NODE_CTR_ENTRY(
    SMF_METR_GTP_NODE_CTR_S5C_RX_CREATESESSIONREQ,
    "gtp_node_s5c_rx_createsession",
    "Received GTPv2C CreateSessionRequest messages")
SMF_METR_GTP_NODE_CTR_ENTRY(
    SMF_METR_GTP_NODE_CTR_S5C_RX_DELETESESSIONREQ,
    "gtp_node_s5c_rx_deletesession",
    "Received GTPv2C DeleteSessionRequest messages")
};
int smf_metrics_init_inst_gtp_node(ogs_metrics_inst_t **inst, const char *addr)
{
    return smf_metrics_init_inst(inst,
                smf_metrics_spec_gtp_node, _SMF_METR_GTP_NODE_MAX,
                smf_metrics_spec_def_gtp_node->num_labels, (const char *[]){ addr });
}
int smf_metrics_free_inst_gtp_node(ogs_metrics_inst_t **inst)
{
    return smf_metrics_free_inst(inst, _SMF_METR_GTP_NODE_MAX);
}

/* BY SLICE */
const char *labels_slice[] = {
    "plmnid",
    "snssai"
};

#define SMF_METR_BY_SLICE_GAUGE_ENTRY(_id, _name, _desc) \
    [_id] = { \
        .type = OGS_METRICS_METRIC_TYPE_GAUGE, \
        .name = _name, \
        .description = _desc, \
        .num_labels = OGS_ARRAY_SIZE(labels_slice), \
        .labels = labels_slice, \
    },
#define SMF_METR_BY_SLICE_CTR_ENTRY(_id, _name, _desc) \
    [_id] = { \
        .type = OGS_METRICS_METRIC_TYPE_COUNTER, \
        .name = _name, \
        .description = _desc, \
        .num_labels = OGS_ARRAY_SIZE(labels_slice), \
        .labels = labels_slice, \
    },
ogs_metrics_spec_t *smf_metrics_spec_by_slice[_SMF_METR_BY_SLICE_MAX];
ogs_hash_t *metrics_hash_by_slice = NULL;   /* hash table for SLICE labels */
smf_metrics_spec_def_t smf_metrics_spec_def_by_slice[_SMF_METR_BY_SLICE_MAX] = {
/* Gauges: */
SMF_METR_BY_SLICE_GAUGE_ENTRY(
    SMF_METR_GAUGE_SM_SESSIONNBR,
    "fivegs_smffunction_sm_sessionnbr",
    "Active Sessions")
SMF_METR_BY_SLICE_CTR_ENTRY(
    SMF_METR_CTR_SM_PDUSESSIONCREATIONREQ,
    "fivegs_smffunction_sm_pdusessioncreationreq",
    "Number of PDU sessions requested to be created by the SMF")
SMF_METR_BY_SLICE_CTR_ENTRY(
    SMF_METR_CTR_SM_PDUSESSIONCREATIONSUCC,
    "fivegs_smffunction_sm_pdusessioncreationsucc",
    "Number of PDU sessions successfully created by the SMF")
};
void smf_metrics_init_by_slice(void);
int smf_metrics_free_inst_by_slice(ogs_metrics_inst_t **inst);
typedef struct smf_metric_key_by_slice_s {
    ogs_plmn_id_t               plmn_id;
    ogs_s_nssai_t               snssai;
    smf_metric_type_by_slice_t  t;
} smf_metric_key_by_slice_t;

void smf_metrics_init_by_slice(void)
{
    metrics_hash_by_slice = ogs_hash_make();
    ogs_assert(metrics_hash_by_slice);
}

void smf_metrics_inst_by_slice_add(ogs_plmn_id_t *plmn,
        ogs_s_nssai_t *snssai, smf_metric_type_by_slice_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_slice_t *slice_key;

    slice_key = ogs_calloc(1, sizeof(*slice_key));
    ogs_assert(slice_key);

    if (plmn) {
        slice_key->plmn_id = *plmn;
    }

    if (snssai) {
        slice_key->snssai = *snssai;
    } else {
        slice_key->snssai.sst = 0;
        slice_key->snssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;
    }

    slice_key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_slice,
            slice_key, sizeof(*slice_key));

    if (!metrics) {
        char plmn_id[OGS_PLMNIDSTRLEN] = "";
        char *s_nssai = NULL;

        if (plmn) {
            ogs_plmn_id_to_string(plmn, plmn_id);
        }

        if (snssai) {
            s_nssai = ogs_sbi_s_nssai_to_string(snssai);
        } else {
            s_nssai = ogs_strdup("");
        }

        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_slice[t],
                smf_metrics_spec_def_by_slice->num_labels,
                (const char *[]){ plmn_id, s_nssai });

        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_slice,
                slice_key, sizeof(*slice_key), metrics);

        if (s_nssai)
            ogs_free(s_nssai);
    } else {
        ogs_free(slice_key);
    }

    ogs_metrics_inst_add(metrics, val);
}

int smf_metrics_free_inst_by_slice(ogs_metrics_inst_t **inst)
{
    return smf_metrics_free_inst(inst, _SMF_METR_BY_SLICE_MAX);
}

/* BY SLICE and 5QI */
const char *labels_5qi[] = {
    "plmnid",
    "snssai",
    "fiveqi"
};

#define SMF_METR_BY_5QI_GAUGE_ENTRY(_id, _name, _desc) \
    [_id] = { \
        .type = OGS_METRICS_METRIC_TYPE_GAUGE, \
        .name = _name, \
        .description = _desc, \
        .num_labels = OGS_ARRAY_SIZE(labels_5qi), \
        .labels = labels_5qi, \
    },
ogs_metrics_spec_t *smf_metrics_spec_by_5qi[_SMF_METR_BY_5QI_MAX];
ogs_hash_t *metrics_hash_by_5qi = NULL;   /* hash table for 5QI label */
smf_metrics_spec_def_t smf_metrics_spec_def_by_5qi[_SMF_METR_BY_5QI_MAX] = {
/* Gauges: */
SMF_METR_BY_5QI_GAUGE_ENTRY(
    SMF_METR_GAUGE_SM_QOSFLOWNBR,
    "fivegs_smffunction_sm_qos_flow_nbr",
    "Number of QoS flows at the SMF")
};
void smf_metrics_init_by_5qi(void);
int smf_metrics_free_inst_by_5qi(ogs_metrics_inst_t **inst);
typedef struct smf_metric_key_by_5qi_s {
    ogs_plmn_id_t               plmn_id;
    ogs_s_nssai_t               snssai;
    uint8_t                     fiveqi;
    smf_metric_type_by_5qi_t    t;
} smf_metric_key_by_5qi_t;

void smf_metrics_init_by_5qi(void)
{
    metrics_hash_by_5qi = ogs_hash_make();
    ogs_assert(metrics_hash_by_5qi);
}
void smf_metrics_inst_by_5qi_add(ogs_plmn_id_t *plmn,
        ogs_s_nssai_t *snssai, uint8_t fiveqi,
        smf_metric_type_by_5qi_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_5qi_t *fiveqi_key;

    fiveqi_key = ogs_calloc(1, sizeof(*fiveqi_key));
    ogs_assert(fiveqi_key);

    if (plmn) {
        fiveqi_key->plmn_id = *plmn;
    }

    if (snssai) {
        fiveqi_key->snssai = *snssai;
    } else {
        fiveqi_key->snssai.sst = 0;
        fiveqi_key->snssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;
    }

    fiveqi_key->fiveqi = fiveqi;
    fiveqi_key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_5qi,
            fiveqi_key, sizeof(*fiveqi_key));

    if (!metrics) {
        char plmn_id[OGS_PLMNIDSTRLEN] = "";
        char *s_nssai = NULL;
        char fiveqi_str[4];

        if (plmn) {
            ogs_plmn_id_to_string(plmn, plmn_id);
        }

        if (snssai) {
            s_nssai = ogs_sbi_s_nssai_to_string(snssai);
        } else {
            s_nssai = ogs_strdup("");
        }

        ogs_snprintf(fiveqi_str, sizeof(fiveqi_str), "%d", fiveqi);

        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_5qi[t],
                smf_metrics_spec_def_by_5qi->num_labels,
                (const char *[]){ plmn_id, s_nssai, fiveqi_str });

        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_5qi,
                fiveqi_key, sizeof(*fiveqi_key), metrics);

        if (s_nssai)
            ogs_free(s_nssai);
    } else {
        ogs_free(fiveqi_key);
    }

    ogs_metrics_inst_add(metrics, val);
}

int smf_metrics_free_inst_by_5qi(ogs_metrics_inst_t **inst)
{
    return smf_metrics_free_inst(inst, _SMF_METR_BY_5QI_MAX);
}

/* BY CAUSE */
const char *labels_cause[] = {
    "cause"
};

#define SMF_METR_BY_CAUSE_CTR_ENTRY(_id, _name, _desc) \
    [_id] = { \
        .type = OGS_METRICS_METRIC_TYPE_COUNTER, \
        .name = _name, \
        .description = _desc, \
        .num_labels = OGS_ARRAY_SIZE(labels_cause), \
        .labels = labels_cause, \
    },
ogs_metrics_spec_t *smf_metrics_spec_by_cause[_SMF_METR_BY_CAUSE_MAX];
ogs_hash_t *metrics_hash_by_cause = NULL;   /* hash table for CAUSE labels */
smf_metrics_spec_def_t smf_metrics_spec_def_by_cause[_SMF_METR_BY_CAUSE_MAX] = {
/* Counters: */
SMF_METR_BY_CAUSE_CTR_ENTRY(
    SMF_METR_CTR_SM_N4SESSIONESTABFAIL,
    "fivegs_smffunction_sm_n4sessionestabfail",
    "Number of failed N4 session establishments evidented by SMF")
SMF_METR_BY_CAUSE_CTR_ENTRY(
    SMF_METR_CTR_SM_PDUSESSIONCREATIONFAIL,
    "fivegs_smffunction_sm_pdusessioncreationfail",
    "Number of PDU sessions failed to be created by the SMF")
};
void smf_metrics_init_by_cause(void);
int smf_metrics_free_inst_by_cause(ogs_metrics_inst_t **inst);
typedef struct smf_metric_key_by_cause_s {
    int                         cause;
    smf_metric_type_by_cause_t  t;
} smf_metric_key_by_cause_t;

void smf_metrics_init_by_cause(void)
{
    metrics_hash_by_cause = ogs_hash_make();
    ogs_assert(metrics_hash_by_cause);
}

void smf_metrics_inst_by_cause_add(int cause,
        smf_metric_type_by_cause_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_cause_t *cause_key;

    cause_key = ogs_calloc(1, sizeof(*cause_key));
    ogs_assert(cause_key);

    cause_key->cause = cause;
    cause_key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_cause,
            cause_key, sizeof(*cause_key));

    if (!metrics) {
        char cause_str[4];
        ogs_snprintf(cause_str, sizeof(cause_str), "%d", cause);

        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_cause[t],
                smf_metrics_spec_def_by_cause->num_labels,
                (const char *[]){ cause_str });

        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_cause,
                cause_key, sizeof(*cause_key), metrics);
    } else {
        ogs_free(cause_key);
    }

    ogs_metrics_inst_add(metrics, val);
}

int smf_metrics_free_inst_by_cause(ogs_metrics_inst_t **inst)
{
    return smf_metrics_free_inst(inst, _SMF_METR_BY_CAUSE_MAX);
}

/* BY RAT (rat label) */
static const char *labels_rat[] = { "rat" };
ogs_metrics_spec_t *smf_metrics_spec_by_rat[_SMF_METR_BY_RAT_MAX];
static ogs_hash_t *metrics_hash_by_rat = NULL;
smf_metrics_spec_def_t smf_metrics_spec_def_by_rat[_SMF_METR_BY_RAT_MAX] = {
[SMF_METR_BY_RAT_CTR_SESSION_CREATE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_session_create_total",
    .description = "Sessions created by the SMF, bucketed by RAT "
            "(lte = MME/SGW S5/S8, wlan = ePDG S2b)",
    .num_labels = OGS_ARRAY_SIZE(labels_rat),
    .labels = labels_rat,
},
[SMF_METR_BY_RAT_CTR_SESSION_DELETE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_session_delete_total",
    .description = "Sessions deleted by the SMF, bucketed by RAT",
    .num_labels = OGS_ARRAY_SIZE(labels_rat),
    .labels = labels_rat,
},
[SMF_METR_BY_RAT_GAUGE_SESSION_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "epc_smf_session_active",
    .description = "Currently active sessions on the SMF, bucketed by RAT",
    .num_labels = OGS_ARRAY_SIZE(labels_rat),
    .labels = labels_rat,
},
[SMF_METR_BY_RAT_CTR_BEARER_CREATE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_bearer_create_total",
    .description = "Bearers created by the SMF (default + dedicated), "
            "bucketed by RAT",
    .num_labels = OGS_ARRAY_SIZE(labels_rat),
    .labels = labels_rat,
},
[SMF_METR_BY_RAT_GAUGE_BEARER_ACTIVE] = {
    .type = OGS_METRICS_METRIC_TYPE_GAUGE,
    .name = "epc_smf_bearer_active",
    .description = "Currently active bearers on the SMF, bucketed by RAT",
    .num_labels = OGS_ARRAY_SIZE(labels_rat),
    .labels = labels_rat,
},
};

typedef struct smf_metric_key_by_rat_s {
    char rat[8];
    smf_metric_type_by_rat_t t;
} smf_metric_key_by_rat_t;

void smf_metrics_inst_by_rat_add(const char *rat,
        smf_metric_type_by_rat_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_rat_t *key;

    if (!rat) rat = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->rat, rat, sizeof(key->rat));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_rat, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_rat[t],
                smf_metrics_spec_def_by_rat->num_labels,
                (const char *[]){ key->rat });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_rat, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

const char *smf_rat_label(uint8_t gtp_rat_type)
{
    switch (gtp_rat_type) {
    case OGS_GTP2_RAT_TYPE_EUTRAN:
        return "lte";
    case OGS_GTP2_RAT_TYPE_WLAN:
        return "wlan";
    default:
        return "other";
    }
}

/* BY DIRECTION (direction label) */
static const char *labels_direction[] = { "direction" };
ogs_metrics_spec_t *smf_metrics_spec_by_direction[_SMF_METR_BY_DIRECTION_MAX];
static ogs_hash_t *metrics_hash_by_direction = NULL;
smf_metrics_spec_def_t smf_metrics_spec_def_by_direction[_SMF_METR_BY_DIRECTION_MAX] = {
[SMF_METR_BY_DIRECTION_CTR_HO_ATTEMPT] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_handover_attempt_total",
    .description = "Handover attempts observed by the SMF",
    .num_labels = OGS_ARRAY_SIZE(labels_direction),
    .labels = labels_direction,
},
[SMF_METR_BY_DIRECTION_CTR_HO_SUCCESS] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_handover_success_total",
    .description = "Handover control-plane successes observed by the SMF",
    .num_labels = OGS_ARRAY_SIZE(labels_direction),
    .labels = labels_direction,
},
[SMF_METR_BY_DIRECTION_CTR_DATA_PLANE_PATH_SWITCH] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_data_plane_path_switch_total",
    .description = "PFCP MODIFY_HANDOVER path switches issued by the SMF",
    .num_labels = OGS_ARRAY_SIZE(labels_direction),
    .labels = labels_direction,
},
[SMF_METR_BY_DIRECTION_CTR_HO_MULTI_DEDICATED_BEARER_LOSS] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_handover_multi_dedicated_bearer_loss_total",
    .description = "Inter-RAT HO attempts where source session had "
                   ">1 dedicated bearer; Open5GS supports only 1 per "
                   "session so bearer #2+ is lost on target "
                   "(future-work.md section 2.2 issue 15)",
    .num_labels = OGS_ARRAY_SIZE(labels_direction),
    .labels = labels_direction,
},
};

typedef struct smf_metric_key_by_direction_s {
    char direction[24];
    smf_metric_type_by_direction_t t;
} smf_metric_key_by_direction_t;

void smf_metrics_inst_by_direction_add(const char *direction,
        smf_metric_type_by_direction_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_direction_t *key;

    if (!direction) direction = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->direction, direction, sizeof(key->direction));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_direction, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_direction[t],
                smf_metrics_spec_def_by_direction->num_labels,
                (const char *[]){ key->direction });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_direction, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY APP+EVENT (app, event) */
static const char *labels_app_event[] = { "app", "event" };
ogs_metrics_spec_t *smf_metrics_spec_by_app_event[_SMF_METR_BY_APP_EVENT_MAX];
static ogs_hash_t *metrics_hash_by_app_event = NULL;
smf_metrics_spec_def_t smf_metrics_spec_def_by_app_event[_SMF_METR_BY_APP_EVENT_MAX] = {
[SMF_METR_BY_APP_EVENT_CTR_DIAMETER_LIFECYCLE] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "epc_smf_diameter_session_lifecycle_total",
    .description = "Diameter session lifecycle transitions on the SMF "
            "(app = gx/gy/s6b/s6a, event = init/update/term/error)",
    .num_labels = OGS_ARRAY_SIZE(labels_app_event),
    .labels = labels_app_event,
},
};

typedef struct smf_metric_key_by_app_event_s {
    char app[8];
    char event[16];
    smf_metric_type_by_app_event_t t;
} smf_metric_key_by_app_event_t;

void smf_metrics_inst_by_app_event_add(const char *app, const char *event,
        smf_metric_type_by_app_event_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_app_event_t *key;

    if (!app) app = "other";
    if (!event) event = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->app, app, sizeof(key->app));
    ogs_cpystrn(key->event, event, sizeof(key->event));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_app_event, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_app_event[t],
                smf_metrics_spec_def_by_app_event->num_labels,
                (const char *[]){ key->app, key->event });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_app_event, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* BY OUTCOME (outcome label) */
static const char *labels_outcome[] = { "outcome" };
ogs_metrics_spec_t *smf_metrics_spec_by_outcome[_SMF_METR_BY_OUTCOME_MAX];
static ogs_hash_t *metrics_hash_by_outcome = NULL;
smf_metrics_spec_def_t smf_metrics_spec_def_by_outcome[_SMF_METR_BY_OUTCOME_MAX] = {
[SMF_METR_BY_OUTCOME_CTR_HOLD_TIMER] = {
    .type = OGS_METRICS_METRIC_TYPE_COUNTER,
    .name = "open5gs_smf_handover_hold_timer_fired_total",
    .description = "Phase 4 PR3+PR4 Handover Hold Timer outcomes "
            "(expired/cancelled/no_op)",
    .num_labels = OGS_ARRAY_SIZE(labels_outcome),
    .labels = labels_outcome,
},
};

typedef struct smf_metric_key_by_outcome_s {
    char outcome[16];
    smf_metric_type_by_outcome_t t;
} smf_metric_key_by_outcome_t;

void smf_metrics_inst_by_outcome_add(const char *outcome,
        smf_metric_type_by_outcome_t t, int val)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_by_outcome_t *key;

    if (!outcome) outcome = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->outcome, outcome, sizeof(key->outcome));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_by_outcome, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(smf_metrics_spec_by_outcome[t],
                smf_metrics_spec_def_by_outcome->num_labels,
                (const char *[]){ key->outcome });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_by_outcome, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, val);
}

/* HISTOGRAM (direction label, milliseconds): HO duration on SMF. */
static const char *labels_histogram_direction[] = { "direction" };
ogs_metrics_spec_t *smf_metrics_spec_histogram[_SMF_METR_HISTOGRAM_MAX];
static ogs_hash_t *metrics_hash_histogram = NULL;
smf_metrics_spec_def_t smf_metrics_spec_def_histogram[_SMF_METR_HISTOGRAM_MAX] = {
[SMF_METR_HISTOGRAM_HO_DURATION] = {
    .type = OGS_METRICS_METRIC_TYPE_HISTOGRAM,
    .name = "epc_smf_handover_duration_milliseconds",
    .description = "Handover duration observed by the SMF, in "
            "milliseconds. From HO start (S2b CSReq with rat=WLAN or "
            "S5C CSReq with request_type=HANDOVER) to MBR success.",
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

typedef struct smf_metric_key_histogram_s {
    char direction[24];
    smf_metric_type_histogram_t t;
} smf_metric_key_histogram_t;

void smf_metrics_inst_histogram_observe(const char *direction,
        smf_metric_type_histogram_t t, int milliseconds)
{
    ogs_metrics_inst_t *metrics = NULL;
    smf_metric_key_histogram_t *key;

    if (!direction) direction = "other";

    key = ogs_calloc(1, sizeof(*key));
    ogs_assert(key);
    ogs_cpystrn(key->direction, direction, sizeof(key->direction));
    key->t = t;

    metrics = ogs_hash_get(metrics_hash_histogram, key, sizeof(*key));
    if (!metrics) {
        metrics = ogs_metrics_inst_new(smf_metrics_spec_histogram[t],
                smf_metrics_spec_def_histogram->num_labels,
                (const char *[]){ key->direction });
        ogs_assert(metrics);
        ogs_hash_set(metrics_hash_histogram, key, sizeof(*key), metrics);
    } else {
        ogs_free(key);
    }
    ogs_metrics_inst_add(metrics, milliseconds);
}

void smf_metrics_init(void)
{
    ogs_metrics_context_t *ctx = ogs_metrics_self();
    ogs_metrics_context_init();

    smf_metrics_init_spec(ctx, smf_metrics_spec_global, smf_metrics_spec_def_global,
            _SMF_METR_GLOB_MAX);

    smf_metrics_init_spec(ctx, smf_metrics_spec_gtp_node, smf_metrics_spec_def_gtp_node,
            _SMF_METR_GTP_NODE_MAX);

    smf_metrics_init_spec(ctx, smf_metrics_spec_by_slice,
            smf_metrics_spec_def_by_slice, _SMF_METR_BY_SLICE_MAX);
    smf_metrics_init_spec(ctx, smf_metrics_spec_by_5qi,
            smf_metrics_spec_def_by_5qi, _SMF_METR_BY_5QI_MAX);
    smf_metrics_init_spec(ctx, smf_metrics_spec_by_cause,
            smf_metrics_spec_def_by_cause, _SMF_METR_BY_CAUSE_MAX);

    /* New scopes added by the Phase 4.x KPI batch. */
    smf_metrics_init_spec(ctx, smf_metrics_spec_by_rat,
            smf_metrics_spec_def_by_rat, _SMF_METR_BY_RAT_MAX);
    smf_metrics_init_spec(ctx, smf_metrics_spec_by_direction,
            smf_metrics_spec_def_by_direction, _SMF_METR_BY_DIRECTION_MAX);
    smf_metrics_init_spec(ctx, smf_metrics_spec_by_app_event,
            smf_metrics_spec_def_by_app_event, _SMF_METR_BY_APP_EVENT_MAX);
    smf_metrics_init_spec(ctx, smf_metrics_spec_by_outcome,
            smf_metrics_spec_def_by_outcome, _SMF_METR_BY_OUTCOME_MAX);
    smf_metrics_init_spec(ctx, smf_metrics_spec_histogram,
            smf_metrics_spec_def_histogram, _SMF_METR_HISTOGRAM_MAX);

    smf_metrics_init_inst_global();
    smf_metrics_init_by_slice();
    smf_metrics_init_by_5qi();
    smf_metrics_init_by_cause();

    metrics_hash_by_rat = ogs_hash_make();
    ogs_assert(metrics_hash_by_rat);
    metrics_hash_by_direction = ogs_hash_make();
    ogs_assert(metrics_hash_by_direction);
    metrics_hash_by_app_event = ogs_hash_make();
    ogs_assert(metrics_hash_by_app_event);
    metrics_hash_by_outcome = ogs_hash_make();
    ogs_assert(metrics_hash_by_outcome);
    metrics_hash_histogram = ogs_hash_make();
    ogs_assert(metrics_hash_histogram);
}

void smf_metrics_final(void)
{
    ogs_hash_index_t *hi;

    if (metrics_hash_by_slice) {
        for (hi = ogs_hash_first(metrics_hash_by_slice); hi; hi = ogs_hash_next(hi)) {
            smf_metric_key_by_slice_t *key =
                (smf_metric_key_by_slice_t *)ogs_hash_this_key(hi);
            //void *val = ogs_hash_this_val(hi);

            ogs_hash_set(metrics_hash_by_slice, key, sizeof(*key), NULL);

            ogs_free(key);
            /* don't free val (metric itself) -
             * it will be free'd by ogs_metrics_context_final() */
            //ogs_free(val);
        }
        ogs_hash_destroy(metrics_hash_by_slice);
    }
    if (metrics_hash_by_5qi) {
        for (hi = ogs_hash_first(metrics_hash_by_5qi); hi; hi = ogs_hash_next(hi)) {
            smf_metric_key_by_5qi_t *key =
                (smf_metric_key_by_5qi_t *)ogs_hash_this_key(hi);
            //void *val = ogs_hash_this_val(hi);

            ogs_hash_set(metrics_hash_by_5qi, key, sizeof(*key), NULL);

            ogs_free(key);
            /* don't free val (metric itself) -
             * it will be free'd by ogs_metrics_context_final() */
            //ogs_free(val);
        }
        ogs_hash_destroy(metrics_hash_by_5qi);
    }
    if (metrics_hash_by_cause) {
        for (hi = ogs_hash_first(metrics_hash_by_cause); hi; hi = ogs_hash_next(hi)) {
            smf_metric_key_by_cause_t *key =
                (smf_metric_key_by_cause_t *)ogs_hash_this_key(hi);
            //void *val = ogs_hash_this_val(hi);

            ogs_hash_set(metrics_hash_by_cause, key, sizeof(*key), NULL);

            ogs_free(key);
            /* don't free val (metric itself) -
             * it will be free'd by ogs_metrics_context_final() */
            //ogs_free(val);
        }
        ogs_hash_destroy(metrics_hash_by_cause);
    }

    ogs_metrics_context_final();
}
