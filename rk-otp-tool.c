/* SPDX-License-Identifier: MIT */
/*
 * rk-otp-tool.c
 *
 * Tool for getting/setting machine ID in the non-protected OTP on RK356x/RK3588
 *
 * Copyright (c) 2024, Matthew Madison
 */

#include <stdio.h>
#include <stdbool.h>
#include <getopt.h>
#include <string.h>
#include <ctype.h>
#include <libgen.h>
#include <tee_client_api.h>

#define RKSTORAGE_TA_UUID { 0x2d26d8a8, 0x5134, 0x4dd8, { 0xb3, 0x2f, 0xb3, 0x4b, 0xce, 0xeb, 0xc4, 0x71 } }
#define RKSTORAGE_CMD_READ_ENABLE_FLAG		5
#define RKSTORAGE_CMD_WRITE_OEM_NP_OTP		12
#define RKSTORAGE_CMD_READ_OEM_NP_OTP		13

typedef int (*option_routine_t)(void);
static char machineid[32];
static char *progname;

static struct option options[] = {
	{ "check-secure-boot",	no_argument,		0, 's' },
	{ "show-machine-id",	no_argument,		0, 'm' },
	{ "set-machine-id",	required_argument,	0, 'M' },
	{ "help",		no_argument,		0, 'h' },
	{ 0,			0,			0, 0   }
};
static const char *shortopts = ":mM:hs";

static char *optarghelp[] = {
	"--check-secure-boot  ",
	"--show-machine-id    ",
	"--set-machine-id     ",
	"--help               ",
};

static char *opthelp[] = {
	"check that the verified-boot flag is set for secure boot",
	"show machine ID programmed into the OTP non-protected OEM zone",
	"program a machine ID into the OTP non-protected OEM zone, arg is 32-byte hex string",
	"display this help text"
};


static bool
allsame (char c, const void *buf, size_t len)
{
	const char *cp;
	for (cp = buf; len > 0; cp++, len--)
		if (*cp != c)
			return false;
	return true;
}

static void
print_usage (void)
{
	int i;
	printf("\nUsage:\n");
	printf("\t%s <option>\n\n", progname);
	printf("Options (use only one per invocation):\n");
	for (i = 0; i < sizeof(options)/sizeof(options[0]) && options[i].name != 0; i++) {
		printf(" %s\t%c%c\t%s\n",
		       optarghelp[i],
		       (options[i].val == 0 ? ' ' : '-'),
		       (options[i].val == 0 ? ' ' : options[i].val),
		       opthelp[i]);
	}

} /* print_usage */

typedef enum {
	OTP_READ,
	OTP_WRITE,
} otp_optype;

static int
access_oem_np_otp_zone (otp_optype op, unsigned int offset, void *buf, size_t bufsize)
{
	TEEC_Result result;
	TEEC_Context ctx;
	TEEC_Session sess;
	TEEC_Operation oper;
	TEEC_UUID rkstorage_uuid = RKSTORAGE_TA_UUID;
	uint32_t cmd, origin;
	int retval;

	memset(&oper, 0, sizeof(oper));

	switch (op) {
		case OTP_READ:
			oper.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
							   TEEC_MEMREF_TEMP_OUTPUT,
							   TEEC_NONE, TEEC_NONE);
			oper.params[0].value.a = offset;
			oper.params[1].tmpref.size = bufsize;
			oper.params[1].tmpref.buffer = buf;
			cmd = RKSTORAGE_CMD_READ_OEM_NP_OTP;
			break;
		case OTP_WRITE:
			oper.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
							   TEEC_MEMREF_TEMP_INPUT,
							   TEEC_NONE, TEEC_NONE);
			oper.params[0].value.a = offset;
			oper.params[1].tmpref.size = bufsize;
			oper.params[1].tmpref.buffer = buf;
			cmd = RKSTORAGE_CMD_WRITE_OEM_NP_OTP;
			break;
		default:
			fprintf(stderr, "Invalid OTP operation code: %u\n", (unsigned int) op);
			return -1;
	}
	result = TEEC_InitializeContext(NULL, &ctx);
	if (result != TEEC_SUCCESS) {
		fprintf(stderr, "Error initializing TEE client context: 0x%x\n", result);
		return -1;
	}
	result = TEEC_OpenSession(&ctx, &sess, &rkstorage_uuid,
				  TEEC_LOGIN_PUBLIC, NULL, NULL, &origin);
	if (result != TEEC_SUCCESS) {
		fprintf(stderr, "Error opening session to rkstorage TA: 0x%x (origin 0x%x)\n",
			result, origin);
		TEEC_FinalizeContext(&ctx);
		return -1;
	}
	result = TEEC_InvokeCommand(&sess, cmd, &oper, &origin);
	if (result == TEEC_SUCCESS)
		retval = 0;
	else {
		fprintf(stderr, "Error invoking command %u: 0x%x (origin 0x%x)\n",
			cmd, result, origin);
		retval = -1;
	}
	TEEC_CloseSession(&sess);
	TEEC_FinalizeContext(&ctx);
	return retval;
}

