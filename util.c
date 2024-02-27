/* SPDX-License-Identifier: MIT */
/*
 * util.c
 *
 * Utility functions.
 *
 * Copyright (c) 2024, Matthew Madison
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include "util.h"

/*
 * set_bootdev_writeable_status
 *
 * Toggles the read-only soft switch in sysfs for eMMC boot0/boot1
 * devices, if present.
 *
 * bootdev: device name
 * make_writeable: true for wrieable, false otherwise
 *
 * Returns: true if changed, false otherwise
 *
 */
bool
set_bootdev_writeable_status (const char *bootdev, bool make_writeable)
{
	char pathname_ro[64];
	char pathname_force[64];
	char buf[1];
	int fd, rc = 0;
	bool is_writeable;

	if (bootdev == NULL)
		return false;
	if (strlen(bootdev) < 6 || strlen(bootdev) > 32)
		return false;
	sprintf(pathname_force, "/sys/block/%s/force_ro", bootdev + 5);
	sprintf(pathname_ro, "/sys/block/%s/ro", bootdev + 5);
	fd = open(pathname_ro, O_RDONLY);
	if (fd < 0)
		return false;
	if (read(fd, buf, sizeof(buf)) != sizeof(buf)) {
		close(fd);
		return false;
	}
	is_writeable = buf[0] == '0';
	close(fd);
	make_writeable = !!make_writeable;
	if (make_writeable == is_writeable)
		return false;
	fd = open(pathname_force, O_WRONLY);
	if (fd < 0)
		return false;
	buf[0] = make_writeable ? '0' : '1';
	if (write(fd, buf, 1) != 1)
		rc = 1;
	close(fd);

	if (rc)
		fprintf(stderr, "warning: could not change boot device write status\n");

	return true;

} /* set_bootdev_writeable_status */
