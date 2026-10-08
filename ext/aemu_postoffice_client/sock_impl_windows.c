#include <winsock2.h>
#include <windows.h>

#include <string.h>
#include <stdio.h>

#include "postoffice_client.h"
#include "sock_impl.h"
#include "log_impl.h"

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src){
	dst->sin_family = AF_INET;
	dst->sin_addr.s_addr = src->addr;
	dst->sin_port = src->port;
	memset(dst->sin_zero, 0, sizeof(dst->sin_zero));
}

void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src){
	dst->sin6_family = AF_INET6;
	dst->sin6_port = src->port;
	dst->sin6_flowinfo = 0;
	memcpy(dst->sin6_addr.s6_addr, src->addr, 16);
	dst->sin6_scope_id = 0;
}

static void init_winsock2(){
	static bool initialized = false;
	if (!initialized){
		initialized = true;
		WSADATA data;
		int init_result = WSAStartup(MAKEWORD(2,2), &data);
		if (init_result != 0){
			printf("%s: warning: WSAStartup seems to have failed, %d\n", __func__, init_result);
		}
	}
}

int native_wait(int fd, bool for_write, int timeout_ms, int cancel_fd){
	fd_set rfds, wfds, efds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	FD_SET((SOCKET)fd, for_write ? &wfds : &rfds);
	/* A failed non-blocking connect is reported as an exception on Windows. */
	FD_SET((SOCKET)fd, &efds);
	if (cancel_fd >= 0){
		FD_SET((SOCKET)cancel_fd, &rfds);
	}
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	int result = select(0, &rfds, &wfds, &efds, timeout_ms < 0 ? NULL : &tv);
	if (result == SOCKET_ERROR){
		return -1;
	}
	if (result == 0){
		return 0;
	}
	if (cancel_fd >= 0 && FD_ISSET((SOCKET)cancel_fd, &rfds)){
		return NATIVE_SOCK_ABORTED;
	}
	return 1;
}

static int connect_with_timeout(int sock, native_sock_addr *addr, int addrlen, int timeout_ms, int *error, int cancel_fd){
	u_long ioctlopt = 1;
	ioctlsocket(sock, FIONBIO, &ioctlopt);

	int ret = 0;
	if (connect(sock, (struct sockaddr *)addr, addrlen) != 0){
		*error = WSAGetLastError();
		/* it was found out together with @hrydgard that after using WinHttp, it could also throw WSAEINVAL for WSAEALREADY */
		if (*error == WSAEISCONN){
			*error = 0;
		}else if (*error != WSAEWOULDBLOCK && *error != WSAEALREADY && *error != WSAEINVAL){
			ret = -1;
		}else{
			/* One wait for the outcome, a cancel, or the timeout. */
			int wait = native_wait(sock, true, timeout_ms, cancel_fd);
			if (wait == 0){
				*error = WSAETIMEDOUT;
				ret = -1;
			}else if (wait == NATIVE_SOCK_ABORTED){
				*error = WSAECANCELLED;
				ret = -1;
			}else if (wait < 0){
				*error = WSAGetLastError();
				ret = -1;
			}else{
				int so_error = 0;
				int so_len = sizeof(so_error);
				getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &so_len);
				*error = so_error;
				ret = so_error == 0 ? 0 : -1;
			}
		}
	}

	// just for completness, NBIO is used on the socket after connection anyway
	ioctlopt = 0;
	ioctlsocket(sock, FIONBIO, &ioctlopt);
	return ret;
}

int native_connect_tcp_sock(void *addr, int addrlen, int cancel_fd){
	init_winsock2();

	native_sock_addr *native_addr = addr;
	SOCKET win_sock = socket(native_addr->sin_family, SOCK_STREAM, 0);
	if (win_sock == INVALID_SOCKET){
		LOG("%s: failed creating socket, %d\n", __func__, WSAGetLastError());
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}
	int sock = win_sock;

	// XXX need to simulate timeout on windows, there's no sockopt for that
	// this also restricts latency to 500ms, if a server is even further away, connection won't be possible

	// Connect
	int error = 0;
	int connect_status = connect_with_timeout(sock, addr, addrlen, 5000, &error, cancel_fd);
	if (connect_status == -1){
		LOG("%s: failed connecting, %d\n", __func__, error);
		closesocket(sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// Set socket options
	int sockopt = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&sockopt, sizeof(sockopt));
	u_long ioctlopt = 1;
	ioctlsocket(sock, FIONBIO, &ioctlopt);

	sockopt = 2626560;
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&sockopt, sizeof(sockopt));
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&sockopt, sizeof(sockopt));

	// Show some socket options
	int opt_len = sizeof(sockopt);
	sockopt = 0;
	int get_ret = getsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&sockopt, &opt_len);
	LOG("%s: TCP_NODELAY is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? WSAGetLastError() : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&sockopt, &opt_len);
	LOG("%s: SO_SNDBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? WSAGetLastError() : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&sockopt, &opt_len);
	LOG("%s: SO_RCVBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? WSAGetLastError() : 0);

	return sock;
}

int native_send_till_done(int fd, const char *buf, int len, bool non_block, retro_atomic_int_t *abort, int cancel_fd){
	int write_offset = 0;
	while(write_offset != len){
		if (retro_atomic_load_acquire_int(abort)){
			return NATIVE_SOCK_ABORTED;
		}
		int write_status = send(fd, &buf[write_offset], len - write_offset, 0);
		if (write_status == -1){
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
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
			LOG("%s: failed sending, %d\n", __func__, err);
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
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		// Other errors
		LOG("%s: failed receving, %d\n", __func__, err);
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
	return closesocket(sock);
}

int native_shutdown_tcp_sock(int sock){
	return shutdown(sock, SD_BOTH);
}

int native_peek(int fd, char *buf, int len){
	int read_result = recv(fd, buf, len, MSG_PEEK);
	if (read_result == 0){
		return 0;
	}
	if (read_result == -1){
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		LOG("%s: failed peeking, %d\n", __func__, WSAGetLastError());
		return -1;
	}
	return read_result;
}

bool native_send_buf_not_full(int fd){
	WSAPOLLFD pfd;
	pfd.fd = fd;
	pfd.events = POLLWRNORM;
	pfd.revents = 0;
	WSAPoll(&pfd, 1, 0);
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
