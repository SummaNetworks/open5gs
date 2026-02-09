# SGWC Delete Indirect Tunnel Crash Fix

## Issue Summary

**Date**: 2026-01-26
**Branch**: `2-crash-issue`
**Severity**: Critical (Process Crash)

### Crash Log

```
01/26 18:11:03.567: [sgwc] FATAL: sgwc_s11_handle_delete_indirect_data_forwarding_tunnel_request:
Assertion `OGS_OK == sgwc_pfcp_send_session_modification_request( sess, s11_xact->id, gtpbuf,
OGS_PFCP_MODIFY_INDIRECT| OGS_PFCP_MODIFY_REMOVE)' failed. (../src/sgwc/s11-handler.c:1520)
01/26 18:11:03.568: [core] FATAL: backtrace() returned 8 addresses (../lib/core/ogs-abort.c:37)
```

## Root Cause Analysis

### Problem Location

**File**: `src/sgwc/s11-handler.c:1518-1524`

```c
ogs_list_for_each(&sgwc_ue->sess_list, sess) {
    ogs_assert(OGS_OK ==
        sgwc_pfcp_send_session_modification_request(
            sess, s11_xact->id, gtpbuf,
            OGS_PFCP_MODIFY_INDIRECT| OGS_PFCP_MODIFY_REMOVE));
}
```

### Why `sgwc_pfcp_send_session_modification_request()` Can Fail

The function can return `OGS_ERROR` for several recoverable reasons:

| Location | Failure Point | Cause |
|----------|---------------|-------|
| pfcp-path.c:361-364 | `ogs_pfcp_xact_local_create()` | PFCP transaction pool exhaustion |
| pfcp-path.c:369-374 | `ogs_pkbuf_copy()` | Memory shortage (packet buffer copy failure) |
| pfcp-path.c:251-255 | `sgwc_sxa_build_bearer_to_modify_list()` | PFCP payload build failure |
| pfcp-path.c:257-261 | `ogs_pfcp_xact_update_tx()` | PFCP transaction update failure |

### Call Flow

```
sgwc_pfcp_send_session_modification_request()
├── ogs_pfcp_xact_local_create() failure → OGS_ERROR
├── ogs_pkbuf_copy() failure → OGS_ERROR
└── sgwc_pfcp_send_bearer_to_modify_list()
    ├── sgwc_sxa_build_bearer_to_modify_list() failure → OGS_ERROR
    └── ogs_pfcp_xact_update_tx() failure → OGS_ERROR
```

**These are all temporary resource issues (memory shortage, pool exhaustion) and are recoverable errors.**

### The Problem with `ogs_assert()`

- `ogs_assert()` is a **debug macro** that terminates the process when the condition is false
- Should be used for conditions that "should never happen"
- However, the above failures **can happen due to temporary resource issues**
- Using assert here is inappropriate for production environments

## Existing Reference Pattern

The Release Access Bearers Handler (lines 1271-1278) already handles this correctly:

```c
ogs_list_for_each(&sgwc_ue->sess_list, sess) {
    int rv = sgwc_pfcp_send_session_modification_request(
            sess, s11_xact->id, gtpbuf,
            OGS_PFCP_MODIFY_DL_ONLY|OGS_PFCP_MODIFY_DEACTIVATE);
    if (rv != OGS_OK) {
        ogs_error("Failed to send PFCP session modification request...");
        continue;  // Continue with other sessions
    }
}
```

## Fix Applied

### Before (Crashes)

```c
ogs_list_for_each(&sgwc_ue->sess_list, sess) {
    ogs_assert(OGS_OK ==
        sgwc_pfcp_send_session_modification_request(
            sess, s11_xact->id, gtpbuf,
            OGS_PFCP_MODIFY_INDIRECT| OGS_PFCP_MODIFY_REMOVE));
}
```

### After (Graceful Handling)

```c
ogs_list_for_each(&sgwc_ue->sess_list, sess) {
    int rv = sgwc_pfcp_send_session_modification_request(
            sess, s11_xact->id, gtpbuf,
            OGS_PFCP_MODIFY_INDIRECT| OGS_PFCP_MODIFY_REMOVE);
    if (rv != OGS_OK) {
        ogs_error("Failed to send PFCP session modification request "
                "for Delete Indirect Data Forwarding Tunnel");
        continue;
    }
}
```

### Key Changes

| Item | Description |
|------|-------------|
| Error Logging | Log error message for debugging |
| Continue Processing | Skip failed session and continue with others |
| Service Continuity | Process does not crash on temporary resource issues |
| Consistency | Matches existing pattern in Release Access Bearers Handler |

## Files Modified

| File | Change |
|------|--------|
| `src/sgwc/s11-handler.c` | Replace `ogs_assert()` with error check and continue |

## Build and Deploy

```bash
# Build
ninja -C build

# Update SGWC binary
cp build/src/sgwc/open5gs-sgwcd install/bin/
sudo ./update_nf_binary.sh sgwc

# Verify binary
md5sum build/src/sgwc/open5gs-sgwcd /usr/bin/open5gs-sgwcd
```

## Verification

After fix, when PFCP send fails, SGWC will:
1. Log error message instead of crashing
2. Continue processing other sessions
3. Remain operational

Expected log output on failure:
```
[sgwc] ERROR: Failed to send PFCP session modification request for Delete Indirect Data Forwarding Tunnel
```
