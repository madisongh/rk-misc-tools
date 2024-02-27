/* SPDX-License-Identifier: MIT */
/*
 * bootinfo.c
 *
 * Implements storage for "boot variables", which are
 * simply named storage outside of a regular filesystem
 * that persist across reboots of the system. Similar
 * to U-Boot's environment variable store, but managed
 * separately.
 *
 * This persistent store is for small quantities of information
 * that are critical to preserve across reboots, even if
 * the ordinary filesystem partitions on the system get corrupted.
 *
 * Derived from tegra-bootinfo.c in tegra-boot-tools; see
 * https://github.com/OE4T/tegra-boot-tools.  Differences
 * from that version:
 *   - removed tegra specifics
 *   - variables may begin with an underscore
 *   - underscore-prefixed variables are preserved when re-initializing
 *     the variable store
 *   - offsets are from the start of the storage device (positive),
 *     rather than from the end (negative).
 *
 * Copyright (c) 2019-2022, Matthew Madison
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <zlib.h>
#include "bootinfo.h"
#include "util.h"

static const char DEVICE_MAGIC[8] = {'B', 'O', 'O', 'T', 'I', 'N', 'F', 'O'};
#define DEVICE_MAGIC_SIZE sizeof(DEVICE_MAGIC)

static const uint16_t DEVINFO_VERSION_CURRENT = 4;

#ifndef EXTENSION_SECTOR_COUNT
#define EXTENSION_SECTOR_COUNT 1023
#endif

#define MAX_EXTENSION_SECTORS 1023

#if (EXTENSION_SECTOR_COUNT == 0) || (EXTENSION_SECTOR_COUNT > MAX_EXTENSION_SECTORS)
#error "EXTENSION_SECTOR_COUNT out of range"
#endif

#ifndef BOOTINFO_STORAGE_DEVICE
#define BOOTINFO_STORAGE_DEVICE "/dev/mmcblk0boot1"
#endif

/*
 * Reserve a full sector for the header.  Variable data
 * will be packed in after the header, then spill over
 * to extension sectors.
 */
#define DEVINFO_BLOCK_SIZE 512
#define EXTENSION_SIZE (EXTENSION_SECTOR_COUNT*512)

struct device_info {
	unsigned char magic[DEVICE_MAGIC_SIZE];
	uint16_t devinfo_version;
	uint8_t	 flags;
	uint8_t	 failed_boots;
	uint32_t crcsum;
	uint8_t	 sernum;
	uint8_t	 unused__;
	uint16_t ext_sectors;
} __attribute__((packed));
#define FLAG_BOOT_IN_PROGRESS	(1<<0)
#define DEVINFO_HDR_SIZE sizeof(struct device_info)
#define VARSPACE_SIZE (DEVINFO_BLOCK_SIZE+EXTENSION_SIZE-(DEVINFO_HDR_SIZE+sizeof(uint32_t)))
/*
 * Maximum size for a variable value is all of the variable space minus two bytes
 * for null terminators (for name and value) and one byte for a name, plus one
 * byte for the null character terminating the variable list.
 */
#define MAX_VALUE_SIZE (VARSPACE_SIZE-4)

#ifndef BOOTINFO_STORAGE_OFFSET_A
#define BOOTINFO_STORAGE_OFFSET_A  0
#endif
/*
 * By default, locate the second storage block directly after
 * the first.  Depending on the type of storage device, you may
 * need to adjust the offset to ensure it is not located in the
 * same erase block as the first.
 */
#ifndef BOOTINFO_STORAGE_OFFSET_B
#define BOOTINFO_STORAGE_OFFSET_B (BOOTINFO_STORAGE_OFFSET_A + DEVINFO_BLOCK_SIZE + EXTENSION_SIZE)
#endif

struct info_var {
	struct info_var *next;
	char *name;
	char *value;
};


