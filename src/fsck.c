/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <libudev.h>
#include <dbus/dbus.h>
#include <locale.h>

#include "util.h"
#include "utf8-util.h"
#include "dbus-common.h"
#include "special.h"
#include "bus-errors.h"
#include "socket-util.h"
#include "def.h"

//#define FSCK_USE_PRETTY_PROGRESS_INFO

static bool arg_skip = false;
static bool arg_force = false;
static bool arg_plymouth = false;

typedef struct FsckProgress {
        const char *device;
        char *progress;
        size_t progress_buffer_size;
        bool merge;
        bool cancel;
        bool finished;
} FsckProgress;

static void start_target(const char *target, bool isolate) {
        DBusMessage *m = NULL, *reply = NULL;
        DBusError error;
        const char *mode, *basic_target = "basic.target";
        DBusConnection *bus = NULL;

        assert(target);

        dbus_error_init(&error);

        if (bus_connect(DBUS_BUS_SYSTEM, &bus, NULL, &error) < 0) {
                log_error("Failed to get D-Bus connection: %s", bus_error_message(&error));
                goto finish;
        }

        if (isolate)
                mode = "isolate";
        else
                mode = "replace";

        log_info("Running request %s/start/%s", target, mode);

        if (!(m = dbus_message_new_method_call("org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartUnitReplace"))) {
                log_error("Could not allocate message.");
                goto finish;
        }

        /* Start these units only if we can replace base.target with it */

        if (!dbus_message_append_args(m,
                                      DBUS_TYPE_STRING, &basic_target,
                                      DBUS_TYPE_STRING, &target,
                                      DBUS_TYPE_STRING, &mode,
                                      DBUS_TYPE_INVALID)) {
                log_error("Could not attach target and flag information to message.");
                goto finish;
        }

        if (!(reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error))) {

                /* Don't print a waring if we aren't called during
                 * startup */
                if (!dbus_error_has_name(&error, BUS_ERROR_NO_SUCH_JOB))
                        log_error("Failed to start unit: %s", bus_error_message(&error));

                goto finish;
        }

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        if (bus) {
                dbus_connection_flush(bus);
                dbus_connection_close(bus);
                dbus_connection_unref(bus);
        }

        dbus_error_free(&error);
}

static int parse_proc_cmdline(void) {
        char *line, *w, *state;
        int r;
        size_t l;

        if (detect_container(NULL) > 0)
                return 0;

        if ((r = read_one_line_file("/proc/cmdline", &line)) < 0) {
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(-r));
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, line, state) {

                if (strneq(w, "fsck.mode=auto", l))
                        arg_force = arg_skip = false;
                else if (strneq(w, "fsck.mode=force", l))
                        arg_force = true;
                else if (strneq(w, "fsck.mode=skip", l))
                        arg_skip = true;
                else if (startswith(w, "fsck.mode"))
                        log_warning("Invalid fsck.mode= parameter. Ignoring.");
#if defined(TARGET_FEDORA) || defined(TARGET_MANDRIVA)
                else if (strneq(w, "fastboot", l))
                        arg_skip = true;
                else if (strneq(w, "forcefsck", l))
                        arg_force = true;
#endif
                else if (strneq(w, "rhgb", l) || strneq(w, "splash", l))
                        arg_plymouth = true;
        }

        free(line);
        return 0;
}

static void test_files(void) {
        if (access("/fastboot", F_OK) >= 0)
                arg_skip = true;

        if (access("/forcefsck", F_OK) >= 0)
                arg_force = true;
}

#ifdef FSCK_USE_PRETTY_PROGRESS_INFO
static bool fsck_progress_msg_strip(char **utf8_msg) {
        /* e2fsck will wrap message as "\001xxxx\r\002" */
#define TO_STRIP "\r\n\001\002"
        char *head, *tail;
        bool new_line = false;

        head = *utf8_msg;
        /*
         * strip tail first
         *   to safely decide whether terminated with new_line.
         */
        tail = head + strlen(head);
        for (tail = utf8_find_prev_char(head, tail);
             tail && strstr(TO_STRIP, tail);
             tail = utf8_find_prev_char(head, tail)) {

                if (!new_line &&
                    (*tail == '\n' || *tail == '\r'))
                        new_line = true;
                *tail = '\0';
        }
        head = head + strspn(head, TO_STRIP);

        *utf8_msg = head;

        return new_line;
}

