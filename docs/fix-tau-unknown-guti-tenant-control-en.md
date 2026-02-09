# TAU Request with Unknown GUTI - Tenant Control Fix

## Problem Overview

When receiving a TAU Request with an Unknown GUTI, the Tenant Control check is executed without IMSI, causing rejection with Cause #13 (`OGS_NAS_EMM_CAUSE_ROAMING_NOT_ALLOWED_IN_THIS_TRACKING_AREA`).

## Root Cause

### TAU Request Handler Issue (Line 718-728)

- For Unknown GUTI, the MME discards the old UE context, so IMSI is not available when TAU Request is received
- However, Tenant Control check is executed regardless of IMSI availability
- Check fails without IMSI and **rejects with Cause #13** (should be Cause #9 instead)

### Lack of Consistency

- **ATTACH REQUEST**: IMSI availability check before Tenant Control (guarded at Line 224, 260)
- **TAU REQUEST**: Tenant Control executed at Line 718 **without guard** ← Only exception

## Fix Details

### Fix: TAU Request Handler (src/mme/emm-handler.c Line 718-728)

**Before:**
```c
/* Check Tenant Control */
if (!mme_check_tenant_access(mme_ue, mme_ue->tai.tac)) {
    ogs_warn("IMSI[%s] not allowed on TAC[%d] - Tenant separation violation",
             MME_UE_HAVE_IMSI(mme_ue) ? mme_ue->imsi_bcd : "Unknown",
             mme_ue->tai.tac);
    r = nas_eps_send_tau_reject(enb_ue, mme_ue,
            OGS_NAS_EMM_CAUSE_ROAMING_NOT_ALLOWED_IN_THIS_TRACKING_AREA);
    ogs_expect(r == OGS_OK);
    ogs_assert(r != OGS_ERROR);
    return OGS_ERROR;
}
```

**After:**
```c
/* Check Tenant Control only when IMSI is known */
if (MME_UE_HAVE_IMSI(mme_ue) &&
    !mme_check_tenant_access(mme_ue, mme_ue->tai.tac)) {
    ogs_warn("IMSI[%s] not allowed on TAC[%d] - Tenant separation violation",
             mme_ue->imsi_bcd, mme_ue->tai.tac);
    r = nas_eps_send_tau_reject(enb_ue, mme_ue,
            OGS_NAS_EMM_CAUSE_ROAMING_NOT_ALLOWED_IN_THIS_TRACKING_AREA);
    ogs_expect(r == OGS_OK);
    ogs_assert(r != OGS_ERROR);
    return OGS_ERROR;
}
```

**Changes:**
- Added `MME_UE_HAVE_IMSI(mme_ue)` guard
- Skip Tenant Control check when IMSI is not available
- Removed ternary operator; use `mme_ue->imsi_bcd` only when IMSI is guaranteed to exist

## Impact of Fix

| Scenario | Before Fix | After Fix |
|----------|-----------|-----------|
| Known GUTI TAU (with IMSI) | ✅ Tenant Control check executed | ✅ Tenant Control check executed |
| Unknown GUTI TAU (no IMSI) | ❌ Immediate Reject with Cause #13 | ✅ Immediate Reject with Cause #9 |

## Processing Flow

### Before Fix (Problematic)

```
Unknown GUTI TAU Request (no IMSI)
  ↓
emm_handle_tau_request()
  ├─ Tenant Control check executed
  └─ Check fails due to missing IMSI → Reject with Cause #13 ❌
```

### After Fix (Correct)

```
Unknown GUTI TAU Request (no IMSI)
  ↓
emm_handle_tau_request()
  ├─ Tenant Control skipped due to missing IMSI ✅
  ↓
emm-sm.c:560
  └─ MME_UE_HAVE_IMSI() check fails → Reject with Cause #9 ✅
```

## Unknown GUTI TAU Behavior

Unknown GUTI TAU Request will ultimately be rejected with Cause #9 due to the following reasons:

1. **Unknown GUTI** → MME discards old UE context
2. **Session context loss** → S11 SGW UE binding and TEID are lost
3. **`SESSION_CONTEXT_IS_AVAILABLE()` check fails** (emm-sm.c:569, mme-s6a-handler.c:108)
4. **Reject with Cause #9** (`OGS_NAS_EMM_CAUSE_UE_IDENTITY_CANNOT_BE_DERIVED_BY_THE_NETWORK`)

This is a design constraint, and Unknown GUTI TAU is not a normal scenario.

## Security Impact

### Positive Impact

- **Improved Reject Cause Accuracy**: Unknown GUTI cases now send Cause #9 (identity cannot be derived)
- **Consistency**: IMSI check pattern is unified between ATTACH/TAU processing

### No Change

- **Known GUTI (IMSI known)**: Tenant Control check is executed as before
- **Tenant separation policy**: Unchanged

## Recommended Testing

### 1. Unknown GUTI TAU Scenario

- Send TAU Request with Unknown GUTI
- Verify rejection with Cause #9

### 2. Known GUTI TAU Scenario

- Send TAU Request with Known GUTI (IMSI known)
- Verify Tenant Control check is executed
- Verify processing follows tenant separation policy

### 3. ATTACH Scenario (Regression Test)

- ATTACH with IMSI
- ATTACH with GUTI → Identity Response
- Verify existing behavior is maintained

## File Information

| Item | Value |
|------|-------|
| **Modified File** | `src/mme/emm-handler.c` |
| **Branch** | `fix-tau-unknown-guti-tenant-control` |
| **Fix Date** | 2025-10-21 |
| **Binary** | `/usr/bin/open5gs-mmed` |
| **Binary MD5** | `871d2c305b4523a23f8307f508cfa3b8` |
| **Binary SHA256** | `4097563e44611ad6ab017f4f678fa8c42151f95b62ecaa3dfc91442ccccaf757` |

## Diff

```diff
diff --git a/src/mme/emm-handler.c b/src/mme/emm-handler.c
index 0f953c59e..617c42bd0 100644
--- a/src/mme/emm-handler.c
+++ b/src/mme/emm-handler.c
@@ -715,11 +715,11 @@ int emm_handle_tau_request(
     }
     ogs_debug("    SERVED_TAI_INDEX[%d]", served_tai_index);

-    /* Check Tenant Control */
-    if (!mme_check_tenant_access(mme_ue, mme_ue->tai.tac)) {
-        ogs_warn("IMSI[%s] not allowed on TAC[%d] - Tenant separation violation",
-                 MME_UE_HAVE_IMSI(mme_ue) ? mme_ue->imsi_bcd : "Unknown",
-                 mme_ue->tai.tac);
+    /* Check Tenant Control only when IMSI is known */
+    if (MME_UE_HAVE_IMSI(mme_ue) &&
+        !mme_check_tenant_access(mme_ue, mme_ue->tai.tac)) {
+        ogs_warn("IMSI[%s] not allowed on TAC[%d] - Tenant separation violation",
+                 mme_ue->imsi_bcd, mme_ue->tai.tac);
         r = nas_eps_send_tau_reject(enb_ue, mme_ue,
                 OGS_NAS_EMM_CAUSE_ROAMING_NOT_ALLOWED_IN_THIS_TRACKING_AREA);
         ogs_expect(r == OGS_OK);
```
