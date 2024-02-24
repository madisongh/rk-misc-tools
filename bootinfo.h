#ifndef bootinfo_h_included
#define bootinfo_h_included
/* Copyright (c) 2022, Matthew Madison */

struct devinfo_context;
typedef struct devinfo_context bootinfo_ctx_t;

/*
 * Flags for bootinfo_open
 */
#define BOOTINFO_O_RDONLY	(1U<<0)
#define BOOTINFO_O_FORCE_INIT	(1U<<1)

int bootinfo_open(bootinfo_ctx_t **ctxp, unsigned int flags);
int bootinfo_mark_successful(bootinfo_ctx_t *ctx, unsigned int *failed_boot_count);
int bootinfo_mark_in_progress(bootinfo_ctx_t *ctx, unsigned int *failed_boot_count);
int bootinfo_is_in_progress(bootinfo_ctx_t *ctx);
int bootinfo_devinfo_version(bootinfo_ctx_t *ctx);
int bootinfo_failed_boot_count(bootinfo_ctx_t *ctx);
int bootinfo_extension_sectors(bootinfo_ctx_t *ctx);
int bootinfo_bootvar_iterate(bootinfo_ctx_t *ctx, void **iterctx, char **name, char **value);
int bootinfo_bootvar_get(bootinfo_ctx_t *ctx, const char *name, char **value);
int bootinfo_bootvar_set(bootinfo_ctx_t *ctx, const char *name, const char *value);
int bootinfo_update(bootinfo_ctx_t *ctx);
void bootinfo_close(bootinfo_ctx_t *ctx);

#endif /* bootinfo_h_included */
