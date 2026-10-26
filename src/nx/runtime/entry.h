/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Switch driver's entry (entry.c, or in a build of the host tools driver/main.c), which scarletnx's main
   (src/multicall) runs under scarletnx's own names and the Switch games' */
#ifndef SNX_ENTRY_H
#define SNX_ENTRY_H

int snx_switch_main(int argc, char** argv); /* the MCP server, the driver's commands, else a game; its exit status */

#endif
