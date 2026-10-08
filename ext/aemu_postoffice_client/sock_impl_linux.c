#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <string.h>
#include <stdio.h>

#include "postoffice_client.h"
#include "sock_impl.h"
#include "log_impl.h"

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src){
	dst->sin_family = AF_INET;
	dst->sin_addr.s_addr = src->addr;
	dst->sin_port = src->port;
}

void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src){
	dst->sin6_family = AF_INET6;
	dst->sin6_port = src->port;
	dst->sin6_flowinfo = 0;
	memcpy(dst->sin6_addr.s6_addr, src->addr, 16);
	dst->sin6_scope_id = 0;
}

int native_wait(int fd, bool for_write, int timeout_ms, int cancel_fd){
	struct pollfd pfd[2];
	int count = 1;
	pfd[0].fd = fd;
	pfd[0].events = for_write ? POLLOUT : POLLIN;
	pfd[0].revents = 0;
	if (cancel_fd >= 0){
		pfd[1].fd = cancel_fd;
		pfd[1].events = POLLIN;
		pfd[1].revents = 0;
		count = 2;
	}
	int result;
	do {
		result = poll(pfd, count, timeout_ms);
	} while (result < 0 && errno == EINTR);
	if (result < 0){
		return -1;
	}
	if (result == 0){
		return 0;
	}
	if (count == 2 && (pfd[1].revents & POLLIN)){
		return NATIVE_SOCK_ABORTED;
	}
	/* POLLERR / POLLHUP count as ready: the next call on the socket reports them. */
	return 1;
}

static int connect_with_timeout(int sock, void *addr, int addrlen, int timeout_ms, int *error, int cancel_fd){
	int flags = fcntl(sock, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);

	int ret = 0;
	if (connect(sock, (struct sockaddr *)addr, addrlen) != 0){
		*error = errno;
		if (*error != EINPROGRESS && *error != EAGAIN && *error != EALREADY){
			return -1;
		}
		/* One wait for the outcome, a cancel, or the timeout. */
		int wait = native_wait(sock, true, timeout_ms, cancel_fd);
		if (wait == 0){
			*error = ETIMEDOUT;
			ret = -1;
		}else if (wait == NATIVE_SOCK_ABORTED){
			*error = ECANCELED;
			ret = -1;
		}else if (wait < 0){
			*error = errno;
			ret = -1;
		}else{
			int so_error = 0;
			socklen_t so_len = sizeof(so_error);
			getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
			*error = so_error;
			ret = so_error == 0 ? 0 : -1;
		}
	}

	// just for completness, NBIO is used on the socket after connection anyway
	flags = fcntl(sock, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);

	return ret;
}


int native_connect_tcp_sock(void *addr, int addrlen, int cancel_fd){
	native_sock_addr *native_addr = addr;
	int sock = socket(native_addr->sin_family, SOCK_STREAM, 0);
	if (sock == -1){
		LOG("%s: failed creating socket, %s\n", __func__, strerror(errno));
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// XXX this restricts latency to 500ms, if a server is even further away, connection won't be possible

	// Connect
	int error = 0;
	int connect_status = connect_with_timeout(sock, addr, addrlen, 5000, &error, cancel_fd);
	if (connect_status == -1){
		LOG("%s: failed connecting, %s\n", __func__, strerror(error));
		close(sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// Set socket options
	socklen_t sockopt = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt));
	int flags = fcntl(sock, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);

	sockopt = 2626560;
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockopt, sizeof(sockopt));
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockopt, sizeof(sockopt));

	#ifndef __linux__
	sockopt = 1;
	setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &sockopt, sizeof(sockopt));
	#endif

	// Show some socket options
	unsigned int opt_len = sizeof(sockopt);
	sockopt = 0;
	int get_ret = getsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, &opt_len);
	LOG("%s: TCP_NODELAY is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockopt, &opt_len);
	LOG("%s: SO_SNDBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockopt, &opt_len);
	LOG("%s: SO_RCVBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);

	#ifndef __linux__
	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &sockopt, &opt_len);
	LOG("%s: SO_NOSIGPIPE is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);
	#endif

	return sock;
}

int native_send_till_done(int fd, const char *buf, int len, bool non_block, retro_atomic_int_t *abort, int cancel_fd){
	int write_offset = 0;
	while(write_offset != len){
		if (retro_atomic_load_acquire_int(abort)){
			return NATIVE_SOCK_ABORTED;
		}
		#ifdef __linux__
		int write_status = send(fd, &buf[write_offset], len - write_offset, MSG_NOSIGNAL);
		#else
		int write_status = send(fd, &buf[write_offset], len - write_offset, 0);
		#endif
		if (write_status == -1){
			int err = errno;
			if (err == EAGAIN || err == EWOULDBLOCK){
				if (non_block && write_offset == 0){
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				/* Block until it can go on: in block mode, or part of the message is out already. */
				if (native_wait(fd, true, -1, cancel_fd) == NATIVE_SOCK_ABORTED){
					return NATIVE_SOCK_ABORTED;
				}
				continue;
			}
			// Other errors
			LOG("%s: failed sending, %s\n", __func__, strerror(errno));
			return write_status;
		}
		write_offset += write_status;
	}
	return write_offset;
}

int native_recv(int fd, char *buf, int len){
	int recv_status = recv(fd, buf, len, 0);
	if (recv_status == 0){
		return recv_status;
	}
	if (recv_status < 0){
		int err = errno;
		if (err == EAGAIN || err == EWOULDBLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		// Other errors
		LOG("%s: failed receving, %s\n", __func__, strerror(errno));
		return recv_status;
	}
	return recv_status;
}

int native_recv_till_done(int fd, char *buf, int len, bool non_block, retro_atomic_int_t *abort, int cancel_fd){
	int read_offset = 0;
	while(read_offset != len){
		if (retro_atomic_load_acquire_int(abort)){
			return NATIVE_SOCK_ABORTED;
		}
		int recv_status = native_recv(fd, &buf[read_offset], len - read_offset);
		if (recv_status == 0){
			return recv_status;
		}
		if (recv_status < 0){
			if (recv_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
				if (non_block && read_offset == 0){
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				/* Block until more comes: in block mode, or part of the message is in already. */
				if (native_wait(fd, false, -1, cancel_fd) == NATIVE_SOCK_ABORTED){
					return NATIVE_SOCK_ABORTED;
				}
				continue;
			}
			return recv_status;
		}
		read_offset += recv_status;
	}
	return read_offset;
}

int native_close_tcp_sock(int sock){
	return close(sock);
}

int native_shutdown_tcp_sock(int sock){
	return shutdown(sock, SHUT_RDWR);
}

int native_peek(int fd, char *buf, int len){
	int read_result = recv(fd, buf, len, MSG_PEEK);
	if (read_result == 0){
		return 0;
	}
	if (read_result == -1){
		int err = errno;
		if (err == EAGAIN || err == EWOULDBLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		LOG("%s: failed peeking, %s\n", __func__, strerror(errno));
		return -1;
	}
	return read_result;
}

bool native_send_buf_not_full(int fd){
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLWRNORM;
	pfd.revents = 0;
	poll(&pfd, 1, 0);
	if (pfd.revents & POLLHUP){
		return false;
	}
	if (pfd.revents & POLLWRNORM){
		return true;
	}
	return false;
}

bool native_hung_up(int fd){
	uint8_t buf;
	int peek_result = native_peek(fd, (char *)&buf, sizeof(buf));
	if (peek_result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK ||
		peek_result == sizeof(buf)
	){
		return false;
	}
	return true;
}
