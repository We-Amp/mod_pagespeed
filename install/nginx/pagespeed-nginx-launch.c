// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/* pagespeed-nginx-launch — the design record UX-7 launch shim.
 *
 * Sets PR_SET_PDEATHSIG so the bundled nginx receives SIGTERM if the parent
 * (the ASP.NET Core host process) dies abruptly — SIGKILL / OOM-kill /
 * container hard-stop — which the managed graceful-shutdown path
 * (StopAsync / Dispose -> Kill(entireProcessTree)) cannot cover. Without it,
 * nginx reparents to PID 1 and keeps holding the public listen port.
 *
 * It then exec's argv[1..], replacing itself, so the managed Process still
 * tracks the nginx master directly (the shim leaves no extra layer at runtime).
 *
 * Linux-only (prctl(2) / PR_SET_PDEATHSIG). Built per-RID in the focal portable
 * recipe (glibc-2.31 floor, libc-only, stripped) and laid into
 * runtimes/<rid>/native/ next to nginx; ProcessSidecarManager.BuildStartInfo
 * launches nginx through it when present (else launches nginx directly).
 */
#define _GNU_SOURCE
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "pagespeed-nginx-launch: usage: %s <program> [args...]\n",
                argv[0] ? argv[0] : "pagespeed-nginx-launch");
        return 2;
    }

    /* Deliver SIGTERM (nginx fast-shutdown) to this process when our parent dies. */
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        perror("pagespeed-nginx-launch: prctl(PR_SET_PDEATHSIG)");
        /* Non-fatal: losing the death-signal is strictly better than refusing to
         * start the optimizer, so fall through and exec nginx anyway. */
    }

    /* Race guard: if the parent already exited before the prctl above ran, we have
     * been reparented to init (PID 1) and the death signal will never arrive.
     * Detect that and exit rather than outlive the host. */
    if (getppid() == 1) {
        fprintf(stderr, "pagespeed-nginx-launch: parent already exited; not starting nginx\n");
        return 0;
    }

    execv(argv[1], &argv[1]);
    perror("pagespeed-nginx-launch: execv");
    return 127;
}
