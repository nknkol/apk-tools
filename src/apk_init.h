/* apk_init.h - initialization helpers for default hapkg root
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once
#include <stdbool.h>
#include "apk_context.h"

/* Prepare ~/.hapkg sysroot/runtime with default repos and keys when missing. */
int apk_auto_init(struct apk_ctx *ac, bool *created_db);
