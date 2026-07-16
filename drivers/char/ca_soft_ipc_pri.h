/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CA_IPC_PRI_H__
#define __CA_IPC_PRI_H__

#include <soc/cortina-access/ca_ipc_pe.h>
#define CA_IPC_VERSION		0x0200

/* IPC return code */
#define	CA_IPC_OK		0
#define	CA_IPC_ETIMEOUT		0x5
#define	CA_IPC_EINVAL		0x13
#define	CA_IPC_EEXIST		0xe
#define	CA_IPC_ENOMEM		0xa
#define	CA_IPC_ENOCLIENT	0x3
#define	CA_IPC_EQFULL		0xc
#define	CA_IPC_EINTERNAL	0x6

/* Mailbox action_type */
#define MB_TYPE_NONE		0x0  //none
#define MB_TYPE_RQ		0x1  //request
#define MB_TYPE_READY		0x2  //ready
#define MB_TYPE_AS		0x3  //async send
#define MB_TYPE_SS		0x4  //sync send
#define MB_TYPE_ACK		0x5  //Ack
#define MB_TYPE_DONE		0x6
#define MB_TYPE_ERROR		0xFF //error

/* message staus */
#define MESSAGE_STATUS_FREE     0x0
#define MESSAGE_STATUS_PREPARED  0x1
#define MESSAGE_STATUS_WAIT     0x2
#define MESSAGE_STATUS_BUSY     0x3

/* Message Priority */
#define CA_IPC_LPRIO		0x0
#define CA_IPC_HPRIO		0x1

/* CPU_ID */
#define CPU_ARM			0x0
#define CPU_RCPU0		0x1
#define CPU_RCPU1		0x2
#if defined(CONFIG_ARCH_CA_MERCURY)
#define CPU_RCPU2		0x3
#define CPU_RCPU3		0x4
#endif

/* Client ID */
#define CA_CLNT_ID_WFO_ARM	0x0
#define CA_CLNT_ID_WFO_PE0	0x1
#define CA_CLNT_ID_WFO_PE1	0x2
#define CA_CLNT_ID_TUNNEL_PE0	0x3
#define CA_CLNT_ID_TUNNEL_PE1	0x4

#if defined(CONFIG_ARCH_CA_MERCURY)
#define IPC_CPU_NUMBER 5
#define IPC_LIST_SIZE 0x20400
#define IPC_ITEM_SIZE 0x200
#define IPC_DEFAULT_TIMEOUT (5 * HZ)
#else
#define IPC_CPU_NUMBER 3
#define IPC_LIST_SIZE 0x2000
#define IPC_ITEM_SIZE 0x80
#define IPC_DEFAULT_TIMEOUT (3 * HZ)
#endif

#ifndef IPC_CPU_NUMBER
#error "CA IPC CPU number is not defined"
#endif

#ifndef IPC_LIST_SIZE
#error "CA IPC list size is not defined"
#endif

#ifndef IPC_ITEM_SIZE
#error "CA IPC item size in list is not defined"
#endif

#define CA_IPC_USED_MSG	0x00
#define	CA_IPC_ASYN_MSG	0x01
#define CA_IPC_SYNC_MSG	0x02
#define CA_IPC_ACK_MSG	0x03
#define CA_IPC_SYNC_V2_MSG	0x00

#if IPC_LIST_SIZE > 665536
#error "Modify data type in fifl_info, msg_header, and others related"
#endif

#if defined(CONFIG_ARCH_CA_MERCURY)
#define PER_IRQ_SOFT0_OFFSET 0x34
#define PER_IRQ_SOFT1_OFFSET 0x30
#define PER_IRQ_SOFT2_OFFSET 0x2c
#define PER_IRQ_SOFT3_OFFSET 0x28
#define PER_IRQ_SOFT4_OFFSET 0x24
#else
#define PER_IRQ_SOFT0_OFFSET 0x18
#define PER_IRQ_SOFT1_OFFSET 0x14
#endif

#define CA_IPC_TASKLET
//#define CA_IPC_DEBUG

