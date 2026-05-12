# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_build_get_property(target IDF_TARGET)

if(target STREQUAL "esp32s3")
    # S3 uses hap_master (SPI master), not hap_slave — skip to avoid
    # pulling P4-specific GPIOs (e.g. GPIO_NUM_22) into the S3 build.
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "hap_slave.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hap_protocol zap_common esp_driver_spi esp_driver_gpio freertos
)