struct devinfo_context {
	int fd;
	int lockfd;
	bool readonly;
	int valid[2];
	int current;
	struct device_info curinfo;
	struct info_var *vars;
	size_t varsize;
	uint8_t infobuf[2][DEVINFO_BLOCK_SIZE+EXTENSION_SIZE];
	char devinfo_dev[PATH_MAX];
	/* storage for setting variables */
	char namebuf[DEVINFO_BLOCK_SIZE];
	char valuebuf[MAX_VALUE_SIZE];
};

#define OFFSET_COUNT 2
static const off_t devinfo_offset[OFFSET_COUNT] = {
	[0] = BOOTINFO_STORAGE_OFFSET_A,
	[1] = BOOTINFO_STORAGE_OFFSET_B,
};

static const off_t extension_offset[OFFSET_COUNT] = {
	[0] = (BOOTINFO_STORAGE_OFFSET_A + DEVINFO_BLOCK_SIZE),
	[1] = (BOOTINFO_STORAGE_OFFSET_B + DEVINFO_BLOCK_SIZE),
};

static const char *devinfo_devices[] = {
	[0] = BOOTINFO_STORAGE_DEVICE,
};

/*
 * find_storage_dev
 *
 * Identifies the devinfo storage device
 * by iterating through devinfo_devices[]. First
 * successful access(F_OK) wins.
 *
 * returns 0 on success, non-0 on failure.
 */
static int
find_storage_dev (char *buf, size_t bufsize)
{
	unsigned int i;

	for (i = 0; i < sizeof(devinfo_devices)/sizeof(devinfo_devices[0]); i++) {
		if (access(devinfo_devices[i], F_OK) == 0) {
		        strncpy(buf, devinfo_devices[i], bufsize-1);
			buf[bufsize-1] = '\0';
			return 0;
		}
	}
	errno = ENODEV;
	return -1;

} /* find_storage_dev */

/*
 * parse_vars
 *
 * A variable consists of a null-terminated name followed
 * by a null-terminated value.	Names and values are simply
 * concatenated into the space after the header, up to the
 * block size. A null byte at the beginning of a variable
 * name indicates the end of the list.
 *
 * It's possible to have a null value, but in this implementation
 * null-valued variables are not written to the info block;
 * setting a value to the null string deletes the variable.
 */
static int
parse_vars (struct devinfo_context *ctx)
{
	struct info_var *var, *last;
	char *cp, *endp, *valp;
	ssize_t remain, varbytes;

	ctx->vars = NULL;
	if (ctx->current < 0) {
		fprintf(stderr, "error: parse_vars called with no valid info block\n");
		return -1;
	}
	for (cp = (char *)(ctx->infobuf[ctx->current] + DEVINFO_HDR_SIZE),
		     remain = sizeof(ctx->infobuf[ctx->current]) - (DEVINFO_HDR_SIZE+sizeof(uint32_t)),
		     ctx->varsize = 0,
		     last = NULL;
	     remain > 0 && *cp != '\0';
	     cp += varbytes, remain -= varbytes) {
		for (endp = cp + 1, varbytes = 1;
		     varbytes < remain && *endp != '\0';
		     endp++, varbytes++);
		if (varbytes >= remain)
			break;
		for (valp = endp + 1, varbytes += 1;
		     varbytes < remain && *valp != '\0';
		     valp++, varbytes++);
		if (varbytes >= remain)
			break;
		var = calloc(1, sizeof(struct info_var));
		if (var == NULL) {
			perror("variable storage");
			return -1;
		}
		var->name = cp;
		var->value = endp + 1;
		if (last == NULL)
			ctx->vars = var;
		else
			last->next = var;
		last = var;
		varbytes += 1; /* for trailing null at end of value */
		ctx->varsize += varbytes;
	}

	return 0;

} /* parse_vars */

/*
 * pack_vars
 *
 * Pack the list of variables into the current devinfo block.
 */
