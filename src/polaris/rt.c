/* Polaris - minimal runtime: memory/string helpers, formatting, keyboard and
 * timer access.  No C library is available in stage 2. */
#include "pl.h"

void pl_memset(void *d, u8 v, u16 n)
{
    u8 *p = d;
    while (n--) *p++ = v;
}

void pl_memcpy(void *d, const void *s, u16 n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--) *p++ = *q++;
}

int pl_memcmp(const void *a, const void *b, u16 n)
{
    const u8 *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}

void pl_fmemcpy(void FAR *d, const void FAR *s, u16 n)
{
    u8 FAR *p = d;
    const u8 FAR *q = s;
    while (n--) *p++ = *q++;
}

void pl_fmemset(void FAR *d, u8 v, u16 n)
{
    u8 FAR *p = d;
    while (n--) *p++ = v;
}

u16 pl_strlen(const char *s)
{
    u16 n = 0;
    while (s[n]) n++;
    return n;
}

void pl_strncpy(char *d, const char *s, u16 n)
{
    if (!n) return;
    while (--n && *s) *d++ = *s++;
    *d = 0;
}

char *pl_strcat(char *d, const char *s)
{
    char *p = d + pl_strlen(d);
    while ((*p++ = *s++) != 0) ;
    return d;
}

int pl_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}

char pl_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

char *pl_utoa(char *d, u32 v)
{
    char tmp[11];
    u8 n = 0;
    char *p = d;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return d;
}

char *pl_hex(char *d, u32 v, u8 digits)
{
    static const char hx[] = "0123456789ABCDEF";
    u8 i;
    for (i = 0; i < digits; i++)
        d[i] = hx[(v >> ((digits - 1 - i) * 4)) & 15];
    d[digits] = 0;
    return d;
}

/* sectors (512 bytes) -> "640 KB", "1.44 MB", "8.4 GB", "1.2 TB" */
char *pl_size(char *d, u32 sectors)
{
    static const char unit[] = "KMGT";
    u32 v = sectors / 2;          /* KB */
    u32 frac = (sectors & 1) * 5;
    u8 u = 0;
    while (v >= 1000 && u < 3) {
        frac = (v % 1024) * 10 / 1024;
        v /= 1024;
        u++;
    }
    pl_utoa(d, v);
    if (v < 100 && u > 0 && frac) {
        char *p = d + pl_strlen(d);
        *p++ = '.';
        *p++ = (char)('0' + frac);
        *p = 0;
    }
    {
        char *p = d + pl_strlen(d);
        *p++ = ' ';
        *p++ = unit[u];
        *p++ = 'B';
        *p = 0;
    }
    return d;
}

u16 pl_hash(const char *s)
{
    u16 h = 0x5A17;
    while (*s) {
        h ^= (u8)pl_upper(*s++);
        h = (u16)((h << 5) | (h >> 11));
        h = (u16)(h * 31 + 0x2B);
    }
    return h ? h : 1;
}

u32 pl_ticks(void)
{
    volatile u32 FAR *t = MKFP(0x40, 0x6C);
    return *t;
}

void pl_delay(u16 ticks)
{
    u32 end = pl_ticks() + ticks;
    while (pl_ticks() < end) cpu_halt();
}

static u16 normalise_key(u16 k)
{
    if (KEYCHAR(k) == 0xE0 && KEYSCAN(k)) k &= 0xFF00;
    if (k == K_KPENTER) k = K_ENTER;
    return k;
}

u16 pl_key_peek(void)
{
    rmregs r;
    r.a.w = 0x1100;
    pl_intr(0x16, &r);
    if (r.flags & ZF_FLAG) return 0;
    return normalise_key(r.a.w ? r.a.w : 0xFFFF);
}

u16 pl_key_get(void)
{
    rmregs r;
    r.a.w = 0x1000;
    pl_intr(0x16, &r);
    return normalise_key(r.a.w);
}

u8 pl_shift_state(void)
{
    return *(u8 FAR *)MKFP(0x40, 0x17);
}
