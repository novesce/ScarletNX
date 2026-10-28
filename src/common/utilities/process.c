/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static volatile LONG interrupted;
static BOOL WINAPI stage_interrupt(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&interrupted, 1);
    return TRUE;
}
static wchar_t* stage_wide(const char* text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t* out;
    if (!n) return NULL;
    out = (wchar_t*)snx_alloc((size_t)n * sizeof(*out));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, n);
    return out;
}
/* Quote argv according to the Windows CRT rules, never through a shell. */
static wchar_t* stage_command(const char* const* argv) {
    size_t cap = 1, slashes, n = 0;
    for (size_t i = 0; argv[i]; ++i) cap += 2 * strlen(argv[i]) + 4;
    char* text = (char*)snx_alloc(cap);
    for (size_t i = 0; argv[i]; ++i) {
        if (i) text[n++] = ' ';
        text[n++] = '"';
        slashes = 0;
        for (size_t j = 0; argv[i][j]; ++j) {
            if (argv[i][j] == '\\') {
                ++slashes;
                continue;
            }
            if (argv[i][j] == '"') slashes = slashes * 2 + 1;
            while (slashes) {
                text[n++] = '\\';
                --slashes;
            }
            text[n++] = argv[i][j];
        }
        slashes *= 2;
        while (slashes) {
            text[n++] = '\\';
            --slashes;
        }
        text[n++] = '"';
    }
    text[n] = 0;
    wchar_t* out = stage_wide(text);
    free(text);
    return out;
}
static int run_stage(const char* const* argv, const char* cwd, const char* stage, int debugger) {
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    HANDLE job;
    wchar_t *command = stage_command(argv), *directory = cwd ? stage_wide(cwd) : NULL;
    DWORD code = 1, error = 0, waited;
    int rc = 1, handler = 0;
    (void)debugger;
    startup.cb = sizeof(startup);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    job = CreateJobObjectW(NULL, NULL);
    interrupted = 0;
    if (!command || (cwd && !directory) || !job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        goto failed;
    handler = SetConsoleCtrlHandler(stage_interrupt, TRUE);
    if (!handler) goto failed;
    /* Assign the suspended child before it can create compilers or extraction subprocesses. */
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, NULL, directory,
                        &startup, &process))
        goto failed;
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == (DWORD)-1) {
        error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        goto failed;
    }
    for (;;) {
        if (InterlockedCompareExchange(&interrupted, 0, 0)) {
            TerminateJobObject(job, 130);
            WaitForSingleObject(process.hProcess, INFINITE);
            snx_error("%s interrupted. You can run the same command again.", stage);
            rc = 130;
            goto done;
        }
        waited = WaitForSingleObject(process.hProcess, 50);
        if (waited == WAIT_OBJECT_0) break;
        if (waited != WAIT_TIMEOUT) goto failed;
    }
    if (!GetExitCodeProcess(process.hProcess, &code)) goto failed;
    /* Negative driver results mean "not a tool command" to the runtime entry point. */
    rc = code <= 255 ? (int)code : 1;
    if (rc) snx_error("%s failed (exit status %lu). You can run the same command again.", stage, (unsigned long)code);
    goto done;
failed:
    if (!error) error = GetLastError();
    snx_error("%s: cannot start or wait for %s%s%s (Windows error %lu)", stage, argv[0], cwd ? " in " : "",
              cwd ? cwd : "", (unsigned long)error);
