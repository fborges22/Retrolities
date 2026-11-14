/*
 * NetBolt - portable iperf3 protocol engine (TCP, client + server)
 *
 * Wire protocol (iperf 3.x):
 *   control connection: 37 byte cookie, then single state bytes and
 *   length-prefixed (32 bit big endian) JSON documents.
 *   data connections:   37 byte cookie, then raw payload.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ip3core.h"
#include "ip3net.h"

/* iperf3 control states */
#define TEST_START        1
#define TEST_RUNNING      2
#define TEST_END          4
#define PARAM_EXCHANGE    9
#define CREATE_STREAMS    10
#define SERVER_TERMINATE  11
#define CLIENT_TERMINATE  12
#define EXCHANGE_RESULTS  13
#define DISPLAY_RESULTS   14
#define IPERF_DONE        16
#define ACCESS_DENIED     (-1)
#define SERVER_ERROR      (-2)

#define COOKIE_SIZE 37
#define CTL_MAX     4096

enum { PH_IDLE, PH_C_CONNECT, PH_C_NEG, PH_C_STREAMS, PH_RUN, PH_END,
       PH_S_LISTEN, PH_S_NEG, PH_S_STREAMS };
enum { CM_STATE, CM_LEN, CM_JSON, CM_COOKIE, CM_ERR };

typedef struct {
    NETH          h;
    int           id;
    int           ckgot;
    char          ck[COOKIE_SIZE];
    unsigned long ib;          /* bytes this interval (unfolded part) */
    double        ifold;       /* bytes this interval (folded part)   */
    double        total;
} stream_t;

static struct {
    ip3_cfg       cfg;
    int           phase;
    NETH          ctrl, lsn;
    char          cookie[COOKIE_SIZE];
    stream_t      st[IP3_MAX_STREAMS];
    int           nst, want;
    int           sending;
    unsigned      chunk;
    int           duration;
    unsigned long t_start, t_int, t_end, t_phase;
    int           cmode;
    unsigned long cneed, cgot;
    double        peer_bytes;
    long          peer_retrans;
    int           summarised;
    unsigned char ip[4];
} g;

static unsigned char g_ctl[CTL_MAX + 1];
static unsigned char g_blk[IP3_BLK_MAX];
static char          g_line[160];

/* ------------------------------------------------------------------ */

void ip3_defaults(ip3_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->port = IP3_DEFAULT_PORT;
    c->parallel = 1;
    c->duration = 10;
    c->blksize = IP3_BLK_MAX;
}

static void fmt_unit(double v, double base, const char **units, char *out)
{
    int u = 0;
    while (v >= base && u < 3) { v /= base; u++; }
    if (v < 9.995)       sprintf(out, "%.2f %s", v, units[u]);
    else if (v < 99.95)  sprintf(out, "%.1f %s", v, units[u]);
    else                 sprintf(out, "%.0f %s", v, units[u]);
}

void ip3_fmt_bytes(double bytes, char *out)
{
    static const char *u[] = { "Bytes", "KBytes", "MBytes", "GBytes" };
    fmt_unit(bytes, 1024.0, u, out);
}

void ip3_fmt_rate(double bps, char *out)
{
    static const char *u[] = { "bits/sec", "Kbits/sec", "Mbits/sec", "Gbits/sec" };
    fmt_unit(bps, 1000.0, u, out);
}

static void report_line(const char *tag, double t0, double t1, double bytes,
                        const char *suffix)
{
    char b[24], r[24], iv[24];
    double dt = t1 - t0;
    ip3_fmt_bytes(bytes, b);
    ip3_fmt_rate(dt > 0 ? bytes * 8.0 / dt : 0.0, r);
    sprintf(iv, "%.2f-%.2f", t0, t1);
    sprintf(g_line, "[%3s] %-12s sec  %11s  %14s  %s", tag, iv, b, r, suffix);
    ui_log(g_line);
}

/* ------------------------------------------------------------------ */

