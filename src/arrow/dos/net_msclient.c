/* Arrow - Microsoft DOS TCP/IP Sockets SDK backend (resident MSCLIENT stack). */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <bios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sock_err.h>
#include "ip3core.h"
#include "ip3net.h"

#define NSLOT (IP3_MAX_STREAMS + 2)
#define DAY_TICKS 0x1800B0UL

volatile unsigned char net_break;
static int sockets[NSLOT];
static int sock_error;
static int stack_up;
static unsigned send_limit;
static char local_ip[20];
static unsigned long last_ticks, milliseconds, fraction;
static void (__interrupt __far *old_break)(void);
static void (__interrupt __far *old_ctrl_c)(void);

static void __interrupt __far break_handler(void) { net_break = 1; }

static int pending(int e)
{
    return e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY;
}

static int valid(NETH h)
{
    return stack_up && h >= 0 && h < NSLOT && sockets[h] >= 0;
}

static int nonblocking(int s)
{
    long on = 1;
    return _ioctl(s, FIONBIO, (char far *)&on, &sock_error);
}

static NETH store_socket(int s)
{
    int i;
    for (i = 0; i < NSLOT; i++) {
        if (sockets[i] < 0) {
            sockets[i] = s;
            return i;
        }
    }
    _close_socket(s, &sock_error);
    return NET_INVALID;
}

static void address(struct sockaddr_in *a, const unsigned char *ip, unsigned port)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = (unsigned short)htons((int)port);
    if (ip) memcpy(&a->sin_addr, ip, 4);
}

static void update_local_ip(int s)
{
    struct sockaddr_in a;
    int n = sizeof(a);
    unsigned char *p = (unsigned char *)&a.sin_addr;
    if (_getsockname(s, (struct sockaddr far *)&a, &n, &sock_error) == 0 &&
        (p[0] || p[1] || p[2] || p[3]))
        sprintf(local_ip, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

int net_init(char *err, int errlen)
{
    int i, s;
    long ticks;
    if (stack_up) return 0;
    /* The SDK detects SOCKETS.EXE on its first socket call. Do not start or
       stop the resident stack: the redirector and other programs share it. */
    s = _socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_error);
    if (s < 0) {
        if (errlen > 0) {
            strncpy(err, "MSCLIENT TCP/IP unavailable: load TCPTSR and SOCKETS (see README)",
                    errlen - 1);
            err[errlen - 1] = 0;
        }
        return -1;
    }
    _close_socket(s, &sock_error);
    for (i = 0; i < NSLOT; i++) sockets[i] = -1;
    local_ip[0] = 0;
    send_limit = 32767U;
    net_break = 0;
    milliseconds = fraction = 0;
    _bios_timeofday(_TIME_GETCLOCK, &ticks);
    last_ticks = (unsigned long)ticks;
    old_break = _dos_getvect(0x1b);
    old_ctrl_c = _dos_getvect(0x23);
    _dos_setvect(0x1b, break_handler);
    _dos_setvect(0x23, break_handler);
    stack_up = 1;
    return 0;
}

void net_shutdown(void)
{
    int i;
    if (!stack_up) return;
    for (i = 0; i < NSLOT; i++) net_close(i);
    _dos_setvect(0x1b, old_break);
    _dos_setvect(0x23, old_ctrl_c);
    stack_up = 0;
}

void net_poll(void) { /* The resident stack processes packets itself. */ }
void net_idle(void) { }
void net_hint_streams(int n) { (void)n; }
const char *net_local_ip(void) { return local_ip; }

unsigned long net_ms(void)
{
    long bios_ticks;
    unsigned long ticks, delta;
    _bios_timeofday(_TIME_GETCLOCK, &bios_ticks);
    ticks = (unsigned long)bios_ticks;
    delta = ticks >= last_ticks ? ticks - last_ticks : DAY_TICKS - last_ticks + ticks;
    last_ticks = ticks;
    /* Split whole/fractional milliseconds to avoid overflow after 72 minutes. */
    milliseconds += delta * 54UL;
    fraction += delta * 925UL;
    milliseconds += fraction / 1000UL;
    fraction %= 1000UL;
    return milliseconds;
}

int net_resolve(const char *host, unsigned char ip[4])
{
    const char *p = host;
    unsigned char numeric[4];
    int i;
    struct hostent far *he;
    for (i = 0; i < 4; i++) {
        unsigned n = 0;
        if (*p < '0' || *p > '9') break;
        do {
            n = n * 10 + (unsigned)(*p++ - '0');
            if (n > 255) break;
        } while (*p >= '0' && *p <= '9');
        if (n > 255) break;
        numeric[i] = (unsigned char)n;
        if (i == 3) {
            if (*p) break;
            memcpy(ip, numeric, 4);
            return 0;
        }
        if (*p++ != '.') break;
    }
    if (net_break) return -1;
    /* Name lookups use the MSCLIENT DNR configuration and may block. */
    he = gethostbyname((char far *)host);
    if (!he || he->h_addrtype != AF_INET || he->h_length != 4 ||
        !he->h_addr_list || !he->h_addr_list[0]) return -1;
    memcpy(ip, he->h_addr_list[0], 4);
    return 0;
}

NETH net_connect(const unsigned char ip[4], unsigned port)
{
    struct sockaddr_in a;
    int s, rc;
    if (!stack_up) return NET_INVALID;
    s = _socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_error);
    if (s < 0) return NET_INVALID;
    address(&a, ip, port);
    if (nonblocking(s) < 0) {
        _close_socket(s, &sock_error);
        return NET_INVALID;
    }
    rc = _connect(s, (struct sockaddr far *)&a, sizeof(a), &sock_error);
    if (rc < 0 && !pending(sock_error)) {
        _close_socket(s, &sock_error);
        return NET_INVALID;
    }
    return store_socket(s);
}

