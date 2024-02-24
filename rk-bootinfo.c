/* SPDX-License-Identifier: MIT */
/*
 * rk-bootinfo.c
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
 * Copyright (c) 2019-2024, Matthew Madison
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
#include <getopt.h>
#include <ctype.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <zlib.h>
#include "bootinfo.h"

#define MAX_BOOT_FAILURES 3

static char *progname;

static struct option options[] = {
	{ "boot-success",	no_argument,		0, 'b' },
	{ "check-status",	no_argument,		0, 'c' },
	{ "initialize",		no_argument,		0, 'I' },
	{ "show",		no_argument,		0, 's' },
	{ "omit-name",		no_argument,		0, 'n' },
	{ "from-file",		required_argument,	0, 'f' },
	{ "force-initialize",	no_argument,		0, 'F' },
	{ "get-variable",	no_argument,		0, 'v' },
	{ "set-variable",	no_argument,		0, 'V' },
	{ "help",		no_argument,		0, 'h' },
	{ "version",		no_argument,		0, 0   },
	{ 0,			0,			0, 0   }
};
static const char *shortopts = ":bcIsnf:FvVh";

static char *optarghelp[] = {
	"--boot-success	      ",
	"--check-status	      ",
	"--initialize	      ",
	"--show		      ",
	"--omit-name	      ",
	"--from-file FILE     ",
	"--force-initialize   ",
	"--get-variable	      ",
	"--set-variable	      ",
	"--help		      ",
	"--version	      ",
};

static char *opthelp[] = {
	"update boot info to record successful boot",
	"increment boot counter and check it is under limit",
	"initialize the device info area",
	"show boot counter information",
	"omit variable name in output (for use with --get-variable)",
	"take variable value from FILE (for use with --set-variable)",
	"force initialization even if bootinfo already initialized (for use with --initialize)",
	"get the value of a stored variable by name, list all if no name specified",
	"set the value of a stored variable (delete if no value)",
	"display this help text",
	"display version information"
};

/*
 * print_usage
 */
static void
print_usage (void)
{
	int i;
	printf("\nUsage:\n");
	printf("\t%s\n", progname);
	printf("Options:\n");
	for (i = 0; i < sizeof(options)/sizeof(options[0]) && options[i].name != 0; i++) {
		printf(" %s\t%c%c\t%s\n",
		       optarghelp[i],
		       (options[i].val == 0 ? ' ' : '-'),
		       (options[i].val == 0 ? ' ' : options[i].val),
		       opthelp[i]);
	}

} /* print_usage */

static int
boot_devinfo_init(int force_init)
{
	bootinfo_ctx_t *ctx;

	if (bootinfo_open(&ctx, force_init ? BOOTINFO_O_FORCE_INIT : 0) < 0) {
		perror("bootinfo_open");
		return 1;
	}
	bootinfo_close(ctx);
	return 0;

} /* boot_devinfo_init */

static int
boot_successful(void)
{
	bootinfo_ctx_t *ctx;
	unsigned int failed_boots;

	if (bootinfo_open(&ctx, 0) < 0) {
		perror("bootinfo_open");
		return 1;
	}
	if (bootinfo_mark_successful(ctx, &failed_boots) < 0) {
		perror("bootinfo_mark_successful");
		bootinfo_close(ctx);
		return 1;
	}
	bootinfo_close(ctx);
	fprintf(stderr, "Failed boot count: %u\n", failed_boots);
	return 0;

} /* boot_successful */

static int
boot_check_status(void)
{
	bootinfo_ctx_t *ctx;
	unsigned int failed_boots;
	int rc = 0;

	if (bootinfo_open(&ctx, 0) < 0) {
		perror("bootinfo_open");
		return 1;
	}
	if (bootinfo_mark_in_progress(ctx, &failed_boots) < 0) {
		perror("bootinfo_mark_in_progress");
		rc = 1;
	} else if (failed_boots >= MAX_BOOT_FAILURES) {
		fprintf(stderr, "Too many boot failures, exit with error to signal boot slot switch\n");
		rc = 77;
		/* clear the boot-in-progress status for the next check after the slot switch */
		bootinfo_mark_successful(ctx, NULL);
	}
	bootinfo_close(ctx);
	return rc;

} /* boot_check_status */

/*
 * show_bootinfo
 *
 * Prints out the boot info header information.
 */
static int
show_bootinfo(void) {

	bootinfo_ctx_t *ctx;
	int sectors;

	if (bootinfo_open(&ctx, BOOTINFO_O_RDONLY) < 0) {
		perror("bootinfo_open");
		return 1;
	}
	sectors = bootinfo_extension_sectors(ctx);
	printf("devinfo version:	%d\n"
	       "Boot in progress:	%s\n"
	       "Failed boots:		%d\n"
	       "Extension space:	%d sector%s\n",
	       bootinfo_devinfo_version(ctx),
	       bootinfo_is_in_progress(ctx) ? "YES" : "NO",
	       bootinfo_failed_boot_count(ctx),
	       sectors, (sectors == 1 ? "" : "s"));
	bootinfo_close(ctx);
	return 0;

} /* show_bootinfo */

/*
 * show_bootvar
 *
 * Prints out the value of a variable, or
 * all var=value settings if varname == NULL
 */