static int nwrite(NETH h, const void *p, unsigned n)
{
    const char *c = (const char *)p;
    unsigned long t0 = net_ms();
    while (n) {
        int r = net_send(h, c, n);
        if (r < 0) return -1;
        c += r; n -= (unsigned)r;
        if (n) {
            net_poll();
            if (net_ms() - t0 > 5000UL) return -1;
        }
    }
    net_poll();
    return 0;
}

static int send_state(int s)
{
    char b = (char)s;
    return nwrite(g.ctrl, &b, 1);
}

static int send_json(const char *js)
{
    unsigned long n = (unsigned long)strlen(js);
    unsigned char l[4];
    l[0] = (unsigned char)(n >> 24); l[1] = (unsigned char)(n >> 16);
    l[2] = (unsigned char)(n >> 8);  l[3] = (unsigned char)n;
    if (nwrite(g.ctrl, l, 4) < 0) return -1;
    return nwrite(g.ctrl, js, (unsigned)n);
}

static void need(int mode, unsigned long n)
{
    g.cmode = mode; g.cneed = n; g.cgot = 0;
}

static int jnum(const char *s, const char *key, double *v)
{
    char k[40];
    const char *p;
    sprintf(k, "\"%s\":", key);
    p = strstr(s, k);
    if (!p) return 0;
    p += strlen(k);
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4))  { *v = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *v = 0; return 1; }
    *v = strtod(p, NULL);
    return 1;
}

static void close_streams(void)
{
    int i;
    for (i = 0; i < g.nst; i++)
        if (g.st[i].h != NET_INVALID) { net_close(g.st[i].h); g.st[i].h = NET_INVALID; }
    g.nst = 0;
}

static void close_ctrl(void)
{
    if (g.ctrl != NET_INVALID) { net_close(g.ctrl); g.ctrl = NET_INVALID; }
}

static void go_idle(int st, const char *msg)
{
    close_streams();
    close_ctrl();
    if (g.lsn != NET_INVALID) { net_close(g.lsn); g.lsn = NET_INVALID; }
    g.phase = PH_IDLE;
    ui_status(st, msg);
}

static void server_listen(void)
{
    close_streams();
    close_ctrl();
    net_hint_streams(1);
    g.phase = PH_S_LISTEN;
    g.duration = 0;
    sprintf(g_line, "Server listening on port %u", g.cfg.port);
    ui_status(IP3_ST_LISTENING, g_line);
}

/* fatal for the running test: clients stop, servers go back to listening */
static void fail(const char *msg)
{
    sprintf(g_line, "error: %s", msg);
    ui_log(g_line);
    if (g.cfg.server && g.lsn != NET_INVALID) {
        server_listen();
    } else {
        go_idle(IP3_ST_ERROR, msg);
    }
}

/* ------------------------------------------------------------------ */

static void start_test(void)
{
    int i;
    for (i = 0; i < g.nst; i++) { g.st[i].ib = 0; g.st[i].ifold = 0; g.st[i].total = 0; }
    g.t_start = g.t_int = net_ms();
    g.summarised = 0;
    g.peer_bytes = 0; g.peer_retrans = -1;
    g.phase = PH_RUN;
    ui_log("[ ID] Interval          Transfer        Bitrate");
    ui_status(IP3_ST_RUNNING, g.sending ? "Test running - sending" : "Test running - receiving");
}

static void interval_report(unsigned long now)
{
    double t0 = (g.t_int - g.t_start) / 1000.0, t1 = (now - g.t_start) / 1000.0;
    double sum = 0, b;
    char tag[8];
    int i;
    if (now == g.t_int) return;
    for (i = 0; i < g.nst; i++) {
        b = g.st[i].ifold + (double)g.st[i].ib;
        g.st[i].total += b;
        g.st[i].ib = 0; g.st[i].ifold = 0;
        sum += b;
        if (g.nst > 1) {
            sprintf(tag, "%3d", g.st[i].id);
            report_line(tag, t0, t1, b, "");
        }
    }
    report_line(g.nst > 1 ? "SUM" : "  1", t0, t1, sum, "");
    ui_interval(t0, t1, sum, sum * 8.0 / (t1 - t0));
    g.t_int = now;
}

