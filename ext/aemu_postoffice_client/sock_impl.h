#ifndef __SOCK_IMPL_COMMON_H
#define __SOCK_IMPL_COMMON_H

#include <stdbool.h>

#if defined(__unix) || defined(__APPLE__) || defined(__PSP__)
#include <netinet/in.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#endif

#include "postoffice_client.h"

typedef struct sockaddr_in native_sock_addr;

#ifdef __PSP__
typedef int native_sock6_addr;
#else
typedef struct sockaddr_in6 native_sock6_addr;
#endif

#include <retro_atomic.h>

#define NATIVE_SOCK_ABORTED -100

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src);
void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src);

/* cancel_fd: -1, or a socket that becomes readable to give up a wait. */
int native_connect_tcp_sock(void *addr, int addrlen, int cancel_fd);
int native_close_tcp_sock(int sock);
/* Ends any wait blocked on sock, for a close from another thread. */
int native_shutdown_tcp_sock(int sock);
int native_send_till_done(int fd, const char *buf, int len, bool non_block, retro_atomic_int_t *abort, int cancel_fd);
int native_recv_till_done(int fd, char *buf, int len, bool non_block, retro_atomic_int_t *abort, int cancel_fd);
/* Blocks until fd is readable (or writable), cancel_fd is readable, or timeout_ms
   passes (< 0: no limit). 1 ready, 0 timeout, NATIVE_SOCK_ABORTED cancelled, -1 error. */
int native_wait(int fd, bool for_write, int timeout_ms, int cancel_fd);
int native_recv(int fd, char *buf, int len);
int native_peek(int fd, char *buf, int len);
bool native_send_buf_not_full(int fd);
bool native_hung_up(int fd);

#endif
