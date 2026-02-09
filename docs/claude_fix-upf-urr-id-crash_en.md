# UPF URR ID=16 Crash Fix

## Problem Overview
Fixed a critical bug where UPF crashes when using URR ID=16. This issue was caused by array out-of-bounds access leading to memory corruption and Segmentation Fault during session deletion.

## Root Cause

### ID Range Mismatch
- **SMF**: Generates and sends URR IDs in range 1-16
- **UPF**: Manages with array `urr_acc[16]` (indices 0-15)
- **Problem**: Direct access via `sess->urr_acc[urr->id]` causes out-of-bounds for ID=16

### Memory Corruption Details
```c
// src/upf/context.h (before fix)
struct upf_sess_s {
    upf_sess_urr_acc_t urr_acc[16];  // Indices 0-15
    char *apn_dnn;                    // ← Corrupted when ID=16
}
```

When accessing with URR ID=16:
1. `sess->urr_acc[16]` overwrites `apn_dnn` pointer area
2. Pointer value corrupted with each counter update
3. Session Report Request contains abnormal values (future dates, near UINT64_MAX)
4. Crash on session deletion when calling `ogs_free(sess->apn_dnn)`

## Fix Implementation

### 1. Array Size Extension
```c
// src/upf/context.h:125
// Before
upf_sess_urr_acc_t urr_acc[OGS_MAX_NUM_OF_URR];     // 16 elements

// After
upf_sess_urr_acc_t urr_acc[OGS_MAX_NUM_OF_URR + 1]; // 17 elements
```
- Index 0 unused (URR IDs start from 1)
- Indices 1-16 correspond to IDs 1-16

### 2. Range Check Addition
Added URR ID validation to all following functions:
- `upf_sess_urr_acc_add()` (line 685)
- `upf_sess_urr_acc_fill_usage_report()` (line 732)
- `upf_sess_urr_acc_snapshot()` (line 831)
- `upf_sess_urr_acc_timers_setup()` (line 847)
- `upf_sess_urr_acc_timers_start()` (line 862)
- `upf_sess_urr_acc_timers_stop()` (line 877)

```c
// Added range check
if (urr->id < 1 || urr->id > OGS_MAX_NUM_OF_URR) {
    ogs_error("Invalid URR ID %d", urr->id);
    return;
}
```

## Impact Scope

### Occurrence Conditions
- Cases using 16 URRs
- Especially Gy charging integration and IMS/VoLTE environments
- Sessions with multiple QoS flows/bearers

### Symptoms
- Sudden UPF restart
- Abnormal values in Session Report Request
- Crash during session deletion
- Unexpected behavior due to memory corruption

## Test Results

### Build and Deploy
```bash
# Execute build
./build.sh

# Update UPF binary
./update_nf_binary.sh upf

# Check service status
systemctl status open5gs-upfd
```

### Verification
✅ Build successful
✅ UPF service started normally
✅ PFCP communication normal (SMF heartbeat confirmed)
✅ No error logs

## Security Considerations

This issue included the following security risks:
- **Memory Corruption**: Possibility of arbitrary code execution
- **Information Leak**: Memory contents sent externally in Session Report
- **DoS Attack**: Service disruption by intentional URR ID=16 usage

## Recommendations

### Immediate Application
This fix has **highest urgency**. URR ID=16 can occur in normal operation, affecting all Open5GS users.

### Additional Verification
```bash
# Check URR ID=16 usage
tcpdump -i any -w pfcp.pcap 'port 8805'
tshark -r pfcp.pcap -Y "pfcp.urr_id == 16"

# Log monitoring
tail -f /var/log/open5gs/upf.log | grep -E "(URR|Invalid)"
```

### Future Improvements
1. Extend URR limit to 32 or 64
2. Migrate to dynamic memory management
3. Unify ID management between SMF and UPF

## Related Files
- `src/upf/context.h` - Array declaration
- `src/upf/context.c` - URR accounting processing
- `src/upf/n4-build.c` - Usage Report generation
- `lib/pfcp/context.c` - URR ID allocation

## Commit Information
- Branch: `fix-upf-urr-id-crash`
- Base Branch: `custom-1.0`
- Fix Scope: UPF only (no SMF changes required)

## Summary
By extending the array to 17 elements and adding range checks, we prevented memory corruption and crashes when using URR ID=16. This fix enables safe usage of up to 16 URRs.