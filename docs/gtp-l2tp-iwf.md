# GTP↔L2TP Interworking (Path A) – Embedded LAC in PGW/SMF

This document outlines a practical design to integrate an L2TP LAC into the PGW/SMF so that an external LNS/PPP assigns the UE IP address, which the PGW/SMF returns as PAA and uses for the datapath.

## Goal
- For selected APNs/DNNs, delegate UE IP assignment to an external L2TP LNS (via PPP/IPCP/IPv6CP).
- Maintain 3GPP call flow: PAA must be present in Create Session Response.
- Route user-plane UL traffic into the PPP tunnel and re-encapsulate DL traffic into GTP-U.

## High-level Architecture
- **GTP side (existing in Open5GS)**
  - Control-plane: S5/S8 GTP-C in `src/smf/s5c-handler.c`.
  - UE IP allocation path: `smf_sess_set_ue_ip()` → `ogs_pfcp_ue_ip_alloc()`.
  - User-plane: GTP-U, default Linux TUN `ogstun`.
- **L2TP/PPP side (new)**
  - L2TP LAC + PPP (pppd with pppol2tp plugin).
  - One `pppX` interface per UE; IPCP/IPv6CP returns the UE IP (and possibly IPv6 prefix).
  - Optional: RADIUS at the LNS to control per-UE policy/IP.

## Control-Plane Flow
1. PGW/SMF receives Create Session for an APN configured to use the L2TP backend.
2. PGW starts an L2TP/PPP session (LAC) towards the configured LNS and blocks (bounded) until PPP negotiates IP (IPCP/IPv6CP).
3. PGW sets PAA to the PPP-assigned IP (and IPv6 if present) and completes the Create Session Response.

## Data-Plane Flow
- **Uplink**: Decapsulated UE IP packets (src = UE_IP) are policy-routed to the per-UE `pppX` interface.
- **Downlink**: Packets from `pppX` destined to UE_IP are routed to `ogstun`, then GTP-U encapsulates them towards the RAN/SGW.

## Open5GS Touch Points
- Preferred hook: `lib/pfcp/context.c` in `ogs_pfcp_ue_ip_alloc()` so the L2TP-backed APNs replace local IP pool logic.
- Alternative: `src/smf/context.c` in `smf_sess_set_ue_ip()` before calling the allocator.

## Configuration (per APN/DNN)
Add a PGW/SMF config section (example pseudo-YAML) marking APNs that use L2TP:

```yaml
smf:
  session:
    - dnn: internet-l2tp
      l2tp:
        lns: 203.0.113.10
        port: 1701
        secret: "<optional-psk>"
        ppp:
          user: "ue@realm"
          pass: "<secret>"
          mtu: 1460
          keepalive: true
      policy_routing: true
```

## Allocator Hook (PoC Pseudocode)
Embed an L2TP-backed allocator returning a "static" IP object so Open5GS does not free it from the pool.

```c
// Example interface for the embedded L2TP/PPP controller
typedef struct {
    int has_ipv4;
    uint32_t ipv4;      // in network byte order
    int has_ipv6;
    uint8_t ipv6[16];
    char ifname[IFNAMSIZ]; // ppp interface name
} l2tp_ppp_ip_t;

int l2tp_ppp_start(const char *dnn, const char *imsi, l2tp_ppp_ip_t *out, int timeout_ms);
int l2tp_ppp_stop(const char *ifname);
void install_ue_policy_routing(const l2tp_ppp_ip_t *info);
void remove_ue_policy_routing(const l2tp_ppp_ip_t *info);

// lib/pfcp/context.c : inside ogs_pfcp_ue_ip_alloc()
ogs_pfcp_ue_ip_t *ogs_pfcp_ue_ip_alloc(uint8_t *cause_value, int family,
                                       const char *dnn, uint8_t *addr)
{
    if (apn_uses_l2tp_backend(dnn)) {
        l2tp_ppp_ip_t got = {0};
        if (l2tp_ppp_start(dnn, current_imsi(), &got, 10000) != 0) {
            *cause_value = OGS_PFCP_CAUSE_NO_RESOURCES_AVAILABLE;
            return NULL;
        }
        ogs_pfcp_ue_ip_t *ue_ip = ogs_calloc(1, sizeof(*ue_ip));
        ue_ip->static_ip = true; // managed externally by PPP/L2TP
        ue_ip->subnet = ogs_pfcp_find_subnet_by_dnn(family, dnn);
        if (family == AF_INET && got.has_ipv4) {
            memcpy(ue_ip->addr, &got.ipv4, 4);
        } else if (family == AF_INET6 && got.has_ipv6) {
            memcpy(ue_ip->addr, got.ipv6, 16);
        } else {
            ogs_free(ue_ip);
            *cause_value = OGS_PFCP_CAUSE_NO_RESOURCES_AVAILABLE;
            return NULL;
        }
        install_ue_policy_routing(&got);
        return ue_ip;
    }

    // Fallback to default behavior (static PAA or pool)
    ...
}
```

