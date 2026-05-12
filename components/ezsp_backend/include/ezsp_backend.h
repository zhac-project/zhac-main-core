// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/ezsp_backend/include/ezsp_backend.h
// EZSP DeviceBackend implementation for Silicon Labs EFR32 NCP.
// Wraps ezsp_driver to implement the DeviceBackend interface.
#pragma once

#include "device_backend.h"

// Returns the singleton EZSP backend instance.
DeviceBackend* ezsp_backend_instance();

// Convenience: registers the EZSP backend in the registry.
bool ezsp_backend_register();
