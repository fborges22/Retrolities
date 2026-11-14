#ifndef ARROW_TEST_DOS_H
#define ARROW_TEST_DOS_H
typedef void (*test_vector)(void);
test_vector _dos_getvect(unsigned n);
void _dos_setvect(unsigned n, test_vector handler);
#endif