static int
get_secure_boot_enable_flag (bool *enabled)
{
	TEEC_Result result;
	TEEC_Context ctx;
	TEEC_Session sess;
	TEEC_Operation oper;
	TEEC_UUID rkstorage_uuid = RKSTORAGE_TA_UUID;
	uint32_t cmd, origin, vbootflag;
	int retval;

	memset(&oper, 0, sizeof(oper));

	oper.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_OUTPUT,
					   TEEC_NONE, TEEC_NONE, TEEC_NONE);
	oper.params[0].tmpref.size = sizeof(vbootflag);
	oper.params[0].tmpref.buffer = &vbootflag;
	cmd = RKSTORAGE_CMD_READ_ENABLE_FLAG;
	result = TEEC_InitializeContext(NULL, &ctx);
	if (result != TEEC_SUCCESS) {
		fprintf(stderr, "Error initializing TEE client context: 0x%x\n", result);
		return -1;
	}

	result = TEEC_OpenSession(&ctx, &sess, &rkstorage_uuid,
				  TEEC_LOGIN_PUBLIC, NULL, NULL, &origin);
	if (result != TEEC_SUCCESS) {
		fprintf(stderr, "Error opening session to rkstorage TA: 0x%x (origin 0x%x)\n",
			result, origin);
		TEEC_FinalizeContext(&ctx);
		return -1;
	}
	result = TEEC_InvokeCommand(&sess, cmd, &oper, &origin);
	if (result == TEEC_SUCCESS) {
		retval = 0;
		*enabled = vbootflag == 0xff;
	} else {
		fprintf(stderr, "Error invoking command %u: 0x%x (origin 0x%x)\n",
			cmd, result, origin);
		retval = -1;
	}
	TEEC_CloseSession(&sess);
	TEEC_FinalizeContext(&ctx);
	return retval;
}

/*
 * show_machine_id
 *
 * Emits just programmed machine ID.
 * Mainly for script use.
 *
 */
static int
show_machine_id (void)
{
	char machid[32];

	if (access_oem_np_otp_zone(OTP_READ, 0, machid, sizeof(machid)) < 0)
		return 1;
	if (allsame('\0', machid, sizeof(machid))) {
		fprintf(stderr, "Machine ID not programmed and locked\n");
		return 1;
	}
	printf("%s\n", machid);

	return 0;

} /* show_machine_id */

/*
 * set_machine_id
 *
 * Programs the 32-byte machine ID into the OTP OEM NP zone
 * at offset 0.
 *
 * Validation checks during command processing:
 *   - machine ID is 32 bytes, all hex digits, and non-zero
 * Validation checks before programming:
 *   - OEM NP zone is all zeros
 */
static int
set_machine_id (void)
{

	char curr_machid[32];
	if (access_oem_np_otp_zone(OTP_READ, 0, curr_machid, sizeof(curr_machid)) < 0)
		return 1;
	if (!allsame('\0', curr_machid, sizeof(curr_machid))) {
		fprintf(stderr, "machine ID already programmed: %s\n", curr_machid);
		return 1;
	}
	if (access_oem_np_otp_zone(OTP_WRITE, 0, machineid, sizeof(machineid)) < 0)
		return 1;

	return 0;

} /* set_machine_id */

static int
show_secure_boot (void)
{
	bool enabled;
	if (get_secure_boot_enable_flag(&enabled) < 0)
		return 1;
	printf("Secure boot %sABLED\n", (enabled ? "EN" : "DIS"));
	return 0;
}

/*
 * main program
 */
int
main (int argc, char * const argv[])
{
	int c, which;
	option_routine_t dispatch = NULL;
	char *argv0_copy = strdup(argv[0]);
	bool machineid_ok;

	progname = basename(argv0_copy);

	if (argc < 2) {
		print_usage();
		return 1;
	}

	c = getopt_long_only(argc, argv, shortopts, options, &which);
	if (c == -1) {
		perror("getopt");
		print_usage();
		return 1;
	}

	switch (c) {

		case 'h':
			print_usage();
			return 0;
		case 'm':
			dispatch = show_machine_id;
			break;
		case 'M':
			machineid_ok = false;
			if (strlen(optarg) == sizeof(machineid)) {
				char *cp;
				for (cp = optarg; *cp != '\0' && isxdigit(*cp); cp++);
				if (*cp == '\0' && !allsame('0', optarg, strlen(optarg))) {
					memcpy(machineid, optarg, sizeof(machineid));
					machineid_ok = true;
				}
			}
			if (!machineid_ok) {
				fprintf(stderr, "Error: machine-id requires 32-byte non-zero hex string as argument\n");
				print_usage();
				return 1;
			}
			dispatch = set_machine_id;
			break;
		case 's':
			dispatch = show_secure_boot;
			break;
		default:
			fprintf(stderr, "Error: unrecognized option\n");
			print_usage();
			return 1;
	}

	if (dispatch == NULL) {
		fprintf(stderr, "Error in option processing\n");
		return 1;
	}

	return dispatch();

} /* main */
