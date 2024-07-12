/*
 * rk-bootloader-update.c
 *
 * Tool for updating RK35xx u-boot and idblock.
 * Only supports eMMC-based systems.
 *
 * Copyright (c) 2019-2024, Matthew Madison
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>

#ifndef UBOOT_SIZE_KB
#if TARGET == 3588
#define UBOOT_SIZE_KB 4096
#elif TARGET == 3568
#define UBOOT_SIZE_KB 2048
#else
#error unknown TARGET
#endif
#endif /* UBOOT_SIZE_KB */

#ifndef UBOOT_COPIES
#define UBOOT_COPIES 2
#endif /* UBOOT_COPIES */

static char *progname;

static struct option options[] = {
	{ "help",  		no_argument,		0, 'h' },
	{ "verify", 		no_argument,		0, 'v' },
	{ "version",		no_argument,		0, 0   },
	{ 0,			0,			0, 0   }
};
static const char *shortopts = ":hv";

static char *optarghelp[] = {
	"--help               ",
	"--verify             ",
	"--version            ",
};

static char *opthelp[] = {
	"display this help text",
	"verify that bootloader contents match the file contents",
	"display version information"
};

static uint8_t zerobuf[UBOOT_SIZE_KB * 1024];


/*
 * print_usage
 *
 * Does what it says, extracting option strings and
 * help from the arrays defined above.
 *
 * Returns: nothing
 */
static void
print_usage (void)
{
	int i;
	printf("\nUsage:\n");
	printf("\t%s <option> <uboot-img> [<idblock-img>]\n\n", progname);
	printf("Options:\n");
	for (i = 0; i < sizeof(options)/sizeof(options[0]) && options[i].name != 0; i++) {
		printf(" %s\t%c%c\t%s\n",
		       optarghelp[i],
		       (options[i].val == 0 ? ' ' : '-'),
		       (options[i].val == 0 ? ' ' : options[i].val),
		       opthelp[i]);
	}
	printf("\nArguments:\n");
	printf(" <uboot-img>\tpathname of U-Boot FIT image\n");
	printf(" <idblock-img>\tpathname of idblock image\n");

} /* print_usage */

/*
 * read_completely_at
 *
 * Utility function for seeking to a specific offset
 * and reading a fixed number of bytes into a buffer,
 * handling short reads.
 *
 * fd: file descriptor
 * buf: pointer to read buffer
 * bufsiz: number of bytes to read
 * offset: offset from start of file/device
 *
 * Returns: number of bytes read, or
 *          -1 on error (errno set)
 *
 */
static ssize_t
read_completely_at (int fd, void *buf, size_t bufsiz, off_t offset)
{
	ssize_t n, total;
	size_t remain;

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
		return -1;
	for (remain = bufsiz, total = 0; remain > 0; total += n, remain -= n) {
		n = read(fd, (uint8_t *) buf + total, remain);
		if (n <= 0)
			return -1;
	}
	return total;

} /* read_completely_at */

/*
 * write_completely_at
 *
 * Utility function for seeking to a specific offset
 * and writing a fixed number of bytes to a file or device,
 * handling short writes.
 *
 * fd: file descriptor
 * buf: pointer to data to be written
 * bufsiz: number of bytes to write
 * offset: offset from start of file/device
 *
 * Returns: number of bytes written, or
 *          -1 on error (errno set)
 *
 */
static ssize_t
write_completely_at (int fd, void *buf, size_t bufsiz, off_t offset, size_t erase_size)
{
	ssize_t n, total;
	size_t remain;

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
		return -1;
	for (remain = erase_size, total = 0; remain > 0; total += n, remain -= n) {
		n = write(fd, (uint8_t *) zerobuf + total, remain);
		if (n <= 0)
			return -1;
	}
	fsync(fd);
	if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
		return -1;
	for (remain = bufsiz, total = 0; remain > 0; total += n, remain -= n) {
		n = write(fd, (uint8_t *) buf + total, remain);
		if (n <= 0)
			return -1;
	}
	return total;

} /* write_completely_at */

