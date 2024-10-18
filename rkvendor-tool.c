/* SPDX-License-Identifier: MIT */
/*
 * rkvendor-tool
 *
 * Tool for reading and modifying fields in the Rockchip-speicific
 * "vendor" storage, using the Rockchip driver interface.
 *
 * Portions adapted from tegra-eeprom-tool:
 *   https://github.com/OE4T/tegra-eeprom-tool
 *
 * Copyright (c) 2024, Matthew Madison
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <libgen.h>
#include <histedit.h>
#include <unistd.h>
#include <ctype.h>
#include <locale.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <misc/rkflash_vendor_storage.h>
#include <net/ethernet.h>

typedef enum {
	SN_ID = 1,
	WIFI_MAC_ID,
	LAN_MAC_ID,
	BT_MAC_ID,
	HDCP_14_HDMI_ID,
	HDCP_14_DP_ID,
	HDCP_2X_ID,
	DRM_KEY_ID,
	PLAYREADY_CERT_ID,
	ATTENTION_KEY_ID,
	PLAYREADY_ROOT_KEY_0_ID,
	PLAYREADY_ROOT_KEY_1_ID,
	HDCP_14_HDMIRX_ID,
	SENSOR_CALIBRATION_ID,
	IMEI_ID,
	LAN_RGMII_DL_ID,
	EINK_VCOM_ID,
	FIRMWARE_VER_ID,
	// must be last
	RKVENDOR_ID___COUNT
} rkvendor_id_type;

#define RKVENDOR_ID_COUNT ((unsigned int) RKVENDOR_ID___COUNT)
#define VENDOR_SN_MAX 513
#define VENDOR_MAX_ETHER 2

static struct RK_VENDOR_REQ *fill_vendor_req(struct RK_VENDOR_REQ *req, bool writing, rkvendor_id_type id, size_t len, void *data)
{
	req->tag = VENDOR_REQ_TAG;
	req->id = (__u16) id;
	if (writing) {
		if (len > sizeof(req->data))
			len = sizeof(req->data);
		req->len = (__u16) len;
		if (len != 0 && data != NULL)
			memcpy(req->data, data, len);
	} else
		req->len = sizeof(req->data);
	return req;
}


struct context_s {
	struct RK_VENDOR_REQ data[RKVENDOR_ID_COUNT];
	bool havedata[RKVENDOR_ID_COUNT];
	bool modified[RKVENDOR_ID_COUNT];
	bool readonly;
	int fd;
};
typedef struct context_s *context_t;

typedef int (*option_routine_t)(context_t ctx, int argc, char * const argv[]);

static int do_help(context_t ctx, int argc, char * const argv[]);
static int do_show(context_t ctx, int argc, char * const argv[]);
static int do_get(context_t ctx, int argc, char * const argv[]);
static int do_set(context_t ctx, int argc, char * const argv[]);
static int do_write(context_t ctx, int argc, char * const argv[]);

static struct {
	const char *name;
	rkvendor_id_type id;
	enum {
		char_string,
		mac_address,
		mac_address_pair,
	} fieldtype;
	size_t maxsize;
} rkvendor_fields[] = {
	{ "serial-number", SN_ID, char_string, VENDOR_SN_MAX },
	{ "wifi-mac",  WIFI_MAC_ID, mac_address, ETH_ALEN },
	{ "bt-mac",  BT_MAC_ID, mac_address, ETH_ALEN },
	{ "ether-macs", LAN_MAC_ID, mac_address_pair, VENDOR_MAX_ETHER * ETH_ALEN },
};
#define RKVENDOR_FIELD_COUNT (sizeof(rkvendor_fields)/sizeof(rkvendor_fields[0]))

static struct {
	const char *cmd;
	option_routine_t rtn;
	const char *help;
} commands[] = {
	{ "show",	do_show,	"show vendor data contents" },
	{ "get",	do_get,		"get value for vendor field" },
	{ "set",	do_set, 	"set a value for a vendor field" },
	{ "help",	do_help, 	"display extended help" },
	// commands not for use in oneshot mode follow
	{ "write",	do_write, 	"write updated vendor data" },
	{ "quit",	NULL,		"exit from program" },
};
static const int non_oneshot_commands = 2;

static struct option options[] = {
	{ "help",		no_argument,		0, 'h' },
	{ 0,			0,			0, 0   }
};
static const char *shortopts = ":d:ch";

static char *optarghelp[] = {
	"--help               ",
};

static char *opthelp[] = {
	"display this help text",
};

static char *progname;
static char promptstr[256];
static int continuation;

static int
get_vendor_data (struct context_s *ctx, int i)
{
	int idx = (int) rkvendor_fields[i].id;
	int ret;
	if (ctx->havedata[idx])
		return 0;
	ret = ioctl(ctx->fd, VENDOR_READ_IO,
		    fill_vendor_req(&ctx->data[idx], false, rkvendor_fields[i].id, 0, 0));
	if (ret) {
		if (errno != EPERM)
			return -1;
		ctx->data[idx].len = 0;
		errno = 0;
	}
	ctx->havedata[idx] = true;
	ctx->modified[idx] = false;
	return 0;
}

static int
set_vendor_data (struct context_s *ctx, int i)
{
	int idx = (int) rkvendor_fields[i].id;
	int ret;
	if (!(ctx->havedata[idx] && ctx->modified[idx]))
		return 0;
	ret = ioctl(ctx->fd, VENDOR_WRITE_IO, &ctx->data[idx]);
	if (ret)
		return -1;
	ctx->modified[idx] = false;
	return 0;
}

static uint8_t
hexdigit (int c)
{
	if (c >= 'a' && c <= 'f')
		return 10 + c - 'a';
	return c - '0';
}

static ssize_t
format_macaddr (char *buf, size_t bufsize, uint8_t *a)
{
	ssize_t n;
	n = snprintf(buf, bufsize-1, "%02x:%02x:%02x:%02x:%02x:%02x",
		     a[0], a[1], a[2], a[3], a[4], a[5]);
	if (n > 0)
		*(buf + n) = '\0';

	return n;

} /* format_macaddr */

