# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "ezsp_backend.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ezsp_driver zigbee_mgr zap_common zap_store device_backend zhc_adapter device_shadow esp_timer
)
