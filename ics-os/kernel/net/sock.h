#ifndef ICSOS_NET_SOCK_H
#define ICSOS_NET_SOCK_H

/* Kernel Berkeley-ish socket objects bound to POSIX FD_SOCK. */

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define INADDR_ANY   0u

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;   /* network order */
    unsigned int   sin_addr;   /* network order */
    unsigned char  sin_zero[8];
};

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

struct sock;

long sys_socket(int domain, int type, int protocol);
long sys_bind(int fd, const struct sockaddr *addr, unsigned int addrlen);
long sys_listen(int fd, int backlog);
long sys_accept(int fd, struct sockaddr *addr, unsigned int *addrlen);
long sys_connect(int fd, const struct sockaddr *addr, unsigned int addrlen);
long sys_send(int fd, const void *buf, unsigned long len, int flags);
long sys_recv(int fd, void *buf, unsigned long len, int flags);
/* DEX syscall arity is 5; flags are omitted (SDK wrappers ignore flags). */
long sys_sendto(int fd, const void *buf, unsigned long len,
                const struct sockaddr *addr, unsigned int addrlen);
long sys_recvfrom(int fd, void *buf, unsigned long len,
                  struct sockaddr *addr, unsigned int *addrlen);
void sock_close(void *sock);
int  sock_read(void *sock, void *buf, long n);
int  sock_write(void *sock, const void *buf, long n);
int  sock_fd_alloc(void);
void sock_fd_install(int fd, void *sock);
void sock_fd_release(int fd);
struct sock *sock_from_fd(int fd);

#endif