static int
pack_vars (struct devinfo_context *ctx, int idx)
{
	struct info_var *var;
	char *cp;
	size_t remain, nlen, vlen;

	if (idx != 0 && idx != 1)
		return -1;
	if (ctx->vars == NULL)
		return 0;
	for (var = ctx->vars, cp = (char *)(ctx->infobuf[idx] + DEVINFO_HDR_SIZE),
		     remain = sizeof(ctx->infobuf[ctx->current]) - (DEVINFO_HDR_SIZE+sizeof(uint32_t)+1);
	     var != NULL && remain > 0;
	     var = var->next) {
		nlen = strlen(var->name) + 1;
		vlen = strlen(var->value) + 1;
		if (nlen + vlen > remain) {
			fprintf(stderr, "error: variables list too large\n");
			return -1;
		}
		memcpy(cp, var->name, nlen);
		cp += nlen; remain -= nlen;
		memcpy(cp, var->value, vlen);
		cp += vlen; remain -= vlen;
	}
	if (var != NULL || remain == 0) {
		fprintf(stderr, "error: variables list too large\n");
		return -1;
	}
	*cp = '\0';

	return 0;

} /* pack_vars */

/*
 * free_vars
 *
 * Frees the memory for variable tracking.
 */
static void
free_vars (struct info_var *firstvar)
{
	struct info_var *var, *vnext;
	for (var = firstvar; var != NULL; var = vnext) {
		vnext = var->next;
		free(var);
	}

} /* free_vars */

/*
 * find_bootinfo
 *
 * Tries to find a valid bootinfo block, and initializes a context
 * if one is found.
 *
 * Returns negative value on an underlying error or if neither block
 * is valid.
 *
 * ctxp is set to NULL if an internal error occurred, otherwise it is
 * set to point to a valid context, and (*ctxp)->fd is the fd of
 * the open channel to the MMC boot1 device. Device is opened readonly
 * and (*ctxp)->readonly is set to true if the readonly arg is non-zero;
 * otherwise, (*ctxp)->readonly is set to true if a valid block is found
 * but an internal error occurred parsing the variables stored in the block.
 */
