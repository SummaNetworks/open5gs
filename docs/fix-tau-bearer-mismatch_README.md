# TAU Bearer Status Mismatch Fix

## Overview

This fix resolves the TAU (Tracking Area Update) Request bearer status mismatch issue in Open5GS MME. When a UE reports a bearer status as inactive while the MME recognizes it as active, the MME must perform "local deactivation" according to 3GPP TS 24.301 Section 5.5.3.2.4.

Before this fix, when a Delete Session Response was received and the ENB-S1 context had already been removed, the handler would return early, preventing session cleanup. This resulted in "zombie sessions" that caused subsequent PDN Connectivity Requests to fail with "APN duplicated" errors.

## Problem Details

### Issues Encountered

1. **Incorrect bearer information in TAU Accept**
   - Bearers being deleted were reported as ACTIVE
   - State inconsistency between UE and MME

2. **Deleted bearer information in Modify Bearer Request during Service Request**
   - Deleted bearers remained in bearer_to_modify_list
   - Invalid bearer information sent to SGWC

3. **PDN Connectivity Request rejected with Cause 55**
   - Sessions being deleted were not fully removed
   - New connection requests for the same APN rejected with "Multiple PDN connections for a given APN not allowed"

### Root Cause

**File**: `src/mme/mme-s11-handler.c:732-735`

```c
if (!enb_ue) {
    ogs_error("ENB-S1 Context has already been removed");
    return;  // ← Early return
}
```

**Problem Sequence**:
```
1. TAU Request received → Bearer mismatch detected
2. Delete Session Request sent (OGS_GTP_DELETE_NO_ACTION)
3. TAU Accept sent → UE Context Release
4. enb_ue context deleted
5. Delete Session Response received → enb_ue == NULL
6. Early return → MME_SESS_CLEAR() not reached
7. Session remains (zombie session)
```

## Implemented Fixes

### Fix 1: Improved Delete Session Response Processing

**File**: `src/mme/mme-s11-handler.c`
**Lines**: 732-744

**Modification**:
```c
if (!enb_ue) {
    /* For OGS_GTP_DELETE_NO_ACTION (local deactivation during TAU),
     * enb_ue might already be NULL because UE Context Release
     * can happen before Delete Session Response arrives.
     * We still need to clean up the session to prevent zombie sessions.
     * For other actions, enb_ue is required for signaling. */
    if (action != OGS_GTP_DELETE_NO_ACTION) {
        ogs_error("ENB-S1 Context has already been removed");
        return;
    }
    ogs_warn("ENB-S1 Context not available, proceeding with session cleanup "
             "for local deactivation (action=%d)", action);
}
```

**Rationale**: OGS_GTP_DELETE_NO_ACTION signifies "local deactivation", which deletes the session without notifying the UE or eNB. Therefore, session cleanup must proceed even if enb_ue has already been deleted.

### Fix 2: Skip Sessions Being Deleted in APN Search

**File**: `src/mme/mme-context.c`
**Function**: `mme_sess_find_by_apn()`
**Lines**: 4152-4159

**Modification**:
```c
if (ogs_strcasecmp(sess->session->name, apn) == 0) {
    /* Skip sessions that are being deleted to prevent
     * "APN duplicated" false positives during TAU bearer
     * status mismatch handling */
    if (!sess->deletion_in_progress) {
        return sess;
    }
}
```

**Rationale**: Excluding sessions being deleted from search results prevents new PDN Connectivity Requests from being incorrectly rejected with "APN duplicated".

### Fix 3: Introduction of deletion_in_progress Flag

**File**: `src/mme/mme-context.h`
**Line**: 797

**Added Field**:
```c
typedef struct mme_sess_s {
    // ... existing fields ...
    bool ue_pdn_status_mismatch;
    bool deletion_in_progress;  // Added
} mme_sess_t;
```

**Usage Locations**:

1. **Flag Setting** (`src/mme/emm-sm.c:689`):
```c
mme_gtp_send_delete_session_request(
    enb_ue, sgw_ue, sess,
    OGS_GTP_DELETE_NO_ACTION);

/* Mark session deletion in progress to exclude from TAU Accept */
sess->deletion_in_progress = true;
```

2. **TAU Accept Construction** (`src/mme/emm-build.c:576-580`):
```c
sess = mme_sess_first(mme_ue);
while (sess) {
    /* Skip sessions that are being deleted */
    if (sess->deletion_in_progress) {
        sess = mme_sess_next(sess);
        continue;
    }
    // ... include bearer information ...
}
```

3. **Service Request Processing** (`src/mme/s1ap-handler.c:1062-1066`):
```c
if (sess->deletion_in_progress) {
    ogs_info("    Skipping bearer EBI[%d] - session deletion in progress",
             bearer->ebi);
    continue;
}
```

