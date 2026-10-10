/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2010 ProFUSION embedded systems
***/

#include <fcntl.h>
#include <sys/swap.h>
#include <unistd.h>

#include "alloc-util.h"
#include "constants.h"
#include "detach-swap.h"
#include "errno-util.h"
#include "fd-util.h"
#include "libmount-util.h"
#include "list.h"
#include "log.h"
#include "pidref.h"
#include "process-util.h"

static void swap_device_free(SwapDevice **head, SwapDevice *m) {
        assert(head);
        assert(m);

        LIST_REMOVE(swap_device, *head, m);

        free(m->path);
        free(m);
}

void swap_devices_list_free(SwapDevice **head) {
        assert(head);

        while (*head)
                swap_device_free(head, *head);
}

int swap_list_get(const char *swaps, SwapDevice **head) {
        _cleanup_(mnt_free_tablep) struct libmnt_table *t = NULL;
        _cleanup_(mnt_free_iterp) struct libmnt_iter *i = NULL;
        int r;

        assert(head);

        r = dlopen_libmount(LOG_ERR);
        if (r < 0)
                return r;

        t = sym_mnt_new_table();
        i = sym_mnt_new_iter(MNT_ITER_FORWARD);
        if (!t || !i)
                return log_oom();

        r = sym_mnt_table_parse_swaps(t, swaps);
        if (r == -ENOENT) /* no /proc/swaps is fine */
                return 0;
        if (r < 0)
                return log_error_errno(r, "Failed to parse %s: %m", swaps ?: "/proc/swaps");

        for (;;) {
                struct libmnt_fs *fs;
                _cleanup_free_ SwapDevice *swap = NULL;
                const char *source;

                r = sym_mnt_table_next_fs(t, i, &fs);
                if (r == 1) /* EOF */
                        break;
                if (r < 0)
                        return log_error_errno(r, "Failed to get next entry from %s: %m", swaps ?: "/proc/swaps");

                source = sym_mnt_fs_get_source(fs);
                if (!source)
                        continue;

                swap = new0(SwapDevice, 1);
                if (!swap)
                        return log_oom();

                swap->path = strdup(source);
                if (!swap->path)
                        return log_oom();

                LIST_PREPEND(swap_device, *head, TAKE_PTR(swap));
        }

        return 0;
}

static int swapoff_with_timeout(const char *path) {
        _cleanup_close_pair_ int pfd[2] = EBADF_PAIR;
        _cleanup_(pidref_done_sigkill_nowait) PidRef pidref = PIDREF_NULL;
        int r;

        assert(path);

        if (pipe2(pfd, O_CLOEXEC|O_NONBLOCK) < 0)
                return -errno;

        /* swapoff() pages everything back in first, which can take arbitrarily long. */
        r = pidref_safe_fork_full("(sd-swapoff)",
                                  /* stdio_fds= */ NULL,
                                  pfd, ELEMENTSOF(pfd),
                                  FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_LOG|FORK_REOPEN_LOG, &pidref);
        if (r < 0)
                return r;
        if (r == 0) {
                pfd[0] = safe_close(pfd[0]);
                report_errno_and_exit(pfd[1], RET_NERRNO(swapoff(path)));
        }

        pfd[1] = safe_close(pfd[1]);

        siginfo_t si;
        r = pidref_wait_for_terminate_full(&pidref, DEFAULT_TIMEOUT_USEC, &si);
        if (r < 0)
                return r;

        pidref_done(&pidref);

        if (si.si_code == CLD_EXITED && si.si_status == 0)
                return 0;

        r = read_errno(pfd[0]);
        return r < 0 && r != -EIO ? r : -EPROTO;
}

static int swap_points_list_off(SwapDevice **head, bool *changed) {
        int n_failed = 0, r;

        assert(head);
        assert(changed);

        LIST_FOREACH(swap_device, m, *head) {
                log_info("Deactivating swap %s.", m->path);
                r = swapoff_with_timeout(m->path);
                if (ERRNO_IS_NEG_DEVICE_ABSENT(r))
                        log_debug_errno(r, "Tried to deactivate swap '%s', but swap disappeared by now, ignoring: %m", m->path);
                else if (r < 0) {
                        log_warning_errno(r, "Could not deactivate swap %s: %m", m->path);
                        n_failed++;
                        continue;
                } else
                        *changed = true;

                swap_device_free(head, m);
        }

        return n_failed;
}

int swapoff_all(bool *changed) {
        _cleanup_(swap_devices_list_free) LIST_HEAD(SwapDevice, swap_list_head);
        int r;

        assert(changed);

        LIST_HEAD_INIT(swap_list_head);

        r = swap_list_get(NULL, &swap_list_head);
        if (r < 0)
                return r;

        return swap_points_list_off(&swap_list_head, changed);
}
