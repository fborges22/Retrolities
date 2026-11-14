/* Arrow - ip3net.h backend for the OS/2 TCP/IP socket API. */
#include <string.h>
#include <stdio.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <tcpustd.h>
#include <sys/time.h>
#include <nerrno.h>
#define INCL_DOS
#include <os2.h>

#include "ip3net.h"

#define NSOCK 24

static int  sock[NSOCK];
static char used[NSOCK];
static char wblock[NSOCK];
static char localip[20];

static void nonblock(int s)
{
    int on = 1;
    ioctl(s, FIONBIO, (char *)&on);
}

static int slot_new(int s)
{
    int i;
    for (i = 0; i < NSOCK; ++i) {
        if (!used[i]) {
            sock[i] = s;
            used[i] = 1;
            wblock[i] = 0;
            return i;
        }
    }
    soclose(s);
    return NET_INVALID;
}

int net_init(char *err, int errlen)
{
    char name[64];
    struct hostent *he;

    if (sock_init() != 0) {
        strncpy(err, "OS/2 TCP/IP sockets are unavailable. Install and start a TCP/IP stack.", errlen);
        if (errlen > 0) err[errlen - 1] = 0;
        return -1;
    }
    if (gethostname(name, sizeof(name)) == 0 &&
        (he = gethostbyname(name)) != NULL && he->h_addr_list[0] != NULL) {
        unsigned char *a = (unsigned char *)he->h_addr_list[0];
        sprintf(localip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else {
        localip[0] = 0;
    }
    return 0;
}

void net_shutdown(void)
{
    int i;
    for (i = 0; i < NSOCK; ++i) {
        if (used[i]) {
            soclose(sock[i]);
            used[i] = 0;
        }
    }
}

void net_poll(void) { }
void net_hint_streams(int n) { (void)n; }

void net_idle(void)
{
    fd_set r, w;
    struct timeval tv;
    int i, maxfd = -1;

    FD_ZERO(&r);
    FD_ZERO(&w);
    for (i = 0; i < NSOCK; ++i) {
        if (!used[i]) continue;
        FD_SET(sock[i], &r);
        if (wblock[i]) FD_SET(sock[i], &w);
        if (sock[i] > maxfd) maxfd = sock[i];
    }
    if (maxfd < 0) {
        DosSleep(1);
        return;
    }
    tv.tv_sec = 0;
    tv.tv_usec = 5000;
    select(maxfd + 1, &r, &w, NULL, &tv);
}

unsigned long net_ms(void)
{
    ULONG ms = 0;
    DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ms, sizeof(ms));
    return (unsigned long)ms;
}

const char *net_local_ip(void) { return localip; }

int net_resolve(const char *host, unsigned char ip[4])
{
    unsigned long a = inet_addr(host);
    struct hostent *he;

    if (a != INADDR_NONE) {
        memcpy(ip, &a, 4);
        return 0;
    }
    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list[0] == NULL) return -1;
    memcpy(ip, he->h_addr_list[0], 4);
    return 0;
}

NETH net_connect(const unsigned char ip[4], unsigned port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) return NET_INVALID;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    memcpy(&a.sin_addr, ip, 4);
    nonblock(s);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0 &&
        sock_errno() != EWOULDBLOCK && sock_errno() != EINPROGRESS) {
        soclose(s);
        return NET_INVALID;
    }
    return slot_new(s);
}

int net_connected(NETH h)
{
    fd_set w, e;
    struct timeval tv;
    int error = 0, len = sizeof(error);

    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(sock[h], &w);
    FD_SET(sock[h], &e);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(sock[h] + 1, NULL, &w, &e, &tv) <= 0) return 0;
    if (FD_ISSET(sock[h], &e)) return -1;
    if (!FD_ISSET(sock[h], &w)) return 0;
    if (getsockopt(sock[h], SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) return -1;
    return 1;
}

int net_send(NETH h, const void *buf, unsigned len)
{
    int r = send(sock[h], buf, len, 0);
    if (r < 0) {
        int e = sock_errno();
        if (e == EWOULDBLOCK || e == EINPROGRESS || e == ENOBUFS) {
            wblock[h] = 1;
            return 0;
        }
        return -1;
    }
    wblock[h] = 0;
    return r;
}

int net_recv(NETH h, void *buf, unsigned len)
{
    int r = recv(sock[h], buf, len, 0);
    if (r < 0) {
        int e = sock_errno();
        return (e == EWOULDBLOCK || e == EINPROGRESS) ? 0 : -1;
    }
    return r == 0 ? -1 : r;
}

void net_close(NETH h)
{
    if (h < 0 || h >= NSOCK || !used[h]) return;
    soclose(sock[h]);
    used[h] = 0;
}

NETH net_listen(unsigned port)
{
    struct sockaddr_in a;
    int on = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) return NET_INVALID;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 5) < 0) {
        soclose(s);
        return NET_INVALID;
    }
    nonblock(s);
    return slot_new(s);
}

NETH net_accept(NETH l, unsigned char ip[4])
{
    struct sockaddr_in a;
    int n = sizeof(a);
    int s = accept(sock[l], (struct sockaddr *)&a, &n);

    if (s < 0) return NET_INVALID;
    memcpy(ip, &a.sin_addr, 4);
    nonblock(s);
    return slot_new(s);
}