static int
parse_macaddr (uint8_t *a, const char *buf)
{
	const char *cp = buf;
	int count = 0;

	/* empty string == all-zero address */
	if (*cp == '\0') {
		memset(a, 0, ETH_ALEN);
		return 0;
	}
	while (*cp != '\0' && count < ETH_ALEN) {
		if (!isxdigit(*cp) || !isxdigit(*(cp+1)))
			break;
		a[count++] = (hexdigit(tolower(*cp)) << 4) | hexdigit(tolower(*(cp+1)));
		cp += 2;
		if (*cp == ':' || *cp == '-')
			cp += 1;
	}
	return (count == 6 && *cp == '\0') ? 0 : -1;

} /* parse_macaddr */

static ssize_t
format_field (context_t ctx, int i, char *strbuf, size_t bufsize)
{
	int reqidx = (int) rkvendor_fields[i].id;
	struct RK_VENDOR_REQ *req = &ctx->data[reqidx];
	ssize_t len = -1;

	if (req->len == 0) {
		*strbuf = '\0';
		return 0;
	}

	switch (rkvendor_fields[i].fieldtype) {
	case char_string:
		len = (ssize_t) req->len;
		if (len >= bufsize)
			len = (ssize_t) bufsize-1;
		memcpy(strbuf, req->data, len);
		strbuf[len] = '\0';
		break;
	case mac_address:
		len = format_macaddr(strbuf, bufsize, req->data);
		break;
	case mac_address_pair:
		len = format_macaddr(strbuf, bufsize, req->data);
		strbuf[len++] = ' ';
		len += format_macaddr(strbuf+len, bufsize-len, req->data + ETH_ALEN);
		break;
	default:
		fprintf(stderr, "Internal error: unknown field type for %d\n", i);
		break;
	}
	return len;

} /* format_field */

static int
parse_fieldname (const char *s)
{
	int i;
	for (i = 0; i < RKVENDOR_FIELD_COUNT && strcasecmp(s, rkvendor_fields[i].name) != 0; i++);
	return i >= RKVENDOR_FIELD_COUNT ? -1 : i;
} /* parse_fieldname */

static void
print_usage (int oneshot)
{
	int i;
	int cmdcount = sizeof(commands)/sizeof(commands[0]);

	if (oneshot) {
		cmdcount -= non_oneshot_commands;
		printf("\nUsage:\n");
		printf("\t%s <option> [<command> [<key>] [<value>]]\n\n", progname);
	}
	printf("Commands:\n");
	for (i = 0; i < cmdcount; i++)
		printf(" %s\t\t%s\n", commands[i].cmd, commands[i].help);
	if (oneshot) {
		printf("\nOptions:\n");
		for (i = 0; i < sizeof(options)/sizeof(options[0]) && options[i].name != 0; i++) {
			printf(" %s\t%c%c\t%s\n",
			       optarghelp[i],
			       (options[i].val == 0 ? ' ' : '-'),
			       (options[i].val == 0 ? ' ' : options[i].val),
			       opthelp[i]);
		}
	}

} /* print_usage */

/*
 * do_help
 *
 * Extended help that lists the valid tag names
 */
