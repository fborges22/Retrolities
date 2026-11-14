/* NetBolt - ip3net.h backend for Winsock 1.1 (Win16, Win32) and,
   with IP3_WINNT, Winsock 2.2 (Windows NT 4.0 / 2000 / x64 Windows) */
#ifdef IP3_WINNT
#include <winsock2.h>
#include <windows.h>
#define WS_VERSION 0x0202
#define SOCK_BUF   262144L      /* per-socket buffers for fast LANs */
#else
#include <windows.h>
#include <winsock.h>
#define WS_VERSION 0x0101
#endif
#include <string.h>
#include "ip3net.h"

#define NSOCK 24

static SOCKET sk[NSOCK];
static char   used[NSOCK];
static char   wblock[NSOCK];
static int    started;
static char   localip[20];

static int slot_new(SOCKET s)
{
    int i;
    for (i = 0; i < NSOCK; i++)
        if (!used[i]) { used[i] = 1; wblock[i] = 0; sk[i] = s; return i; }
    closesocket(s);
    return NET_INVALID;
}

#ifdef IP3_WINNT
static LARGE_INTEGER qfreq, qbase;

/* must be set before connect()/listen() so the TCP window is negotiated to match */
static void tune(SOCKET s)
{
    int n = SOCK_BUF;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&n, sizeof(n));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&n, sizeof(n));
}
#else
#define tune(s)
#endif

static void nonblock(SOCKET s)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}

int net_init(char *err, int errlen)
{
    WSADATA w;
    if (WSAStartup(WS_VERSION, &w) != 0) {
        strncpy(err, "Windows Sockets (WINSOCK) is not available. Install a TCP/IP stack first.", errlen);
        return -1;
    }
#ifdef IP3_WINNT
    if (!QueryPerformanceFrequency(&qfreq) || !QueryPerformanceCounter(&qbase))
        qfreq.QuadPart = 0;
#endif
    started = 1;
    return 0;
}

void net_shutdown(void)
{
    int i;
    if (!started) return;
    for (i = 0; i < NSOCK; i++) if (used[i]) closesocket(sk[i]);
    WSACleanup();
    started = 0;
}

void net_poll(void) { }
void net_hint_streams(int n) { (void)n; }

void net_idle(void)
{
#if defined(__NT__) || defined(_WIN32)
    fd_set r, w;
    struct timeval tv;
    int i, n = 0;
    FD_ZERO(&r); FD_ZERO(&w);
    for (i = 0; i < NSOCK; i++) {
        if (!used[i]) continue;
        FD_SET(sk[i], &r);
        if (wblock[i]) FD_SET(sk[i], &w);
        n++;
    }
    tv.tv_sec = 0; tv.tv_usec = 5000;
    if (n) select(0, &r, &w, NULL, &tv); else Sleep(1);
#endif
    /* Win16: the caller's PeekMessage loop yields to other applications */
}

unsigned long net_ms(void)
{
#ifdef IP3_WINNT
    /* GetTickCount() only advances every 10-16 ms on NT */
    LARGE_INTEGER q;
    if (qfreq.QuadPart > 0 && QueryPerformanceCounter(&q))
        return (unsigned long)((q.QuadPart - qbase.QuadPart) * 1000 / qfreq.QuadPart);
#endif
    return GetTickCount();
}

const char *net_local_ip(void)
{
    char name[64];
    struct hostent FAR *he;
    localip[0] = 0;
    if (gethostname(name, sizeof(name)) == 0 && (he = gethostbyname(name)) != NULL) {
        unsigned char FAR *a = (unsigned char FAR *)he->h_addr_list[0];
        wsprintf(localip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    }
    return localip;
}

int net_resolve(const char *host, unsigned char ip[4])
{
    unsigned long a = inet_addr(host);
    struct hostent FAR *he;
    if (a != INADDR_NONE) { memcpy(ip, &a, 4); return 0; }
    he = gethostbyname(host);
    if (he == NULL) return -1;
#if defined(__NT__) || defined(_WIN32)
    memcpy(ip, he->h_addr_list[0], 4);
#else
    _fmemcpy(ip, he->h_addr_list[0], 4);
#endif
    return 0;
}

NETH net_connect(const unsigned char ip[4], unsigned port)
{
    struct sockaddr_in a;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return NET_INVALID;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    memcpy(&a.sin_addr, ip, 4);
    tune(s);
    nonblock(s);
    if (connect(s, (struct sockaddr FAR *)&a, sizeof(a)) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return NET_INVALID;
    }
    return slot_new(s);
}

int net_connected(NETH h)
{
    fd_set w, e;
    struct timeval tv;
    tv.tv_sec = 0; tv.tv_usec = 0;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(sk[h], &w); FD_SET(sk[h], &e);
    if (select(0, NULL, &w, &e, &tv) <= 0) return 0;
    if (FD_ISSET(sk[h], &e)) return -1;
    return FD_ISSET(sk[h], &w) ? 1 : 0;
}

int net_send(NETH h, const void *buf, unsigned len)
{
    int r = send(sk[h], (const char FAR *)buf, (int)len, 0);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAENOBUFS || e == WSAEINPROGRESS) { wblock[h] = 1; return 0; }
        return -1;
    }
    wblock[h] = 0;
    return r;
}

int net_recv(NETH h, void *buf, unsigned len)
{
    int r = recv(sk[h], (char FAR *)buf, (int)len, 0);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        return (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS) ? 0 : -1;
    }
    return r == 0 ? -1 : r;
}

void net_close(NETH h)
{
    if (h < 0 || !used[h]) return;
    closesocket(sk[h]);
    used[h] = 0;
}

NETH net_listen(unsigned port)
{
    struct sockaddr_in a;
    int on = 1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return NET_INVALID;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char FAR *)&on, sizeof(on));
    tune(s);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (bind(s, (struct sockaddr FAR *)&a, sizeof(a)) == SOCKET_ERROR || listen(s, 5) == SOCKET_ERROR) {
        closesocket(s);
        return NET_INVALID;
    }
    nonblock(s);
    return slot_new(s);
}

NETH net_accept(NETH l, unsigned char ip[4])
{
    struct sockaddr_in a;
    int n = sizeof(a);
    SOCKET s = accept(sk[l], (struct sockaddr FAR *)&a, &n);
    if (s == INVALID_SOCKET) return NET_INVALID;
    memcpy(ip, &a.sin_addr, 4);
    nonblock(s);
    return slot_new(s);
}
