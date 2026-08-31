/* SPDX-License-Identifier: MIT */
#pragma once

#include "direct-output-v2.h"

G_BEGIN_DECLS

/* Private synchronization seam for the isolated production-path test. */
gint stpw_direct_output_v2_test_get_queued_route_serial(
    StpwDirectOutputV2 *self);

G_END_DECLS
