/*
 * xbased on Windows: the BSD socket API on Winsock 2, for native/net and
 * libslirp. Sockets are SOCKETs: closed with nc_sock_close, their errors
 * read through nc_sock_errno (as errno values), made non-blocking with
 * nc_sock_nonblock.
 */
#ifndef NC_WIN_NET_H
#define NC_WIN_NET_H
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long nfds_t;

/* An eventfd's wakeups as a loopback UDP socket's datagrams (WSAPoll waits
 * on sockets only). */
int nc_eventfd(void);
void nc_eventfd_write(int fd);
void nc_eventfd_read(int fd);

struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_broadaddr;
    void *ifa_data;
};
int getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);

int nc_sock_close(SOCKET s);
int nc_sock_nonblock(SOCKET s);
int nc_sock_errno(void);
#ifdef __cplusplus
}
#endif
#endif
