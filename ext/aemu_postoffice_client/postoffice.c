#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "postoffice_client.h"

#include "log_impl.h"
#include "sock_impl.h"
#include "mutex_impl.h"
#include "postoffice_mem.h"

#include "aemu_postoffice_packets.h"

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

/* A close parks here until the operations in flight on its session are done. */
static retro_eventcount_t close_ec;

#define ABORTING(session) (retro_atomic_load_acquire_int(&(session)->abort) != 0)

static void op_end(retro_atomic_int_t *inflight, retro_atomic_int_t *abort){
	if (retro_atomic_fetch_sub_int(inflight, 1) == 1){
		/* Paired with the fence in session_close: either it sees us gone, or we see it waiting. */
		retro_atomic_thread_fence_seq_cst();
		if (retro_atomic_load_acquire_int(abort)){
			retro_eventcount_notify(&close_ec);
		}
	}
}

/* False if the session is closing; the operation must not touch it then. */
static bool op_begin(retro_atomic_int_t *inflight, retro_atomic_int_t *abort){
	retro_atomic_fetch_add_int(inflight, 1);
	retro_atomic_thread_fence_seq_cst();
	if (retro_atomic_load_acquire_int(abort)){
		op_end(inflight, abort);
		return false;
	}
	return true;
}

/* Stops new operations, wakes any that are blocked, and waits for those in flight. */
static void session_close(retro_atomic_int_t *inflight, retro_atomic_int_t *abort, int sock, bool dead){
	retro_atomic_store_release_int(abort, 1);
	retro_atomic_thread_fence_seq_cst();
	if (retro_atomic_load_acquire_int(inflight) == 0){
		return;
	}
	if (!dead){
		native_shutdown_tcp_sock(sock);
	}
	for (;;){
		int key = retro_eventcount_prepare_wait(&close_ec);
		if (retro_atomic_load_acquire_int(inflight) == 0){
			retro_eventcount_cancel_wait(&close_ec);
			return;
		}
		retro_eventcount_commit_wait(&close_ec, key);
	}
}

static void session_init_flags(retro_atomic_int_t *inflight, retro_atomic_int_t *abort){
	retro_atomic_int_init(inflight, 0);
	retro_atomic_int_init(abort, 0);
}

/* A blocking receive waiting for data. False if the session is closing or broke. */
static bool session_wait_readable(retro_atomic_int_t *inflight, retro_atomic_int_t *abort, int sock){
	if (!op_begin(inflight, abort)){
		return false;
	}
	int wait = native_wait(sock, false, -1, -1);
	op_end(inflight, abort);
	return wait > 0 && !retro_atomic_load_acquire_int(abort);
}

int aemu_post_office_init(){
	static bool first_run = true;
	if (first_run){
		first_run = false;
		init_postoffice_mem();

		for (int i = 0;i < NUM_PDP_SESSIONS;i++){
			pdp_sessions[i].sock = -1;
			retro_atomic_int_init(&pdp_sessions[i].claimed, 0);
		}
		for (int i = 0;i < NUM_PTP_LISTEN_SESSIONS;i++){
			ptp_listen_sessions[i].sock = -1;
			retro_atomic_int_init(&ptp_listen_sessions[i].claimed, 0);
		}
		for (int i = 0;i < NUM_PTP_SESSIONS;i++){
			ptp_sessions[i].sock = -1;
			retro_atomic_int_init(&ptp_sessions[i].claimed, 0);
		}

		init_drain_mutex();
		retro_eventcount_init(&close_ec);
	}else{
		// re-run, close all opened sessions
		for (int i = 0;i < NUM_PDP_SESSIONS;i++){
			if(pdp_sessions[i].sock != -1){
				pdp_delete(&pdp_sessions[i]);
			}
		}
		for (int i = 0;i < NUM_PTP_LISTEN_SESSIONS;i++){
			if(ptp_listen_sessions[i].sock != -1){
				ptp_listen_close(&ptp_listen_sessions[i]);
			}
		}
		for (int i = 0;i < NUM_PTP_SESSIONS;i++){
			if(ptp_sessions[i].sock != -1){
				ptp_close(&ptp_sessions[i]);
			}
		}
	}
	return 0;
}

