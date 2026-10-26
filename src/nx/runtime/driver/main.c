/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/setup.h"
#include "nx/runtime/driver/driver.h"
#include "nx/runtime/driver/mcp.h"
#include "nx/runtime/entry.h"
#include <stdio.h>
#include <string.h>
/* The Switch driver of a build of the host tools (SNX_TOOLS_ONLY), in the program as entry.c's is in the runtime's
   (src/multicall). The full runtime uses the same driver */
int snx_switch_main(int argc, char** argv) {
    char why[160];
    int result;
    argc = snx_setup_options(argc, argv, why, sizeof why);
    if (argc < 0) {
        fprintf(stderr, "scarletnx: %s\n", why);
        return 2;
    }
    if (argc > 1 && !strcmp(argv[1], "--mcp")) return snx_mcp(argc, argv);
    result = snx_driver(argc, argv);
    if (result >= 0) return result;
    fputs("Pass a dump file, -x, -r or -h. This build contains host tools only.\n", stderr);
    return 2;
}
