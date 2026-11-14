/*
 * Arrow - portable iperf3 protocol engine (TCP, client + server)
 *
 * The engine is a non-blocking state machine: the front end calls
 * ip3_step() from its main loop and receives progress through the ui_*
 * callbacks.  It builds unchanged for 16-bit DOS, Win16 and Win32.
 */
#ifndef IP3CORE_H
#define IP3CORE_H

#define IP3_VERSION "1.0"

#ifndef IP3_MAX_STREAMS
#define IP3_MAX_STREAMS 8
#endif
#ifndef IP3_BLK_MAX            /* largest I/O chunk (must fit in an int) */
#define IP3_BLK_MAX 16384
#endif
#ifndef IP3_BURST              /* I/O calls per stream per step */
#define IP3_BURST 8
#endif
#define IP3_DEFAULT_PORT 5201

typedef struct {
    char          host[64];
    unsigned      port;
    int           server;      /* 1 = act as iperf3 server            */
    int           reverse;     /* client only: server sends, we recv  */
    int           parallel;    /* client only: number of streams      */
    int           duration;    /* client only: seconds                */
    unsigned long blksize;     /* client only: block length announced */
} ip3_cfg;

enum {
    IP3_ST_IDLE, IP3_ST_RESOLVING, IP3_ST_CONNECTING, IP3_ST_LISTENING,
    IP3_ST_NEGOTIATING, IP3_ST_RUNNING, IP3_ST_FINISHING, IP3_ST_DONE,
    IP3_ST_ERROR
};

typedef struct {
    double secs;
    double sent_bytes, recv_bytes;
    double sent_bps, recv_bps;
    long   retrans;            /* -1 when unknown */
    int    we_send;
} ip3_summary;

/* ---- engine API ---- */
void ip3_defaults(ip3_cfg *c);
int  ip3_start(const ip3_cfg *c);      /* 0 = started */
int  ip3_step(void);                   /* 0 idle, 1 made progress, 2 waiting */
void ip3_stop(void);                   /* user abort */
int  ip3_active(void);
int  ip3_duration(void);               /* seconds of running test, 0 = open ended */

void ip3_fmt_bytes(double bytes, char *out);   /* "1.12 MBytes"    */
void ip3_fmt_rate(double bps, char *out);      /* "9.41 Mbits/sec" */

/* ---- callbacks supplied by the front end ---- */
void ui_log(const char *line);
void ui_status(int state, const char *text);
void ui_interval(double t0, double t1, double bytes, double bps);
void ui_summary(const ip3_summary *s);

#endif
