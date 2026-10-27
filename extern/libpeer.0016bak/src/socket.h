#ifndef SOCKET_H_
#define SOCKET_H_

#include "address.h"

typedef struct UdpSocket {
  int fd;
  Address bind_addr;
} UdpSocket;

typedef struct TcpSocket {
  int fd;
  Address bind_addr;
} TcpSocket;

int udp_socket_open(UdpSocket* udp_socket, int family, int port);

int udp_socket_bind(UdpSocket* udp_socket, int port);

void udp_socket_close(UdpSocket* udp_socket);

int udp_socket_sendto(UdpSocket* udp_socket, Address* bind_addr, const uint8_t* buf, int len);

int udp_socket_recvfrom(UdpSocket* udp_sock, Address* bind_addr, uint8_t* buf, int len);

/**
 * Read ONE datagram WITHOUT blocking.
 *
 * 0040: the DTLS bio has to be able to say "there is nothing right now"
 * instead of waiting for a packet -- a blocking read in the bio stalls the
 * whole peer connection loop (the ICE retransmission timer, the consent
 * freshness timer and the media pump all live in it).
 *
 * Returns the number of bytes, or -1 when the socket is dry.  It NEVER
 * returns 0: a zero length read would be indistinguishable from "the peer
 * closed the connection" (mbedTLS turns a 0 from the bio into
 * MBEDTLS_ERR_SSL_CONN_EOF, -0x7280).
 */
int udp_socket_recvfrom_nonblocking(UdpSocket* udp_socket, Address* addr, uint8_t* buf, int len);

int udp_socket_add_multicast_group(UdpSocket* udp_socket, Address* mcast_addr);

int tcp_socket_open(TcpSocket* tcp_socket, int family);

int tcp_socket_connect(TcpSocket* tcp_socket, Address* addr);

void tcp_socket_close(TcpSocket* tcp_socket);

int tcp_socket_send(TcpSocket* tcp_socket, const uint8_t* buf, int len);

int tcp_socket_recv(TcpSocket* tcp_socket, uint8_t* buf, int len);

#endif  // SOCKET_H_
