#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "util.h"

int main(int argc, char **argv, char **env)
{
	static const char *clear_cmd = "\33[H\33[2J";
	char *prompt = NULL;
	static const char *ttys[] ={
		"/dev/tty1",
		"/dev/tty2",
		"/dev/tty3",
		"/dev/tty4",
		"/dev/tty5",
		"/dev/tty6",
		NULL
	};
	const char **i;

	if (argc < 2) {
		printf("Usage: %s prompt\n", argv[0]);
		return 0;
	}

	if (asprintf(&prompt, "%s\r\n", argv[1]) < 0) {
		log_error("asprintf() %m");
		return -1;
	}

	for (i = ttys; *i; i++) {
		const char *t = *i;
		int fd = -1;

		if ((fd = open_terminal(t, O_RDWR|O_NOCTTY|O_CLOEXEC)) < 0)
			continue;

		loop_write(fd, clear_cmd, strlen(clear_cmd), false) && \
		  loop_write(fd, prompt, strlen(prompt), false);
		close_nointr_nofail(fd);
	}

	free(prompt);

	return 0;
}
