/* Test the actual backend against scripted SDK responses, without a DOS TSR. */
#include <assert.h>
#include <stdio.h>
#include "../dos/net_msclient.c"

static test_vector vectors[256];
static long clock_ticks;
static int next_socket = 0x100, closed, last_closed;
static int socket_error, ioctl_error, connect_error, bind_error, listen_error;
static int select_result, select_exception, peer_error, io_result, io_error;
static int accepted = -1, dns_calls, last_len;
static int max_send;
static struct sockaddr_in target;

test_vector _dos_getvect(unsigned n) { return vectors[n]; }
void _dos_setvect(unsigned n, test_vector h) { vectors[n] = h; }
unsigned _bios_timeofday(unsigned command, long *ticks)
{
    assert(command == _TIME_GETCLOCK);
    *ticks = clock_ticks;
    return 0;
}

static int result(int error, int ok, int *e)
{
    *e = error;
    return error ? -1 : ok;
}

int _socket(int family, int type, int protocol, int *e)
{
    assert(family == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    return result(socket_error, next_socket++, e);
}
int _close_socket(int s, int *e)
{
    last_closed = s;
    closed++;
    return result(0, 0, e);
}
int _ioctl(int s, int cmd, char *p, int *e)
{
    (void)s;
    assert(cmd == FIONBIO && *(long *)p == 1);
    return result(ioctl_error, 0, e);
}
int htons(int n) { return ((n & 255) << 8) | ((n >> 8) & 255); }
int _connect(int s, struct sockaddr *a, int n, int *e)
{
    (void)s;
    assert(n == sizeof(target));
    memcpy(&target, a, n);
    return result(connect_error, 0, e);
}
int _select(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *tv, int *e)
{
    assert(n == MAX_SOCKETS && !r && tv->tv_sec == 0 && tv->tv_usec == 0);
    if (!select_exception) FD_ZERO(x);
    if (!select_result) FD_ZERO(w);
    return result(select_result < 0 ? ENETDOWN : 0, select_result, e);
}
int _getpeername(int s, struct sockaddr *a, int *n, int *e)
{
    (void)s; (void)a; (void)n;
    return result(peer_error, 0, e);
}
int _getsockname(int s, struct sockaddr *a, int *n, int *e)
{
    static const unsigned char ip[4] = {192, 168, 1, 20};
    struct sockaddr_in *p = (struct sockaddr_in *)a;
    (void)s;
    assert(*n == sizeof(*p));
    memset(p, 0, sizeof(*p));
    memcpy(&p->sin_addr, ip, 4);
    return result(0, 0, e);
}
int _send(int s, char *p, int n, int flags, int *e)
{
    (void)s; (void)p;
    assert(flags == 0);
    last_len = n;
    if (max_send && n > max_send) return result(EMSGSIZE, 0, e);
    return result(io_error, io_result, e);
}
int _recv(int s, char *p, int n, int flags, int *e)
{
    return _send(s, p, n, flags, e);
}
int _setsockopt(int s, int level, int option, char *p, int n, int *e)
{
    (void)s;
    assert(level == SOL_SOCKET && option == SO_REUSEADDR);
    assert(n == sizeof(int) && *(int *)p == 1);
    return result(0, 0, e);
}
int _bind(int s, struct sockaddr *a, int n, int *e)
{
    (void)s;
    assert(n == sizeof(target));
    memcpy(&target, a, n);
    assert(target.sin_addr.s_addr == 0);
    return result(bind_error, 0, e);
}
int _listen(int s, int n, int *e)
{
    (void)s;
    assert(n == SOMAXCONN);
    return result(listen_error, 0, e);
}
int _accept(int s, struct sockaddr *a, int *n, int *e)
{
    static const unsigned char ip[4] = {10, 0, 0, 7};
    struct sockaddr_in *p = (struct sockaddr_in *)a;
    (void)s;
    assert(*n == sizeof(*p));
    memcpy(&p->sin_addr, ip, 4);
    return result(accepted < 0 ? EWOULDBLOCK : 0, accepted, e);
}
struct hostent *gethostbyname(char *name)
{
    static struct in_addr addr;
    static struct in_addr *list[] = {&addr, NULL};
    static struct hostent he;
    static const unsigned char ip[4] = {10, 1, 2, 3};
    dns_calls++;
    if (strcmp(name, "peer")) return NULL;
    memcpy(&addr, ip, 4);
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}

static void original_handler(void) { }

int main(void)
{
    char err[100], tiny[3], buf[8];
    unsigned char ip[4] = {192, 168, 1, 10};
    NETH h, l, a, held[IP3_MAX_STREAMS + 2];
    int i, before;
    unsigned long t;

    vectors[0x1b] = vectors[0x23] = original_handler;
    socket_error = EHOSTDOWN;
    assert(net_init(tiny, sizeof(tiny)) < 0 && tiny[2] == 0);
    assert(vectors[0x23] == original_handler);
    socket_error = 0;
    clock_ticks = DAY_TICKS - 2;
    assert(net_init(err, sizeof(err)) == 0);
    clock_ticks = 1;
    assert(net_ms() == 164);
    clock_ticks += 80000;
    t = net_ms();
    assert(t == (80003UL * 54925UL) / 1000UL);
    assert(net_ms() == t);
    vectors[0x23]();
    assert(net_break);
    net_break = 0;

    assert(net_resolve("192.168.1.10", ip) == 0 && dns_calls == 0);
    assert(ip[0] == 192 && ip[3] == 10);
    assert(net_resolve("256.1.1.1", ip) < 0);
    assert(net_resolve("1.2.3.4junk", ip) < 0);
    assert(net_resolve("peer", ip) == 0 && ip[0] == 10 && ip[3] == 3);

    connect_error = EINPROGRESS;
    h = net_connect(ip, 5201);
    assert(h != NET_INVALID && target.sin_port == htons(5201));
    assert(!memcmp(&target.sin_addr, ip, 4));
    assert(net_connected(h) == 0);
    select_result = 1;
    peer_error = ENOTCONN;
    assert(net_connected(h) == 0);
    peer_error = 0;
    assert(net_connected(h) == 1);
    assert(!strcmp(net_local_ip(), "192.168.1.20"));
    select_exception = 1;
    assert(net_connected(h) == -1);
    select_exception = 0;
    select_result = -1;
    assert(net_connected(h) == -1);
    select_result = 1;

    io_error = EWOULDBLOCK;
    assert(net_send(h, buf, sizeof(buf)) == 0);
    assert(net_recv(h, buf, sizeof(buf)) == 0);
    io_error = ECONNRESET;
    assert(net_send(h, buf, sizeof(buf)) == -1);
    assert(net_recv(h, buf, sizeof(buf)) == -1);
    io_error = 0;
    io_result = 3;
    assert(net_send(h, buf, sizeof(buf)) == 3);
    assert(net_recv(h, buf, sizeof(buf)) == 3);
    assert(net_send(h, buf, 65535U) == 3 && last_len == 32767);
    max_send = 1024;
    assert(net_send(h, buf, 8192U) == 3 && last_len == 1024);
    assert(net_send(h, buf, 8192U) == 3 && last_len == 1024);
    max_send = 0;
    io_result = 0;
    assert(net_recv(h, buf, sizeof(buf)) == -1);
    net_close(h);
    assert(net_connected(h) == -1);
    before = closed;
    net_close(h);
    assert(closed == before);

    connect_error = ECONNREFUSED;
    assert(net_connect(ip, 5201) == NET_INVALID && closed == before + 1);
    connect_error = 0;
    ioctl_error = ENOBUFS;
    before = closed;
    assert(net_connect(ip, 5201) == NET_INVALID && closed == before + 1);
    ioctl_error = 0;
    bind_error = EADDRINUSE;
    before = closed;
    assert(net_listen(5201) == NET_INVALID && closed == before + 1);
    bind_error = 0;
    l = net_listen(5201);
    assert(l != NET_INVALID);
    assert(net_accept(l, ip) == NET_INVALID);
    accepted = 0x130;
    ioctl_error = ENOBUFS;
    assert(net_accept(l, ip) == NET_INVALID && last_closed == accepted);
    ioctl_error = 0;
    a = net_accept(l, ip);
    assert(a != NET_INVALID && ip[0] == 10 && ip[3] == 7);
    net_close(a);
    net_close(l);

    for (i = 0; i < IP3_MAX_STREAMS + 2; i++) {
        held[i] = net_connect(ip, 5201);
        assert(held[i] != NET_INVALID);
    }
    before = closed;
    assert(net_connect(ip, 5201) == NET_INVALID && closed == before + 1);
    before = closed;
    net_shutdown();
    assert(closed == before + IP3_MAX_STREAMS + 2);
    assert(vectors[0x1b] == original_handler && vectors[0x23] == original_handler);
    net_shutdown();
    assert(closed == before + IP3_MAX_STREAMS + 2);
    assert(net_connect(ip, 5201) == NET_INVALID);
    assert(net_init(err, sizeof(err)) == 0 && !net_break);
    net_shutdown();
    puts("MSCLIENT backend tests passed");
    return 0;
}
