#include "disk.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    char display[32];

    dt_display_utf8("caf\303\251", display, sizeof(display), 0);
    assert(!strcmp(display, "caf\202"));
    dt_display_utf8("caf\303\251", display, sizeof(display), 3);
    assert(!strcmp(display, "\202"));
    dt_display_utf8("A\360\237\230\200B", display, sizeof(display), 0);
    assert(!strcmp(display, "A?B"));
    dt_display_utf8("A\360\237\230\200B", display, sizeof(display), 2);
    assert(!strcmp(display, "B"));
    dt_display_utf8("A\303", display, sizeof(display), 0);
    assert(!strcmp(display, "A?"));
    dt_display_utf8("abcdef", display, 4, 0);
    assert(!strcmp(display, "abc"));
    dt_display_utf8("abcdef", display, 1, 0);
    assert(!display[0]);
    puts("PASS: UTF-8 display, glyph fallback, horizontal scrolling and bounds");
    return 0;
}
