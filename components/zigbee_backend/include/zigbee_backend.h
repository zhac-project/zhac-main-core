// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_backend/include/zigbee_backend.h
// Thin adapter: wraps existing zigbee_mgr_*() functions as a DeviceBackend.
// No new logic — pure delegation to zigbee_mgr, zigbee_pool, zap_store.
#pragma once

#include "device_backend.h"

// Returns the singleton Zigbee backend instance.
// Call device_backend_register(zigbee_backend_instance()) at startup.
DeviceBackend* zigbee_backend_instance();

// Convenience: registers the Zigbee backend in the registry.
bool zigbee_backend_register();