## Starting L2TP/PPP (pppd + pppol2tp)
Use kernel L2TP with pppd plugin; one process per UE for PoC.

```bash
pppd \
  plugin pppol2tp.so pppol2tp \
  l2tpopt lns=203.0.113.10 l2spec=static tid=auto sid=auto \
  user "ue@realm" password "secret" \
  noauth usepeerdns ipcp-accept-local ipcp-accept-remote \
  persist maxfail 0 \
  mtu 1460 mru 1460 \
  ifname "ppp-imsi-<IMSI>" \
  updetach nodetach ipparam "IMSI=<IMSI>,APN=<APN>"
```

## Returning Assigned IP to PGW/SMF
Use pppd `ip-up` hook to submit the assigned IP back to the PGW/SMF process via a UNIX socket.

```sh
# /etc/ppp/ip-up.d/ogs-assign (snippet)
IMSI=$(echo "$PPP_IPPARAM" | sed -n 's/.*IMSI=\([^,]*\).*/\1/p')
APN=$(echo "$PPP_IPPARAM" | sed -n 's/.*APN=\([^,]*\).*/\1/p')
IFACE="$1"
LOCAL="$4"   # peer’s view
REMOTE="$5"  # assigned to this side (UE IP)
printf '%s %s %s %s\n' "$IMSI" "$APN" "$IFACE" "$REMOTE" \
  | socat - UNIX-CONNECT:/run/ogs-gtp-l2tp.sock
```

The PGW-side `l2tp_ppp_start()` blocks on the socket until it receives the tuple (IMSI/APN/ifname/IP) or times out.

## Per-UE Policy Routing (UL)
Route UE traffic to the correct `pppX` device by source IP.

```bash
TABLE=$((10000 + UE_INDEX))
ip rule add from UE_IP lookup $TABLE
ip route add default dev pppX table $TABLE
```

## Downlink Path
Maintain routes to UE_IP via `ogstun` so Open5GS encapsulates back to GTP-U (already the default). If needed, add:

```bash
ip route add UE_IP dev ogstun
```

## Teardown
On session release:
- Terminate PPP for the UE (`l2tp_ppp_stop()` / kill pppd PID for ifname).
- Remove `ip rule` and `ip route` for the UE.
- Free tracking state in the controller.

## Limitations and Notes
- Block-and-wait for PPP must be bounded (e.g., 10s); fail Create Session if LNS doesn’t assign in time.
- Process-per-UE is heavy; for scale, embed or multiplex the L2TP/PPP control plane.
- IPv6: handle IPv6CP; PAA may need a prefix or single /128 depending on LNS behavior.
- PCC/QoS/charging interworking to PPP is limited in the PoC.
- Securely store credentials; consider RADIUS on LNS for dynamic policy/IP.

## Testing Checklist
- Create Session returns PAA equal to PPP-assigned IP.
- UL traffic from UE exits via `pppX` and reaches the internet through LNS.
- DL traffic from internet to UE_IP arrives on `pppX`, routes to `ogstun`, and is GTP-U encapsulated towards RAN.
- On detach, PPP terminates and routes/rules are cleaned.

## Summary
- For designated APNs, replace local IP pool with an L2TP-backed allocator.
- Obtain UE IP from LNS/PPP before completing Create Session, set PAA, and install per-UE routing to `pppX`.
- Keep `ogstun` for the DL path so Open5GS continues standard GTP-U encapsulation.
