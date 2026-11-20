/* Polaris - configuration helpers that need the device tables. */
#ifndef CONFIG_H
#define CONFIG_H

#include "polaris.h"
#include "disk.h"
#include "atapi.h"

void entry_from_part(struct pl_entry *e, struct part *p);
void entry_from_atapi(struct pl_entry *e, struct atadev *a);
void entry_simple(struct pl_entry *e, u8 kind, u8 drive);
void cfg_autodetect(struct pl_config *c);

/* provided by the program (stage 2 or installer) */
int  cfg_store(void);            /* write cfg back to its medium */
extern u8 cfg_drive;             /* drive that holds the configuration */

#endif
