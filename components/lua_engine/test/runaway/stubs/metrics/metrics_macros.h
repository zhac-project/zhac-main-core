// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim: the metric-id token is discarded, so no metric symbols are needed.
#pragma once
#define _METRIC_VALUE(id, v)       ((void)0)
#define _METRIC_COUNTER_INC(id, n) ((void)0)
#define _METRIC_TIMER_SCOPE(id)
#define _METRIC_INC(id)
#define _METRIC_ADD(id, n)
#define _METRIC_SET(id, n)
