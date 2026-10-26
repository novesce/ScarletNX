/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA driver's command line (main.c): scarletnx-agb's own program, or a name of scarletnx's (src/multicall) */
#ifndef SNX_AGB_MAIN_H
#define SNX_AGB_MAIN_H

#include <stdbool.h>

int snx_agb_main(int argc, char** argv); /* recompiles and plays the ROM the arguments name; its exit status */
/* Whether the program started as argv0 is a GBA game's copy of the runtime: named after the game, beside its module
   <name>_code.so, and its folder's <name>.ini saying system=gba. A GBA game is named after its ROM's file, any name */
bool snx_agb_game_name(const char* argv0);

#endif
