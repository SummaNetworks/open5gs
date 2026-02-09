# MME Handover Crash Fix - source_ue NULL Assertion

## Issue Summary

**Date**: 2026-01-26
**Branch**: `2-crash-issue`
**Severity**: Critical (Process Crash)

### Crash Log

```
01/26 08:23:59.580: [mme] FATAL: mme_s11_handle_create_indirect_data_forwarding_tunnel_response: Assertion `source_ue' failed. (../src/mme/mme-s11-handler.c:1906)
01/26 08:23:59.580: [core] FATAL: backtrace() returned 8 addresses (../lib/core/ogs-abort.c:37)
```

## Root Cause Analysis

### Problem Location

**File**: `src/mme/mme-s11-handler.c:1905-1906`

```c
source_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
ogs_assert(source_ue);  // ← Crashes when NULL
```

### Race Condition During Handover

The crash occurs due to a race condition between GTP-C and S1AP processing during S1/X2 handover:

```
┌──────────┐      ┌──────────┐      ┌──────────┐      ┌──────────┐
│ Source   │      │   MME    │      │   SGW    │      │ Target   │
│   eNB    │      │          │      │          │      │   eNB    │
└────┬─────┘      └────┬─────┘      └────┬─────┘      └────┬─────┘
     │                 │                 │                 │
     │ Handover Required                 │                 │
     │────────────────>│                 │                 │
     │                 │ Create Indirect Tunnel Req        │
     │                 │────────────────>│                 │
     │                 │                 │                 │
     │ UE Context Release / HO Cancel    │ (processing...)│
     │────────────────>│                 │                 │
     │                 │ (S1 context     │                 │
     │                 │  removed)       │                 │
     │                 │                 │                 │
     │                 │ Create Indirect Tunnel Rsp        │
     │                 │<────────────────│                 │
     │                 │                 │                 │
     │                 │ source_ue = NULL → CRASH!         │
```

### Timeline

1. MME sends Create Indirect Data Forwarding Tunnel Request to SGW
2. Source eNB sends UE CONTEXT RELEASE or HO CANCEL (S1 context removed)
3. SGW responds with Create Indirect Data Forwarding Tunnel Response
4. MME tries to find source_ue → NULL → **CRASH**

## Fix Applied

### Before (Crashes)

```c
source_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
ogs_assert(source_ue);

r = s1ap_send_handover_command(source_ue);
ogs_expect(r == OGS_OK);
ogs_assert(r != OGS_ERROR);
```

### After (Graceful Handling)

```c
source_ue = enb_ue_find_by_id(mme_ue->enb_ue_id);
if (!source_ue) {
    ogs_error("[%s] Source ENB-S1 context has already been removed",
            mme_ue->imsi_bcd);
    mme_ue_clear_indirect_tunnel(mme_ue);
    return;
}

r = s1ap_send_handover_command(source_ue);
ogs_expect(r == OGS_OK);
ogs_assert(r != OGS_ERROR);
```

### Key Changes

| Item | Description |
|------|-------------|
| Error Logging | Include IMSI for easier debugging |
| Cleanup | Call `mme_ue_clear_indirect_tunnel()` to clean up indirect tunnel state |
| Graceful Return | Return instead of crashing |
| Consistency | Matches existing error handling pattern in same file (lines 1826-1830, 1844-1848, 1858-1862) |

## How to Reproduce

### Prerequisites
- Environment with 2+ eNBs connected
- UE with active session

### Method 1: Disconnect eNB During Handover

```bash
# While UE is connected to source eNB:
# 1. Trigger handover (move UE)
# 2. During handover processing, disconnect source eNB's SCTP connection
sudo iptables -A INPUT -p sctp --sport 36412 -s <source_enb_ip> -j DROP
```

### Method 2: Use Simulator (srsRAN/OAI)

1. Start 2 eNBs
2. Connect UE to source eNB
3. Trigger handover
4. Stop source eNB or send HO Cancel during handover

### Verification After Fix

After fix, the same scenario should produce this log instead of crash:
```
[mme] ERROR: [IMSI] Source ENB-S1 context has already been removed
```

## Files Modified

| File | Change |
|------|--------|
| `src/mme/mme-s11-handler.c` | Replace `ogs_assert(source_ue)` with NULL check and graceful handling |

## Build and Deploy

```bash
# Build
ninja -C build

# Update MME binary
cp build/src/mme/open5gs-mmed install/bin/
sudo ./update_nf_binary.sh mme
```
