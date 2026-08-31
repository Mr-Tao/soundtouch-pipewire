/* SPDX-License-Identifier: MIT */
#pragma once

#include "direct-pw-context-v2.h"

void stpw_direct_pw_context_v2_test_core_error(
    StpwDirectPwContextV2 *self, guint32 id, gint result,
    const gchar *message);
