#include "ppsspp_config.h"

// First, so Windows gets its socket error codes instead of the CRT's.
#include "Common/Net/SocketCompat.h"

#include <cmath>

#include "Common/Net/Cancel.h"
#include "Common/File/FileDescriptor.h"
#include "Common/TimeUtil.h"

namespace net {

WakeSocket::WakeSocket() {
	intptr_t s = (intptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == (intptr_t)INVALID_SOCKET)
		return;
#if !PPSSPP_PLATFORM(WINDOWS)
	if (s >= FD_SETSIZE) {
		closesocket(s);
		return;
	}
#endif
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	socklen_t len = sizeof(addr);
	if (bind(s, (sockaddr *)&addr, sizeof(addr)) != 0 ||
		getsockname(s, (sockaddr *)&addr, &len) != 0 ||
		connect(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
		closesocket(s);
		return;
	}
	fd_util::SetNonBlocking((int)s, true);
	fd_ = s;
}

WakeSocket::~WakeSocket() {
	if (fd_ != -1)
		closesocket(fd_);
}

void WakeSocket::Wake() {
	if (fd_ != -1) {
		// Non-blocking: with the buffer full, a wake is already pending.
		const char b = 1;
		send(fd_, &b, 1, MSG_NOSIGNAL);
	}
}

void WakeSocket::Drain() {
	if (fd_ != -1) {
		char buf[64];
		while (recv(fd_, buf, sizeof(buf), 0) > 0) {
		}
	}
}

void CancelToken::Cancel() {
	cancelled_.store(true, std::memory_order_release);
	// Never drained, so the socket stays readable from here on.
	wake_.Wake();
}

static WaitResult WaitInternal(const uintptr_t *socks, bool *ready, int count, bool forWrite, double timeout, intptr_t wake, const CancelToken *cancel) {
	const double deadline = timeout >= 0.0 ? time_now_d() + timeout : 0.0;
	for (;;) {
		if (cancel && cancel->IsCancelled())
			return WaitResult::CANCELLED;

		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		intptr_t maxfd = -1;
		for (int i = 0; i < count; i++) {
			if ((intptr_t)socks[i] == -1)
				continue;
			FD_SET((SOCKET)socks[i], forWrite ? &wfds : &rfds);
			if ((intptr_t)socks[i] > maxfd)
				maxfd = (intptr_t)socks[i];
		}
		if (wake != -1) {
			FD_SET((SOCKET)wake, &rfds);
			if (wake > maxfd)
				maxfd = wake;
		}

		timeval tv{};
		timeval *ptv = nullptr;
		if (timeout >= 0.0) {
			double left = deadline - time_now_d();
			if (left < 0.0)
				left = 0.0;
			tv.tv_sec = (long)floor(left);
			tv.tv_usec = (long)((left - floor(left)) * 1000000.0);
			ptv = &tv;
		}
		if (maxfd == -1) {
			// Only the timeout could end this, and that would be a sleep.
			return WaitResult::FAILED;
		}

		const int r = select((int)maxfd + 1, &rfds, forWrite ? &wfds : nullptr, nullptr, ptv);
		if (r < 0) {
			if (socket_errno == EINTR)
				continue;
			return WaitResult::FAILED;
		}
		if (cancel && cancel->IsCancelled())
			return WaitResult::CANCELLED;
		if (r == 0)
			return WaitResult::TIMEOUT;

		bool any = false;
		for (int i = 0; i < count; i++) {
			const bool set = (intptr_t)socks[i] != -1 && FD_ISSET((SOCKET)socks[i], forWrite ? &wfds : &rfds) != 0;
			if (ready)
				ready[i] = set;
			any = any || set;
		}
		if (any)
			return WaitResult::READY;
		if (!cancel && wake != -1 && FD_ISSET((SOCKET)wake, &rfds))
			return WaitResult::CANCELLED;
	}
}

WaitResult WaitSockets(const uintptr_t *socks, bool *ready, int count, bool forWrite, double timeout, const CancelToken *cancel) {
	return WaitInternal(socks, ready, count, forWrite, timeout, cancel ? cancel->WakeFd() : -1, cancel);
}

WaitResult WaitSocket(uintptr_t sock, bool forWrite, double timeout, const CancelToken *cancel) {
	return WaitSockets(&sock, nullptr, 1, forWrite, timeout, cancel);
}

WaitResult WaitSocketsOrWake(const uintptr_t *socks, bool *ready, int count, bool forWrite, double timeout, WakeSocket *wake) {
	return WaitInternal(socks, ready, count, forWrite, timeout, wake ? wake->Fd() : -1, nullptr);
}

}  // namespace net
