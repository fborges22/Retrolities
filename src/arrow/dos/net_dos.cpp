// Arrow - ip3net.h backend for DOS on top of mTCP (packet driver)
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "udp.h"
#include "dns.h"
#include "timer.h"

#include "ip3net.h"

#define NSLOT        TCP_MAX_SOCKETS

// Receive buffer (= advertised TCP window) per socket.  The sum over all data
// streams is kept near 6 segments: classic NICs such as the NE2000 can only
// hold about 8-10 full frames, and overrunning them costs retransmit timeouts.
#define RECV_SEGMENTS 6
static uint16_t recvBufSize = 8192;
static int      listenSlot = -1;

extern "C" volatile unsigned char net_break = 0;

static TcpSocket *slot[NSLOT];
static TcpSocket *closing[NSLOT];
static int        stackUp = 0;
static uint16_t   nextPort;
static char       localIp[20];

static void __interrupt __far breakHandler( ) { net_break = 1; }

static inline void drive( void ) {
  PACKET_PROCESS_SINGLE;
  Arp::driveArp( );
  Tcp::drivePackets( );
}

static int newSlot( TcpSocket *s ) {
  for ( int i = 0; i < NSLOT; i++ ) if ( slot[i] == NULL ) { slot[i] = s; return i; }
  return -1;
}

int net_init( char *err, int errlen ) {
  if ( Utils::parseEnv( ) != 0 ) {
    strncpy( err, "mTCP configuration not found: set MTCPCFG=<config file> (see README)", errlen );
    return -1;
  }
  if ( Utils::initStack( TCP_MAX_SOCKETS, TCP_MAX_XMIT_BUFS, breakHandler, breakHandler ) ) {
    strncpy( err, "could not initialise TCP/IP - is the packet driver loaded?", errlen );
    return -1;
  }
  stackUp = 1;
  // Start local ports from the BIOS time of day so consecutive runs do not
  // reuse the port numbers of connections the peer may still remember.
  uint32_t far *biosTicks = (uint32_t far *)MK_FP( 0x40, 0x6c );
  nextPort = 2048 + (uint16_t)( ( *biosTicks * 8ul ) % 30000ul );
  sprintf( localIp, "%d.%d.%d.%d", MyIpAddr[0], MyIpAddr[1], MyIpAddr[2], MyIpAddr[3] );
  net_hint_streams( 1 );
  return 0;
}

void net_shutdown( void ) {
  if ( !stackUp ) return;
  // Let queued data and FINs go out so the peer sees a clean close.
  clockTicks_t start = TIMER_GET_CURRENT( );
  for ( ;; ) {
    int busy = 0;
    net_poll( );
    for ( int i = 0; i < NSLOT; i++ ) if ( closing[i] != NULL ) busy = 1;
    if ( !busy || (TIMER_GET_CURRENT( ) - start) > 54 ) break;
  }
  Utils::endStack( );
  stackUp = 0;
}

const char *net_local_ip( void ) { return localIp; }

void net_hint_streams( int n ) {
  int segs = RECV_SEGMENTS / ( n < 1 ? 1 : n );
  if ( segs < 2 ) segs = 2;
  recvBufSize = segs * TcpSocketMgr::MSS_to_advertise;
  // Sockets accepted from now on inherit the listener's buffer size.
  if ( listenSlot >= 0 && slot[listenSlot] != NULL ) slot[listenSlot]->rcvBufSize = recvBufSize;
}

void net_poll( void ) {
  drive( );
  for ( int i = 0; i < NSLOT; i++ ) {
    if ( closing[i] == NULL ) continue;
    if ( closing[i]->isCloseDone( ) ) {
      TcpSocketMgr::freeSocket( closing[i] );
      closing[i] = NULL;
    }
    else {
      // Keep discarding inbound data: a peer that still has bytes queued for
      // us cannot finish the close handshake while our window is shut.
      static uint8_t scratch[512];
      while ( closing[i]->recv( scratch, sizeof( scratch ) ) > 0 ) { }
    }
  }
}

void net_idle( void ) { }

unsigned long net_ms( void ) {
  // BIOS ticks are 54.925 ms apart
  return ( (unsigned long)TIMER_GET_CURRENT( ) * 5493ul ) / 100ul;
}

int net_resolve( const char *host, unsigned char ip[4] ) {
  IpAddr_t addr;
  if ( Dns::resolve( host, addr, 1 ) < 0 ) return -1;
  while ( Dns::isQueryPending( ) ) {
    if ( net_break ) return -1;
    drive( );
    Dns::drivePendingQuery( );
  }
  if ( Dns::resolve( host, addr, 0 ) != 0 ) return -1;
  memcpy( ip, addr, 4 );
  return 0;
}

NETH net_connect( const unsigned char ip[4], unsigned port ) {
  TcpSocket *s = TcpSocketMgr::getSocket( );
  if ( s == NULL ) return NET_INVALID;
  IpAddr_t addr;
  memcpy( addr, ip, 4 );
  if ( ++nextPort < 2048 ) nextPort = 2048;
  if ( s->setRecvBuffer( recvBufSize ) || s->connectNonBlocking( nextPort, addr, port ) ) {
    TcpSocketMgr::freeSocket( s );
    return NET_INVALID;
  }
  int h = newSlot( s );
  if ( h < 0 ) { s->close( ); TcpSocketMgr::freeSocket( s ); }
  return h;
}

int net_connected( NETH h ) {
  TcpSocket *s = slot[h];
  if ( s->isConnectComplete( ) ) return 1;
  if ( s->isClosed( ) ) return -1;
  return 0;
}

int net_send( NETH h, const void *buf, unsigned len ) {
  TcpSocket *s = slot[h];
  if ( s->isRemoteClosed( ) || s->isClosed( ) ) return -1;
  int16_t rc = s->send( (uint8_t *)buf, len );
  return rc < 0 ? -1 : rc;
}

int net_recv( NETH h, void *buf, unsigned len ) {
  TcpSocket *s = slot[h];
  int16_t rc = s->recv( (uint8_t *)buf, len );
  if ( rc > 0 ) return rc;
  if ( rc < 0 || s->isRemoteClosed( ) || s->isClosed( ) ) return -1;
  return 0;
}

void net_close( NETH h ) {
  TcpSocket *s = slot[h];
  if ( s == NULL ) return;
  slot[h] = NULL;
  if ( h == listenSlot ) listenSlot = -1;
  for ( int i = 0; i < NSLOT; i++ ) {
    if ( closing[i] == NULL ) {
      s->closeNonblocking( );
      closing[i] = s;
      return;
    }
  }
  s->close( );
  TcpSocketMgr::freeSocket( s );
}

NETH net_listen( unsigned port ) {
  TcpSocket *s = TcpSocketMgr::getSocket( );
  if ( s == NULL ) return NET_INVALID;
  net_hint_streams( 1 );
  if ( s->listen( port, recvBufSize ) ) { TcpSocketMgr::freeSocket( s ); return NET_INVALID; }
  int h = newSlot( s );
  if ( h < 0 ) { s->close( ); TcpSocketMgr::freeSocket( s ); }
  listenSlot = h;
  return h;
}

NETH net_accept( NETH l, unsigned char ip[4] ) {
  (void)l;
  TcpSocket *s = TcpSocketMgr::accept( );
  if ( s == NULL ) return NET_INVALID;
  int h = newSlot( s );
  if ( h < 0 ) { s->close( ); TcpSocketMgr::freeSocket( s ); return NET_INVALID; }
  memcpy( ip, s->dstHost, 4 );
  return h;
}