done:
    if (job) CloseHandle(job);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (handler) SetConsoleCtrlHandler(stage_interrupt, FALSE);
    free(command);
    free(directory);
    return rc;
}
#else
static volatile sig_atomic_t stage_pid, interrupted;
static void stage_interrupt(int sig) {
    int saved = errno;
    interrupted = sig;
    if (stage_pid > 0) {
        kill(-(pid_t)stage_pid, sig);
        if (sig == SIGTERM || sig == SIGHUP || sig == SIGINT) kill(-(pid_t)stage_pid, SIGCONT);
    }
    errno = saved;
}
static int run_stage(const char* const* argv, const char* cwd, const char* stage, int debugger) {
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGTSTP};
    struct sigaction action, old[4], ignore, old_ttou;
    sigset_t blocked, previous;
    pid_t pid, waited, foreground = -1;
    int status = 0, rc = 1, sig = 0, installed = 0, tty = 0, group[2] = {-1, -1}, made;
    size_t i;
    sigemptyset(&blocked);
    for (i = 0; i < 4; ++i) sigaddset(&blocked, signals[i]);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) {
        perror("scarletnx: blocking stage signals");
        return 1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = stage_interrupt;
    sigemptyset(&action.sa_mask);
    interrupted = 0;
    stage_pid = 0;
    for (i = 0; i < 4; ++i) {
        if (sigaction(signals[i], &action, &old[i])) goto setup_failed;
        ++installed;
    }
    /* Give the child the terminal while it runs, so interactive games can read stdin. */
    foreground = tcgetpgrp(STDIN_FILENO);
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (foreground == getpgrp()) {
        if (sigaction(SIGTTOU, &ignore, &old_ttou)) goto setup_failed;
        tty = 1;
    }
    /* The stage leads a process group of its own, which the parent alone makes, and the child waits on this pipe for
       it before going on: both sides making the same group at once had macOS refuse one of them with EPERM, the
       group not made yet as the loser saw it. The child reads the parent's errno, 0 when the group is made */
    if (pipe(group) || fcntl(group[0], F_SETFD, FD_CLOEXEC) || fcntl(group[1], F_SETFD, FD_CLOEXEC)) goto setup_failed;
    fflush(NULL);
    pid = fork();
    if (!pid) {
        int error = EPIPE;
        close(group[1]);
        while (read(group[0], &error, sizeof error) < 0 && errno == EINTR) continue;
        if (error || getpgrp() != getpid()) {
            fprintf(stderr, "scarletnx: creating child process group: %s (pid %ld, group %ld, session %ld)\n",
                    strerror(error ? error : EPERM), (long)getpid(), (long)getpgrp(), (long)getsid(0));
            _exit(126);
        }
        if (tty && tcsetpgrp(STDIN_FILENO, getpgrp())) {
            perror("scarletnx: assigning child terminal");
            _exit(126);
        }
        for (i = 0; i < 4; ++i) {
            action.sa_handler = SIG_DFL;
            sigaction(signals[i], &action, NULL);
        }
        if (tty) sigaction(SIGTTOU, &old_ttou, NULL);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        if (cwd && chdir(cwd)) {
            perror("scarletnx: chdir to the child's working directory");
            _exit(126);
        }
        execvp(argv[0], (char* const*)argv);
        perror("scarletnx: executing child process");
        _exit(127);
    }
    if (pid < 0) goto setup_failed;
    made = setpgid(pid, pid) ? errno : 0;
    while (write(group[1], &made, sizeof made) < 0 && errno == EINTR) continue;
    close(group[0]);
    close(group[1]);
    group[0] = group[1] = -1;
    stage_pid = (sig_atomic_t)pid;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    /* Debugger stops are intentional, unlike a suspended extraction/compiler. */
    do { waited = waitpid(pid, &status, debugger ? 0 : WUNTRACED); } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        perror("scarletnx: waiting for child process");
    else if (WIFSTOPPED(status))
        sig = WSTOPSIG(status);
    else if (WIFSIGNALED(status))
        sig = WTERMSIG(status);
    else if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
    /* Stop/abort is a failed stage, not a checkpoint. Also stop any outstanding workers. */
    sigprocmask(SIG_BLOCK, &blocked, NULL);
    stage_pid = 0;
    if (interrupted) sig = (int)interrupted;
    if (sig || rc) kill(-pid, SIGKILL);
    if (waited > 0 && WIFSTOPPED(status)) {
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    }
    if (sig) {
        rc = 128 + sig;
        snx_error("%s interrupted by signal %d (%s). You can run the same command again.", stage, sig, strsignal(sig));
    } else if (rc)
        snx_error("%s failed (exit status %d). You can run the same command again.", stage, rc);
    goto done;
setup_failed:
    perror("scarletnx: starting child process");
    if (group[0] >= 0) close(group[0]);
    if (group[1] >= 0) close(group[1]);
done:
    if (tty) {
        if (tcsetpgrp(STDIN_FILENO, foreground)) perror("scarletnx: restoring terminal");
        sigaction(SIGTTOU, &old_ttou, NULL);
    }
    for (i = 0; i < (size_t)installed; ++i) sigaction(signals[i], &old[i], NULL);
    sigprocmask(SIG_SETMASK, &previous, NULL);
    return rc;
}
#endif
static int (*stage_runner)(int argc, char** argv);
void snx_set_stage_runner(int (*run)(int argc, char** argv)) {
    stage_runner = run;
}
/* a stage run in this process: its own copy of argv, as a driver's main may rearrange it */
static int run_stage_here(const char* const* argv, const char* stage) {
    int argc = 0;
    char** copy;
    while (argv[argc]) ++argc;
    copy = (char**)snx_alloc((size_t)(argc + 1) * sizeof(*copy));
    for (int i = 0; i < argc; ++i) copy[i] = snx_strdup(argv[i]);
    copy[argc] = NULL;
    int rc = stage_runner(argc, copy);
    for (int i = 0; i < argc; ++i) free(copy[i]);
    free(copy);
    if (rc) snx_error("%s failed (exit status %d). You can run the same command again.", stage, rc);
    return rc;
}
int snx_run_stage(const char* const* argv, const char* cwd, const char* stage) {
    if (stage_runner && !cwd) return run_stage_here(argv, stage);
    return run_stage(argv, cwd, stage, 0);
}
int snx_run_debug_stage(const char* const* argv, const char* cwd, const char* stage) {
    return run_stage(argv, cwd, stage, 1);
}