/*
 * process_idblock
 *
 * Updates or verifies the IDBLOCK slots.  On systems booting from eMMC,
 * the idblock is expected at sector 64, with up to 4 backup copies at
 * 1024-sector intervals thereafter.
 *
 * update: true if updating, false if just verifying
 * bootfd: file descriptor for boot device
 * idblock: pointer to new idblock to write (maybe)
 * idblock_len: length of new idblock
 *
 *
 * returns: 0 on success, -1 on error (errno not set), >0 = number of copies
 * needing update
 *
 */
static int
process_idblock (bool update, int bootfd, void *idblock, size_t idblock_len)
{
	static uint8_t idb_buf[1024*512];
	off_t offset;
	int i;
	int mismatched = 0;

	if (idblock_len > sizeof(idb_buf)) {
		fprintf(stderr, "ERR: idblock image size exceeds 512KiB maximum\n");
		return -1;
	}
	if (update)
		printf("idblock: ");
	for (i = 1, offset = 64 * 512; i <= 5; i++, offset += 1024 * 512) {
		if (read_completely_at(bootfd, idb_buf, sizeof(idb_buf), offset) < 0) {
			perror("idblock read");
			return -1;
		}
		if (memcmp(idb_buf, idblock, idblock_len) != 0) {
			mismatched += 1;
			if (update) {
				printf("[copy %d]...", i);
				if (write_completely_at(bootfd, idblock, idblock_len, offset, sizeof(idb_buf)) < 0) {
					printf("[FAIL]\n");
					perror("idblock write");
					return -1;
				}
			}
		}
	}

	if (update) {
		fsync(bootfd);
		printf("[OK]\n");
	}
	return mismatched;

} /* process_idblock */

/*
 * process_uboot
 *
 * Updates the u-boot slots.  On systems booting from eMMC,
 * the SPL looks first for a partition named 'uboot'; if present,
 * it loads from there.  If not present, or loading from the partition
 * fails, it tries loading at sector 16384.  Backup copies at
 * each location are possible, 2 by default (CONFIG_SPL_FIT_IMAGE_MULTIPLE),
 * at intervals controlled by CONFIG_SPL_FIT_IMAGE_KB (4096 for 3588,
 * 2048 for 3566/3568, by default).
 *
 * update: true if updating, false if just verifying
 * bootfd: file descriptor for boot device
 * ubootimg: pointer to new uboot FIT to write (if needed)
 * ubootimg_len: length of ubootimg
 * offset: starting offset
 * copycount: number of copies to check
 *
 * returns: 0 on success, -1 on error (errno not set), >0 = number of copies needing update
 *
 */
static int
process_uboot (bool update, int bootfd, void *ubootimg, size_t ubootimg_len, off_t offset, int copycount)
{
	size_t uboot_size = UBOOT_SIZE_KB * 1024;
	static uint8_t uboot_buf[UBOOT_SIZE_KB * 1024];
	int i;
	int mismatched = 0;

	if (ubootimg_len > uboot_size) {
		fprintf(stderr, "ERR: u-boot FIT image size exceeds %uKiB maximum\n", UBOOT_SIZE_KB);
		return -1;
	}
	if (update)
		printf("uboot: ");
	for (i = 1;  i <= copycount; i++, offset += (off_t) uboot_size) {
		if (read_completely_at(bootfd, uboot_buf, sizeof(uboot_buf), offset) < 0) {
			perror("uboot read");
			return -1;
		}
		if (memcmp(uboot_buf, ubootimg, ubootimg_len) != 0) {
			mismatched += 1;
			if (update) {
				printf("[copy %d]...", i);
				if (write_completely_at(bootfd, ubootimg, ubootimg_len, offset, sizeof(uboot_buf)) < 0) {
					printf("[FAIL]\n");
					perror("uboot write");
					return -1;
				}
			}
		}
	}

	if (update) {
		fsync(bootfd);
		printf("[OK]\n");
	}
	return mismatched;

} /* process_uboot */


/*
 * main program
 */
