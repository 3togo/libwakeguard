/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 libwakeguard contributors */
#include <string.h>
#include <wakeguard/wakeguard.h>
int main(void) {
    wg_options options = wg_default_options();
    wg_error error;
    wg_guard *guard = NULL;
    options.reason = NULL;
    if (wg_acquire(&options, &guard, &error) != WG_INVALID_ARGUMENT || guard != NULL)
        return 1;
    return strstr(error.message, "Invalid") == NULL;
}
