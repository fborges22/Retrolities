#include <stdio.h>
#include <string.h>
#include "retro.h"

#ifdef RETRO_WINDOWS
#include <windows.h>
#else
#include <stdlib.h>
#endif

const char *retro_target(void)
{
#if defined(RETRO_DOS)
    return "MS-DOS";
#elif defined(RETRO_WIN16)
    return "Windows 3.x";
#elif defined(RETRO_WIN9X)
    return "Windows 9x";
#else
    return "Windows NT";
#endif
}

char *retro_host(char *buf, unsigned size)
{
    char tmp[40];
#if defined(RETRO_DOS)
    sprintf(tmp, "DOS %u.%02u", (unsigned)_osmajor, (unsigned)_osminor);
#else
    DWORD v = GetVersion();
    unsigned major = LOBYTE(LOWORD(v));
    unsigned minor = HIBYTE(LOWORD(v));
#if defined(RETRO_WIN16)
    sprintf(tmp, "Windows %u.%02u", major, minor);
#else
    /* The high bit is clear on the NT family only. */
    sprintf(tmp, "%s %u.%02u", (v & 0x80000000UL) ? "Windows" : "Windows NT",
            major, minor);
#endif
#endif
    if (size == 0)
        return buf;
    strncpy(buf, tmp, size - 1);
    buf[size - 1] = '\0';
    return buf;
}
