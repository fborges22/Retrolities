#ifndef BOOT_H
#define BOOT_H

#include "polaris.h"

int  boot_entry(struct pl_entry *e);   /* returns only on failure */
void boot_reboot(void);
int  boot_poweroff(void);
void boot_before_jump(void);           /* provided by stage2.c */

#endif