static const char *parse_fsck_progress(FsckProgress *fp, char *msg, size_t nbytes) {
        char *utf8_msg = NULL, *info = NULL;
        int k = locale_to_utf8(&utf8_msg, msg, (ssize_t) nbytes, NULL, NULL);

        if (k < 0)
                log_warning("locale_to_utf8(): %s", strerror(-k));

        if (utf8_msg && strlen(utf8_msg)) {
                char *m, *p, *stripped_utf8_msg = utf8_msg;
                bool do_merge = fp->merge;
                size_t l;

                fp->merge = !fsck_progress_msg_strip(&stripped_utf8_msg);

                /* only returns the last line */
                for (m = p = stripped_utf8_msg; (p = strpbrk(p, "\n\r")); p++) {
                        do_merge = false;
                        m = p + 1;
                }

                fsck_progress_msg_strip(&m); /* strip '\001', '\002' */

                /* re-calculate space occupation */
                m = utf8_merge_backspace_char(m);
                l = strlen(m) + 1;
                if (do_merge && fp->progress)
                        l += strlen(fp->progress);

                if (l > fp->progress_buffer_size) {
                        char *tmp = realloc(fp->progress, l);
                        if (!tmp)
                                /* Note: quit from here, may break integrity of progress info */
                                goto finish;
                        fp->progress = tmp;
                        fp->progress_buffer_size = l;
                }

                if (do_merge) {
                        strcat(fp->progress, m);
                        utf8_merge_backspace_char(fp->progress);
                } else
                        strcpy(fp->progress, m);

                if (asprintf(&info, "%s%s",
                             fp->cancel ? "STOP\t " : "",
                             fp->progress) < 0) {
                        info = NULL;
                        goto finish;
                }
        }

finish:
        if (utf8_msg)
                free(utf8_msg);

        return (const char *) info;
}
#else
static const char *parse_fsck_progress(FsckProgress *fp, char *msg, size_t nbytes) {
        char device[128];
        char *info = NULL, *end, *last_line;
        size_t l;
        int pass;
        unsigned long cur, max;

        assert_se(msg && nbytes);

        /* Note:
         *   1. msg isn't NULL terminated
         *   2. msg is integrate (i.e. is '%d %lu %lu %s\n' of N)
         *   3. p > msg
         */
        end = msg + nbytes;

        /* strip tailing '\n' */
        if (end[-1] == '\n')
                end[-1] = '\0';

        /* Find the last line */
        for (last_line = end - 1; last_line > msg; last_line--) {
                if (last_line[-1] == '\n' || last_line[-1] == '\0')
                        break;
        }

        /* re-calculate space occupation */
        l = end - last_line + 1 /* \0 */;

        if (l > fp->progress_buffer_size) {
                char *tmp = realloc(fp->progress, l);
                if (!tmp)
                        goto finish;
                fp->progress = tmp;
                fp->progress_buffer_size = l;
        }

        strncpy(fp->progress, last_line, l - 1);

        errno = 0;
        sscanf(fp->progress, "%d %lu %lu %s", &pass, &cur, &max, device);

        if (!errno) {
                float percent;

                if (pass < 0) {
                        pass = 0;
                        percent = 0.0;
                } else if (max == 0)
                        percent = 100.0;
                else
                        percent = ((float) cur) / ((float) max) * 100;

                if (asprintf(&info, "%s%s: \tPass:%-2d %4.1f%%",
                             fp->cancel ? "STOP\t " : "",
                             device, pass, percent) < 0) {
                        info = NULL;
                        goto finish;
                }
        }

finish:
        return (const char *) info;
}
#endif /* FSCK_USE_PRETTY_PROGRESS_INFO */

