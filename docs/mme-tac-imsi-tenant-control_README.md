# MME TAC-IMSI Tenant Control Feature

## Overview

This patch implements a tenant control mechanism in the MME (Mobility Management Entity) to separate subscribers by TAC (Tracking Area Code) and IMSI ranges. This feature prevents subscribers from one tenant from accessing base stations of another tenant.

## Features

- **TAC-based tenant separation**: Each tenant is associated with specific TAC values
- **IMSI range validation**: Support for both single IMSI and IMSI ranges per tenant
- **Attach/TAU rejection**: Subscribers attempting to connect to wrong tenant TACs are rejected with cause #13 (Roaming not allowed in this tracking area)
- **YAML configuration support**: Flexible configuration format for tenant definitions
- **REST API for dynamic reload**: Reload tenant configuration without MME restart

## Configuration Format

Add the following configuration to your `/etc/open5gs/mme.yaml`:

```yaml
mme:
  tenant_control:
    - tenant_id: "tenant1"
      tac: 1
      allowed_imsi:
        - "441216000000006"                    # Single IMSI
        - "441216000000010-441216000000100"    # IMSI range
    - tenant_id: "tenant2"
      tac: 2
      allowed_imsi:
        - "441216000000001-441216000000005"
        - "441216000000101-441216000000110"
```

## Behavior

- **Tenant1 IMSI (441216000000050) connecting to TAC=2** → Attach/TAU Reject with cause #13
- **Tenant2 IMSI (441216000000003) connecting to TAC=1** → Attach/TAU Reject with cause #13
- **Same tenant IMSI connecting to corresponding TAC** → Normal Attach/TAU processing

## Important Notes

- Tenant control is applied to all attach types (IMSI attach, GUTI attach)
- For GUTI attach without known IMSI, validation occurs after Identity Response
- TAU requests are always validated as they should have IMSI available

## REST API

The MME provides REST API endpoints for managing tenant control:

### GET /tenant_control (Port 8880)
Returns current in-memory tenant control configuration

```bash
curl -X GET http://localhost:8880/tenant_control | jq .
```

**Response format**: JSON with tenant_id, tac, and allowed_imsi arrays

### POST /tenant_control/reload (Port 8880)
Reloads tenant_control section from `/etc/open5gs/mme.yaml`

```bash
# First, manually edit /etc/open5gs/mme.yaml
# Then reload the configuration
curl -X POST http://localhost:8880/tenant_control/reload | jq .
```

**Response includes**:
- Added, removed, and modified tenants
- Detailed changes (TAC changes, IMSI range additions/removals)

### API Implementation Details
- Thread-safe with mutex protection
- API does NOT write to YAML files - operators must manually edit mme.yaml
- Reload function parses only the tenant_control section from YAML

## Installation

1. Apply the patch to your Open5GS source code (based on custom-1.0 branch):
   ```bash
   cd /path/to/open5gs
   git apply mme-tac-imsi-tenant-control.patch
   ```

2. Rebuild the MME:
   ```bash
   meson setup build
   ninja -C build
   sudo ninja -C build install
   ```

3. Configure tenant control in `/etc/open5gs/mme.yaml` as shown above

4. Restart MME:
   ```bash
   sudo systemctl restart open5gs-mmed
   ```

## Modified Files

- `src/mme/mme-context.h` - Added tenant control data structures and mutex
- `src/mme/mme-context.c` - Implemented IMSI range validation functions, YAML parsing, and reload functions
- `src/mme/emm-handler.c` - Added tenant validation to Attach Request (IMSI and GUTI), TAU Request, and Identity Response handlers
- `src/mme/mme-init.c` - Added REST API endpoints using ulfius framework
- `CLAUDE.md` - Updated with feature documentation

## Commits Included

1. `5117d173b` - Implement TAC-IMSI tenant control feature for MME
2. `ac0f060b6` - Fix GUTI attach bypass of tenant control
3. `a3e09d205` - Update CLAUDE.md with GUTI attach tenant control details
4. `16047189a` - feat(mme): Add REST API for dynamic tenant control reload

## Use Cases

- Multi-tenant mobile network deployments
- Separate enterprise customers on shared infrastructure
- Prevent unauthorized roaming between network segments
- Geographic or organizational network partitioning

## Testing

Test the feature by:
1. Configuring multiple tenants with different TACs
2. Attempting to attach subscribers from one tenant to another tenant's TAC
3. Verifying attach rejection with cause #13
4. Testing normal attach when subscriber uses correct TAC
5. Testing REST API reload functionality

## License

This patch follows the same license as Open5GS (AGPL-3.0).
