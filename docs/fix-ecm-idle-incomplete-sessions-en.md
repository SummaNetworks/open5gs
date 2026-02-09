# Fix for Incomplete Sessions in ECM-IDLE State

## Overview

This fix prevents crashes caused by multiple session issues by skipping session migration and cleaning up old contexts when an MME-UE in ECM-IDLE state (after S1 disconnection due to eNB Reset) holds incomplete sessions during a new Attach request.

**Fix Date**: 2025-10-30
**Target File**: `src/mme/mme-context.c`
**Related Issue**: TAU Unknown GUTI issue, Session migration bugs

## Background

### Crash Scenario

1. UE (IMSI: 441216000000000) sends initial Attach request
2. Session creation process begins
3. **eNB Reset occurs** (Cause: Group:1 Cause:3)
   - Entire S1 interface is reset
   - ENB-UE context is deleted
   - MME-UE transitions to ECM-IDLE state
4. Authentication process fails (S1 context already removed)
5. GTP Create Session Request (Type 170) times out (twice)
   - Session remains in **incomplete state** (TEIDs not set)
6. UE sends second Attach request
7. `mme_ue_find_by_imsi()` finds old MME-UE context
   - Old context: ECM-IDLE + holding incomplete sessions
8. **Session migration from old to new context**
9. New Attach process **also creates new session**
10. **Multiple sessions exist** → Crash

### Crash Log

```
10/30 17:36:52.473: [mme] ERROR: There should only be one SESSION (../src/mme/nas-path.c:135)
10/30 17:36:52.473: [esm] ERROR: esm_handle_information_response: Expectation `r == OGS_OK' failed.
10/30 17:36:52.473: [esm] FATAL: esm_handle_information_response: Assertion `r != OGS_ERROR' failed.
```

### Root Cause

Assertion failure in `nas-path.c:132-136` which assumes single session:

```c
sess = mme_sess_first(mme_ue);
ogs_assert(sess);
if (mme_sess_next(sess)) {
    ogs_error("There should only be one SESSION");  // Error occurs here
    return OGS_ERROR;
}
```

Sessions were migrated from old context in ECM-IDLE state with incomplete sessions, and new Attach also created a new session, resulting in multiple sessions.

## Fix Details

### 1. Implementation of has_incomplete_sessions() Function

**Location**: `src/mme/mme-context.c` (lines 3911-3975)

Added static function to check if MME-UE has incomplete sessions that should not be migrated.

```c
/**
 * Check if MME-UE has incomplete sessions that should not be migrated
 *
 * A session is considered "incomplete" if any of the following conditions are met:
 * 1. MME-UE is not in EMM-REGISTERED state
 * 2. Session has no PDN context (session->session == NULL)
 * 3. Session has no Bearers
 * 4. Any Bearer has no TEIDs (sgw_s1u_teid or enb_s1u_teid == 0)
 * 5. Any Bearer is not in esm_state_active
 * 6. Session deletion is in progress
 */
static bool has_incomplete_sessions(mme_ue_t *mme_ue)
{
    mme_sess_t *sess = NULL;
    mme_bearer_t *bearer = NULL;

    ogs_assert(mme_ue);

    /* No sessions = not incomplete */
    if (!SESSION_CONTEXT_IS_AVAILABLE(mme_ue)) {
        return false;
    }

    /* EMM state check: if not fully registered, sessions are incomplete */
    if (!OGS_FSM_CHECK(&mme_ue->sm, emm_state_registered)) {
        ogs_debug("[%s] MME-UE not in EMM-REGISTERED state",
                  mme_ue->imsi_bcd);
        return true;
    }

    /* Check each session */
    ogs_list_for_each(&mme_ue->sess_list, sess) {
        /* No PDN context = incomplete */
        if (!sess->session) {
            ogs_debug("[%s] Session without PDN context",
                      mme_ue->imsi_bcd);
            return true;
        }

        /* Deletion in progress = incomplete */
        if (sess->deletion_in_progress) {
            ogs_debug("[%s] Session deletion in progress",
                      mme_ue->imsi_bcd);
            return true;
        }

        /* No Bearers = incomplete */
        if (ogs_list_count(&sess->bearer_list) == 0) {
            ogs_debug("[%s] Session without bearers",
                      mme_ue->imsi_bcd);
            return true;
        }

        /* Check each Bearer */
        ogs_list_for_each(&sess->bearer_list, bearer) {
            /* No TEIDs = incomplete (not established) */
            if (bearer->sgw_s1u_teid == 0 || bearer->enb_s1u_teid == 0) {
                ogs_debug("[%s] Bearer[EBI:%d] without complete TEIDs "
                          "(SGW:%u, eNB:%u)",
                          mme_ue->imsi_bcd, bearer->ebi,
                          bearer->sgw_s1u_teid, bearer->enb_s1u_teid);
                return true;
            }

            /* Bearer not active = incomplete */
            if (!OGS_FSM_CHECK(&bearer->sm, esm_state_active)) {
                ogs_debug("[%s] Bearer[EBI:%d] not in esm_state_active",
                          mme_ue->imsi_bcd, bearer->ebi);
                return true;
            }
        }
    }

    /* All checks passed = all sessions are complete */
    return false;
}
```

### 2. Integration into mme_ue_set_imsi()

**Location**: `src/mme/mme-context.c` (lines 3996-4010)