static int
do_help (context_t ctx, int argc, char * const argv[])
{

	int i;

	print_usage(0);
	printf("\nRecognized fields:\n");
	for (i = 0; i < RKVENDOR_FIELD_COUNT; i++)
		printf("  %s\n", rkvendor_fields[i].name);
	return 0;

} /* do_help */

/*
 * do_show
 *
 * Print vendor data
 */
static int
do_show (context_t ctx, int argc, char * const argv[])
{
	int i;
	char strbuf[128];

	for (i = 0; i < RKVENDOR_FIELD_COUNT; i++) {
		if (get_vendor_data(ctx, i) < 0) {
			perror(rkvendor_fields[i].name);
			continue;
		}
		if (format_field(ctx, i, strbuf, sizeof(strbuf)) < 0)
			fprintf(stderr, "Error: could not format field '%s'\n", rkvendor_fields[i].name);
		else
			printf("%s: %s\n", rkvendor_fields[i].name, strbuf);
	}

	return 0;

} /* do_show */

/*
 * do_get
 *
 * Get a single value
 */
static int
do_get (context_t ctx, int argc, char * const argv[])
{
	char strbuf[128];
	int i;

	if (argc < 1) {
		fprintf(stderr, "missing required argument: field-name\n");
		return 1;
	}
	i = parse_fieldname(argv[0]);
	if (i < 0) {
		fprintf(stderr, "unrecognized field name: %s\n", argv[0]);
		return 1;
	}
	if (get_vendor_data(ctx, i) < 0) {
		perror(rkvendor_fields[i].name);
		return 1;
	}
	if (format_field(ctx, i, strbuf, sizeof(strbuf)) < 0) {
		fprintf(stderr, "Error: could not format field '%s'\n", rkvendor_fields[i].name);
		return 1;
	}
	printf("%s\n", strbuf);
	return 0;

} /* do_get */


/*
 * do_set
 *
 * Set a single value
 */
static int
do_set (context_t ctx, int argc, char * const argv[])
{
	int i, idx;
	uint8_t addr[ETH_ALEN];
	uint8_t addr_pair[ETH_ALEN*2];
	const char *value;
	size_t len;

	if (argc < 1) {
		fprintf(stderr, "missing field name argument\n");
		return 1;
	}
	if (argc < 2)
		value = "";
	else
		value = argv[1];
	i = parse_fieldname(argv[0]);
	if (i < 0) {
		fprintf(stderr, "unrecognized field name: %s\n", argv[0]);
		return 1;
	}
	if (ctx->readonly) {
		fprintf(stderr, "Error: vendor data is read-only\n");
		return 1;
	}
	idx = (int) rkvendor_fields[i].id;
	if (get_vendor_data(ctx, i) < 0) {
		perror(rkvendor_fields[i].name);
		return 1;
	}

	switch (rkvendor_fields[i].fieldtype) {
	case char_string:
		len = strlen(value);
		if (len >= rkvendor_fields[i].maxsize) {
			fprintf(stderr, "Error: value longer than field length (%zu)\n", rkvendor_fields[i].maxsize-1);
			return 1;
		}
		fill_vendor_req(&ctx->data[idx], true, rkvendor_fields[i].id, len, argv[1]);
		ctx->modified[idx] = true;
		break;
	case mac_address:
		if (parse_macaddr(addr, value) < 0) {
			fprintf(stderr, "Error: could not parse MAC address '%s'\n", argv[1]);
			return 1;
		}
		fill_vendor_req(&ctx->data[idx], true, rkvendor_fields[i].id, sizeof(addr), addr);
		ctx->modified[idx] = true;
		break;
	case mac_address_pair:
		if (parse_macaddr(addr_pair, value) < 0) {
			fprintf(stderr, "Error: could not parse MAC address '%s'\n", argv[1]);
			return 1;
		}
		if (argc > 2) {
			if (parse_macaddr(addr_pair + ETH_ALEN, argv[2]) < 0) {
				fprintf(stderr, "Error: could not parse MAC address '%s'\n", argv[2]);
				return 1;
			}
		} else
			memset(addr_pair + ETH_ALEN, 0, ETH_ALEN);
		fill_vendor_req(&ctx->data[idx], true, rkvendor_fields[i].id, sizeof(addr_pair), addr_pair);
		ctx->modified[idx] = true;
		break;
	default:
		fprintf(stderr, "Internal error: unrecognized field type for '%s'\n", rkvendor_fields[i].name);
		return 2;
	}

	return 0;

} /* do_set */

/*
 * do_write
 *
 * Write EEPROM contents;
 */