static bool update_fsck_progress_plymouth(FsckProgress *fp, int fsck_fd) {
        bool r = true;
        char message[255 + 255];
        const char *info = NULL;
        int msg_len;

        union sockaddr_union sa;
        int ply_sock_fd = -1;

        int n = 0;

        if (!fp->finished) {
                char *buf;
                size_t nbytes;
                ssize_t cnt;

                if ((cnt = read_all(fsck_fd, (void **) &buf, &nbytes)) < 0) {
                        log_error("Failed to read fsck progress info: %s", strerror(-cnt));
                        return false;
                }

                if (cnt == 0)
                        return true;

                info = parse_fsck_progress(fp, buf, nbytes);
                free(buf);

                if (!info || !strlen(info))
                        return true;
        } else
                info = strdup("");

        msg_len = strlen("fsck:") + strlen(fp->device) + 1 /* tailing '\0' */;
        msg_len = msg_len <= 255 ? msg_len : 255; /* %c => max len is 255 */
        snprintf(message, 255, "U\003%c%s%s", msg_len,
                 "fsck:", fp->device);
        n = strlen(message) + 1;

        msg_len = strlen(info) + 1 /* tailing '\0' */;
        msg_len = msg_len <= 255 ? msg_len : 255;
        snprintf(message + n, 255, "%c%s", msg_len, info);
        n += strlen(message + n) + 1;
        free((void *) info);

        if ((ply_sock_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0)) < 0) {
                r = false;
                goto finish;
        }

        zero(sa);
        sa.sa.sa_family = AF_UNIX;
        strncpy(sa.un.sun_path+1, "/org/freedesktop/plymouthd", sizeof(sa.un.sun_path)-1);
        if (connect(ply_sock_fd, &sa.sa,
                    offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sa.un.sun_path+1)) < 0) {
                log_error("Failed to connect to Plymouth: %m");

                r = false;
                goto finish;
        }

        errno = 0;
        if (write(ply_sock_fd, message, n) != n) {

                if (errno != EPIPE &&
                    errno != EAGAIN &&
                    errno != ENOENT &&
                    errno != ECONNREFUSED &&
                    errno != ECONNRESET &&
                    errno != ECONNABORTED) {
                        log_error("Failed to write Plymouth message: %m");

                        r = false;
                        goto finish;
                }
        }

finish:
        if (ply_sock_fd >= 0)
                close_nointr(ply_sock_fd);

        return r;
}

static const char *get_device_of_rootdir(void) {
        struct udev *udev = NULL;
        struct udev_device *udev_device = NULL;

        struct stat st;
        struct timespec times[2];

        const char *device = NULL;

        /* Find root device */

        if (stat("/", &st) < 0) {
                log_error("Failed to stat() the root directory: %m");
                goto finish;
        }

        /* Virtual root devices don't need an fsck */
        if (major(st.st_dev) == 0)
                return NULL;

        /* check if we are already writable */
        times[0] = st.st_atim;
        times[1] = st.st_mtim;
        if (utimensat(AT_FDCWD, "/", times, 0) == 0) {
                log_info("Root directory is writable, skipping check.");
                return NULL;
        }

        if (!(udev = udev_new())) {
                log_error("Out of memory");
                goto finish;
        }

        if (!(udev_device = udev_device_new_from_devnum(udev, 'b', st.st_dev))) {
                log_error("Failed to detect root device.");
                goto finish;
        }

        if (!(device = udev_device_get_devnode(udev_device))) {
                log_error("Failed to detect device node of root directory.");
                goto finish;
        }

finish:
        if (device)
                /* device is an internal string of udev_device,
                 * it will be invalid of we unref udev_device */
                device = strdup(device);

        if (udev_device)
                udev_device_unref(udev_device);

        if (udev)
                udev_unref(udev);

        return device;
}


