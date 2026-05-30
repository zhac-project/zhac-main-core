# Install-code / APS link-key hardening (FINDINGS.md F7)

Status: **planned — requires on-hardware validation, not yet implemented.**

## Problem

The coordinator commissions devices under the well-known global Trust-Center
(TC) link key `ZigBeeAlliance09` (`zigbee_mgr.cpp` `commission()` writes
`PRECFGKEYS_ENABLE=1` + `PRECFGKEY`). The network key is delivered to a joining
device encrypted under that *public* key, so an attacker sniffing the join
exchange can decrypt and recover the network key — compromising the whole mesh.

Per-device **install codes** close this: each device ships with a printed
install code; the coordinator derives a unique APS link key from it and installs
it in the TC link-key table *before* the device joins, so the network-key
transport is encrypted under a secret only that device and the coordinator know.

## Why this is a follow-up, not a same-session fix

It is a feature addition that touches the security-critical commissioning path
and cannot be validated without a real install-code device + RF capture. A
subtly wrong derivation or NV layout silently fails joins or gives false
security. It must land with hardware testing.

## Implementation outline

1. **Derivation primitive** (host-testable, do first):
   - `bool install_code_to_link_key(const uint8_t* code, size_t code_len, uint8_t out_key[16])`
   - Validate the trailing CRC-16 of the install code (8/10/14/16/18-byte forms).
   - Derive the key via **AES-MMO-128** (Matyas–Meyer–Oseas) hash of the code,
     per Zigbee R21 spec §. Verify against the spec test vector:
     code `83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5`
     → link key `66B6 9000 7359 7388 6855 95B6 4B62 7F71` (CRC-checked).
   - Land in `zhac-components` with a host/ctest unit test (this is the part
     most prone to error — prove it in isolation).

2. **NV / TC link-key table** (ZNP path): add the device's `(ieee, link_key)`
   to the Z-Stack TC link-key table via the appropriate `ZDSecMgr` / NV MT
   command before opening permit-join. (EZSP path: `addTransientLinkKey` /
   `setPolicy` — but EZSP is gated experimental, F14.)

3. **Entry API**: REST `POST /api/zigbee/install-code {ieee, code}` + WS
   `zigbee.install_code`, auth-gated (F1), validating CRC before accepting.
   SPA: a field on the Devices/add flow to paste the printed code.

4. **Policy**: keep global-key join as an explicit, documented opt-in
   ("distributed security" / legacy devices); default to requiring an install
   code when one is registered for the joining IEEE.

5. **Validation**: pair a real install-code device (e.g. many Bosch / Develco /
   metering devices), confirm join succeeds with the code and fails/rekeys
   without it; sniff to confirm the network key is no longer recoverable.

## Interim mitigation (in place)

`commission()` carries a SECURITY comment documenting the threat; operators are
advised to join devices in a trusted RF environment until install codes ship.