int
main (int argc, char * const argv[]) {
	int c, which, fd = -1;
	bool update = true;
	struct stat st;
	size_t uboot_len, idblock_len;
	static uint8_t uboot_image[UBOOT_SIZE_KB * 1024];
	static uint8_t idblock_image[512 * 1024];
	int totalcount = 0, count;
	char *argv0_copy = strdup(argv[0]);

	progname = basename(argv0_copy);

	while ((c = getopt_long_only(argc, argv, shortopts, options, &which)) != -1) {
		switch (c) {
			case 'h':
				print_usage();
				return 0;
			case 'v':
				update = false;
				break;
			case 0:
				if (strcmp(options[which].name, "version") == 0) {
					printf("%s\n", VERSION);
					return 0;
				}
				/* fallthrough */
			default:
				fprintf(stderr, "Error: unrecognized option\n");
				print_usage();
				return 1;
		}
	}

	if (optind + 1 >= argc) {
		fprintf(stderr, "Error: missing required argument\n");
		print_usage();
		return 1;
	}

	fd = open(argv[optind], O_RDONLY);
	if (fd < 0 || fstat(fd, &st) != 0) {
		perror(argv[optind]);
		if (fd >= 0)
			close(fd);
		return 1;
	}
	if (st.st_size > sizeof(uboot_image)) {
		fprintf(stderr, "ERR: u-boot image too large\n");
		close(fd);
		return 1;
	}
	uboot_len = read_completely_at(fd, uboot_image, st.st_size, 0);
	if (uboot_len != st.st_size) {
		perror("reading uboot image");
		close(fd);
		return 1;
	}
	close(fd);
	optind += 1;

	fd = open(argv[optind], O_RDONLY);
	if (fd < 0 || fstat(fd, &st) != 0) {
		perror(argv[optind]);
		if (fd >= 0)
			close(fd);
		return 1;
	}
	if (st.st_size > sizeof(idblock_image)) {
		fprintf(stderr, "ERR: idblock image too large\n");
		close(fd);
		return 1;
	}
	idblock_len = read_completely_at(fd, idblock_image, st.st_size, 0);
	if (idblock_len != st.st_size) {
		perror("reading idblock image");
		close(fd);
		return 1;
	}
	close(fd);

	fd = open("/dev/disk/by-partlabel/uboot", (update ? O_RDWR : O_RDONLY));
	if (fd >= 0) {
		off_t endpos;
		int copycount;
		endpos = lseek(fd, 0, SEEK_END);
		if (endpos == (off_t) -1) {
			perror("uboot partition");
			close(fd);
			return 1;
		}
		copycount = (int) (endpos / (UBOOT_SIZE_KB * 1024));
		if (copycount == 0) {
			fprintf(stderr, "uboot partition too small, skipping\n");
			close(fd);
		} else {
			if (copycount > UBOOT_COPIES)
				copycount = UBOOT_COPIES;
			count = process_uboot(update, fd, uboot_image, uboot_len, 0, copycount);
			close(fd);
			if (count < 0) {
				fprintf(stderr, "error processing uboot partition\n");
				return 1;
			}
			totalcount += count;
		}
	}
	fd = open("/dev/mmcblk0", (update ? O_RDWR: O_RDONLY));
	if (fd < 0) {
		perror("/dev/mmcblk0");
		return 1;
	}
	count = process_uboot(update, fd, uboot_image, uboot_len, 16384 * 512, UBOOT_COPIES);
	if (count < 0) {
		fprintf(stderr, "error processing uboot\n");
		close(fd);
		return 1;
	}
	totalcount += count;
	count = process_idblock(update, fd, idblock_image, idblock_len);
	if (count < 0) {
		fprintf(stderr, "error processing idblock\n");
		close(fd);
		return 1;
	}
	totalcount += count;
	if (update) {
		printf("Total update count: %d\n", totalcount);
		return 0;
	} else if (totalcount > 0) {
		fprintf(stderr, "Verification failed, updates needed: %d\n", totalcount);
	}

	return totalcount == 0 ? 0 : 1;

} /* main */