static int data_pump(void)
{
    static int first;
    int i, k, n, r, progress = 0;
    if (++first >= g.nst) first = 0;         /* rotate for fairness between streams */
    for (k = 0; k < g.nst; k++) {
        stream_t *s;
        i = first + k;
        if (i >= g.nst) i -= g.nst;
        s = &g.st[i];
        if (s->h == NET_INVALID) continue;
        for (n = 0; n < IP3_BURST; n++) {
            r = g.sending ? net_send(s->h, g_blk, g.chunk)
                          : net_recv(s->h, g_blk, g.chunk);
            if (r <= 0) {
                if (r < 0) {
                    net_close(s->h); s->h = NET_INVALID;
                    if (g.phase == PH_RUN) { fail("data connection lost"); return 1; }
                }
                break;
            }
            s->ib += (unsigned long)r;
            if (s->ib > 0x40000000UL) { s->ifold += (double)s->ib; s->ib = 0; }
            progress = 1;
        }
    }
    return progress;
}

static void end_test(unsigned long now)
{
    if (!g.sending) data_pump();
    if (now - g.t_int >= 100UL || now == g.t_start) interval_report(now);
    else {                               /* fold a tiny tail into the totals */
        int i;
        for (i = 0; i < g.nst; i++) {
            g.st[i].total += g.st[i].ifold + (double)g.st[i].ib;
            g.st[i].ib = 0; g.st[i].ifold = 0;
        }
    }
    g.t_end = now;
    g.phase = PH_END;
    g.t_phase = now;
    ui_status(IP3_ST_FINISHING, "Exchanging results");
}

static double own_total(void)
{
    double t = 0;
    int i;
    for (i = 0; i < g.nst; i++) t += g.st[i].total + g.st[i].ifold + (double)g.st[i].ib;
    return t;
}

static int send_results(void)
{
    char *p = (char *)g_ctl;       /* control buffer is free while we transmit */
    double secs = (g.t_end - g.t_start) / 1000.0;
    int i;
    p += sprintf(p, "{\"cpu_util_total\":0,\"cpu_util_user\":0,\"cpu_util_system\":0,"
                    "\"sender_has_retransmits\":0,\"streams\":[");
    for (i = 0; i < g.nst; i++)
        p += sprintf(p, "%s{\"id\":%d,\"bytes\":%.0f,\"retransmits\":-1,\"jitter\":0,"
                        "\"errors\":0,\"packets\":0,\"start_time\":0,\"end_time\":%.3f}",
                     i ? "," : "", g.st[i].id, g.st[i].total, secs);
    strcpy(p, "]}");
    return send_json((char *)g_ctl);
}

static void parse_results(char *js)
{
    char *p = strstr(js, "\"streams\""), *q, *e;
    double v;
    g.peer_bytes = 0; g.peer_retrans = -1;
    while (p && (q = strchr(p, '{')) != NULL && (e = strchr(q, '}')) != NULL) {
        *e = 0;
        if (jnum(q, "bytes", &v)) g.peer_bytes += v;
        if (jnum(q, "retransmits", &v) && v >= 0) {
            if (g.peer_retrans < 0) g.peer_retrans = 0;
            g.peer_retrans += (long)v;
        }
        p = e + 1;
    }
}

