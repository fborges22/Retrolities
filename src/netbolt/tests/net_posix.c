/* POSIX backend for ip3net.h - lets the protocol engine be tested on the build host */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ip3net.h"

static void nb(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
int net_init(char *e, int n) { (void)e; (void)n; return 0; }
void net_shutdown(void) {}
void net_hint_streams(int n) { (void)n; }
void net_poll(void) {}
void net_idle(void) { usleep(200); }
unsigned long net_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000UL + t.tv_nsec / 1000000; }
const char *net_local_ip(void) { return "0.0.0.0"; }
int net_resolve(const char *h, unsigned char ip[4]) {
    struct hostent *he = gethostbyname(h);
    if (!he) return -1;
    memcpy(ip, he->h_addr_list[0], 4); return 0;
}
NETH net_connect(const unsigned char ip[4], unsigned port) {
    struct sockaddr_in a; int s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); memcpy(&a.sin_addr, ip, 4);
    nb(s);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(s); return -1; }
    return s;
}
int net_connected(NETH h) {
    fd_set w; struct timeval tv = {0, 0}; int e = 0; socklen_t l = sizeof e;
    FD_ZERO(&w); FD_SET(h, &w);
    if (select(h + 1, NULL, &w, NULL, &tv) <= 0) return 0;
    getsockopt(h, SOL_SOCKET, SO_ERROR, &e, &l);
    return e ? -1 : 1;
}
int net_send(NETH h, const void *b, unsigned n) {
    ssize_t r = send(h, b, n, MSG_NOSIGNAL);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (int)r;
}
int net_recv(NETH h, void *b, unsigned n) {
    ssize_t r = recv(h, b, n, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return r == 0 ? -1 : (int)r;
}
void net_close(NETH h) { close(h); }
NETH net_listen(unsigned port) {
    struct sockaddr_in a; int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) { close(s); return -1; }
    nb(s); return s;
}
NETH net_accept(NETH l, unsigned char ip[4]) {
    struct sockaddr_in a; socklen_t n = sizeof a; int s = accept(l, (struct sockaddr *)&a, &n);
    if (s < 0) return -1;
    memcpy(ip, &a.sin_addr, 4); nb(s); return s;
}
