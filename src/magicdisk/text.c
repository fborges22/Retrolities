/* UTF-8 diagnostics retain full names in reports; VGA uses its CP437 glyphs. */
#include "disk.h"
#include <string.h>

static const unsigned short glyphs[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef,
    0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1, 0x00ed, 0x00f3, 0x00fa,
    0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255d, 0x255c, 0x255b, 0x2510, 0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4,
    0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

void dt_display_utf8(const char *input, char *output, size_t capacity, size_t skip)
{
    const unsigned char *p = (const unsigned char *)input;
    size_t written = 0;

    if (!capacity)
        return;
    while (*p && written + 1 < capacity) {
        dt_u32 code = *p++;
        unsigned count = 0, i;
        char glyph = '?';

        if (code >= 0xc2 && code <= 0xdf) {
            code &= 31;
            count = 1;
        } else if (code >= 0xe0 && code <= 0xef) {
            code &= 15;
            count = 2;
        } else if (code >= 0xf0 && code <= 0xf4) {
            code &= 7;
            count = 3;
        } else if (code >= 128) {
            code = '?';
        }
        for (i = 0; i < count; i++) {
            if ((*p & 0xc0) != 0x80) {
                code = '?';
                break;
            }
            code = (code << 6) | (*p++ & 63);
        }
        if (code >= 32 && code < 127)
            glyph = (char)code;
        else if (code == '\n' || code == '\r' || code == '\t')
            glyph = (char)code;
        else
            for (i = 0; i < 128; i++)
                if (glyphs[i] == code) {
                    glyph = (char)(i + 128);
                    break;
                }
        if (skip)
            skip--;
        else
            output[written++] = glyph;
    }
    output[written] = 0;
}

/* Human-readable sizes are rounded; callers retain exact sector ranges nearby. */
void dt_format_sectors(dt_u32 sectors, char *output, size_t capacity)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    dt_u64 bytes = (dt_u64)sectors * 512, scale = 1, tenths;
    unsigned unit = 0;
    char text[32];
    size_t length;

    if (!capacity)
        return;
    while (unit < 4 && bytes >= scale * 1024) {
        scale *= 1024;
        unit++;
    }
    tenths = (bytes * 10 + scale / 2) / scale;
    if (unit < 4 && tenths >= 10240) {
        scale *= 1024;
        unit++;
        tenths = (bytes * 10 + scale / 2) / scale;
    }
    if (!unit)
        sprintf(text, "%lu B", (unsigned long)bytes);
    else
        sprintf(text,
                "%lu.%lu %s",
                (unsigned long)(tenths / 10),
                (unsigned long)(tenths % 10),
                units[unit]);
    length = strlen(text);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(output, text, length);
    output[length] = 0;
}

/* Parse the same unsigned grammar on 32-bit DOS and 64-bit native hosts.
   Failure leaves the caller's value untouched. Hex prefixes are optional only
   when the caller explicitly requests base 16. Signs and spaces are invalid. */
int dt_parse_u32(const char *text, unsigned base, dt_u32 *value)
{
    dt_u32 parsed = 0;
    unsigned digit;

    if (!text || !value || (base != 10 && base != 16))
        return 0;
    if (base == 16 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    if (!*text)
        return 0;
    while (*text) {
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A' + 10);
        else
            return 0;
        if (digit >= base || parsed > (0xffffffffUL - digit) / base)
            return 0;
        parsed = parsed * base + digit;
        text++;
    }
    *value = parsed;
    return 1;
}
