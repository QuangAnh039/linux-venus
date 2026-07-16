/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CA_IPC_PE_H__
#define __CA_IPC_PE_H__

enum ca_ipc_session_id {
	CA_IPC_SESSION_VPN = 1,
	CA_IPC_SESSION_WFO = 2,
	CA_IPC_SESSION_PKT_SHAPER = 3,
	CA_IPC_SESSION_SW_RATE_LIMITER = 4,
	CA_IPC_SESSION_MACSEC = 5,
	CA_IPC_SESSION_WMC_FLOODING = 6,
	CA_IPC_SESSION_FDB = 7,
	CA_IPC_SESSION_SYSTEM = 8,
	CA_IPC_SESSION_HTTP_TEST = 9,
	CA_IPC_SESSION_NET = 10,
	CA_IPC_SESSION_MAX = 10,
};

#if defined(CONFIG_ARCH_CA_MERCURY)
enum ca_ipc_cpu_id {
	CA_IPC_CPU_ARM = 0,
	CA_IPC_CPU_PE0 = 1,
	CA_IPC_CPU_PE1 = 2,
	CA_IPC_CPU_PE2 = 3,
	CA_IPC_CPU_PE3 = 4,
	CA_IPC_CPU_MAX,
};
#else
enum ca_ipc_cpu_id {
	CA_IPC_CPU_ARM = 0,
	CA_IPC_CPU_PE0 = 1,
	CA_IPC_CPU_PE1 = 2,
	CA_IPC_CPU_MAX = 2,
};
#endif

enum ca_ipc_priority {
	CA_IPC_PRIO_LOW = 0,
	CA_IPC_PRIO_HIGH = 1,
};

struct ca_ipc_addr {
	enum ca_ipc_session_id	session_id;
	enum ca_ipc_cpu_id		cpu_id;
} __packed;

typedef int (*ca_ipc_msg_proc_t) (struct ca_ipc_addr peer, unsigned short msg_no,
				  unsigned short trans_id, const void *msg_data,
				  unsigned short *msg_size);

struct ca_ipc_msg_handle {
	unsigned short msg_no;
	/* 'proc' is callback function pointer, it must be 32-bit aligned. */
	unsigned short reserved;
	ca_ipc_msg_proc_t proc;
} __packed;

struct ca_ipc_pkt {
	enum ca_ipc_session_id session_id; /* the session_id */
	enum ca_ipc_cpu_id dst_cpu_id; /* the destination cpu_id */
	enum ca_ipc_priority priority; /* the priority of message*/
	/* the message number within the session.
	 * This is customized message number.
	 * The destination CPU will be based on this number
	 * to look for the message handler.
	 */
	unsigned short msg_no;

	const void *msg_data; /* the message data to transmit. */
	unsigned short msg_size; /* the length of message data. */
}; //ipc_send_data_t

int ca_ipc_msg_handle_register(enum ca_ipc_session_id session_id,
			       const struct ca_ipc_msg_handle *msg_handle_array,
			       unsigned int msg_handle_count);
int ca_ipc_msg_handle_unregister(enum ca_ipc_session_id session_id);
int ca_ipc_msg_async_send(struct ca_ipc_pkt *p_ipc_pkt);
int ca_ipc_msg_sync_send(struct ca_ipc_pkt *p_ipc_pkt, void *result_data,
			 unsigned short *result_size);
int ca_ipc_msg_sync_v2_send(struct ca_ipc_pkt *p_ipc_pkt, void *result_data,
			    unsigned short *result_size);

#endif /* __CA_IPC_PE_H__ */