static int create_and_init_socket(void *addr, int addrlen, const char *init_packet, int init_packet_len, const char *caller_name, int cancel_fd){
	int sock = native_connect_tcp_sock(addr, addrlen, cancel_fd);
	if (sock < 0){
		LOG("%s: tcp connection failed\n", caller_name);
		return sock;
	}

	retro_atomic_int_t abort;
	retro_atomic_int_init(&abort, 0);
	int write_status = native_send_till_done(sock, (char *)init_packet, init_packet_len, false, &abort, cancel_fd);
	if (write_status == -1){
		LOG("%s: failed sending init packet\n", caller_name);
		native_close_tcp_sock(sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	return sock;
}

static void *pdp_create(void *addr, int addrlen, const char *pdp_mac, int pdp_port, int *state){
	struct pdp_session* session = NULL;
	for(int i = 0;i < NUM_PDP_SESSIONS;i++){
		if (retro_atomic_cas_int(&pdp_sessions[i].claimed, 0, 1)){
			session = &pdp_sessions[i];
			session->sock = 0;
			break;
		}
	}
	if (session == NULL){
		LOG("%s: failed allocating memory for pdp session\n", __func__);
		*state = AEMU_POSTOFFICE_CLIENT_OUT_OF_MEMORY;
		return NULL;
	}

	// Prepare init packet
	struct aemu_postoffice_init init_packet = {0};
	init_packet.init_type = AEMU_POSTOFFICE_INIT_PDP;
	memcpy(init_packet.src_addr, pdp_mac, 6);
	init_packet.sport = pdp_port;

	int sock = create_and_init_socket(addr, addrlen, (char *)&init_packet, sizeof(init_packet), __func__, -1);

	if (sock < 0){
		*state = sock;
		session->sock = -1;
		retro_atomic_store_release_int(&session->claimed, 0);
		return NULL;
	}

	memcpy(session->pdp_mac, pdp_mac, 6);
	session->pdp_port = pdp_port;
	session->sock = sock;
	session->dead = false;
	session_init_flags(&session->inflight, &session->abort);
	session->recv_ring_buf_start = 0;
	session->recv_ring_buf_used = 0;
	session->buffered_data = 0;
	session->bytes_till_next_header = 0;
	session->last_block_size = 0;

	*state = AEMU_POSTOFFICE_CLIENT_OK;
	return session;
}

void *pdp_create_v6(const struct aemu_post_office_sock6_addr *addr, const char *pdp_mac, int pdp_port, int *state){
	native_sock6_addr native_addr;
	to_native_sock6_addr(&native_addr, addr);

	return pdp_create(&native_addr, sizeof(native_addr), pdp_mac, pdp_port, state);
}

void *pdp_create_v4(const struct aemu_post_office_sock_addr *addr, const char *pdp_mac, int pdp_port, int *state){
	native_sock_addr native_addr;
	to_native_sock_addr(&native_addr, addr);

	return pdp_create(&native_addr, sizeof(native_addr), pdp_mac, pdp_port, state);
}

int pdp_send(void *pdp_handle, const char *pdp_mac, int pdp_port, const char *buf, int len, bool non_block){
	if (pdp_handle == NULL){
		return -1;
	}
	struct pdp_session *session = (struct pdp_session *)pdp_handle;
	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (len > AEMU_POSTOFFICE_PDP_BLOCK_MAX){
		LOG("%s: failed sending data, data too big, %d\n", __func__, len);
		return AEMU_POSTOFFICE_CLIENT_OUT_OF_MEMORY;
	}

	// Write header
	struct aemu_postoffice_pdp pdp_header = {
		.port = pdp_port,
		.size = len
	};
	memcpy(pdp_header.addr, pdp_mac, 6);

	if (!op_begin(&session->inflight, &session->abort)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	int send_status = native_send_till_done(session->sock, (char *)&pdp_header, sizeof(pdp_header), non_block, &session->abort, -1);
	op_end(&session->inflight, &session->abort);
	if (send_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
	}
	if (send_status == NATIVE_SOCK_ABORTED){
		// getting aborted
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (send_status < 0){
		// Error
		LOG("%s: failed sending header\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (!op_begin(&session->inflight, &session->abort)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	send_status = native_send_till_done(session->sock, buf, len, false, &session->abort, -1);
	op_end(&session->inflight, &session->abort);
	if (send_status == NATIVE_SOCK_ABORTED){
		// getting aborted
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (send_status < 0){
		// Error
		LOG("%s: failed sending data\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	return AEMU_POSTOFFICE_CLIENT_OK;
}

static int peek_ring_buf(uint8_t *dst, int dst_size, const uint8_t *ring_buf, int ring_buf_size, int ring_buf_begin, int ring_buf_used){
	int to_peek = dst_size > ring_buf_used ? ring_buf_used : dst_size;
	for (int i = 0;i < to_peek;i++){
		int ring_buf_offset = (ring_buf_begin + i) % ring_buf_size;
		dst[i] = ring_buf[ring_buf_offset];
	}
	return to_peek;
}

static int consume_ring_buf(uint8_t *dst, int dst_size, const uint8_t *ring_buf, int ring_buf_size, int *ring_buf_begin, int *ring_buf_used){
	int peeked = peek_ring_buf(dst, dst_size, ring_buf, ring_buf_size, *ring_buf_begin, *ring_buf_used);
	*ring_buf_begin = (*ring_buf_begin + peeked) % ring_buf_size;
	*ring_buf_used = *ring_buf_used - peeked;
	return peeked;
}

static int pdp_drain_blocks_to_ring_buf(struct pdp_session *session){
	while (true){
		int ring_buf_free = sizeof(session->recv_ring_buf) - session->recv_ring_buf_used;
		if (session->bytes_till_next_header == 0){
			if (session->last_block_size != 0){
				session->buffered_data = session->buffered_data + session->last_block_size;
				session->last_block_size = 0;
			}
			if (ring_buf_free < sizeof(aemu_postoffice_pdp)){
				return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
			}

			aemu_postoffice_pdp header;
			int peek_len = native_peek(session->sock, (char *)&header, sizeof(header));
			if (peek_len == 0){
				LOG("%s: remote closed the socket\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (peek_len == -1){
				LOG("%s: failed peeking header\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (peek_len != sizeof(header)){
				return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
			}

			int recv_status = native_recv(session->sock, (char *)&header, sizeof(header));
			if (recv_status == 0){
				LOG("%s: remote closed the socket\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (recv_status == -1){
				LOG("%s: failed receiving header\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (header.size > AEMU_POSTOFFICE_PDP_BLOCK_MAX){
				LOG("%s: remote sent unexpected amount of data\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}

			session->bytes_till_next_header = header.size;
			session->last_block_size = header.size;
			uint8_t *header_bytes = (uint8_t *)&header;
			int ring_buf_end = (session->recv_ring_buf_start + session->recv_ring_buf_used) % sizeof(session->recv_ring_buf);
			for (int i = 0;i < sizeof(header);i++){
				int ring_buf_offset = (ring_buf_end + i) % sizeof(session->recv_ring_buf);
				session->recv_ring_buf[ring_buf_offset] = header_bytes[i];
			}
			session->recv_ring_buf_used = session->recv_ring_buf_used + sizeof(header);
			continue;
		}

		int ring_buf_end = (session->recv_ring_buf_start + session->recv_ring_buf_used) % sizeof(session->recv_ring_buf);
		int linear_size_from_end = sizeof(session->recv_ring_buf) - ring_buf_end;
		int to_consume = ring_buf_free;
		if (to_consume > linear_size_from_end){
			to_consume = linear_size_from_end;
		}
		if (to_consume > session->bytes_till_next_header){
			to_consume = session->bytes_till_next_header;
		}

		if (to_consume == 0){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}

		int recv_status = native_recv(session->sock, &session->recv_ring_buf[ring_buf_end], to_consume);
		if (recv_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		if (recv_status == 0){
			LOG("%s: remote closed the socket\n", __func__);
			native_close_tcp_sock(session->sock);
			session->dead = true;
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		if (recv_status == -1){
			LOG("%s: failed receiving data\n", __func__);
			native_close_tcp_sock(session->sock);
			session->dead = true;
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		session->recv_ring_buf_used = session->recv_ring_buf_used + recv_status;
		session->bytes_till_next_header = session->bytes_till_next_header - recv_status;
	}
}

static int pdp_drain_blocks_to_ring_buf_locked(struct pdp_session *session){
	if (!op_begin(&session->inflight, &session->abort)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	lock_drain_mutex();
	int result = pdp_drain_blocks_to_ring_buf(session);
	unlock_drain_mutex();
	op_end(&session->inflight, &session->abort);
	return result;
}

int pdp_recv(void *pdp_handle, char *pdp_mac, int *pdp_port, char *buf, int *len, bool non_block){
	if (pdp_handle == NULL){
		return -1;
	}
	struct pdp_session *session = (struct pdp_session *)pdp_handle;
	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	while (true){
		if (ABORTING(session)){
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		int drain_result = pdp_drain_blocks_to_ring_buf_locked(session);
		if (drain_result == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD){
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		// AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK

		aemu_postoffice_pdp header;
		int peeked = peek_ring_buf((uint8_t *)&header, sizeof(header), (uint8_t *)session->recv_ring_buf, sizeof(session->recv_ring_buf), session->recv_ring_buf_start, session->recv_ring_buf_used);
		if (peeked != sizeof(header)){
			if (non_block){
				return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
			}else{
				/* Block until more arrives, or a close wakes us. */
				if (!session_wait_readable(&session->inflight, &session->abort, session->sock)){
					return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
				}
				continue;
			}
		}

		int total_size = sizeof(header) + header.size;
		if (total_size > session->recv_ring_buf_used){
			if (non_block){
				return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
			}else{
				/* Block until more arrives, or a close wakes us. */
				if (!session_wait_readable(&session->inflight, &session->abort, session->sock)){
					return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
				}
				continue;
			}
		}

		break;
	}

	aemu_postoffice_pdp header;
	peek_ring_buf((uint8_t *)&header, sizeof(header), (uint8_t *)session->recv_ring_buf, sizeof(session->recv_ring_buf), session->recv_ring_buf_start, session->recv_ring_buf_used);
	*pdp_port = header.port;
	memcpy(pdp_mac, header.addr, 6);
	if (header.size > *len){
		*len = header.size;
		return AEMU_POSTOFFICE_CLIENT_SESSION_DATA_TRUNC;
	}
	consume_ring_buf((uint8_t *)&header, sizeof(header), (uint8_t *)session->recv_ring_buf, sizeof(session->recv_ring_buf), &session->recv_ring_buf_start, &session->recv_ring_buf_used);
	consume_ring_buf((uint8_t *)buf, header.size, (uint8_t *)session->recv_ring_buf, sizeof(session->recv_ring_buf), &session->recv_ring_buf_start, &session->recv_ring_buf_used);
	session->buffered_data = session->buffered_data - header.size;
	*len = header.size;

	return AEMU_POSTOFFICE_CLIENT_OK;
}

void pdp_delete(void *pdp_handle){
	if (pdp_handle == NULL){
		return;
	}
	struct pdp_session *session = (struct pdp_session *)pdp_handle;

	/* abort on-going ops, and wait for the ones in flight */
	session_close(&session->inflight, &session->abort, session->sock, session->dead);

	if (!session->dead)
		native_close_tcp_sock(session->sock);
	session->sock = -1;
	retro_atomic_store_release_int(&session->claimed, 0);
}

int pdp_peek_next_size(void *pdp_handle){
	struct pdp_session *session = pdp_handle;

	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	int drain_result = pdp_drain_blocks_to_ring_buf_locked(session);
	if (drain_result == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (session->recv_ring_buf_used < sizeof(aemu_postoffice_pdp)){
		return 0;
	}

	aemu_postoffice_pdp header;
	peek_ring_buf((uint8_t *)&header, sizeof(header), (uint8_t *)session->recv_ring_buf, sizeof(session->recv_ring_buf), session->recv_ring_buf_start, session->recv_ring_buf_used);
	if (session->buffered_data >= header.size){
		return header.size;
	}
	return 0;
}

int pdp_buffered_data_size(void *pdp_handle){
	struct pdp_session *session = pdp_handle;

	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	int drain_result = pdp_drain_blocks_to_ring_buf_locked(session);
	if (drain_result == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	return session->buffered_data;
}

int pdp_is_dead(void *pdp_handle){
	if (pdp_handle == NULL){
		return 1;
	}

	struct pdp_session *session = (struct pdp_session *)pdp_handle;
	if (session->dead || ABORTING(session)){
		return 1;
	}

	if (native_hung_up(session->sock)){
		LOG("%s: the other side closed the listen socket\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return 1;
	}

	return 0;
}

int pdp_send_buf_not_full(void *pdp_handle){
	if (pdp_is_dead(pdp_handle)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	struct pdp_session *session = pdp_handle;

	return native_send_buf_not_full(session->sock);
}

static void *ptp_listen(void *addr, int addrlen, const char *ptp_mac, int ptp_port, int *state){
	struct ptp_listen_session* session = NULL;
	for(int i = 0;i < NUM_PTP_LISTEN_SESSIONS;i++){
		if (retro_atomic_cas_int(&ptp_listen_sessions[i].claimed, 0, 1)){
			session = &ptp_listen_sessions[i];
			session->sock = 0;
			break;
		}
	}
	if (session == NULL){
		LOG("%s: failed allocating memory for ptp listen session\n", __func__);
		*state = AEMU_POSTOFFICE_CLIENT_OUT_OF_MEMORY;
		return NULL;
	}

	// Prepare init packet
	struct aemu_postoffice_init init_packet = {0};
	init_packet.init_type = AEMU_POSTOFFICE_INIT_PTP_LISTEN;
	memcpy(init_packet.src_addr, ptp_mac, 6);
	init_packet.sport = ptp_port;

	int sock = create_and_init_socket(addr, addrlen, (char *)&init_packet, sizeof(init_packet), __func__, -1);

	if (sock < 0){
		*state = sock;
		session->sock = -1;
		retro_atomic_store_release_int(&session->claimed, 0);
		return NULL;
	}

	memcpy(session->ptp_mac, ptp_mac, 6);
	session->ptp_port = ptp_port;
	session->sock = sock;
	memcpy(session->addr, addr, addrlen);
	session->addrlen = addrlen;
	session->dead = false;
	session_init_flags(&session->inflight, &session->abort);

	*state = AEMU_POSTOFFICE_CLIENT_OK;
	return session;
}

void *ptp_listen_v6(const struct aemu_post_office_sock6_addr *addr, const char *ptp_mac, int ptp_port, int *state){
	native_sock6_addr native_addr;
	to_native_sock6_addr(&native_addr, addr);

	return ptp_listen(&native_addr, sizeof(native_addr), ptp_mac, ptp_port, state);
}

void *ptp_listen_v4(const struct aemu_post_office_sock_addr *addr, const char *ptp_mac, int ptp_port, int *state){
	native_sock_addr native_addr;
	to_native_sock_addr(&native_addr, addr);

	return ptp_listen(&native_addr, sizeof(native_addr), ptp_mac, ptp_port, state);
}

void *ptp_accept(void *ptp_listen_handle, char *ptp_mac, int *ptp_port, bool nonblock, int *state){
	if (ptp_listen_handle == NULL){
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		return NULL;
	}

	struct ptp_listen_session *session = (struct ptp_listen_session *)ptp_listen_handle;
	if (session->dead){
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		return NULL;
	}

	struct aemu_postoffice_ptp_connect connect_packet;
	if (!op_begin(&session->inflight, &session->abort)){
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		return NULL;
	}
	int recv_status = native_recv_till_done(session->sock, (char *)&connect_packet, sizeof(connect_packet), nonblock, &session->abort, -1);
	op_end(&session->inflight, &session->abort);
	if (recv_status == NATIVE_SOCK_ABORTED){
		// getting aborted
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		return NULL;
	}
	if (recv_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		return NULL;
	}
	if (recv_status == 0){
		LOG("%s: the other side closed the listen socket\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		return NULL;
	}
	if (recv_status <= 0){
		LOG("%s: socket error, %d\n", __func__, recv_status);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		return NULL;
	}

	// Allocate memory
	struct ptp_session *new_session = NULL;
	for(int i = 0;i < NUM_PTP_SESSIONS;i++){
		if (retro_atomic_cas_int(&ptp_sessions[i].claimed, 0, 1)){
			new_session = &ptp_sessions[i];
			new_session->sock = 0;
			break;
		}
	}
	if (new_session == NULL){
		*state = AEMU_POSTOFFICE_CLIENT_OUT_OF_MEMORY;
		return NULL;
	}

	// Prepare init packet
	struct aemu_postoffice_init init_packet;
	init_packet.init_type = AEMU_POSTOFFICE_INIT_PTP_ACCEPT;
	memcpy(init_packet.src_addr, session->ptp_mac, 6);
	init_packet.sport = session->ptp_port;
	memcpy(init_packet.dst_addr, connect_packet.addr, 6);
	init_packet.dport = connect_packet.port;

	int sock = create_and_init_socket(session->addr, session->addrlen, (char *)&init_packet, sizeof(init_packet), __func__, -1);

	if (sock < 0){
		*state = sock;
		new_session->sock = -1;
		retro_atomic_store_release_int(&new_session->claimed, 0);
		return NULL;
	}

	// Consume the ack packet
	retro_atomic_int_t abort;
	retro_atomic_int_init(&abort, 0);
	int read_status = native_recv_till_done(sock, (char *)&connect_packet, sizeof(connect_packet), false, &abort, -1);
	if (read_status == 0){
		LOG("%s: remote closed the socket during initial recv\n", __func__);
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
		native_close_tcp_sock(sock);
		new_session->sock = -1;
		retro_atomic_store_release_int(&new_session->claimed, 0);
		return NULL;
	}
	if (read_status == -1){
		LOG("%s: socket error receiving initial packet\n", __func__);
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
		native_close_tcp_sock(sock);
		new_session->sock = -1;
		retro_atomic_store_release_int(&new_session->claimed, 0);
		return NULL;
	}

	// Now the session is ready
	new_session->sock = sock;
	new_session->dead = false;
	session_init_flags(&new_session->inflight, &new_session->abort);
	new_session->recv_ring_buf_start = 0;
	new_session->recv_ring_buf_used = 0;
	new_session->bytes_till_next_header = 0;
	*state = AEMU_POSTOFFICE_CLIENT_OK;
	*ptp_port = connect_packet.port;
	memcpy(ptp_mac, connect_packet.addr, 6);
	return new_session;
}

static void *ptp_connect(void *addr, int addrlen, const char *src_ptp_mac, int ptp_sport, const char *dst_ptp_mac, int ptp_dport, int *state, int cancel_fd){
	// Allocate memory
	struct ptp_session *new_session = NULL;
	for(int i = 0;i < NUM_PTP_SESSIONS;i++){
		if (retro_atomic_cas_int(&ptp_sessions[i].claimed, 0, 1)){
			new_session = &ptp_sessions[i];
			new_session->sock = 0;
			break;
		}
	}
	if (new_session == NULL){
		*state = AEMU_POSTOFFICE_CLIENT_OUT_OF_MEMORY;
		return NULL;
	}

	// Prepare init packet
	struct aemu_postoffice_init init_packet;
	init_packet.init_type = AEMU_POSTOFFICE_INIT_PTP_CONNECT;
	memcpy(init_packet.src_addr, src_ptp_mac, 6);
	init_packet.sport = ptp_sport;
	memcpy(init_packet.dst_addr, dst_ptp_mac, 6);
	init_packet.dport = ptp_dport;

	int sock = create_and_init_socket(addr, addrlen, (char *)&init_packet, sizeof(init_packet), __func__, cancel_fd);

	if (sock < 0){
		*state = sock;
		new_session->sock = -1;
		retro_atomic_store_release_int(&new_session->claimed, 0);
		return NULL;
	}

	// Consume the ack packet
	struct aemu_postoffice_ptp_connect connect_packet;
	retro_atomic_int_t abort;
	retro_atomic_int_init(&abort, 0);
	int read_status = native_recv_till_done(sock, (char *)&connect_packet, sizeof(connect_packet), false, &abort, cancel_fd);
	if (read_status == 0){
		LOG("%s: remote closed the socket during initial recv\n", __func__);
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
		native_close_tcp_sock(sock);
		new_session->sock = -1;
		retro_atomic_store_release_int(&new_session->claimed, 0);
		return NULL;
	}
	if (read_status == -1){
		LOG("%s: socket error receiving initial packet\n", __func__);
		*state = AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
		native_close_tcp_sock(sock);
		new_session->sock = -1;
		retro_atomic_store_release_int(&new_session->claimed, 0);
		return NULL;
	}

	// Now the session is ready
	new_session->sock = sock;
	new_session->dead = false;
	session_init_flags(&new_session->inflight, &new_session->abort);
	new_session->recv_ring_buf_start = 0;
	new_session->recv_ring_buf_used = 0;
	new_session->bytes_till_next_header = 0;
	*state = AEMU_POSTOFFICE_CLIENT_OK;
	return new_session;
}

void *ptp_connect_v6(const struct aemu_post_office_sock6_addr *addr, const char *src_ptp_mac, int ptp_sport, const char *dst_ptp_mac, int ptp_dport, int *state, int cancel_fd){
	native_sock6_addr native_addr;
	to_native_sock6_addr(&native_addr, addr);

	return ptp_connect(&native_addr, sizeof(native_addr), src_ptp_mac, ptp_sport, dst_ptp_mac, ptp_dport, state, cancel_fd);
}

void *ptp_connect_v4(const struct aemu_post_office_sock_addr *addr, const char *src_ptp_mac, int ptp_sport, const char *dst_ptp_mac, int ptp_dport, int *state, int cancel_fd){
	native_sock_addr native_addr;
	to_native_sock_addr(&native_addr, addr);

	return ptp_connect(&native_addr, sizeof(native_addr), src_ptp_mac, ptp_sport, dst_ptp_mac, ptp_dport, state, cancel_fd);
}

int ptp_send(void *ptp_handle, const char *buf, int len, bool non_block){
	if (ptp_handle == NULL){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	struct ptp_session *session = (struct ptp_session *)ptp_handle;
	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (len > AEMU_POSTOFFICE_PTP_BLOCK_MAX){
		LOG("%s: failed sending data, data too big, %d\n", __func__, len);
		return AEMU_POSTOFFICE_CLIENT_OUT_OF_MEMORY;
	}

	struct aemu_postoffice_ptp_data header = {
		.size = len
	};

	if (!op_begin(&session->inflight, &session->abort)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	int send_status = native_send_till_done(session->sock, (char *)&header, sizeof(header), non_block, &session->abort, -1);
	op_end(&session->inflight, &session->abort);
	if (send_status == NATIVE_SOCK_ABORTED){
		// getting aborted
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	if (send_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
	}

	if (send_status < 0){
		LOG("%s: failed sending header\n", __func__);
		native_close_tcp_sock(session->sock);
		session->dead = true;
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (!op_begin(&session->inflight, &session->abort)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	send_status = native_send_till_done(session->sock, buf, len, false, &session->abort, -1);
	op_end(&session->inflight, &session->abort);
	if (send_status == NATIVE_SOCK_ABORTED){
		// getting aborted
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	if (send_status < 0){
		LOG("%s: failed sending data\n", __func__);
		native_close_tcp_sock(session->sock);
		session->dead = true;
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	return AEMU_POSTOFFICE_CLIENT_OK;
}

static int ptp_drain_blocks_to_ring_buf(struct ptp_session *session){
	while(true){
		if (session->bytes_till_next_header == 0){
			struct aemu_postoffice_ptp_data header = {0};
			int peek_result = native_peek(session->sock, (char *)&header, sizeof(header));
			if (peek_result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
				return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
			}
			if (peek_result == 0){
				LOG("%s: remote closed the socket\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (peek_result == -1){
				LOG("%s: failed peeking header\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (peek_result != sizeof(header)){
				return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
			}
			int recv_status = native_recv(session->sock, (char *)&header, sizeof(header));
			if (recv_status == 0){
				LOG("%s: remote closed the socket\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}
			if (recv_status == -1){
				LOG("%s: failed reading header\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}

			if (header.size > AEMU_POSTOFFICE_PTP_BLOCK_MAX){
				LOG("%s: remote sent unexpected amount of data\n", __func__);
				native_close_tcp_sock(session->sock);
				session->dead = true;
				return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
			}

			session->bytes_till_next_header = header.size;
		}

		int ring_buf_free = sizeof(session->recv_ring_buf) - session->recv_ring_buf_used;
		int ring_buf_end = (session->recv_ring_buf_start + session->recv_ring_buf_used) % sizeof(session->recv_ring_buf);
		int linear_size_from_end = sizeof(session->recv_ring_buf) - ring_buf_end;
		int to_append = ring_buf_free;
		if (to_append > session->bytes_till_next_header){
			to_append = session->bytes_till_next_header;
		}
		if (to_append > linear_size_from_end){
			to_append = linear_size_from_end;
		}
		if (to_append == 0){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}

		int recv_status = native_recv(session->sock, &session->recv_ring_buf[ring_buf_end], to_append);
		if (recv_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		if (recv_status == 0){
			LOG("%s: remote closed the socket\n", __func__);
			native_close_tcp_sock(session->sock);
			session->dead = true;
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		if (recv_status == -1){
			LOG("%s: failed reading data\n", __func__);
			native_close_tcp_sock(session->sock);
			session->dead = true;
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		session->recv_ring_buf_used = session->recv_ring_buf_used + recv_status;
		session->bytes_till_next_header = session->bytes_till_next_header - recv_status;
	}
}

static int ptp_drain_blocks_to_ring_buf_locked(struct ptp_session *session){
	if (!op_begin(&session->inflight, &session->abort)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	lock_drain_mutex();
	int result = ptp_drain_blocks_to_ring_buf(session);
	unlock_drain_mutex();
	op_end(&session->inflight, &session->abort);
	return result;
}

int ptp_recv(void *ptp_handle, char *buf, int *len, bool non_block){
	if (ptp_handle == NULL){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	struct ptp_session *session = (struct ptp_session *)ptp_handle;
	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	while(true){
		if (ABORTING(session)){
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		int drain_result = ptp_drain_blocks_to_ring_buf_locked(session);
		if (drain_result == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD){
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
		if (drain_result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
			if (non_block){
				break;
			}
			if (session->recv_ring_buf_used != 0){
				break;
			}
		}
		/* Block until more arrives, or a close wakes us. */
		if (!session_wait_readable(&session->inflight, &session->abort, session->sock)){
			return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
		}
	}

	if (session->recv_ring_buf_used == 0){
		return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
	}

	int ring_buffer_consumed = consume_ring_buf((uint8_t *)buf, *len, (uint8_t *)session->recv_ring_buf, sizeof(session->recv_ring_buf), &session->recv_ring_buf_start, &session->recv_ring_buf_used);
	*len = ring_buffer_consumed;

	return AEMU_POSTOFFICE_CLIENT_OK;
}

void ptp_close(void *ptp_handle){
	if (ptp_handle == NULL){
		return;
	}

	struct ptp_session *session = (struct ptp_session *)ptp_handle;

	/* abort on-going ops, and wait for the ones in flight */
	session_close(&session->inflight, &session->abort, session->sock, session->dead);

	if (!session->dead)
		native_close_tcp_sock(session->sock);
	session->sock = -1;
	retro_atomic_store_release_int(&session->claimed, 0);
}

void ptp_listen_close(void *ptp_listen_handle){
	if (ptp_listen_handle == NULL){
		return;
	}

	struct ptp_listen_session *session = (struct ptp_listen_session *)ptp_listen_handle;

	/* abort on-going ops, and wait for the ones in flight */
	session_close(&session->inflight, &session->abort, session->sock, session->dead);

	if (!session->dead)
		native_close_tcp_sock(session->sock);
	session->sock = -1;
	retro_atomic_store_release_int(&session->claimed, 0);
}

int pdp_get_native_sock(void *pdp_handle){
	struct pdp_session *session = pdp_handle;
	return session->sock;
}

int ptp_get_native_sock(void *ptp_handle){
	struct ptp_session *session = ptp_handle;
	return session->sock;
}

int ptp_listen_get_native_sock(void *ptp_listen_handle){
	struct ptp_listen_session *session = ptp_listen_handle;
	return session->sock;
}

int ptp_peek_next_size(void *ptp_handle){
	struct ptp_session *session = ptp_handle;

	if (session->dead || ABORTING(session)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	int drain_result = ptp_drain_blocks_to_ring_buf_locked(session);
	if (drain_result == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	// AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK

	return session->recv_ring_buf_used;
}

int ptp_is_dead(void *ptp_handle){
	if (ptp_handle == NULL){
		return 1;
	}

	struct ptp_session *session = (struct ptp_session *)ptp_handle;
	if (session->dead || ABORTING(session)){
		return 1;
	}

	if (native_hung_up(session->sock)){
		LOG("%s: the other side closed the listen socket\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return 1;
	}

	return 0;
}

int ptp_send_buf_not_full(void *ptp_handle){
	if (ptp_is_dead(ptp_handle)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	struct ptp_session *session = ptp_handle;

	return native_send_buf_not_full(session->sock);
}

int ptp_listen_is_dead(void *ptp_listen_handle){
	if (ptp_listen_handle == NULL){
		return 1;
	}

	struct ptp_listen_session *session = (struct ptp_listen_session *)ptp_listen_handle;
	if (session->dead){
		return 1;
	}

	if (native_hung_up(session->sock)){
		LOG("%s: the other side closed the listen socket\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return 1;
	}

	return 0;
}

int ptp_listen_has_request(void *ptp_listen_handle){
	if (ptp_listen_handle == NULL){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	if (ptp_listen_is_dead(ptp_listen_handle)){
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}

	struct ptp_listen_session *session = (struct ptp_listen_session *)ptp_listen_handle;

	struct aemu_postoffice_ptp_connect connect_packet;
	int peek_result = native_peek(session->sock, (char *)&connect_packet, sizeof(connect_packet));
	if (peek_result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		return 0;
	}
	if (peek_result == 0){
		LOG("%s: the other side closed the listen socket\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	if (peek_result <= 0){
		LOG("%s: socket error\n", __func__);
		session->dead = true;
		native_close_tcp_sock(session->sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_DEAD;
	}
	if (peek_result != sizeof(connect_packet)){
		return 0;
	}
	return 1;
}