int main(int argc, char *argv[]) {
        const char *cmdline[8];
        int i = 0, r = EXIT_FAILURE, q;
        int pid;
        siginfo_t status;
        const char *device;
        bool root_directory;
        int prog_fd_read = -1, signal_fd = -1;
#ifndef FSCK_USE_PRETTY_PROGRESS_INFO
        int prog_fd_write = -1;
#endif

        /*
         * On startup of the main program, the portable "C" locale is
         * selected as default.
         * Call setlocale(LC_ALL, "") at the beginning of the program
         * to enable selecting the locale from the environment, thus
         * nl_langinfo(CODESET) will return "UTF-8" instead of "ANSI_X3.4-196".
         */
        setlocale(LC_ALL, "");
        if (argc > 2) {
                log_error("This program expects one or no arguments.");
                return EXIT_FAILURE;
        }

        log_set_target(LOG_TARGET_SYSLOG_OR_KMSG);
        log_parse_environment();
        log_open();

        parse_proc_cmdline();
        test_files();

        if (!arg_force && arg_skip)
                return 0;

        if (argc > 1) {
                device = strdup(argv[1]);
                root_directory = false;
        } else {
                if (!(device = get_device_of_rootdir()))
                        goto finish;
                root_directory = true;
        }

        cmdline[i++] = "/sbin/fsck";
        cmdline[i++] = "-a";
        cmdline[i++] = "-T";
        cmdline[i++] = "-l";

        if (!root_directory)
                cmdline[i++] = "-M";

        if (arg_force)
                cmdline[i++] = "-f";

        if (arg_plymouth) {
                char tmp[10];
                sigset_t mask;

                /*
                 * The '-CN' option directs fsck to produce progress info.
                 * if N <= 0, print human readable info to stdout(progress bar, update with '\b...')
                 *
                 * if N > 0, print message to fd N, in form of
                 * "%d %lu %lu %s\n"  <--  pass, cur, max, device_name
                 *
                 * Note: Not all fsck support '-C' option.
                 */

#ifdef FSCK_USE_PRETTY_PROGRESS_INFO
                strncpy(tmp, "-C0", sizeof(tmp));
#else
                int p[2];
                if (pipe(p) < 0) {
                        log_error("pipe(): %m");
                        goto finish;
                }
                prog_fd_read = p[0];
                prog_fd_write = p[1];
                snprintf(tmp, sizeof(tmp), "-C%d", prog_fd_write);
#endif
                cmdline[i++] = tmp;

                assert_se(sigemptyset(&mask) == 0);
                sigset_add_many(&mask, SIGCHLD, SIGINT, SIGTERM, SIGALRM, -1);
                /* block signals, they will be processed though signalfd */
                assert_se(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);

                if ((signal_fd = signalfd(-1, &mask, SFD_CLOEXEC)) < 0) {
                        log_error("signalfd(): %m");
                        goto finish;
                }

        }

        cmdline[i++] = device;
        cmdline[i++] = NULL;

#ifdef FSCK_USE_PRETTY_PROGRESS_INFO
        if ((q = spawn_async_with_pipes(cmdline, &pid, NULL,
                                        arg_plymouth? &prog_fd_read : NULL,
                                        arg_plymouth? &prog_fd_read : NULL)) < 0) {
                log_error("spawn_async_with_pipes(): %s", strerror(-q));
                goto finish;
        }
#else
        if ((pid = (int) fork()) < 0) {
                log_error("fork(): %m");
                goto finish;
        } else if (pid == 0) { /* Child */
                sigset_t mask;
                /* Revert signal block states -- don't block any signal */
                assert_se(sigemptyset(&mask) == 0);
                assert_se(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);

                if (prog_fd_read >= 0)
                        close_nointr(prog_fd_read);
                execv(cmdline[0], (char **) cmdline);
                _exit(8); /* Operational error */
        } else { /* Parent */
                if (prog_fd_write >= 0)
                        close_nointr(prog_fd_write);
                prog_fd_write = -1;
        }
#endif

        if (arg_plymouth) {
                FsckProgress fp = {
                        .device = device,
                        .progress = NULL,
                        .progress_buffer_size = 0,
                        .merge = true,
                        .cancel = false,
                        .finished = false,
                };
                unsigned int interval = 300 * 1000 * 1000; /* 300ms */

                /* Ignore SIGPIPE, if plymouth dies unexpectedly, will receive EPIPE */
                ignore_signals(SIGNALS_IGNORE, -1);

                /*
                 * poll vs periodically read:
                 * frequent POLLIN will kill the performance.
                 * -- Thought it was not case here:
                 *    fsck.extN will send progress info on signal SIGUSR1,
                 *    I guess fsck sends SIGUSR1 to fsck.extN periodically.
                 *
                 * Nevertheless leave the periodically read logic unchanged.
                 */
                if ((q = alarm_ns(CLOCK_MONOTONIC, interval) < 0)) {
                        log_error("alarm_ns(CLOCK_MONOTONIC, %d) %s", interval, strerror(-q));
                        goto watch_finish;
                }

                /* Not block on reading fsck fd */
                fd_nonblock(prog_fd_read, true);

                for (;;) {
                        struct signalfd_siginfo sig;
                        if (read(signal_fd, (void *) &sig, sizeof(sig)) < 0) {
                                if (errno == EINTR || errno == EAGAIN)
                                        continue;

                                log_error("Failed to read signalfd: %m");
                                goto watch_finish;
                        }

                        if (sig.ssi_signo == SIGCHLD) {
                                zero(status);
                                q = waitid(P_PID, pid, &status, WEXITED | WNOHANG | WNOWAIT);
                                assert(q == 0);

                                if (!status.si_signo) /* Child not exit, maybe SIGSTOP or SIGCONT */
                                        continue;

                                update_fsck_progress_plymouth(&fp, prog_fd_read);
                                break;
                        } else if (sig.ssi_signo == SIGINT || sig.ssi_signo == SIGTERM) {
                                log_info("Recevied signal, terminating fsck %s", device);
                                fp.cancel = true;
                                if (kill(pid, SIGTERM) < 0)
                                        log_warning("Failed to kill %d(fsck %s) with SIGINT: %m",
                                                    pid, device);
                        } else if (sig.ssi_signo == SIGALRM) {
                                update_fsck_progress_plymouth(&fp, prog_fd_read);
                        }
                }

        watch_finish:
                /* notify plymouth fsck is finished. */
                fp.finished = true;
                update_fsck_progress_plymouth(&fp, prog_fd_read);

                if (fp.progress)
                        free(fp.progress);

                alarm_ns(CLOCK_MONOTONIC, 0); /* stop timer  */
                if (signal_fd >= 0)
                        close_nointr(signal_fd);
        }

        if ((q = wait_for_terminate(pid, &status)) < 0) {
                log_error("waitid(): %s", strerror(-q));
                goto finish;
        }

        if (status.si_code != CLD_EXITED || (status.si_status & ~1)) {

                if (status.si_code == CLD_KILLED || status.si_code == CLD_DUMPED)
                        log_error("fsck terminated by signal %s.", signal_to_string(status.si_status));
                else if (status.si_code == CLD_EXITED)
                        log_error("fsck failed with error code %i.", status.si_status);
                else
                        log_error("fsck failed due to unknown reason.");

                if (status.si_code == CLD_EXITED && (status.si_status & 2) && root_directory)
                        /* System should be rebooted. */
                        start_target(SPECIAL_REBOOT_TARGET, false);
                else if (status.si_code == CLD_EXITED && (status.si_status & 6))
                        /* Some other problem */
                        start_target(SPECIAL_EMERGENCY_TARGET, true);
                else {
                        r = EXIT_SUCCESS;
                        log_warning("Ignoring error.");
                }

        } else
                r = EXIT_SUCCESS;

        if (status.si_code == CLD_EXITED && (status.si_status & 1))
                touch("/run/systemd/quotacheck");

finish:
        if (device)
                free((void *) device);

        if (prog_fd_read >= 0)
                close_nointr(prog_fd_read);
#ifndef FSCK_USE_PRETTY_PROGRESS_INFO
        if (prog_fd_write >= 0)
                close_nointr(prog_fd_write);
#endif

        return r;
}