int
show_bootvar (const char *name, int omitname)
{
	bootinfo_ctx_t *ctx;
	void *iterctx = 0;
	char *vname, *value;
	int ret;
	int found = (name == NULL) ? 1 : 0;

	if (bootinfo_open(&ctx, BOOTINFO_O_RDONLY) < 0) {
		perror("bootinfo_open");
		return 1;
	}
	for (ret = bootinfo_bootvar_iterate(ctx, &iterctx, &vname, &value);
	     ret >= 0 && vname != NULL;
	     ret = bootinfo_bootvar_iterate(ctx, &iterctx, &vname, &value)) {
		if (name == NULL || strcmp(name, vname) == 0) {
			found = 1;
			if (omitname)
				printf("%s\n", value);
			else
				printf("%s=%s\n", vname, value);
			if (name != NULL)
				break;
		}
	}
	bootinfo_close(ctx);
	if (!found) {
		fprintf(stderr, "not found: %s\n", name);
		return 1;
	}
	return 0;

} /* show_bootvar */

/*
 * set_bootvar
 *
 * Sets or deletes a variable.
 */
int
set_bootvar (const char *name, const char *value, char *inputfile)
{
	bootinfo_ctx_t *ctx;
	int ret = 0;
	static char valuebuf[512*1024];

	if (inputfile != NULL) {
		FILE *fp;
		ssize_t n, cnt;

		if ((value != NULL) || strchr(name, '=') != NULL) {
			fprintf(stderr, "cannot specify both value and input file\n");
			return 1;
		}
		if (strcmp(inputfile, "-") == 0)
			fp = stdin;
		else {
			fp = fopen(inputfile, "r");
			if (fp == NULL) {
				perror(inputfile);
				return 1;
			}
		}
		for (n = 0; n < sizeof(valuebuf); n += cnt) {
			cnt = fread(valuebuf + n, sizeof(char), sizeof(valuebuf)-n, fp);
			if (cnt < sizeof(valuebuf)-n) {
				if (feof(fp)) {
					n += cnt;
					break;
				}
				fprintf(stderr, "error reading %s\n",
					(fp == stdin ? "input" : inputfile));
				if (fp != stdin)
					fclose(fp);
				return 1;
			}
		}
		if (fp != stdin)
			fclose(fp);
		if (n >= sizeof(valuebuf)-1) {
			fprintf(stderr, "input value too large\n");
			return 1;
		}
		valuebuf[n] = '\0';
		if (strlen(valuebuf) != n) {
			fprintf(stderr, "null character in input value not allowed\n");
			return 1;
		}
		value = valuebuf;
	}

	/*
	 * Allow 'name=value' as a single argument
	 * Or 'name=' to unset a variable
	 */
	if (value == NULL) {
		char *cp = strchr(name, '=');
		if (cp != NULL) {
			if (cp == name) {
				fprintf(stderr, "invalid variable name\n");
				return 1;
			}
			*cp = '\0';
			value = cp + 1;
		}
	}
	if (bootinfo_open(&ctx, 0) < 0) {
		perror("bootinfo_open");
		return 1;
	}
	if (bootinfo_bootvar_set(ctx, name, value) < 0) {
		perror("bootinfo_bootvar_set");
		ret = 1;
	}
	if (bootinfo_update(ctx) < 0) {
		perror("bootinfo_update");
		ret = 1;
	}
	bootinfo_close(ctx);
	return ret;

} /* set_bootvar */

/*
 * main program
 */
int
main (int argc, char * const argv[])
{

	int c, which;
	int omitname = 0;
	int force_init = 0;
	char *inputfile = NULL;
	char *argv0_copy = strdup(argv[0]);
	enum {
		nocmd,
		success,
		check,
		show,
		showvar,
		setvar,
		init,
	} cmd = nocmd;

	if (argc < 2) {
		print_usage();
		return 1;
	}

	progname = basename(argv0_copy);

	while ((c = getopt_long_only(argc, argv, shortopts, options, &which)) != -1) {

		switch (c) {
		case 'h':
			print_usage();
			return 0;
		case 'b':
			cmd = success;
			break;
		case 'c':
			cmd = check;
			break;
		case 'I':
			cmd = init;
			break;
		case 's':
			cmd = show;
			break;
		case 'n':
			omitname = 1;
			break;
		case 'f':
			inputfile = strdup(optarg);
			break;
		case 'F':
			force_init = 1;
			break;
		case 'v':
		case 'V':
			if (cmd != nocmd) {
				fprintf(stderr, "Error: only one of -v/-V permitted\n");
				print_usage();
				return 1;
			}
			cmd = (c == 'v' ? showvar : setvar);
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
		} /* switch (c) */

	} /* while getopt */

	switch (cmd) {
	case success:
		return boot_successful();
	case check:
		return boot_check_status();
	case show:
		return show_bootinfo();
	case init:
		return boot_devinfo_init(force_init);
	case showvar:
		if (optind >= argc)
			return show_bootvar(NULL, 0);
		return show_bootvar(argv[optind], omitname);
	case setvar:
		if (optind >= argc) {
			fprintf(stderr, "Error: missing variable name\n");
			print_usage();
			return 1;
		}
		return set_bootvar(argv[optind], (optind < argc - 1 ? argv[optind+1] : NULL), inputfile);
	default:
		break;
	}


	print_usage();
	return 1;

} /* main */
