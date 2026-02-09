# TAC-based Network Name Feature for Open5GS MME

## Overview

This feature allows MME to send different network names (PLMN name/SPN) to UEs based on their Tracking Area Code (TAC). This enables tenant-specific branding where different base stations (with different TACs) can display different operator names to users.

## Problem

Previously, Open5GS MME could only configure a single global network name that would be sent to all UEs regardless of their location or tenant. In multi-tenant environments where different operators share infrastructure, each tenant wants their own branding displayed on UE devices.

## Solution

Extended the existing `tenant_control` configuration in MME to support TAC-specific network names. When a UE completes attachment, MME retrieves the network name based on the UE's TAC and sends it via EMM Information message.

## Implementation Details

### Modified Files

1. **src/mme/mme-context.h**
   - Added `full_name` and `short_name` fields to `tenant_control` structure
   - Added `mme_get_network_name_for_tac()` function declaration

2. **src/mme/mme-context.c**
   - Extended YAML parser to read `network_name` section in `tenant_control`
   - Implemented `mme_get_network_name_for_tac()` function with fallback to global configuration
   - Thread-safe access using existing `tenant_control_mutex`

3. **src/mme/emm-handler.c**
   - Modified `emm_handle_attach_complete()` to retrieve TAC-specific network name
   - Replaced direct access to `mme_self()->full_name/short_name` with `mme_get_network_name_for_tac()`

4. **update_nf_binary.sh**
   - Fixed binary deployment path from `install/bin/` to `build/src/${NF}/`

### Configuration Format

```yaml
mme:
  tenant_control:
    - tenant_id: "tenant1"
      tac: 1
      network_name:
        full: "Operator A Wireless"
        short: "Op-A"
      allowed_imsi:
        - "441216000000001-441216000001000"

    - tenant_id: "tenant2"
      tac: 2
      network_name:
        full: "Reseller's tenant"
        short: "Reseller"
      allowed_imsi:
        - "441216000000001-441216000001000"

  # Global fallback (used when TAC has no specific network_name configured)
  network_name:
    full: TestBed-Eureka
    short: TB-Eureka
```

### Behavior

1. **TAC-specific name configured**: Uses the network name configured for that TAC
2. **TAC matched but no network_name**: Falls back to global `mme.network_name`
3. **TAC not in tenant_control**: Falls back to global `mme.network_name`

### Network Name Encoding

Network names are encoded in UCS-2 format (similar to global network name configuration):
- Each ASCII character is converted to 2 bytes (0x00 + character)
- Length field includes encoding byte (hence length = string_length * 2 + 1)
- Coding scheme is set to 1 (UCS-2)

Example:
- Input: "Reseller's tenant" (17 characters)
- Encoded length: 17 * 2 + 1 = 35 bytes

## Testing

### Verification Steps

1. **Configure TAC-specific network names** in `/etc/open5gs/mme.yaml`
2. **Restart MME**: `systemctl restart open5gs-mmed`
3. **Attach UE** to base station with configured TAC
4. **Check MME logs**:
   ```
   Found tenant for TAC[2], full_name.length=35
   Using TAC-specific network name for TAC[2]
   Added full network name to EMM Information (length=35)
   Added short network name to EMM Information (length=17)
   ```

5. **Verify with Wireshark**:
   - Filter: `nas_eps.nas_msg_emm_type == 0x61` (EMM Information)
   - Look for message after "Attach Complete"
   - Check "Full name for network" and "Short name for network" fields

6. **Check UE display**: Network name should appear in UE settings/status bar

### Test Results

- ✅ TAC-based network name retrieval working correctly
- ✅ Network name added to EMM Information message
- ✅ Fallback to global configuration working
- ✅ Thread-safe access with mutex
- ✅ Compatible with existing tenant control features

## Migration Notes

### For Existing Deployments

1. **Backward Compatible**: If no `network_name` is configured in `tenant_control`, the existing global `mme.network_name` is used
2. **No Breaking Changes**: Existing configurations continue to work without modification
3. **Optional Feature**: Add `network_name` to `tenant_control` only when needed

### Recommended Configuration

For multi-tenant deployments:
```yaml
mme:
  # Keep global network_name as fallback
  network_name:
    full: "Default Operator"
    short: "Default"

  tenant_control:
    - tenant_id: "tenant1"
      tac: 1
      network_name:
        full: "Tenant 1 Network"
        short: "T1-Net"
      allowed_imsi:
        - "IMSI-RANGE-1"

    - tenant_id: "tenant2"
      tac: 2
      network_name:
        full: "Tenant 2 Network"
        short: "T2-Net"
      allowed_imsi:
        - "IMSI-RANGE-2"
```

## Known Limitations

1. Network name change requires MME restart (no dynamic reload via REST API)
2. Network name is sent only during:
   - Initial attach (after Attach Complete)
   - Not re-sent during TAU unless EMM context is refreshed
3. Maximum network name length limited by NAS protocol (up to 255 characters)

## Future Enhancements

Possible improvements:
- Add REST API endpoint to reload network name configuration dynamically
- Support network name change notification to already-attached UEs
- Add per-PLMN network name configuration
- Support additional encoding schemes (7-bit GSM, etc.)

## Debug Logging

Enable debug logging to troubleshoot:

```yaml
logger:
  level: debug
```

Key log messages:
```
[mme] INFO: Found tenant for TAC[X], full_name.length=Y
[mme] INFO: Using TAC-specific network name for TAC[X]
[emm] INFO: Network name retrieval for TAC[X]: full_name=..., full_name->length=Y
[emm] INFO: Added full network name to EMM Information (length=Y)
[emm] INFO: Added short network name to EMM Information (length=Z)
```

## References

- 3GPP TS 24.301 Section 8.2.12: EMM information
- 3GPP TS 24.008 Section 10.5.3.5a: Network name
- Open5GS MME tenant control documentation

---

**Implementation Date**: 2025-10-10
**Branch**: tac-based-network-name
**Tested with**: Open5GS v2.7.2-77-gda1804b+
