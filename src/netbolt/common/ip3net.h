/* NetBolt - minimal non-blocking TCP abstraction (mTCP / Winsock 1.1) */
#ifndef IP3NET_H
#define IP3NET_H

typedef int NETH;
#define NET_INVALID (-1)

#ifdef __cplusplus
extern "C" {
#endif

int           net_init(char *err, int errlen);        /* 0 = ok */
void          net_shutdown(void);
void          net_poll(void);                         /* drive the stack */
void          net_idle(void);                         /* nothing to do: wait briefly */
unsigned long net_ms(void);                           /* monotonic milliseconds */
int           net_resolve(const char *host, unsigned char ip[4]);
NETH          net_connect(const unsigned char ip[4], unsigned port);
int           net_connected(NETH h);                  /* 1 yes, 0 pending, -1 failed */
int           net_send(NETH h, const void *buf, unsigned len);  /* >=0 sent, -1 error */
int           net_recv(NETH h, void *buf, unsigned len);        /* >0 got, 0 none, -1 closed */
void          net_close(NETH h);
NETH          net_listen(unsigned port);
NETH          net_accept(NETH l, unsigned char ip[4]);
void          net_hint_streams(int n);                /* parallel data streams about to be opened */
const char   *net_local_ip(void);                     /* "a.b.c.d" or "" */

#ifdef __cplusplus
}
#endif
#endif
