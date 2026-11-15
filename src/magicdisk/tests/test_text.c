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
    {
        static const struct {
            dt_u32 sectors;
            const char *expected;
        } cases[] = {{0, "0 B"},
                     {1, "512 B"},
                     {2, "1.0 KiB"},
                     {3, "1.5 KiB"},
                     {2047, "1023.5 KiB"},
                     {2048, "1.0 MiB"},
                     {2880, "1.4 MiB"},
                     {2097151, "1.0 GiB"},
                     {2097152, "1.0 GiB"},
                     {8388607, "4.0 GiB"},
                     {0xffffffffUL, "2.0 TiB"}};

        unsigned i;

        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            dt_format_sectors(cases[i].sectors, display, sizeof(display));
            assert(!strcmp(display, cases[i].expected));
        }
        memset(display, 'X', sizeof(display));
        dt_format_sectors(1, display, 0);
        assert(display[0] == 'X');
        dt_format_sectors(1, display, 1);
        assert(!display[0] && display[1] == 'X');
        dt_format_sectors(1, display, 4);
        assert(!strcmp(display, "512") && display[4] == 'X');
    }
    {
        static const char *invalid[] = {"",
                                        " ",
                                        "-2",
                                        " -2",
                                        "+1",
                                        "1 ",
                                        "1x",
                                        "0x",
                                        "-4294967294",
                                        "-4294967295",
                                        "4294967296",
                                        "999999999999999999999999999999"};
        dt_u32 value;
        unsigned i;

        assert(dt_parse_u32("4294967295", 10, &value) && value == 0xffffffffUL);
        assert(dt_parse_u32("00010", 10, &value) && value == 10);
        assert(dt_parse_u32("0", 10, &value) && value == 0);
        assert(dt_parse_u32("0XffffffFE", 16, &value) && value == 0xfffffffeUL);
        assert(dt_parse_u32("FFFFFFFF", 16, &value) && value == 0xffffffffUL);
        assert(dt_parse_u32("80", 16, &value) && value == 128);
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            value = 123;
            assert(!dt_parse_u32(invalid[i], 10, &value) && value == 123);
        }
        value = 123;
        assert(!dt_parse_u32("0x100000000", 16, &value) && value == 123);
        assert(!dt_parse_u32("0x", 16, &value) && value == 123);
        assert(!dt_parse_u32("0x10", 10, &value) && value == 123);
        assert(!dt_parse_u32("10", 8, &value) && value == 123);
        assert(!dt_parse_u32(NULL, 10, &value) && value == 123);
        assert(!dt_parse_u32("10", 10, NULL));
    }
    puts("PASS: UTF-8 display, glyph fallback, horizontal scrolling, sector sizes and bounds");
    return 0;
}
