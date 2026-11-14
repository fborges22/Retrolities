#ifndef ARROW_TEST_BIOS_H
#define ARROW_TEST_BIOS_H
#define _TIME_GETCLOCK 0
unsigned _bios_timeofday(unsigned command, long *ticks);
#endif
