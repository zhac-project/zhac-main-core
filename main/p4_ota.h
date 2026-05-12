// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "hap_protocol.h"

// OTA chunk streaming (S3→P4 firmware update for P4 itself)
void handle_ota_chunk(const HapFrame& f);
void handle_ota_checkpoint_req(const HapFrame& req);