Added incomplete session check for ECM-IDLE state before ECM-CONNECTED check.

```c
/* Check if OLD mme_ue_t is different with NEW mme_ue_t */
if (ogs_pool_index(&mme_ue_pool, mme_ue) !=
    ogs_pool_index(&mme_ue_pool, old_mme_ue)) {
    ogs_warn("[%s] OLD UE Context Release", mme_ue->imsi_bcd);

    /* New addition: Check for incomplete sessions in ECM-IDLE state */
    if (ECM_IDLE(old_mme_ue) && has_incomplete_sessions(old_mme_ue)) {
        ogs_warn("[%s] OLD MME-UE in ECM-IDLE with incomplete sessions",
                 mme_ue->imsi_bcd);
        ogs_warn("[%s] Cleaning up old context instead of migration",
                 mme_ue->imsi_bcd);

        /* Remove old context - UE starts fresh */
        mme_ue_remove(old_mme_ue);

        /* Set IMSI hash for new UE */
        ogs_hash_set(self.imsi_ue_hash,
                    mme_ue->imsi, mme_ue->imsi_len, mme_ue);
        return OGS_OK;
    }

    if (ECM_CONNECTED(old_mme_ue)) {
        // Existing ECM-CONNECTED processing continues...
```

## Operation Flow

### Before Fix (Crash Occurs)

```
1. eNB Reset occurs
2. Old MME-UE: ECM-IDLE + incomplete sessions (TEIDs not set)
3. New Attach request
4. mme_ue_set_imsi() called
5. ❌ Session migration executed (incomplete sessions to new UE)
6. New session created by Attach process
7. ❌ Multiple sessions exist
8. ❌ FATAL: "There should only be one SESSION"
```

### After Fix (Crash Avoided)

```
1. eNB Reset occurs
2. Old MME-UE: ECM-IDLE + incomplete sessions (TEIDs not set)
3. New Attach request
4. mme_ue_set_imsi() called
5. ✓ has_incomplete_sessions() detects incomplete state
6. ✓ Session migration skipped
7. ✓ Old context cleaned up (mme_ue_remove)
8. ✓ Only new session created by Attach process
9. ✓ Single session → Crash avoided
```

## Expected Benefits

### 1. Crash Prevention
- Prevents FATAL errors due to multiple session issues
- Avoids inappropriate migration of sessions interrupted by eNB Reset, GTP timeout, etc.

### 2. Proper Error Handling
- Detects incomplete sessions and notifies via warning logs
- Prompts UE to start fresh

### 3. Improved Operations Monitoring
- Incomplete session detection enables identification of eNB instability issues
- Detailed state recorded in debug logs

## Check Condition Details

| Condition | Description | Applicable Case |
|-----------|-------------|-----------------|
| Not in EMM-REGISTERED state | UE not fully registered | Authentication failure, Attach interrupted |
| No PDN context | `sess->session == NULL` | Session creation in progress |
| No Bearers | `bearer_list` is empty | Before Bearer establishment |
| TEIDs not set | `sgw_s1u_teid == 0` or `enb_s1u_teid == 0` | **GTP timeout, eNB Reset** |
| Not esm_state_active | Bearer in inactive state | Bearer establishment in progress |
| Deletion in progress | `deletion_in_progress == true` | Session deletion process ongoing |

## Build and Deployment

### Build

```bash
./build.sh
```

### Update Binary

```bash
./update_nf_binary.sh mme
```

### Verification

```bash
# Verify the fix is included in running binary
strings /usr/bin/open5gs-mmed | grep "OLD MME-UE in ECM-IDLE with incomplete sessions"
```

## Testing

### Reproduction Scenario

1. Attach UE
2. Execute eNB Reset during session establishment
3. UE sends Attach request again
4. Verify no crash occurs and new session is established normally

### Expected Log Output

```
[mme] WARN: [441216000000000] OLD MME-UE in ECM-IDLE with incomplete sessions
[mme] WARN: [441216000000000] Cleaning up old context instead of migration
[mme] DEBUG: [441216000000000] Bearer[EBI:5] without complete TEIDs (SGW:0, eNB:0)
```

## Impact Scope

### Modified Files
- `src/mme/mme-context.c`

### Affected Functionality
- MME-UE context migration during IMSI setting
- Old context handling in ECM-IDLE state

### Unaffected Functionality
- Processing in ECM-CONNECTED state (maintains existing behavior)
- Normal migration of completed sessions (maintains existing behavior)

## Related Fixes

This fix is part of comprehensive countermeasures for the following issues:

1. **TAU Request with Unknown GUTI - Tenant Control check** (7a4efa853)
2. **UE context inheritance bugs in mme_ue_set_imsi()** (26b77984e)
3. **MME UE context inheritance fix** (6599486e9)

## References

- Issue: TAU Unknown GUTI issue
- Related commits:
  - `7a4efa853`: Fix TAU Request with Unknown GUTI - Tenant Control check
  - `26b77984e`: Fix UE context inheritance bugs in mme_ue_set_imsi()
  - `84a90e36e`: Add comprehensive documentation for TAU Unknown GUTI fix

## Summary

This fix enables the MME to handle network failures such as eNB Reset gracefully without crashing, allowing UEs to start fresh with new Attach requests even when session establishment is interrupted. This improves system robustness and operational stability.