static int
find_bootinfo (bool readonly, struct devinfo_context **ctxp, const char *devinfo_dev)
{
	struct devinfo_context *ctx;
	struct device_info *dp;
	ssize_t n, cnt;
	int i, dirfd;

	*ctxp = NULL;
	ctx = calloc(1, sizeof(struct devinfo_context));
	if (ctx == NULL)
		return -1;
	ctx->readonly = readonly;
	strcpy(ctx->devinfo_dev, devinfo_dev);

	dirfd = open("/run/rk-bootinfo", O_PATH);
	if (dirfd < 0) {
		if (mkdir("/run/rk-bootinfo", 02770) < 0) {
			close(ctx->fd);
			if (!ctx->readonly)
				set_bootdev_writeable_status(ctx->devinfo_dev, false);
			free(ctx);
			return -1;
		}
		dirfd = open("/run/rk-bootinfo", O_PATH);
	}
	ctx->lockfd = openat(dirfd, "lockfile", O_CREAT|O_RDWR, 0770);
	if (ctx->lockfd < 0) {
		close(dirfd);
		free(ctx);
		return -1;
	}
	close(dirfd);
	if (flock(ctx->lockfd, (readonly ? LOCK_SH : LOCK_EX)) < 0) {
		close(ctx->lockfd);
		free(ctx);
		return -1;
	}
	if (!ctx->readonly)
		set_bootdev_writeable_status(ctx->devinfo_dev, true);

	ctx->fd = open(devinfo_dev, (readonly ? O_RDONLY : O_RDWR|O_DSYNC));
	if (ctx->fd < 0) {
		if (!ctx->readonly)
			set_bootdev_writeable_status(ctx->devinfo_dev, false);
		close(ctx->lockfd);
		free(ctx);
		return -1;
	}
	for (i = 0; i < OFFSET_COUNT; i++) {
		/*
		 * Read base block
		 */
		if (lseek(ctx->fd, devinfo_offset[i], SEEK_SET) < 0)
			continue;
		for (n = 0; n < DEVINFO_BLOCK_SIZE; n += cnt) {
			cnt = read(ctx->fd, &ctx->infobuf[i][n], DEVINFO_BLOCK_SIZE-n);
			if (cnt < 0)
				break;
		}
		if (n < DEVINFO_BLOCK_SIZE)
			continue;

		dp = (struct device_info *)(ctx->infobuf[i]);

		if (memcmp(dp->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE) != 0)
			continue;
		if (dp->devinfo_version >= DEVINFO_VERSION_CURRENT) {
			uint32_t crcsum;
			if (dp->ext_sectors != EXTENSION_SECTOR_COUNT) {
				continue;
			}
			/*
			 * Read extension block
			 */
			if (lseek(ctx->fd, extension_offset[i], SEEK_SET) < 0)
				continue;
			for (n = 0; n < EXTENSION_SIZE; n += cnt) {
				cnt = read(ctx->fd, &ctx->infobuf[i][DEVINFO_BLOCK_SIZE+n], EXTENSION_SIZE-n);
				if (cnt < 0)
					break;
			}
			if (n < EXTENSION_SIZE)
				continue;
			crcsum = *(uint32_t *)(&ctx->infobuf[i][DEVINFO_BLOCK_SIZE+EXTENSION_SIZE-sizeof(uint32_t)]);
			if (crc32(0, &ctx->infobuf[i][DEVINFO_BLOCK_SIZE], EXTENSION_SIZE-sizeof(uint32_t)) != crcsum)
				continue;
		} else
			continue; /* unrecognized version */
		ctx->valid[i] = 1;
	}
	*ctxp = ctx;
	if (!(ctx->valid[0] || ctx->valid[1])) {
		ctx->current = -1;
		memset(&ctx->curinfo, 0, sizeof(ctx->curinfo));
		return -1;
	} else if (ctx->valid[0] && !ctx->valid[1])
		ctx->current = 0;
	else if (!ctx->valid[0] && ctx->valid[1])
		ctx->current = 1;
	else {
		/* both valid */
		struct device_info *dp1 = (struct device_info *)(ctx->infobuf[1]);
		dp = (struct device_info *)(ctx->infobuf[0]);
		if (dp->sernum == 255 && dp1->sernum == 0)
			ctx->current = 1;
		else if (dp1->sernum == 255 && dp->sernum == 0)
			ctx->current = 0;
		else if (dp1->sernum > dp->sernum)
			ctx->current = 1;
		else
			ctx->current = 0;
	}
	memcpy(&ctx->curinfo, ctx->infobuf[ctx->current], sizeof(ctx->curinfo));
	if (parse_vars(ctx) < 0) {
		/* internal error ? */
		if (!ctx->readonly)
			set_bootdev_writeable_status(ctx->devinfo_dev, false);
		ctx->readonly = true;
	}
	return 0;

} /* find_bootinfo */

/*
 * bootinfo_update
 *
 * Write out a device info block based on the current context.
 */