static int
do_write (context_t ctx, int argc, char * const argv[])
{
	int i;

	if (ctx->readonly) {
		fprintf(stderr, "Error: vendor data is read-only\n");
		return 1;
	}
	for (i = 0; i < RKVENDOR_FIELD_COUNT; i++) {
		int idx = rkvendor_fields[i].id;
		if (ctx->modified[idx]) {
			if (set_vendor_data(ctx, i) < 0) {
				perror(rkvendor_fields[i].name);
				return 1;
			}
		}
	}
	return 0;

} /* do_write */

static char *prompt (EditLine *e)
{
	return promptstr + (continuation ? 0 : 1);
}
/*
 * command_loop
 *
 */
static int
command_loop (context_t ctx)
{
	option_routine_t dispatch;
	EditLine *el;
	History *hist;
	HistEvent ev;
	const LineInfo *li;
	Tokenizer *tok;
	char *editor;
	const char *line, **argv;
	size_t promptlen;
	int argc, alldone, llen, which, ret = 0, n;

	setlocale(LC_CTYPE, "");
	promptlen = snprintf(promptstr, sizeof(promptstr)-2, "_%s> ", progname);
	promptstr[promptlen] = '\0';
	el = el_init(progname, stdin, stdout, stderr);

	el_set(el, EL_PROMPT, &prompt);
	editor = getenv("EDITOR");
	if (editor != NULL && strchr(editor, ' ') != NULL) {
		char *edtemp = strdup(editor);
		char *sp = strchr(edtemp, ' ');
		*sp = '\0';
		el_set(el, EL_EDITOR, edtemp);
		free(edtemp);
	} else if (editor != NULL)
		el_set(el, EL_EDITOR, editor);
	hist = history_init();
	if (hist != NULL) {
		history(hist, &ev, H_SETSIZE, 100);
		el_set(el, EL_HIST, history, hist);
	}
	el_set(el, EL_SIGNAL, 1);
	tok = tok_init(NULL);
	continuation = alldone = 0;
	while (!alldone && (line = el_gets(el, &llen)) != NULL && llen != 0) {
		li = el_line(el);
		if (!continuation && llen == 1)
			continue;
		argc = 0;
		n = tok_line(tok, li, &argc, &argv, NULL, NULL);
		if (n < 0) {
			fprintf(stderr, "internal error\n");
			continuation = 0;
			continue;
		}
		history(hist, &ev, (continuation ? H_APPEND : H_ENTER), line);
		continuation = n;
		if (continuation)
			continue;
		dispatch = NULL;
		for (which = 0; which < sizeof(commands)/sizeof(commands[0]); which++) {
			if (strcmp(argv[0], commands[which].cmd) == 0) {
				dispatch = commands[which].rtn;
				if (dispatch == NULL)
					alldone = 1;
				break;
			}
		}
		if (which >= sizeof(commands)/sizeof(commands[0]))
			fprintf(stderr, "unrecognized command: %s\n", argv[0]);
		else if (alldone || dispatch == NULL)
			break;
		else
			ret = dispatch(ctx, argc-1, (char * const *)&argv[1]);

		tok_reset(tok);
	}
	if (line == NULL && isatty(fileno(stdin)))
		printf("\n");
	el_end(el);
	tok_end(tok);
	history_end(hist);
	return ret;

} /* command_loop */

/*
 * main program
 */
int
main (int argc, char * const argv[])
{
	int c, which, ret;
	context_t ctx = NULL;
	option_routine_t dispatch = NULL;
	char *argv0_copy = strdup(argv[0]);

	progname = basename(argv0_copy);

	for (;;) {
		c = getopt_long_only(argc, argv, shortopts, options, &which);
		if (c == -1)
			break;


		switch (c) {

		case 'h':
			print_usage(1);
			ret = 0;
			goto depart;
		default:
			fprintf(stderr, "Error: unrecognized option\n");
			print_usage(1);
			ret = 1;
			goto depart;
		}
	}

	argc -= optind;
	argv += optind;

	ctx = calloc(1, sizeof(struct context_s));
	if (ctx == NULL) {
		perror("allocating context structure");
		return errno;
	}
	ctx->fd = open("/dev/vendor_storage", O_RDWR, 0);

	if (argc < 1) {
		ret = command_loop(ctx);
		goto depart;
	}

	for (which = 0; which < sizeof(commands)/sizeof(commands[0])-non_oneshot_commands; which++) {
		if (strcmp(argv[0], commands[which].cmd) == 0) {
			dispatch = commands[which].rtn;
			break;
		}
	}

	if (dispatch == NULL) {
		fprintf(stderr, "Unrecognized command\n");
		ret = 1;
		goto depart;
	}

	argc -= 1;
	argv += 1;

	ret = dispatch(ctx, argc, argv);
depart:
	if (ctx != NULL) {
		do_write(ctx, 0, NULL);
		close(ctx->fd);
		free(ctx);
	}
	free(argv0_copy);
	return ret;

} /* main */
