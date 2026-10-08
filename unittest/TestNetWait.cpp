// Socket waits: a wake or a cancel ends one at once, data ends one, and a timeout still does.

#include "Common/Net/SocketCompat.h"

#include <thread>

#include "Common/Net/Cancel.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/Resolve.h"
#include "Common/TimeUtil.h"

#include "UnitTest.h"

static uintptr_t LoopbackUDP(sockaddr_in *addr) {
	uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(*addr);
	bind(s, (sockaddr *)addr, sizeof(*addr));
	getsockname(s, (sockaddr *)addr, &len);
	return s;
}

bool TestNetWait() {
	net::Init();

	// A wake ends a wait that has no socket and no timeout, whether it comes first or second.
	{
		net::WakeSocket wake;
		EXPECT_TRUE(wake.Fd() != -1);
		std::thread waker([&] { wake.Wake(); });
		EXPECT_EQ_INT((int)net::WaitSocketsOrWake(nullptr, nullptr, 0, false, -1.0, &wake), (int)net::WaitResult::CANCELLED);
		waker.join();
		wake.Drain();
		// Drained, so nothing is pending: a zero timeout times out.
		EXPECT_EQ_INT((int)net::WaitSocketsOrWake(nullptr, nullptr, 0, false, 0.0, &wake), (int)net::WaitResult::TIMEOUT);
	}

	// Data ends a wait; a quiet socket times out.
	{
		sockaddr_in addrA, addrB;
		uintptr_t a = LoopbackUDP(&addrA);
		uintptr_t b = LoopbackUDP(&addrB);
		EXPECT_EQ_INT((int)net::WaitSocket(a, false, 0.05, nullptr), (int)net::WaitResult::TIMEOUT);
		const char x = 'x';
		sendto(b, &x, 1, 0, (sockaddr *)&addrA, sizeof(addrA));
		EXPECT_EQ_INT((int)net::WaitSocket(a, false, 5.0, nullptr), (int)net::WaitResult::READY);
		closesocket(a);
		closesocket(b);
	}

	// A cancelled token ends every wait on it, before or during, and stays cancelled.
	{
		sockaddr_in addr;
		uintptr_t a = LoopbackUDP(&addr);
		net::CancelToken cancel;
		std::thread canceller([&] { cancel.Cancel(); });
		EXPECT_EQ_INT((int)net::WaitSocket(a, false, -1.0, &cancel), (int)net::WaitResult::CANCELLED);
		canceller.join();
		EXPECT_EQ_INT((int)net::WaitSocket(a, false, -1.0, &cancel), (int)net::WaitResult::CANCELLED);
		closesocket(a);
	}

	// A cancel ends a connect that would otherwise wait out its timeout (nothing answers here).
	{
		http::Client client(nullptr);
		if (client.Resolve("10.255.255.1", 9)) {
			net::CancelToken cancel;
			const double start = time_now_d();
			std::thread canceller([&] { cancel.Cancel(); });
			EXPECT_FALSE(client.Connect(1, 30.0, &cancel));
			canceller.join();
			EXPECT_TRUE(time_now_d() - start < 5.0);
		}
	}

	return true;
}
