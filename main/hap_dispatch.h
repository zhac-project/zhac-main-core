// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "hap_protocol.h"

// One-shot HapFrame builder + send. Collapses ~6 lines of field setup at
// every emit site. Pass `ack_seq=req.seq` for replies that participate in
// ACK tracking; leave it zero for fire-and-forget notifications.
void hap_send(HapMsgType type, const uint8_t* payload,
              uint16_t payload_len, uint8_t flags = 0,
              uint16_t ack_seq = 0);
