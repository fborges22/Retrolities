/* Arrow for OS/2 2.0 - command-line console front end. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ip3core.h"
#include "ip3net.h"

static volatile int interrupted;
static int status = IP3_ST_IDLE;

static void on_break(int sig)
{
    (void)sig;
    interrupted = 1;
}

void ui_log(const char *line)
{
    puts(line);
}

void ui_status(int state, const char *text)
{
    status = state;
    if (state == IP3_ST_ERROR) fprintf(stderr, "%s\n", text);
}

void ui_interval(double t0, double t1, double bytes, double bps)
{
    char rate[24], amount[24];
    (void)t0;
    ip3_fmt_rate(bps, rate);
    ip3_fmt_bytes(bytes, amount);
    printf("[ %.2f-%.2f] %s  %s\n", t0, t1, amount, rate);
}

void ui_summary(const ip3_summary *s)
{
    (void)s;
}

static void usage(void)
{
    puts("Arrow for OS/2 2.0+ v" IP3_VERSION " - TCP throughput test\n"
         "\n"
         "Usage: ARROWO -c <host> [options]\n"
         "       ARROWO -s [options]\n"
         "  -c <host>   run as client and connect to <host>\n"
         "  -s          run as server\n"
         "  -p <port>   port (default 5201)\n"
         "  -t <secs>   test duration (default 10)\n"
         "  -P <n>      parallel streams\n"
         "  -l <bytes>  block length\n"
         "  -R          reverse: server sends, client receives\n"
         "  -h          show this help\n"
         "\n"
         "Requires an installed OS/2 TCP/IP stack.");
}

int main(int argc, char **argv)
{
    ip3_cfg cfg;
    char err[128];
    int i, rc;

    ip3_defaults(&cfg);
    for (i = 1; i < argc; ++i) {
        char *arg = argv[i];
        char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(arg, "-s") == 0) {
            cfg.server = 1;
        } else if (strcmp(arg, "-R") == 0) {
            cfg.reverse = 1;
        } else if (strcmp(arg, "-c") == 0 && value) {
            strncpy(cfg.host, value, sizeof(cfg.host) - 1);
            ++i;
        } else if (strcmp(arg, "-p") == 0 && value) {
            cfg.port = (unsigned)strtoul(value, NULL, 10);
            ++i;
        } else if (strcmp(arg, "-t") == 0 && value) {
            cfg.duration = atoi(value);
            ++i;
        } else if (strcmp(arg, "-P") == 0 && value) {
            cfg.parallel = atoi(value);
            ++i;
        } else if (strcmp(arg, "-l") == 0 && value) {
            cfg.blksize = strtoul(value, NULL, 10);
            ++i;
        } else {
            usage();
            return 1;
        }
    }
    if ((!cfg.server && cfg.host[0] == 0) || (cfg.server && cfg.host[0] != 0)) {
        usage();
        return 1;
    }
    if (net_init(err, sizeof(err)) != 0) {
        fprintf(stderr, "ARROWO: %s\n", err);
        return 2;
    }

    signal(SIGINT, on_break);
    if (ip3_start(&cfg) == 0) {
        while (ip3_active() && !interrupted) {
            if (ip3_step() == 2) net_idle();
        }
        if (interrupted && ip3_active()) ip3_stop();
    }
    rc = (status == IP3_ST_DONE || (cfg.server && status != IP3_ST_ERROR)) ? 0 : 1;
    net_shutdown();
    return rc;
}
