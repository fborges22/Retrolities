/* Test-only DOS TSR: inject BIOS keys only when the expected UI is visible.
   The timer handler never calls DOS, allocates memory, or writes a disk. */
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS 128

typedef struct {
    char present[76], absent[48];
    unsigned attr, key;
} Step;

static Step steps[MAX_STEPS];
static volatile unsigned current, count;
static void(__interrupt __far *previous_timer)(void);

static int visible(const char *text, unsigned attr)
{
    volatile unsigned short __far *screen = MK_FP(0xb800, 0);
    unsigned cell, i;

    for (cell = 0; cell < 2000; cell++) {
        for (i = 0; text[i]; i++) {
            if (cell % 80 + i >= 80 || (screen[cell + i] & 255) != (unsigned char)text[i] ||
                (attr && (screen[cell + i] >> 8) != attr))
                break;
        }
        if (!text[i])
            return 1;
    }
    return 0;
}

static void __interrupt __far timer(void)
{
    volatile unsigned short __far *head = MK_FP(0x40, 0x1a);
    volatile unsigned short __far *tail = MK_FP(0x40, 0x1c);
    volatile unsigned char __far *mode = MK_FP(0x40, 0x49);
    unsigned next;
    Step *step;

    if (current < count && *mode == 3 && *head == *tail) {
        step = &steps[current];
        if (visible(step->present, step->attr) && (!step->absent[0] || !visible(step->absent, 0))) {
            next = *tail + 2;
            if (next >= 0x3e)
                next = 0x1e;
            *(volatile unsigned short __far *)MK_FP(0x40, *tail) = step->key;
            *tail = next;
            current++;
        }
    }
    _chain_intr(previous_timer);
}

int main(int argc, char **argv)
{
    FILE *file;
    char line[256], *present, *absent, *key, *end;
    unsigned paragraphs;

    if (argc != 2 || !(file = fopen(argv[1], "r"))) {
        puts("Usage: KEYPLAY script (attr|visible text|absent text|BIOS key in hex)");
        return 1;
    }
    while (fgets(line, sizeof(line), file)) {
        if (count == MAX_STEPS || !(present = strchr(line, '|')) ||
            !(absent = strchr(present + 1, '|')) || !(key = strchr(absent + 1, '|')))
            goto invalid;
        *present++ = *absent++ = *key++ = 0;
        steps[count].attr = (unsigned)strtoul(line, &end, 16);
        if (*end || steps[count].attr > 255 || !*present ||
            strlen(present) >= sizeof(steps[count].present) ||
            strlen(absent) >= sizeof(steps[count].absent))
            goto invalid;
        strcpy(steps[count].present, present);
        strcpy(steps[count].absent, absent);
        steps[count].key = (unsigned)strtoul(key, &end, 16);
        if ((*end && *end != '\n' && *end != '\r') || !steps[count].key)
            goto invalid;
        count++;
    }
    if (ferror(file) || !count)
        goto invalid;
    fclose(file);
    /* The build caps the EXE maximum allocation at its minimum plus 4 KiB,
       leaving conventional memory available for the program under test. */
    paragraphs = *(unsigned short __far *)MK_FP(_psp - 1, 3);
    if (paragraphs > 8192) {
        puts("KEYPLAY resident allocation is unexpectedly large");
        return 1;
    }
    previous_timer = _dos_getvect(0x1c);
    _dos_setvect(0x1c, timer);
    _dos_keep(0, paragraphs);
invalid:
    fclose(file);
    puts("Invalid KEYPLAY script");
    return 1;
}