#ifdef CA_IPC_DEBUG
#define CA_DEBUG(fmt, args...) pr_err("CA_IPC:" fmt "\n", ##args)
#elif defined(CONFIG_DYNAMIC_DEBUG)
#define CA_DEBUG(fmt, args...) pr_debug("CA_IPC:" fmt "\n", ##args)
#else
#define CA_DEBUG(fmt, args...) \
	({								\
	if (0)							\
		pr_debug("CA_IPC:" fmt "\n", ##args);	\
	})
#endif
#define CA_ERROR(fmt, args...) pr_err("CA_IPC:" fmt "\n", ##args)

#define DUMMY_SIZE (IPC_ITEM_SIZE  - (3 * sizeof(unsigned int)))

#define MSG_V2_ACK_SIZE	(DUMMY_SIZE - 1)
struct list_ctrl {
	unsigned int done_offset;
	unsigned int current_send_offset;
	unsigned int version;
	u8 magic_id;
	u8 v2_ack_buffer[MSG_V2_ACK_SIZE];  // aligned to IPC_ITEM_SIZE
} __packed;

struct msg_header {
	struct ca_ipc_addr src_addr;
	struct ca_ipc_addr dst_addr;

	unsigned short priority : 1;
	unsigned short ipc_flag : 2;
	unsigned short msg_no : 13;     //16 bit
	unsigned short payload_size;    //16 bit
	unsigned short trans_id;        //16 bit
	unsigned short next_offset;     //16 bit
} __packed;

#define PAYLOAD_SIZE (IPC_ITEM_SIZE - sizeof(struct msg_header))
#define MAX_ITEM_NO (((IPC_LIST_SIZE - sizeof(struct list_ctrl)) / IPC_ITEM_SIZE) - 1)
#define LAST_ITEM_NO	(MAX_ITEM_NO - 1)

struct list_item {
	struct msg_header msg_header;
	unsigned char payload[PAYLOAD_SIZE];
} __packed;

struct list {
	struct list_ctrl list_ctrl;
	struct list_item list_item[MAX_ITEM_NO];
	struct list_item sync_ack;
} __packed;

/* Information used by session */
struct ipc_context {
	struct list_head list;
	struct ca_ipc_addr addr;
	unsigned short trans_id;
	void *private_data;
	struct ca_ipc_msg *msg_procs;
	unsigned short msg_number;
	unsigned short invoke_number;
	unsigned short wait_trans_id;
	/* protect ipc_context */
	spinlock_t lock;
	struct completion complete;
	struct msg_header *ack_item;
	unsigned short ack_offset;
};

struct ipc_module {
	struct ca_ipc_addr addr;
	struct list *root_list;
	struct list *arm_pe0;
	struct list *arm_pe1;
	struct list *pe0_arm;
	struct list *pe1_arm;
	//void __iomem * mbox_addr;
	void __iomem *irq_reg_to_pe;
	void __iomem *irq_reg_from_pe;
	unsigned int mbx_irq;
#if defined(CA_IPC_TASKLET)
	struct tasklet_struct tasklet_pe0;
	struct tasklet_struct tasklet_pe1;
#else
	struct work_struct work_pe0;
	struct work_struct work_pe1;
#endif
	unsigned short wait_queue0_count;
	unsigned short wait_queue1_count;
	/* protect ipc_module */
	spinlock_t lock;
	/* protect ca_ipc_msg_sync_v2_send */
	spinlock_t sync_v2_lock;
	struct list_head session_list;
	//struct msg_header *ack_item;
	//unsigned short pe0_current_offset;
	//unsigned short pe1_current_offset;
#if defined(CONFIG_ARCH_CA_MERCURY)
	struct list *arm_pe2;
	struct list *pe2_arm;
#if defined(CA_IPC_TASKLET)
	struct tasklet_struct tasklet_pe2;
#else
	struct work_struct work_pe2;
#endif
#endif

#if defined(CONFIG_ARCH_CA_MERCURY)
	struct list *arm_pe3;
	struct list *pe3_arm;
#if defined(CA_IPC_TASKLET)
	struct tasklet_struct tasklet_pe3;
#else
	struct work_struct work_pe3;
#endif
#endif
};

//struct ipc_addr {
//	unsigned char	client_id;
//	unsigned char	cpu_id;
//} __packed;

struct ipc_context;
//ca_ipc_cpu_id_t int (*ipc_msg_proc)( struct ipc_addr peer, unsigned short msg_no,
//	       const void *msg_data, unsigned short msg_size,
//	       struct ipc_context* context);

struct ca_ipc_msg {
	unsigned char msg_no;
	unsigned long proc;
};

//int ca_ipc_msg_handle_register(unsigned char client_id, const struct ca_ipc_msg *msg_procs,
//		unsigned short msg_count, unsigned short invoke_count, void *private_data,
//		struct ipc_context **context);

//int ca_ipc_send(struct ipc_context *context, unsigned char cpu_id,
//		unsigned char client_id, unsigned char priority, unsigned short msg_no,
//		const void *msg_data, unsigned short msg_size);

#define _CA_IPC_SYNC__
#ifdef _CA_IPC_SYNC__

//enum ca_ipc_cpu_id int (*ipc_invoke_proc)(struct ipc_addr peer, unsigned short msg_no,
//	       const void *msg_data, unsigned short msg_size,
//	       void *result_data, unsigned short *result_data_size,
//	       struct ipc_context* context );

//int ca_ipc_invoke(struct ipc_context *context, unsigned char cpu_id,
//		    unsigned char client_id, unsigned char priority, unsigned short msg_no,
//		    const void *msg_data, unsigned short msg_size,
//		    void *result_data, unsigned short *result_size );
#endif

//int ca_ipc_msg_handle_unregister(struct ipc_context *context);
void ca_ipc_print_status(unsigned char cpu_id);
void ca_ipc_reset_list_info(unsigned char cpu_id);

#endif /* __CA_IPC_PRI_H__ */
