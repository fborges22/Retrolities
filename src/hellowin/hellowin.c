#include <windows.h>
#include <stdio.h>
#include "retro.h"
#include "resource.h"

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char title[32], format[128], host[40], text[256];

    (void)prev; (void)cmdline; (void)show;

    LoadString(inst, IDS_TITLE, title, sizeof(title));
    LoadString(inst, IDS_FORMAT, format, sizeof(format));
    sprintf(text, format, retro_target(), RETRO_BITS, retro_host(host, sizeof(host)));
    MessageBox(NULL, text, title, MB_OK | MB_ICONINFORMATION);
    return 0;
}
