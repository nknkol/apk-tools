/* apk_shim.h - helper for installing runtime shim wrappers
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once
#include "apk_context.h"

int apk_shim_install(struct apk_ctx *ac, const char *relative_path);
int apk_shim_remove(struct apk_ctx *ac, const char *relative_path);