int net_connected(NETH h)
{
    fd_set w, e;
    struct timeval tv;
    struct sockaddr_in peer;
    int n = sizeof(peer), rc, s;
    if (!valid(h)) return -1;
    s = sockets[h];
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    memset(&tv, 0, sizeof(tv));
    /* SDK descriptors include an offset; its fd_set macros strip it.
       Use the SDK mask size, not the encoded descriptor + 1, for nfds. */
    rc = _select(MAX_SOCKETS, NULL, &w, &e, &tv, &sock_error);
    if (rc < 0 || FD_ISSET(s, &e)) return -1;
    if (!rc || !FD_ISSET(s, &w)) return 0;
    /* This SDK has no SO_ERROR option. Verify the peer after readiness. */
    if (_getpeername(s, (struct sockaddr far *)&peer, &n, &sock_error) < 0)
        return pending(sock_error) || sock_error == ENOTCONN ? 0 : -1;
    update_local_ip(s);
    return 1;
}

int net_send(NETH h, const void *buf, unsigned len)
{
    int rc;
    if (!valid(h)) return -1;
    if (len > send_limit) len = send_limit;
    for (;;) {
        rc = _send(sockets[h], (char far *)buf, (int)len, 0, &sock_error);
        if (rc >= 0 || sock_error != EMSGSIZE || len <= 1) break;
        /* TCPUTILS.INI's maxsendsize may be smaller than an Arrow block. */
        len /= 2;
        send_limit = len;
    }
    return rc < 0 ? (sock_error == EWOULDBLOCK ? 0 : -1) : rc;
}

int net_recv(NETH h, void *buf, unsigned len)
{
    int rc;
    if (!valid(h)) return -1;
    if (len > 32767U) len = 32767U;
    rc = _recv(sockets[h], (char far *)buf, (int)len, 0, &sock_error);
    if (rc < 0) return sock_error == EWOULDBLOCK ? 0 : -1;
    return rc == 0 ? -1 : rc;
}

void net_close(NETH h)
{
    if (!valid(h)) return;
    _close_socket(sockets[h], &sock_error);
    sockets[h] = -1;
}

NETH net_listen(unsigned port)
{
    struct sockaddr_in a;
    int s, on = 1;
    if (!stack_up) return NET_INVALID;
    s = _socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_error);
    if (s < 0) return NET_INVALID;
    _setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char far *)&on, sizeof(on), &sock_error);
    address(&a, NULL, port);
    if (nonblocking(s) < 0 ||
        _bind(s, (struct sockaddr far *)&a, sizeof(a), &sock_error) < 0 ||
        _listen(s, SOMAXCONN, &sock_error) < 0) {
        _close_socket(s, &sock_error);
        return NET_INVALID;
    }
    return store_socket(s);
}

NETH net_accept(NETH l, unsigned char ip[4])
{
    struct sockaddr_in a;
    int s, n = sizeof(a);
    NETH h;
    if (!valid(l)) return NET_INVALID;
    s = _accept(sockets[l], (struct sockaddr far *)&a, &n, &sock_error);
    if (s < 0) return NET_INVALID;
    if (nonblocking(s) < 0) {
        _close_socket(s, &sock_error);
        return NET_INVALID;
    }
    h = store_socket(s);
    if (h != NET_INVALID) {
        memcpy(ip, &a.sin_addr, 4);
        update_local_ip(s);
    }
    return h;
}