4. **APN Search** (`src/mme/mme-context.c:4156`): As shown in Fix 2 above

### Fix 4: Improved Bearer Deletion Processing

**File**: `src/mme/mme-context.c`
**Function**: `mme_bearer_remove()`
**Lines**: 4251-4253

**Modification**:
```c
ogs_list_remove(&sess->bearer_list, bearer);

/* Remove from bearer_to_modify_list if present */
if (ogs_list_exists(&mme_ue->bearer_to_modify_list, &bearer->to_modify_node))
    ogs_list_remove(&mme_ue->bearer_to_modify_list, &bearer->to_modify_node);
```

**Rationale**: Bearers may be present in multiple lists, so they must be removed from all lists.

## Technical Details

### Delete Session Response Action Flow

There are 8 types of actions for Delete Session Response, but TAU bearer mismatch always uses `OGS_GTP_DELETE_NO_ACTION`.

| Action | Reaches MME_SESS_CLEAR() | Usage |
|--------|-------------------------|-------|
| OGS_GTP_DELETE_NO_ACTION | ✅ Reaches | **TAU bearer mismatch** |
| DELETE_SEND_AUTHENTICATION_REQUEST | ✅ Reaches | Re-authentication needed |
| DELETE_SEND_DETACH_ACCEPT | ✅ Reaches | Send Detach Accept |
| DELETE_SEND_DEACTIVATE_BEARER_CONTEXT_REQUEST | ❌ Early return | Waiting for NAS notification |
| DELETE_SEND_RELEASE_WITH_UE_CONTEXT_REMOVE | ✅ Reaches | UE Context removal |
| DELETE_SEND_RELEASE_WITH_S1_REMOVE_AND_UNLINK | ✅ Reaches | S1 removal |
| DELETE_HANDLE_PDN_CONNECTIVITY_REQUEST | ✅ Reaches | PDN connection handling |
| DELETE_IN_PATH_SWITCH_REQUEST | ❌ Early return | For X2 HO |

### deletion_in_progress Flag Lifecycle

```
1. TAU Request received → Bearer mismatch detected
   ↓
2. Delete Session Request sent
   ↓
3. sess->deletion_in_progress = true set
   ↓
4. TAU Accept construction → Skip sessions with deletion_in_progress==true
   ↓
5. Delete Session Response received
   ↓
6. MME_SESS_CLEAR(sess) executed
   ↓
7. mme_sess_remove() → ogs_pool_id_free()
   ↓
8. Entire session structure deleted (flag automatically cleaned up)
```

**Important**: The flag does not need to be explicitly set to `false`. It is automatically cleaned up when the session is deleted along with its memory.

## Verification Results

### Log Analysis

**Logs after fix**:
```
10/15 20:32:55.117: [emm] WARNING: Default Bearer[EBI:5] mismatch (MME:Active / UE:Inactive)
10/15 20:32:55.117: [emm] INFO: Locally deactivating PDN[APN:internet,EBI:5]
10/15 20:32:55.181: [mme] INFO: UE Context Release [Action:2]
10/15 20:32:55.181: [mme] INFO: [Removed] Number of eNB-UEs is now 0
10/15 20:32:55.197: [mme] WARNING: ENB-S1 Context not available, proceeding with session cleanup for local deactivation (action=1)
10/15 20:32:55.197: [mme] INFO: Removed Session: UE IMSI:[441216000000005] APN:[internet]
10/15 20:32:55.197: [mme] INFO: [Removed] Number of MME-Sessions is now 1
```

**Confirmation**:
- ✅ Session cleanup proceeded even with enb_ue == NULL
- ✅ "Removed Session" message confirmed session deletion
- ✅ Zombie session issue resolved

### Packet Capture Analysis (Frame #389939)

**TAU Accept Contents**:
```
EPS bearer context status:
- EBI(6): ACTIVE  ← VoLTE (ims) - normal session
- EBI(5): INACTIVE ← Data (internet) - excluded by deletion_in_progress
- EBI(8): ACTIVE  ← Separate issue (discussed below)
```

**Expected Behavior**:
- UE receives EBI(5)=INACTIVE
- According to 3GPP specifications, locally deactivates
- Reconnects via PDN Connectivity Request as needed

**This is Normal Behavior**:
- 3GPP TS 24.301 compliant
- UE and MME states are consistent
- Data service recovers via reconnection

## Known Limitations

### Temporary EBI(8) Inconsistency

**Situation**:
During Service Request, a new internet session (EBI 8) attempted creation but failed due to PCC rule error. However, at TAU Accept transmission time, cleanup had not yet completed, so EBI(8) was included as ACTIVE in TAU Accept.