int
bootinfo_update (struct devinfo_context *ctx)
{
	uint32_t *crcptr;
	struct device_info *info;
	ssize_t n, cnt;
	int idx;

	if (ctx == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (ctx->readonly) {
		errno = EROFS;
		return -1;
	}
	/*
	 * Invalid current index -> initialize
	 */
	if (ctx->current < 0 || ctx->current > 1)
		idx = 0;
	else
		idx = 1 - ctx->current;

	info = (struct device_info *) ctx->infobuf[idx];
	crcptr = (uint32_t *) &ctx->infobuf[idx][DEVINFO_BLOCK_SIZE + EXTENSION_SIZE - sizeof(uint32_t)];
	memset(info, 0, DEVINFO_BLOCK_SIZE);
	memcpy(info->magic, DEVICE_MAGIC, sizeof(info->magic));
	info->devinfo_version = DEVINFO_VERSION_CURRENT;
	info->flags = ctx->curinfo.flags;
	info->failed_boots = ctx->curinfo.failed_boots;
	info->sernum = ctx->curinfo.sernum + 1;
	info->ext_sectors = EXTENSION_SECTOR_COUNT;
	if (pack_vars(ctx, idx) < 0)
		return -1;
	info->crcsum = crc32(0, ctx->infobuf[idx], DEVINFO_BLOCK_SIZE);
	*crcptr = crc32(0, &ctx->infobuf[idx][DEVINFO_BLOCK_SIZE], EXTENSION_SIZE-sizeof(uint32_t));

	if (lseek(ctx->fd, devinfo_offset[idx], SEEK_SET) < 0)
		return -1;
	for (n = 0; n < DEVINFO_BLOCK_SIZE; n += cnt) {
		cnt = write(ctx->fd, ctx->infobuf[idx] + n, DEVINFO_BLOCK_SIZE-n);
		if (cnt < 0)
			return -1;
	}
	if (lseek(ctx->fd, extension_offset[idx], SEEK_SET) < 0)
		return -1;
	for (n = 0; n < EXTENSION_SIZE; n += cnt) {
		cnt = write(ctx->fd, ctx->infobuf[idx] + DEVINFO_BLOCK_SIZE + n, EXTENSION_SIZE-n);
		if (cnt < 0)
			return -1;
	}

	return 0;

} /* bootinfo_update */

/*
 * close_bootinfo
 *
 * Cleans up a context, freeing memory and closing open channels.
 */
static int
close_bootinfo (struct devinfo_context *ctx, bool keeplock)
{
	int lockfd = -1;

	if (ctx == NULL)
		return lockfd;
	if (!ctx->readonly)
		set_bootdev_writeable_status(ctx->devinfo_dev, false);
	if (ctx->fd >= 0)
		close(ctx->fd);
	if (keeplock)
		lockfd = ctx->lockfd;
	else if (ctx->lockfd >= 0)
		close(ctx->lockfd);
	ctx->fd = -1;
	ctx->lockfd = -1;
	free_vars(ctx->vars);
	free(ctx);

	return lockfd;

} /* close_bootinfo */

/*
 * bootinfo_close
 *
 * Public API for close_bootinfo.
 */
void
bootinfo_close (struct devinfo_context *ctx)
{
	if (ctx != NULL)
		close_bootinfo(ctx, false);

} /* bootinfo_close */

/*
 * bootinfo_open
 *
 * Open a context for using bootinfo
 * Flags:
 *    BOOTINFO_O_RDONLY      - open read-only, otherwise will be read-write
 *    BOOTINFO_O_FORCE_INIT  - init in-storage structures even if present
 *
 * If ctxp is non-NULL, the initialized context is left open for
 * further bootinfo API calls.
 */
int
bootinfo_open (struct devinfo_context **ctxp, unsigned int flags)
{
	int i, fd = -1, lockfd = -1;
	bool reset_bootdev = false;
	ssize_t n, cnt;
	struct devinfo_context *ctx = NULL;
	uint8_t *buf = NULL;
	struct info_var *var, *preserve_list = NULL;
	char devinfo_dev[PATH_MAX];

	if (ctxp == NULL || ((flags & BOOTINFO_O_RDONLY) != 0 &&
			     (flags & BOOTINFO_O_FORCE_INIT) != 0)) {
		errno = EINVAL;
		return -1;
	}

	if (find_storage_dev(devinfo_dev, sizeof(devinfo_dev)) < 0) {
		*ctxp = NULL;
		return -1;
	}

	if ((flags & BOOTINFO_O_RDONLY) != 0)
		return find_bootinfo(true, ctxp, devinfo_dev);

	/*
	 * For read-write opens, we initialize the in-storage
	 * structures if find_bootinfo returns an error. If it
	 * does *not* return an error, we only initialize if
	 * the FORCE_INIT flag is set.
	 */
	if (find_bootinfo(false, &ctx, devinfo_dev) == 0 &&
	    ctx != NULL &&
	    (flags & BOOTINFO_O_FORCE_INIT) == 0) {
		*ctxp = ctx;
		return 0;
	}
	/*
	 * Initialization code below here.
	 *
	 *
	 * Preserve variables that begin with an underscore.
	 * The linked list we build here gets reused in the new
	 * context we create after initialization as the new variable
	 * list.
	 */
	if (ctx != NULL) {
		struct info_var *prev = NULL;
		for (var = ctx->vars; var != NULL; var = var->next) {
			if (*var->name == '_') {
				struct info_var *varcopy;
				size_t namelen = strlen(var->name);
				size_t vallen = strlen(var->value);
				varcopy = calloc(1, sizeof(*var) + namelen + vallen + 2);
				if (varcopy == NULL) {
					close_bootinfo(ctx, false);
					*ctxp = NULL;
					return -1;
				}
				varcopy->name = (char *)(varcopy + 1);
				memcpy(varcopy->name, var->name, namelen);
				varcopy->value = varcopy->name + namelen + 1;
				memcpy(varcopy->value, var->value, vallen);
				if (prev == NULL)
					prev = preserve_list = varcopy;
				else
					prev->next = varcopy;
				prev = varcopy;
			}
		}
		lockfd = close_bootinfo(ctx, true);
	} else
		lockfd = -1;

	buf = calloc(1, DEVINFO_BLOCK_SIZE + EXTENSION_SIZE);
	if (buf == NULL)
		goto error_depart;
	reset_bootdev = set_bootdev_writeable_status(devinfo_dev, true);
	fd = open(devinfo_dev, O_RDWR|O_DSYNC);
	if (fd < 0)
		goto error_depart;
	/*
	 * Initialize the header block in both copies
	 */
	for (i = 0; i < 2; i++) {
		if (lseek(fd, devinfo_offset[i], SEEK_SET) < 0)
			break;
		for (n = 0; n < DEVINFO_BLOCK_SIZE; n += cnt) {
			cnt = write(fd, buf+n, DEVINFO_BLOCK_SIZE-n);
			if (cnt < 0)
				break;
		}
		if (n < DEVINFO_BLOCK_SIZE)
			break;
		if (lseek(fd, extension_offset[i], SEEK_SET) < 0)
			break;
		for (n = 0; n < EXTENSION_SIZE; n += cnt) {
			cnt = write(fd, buf+DEVINFO_BLOCK_SIZE+n, EXTENSION_SIZE-n);
			if (cnt < 0)
				break;
		}
		if (n < EXTENSION_SIZE)
			break;
	}
	/*
	 * Both header block writes must succeed
	 */
	if (i < 2) {
		errno = EIO;
		goto error_depart;
	}

	ctx = calloc(1, sizeof(struct devinfo_context));
	if (ctx == NULL)
		goto error_depart;
	ctx->fd = fd;
	ctx->lockfd = lockfd;
	ctx->current = -1;
	ctx->vars = preserve_list;
	*ctxp = ctx;
	free(buf);
	return bootinfo_update(ctx);

  error_depart:
	if (fd >= 0)
		close(fd);
	if (lockfd >= 0)
		close(lockfd);
	if (reset_bootdev)
		set_bootdev_writeable_status(devinfo_dev, false);
	if (buf != NULL)
		free(buf);
	if (ctx != NULL)
		free(ctx);
	*ctxp = NULL;
	free_vars(preserve_list);
	return -1;


} /* bootinfo_open */

/*
 * bootinfo_mark_successful
 *
 * Clears the boot-in-progress flag to indicate a successful boot.
 * If failed_boot_count is non-NULL, the number of recorded boot
 * failures is passed back to the caller.
 *
 * This should be called after the system has booted past the point
 * where it can be considered successful.
 */
int
bootinfo_mark_successful (struct devinfo_context *ctx,
			  unsigned int *failed_boot_count)
{
	int ret = -1;

	if (ctx == NULL)
		errno = EINVAL;
	else if (ctx->readonly)
		errno = EROFS;
	else {
		ctx->curinfo.flags &= ~FLAG_BOOT_IN_PROGRESS;
		if (failed_boot_count != NULL)
			*failed_boot_count = ctx->curinfo.failed_boots;
		ctx->curinfo.failed_boots = 0;
		ret = bootinfo_update(ctx);
	}

	return ret;

} /* bootinfo_mark_successful */

/*
 * bootinfo_boot_in_progress
 *
 * Marks the current boot as "in progress", recording a boot
 * failure if the in-progress flag was already set.
 * If failed_boot_count is non-NULL, the number of recorded boot
 * failures is passed back to the caller.
 *
 * This should be called near the beginning of the system boot
 * sequence (e.g., from a bootloader or the initrd phase of
 * system startup). If the returned failure count exceeds a
 * threshold, the caller should initiate a failover or recovery
 * mechanism.
 */
int
bootinfo_mark_in_progress (struct devinfo_context *ctx,
			   unsigned int *failed_boot_count)
{
	int ret = -1;

	if (ctx == NULL)
		errno = EINVAL;
	else if (ctx->readonly)
		errno = EROFS;
	else {
		if (ctx->curinfo.flags & FLAG_BOOT_IN_PROGRESS)
			ctx->curinfo.failed_boots += 1;
		else
			ctx->curinfo.flags |= FLAG_BOOT_IN_PROGRESS;
		if (failed_boot_count != NULL)
			*failed_boot_count = ctx->curinfo.failed_boots;
		ret = bootinfo_update(ctx);
	}
	return ret;

} /* bootinfo_mark_in_progress */

/*
 * Getters for boot info fields
 *
 */
int
bootinfo_is_in_progress (struct devinfo_context *ctx)
{
	if (ctx == NULL) {
		errno = EINVAL;
		return -1;
	}
	return (ctx->curinfo.flags & FLAG_BOOT_IN_PROGRESS) != 0 ? 1 : 0;
}

int
bootinfo_devinfo_version (struct devinfo_context *ctx)
{
	if (ctx == NULL) {
		errno = EINVAL;
		return -1;
	}
	return (int) ctx->curinfo.devinfo_version;
}

int
bootinfo_failed_boot_count (struct devinfo_context *ctx)
{
	if (ctx == NULL) {
		errno = EINVAL;
		return -1;
	}
	return (int) ctx->curinfo.failed_boots;
}

int
bootinfo_extension_sectors (struct devinfo_context *ctx)
{
	if (ctx == NULL) {
		errno = EINVAL;
		return -1;
	}
	return (int) ctx->curinfo.ext_sectors;
}

/*
 * bootinfo_bootvar_iterate
 *
 * Iterates through the list of boot variables.
 * Set itercontext to NULL before first call, and
 * do not modify it, or call any other function in the
 * bootinfo_bootvar API, between calls - you must restart
 * the iteration from the beginning in that case.
 *
 * Negative return code on error.
 * Zero return code on success, with name and value set to NULL
 * if the end of the list has been passed.
 */
int
bootinfo_bootvar_iterate (struct devinfo_context *ctx,
			  void **itercontext,
			  char **name, char **value)
{
	struct info_var *var;

	if (ctx == NULL || itercontext == NULL || name == NULL || value == NULL) {
		errno = EINVAL;
		return -1;
	}
	*name = *value = NULL;
	if (*itercontext == NULL) {
		var = ctx->vars;
	} else {
		var = *itercontext;
		var = var->next;
	}
	*itercontext = var;
	if (var != NULL) {
		*name = var->name;
		*value = var->value;
	}
	return 0;

} /* bootinfo_bootvar_iterate */

/*
 * bootinfo_bootvar_get
 *
 * Retrieves a single boot variable by name.
 * The returned value pointer is to a null-terminated
 * printable character string and should be treated
 * as read-only and not freeable.
 */
int
bootinfo_bootvar_get (struct devinfo_context *ctx,
		      const char *name, char **value)
{
	void *iterctx = 0;
	char *vname, *vval;
	int ret;

	if (ctx == NULL || name == NULL || value == NULL) {
		errno = EINVAL;
		return -1;
	}

	for (ret = bootinfo_bootvar_iterate(ctx, &iterctx, &vname, &vval);
	     ret >= 0 && vname != NULL;
	     ret = bootinfo_bootvar_iterate(ctx, &iterctx, &vname, &vval)) {
		if (strcmp(name, vname) == 0) {
			*value = vval;
			return 0;
		}
	}
	errno = ENOENT;
	return -1;

} /* bootinfo_bootvar_get */

/*
 * bootinfo_bootvar_set
 *
 * Sets or deletes a variable. To delete, either pass NULL as
 * the value pointer, or use a null string as the value.
 * Caller must call bootinfo_update() to finalize the set
 * before freeing the name or value strings.
 *
 */
int
bootinfo_bootvar_set (struct devinfo_context *ctx, const char *name,
		      const char *value)
{
	struct info_var *var, *prev;

	if (ctx == NULL || name == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (ctx->readonly) {
		errno = EROFS;
		return -1;
	}
	/*
	 * Check for a null (0-length) value and just set value to NULL
	 * to indicate that we want to delete the variable in that
	 * case.
	 */
	if (value != NULL && *value == '\0')
		value = NULL;
	/*
	 * Variable names must begin with a letter or underscore,
	 * and can contain letters, digits, or underscores.
	 */
	if (*name != '_' && !isalpha(*name)) {
		errno = EINVAL;
		return -1;
	} else {
		const char *cp;
		for (cp = name + 1; *cp != '\0'; cp++) {
			if (!(*cp == '_' || isalnum(*cp))) {
				errno = EINVAL;
				return -1;
			}
		}
	}
	/*
	 * Values may only contain printable characters
	 */
	if (value != NULL) {
		const char *cp;
		for (cp = value; *cp != '\0'; cp++) {
			if (!isprint(*cp)) {
				errno = EINVAL;
				return -1;
			}
		}
	}

	if (strlen(name) >= DEVINFO_BLOCK_SIZE) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (value != NULL) {
		size_t vallen = strlen(value);
		size_t s = strlen(name) + vallen + 2;
		if (vallen >= MAX_VALUE_SIZE ||
		    ctx->varsize + s > MAX_VALUE_SIZE) {
			errno = EMSGSIZE;
			return -1;
		}
	}

	for (var = ctx->vars, prev = NULL;
	     var != NULL && strcmp(name, var->name) != 0;
	     prev = var, var = var->next);

	if (var == NULL) {
		if (value == NULL) {
			errno = ENOENT;
			return -1;
		}
		var = calloc(1, sizeof(struct info_var));
		if (var == NULL)
			return -1;
		var->name = (char *) name;
		var->value = (char *) value;
		/* Add to end of list */
		if (prev == NULL)
			ctx->vars = var;
		else
			prev->next = var;
	} else if (value == NULL) {
		/* Deleting found variable */
		if (prev == NULL)
			ctx->vars = var->next;
		else
			prev->next = var->next;
		free(var);
	} else
		/* Changing value of found variable */
		var->value = (char *) value;

	return 0;

} /* bootinfo_bootvar_set */
