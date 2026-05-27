#include "glyph_sidecar.h"

#include "util/log.h"

#ifndef _WIN32
# include <errno.h>
# include <signal.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/wait.h>
# include <unistd.h>
#endif

#ifndef _WIN32
static pid_t sidecar_pid = -1;
#endif

bool
sc_glyph_sidecar_spawn(const char *serial) {
#ifdef _WIN32
    (void) serial;
    LOGW("--glyph: sidecar spawn is not implemented on Windows yet.");
    return false;
#else
    if (sidecar_pid > 0) {
        LOGW("--glyph: sidecar already running (pid=%ld).", (long) sidecar_pid);
        return true;
    }

    const char *script = getenv("SCRCPY_GLYPH_SIDECAR");
    if (!script || !*script) {
        LOGE("--glyph: SCRCPY_GLYPH_SIDECAR env var is unset. "
             "Use ./run, or export it to point at glyph_sidecar.py.");
        return false;
    }

    // Allow overriding the Python interpreter — Homebrew's python3 often
    // ships without _tkinter, while /usr/bin/python3 on macOS does have it.
    const char *python = getenv("SCRCPY_GLYPH_PYTHON");
    if (!python || !*python) {
#ifdef __APPLE__
        python = "/usr/bin/python3";
#else
        python = "python3";
#endif
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("--glyph: fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child: detach from scrcpy's process group so Ctrl-C in the
        // terminal doesn't reach us — we want lifecycle to be tied to
        // sc_glyph_sidecar_stop() (SIGTERM from the parent), not the tty.
        setpgid(0, 0);

        const char *s = serial ? serial : "";
        char *argv[] = {
            (char *) python,
            (char *) script,
            (char *) "--serial", (char *) s,
            NULL,
        };
        execvp(python, argv);
        // execvp only returns on failure
        fprintf(stderr, "--glyph: execvp %s failed: %s\n",
                python, strerror(errno));
        _exit(127);
    }

    sidecar_pid = pid;
    LOGI("--glyph: sidecar spawned (pid=%ld, script=%s).",
         (long) sidecar_pid, script);
    return true;
#endif
}

void
sc_glyph_sidecar_stop(void) {
#ifndef _WIN32
    if (sidecar_pid <= 0) {
        return;
    }

    pid_t pid = sidecar_pid;
    sidecar_pid = -1;

    if (kill(pid, SIGTERM) < 0) {
        if (errno != ESRCH) {
            LOGW("--glyph: kill(SIGTERM) failed: %s", strerror(errno));
        }
        return;
    }

    // Best-effort short wait, then move on. We don't want to block scrcpy
    // shutdown if the sidecar is wedged.
    for (int i = 0; i < 20; ++i) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) {
            return;
        }
        usleep(50 * 1000); // 50ms
    }
    LOGW("--glyph: sidecar (pid=%ld) did not exit within 1s; abandoning.",
         (long) pid);
#endif
}