static void summarise(void)
{
    ip3_summary s;
    double own = own_total();
    s.secs = (g.t_end - g.t_start) / 1000.0;
    if (s.secs <= 0) s.secs = 0.001;
    s.we_send = g.sending;
    s.sent_bytes = g.sending ? own : g.peer_bytes;
    s.recv_bytes = g.sending ? g.peer_bytes : own;
    s.sent_bps = s.sent_bytes * 8.0 / s.secs;
    s.recv_bps = s.recv_bytes * 8.0 / s.secs;
    s.retrans = g.sending ? -1 : g.peer_retrans;
    ui_log("- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
    report_line(g.nst > 1 ? "SUM" : "  1", 0.0, s.secs, s.sent_bytes, "sender");
    report_line(g.nst > 1 ? "SUM" : "  1", 0.0, s.secs, s.recv_bytes, "receiver");
    g.summarised = 1;
    ui_summary(&s);
}

/* ------------------------------------------------------------------ */

static void client_next_stream(void)
{
    stream_t *s = &g.st[g.nst];
    s->h = net_connect(g.ip, g.cfg.port);
    if (s->h == NET_INVALID) { fail("unable to open data connection"); return; }
    s->id = g.nst ? g.nst + 2 : 1;       /* iperf3 numbers streams 1,3,4,5... */
    s->ckgot = 0; s->ib = 0; s->ifold = 0; s->total = 0;
    g.nst++;
    g.t_phase = net_ms();
}

static void on_state(int s)
{
    if (g.cfg.server) {
        switch (s) {
        case TEST_END:
            if (g.phase != PH_RUN) break;
            end_test(net_ms());
            if (send_state(EXCHANGE_RESULTS) < 0) { fail("control connection lost"); return; }
            need(CM_LEN, 4);
            return;
        case IPERF_DONE:
            ui_log("Test complete.");
            server_listen();
            return;
        case CLIENT_TERMINATE:
            fail("the client has terminated");
            return;
        }
        need(CM_STATE, 1);
        return;
    }

    switch (s) {
    case PARAM_EXCHANGE:
        sprintf((char *)g_ctl, "{\"tcp\":true,\"omit\":0,\"time\":%d,\"parallel\":%d,"
                "\"len\":%lu,%s\"client_version\":\"3.16\"}",
                g.cfg.duration, g.cfg.parallel, g.cfg.blksize,
                g.cfg.reverse ? "\"reverse\":true," : "");
        if (send_json((char *)g_ctl) < 0) { fail("control connection lost"); return; }
        break;
    case CREATE_STREAMS:
        net_hint_streams(g.cfg.parallel);
        g.phase = PH_C_STREAMS;
        g.nst = 0;
        client_next_stream();
        if (g.phase == PH_IDLE) return;
        break;
    case TEST_START:
        break;
    case TEST_RUNNING:
        start_test();
        break;
    case EXCHANGE_RESULTS:
        if (g.phase == PH_RUN) end_test(net_ms());
        if (send_results() < 0) { fail("control connection lost"); return; }
        need(CM_LEN, 4);
        return;
    case DISPLAY_RESULTS:
        summarise();
        send_state(IPERF_DONE);
        ui_log("iperf Done.");
        go_idle(IP3_ST_DONE, "Test complete");
        return;
    case SERVER_TERMINATE:
        fail("the server has terminated");
        return;
    case ACCESS_DENIED:
        fail("server is busy or refused the test");
        return;
    case SERVER_ERROR:
        need(CM_ERR, 8);
        return;
    default:
        fail("unexpected control message");
        return;
    }
    need(CM_STATE, 1);
}

static void server_params(char *js)
{
    double v;
    int par = 1;
    unsigned long len = IP3_BLK_MAX;
    if ((jnum(js, "udp", &v) && v != 0) || (jnum(js, "bidirectional", &v) && v != 0) ||
        !jnum(js, "tcp", &v)) {
        ui_log("Refused: only TCP tests are supported");
        send_state(ACCESS_DENIED);
        server_listen();
        return;
    }
    if (jnum(js, "parallel", &v)) par = (int)v;
    if (par < 1 || par > IP3_MAX_STREAMS) {
        sprintf(g_line, "Refused: %d streams requested (max %d)", par, IP3_MAX_STREAMS);
        ui_log(g_line);
        send_state(ACCESS_DENIED);
        server_listen();
        return;
    }
    g.sending = (jnum(js, "reverse", &v) && v != 0);
    if (jnum(js, "len", &v) && v > 0) len = (unsigned long)v;
    g.chunk = len > IP3_BLK_MAX ? IP3_BLK_MAX : (unsigned)len;
    g.duration = jnum(js, "time", &v) ? (int)v : 0;
    g.want = par;
    g.nst = 0;
    net_hint_streams(par);
    if (send_state(CREATE_STREAMS) < 0) { fail("control connection lost"); return; }
    g.phase = PH_S_STREAMS;
    g.t_phase = net_ms();
    need(CM_STATE, 1);
}

static void on_ctl_message(void)
{
    switch (g.cmode) {
    case CM_STATE:
        on_state((int)(signed char)g_ctl[0]);
        break;
    case CM_COOKIE:
        memcpy(g.cookie, g_ctl, COOKIE_SIZE);
        if (send_state(PARAM_EXCHANGE) < 0) { fail("control connection lost"); return; }
        need(CM_LEN, 4);
        break;
    case CM_LEN: {
        unsigned long n = ((unsigned long)g_ctl[0] << 24) | ((unsigned long)g_ctl[1] << 16) |
                          ((unsigned long)g_ctl[2] << 8) | g_ctl[3];
        if (n == 0 || n > CTL_MAX) { fail("control message too large"); return; }
        need(CM_JSON, n);
        break; }
    case CM_JSON:
        g_ctl[g.cneed] = 0;
        if (g.cfg.server && g.phase == PH_S_NEG) {
            server_params((char *)g_ctl);
        } else if (g.cfg.server) {           /* client results received */
            parse_results((char *)g_ctl);
            if (send_results() < 0 || send_state(DISPLAY_RESULTS) < 0) {
                fail("control connection lost"); return;
            }
            summarise();
            g.t_phase = net_ms();
            need(CM_STATE, 1);
        } else {
            parse_results((char *)g_ctl);
            need(CM_STATE, 1);
        }
        break;
    case CM_ERR:
        fail("the server reported an error");
        break;
    }
}

/* returns 1 if something was read */
static int ctl_pump(void)
{
    int r, progress = 0;
    unsigned want;
    while (g.phase != PH_IDLE && g.ctrl != NET_INVALID) {
        want = (unsigned)(g.cneed - g.cgot);
        r = net_recv(g.ctrl, g_ctl + g.cgot, want);
        if (r == 0) break;
        if (r < 0) {
            if (g.cfg.server && g.summarised && g.phase == PH_END) { ui_log("Test complete."); server_listen(); }
            else fail("control connection closed by peer");
            return 1;
        }
        progress = 1;
        g.cgot += (unsigned long)r;
        if (g.cgot >= g.cneed) on_ctl_message();
    }
    return progress;
}

/* ------------------------------------------------------------------ */

int ip3_start(const ip3_cfg *c)
{
    static const char cs[] = "abcdefghijklmnopqrstuvwxyz234567";
    unsigned i;

    if (g.phase != PH_IDLE) return -1;
    memset(&g, 0, sizeof(g));
    g.cfg = *c;
    g.ctrl = g.lsn = NET_INVALID;
    if (g.cfg.port == 0) g.cfg.port = IP3_DEFAULT_PORT;
    if (g.cfg.parallel < 1) g.cfg.parallel = 1;
    if (g.cfg.parallel > IP3_MAX_STREAMS) g.cfg.parallel = IP3_MAX_STREAMS;
    if (g.cfg.duration < 1) g.cfg.duration = 1;
    if (g.cfg.blksize < 64) g.cfg.blksize = 64;
    if (g.cfg.blksize > 1048576UL) g.cfg.blksize = 1048576UL;
    for (i = 0; i < IP3_BLK_MAX; i++) g_blk[i] = (unsigned char)('0' + i % 10);

    if (g.cfg.server) {
        g.lsn = net_listen(g.cfg.port);
        if (g.lsn == NET_INVALID) {
            ui_log("error: unable to listen on the requested port");
            ui_status(IP3_ST_ERROR, "Unable to listen");
            return -1;
        }
        sprintf(g_line, "Server listening on %s port %u", net_local_ip(), g.cfg.port);
        ui_log(g_line);
        server_listen();
        return 0;
    }

    sprintf(g_line, "Resolving %s", g.cfg.host);
    ui_status(IP3_ST_RESOLVING, g_line);
    if (g.cfg.host[0] == 0 || net_resolve(g.cfg.host, g.ip) != 0) {
        sprintf(g_line, "error: unable to resolve host '%s'", g.cfg.host);
        ui_log(g_line);
        ui_status(IP3_ST_ERROR, "Unable to resolve host");
        return -1;
    }
    srand((unsigned)net_ms());
    for (i = 0; i < COOKIE_SIZE - 1; i++) g.cookie[i] = cs[rand() % 32];
    g.cookie[COOKIE_SIZE - 1] = 0;
    g.sending = !g.cfg.reverse;
    g.duration = g.cfg.duration;
    g.chunk = g.cfg.blksize > IP3_BLK_MAX ? IP3_BLK_MAX : (unsigned)g.cfg.blksize;

    sprintf(g_line, "Connecting to host %s (%u.%u.%u.%u), port %u", g.cfg.host,
            g.ip[0], g.ip[1], g.ip[2], g.ip[3], g.cfg.port);
    ui_log(g_line);
    ui_status(IP3_ST_CONNECTING, g_line);
    g.ctrl = net_connect(g.ip, g.cfg.port);
    if (g.ctrl == NET_INVALID) {
        ui_log("error: unable to open a socket");
        ui_status(IP3_ST_ERROR, "Unable to open a socket");
        return -1;
    }
    g.phase = PH_C_CONNECT;
    g.t_phase = net_ms();
    return 0;
}

int ip3_active(void)   { return g.phase != PH_IDLE; }
int ip3_duration(void) { return g.phase == PH_RUN ? g.duration : 0; }

void ip3_stop(void)
{
    char b;
    if (g.phase == PH_IDLE) return;
    if (g.ctrl != NET_INVALID) {
        b = (char)(g.cfg.server ? SERVER_TERMINATE : CLIENT_TERMINATE);
        net_send(g.ctrl, &b, 1);
        net_poll();
    }
    ui_log("Stopped by user.");
    go_idle(IP3_ST_IDLE, "Stopped");
}

static int server_streams(void)
{
    unsigned char ip[4];
    int i, r, ready = 0, progress = 0;
    NETH h;

    while (g.nst < g.want && (h = net_accept(g.lsn, ip)) != NET_INVALID) {
        stream_t *s = &g.st[g.nst++];
        memset(s, 0, sizeof(*s));
        s->h = h;
        s->id = (g.nst == 1) ? 1 : g.nst + 1;
        progress = 1;
    }
    for (i = 0; i < g.nst; i++) {
        stream_t *s = &g.st[i];
        if (s->ckgot < COOKIE_SIZE) {
            r = net_recv(s->h, s->ck + s->ckgot, (unsigned)(COOKIE_SIZE - s->ckgot));
            if (r < 0) { fail("data connection lost"); return 1; }
            if (r > 0) { s->ckgot += r; progress = 1; }
        }
        if (s->ckgot == COOKIE_SIZE) {
            if (memcmp(s->ck, g.cookie, COOKIE_SIZE) != 0) {
                /* not one of our streams (another client?): turn it away */
                char b = (char)ACCESS_DENIED;
                net_send(s->h, &b, 1);
                net_poll();
                net_close(s->h);
                g.nst--;
                for (r = i; r < g.nst; r++) g.st[r] = g.st[r + 1];
                for (r = 0; r < g.nst; r++) g.st[r].id = r ? r + 2 : 1;
                return 1;
            }
            ready++;
        }
    }
    if (ready == g.want) {
        if (send_state(TEST_START) < 0 || send_state(TEST_RUNNING) < 0) {
            fail("control connection lost"); return 1;
        }
        start_test();
        return 1;
    }
    if (net_ms() - g.t_phase > 30000UL) { fail("timed out waiting for data connections"); return 1; }
    return progress;
}

static void server_reject_extra(void)
{
    unsigned char ip[4];
    char b = (char)ACCESS_DENIED;
    NETH h = net_accept(g.lsn, ip);
    if (h != NET_INVALID) { net_send(h, &b, 1); net_poll(); net_close(h); }
}

int ip3_step(void)
{
    int progress = 0, r;
    unsigned long now;
    unsigned char ip[4];

    if (g.phase == PH_IDLE) return 0;
    net_poll();
    now = net_ms();

    switch (g.phase) {
    case PH_C_CONNECT:
        r = net_connected(g.ctrl);
        if (r < 0 || (r == 0 && now - g.t_phase > 10000UL)) {
            fail("unable to connect to server");
            return 1;
        }
        if (r > 0) {
            if (nwrite(g.ctrl, g.cookie, COOKIE_SIZE) < 0) { fail("control connection lost"); return 1; }
            g.phase = PH_C_NEG;
            need(CM_STATE, 1);
            ui_status(IP3_ST_NEGOTIATING, "Connected - negotiating test");
            progress = 1;
        }
        break;

    case PH_C_NEG:
        progress = ctl_pump();
        break;

    case PH_C_STREAMS: {
        stream_t *s = &g.st[g.nst - 1];
        r = net_connected(s->h);
        if (r < 0 || (r == 0 && now - g.t_phase > 10000UL)) {
            fail("unable to open data connection");
            return 1;
        }
        if (r > 0) {
            if (nwrite(s->h, g.cookie, COOKIE_SIZE) < 0) { fail("data connection lost"); return 1; }
            progress = 1;
            if (g.nst < g.cfg.parallel) client_next_stream();
            else g.phase = PH_C_NEG;
        }
        break; }

    case PH_RUN:
        progress = data_pump();
        if (g.phase != PH_RUN) return 1;
        progress |= ctl_pump();
        if (g.phase != PH_RUN) return 1;
        now = net_ms();
        if (!g.cfg.server && now - g.t_start >= (unsigned long)g.duration * 1000UL) {
            end_test(now);
            if (send_state(TEST_END) < 0) fail("control connection lost");
            return 1;
        }
        if (now - g.t_int >= 1000UL) interval_report(now);
        if (g.cfg.server) server_reject_extra();
        break;

    case PH_END:
        /* keep draining so a sending peer never blocks on a full window */
        if (!g.sending && !g.cfg.server) data_pump();
        progress = ctl_pump();
        if (g.phase == PH_END && now - g.t_phase > 15000UL) {
            if (g.cfg.server && g.summarised) server_listen();
            else fail("timed out waiting for results");
            return 1;
        }
        break;

    case PH_S_LISTEN:
        g.ctrl = net_accept(g.lsn, ip);
        if (g.ctrl != NET_INVALID) {
            sprintf(g_line, "Accepted connection from %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            ui_log(g_line);
            ui_status(IP3_ST_NEGOTIATING, g_line);
            g.phase = PH_S_NEG;
            g.t_phase = now;
            need(CM_COOKIE, COOKIE_SIZE);
            progress = 1;
        }
        break;

    case PH_S_NEG:
        progress = ctl_pump();
        if (g.phase == PH_S_NEG && now - g.t_phase > 15000UL) { fail("negotiation timed out"); return 1; }
        break;

    case PH_S_STREAMS:
        progress = server_streams();
        if (g.phase == PH_S_STREAMS) progress |= ctl_pump();
        break;
    }
    if (g.phase == PH_IDLE) return 0;
    return progress ? 1 : 2;
}
