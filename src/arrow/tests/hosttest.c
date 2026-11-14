/* Host-side harness: runs the protocol engine against a real iperf3.
 *   hosttest -c host [-p port] [-t secs] [-P n] [-R]
 *   hosttest -s [-p port] [-1]        (-1: exit after one test)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ip3core.h"
#include "ip3net.h"

static int g_state, g_tests;
void ui_log(const char *l) { puts(l); fflush(stdout); }
void ui_status(int st, const char *t) { g_state = st; printf("# %s\n", t); fflush(stdout); }
void ui_interval(double a, double b, double c, double d) { (void)a; (void)b; (void)c; (void)d; }
void ui_summary(const ip3_summary *s) { g_tests++; printf("# summary sent=%.0f recv=%.0f secs=%.2f\n", s->sent_bytes, s->recv_bytes, s->secs); }

int main(int argc, char **argv)
{
    ip3_cfg c; int i, once = 0;
    ip3_defaults(&c);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) strcpy(c.host, argv[++i]);
        else if (!strcmp(argv[i], "-s")) c.server = 1;
        else if (!strcmp(argv[i], "-p")) c.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) c.duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-P")) c.parallel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-R")) c.reverse = 1;
        else if (!strcmp(argv[i], "-1")) once = 1;
    }
    if (ip3_start(&c) != 0) return 2;
    while (ip3_active()) {
        if (ip3_step() == 2) net_idle();
        if (once && g_tests && g_state == IP3_ST_LISTENING) ip3_stop();
    }
    return (c.server ? g_tests > 0 : g_state == IP3_ST_DONE) ? 0 : 1;
}
