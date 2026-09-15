#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define INADDR_ANY   0u
#define SOL_SOCKET   1
#define SO_REUSEADDR 2

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
typedef unsigned int in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    sa_family_t sin_family;
    unsigned short sin_port;
    in_addr_t sin_addr;
    unsigned char sin_zero[8];
};

static inline unsigned short htons(unsigned short x)
{
    return (unsigned short)((x << 8) | (x >> 8));
}

static inline unsigned short ntohs(unsigned short x)
{
    return htons(x);
}

static inline unsigned int htonl(unsigned int x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}

static inline unsigned int ntohl(unsigned int x)
{
    return htonl(x);
}

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *addr, socklen_t addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *addr, socklen_t *addrlen);

#endif