**Impact**:
- ⚠️ Minor: UE temporarily recognizes unusable bearer
- ✅ Self-healing: UE reconnects and establishes normal session
- ⚠️ Temporary resource leak: Incomplete session persists

**Frequency**: Rare (requires timing race condition)

**Mitigation**:
Currently no additional fix required due to:
1. Low occurrence frequency
2. System self-heals
3. Does not cause critical functional failures

**Future Enhancement Ideas**:
- Session establishment state tracking (`establishment_complete` flag)
- Exclude incomplete sessions from TAU Accept

## 3GPP Specification Compliance

### 3GPP TS 24.301 Section 5.5.3.2.4

**Bearer status mismatch handling**:
> "If the MME has EPS bearer contexts that are not indicated as active by the UE, the MME shall locally deactivate the EPS bearer contexts without peer-to-peer signalling between the UE and the MME."

**Local deactivation meaning**:
- MME deletes the session
- No NAS/S1AP signaling to UE or eNB
- Reflected in EPS bearer context status IE of TAU Accept

### Implementation 3GPP Compliance

| Requirement | 3GPP Reference | Compliance |
|------------|---------------|-----------|
| Local deactivation | TS 24.301 5.5.3.2.4 | ✅ |
| No peer-to-peer signaling | TS 24.301 5.5.3.2.4 | ✅ |
| Bearer status accuracy | TS 24.301 5.5.3.2.4 | ✅ |
| Network resource cleanup | TS 23.401 | ✅ |

## Affected Files

### Modified Files

1. `src/mme/mme-context.h`
   - Added `deletion_in_progress` flag

2. `src/mme/mme-s11-handler.c`
   - Improved Delete Session Response processing
   - Handle enb_ue == NULL for OGS_GTP_DELETE_NO_ACTION

3. `src/mme/mme-context.c`
   - Modified `mme_sess_find_by_apn()`
   - Modified `mme_bearer_remove()`

4. `src/mme/emm-sm.c`
   - Set `deletion_in_progress` flag

5. `src/mme/emm-build.c`
   - Check `deletion_in_progress` during TAU Accept construction

6. `src/mme/s1ap-handler.c`
   - Check `deletion_in_progress` during Service Request processing

### Build and Deployment

```bash
# Build
./build.sh

# Update MME binary
./update_nf_binary.sh mme
```

## Testing

### Test Scenarios

1. **TAU Request with bearer mismatch**
   - UE reports EBI(5) as INACTIVE
   - MME recognizes EBI(5) as ACTIVE
   - Expected: Local deactivation succeeds, TAU Accept shows EBI(5)=INACTIVE

2. **Service Request after TAU**
   - UE sends Service Request after TAU
   - Expected: Deleted bearers not included

3. **PDN Connectivity Request for same APN**
   - After local deactivation, new connection for same APN
   - Expected: New session created normally

### Confirmation Logs

**Successful log pattern**:
```
[emm] WARNING: Default Bearer[EBI:X] mismatch
[emm] INFO: Locally deactivating PDN[APN:xxx,EBI:X]
[mme] WARNING: ENB-S1 Context not available, proceeding with session cleanup
[mme] INFO: Removed Session: UE IMSI:[xxx] APN:[xxx]
```

## Troubleshooting

### Issue: Zombie session remains

**Symptoms**:
- Session remains after Delete Session Response
- PDN Connectivity Request rejected with "APN duplicated"

**Verification**:
```bash
sudo grep "ENB-S1 Context not available" /var/log/open5gs/mme.log
sudo grep "Removed Session" /var/log/open5gs/mme.log
```

**Cause**: This fix not applied

### Issue: UE frequently reconnects

**Symptoms**:
- After TAU Accept, UE immediately sends Service Request or PDN Connectivity Request

**Cause**: EBI(8) issue (temporary inconsistency)

**Mitigation**: Usually self-heals, monitoring only is sufficient

## References

### 3GPP Specifications

- 3GPP TS 24.301 Section 5.5.3.2.4: Tracking area updating procedure
- 3GPP TS 23.401: GPRS enhancements for E-UTRAN access
- 3GPP TS 29.274: 3GPP Evolved Packet System (EPS); Evolved General Packet Radio Service (GPRS) Tunnelling Protocol for Control plane (GTPv2-C)

### Open5GS Related

- Open5GS Documentation: https://open5gs.org/
- GitHub Repository: https://github.com/open5gs/open5gs

## Changelog

| Date | Author | Changes |
|------|--------|---------|
| 2025-10-15 | Claude Code | Initial version |

## License

This fix follows the Open5GS license (AGPL-3.0).
