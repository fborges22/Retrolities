#include <stdio.h>
#include "retro.h"

int main(void)
{
    char host[40];

    printf("Hello from Retrolities!\n");
    printf("target: %s (%d-bit)\n", retro_target(), RETRO_BITS);
    printf("host:   %s\n", retro_host(host, sizeof(host)));
    return 0;
}
