/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's log, scarletnx-console.log (SNX_CONSOLE_LOG) in its state folder, on iOS in logs/ of the app's
   Documents, where stdout and stderr go too (apple.h): what the launcher says, its failures, the warnings it gives as
   it opens and a dump's icon the driver did not give, a line each after the time and the launcher's pid
   (snx_log_lines), so two launchers open at once are told apart. A fatal error also goes to scarletnx-error.log
   beside it. Any thread may write */
#ifndef SNX_GUI_LOG_H
#define SNX_GUI_LOG_H
#include <stddef.h>

/* the folder the logs go to from now on, copied (made by the caller); NULL: none, the lines go to stderr alone */
void gui_log_folder(const char* folder);
/* where a log is, file one of SNX_CONSOLE_LOG and SNX_ERROR_LOG, or "" without a folder */
void gui_log_path(const char* file, char* out, size_t size);
/* a line, as printf formats it, in the log and on stderr (iOS: the log is stderr's file, written once); a text of
   several lines takes a stamp each */
void gui_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
/* a fatal error: in the log and in scarletnx-error.log */
void gui_log_fatal(const char* text);
#endif
