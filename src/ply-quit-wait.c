#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "util.h"
#include "socket-util.h"

static int plymouth_wait_quit(int timeout)
{
	int r = 0;
	int efd = -1, ply_sock_fd = -1;
	union sockaddr_union sa;
	struct epoll_event ev = {
		.events = EPOLLERR | EPOLLHUP | EPOLLIN,
	};
	char quit_cmd[] = "Q";
	usec_t cur, last;
	bool is_connected = false;

	if  ((efd = epoll_create(1)) < 0) {
		r = -errno;
		goto finish;
	}

	if ((ply_sock_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0)) < 0) {
		r = -errno;
		goto finish;
	}

	if (epoll_ctl(efd, EPOLL_CTL_ADD, ply_sock_fd, &ev)  < 0) {
		r = -errno;
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
	is_connected = true;

	if (timeout > 0)
		last = now(CLOCK_MONOTONIC);
	do {
		struct epoll_event events[10];
		int nr_events, i;

		do {
			if (timeout > 0) {
				int elapse;

				cur = now(CLOCK_MONOTONIC);
				assert_se(cur >= last);

				elapse = (cur - last) / USEC_PER_MSEC;

				if (elapse >= timeout) {
					/* Already timeout?
					 * rearm timeout to 0 and tell is timed out
					 * and break
					 */
					timeout = 0;
					nr_events = 0;
					break;
				}
				timeout -= elapse;
				last = cur;
			}

			nr_events = epoll_wait(efd, events, sizeof(events)/sizeof(events[0]),
			                       timeout);
		} while (nr_events < 0 && ((errno == EINTR) || (errno == EAGAIN)));

		/* Process events */
		for (i = 0; i < nr_events; i++) {
			if ((events[i].events & EPOLLHUP) || (events[i].events & EPOLLERR)) {
				int bytes_ready = 0;

				if (ioctl(ply_sock_fd, FIONREAD, &bytes_ready) < 0)
					bytes_ready = 0;

				if (bytes_ready <= 0)
					is_connected = false;
			}
		}

		/* Timeout, tell plymouth quit */
		if (nr_events <= 0) {
			if (write(ply_sock_fd, quit_cmd, sizeof(quit_cmd)) != sizeof(quit_cmd)) {
				if (errno != EPIPE &&
				    errno != EAGAIN &&
				    errno != ENOENT &&
				    errno != ECONNREFUSED &&
				    errno != ECONNRESET &&
				    errno != ECONNABORTED) {
					log_error("Failed to tell plymouth to quit: %m");
					r = -errno;
				}
				/* consider disconnect if write failed */
				is_connected = false;
			}
			/* wait plymouthd to quit */
			timeout = -1;
		}
	} while (is_connected);

finish:
	if (efd >= 0)
		close_nointr(efd);

	if (ply_sock_fd >= 0)
		close_nointr(ply_sock_fd);

	return r;
}

int main(int argc, char **argv, char **env)
{
	int timeout, r;
	const char *usage = "Usage: plymouth-quit-wait <timeout in seconds, -1 means forever>";
	char *tmp;

	if (argc < 2)
		goto err_usage;

	timeout = strtol(argv[1], &tmp, 10);

	if (*tmp != '\0')
		goto err_usage;

	if (timeout < 0)
		timeout = -1;
	else
		timeout *= MSEC_PER_SEC; /* ms */

	r = plymouth_wait_quit(timeout);
	if (r)
		fprintf(stderr, "Failed to wait and force plymouthd quit in %d seconds:%s",
		        timeout, strerror(-r));

	return (r == 0) ? 0 : 2;

err_usage:
	puts(usage);

	return -1;
}
