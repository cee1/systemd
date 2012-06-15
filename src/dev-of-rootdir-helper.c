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
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <libudev.h>

static const char *get_device_of_rootdir(void) {
        struct udev *udev = NULL;
        struct udev_device *udev_device = NULL;

        struct stat st;

        const char *device = NULL;

        /* Find root device */

        if (stat("/", &st) < 0) {
                fprintf(stderr, "Failed to stat() the root directory: %m");
                goto finish;
        }

        /* Virtual root devices don't need an fsck */
        if (major(st.st_dev) == 0)
                return NULL;

        if (!(udev = udev_new())) {
                fputs("Out of memory", stderr);
                goto finish;
        }

        if (!(udev_device = udev_device_new_from_devnum(udev, 'b', st.st_dev))) {
                fputs("Failed to detect root device.", stderr);
                goto finish;
        }

        if (!(device = udev_device_get_devnode(udev_device))) {
                fputs("Failed to detect device node of root directory.", stderr);
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
        puts(get_device_of_rootdir());
        return 0;
}
