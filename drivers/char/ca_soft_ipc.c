// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kd.h>
#include <linux/slab.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/tiocl.h>
#include <linux/kbd_kern.h>
#include <linux/consolemap.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/pm.h>
#include <linux/font.h>
#include <linux/bitops.h>
#include <linux/notifier.h>
#include <linux/device.h>
#include <linux/completion.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/semaphore.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/io.h>

#include "ca_soft_ipc_pri.h"
#include <soc/cortina-access/ca_ipc_pe.h>

MODULE_LICENSE("GPL");

#define CA_IPC_LOCK(l, f)   spin_lock_irqsave(l, f)
#define CA_IPC_UNLOCK(l, f) spin_unlock_irqrestore(l, f)

/* This mutex is used for do_sync_message(). */
DEFINE_MUTEX(ipc_msg_context);
/* This mutex is used for ca_ipc_msg_sync_send(). */
DEFINE_MUTEX(ipc_sync_send);

static struct ipc_context *findsession(struct ipc_module *context,
				       unsigned char session_id)
{
	struct list_head *ptr;
	struct ipc_context *session;

	list_for_each(ptr, &context->session_list) {
		session = list_entry(ptr, struct ipc_context, list);
		if (session->addr.session_id == session_id)
			return session;
	}

	return NULL;
}

static struct ipc_module *module_context;

void dumpmem(int *ptr, int size)
{
	int i;

	for (i = 0; i < size; i += 4) {
		if (i % 16 == 0)
			pr_err("\n0x%lx: ", (unsigned long)ptr);
		pr_err("0x%x ", *ptr);
		ptr++;
	}
	pr_err("\n");
}

static void _ipc_raise_int(unsigned char target_cpu)
{
	void __iomem *reg_addr;
	//unsigned int reg_val;

#if !defined(CONFIG_ARCH_CA_MERCURY)
	switch (target_cpu) {
	case  CPU_RCPU0:
		reg_addr = module_context->irq_reg_to_pe
			   + PER_IRQ_SOFT1_OFFSET;
		break;
	case  CPU_RCPU1:
		reg_addr = module_context->irq_reg_to_pe
			   + PER_IRQ_SOFT0_OFFSET;
		break;
	default:
		CA_ERROR("No such targeted CPU ID:%d", target_cpu);
	}
#else
	switch (target_cpu) {
	case  CPU_RCPU0:
		reg_addr = module_context->irq_reg_to_pe
			   + PER_IRQ_SOFT1_OFFSET;
		break;
	case  CPU_RCPU1:
		reg_addr = module_context->irq_reg_to_pe
			   + PER_IRQ_SOFT2_OFFSET;
		break;
	case  CPU_RCPU2:
		reg_addr = module_context->irq_reg_to_pe
			   + PER_IRQ_SOFT3_OFFSET;
		break;
	case  CPU_RCPU3:
		reg_addr = module_context->irq_reg_to_pe
			   + PER_IRQ_SOFT4_OFFSET;
		break;
	default:
		CA_ERROR("No such targeted CPU ID:%d", target_cpu);
	}
#endif
	writel(0x00000001, reg_addr);
}

#define PER_TMR_LD	0x00
#define PER_TMR_CTRL	0x04
#define PER_TMR_CNT	0x08
#define PER_TMR_IE0	0x0c
#define PER_TMR_INT0	0x14
#define PER_TMR_LOADE   0x00

static void _ipc_clear_int(void)
{
	void __iomem *reg_status_addr;

#if !defined(CONFIG_ARCH_CA_MERCURY)
	reg_status_addr = module_context->irq_reg_from_pe + PER_TMR_CTRL;
	writel(0x00000000, reg_status_addr);   // Disable timer

	reg_status_addr = module_context->irq_reg_from_pe + PER_TMR_IE0;
	writel(0x00000000, reg_status_addr);   // Disable int

	reg_status_addr = module_context->irq_reg_from_pe + PER_TMR_INT0;
	writel(0x00000001, reg_status_addr);   // Disable clear int
	//pr_err("_ipc_clear_int\n");
#else
	reg_status_addr = module_context->irq_reg_from_pe + PER_IRQ_SOFT0_OFFSET;
	writel(0x00000000, reg_status_addr);   // Clear int
#endif
}

static void initial_list_ctrl(struct list *ipc_list)
{
	struct list_ctrl *listctrl;

	listctrl = &ipc_list->list_ctrl;
	listctrl->version = CA_IPC_VERSION;
	listctrl->done_offset = LAST_ITEM_NO;
	listctrl->current_send_offset = LAST_ITEM_NO;
}

static void initial_offload_send_offset(struct list *ipc_list)
{
	struct list_ctrl *listctrl;

	listctrl = &ipc_list->list_ctrl;
	listctrl->current_send_offset = LAST_ITEM_NO;
}

void ca_ipc_print_status(unsigned char cpu_id)
{
}

void ca_ipc_reset_list_info(unsigned char cpu_id)
{
	//initial_list_info(cpu_id);
}

int _ipc_msg_handle_register(unsigned char session_id,
			     const struct ca_ipc_msg *msg_procs,
			     unsigned short msg_count,
			     unsigned short invoke_count,
			     void *private_data)
{
	struct ipc_context *session;
	unsigned short total_procs_no;

	total_procs_no = msg_count + invoke_count;
	if (0 == session_id || (0 != total_procs_no && NULL == msg_procs) ||
	    (0 == total_procs_no && NULL != msg_procs)) {
		CA_ERROR("register parameters error: session[%d]", session_id);
		return CA_IPC_EINVAL;
	}

	session = findsession(module_context, session_id);
	if (session) {
		CA_ERROR("has the session: id[%d]", session_id);
		return CA_IPC_EEXIST;
	}

	session = kmalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		CA_ERROR("malloc failed: session[%d]", session_id);
		return CA_IPC_ENOMEM;
	}

	if (total_procs_no == 0) {
		session->msg_procs = NULL;
	} else {
		session->msg_procs = kmalloc_array(total_procs_no,
						   sizeof(struct ca_ipc_msg), GFP_KERNEL);

		if (!session->msg_procs) {
			kfree(session->msg_procs);
			kfree(session);
			CA_ERROR("malloc failed: session[%d]", session_id);
			return CA_IPC_ENOMEM;
		}

		memcpy(session->msg_procs, msg_procs,
		       sizeof(struct ca_ipc_msg) * total_procs_no);
	}

	session->msg_number = msg_count;
	session->invoke_number = invoke_count;
	session->addr.cpu_id = module_context->addr.cpu_id;
	session->addr.session_id = session_id;
	session->trans_id = 0;
	session->private_data = private_data;
	session->wait_trans_id = 0;
	session->ack_item = NULL;
	session->ack_offset = 0;

	spin_lock_init(&session->lock);
	init_completion(&session->complete);
	INIT_LIST_HEAD(&session->list);

	list_add(&session->list, &module_context->session_list);
//	*context = session;

	CA_ERROR("register succesfull:session_id[%d] session %p msg_count %d\n",
		 session_id, session, msg_count);
	return 0;
}

int ca_ipc_msg_handle_register(enum ca_ipc_session_id session_id,
			       const struct ca_ipc_msg_handle *msg_handle_array,
			       unsigned int msg_handle_count)
{
	int result;
	struct ca_ipc_msg msg_handle[16];
	unsigned short i;

	if (msg_handle_count > 16) {
		CA_ERROR("%s parameters invalid", __func__);
		return CA_IPC_ENOMEM;
	}

	for (i = 0; i < msg_handle_count; i++) {
		msg_handle[i].msg_no = (unsigned char)msg_handle_array[i].msg_no;
		msg_handle[i].proc = (unsigned long)msg_handle_array[i].proc;
	}
	result = _ipc_msg_handle_register((unsigned char)session_id, msg_handle,
					  msg_handle_count, 0, NULL);

	return result;
}
EXPORT_SYMBOL(ca_ipc_msg_handle_register);

int _ipc_msg_handle_unregister(struct ipc_context *context)
{
	struct list_head *ptr;
	struct ipc_context *session;
	enum ca_ipc_session_id session_id;

	if (!context) {
		CA_ERROR("%s parameters invalid", __func__);
		return CA_IPC_ENOCLIENT;
	}

	session_id = context->addr.session_id;

	list_for_each(ptr, &module_context->session_list) {
		session = list_entry(ptr, struct ipc_context, list);
		if (session == context) {
			list_del(ptr);
			kfree(session->msg_procs);
			kfree(session);

			CA_DEBUG("session deregister: session[%d]", session_id);
			return 0;
		}
	}

	CA_ERROR("No such session: session[%d]", session_id);
	return CA_IPC_ENOCLIENT;
}

int ca_ipc_msg_handle_unregister(enum ca_ipc_session_id session_id)
{
	struct ipc_context *session;
	int result;

	session = findsession(module_context, session_id);
	if (!session) {
		CA_ERROR("IPC has no the session: id[%d]", session_id);
		return CA_IPC_EEXIST;
	}

	result = _ipc_msg_handle_unregister(session);

	return result;
}
EXPORT_SYMBOL(ca_ipc_msg_handle_unregister);

static int check_client_ready(unsigned char cpu_id)
{
	struct list_ctrl *list = NULL;
	int result = 0;

	if (cpu_id == CPU_RCPU0)
		list = &module_context->pe0_arm->list_ctrl;
	else if (cpu_id == CPU_RCPU1)
		list = &module_context->pe1_arm->list_ctrl;
#if defined(CONFIG_ARCH_CA_MERCURY)
	else if (cpu_id == CPU_RCPU2)
		list = &module_context->pe2_arm->list_ctrl;
	else if (cpu_id == CPU_RCPU3)
		list = &module_context->pe3_arm->list_ctrl;
#endif

	if (!list) {
		pr_err("CA_IPC error: list is empty. (cpu_id %u)\n", cpu_id);
		return CA_IPC_ENOCLIENT;
	}

	//pr_err("IPC:  check_client_ready list is 0x%p\n", list);
	//pr_err("IPC:  check_client_ready &&version is 0x%x\n", list->version);
	if (list->version == 0xFFFFFFFF) {
		pr_err("CA_IPC error: please check co-processer already be booted\n");
		result = CA_IPC_ENOCLIENT;
	}
	return result;
}

static void update_send_offset(unsigned char target_cpu_id,
			       unsigned int send_offset)
{
	struct list_ctrl *this_list_ctrl;

#if !defined(CONFIG_ARCH_CA_MERCURY)
	if (target_cpu_id == CPU_RCPU0)
		this_list_ctrl = &module_context->arm_pe0->list_ctrl;
	else
		this_list_ctrl = &module_context->arm_pe1->list_ctrl;
#else
	if (target_cpu_id == CPU_RCPU0)
		this_list_ctrl = &module_context->arm_pe0->list_ctrl;
	else if (target_cpu_id == CPU_RCPU1)
		this_list_ctrl = &module_context->arm_pe1->list_ctrl;
	else if (target_cpu_id == CPU_RCPU2)
		this_list_ctrl = &module_context->arm_pe2->list_ctrl;
	else if (target_cpu_id == CPU_RCPU3)
		this_list_ctrl = &module_context->arm_pe3->list_ctrl;
#endif

	//pr_err("update_send_offset: this_list_ctrl =0x%p send_offset=0x%x\n",
	//	 this_list_ctrl, send_offset);

	this_list_ctrl->current_send_offset = send_offset;
}

static struct msg_header *_ca_ipc_msg_context(unsigned char target_cpu_id,
					      unsigned int *offset)
{
	//list_item_t *msg_item=NULL;
	struct msg_header *msg_header = NULL;
	struct list_ctrl *this_list_ctrl, *target_list_ctrl;
	struct list *this_list;

	unsigned int tmp_current_send_offset;

#if !defined(CONFIG_ARCH_CA_MERCURY)
	if (target_cpu_id == CPU_RCPU0) {
		this_list_ctrl = &module_context->arm_pe0->list_ctrl;
		target_list_ctrl = &module_context->pe0_arm->list_ctrl;
		this_list = module_context->arm_pe0;
	} else {
		this_list_ctrl = &module_context->arm_pe1->list_ctrl;
		target_list_ctrl = &module_context->pe1_arm->list_ctrl;
		this_list = module_context->arm_pe1;
	}
#else
	if (target_cpu_id == CPU_RCPU0) {
		this_list_ctrl = &module_context->arm_pe0->list_ctrl;
		target_list_ctrl = &module_context->pe0_arm->list_ctrl;
		this_list = module_context->arm_pe0;
	} else if (target_cpu_id == CPU_RCPU1) {
		this_list_ctrl = &module_context->arm_pe1->list_ctrl;
		target_list_ctrl = &module_context->pe1_arm->list_ctrl;
		this_list = module_context->arm_pe1;
	} else if (target_cpu_id == CPU_RCPU2) {
		this_list_ctrl = &module_context->arm_pe2->list_ctrl;
		target_list_ctrl = &module_context->pe2_arm->list_ctrl;
		this_list = module_context->arm_pe2;
	} else if (target_cpu_id == CPU_RCPU3) {
		this_list_ctrl = &module_context->arm_pe3->list_ctrl;
		target_list_ctrl = &module_context->pe3_arm->list_ctrl;
		this_list = module_context->arm_pe3;
	}
#endif

	//mutex lock
	//k_mutex_lock(&msg_context_mutex, K_FOREVER);

	tmp_current_send_offset = this_list_ctrl->current_send_offset + 1;

	if (tmp_current_send_offset >= MAX_ITEM_NO)
		tmp_current_send_offset = 0;

	if (tmp_current_send_offset == target_list_ctrl->done_offset) {
		pr_err("CA_IPC error: %s buffer overrun\n", __func__);
		goto done;
	}

	//this_list_ctrl->current_send_offset = tmp_current_send_offset;
	*offset = tmp_current_send_offset; // update to this_list_ctrl after data prepared

	msg_header = &this_list->list_item[tmp_current_send_offset].msg_header;

done:
	//mutex unlock
	//k_mutex_unlock(&msg_context_mutex);

	/* Return the address of payload*/
	return msg_header;
}

static int fill_header(struct ipc_context *context, unsigned char cpu_id,
		       unsigned char session_id, unsigned char ipc_flag,
		       unsigned char priority, unsigned short msg_no,
		       unsigned short msg_size, struct msg_header *header)
{
	if (NULL == context || IPC_CPU_NUMBER <= cpu_id ||
	    IPC_ITEM_SIZE < (msg_size + sizeof(struct msg_header))) {
		CA_ERROR("Invalid Parameter");
		return CA_IPC_EINVAL;
	}

	header->src_addr = context->addr;
	header->dst_addr.cpu_id = cpu_id;
	header->dst_addr.session_id = session_id;
	header->ipc_flag = ipc_flag;
	header->priority = priority;
	header->msg_no = msg_no;
	header->payload_size = msg_size;

	context->trans_id += 1;
	header->trans_id = context->trans_id;
	return 0;
}

static unsigned long find_callback(struct ca_ipc_msg *messages,
				   unsigned short msg_number,
				   unsigned short search_target)
{
	unsigned int i;

	for (i = 0; i < msg_number; ++i) {
		if (messages[i].msg_no == search_target)
			return messages[i].proc;
	}
	return 0;
}

static int do_asyn_message(struct ipc_context *session,
			   struct msg_header *header)
{
	unsigned long addr;
	ca_ipc_msg_proc_t callback;

	CA_DEBUG("%s: session_id = %d, msg_no = %d\n",
		 __func__, header->src_addr.session_id, header->msg_no);

	addr = find_callback(session->msg_procs, session->msg_number,
			     header->msg_no);
	if (addr == 0) {
		CA_ERROR("%s: No intestesd message: sender[%d:%d] msg_no[%d] receiver[%d:%d]",
			 __func__, header->src_addr.cpu_id,
			 header->src_addr.session_id, header->msg_no,
			 session->addr.cpu_id, session->addr.session_id);
	} else {
		callback = (ca_ipc_msg_proc_t)addr;
		callback(header->src_addr, header->msg_no, header->trans_id, header + 1,
			 &header->payload_size);
	}

	return 0;
}

static int do_sync_message(struct ipc_context *session,
			   struct msg_header *header)
{
	unsigned long addr;
	ca_ipc_msg_proc_t callback;
	struct msg_header *ack_header, *cb_header;
	unsigned int ack_send_offset;
	unsigned char *ack_buffer;
	unsigned long flags;

	CA_DEBUG("IPC debug ARM: %s\n", __func__);
	addr = find_callback(session->msg_procs, session->msg_number,
			     header->msg_no);
	if (addr == 0) {
		CA_ERROR("%s: No intestesd message: sender[%d:%d] msg_no[%d] receiver[%d:%d]\n",
			 __func__, header->src_addr.cpu_id,
			 header->src_addr.session_id, header->msg_no,
			 session->addr.cpu_id, session->addr.session_id);
	} else {
		callback = (ca_ipc_msg_proc_t)addr;
		callback(header->src_addr, header->msg_no, header->trans_id, header + 1,
			 &header->payload_size);
	}

	if (header->payload_size > PAYLOAD_SIZE) {
		pr_err("IPC Error message size big than PAYLOAD_SIZE:");
		pr_err("sender[%d:%d] msg_no[%d] receiver[%d:%d]\n",
		       header->src_addr.cpu_id,	header->src_addr.session_id, header->msg_no,
		       session->addr.cpu_id, session->addr.session_id);
		goto cleanup;
	}

	//mutex_lock(&ipc_msg_context);
	CA_IPC_LOCK(&module_context->lock, flags);

	cb_header = _ca_ipc_msg_context(header->src_addr.cpu_id, &ack_send_offset);

#if !defined(CONFIG_ARCH_CA_MERCURY)
	if (header->src_addr.cpu_id == CA_IPC_CPU_PE0)
		ack_header = &module_context->arm_pe0->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE1)
		ack_header = &module_context->arm_pe1->sync_ack.msg_header;
#else
	if (header->src_addr.cpu_id == CA_IPC_CPU_PE0)
		ack_header = &module_context->arm_pe0->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE1)
		ack_header = &module_context->arm_pe1->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE2)
		ack_header = &module_context->arm_pe2->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE3)
		ack_header = &module_context->arm_pe3->sync_ack.msg_header;
#endif

	//cb_buffer = (struct msg_header *)
	//	      ((unsigned long)(&module_context->root_list[header->src_addr.cpu_id])
	//	      + offset);
	ack_header->payload_size = header->payload_size;//IPC_ITEM_SIZE - sizeof(struct msg_header);
	//ack_buffer = module_context->arm_pe0->sync_ack.payload;
	ack_buffer = (unsigned char *)(ack_header + 1);
	memcpy_fromio(ack_buffer, header + 1, ack_header->payload_size);

	fill_header(session, header->src_addr.cpu_id, header->src_addr.session_id,
		    CA_IPC_ACK_MSG, header->priority, header->trans_id,
		    ack_header->payload_size, ack_header);

	memcpy_fromio(cb_header, ack_header, sizeof(struct msg_header));

	//write_dest_list(header->src_addr.cpu_id, header->priority, offset);
	update_send_offset(header->src_addr.cpu_id, ack_send_offset);
	_ipc_raise_int(header->src_addr.cpu_id);
	//mutex_unlock(&ipc_msg_context);
	CA_IPC_UNLOCK(&module_context->lock, flags);

cleanup:
	//free_list_item(session->addr.cpu_id, header);

	return 0;
}

static int do_ack_message(struct ipc_context *session, struct msg_header *header)
{
	CA_DEBUG("IPC debug ARM: %s\n", __func__);

	if (NULL == session || NULL == header) {
		CA_ERROR("session %p ack message %p", session, header);
		return 0;
	}

#if !defined(CONFIG_ARCH_CA_MERCURY)
	if (header->src_addr.cpu_id == CA_IPC_CPU_PE0)
		session->ack_item = &module_context->pe0_arm->sync_ack.msg_header;
	else
		session->ack_item = &module_context->pe1_arm->sync_ack.msg_header;
#else
	if (header->src_addr.cpu_id == CA_IPC_CPU_PE0)
		session->ack_item = &module_context->pe0_arm->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE1)
		session->ack_item = &module_context->pe1_arm->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE2)
		session->ack_item = &module_context->pe2_arm->sync_ack.msg_header;
	else if (header->src_addr.cpu_id == CA_IPC_CPU_PE3)
		session->ack_item = &module_context->pe3_arm->sync_ack.msg_header;
#endif

	complete(&session->complete);

	return 1;
}

static unsigned int pe0_publisher_todo_offset;
static unsigned int pe1_publisher_todo_offset;
#if defined(CONFIG_ARCH_CA_MERCURY)
static unsigned int pe2_publisher_todo_offset;
static unsigned int pe3_publisher_todo_offset;
#endif

static void update_done_offset(unsigned char pe_id, unsigned int done_offset)
{
	struct list_ctrl *this_list;
	unsigned long flags;

#if !defined(CONFIG_ARCH_CA_MERCURY)
	if (pe_id == 0)
		this_list = &module_context->arm_pe0->list_ctrl;
	else
		this_list = &module_context->arm_pe1->list_ctrl;
#else
	if (pe_id == 0)
		this_list = &module_context->arm_pe0->list_ctrl;
	else if (pe_id == 1)
		this_list = &module_context->arm_pe1->list_ctrl;
	else if (pe_id == 2)
		this_list = &module_context->arm_pe2->list_ctrl;
	else if (pe_id == 3)
		this_list = &module_context->arm_pe3->list_ctrl;
#endif

	CA_IPC_LOCK(&module_context->lock, flags);

	if (done_offset > MAX_ITEM_NO) {
		pr_err("%s error\n", __func__);
		CA_IPC_UNLOCK(&module_context->lock, flags);
		return;
	}

	this_list->done_offset = done_offset;

	CA_IPC_UNLOCK(&module_context->lock, flags);
}

static unsigned int get_next_todo_offset(unsigned int current_done_offset)
{
	unsigned int update_value = current_done_offset;

	if (current_done_offset != LAST_ITEM_NO)
		update_value = update_value + 1;
	else
		update_value = 0;

	return update_value;
}

#if defined(CA_IPC_TASKLET)
static void do_message_pe0(unsigned long pe)
#else
static void do_message_pe0(struct work_struct *work)
#endif
{
	unsigned long offset, flags;
	struct ipc_context *session;
	struct msg_header *item;
	struct list_ctrl *publisher_list, *this_list;

	CA_DEBUG("do_message_pe0");

	publisher_list = &module_context->pe0_arm->list_ctrl;
	this_list = &module_context->arm_pe0->list_ctrl;

	while (pe0_publisher_todo_offset != this_list->done_offset) {
		CA_IPC_LOCK(&module_context->lock, flags);
		offset = get_next_todo_offset(this_list->done_offset);//this_list->done_offset;

		item = (struct msg_header *)&module_context->pe0_arm->list_item[offset];

		session = findsession(module_context, item->dst_addr.session_id);

		CA_DEBUG("do_message session id = %d", item->dst_addr.session_id);
		if (!session) {
			CA_ERROR("do_message: can't find the session id = %d",
				 item->dst_addr.session_id);
			CA_IPC_UNLOCK(&module_context->lock, flags);
			update_done_offset(0, offset);
			continue;
		}

		CA_DEBUG("Recev msg_type[%d] session %p", item->ipc_flag, session);
		CA_DEBUG(" message:sender[%d:%d] msg_no[%d] receiver[%d:%d] len %d trans_id %d",
			 item->src_addr.cpu_id, item->src_addr.session_id,
			 item->msg_no, session->addr.cpu_id, session->addr.session_id,
			 item->payload_size, item->trans_id);

		//this_list->done_offset = pe0_publisher_todo_offset;
		CA_IPC_UNLOCK(&module_context->lock, flags);

		switch (item->ipc_flag) {
		case CA_IPC_ASYN_MSG:
			do_asyn_message(session, item);
			update_done_offset(0, offset);
			break;
		case CA_IPC_SYNC_MSG:
			do_sync_message(session, item);
			update_done_offset(0, offset);
			break;
		case CA_IPC_ACK_MSG:
			do_ack_message(session, item);
			update_done_offset(0, offset);
			break;
		default:
			update_done_offset(0, offset);
			break;
		};

		/* Update again. */
		//pe0_publisher_todo_offset = publisher_list->current_send_offset;
	}
}

#if defined(CA_IPC_TASKLET)
static void do_message_pe1(unsigned long pe)
#else
static void do_message_pe1(struct work_struct *work)
#endif
{
	unsigned long offset, flags;
	struct ipc_context *session;
	struct msg_header *item;
	struct list_ctrl *publisher_list, *this_list;
	struct list_item *list_item_addr;

	CA_DEBUG("do_message_pe1");

	publisher_list = &module_context->pe1_arm->list_ctrl;
	this_list = &module_context->arm_pe1->list_ctrl;

	while (pe1_publisher_todo_offset != this_list->done_offset) {
		CA_IPC_LOCK(&module_context->lock, flags);
		offset = get_next_todo_offset(this_list->done_offset);

		list_item_addr = &module_context->pe1_arm->list_item[offset];
		item = (struct msg_header *)list_item_addr;

		session = findsession(module_context,
				      item->dst_addr.session_id);

		CA_DEBUG("do_message session id = %d",
			 item->dst_addr.session_id);
		if (!session) {
			CA_ERROR("do_message : can't find the session id = %d",
				 item->dst_addr.session_id);
			CA_IPC_UNLOCK(&module_context->lock, flags);
			update_done_offset(1, offset);
			continue;
		}

		CA_DEBUG("Recev msg_type[%d] session %p",
			 item->ipc_flag, session);
		CA_DEBUG(" message:");
		CA_DEBUG(" sender[%d:%d] msg_no[%d]",
			 item->src_addr.cpu_id,
			 item->src_addr.session_id,
			 item->msg_no);
		CA_DEBUG(" receiver[%d:%d] len %d trans_id %d",
			 session->addr.cpu_id,
			 session->addr.session_id,
			 item->payload_size, item->trans_id);

		//this_list->done_offset = pe0_publisher_todo_offset;
		CA_IPC_UNLOCK(&module_context->lock, flags);

		switch (item->ipc_flag) {
		case CA_IPC_ASYN_MSG:
			do_asyn_message(session, item);
			update_done_offset(1, offset);
			break;
		case CA_IPC_SYNC_MSG:
			do_sync_message(session, item);
			update_done_offset(1, offset);
			break;
		case CA_IPC_ACK_MSG:
			do_ack_message(session, item);
			update_done_offset(1, offset);
			break;
		default:
			update_done_offset(1, offset);
			break;
		};

		/* Update again. */
		//pe1_publisher_todo_offset = publisher_list->current_send_offset;
	}
}

#if defined(CONFIG_ARCH_CA_MERCURY)
#if defined(CA_IPC_TASKLET)
static void do_message_pe2(unsigned long pe)
#else
static void do_message_pe2(struct work_struct *work)
#endif
{
	unsigned long offset, flags;
	struct ipc_context *session;
	struct msg_header *item;
	struct list_ctrl *publisher_list, *this_list;
	struct list_item *list_item_addr;

	CA_DEBUG("do_message_pe2");

	publisher_list = &module_context->pe2_arm->list_ctrl;
	this_list = &module_context->arm_pe2->list_ctrl;

	while (pe2_publisher_todo_offset != this_list->done_offset) {
		CA_IPC_LOCK(&module_context->lock, flags);
		offset = get_next_todo_offset(this_list->done_offset);

		list_item_addr = &module_context->pe2_arm->list_item[offset];
		item = (struct msg_header *)list_item_addr;

		session = findsession(module_context,
				      item->dst_addr.session_id);

		CA_DEBUG("do_message_pe2 session id = %d",
			 item->dst_addr.session_id);
		if (!session) {
			CA_ERROR("do_message : can't find the session id = %d",
				 item->dst_addr.session_id);
			CA_IPC_UNLOCK(&module_context->lock, flags);
			update_done_offset(2, offset);
			continue;
		}

		CA_DEBUG("do_message_pe2 Recev msg_type[%d] session %p",
			 item->ipc_flag, session);
		CA_DEBUG(" message:");
		CA_DEBUG(" sender[%d:%d] msg_no[%d]",
			 item->src_addr.cpu_id,
			 item->src_addr.session_id,
			 item->msg_no);
		CA_DEBUG(" receiver[%d:%d] len %d trans_id %d",
			 session->addr.cpu_id,
			 session->addr.session_id,
			 item->payload_size, item->trans_id);

		CA_IPC_UNLOCK(&module_context->lock, flags);

		switch (item->ipc_flag) {
		case CA_IPC_ASYN_MSG:
			do_asyn_message(session, item);
			update_done_offset(2, offset);
			break;
		case CA_IPC_SYNC_MSG:
			do_sync_message(session, item);
			update_done_offset(2, offset);
			break;
		case CA_IPC_ACK_MSG:
			do_ack_message(session, item);
			update_done_offset(2, offset);
			break;
		default:
			update_done_offset(2, offset);
			break;
		};
	}
}

#if defined(CA_IPC_TASKLET)
static void do_message_pe3(unsigned long pe)
#else
static void do_message_pe3(struct work_struct *work)
#endif
{
	unsigned long offset, flags;
	struct ipc_context *session;
	struct msg_header *item;
	struct list_ctrl *publisher_list, *this_list;
	struct list_item *list_item_addr;

	CA_DEBUG("do_message_pe3");

	publisher_list = &module_context->pe3_arm->list_ctrl;
	this_list = &module_context->arm_pe3->list_ctrl;

	while (pe3_publisher_todo_offset != this_list->done_offset) {
		CA_IPC_LOCK(&module_context->lock, flags);
		offset = get_next_todo_offset(this_list->done_offset);

		list_item_addr = &module_context->pe3_arm->list_item[offset];
		item = (struct msg_header *)list_item_addr;

		session = findsession(module_context,
				      item->dst_addr.session_id);

		CA_DEBUG("do_message_pe3 session id = %d",
			 item->dst_addr.session_id);
		if (!session) {
			CA_ERROR("do_message : can't find the session id = %d",
				 item->dst_addr.session_id);
			CA_IPC_UNLOCK(&module_context->lock, flags);
			update_done_offset(3, offset);
			continue;
		}

		CA_DEBUG("do_message_pe3 Recev msg_type[%d] session %p",
			 item->ipc_flag, session);
		CA_DEBUG(" message:");
		CA_DEBUG(" sender[%d:%d] msg_no[%d]",
			 item->src_addr.cpu_id,
			 item->src_addr.session_id,
			 item->msg_no);
		CA_DEBUG(" receiver[%d:%d] len %d trans_id %d",
			 session->addr.cpu_id,
			 session->addr.session_id,
			 item->payload_size, item->trans_id);

		CA_IPC_UNLOCK(&module_context->lock, flags);

		switch (item->ipc_flag) {
		case CA_IPC_ASYN_MSG:
			do_asyn_message(session, item);
			update_done_offset(3, offset);
			break;
		case CA_IPC_SYNC_MSG:
			do_sync_message(session, item);
			update_done_offset(3, offset);
			break;
		case CA_IPC_ACK_MSG:
			do_ack_message(session, item);
			update_done_offset(3, offset);
			break;
		default:
			update_done_offset(3, offset);
			break;
		};
	}
}
#endif

static irqreturn_t list_proc(int irq, void *device)
{
	struct list_ctrl *publisher_list, *this_list;
	/* Clear interrupt status */
	CA_DEBUG("IPC %s", __func__);
	_ipc_clear_int();

	publisher_list = &module_context->pe0_arm->list_ctrl;
	/* Command form pe0 */
	this_list = &module_context->arm_pe0->list_ctrl;

	if (this_list->done_offset != publisher_list->current_send_offset) {
		CA_DEBUG("IPC %s: recivie form pe0", __func__);
		pe0_publisher_todo_offset = publisher_list->current_send_offset;
#if defined(CA_IPC_TASKLET)
		tasklet_schedule(&module_context->tasklet_pe0);
#else
		schedule_work(&module_context->work_pe0);
#endif
	}

	publisher_list = &module_context->pe1_arm->list_ctrl;
	/* Command form pe1 */
	this_list = &module_context->arm_pe1->list_ctrl;

	if (this_list->done_offset != publisher_list->current_send_offset) {
		CA_DEBUG("IPC %s : recivie form pe1", __func__);
		pe1_publisher_todo_offset = publisher_list->current_send_offset;
#if defined(CA_IPC_TASKLET)
		tasklet_schedule(&module_context->tasklet_pe1);
#else
		schedule_work(&module_context->work_pe1);
#endif
	}

#if defined(CONFIG_ARCH_CA_MERCURY)
	publisher_list = &module_context->pe2_arm->list_ctrl;
	/* Command form pe2 */
	this_list = &module_context->arm_pe2->list_ctrl;

	if (this_list->done_offset != publisher_list->current_send_offset) {
		CA_DEBUG("IPC %s : recivie form pe2", __func__);
		pe2_publisher_todo_offset = publisher_list->current_send_offset;
#if defined(CA_IPC_TASKLET)
		tasklet_schedule(&module_context->tasklet_pe2);
#else
		schedule_work(&module_context->work_pe2);
#endif
	}

	publisher_list = &module_context->pe3_arm->list_ctrl;
	/* Command form pe3 */
	this_list = &module_context->arm_pe3->list_ctrl;

	if (this_list->done_offset != publisher_list->current_send_offset) {
		CA_DEBUG("IPC %s : recivie form pe3", __func__);
		pe3_publisher_todo_offset = publisher_list->current_send_offset;
#if defined(CA_IPC_TASKLET)
		tasklet_schedule(&module_context->tasklet_pe3);
#else
		schedule_work(&module_context->work_pe3);
#endif
	}
#endif

	return IRQ_HANDLED;
}

//static unsigned int current_send_offset=0xFFFF;

static int ipc_send_msg(struct ipc_context *context, unsigned char cpu_id,
			unsigned char session_id, unsigned char ipc_flag,
			unsigned char priority, unsigned short msg_no,
			const void *msg_data, unsigned short msg_size)
{
	struct msg_header *header = NULL;
	unsigned int rc;
	unsigned int offset = 0;
	unsigned long flags;

	if (check_client_ready(cpu_id) != 0)
		return CA_IPC_ENOCLIENT;

	CA_IPC_LOCK(&module_context->lock, flags);

	header = _ca_ipc_msg_context(cpu_id, &offset);

	if (!header) {
		CA_IPC_UNLOCK(&module_context->lock, flags);
		return CA_IPC_ENOMEM;
	}

	rc = fill_header(context, cpu_id, session_id, ipc_flag, priority, msg_no, msg_size, header);

	if (rc != 0) {
		CA_IPC_UNLOCK(&module_context->lock, flags);
		return rc;
	}

	memcpy_toio(header + 1, msg_data, msg_size);

	//send_mb_cmd(cpu_id,MB_TYPE_AS,offset);
	update_send_offset(cpu_id, offset);
	_ipc_raise_int(cpu_id);

	CA_IPC_UNLOCK(&module_context->lock, flags);
	CA_DEBUG("Send Message to [%d:%d] trans_id %d OK", cpu_id, session_id, header->trans_id);

	return 0;
}

int ca_ipc_msg_async_send(struct ca_ipc_pkt *p_ipc_pkt)
{
	struct ipc_context *sender;

	sender = findsession(module_context, p_ipc_pkt->session_id);

	if (p_ipc_pkt->msg_size > PAYLOAD_SIZE) {
		CA_ERROR
		    ("%s: message size[%d] more than PAYLOAD_SIZE[%ld]\n",
		     __func__, p_ipc_pkt->msg_size, PAYLOAD_SIZE);
		return CA_IPC_EINVAL;
	}

	if (!sender) {
		CA_ERROR("%s: No Such session_id %d\n",
			 __func__, p_ipc_pkt->session_id);
		return CA_IPC_ENOCLIENT;
	}

	return ipc_send_msg(sender, p_ipc_pkt->dst_cpu_id,
			    p_ipc_pkt->session_id, CA_IPC_ASYN_MSG,
			    p_ipc_pkt->priority, p_ipc_pkt->msg_no,
			    p_ipc_pkt->msg_data, p_ipc_pkt->msg_size);
}
EXPORT_SYMBOL(ca_ipc_msg_async_send);

static struct list_ctrl *__get_client_list_ctrl(u8 cpu_id)
{
	struct list_ctrl *list = NULL;

	if (cpu_id == CPU_RCPU0)
		list = &module_context->pe0_arm->list_ctrl;
	else if (cpu_id == CPU_RCPU1)
		list = &module_context->pe1_arm->list_ctrl;
#if defined(CONFIG_ARCH_CA_MERCURY)
	else if (cpu_id == CPU_RCPU2)
		list = &module_context->pe2_arm->list_ctrl;
	else if (cpu_id == CPU_RCPU3)
		list = &module_context->pe3_arm->list_ctrl;
#endif

	if (!list) {
		pr_err("CA_IPC error: list is empty. (cpu_id %u)\n", cpu_id);
		return NULL;
	}

	return list;
}

int ca_ipc_msg_sync_v2_send(struct ca_ipc_pkt *p_ipc_pkt,
			    void *result_data, unsigned short *result_size)
{
	struct ipc_context *sender;
	int rc;
	struct list_ctrl *list = NULL;
	unsigned int retry = 0;
	unsigned long flags;

	sender = findsession(module_context, p_ipc_pkt->session_id);

	if (p_ipc_pkt->msg_size > PAYLOAD_SIZE) {
		CA_ERROR("%s : message size[%d] more than PAYLOAD_SIZE[%ld]\n",
			 __func__, p_ipc_pkt->msg_size, PAYLOAD_SIZE);
		return CA_IPC_EINVAL;
	}

	if (!sender) {
		CA_ERROR("%s : No Such session_id %d\n",
			 __func__, p_ipc_pkt->session_id);
		return CA_IPC_ENOCLIENT;
	}

	CA_IPC_LOCK(&module_context->sync_v2_lock, flags);

	list = __get_client_list_ctrl(p_ipc_pkt->dst_cpu_id);

	if (!list) {
		CA_ERROR("%s : No Such session_id %d\n",
			 __func__, p_ipc_pkt->session_id);
		CA_IPC_UNLOCK(&module_context->sync_v2_lock, flags);
		return CA_IPC_ENOCLIENT;
	}

	/* Clean up ack memory */
	list->magic_id = 0xbe;

	rc = ipc_send_msg(sender, p_ipc_pkt->dst_cpu_id, p_ipc_pkt->session_id,
			  CA_IPC_SYNC_V2_MSG, p_ipc_pkt->priority,
			  p_ipc_pkt->msg_no, p_ipc_pkt->msg_data,
			  p_ipc_pkt->msg_size);

	if (rc != CA_IPC_OK) {
		CA_IPC_UNLOCK(&module_context->sync_v2_lock, flags);
		return rc;
	}
	CA_DEBUG("%s session %p\n", __func__, sender);

	retry = 5000000;
	while ((list->magic_id == 0xbe) && (retry > 0)) {
		udelay(1);
		retry--;
	}

	if (list->magic_id == 0xdd) {
		rc = CA_IPC_OK;
		if ((*result_size) > (MSG_V2_ACK_SIZE))
			*result_size = (MSG_V2_ACK_SIZE);
		memcpy_fromio(result_data,
			      &list->v2_ack_buffer[0], *result_size);
	} else {
		rc = CA_IPC_ENOCLIENT;
	}

	CA_IPC_UNLOCK(&module_context->sync_v2_lock, flags);
	return rc;
}
EXPORT_SYMBOL(ca_ipc_msg_sync_v2_send);

int ca_ipc_msg_sync_send(struct ca_ipc_pkt *p_ipc_pkt, void *result_data,
			 unsigned short *result_size)
{
	struct ipc_context *sender;
	struct msg_header *ack_header = NULL;
	int rc;

	sender = findsession(module_context, p_ipc_pkt->session_id);

	if (p_ipc_pkt->msg_size > PAYLOAD_SIZE) {
		CA_ERROR
		    ("%s: message size[%d] more than PAYLOAD_SIZE[%ld]\n",
		     __func__, p_ipc_pkt->msg_size, PAYLOAD_SIZE);
		return CA_IPC_EINVAL;
	}

	if (!sender) {
		CA_ERROR("%s: No Such session_id %d\n",
			 __func__, p_ipc_pkt->session_id);
		return CA_IPC_ENOCLIENT;
	}

	mutex_lock(&ipc_sync_send);

	init_completion(&sender->complete);

	rc = ipc_send_msg(sender, p_ipc_pkt->dst_cpu_id, p_ipc_pkt->session_id,
			  CA_IPC_SYNC_MSG, p_ipc_pkt->priority,
			  p_ipc_pkt->msg_no, p_ipc_pkt->msg_data,
			  p_ipc_pkt->msg_size);

	if (rc != 0) {
		mutex_unlock(&ipc_sync_send);
		return rc;
	}
	CA_DEBUG("%s session %p\n", __func__, sender);

	rc = wait_for_completion_timeout(&sender->complete,
					 IPC_DEFAULT_TIMEOUT);
	sender->wait_trans_id = 0;

	if (rc == 0) {
		CA_ERROR("%s timeout\n", __func__);
		mutex_unlock(&ipc_sync_send);
		return CA_IPC_EINTERNAL;
	}

	rc = CA_IPC_EINTERNAL;

	if (sender->ack_item) {
		if (sender->ack_item->ipc_flag == CA_IPC_ACK_MSG) {
			ack_header = sender->ack_item;

			if (ack_header->payload_size > 0) {
				if (ack_header->payload_size <= *result_size) {
					*result_size = ack_header->payload_size;
					memcpy_fromio(result_data,
						      ack_header + 1,
						      *result_size);
					rc = 0;
				} else {
					CA_ERROR("Buffer is too small");
					rc = CA_IPC_ENOMEM;
				}
			} else {
				*result_size = 0;
				rc = 0;
			}
		}
	} else {
		mutex_unlock(&ipc_sync_send);
		CA_ERROR("%s error %d\n", __func__, __LINE__);
	}

	//free_list_item(sender->addr.cpu_id, sender->ack_item);
	memset_io(ack_header, 0, IPC_ITEM_SIZE);
	sender->ack_item = NULL;

	mutex_unlock(&ipc_sync_send);
	return rc;
}
EXPORT_SYMBOL(ca_ipc_msg_sync_send);

#ifdef CONFIG_OF
/* Match table for of_platform binding */
static const struct of_device_id ca_ipc_of_match[] = {
	{ .compatible = "cortina-access,soft_ipc", },
	{},
};
MODULE_DEVICE_TABLE(of, ca_ipc_of_match);
#endif

//#define CA_IPC_TEST
#ifdef CA_IPC_TEST
#include <linux/kthread.h>
#define async_test 1
static struct task_struct *send_sync_tsk;
static int data;

static int send_sync_thread(void *arg)
{
	unsigned int timeout;
	int *d = (int *)arg;

	struct ca_ipc_pkt send_date;
	enum ca_ipc_cpu_id target_cpu = CA_IPC_CPU_PE0;

#if async_test
	char asyncstr[] = {"Hello from ARM, asyncstr"};
#else
	char byestr[] = {"Hello from ARM, nice to meet you"};
	int size_str = sizeof(byestr);
	char result[100];
	unsigned short result_size;
	int rc;
#endif

	for (;;) {
		if (kthread_should_stop())
			break;
		pr_err("%s(): %d\n", __func__, (*d)++);

		do {
#if async_test
			send_date.dst_cpu_id = target_cpu;
			send_date.session_id = 6;
			send_date.msg_no = 1;
			send_date.msg_size = sizeof(asyncstr);
			send_date.msg_data = asyncstr;
			send_date.priority = CA_IPC_PRIO_HIGH;

			ca_ipc_msg_async_send(&send_date);
			ca_ipc_msg_async_send(&send_date);
			ca_ipc_msg_async_send(&send_date);
#else
			send_date.dst_cpu_id = target_cpu;
			send_date.session_id = 3;
			send_date.msg_no = 1;
			send_date.msg_size = size_str;
			send_date.msg_data = byestr;
			send_date.priority = CA_IPC_PRIO_HIGH;

			result_size = 100;
//			rc = ca_ipc_msg_sync_send(&send_date, result, &result_size);
			rc = ca_ipc_msg_sync_v2_send(&send_date, result, &result_size);

			pr_err("rx_callback_async_sync_send result_data = %s, result_size=%d\n",
			       result, result_size);
#endif
			set_current_state(TASK_INTERRUPTIBLE);

			timeout = schedule_timeout(1 * HZ);
		} while (timeout);
	}
	pr_err("break\n");

	return 0;
}

int rx_callback_async(struct ca_ipc_addr peer, unsigned short msg_no,
		      unsigned short trans_id, const void *msg_data,
		      unsigned short *msg_size)
{
	static const char hellostr[] = "Hello from ARM";

	struct ca_ipc_pkt send_date;
	enum ca_ipc_cpu_id target_cpu = CA_IPC_CPU_PE0;

	/* Print out the message from Taroko */
	pr_err("async callback receives msg_no[%d] message[%s]\n", msg_no, (const char *)msg_data);

	send_date.dst_cpu_id = target_cpu;
	send_date.session_id = 5;
	send_date.msg_no = 1;
	send_date.msg_size = 0x10;
	send_date.msg_data = hellostr;
	send_date.priority = CA_IPC_PRIO_HIGH;

	//ca_ipc_send(context, CPU_RCPU0, 5, CA_IPC_HPRIO, 1, hellostr, sizeof(hellostr));
	//ca_ipc_msg_sync_send(struct ca_ipc_pkt *p_ipc_pkt, void *result_data,
	//		       unsigned short *result_size)
	ca_ipc_msg_async_send(&send_date);

	return 543;
}

static unsigned int verify_count;

int rx_callback_async_st(struct ca_ipc_addr peer, unsigned short msg_no,
			 unsigned short trans_id, const void *msg_data,
			 unsigned short *msg_size)
{
	struct ca_ipc_pkt send_date;
	enum ca_ipc_cpu_id target_cpu = CA_IPC_CPU_PE0;

	/* Print out the message from Taroko */

	//verify_count = *(unsigned int*)msg_data;
	memcpy_toio(&verify_count, msg_data, *msg_size);

	pr_err("%s: msg_no[%d] message: %d\n", __func__, msg_no, verify_count);

	send_date.dst_cpu_id = target_cpu;
	send_date.session_id = 6;
	send_date.msg_no = 1;
	send_date.msg_size = sizeof(unsigned int);
	send_date.msg_data = &verify_count;
	send_date.priority = CA_IPC_PRIO_HIGH;

	ca_ipc_msg_async_send(&send_date);

	//msleep(2000);

	return 543;
}

int rx_callback_sync(struct ca_ipc_addr peer, unsigned short msg_no,
		     unsigned short trans_id, const void *msg_data,
		     unsigned short *msg_size)
{
	char byestr[] = {"Bye from ARM, nice to meet you"};
	int size_str = sizeof(byestr);

	pr_err("sync callback receives msg_no[%d] message[%s] size[%d]\n",
	       msg_no, (const char *)msg_data, *msg_size);

	memcpy_toio((char *)msg_data, byestr, size_str);
	pr_err("%s is done\n", __func__);
	*msg_size = size_str;
	return 0;
}

int rx_callback_async_sync_send(struct ca_ipc_addr peer, unsigned short msg_no,
				unsigned short trans_id, const void *msg_data,
				unsigned short *msg_size)
{
	/* Print out the message from Taroko */
	pr_err("async callback receives msg_no[%d] message[%s]\n",
	       msg_no, (const char *)msg_data);

	wake_up_process(send_sync_tsk);
//	pr_err("%s: wake_up_process ipc_test_send_sync error\n" __func__);

	return 0;
}

int rx_callback_sync_st(struct ca_ipc_addr peer, unsigned short msg_no,
			unsigned short trans_id, const void *msg_data,
			unsigned short *msg_size)
{
	struct ca_ipc_pkt send_date;
	enum ca_ipc_cpu_id target_cpu = CA_IPC_CPU_PE0;

	unsigned long long result_data = 0;
	unsigned short result_size = 0;

	int rc;

	/* Print out the message from Taroko */

	//verify_count = *(unsigned int *)msg_data;
	memcpy_toio(&verify_count, msg_data, *msg_size);

	pr_err("%s: msg_no[%d] message: %d\n", __func__, msg_no, verify_count);

	send_date.dst_cpu_id = target_cpu;
	send_date.session_id = 4;
	send_date.msg_no = 1;
	send_date.msg_size = sizeof(result_data);
	send_date.msg_data = &result_data;
	send_date.priority = CA_IPC_PRIO_HIGH;

	//ca_ipc_msg_async_send(&send_date);
	//ca_ipc_msg_sync_send(struct ca_ipc_pkt *p_ipc_pkt, void *result_data,
	//		       unsigned short *result_size);
	result_size = sizeof(result_data);

	rc = ca_ipc_msg_sync_send(&send_date, &result_data, &result_size);

	//if (rc == 0)
	pr_err("%s: result_data = 0x%llx , result_size=%d\n",
	       __func__, result_data, result_size);

	//msleep(2000);

	return 543;
}

struct ca_ipc_msg_handle invoke_procs_async[] = {
	{ .msg_no = 6, .proc = rx_callback_async }
};

struct ca_ipc_msg_handle invoke_procs_async_st[] = {
	{ .msg_no = 6, .proc = rx_callback_async_st }
};

struct ca_ipc_msg_handle invoke_procs_async_sync_send[] = {
	{ .msg_no = 6, .proc = rx_callback_async_sync_send }
};

struct ca_ipc_msg_handle invoke_procs_sync[] = {
	{ .msg_no = 8, .proc = rx_callback_sync }
};

struct ca_ipc_msg_handle invoke_procs_sync_st[] = {
	{ .msg_no = 8, .proc = rx_callback_sync_st }
};

//static struct ipc_context *context;

static int init_receiver(void)
{
	int rc;
	int ret;
	//unsigned short result_size = 0x10;

	rc = ca_ipc_msg_handle_register(5, invoke_procs_async, 1);
	if (rc) {
		pr_err("%s Register Failed :%d\n", __FILE__, __LINE__);
		return 1;
	}

	rc = ca_ipc_msg_handle_register(6, invoke_procs_async_st, 1);
	if (rc) {
		pr_err("%s Register Failed :%d\n", __FILE__, __LINE__);
		return 1;
	}

	rc = ca_ipc_msg_handle_register(7, invoke_procs_async_sync_send, 1);
	if (rc) {
		pr_err("%s Register Failed :%d\n", __FILE__, __LINE__);
		return 1;
	}

	send_sync_tsk = kthread_create(send_sync_thread, &data, "ipc_test_send_sync");
	if (IS_ERR(send_sync_tsk)) {
		ret = PTR_ERR(send_sync_tsk);
		send_sync_tsk = NULL;
		pr_err("rx_callback_async_sync_send : create ipc_test_send_sync error\n");
		return 0;
	}

	rc = ca_ipc_msg_handle_register(3, invoke_procs_sync, 1);
	if (rc) {
		pr_err("%s Register Failed :%d\n", __FILE__, __LINE__);
		return 1;
	}

	rc = ca_ipc_msg_handle_register(4, invoke_procs_sync_st, 1);
	if (rc) {
		pr_err("%s Register Failed :%d\n", __FILE__, __LINE__);
		return 1;
	}

	return 0;
}
#endif

#if defined(CONFIG_ARCH_CA_MERCURY)
#define NAME_SIZE 50

enum ca_system_ipc_command_t {
	CA_SYSTEM_APP_NAME = 1,
	CA_SYSTEM_VERSION = 2,
	CA_SYSTEM_READY_NOTIFY = 3,
};

struct ca_dsp_status_t {
	char name[NAME_SIZE];
	char version[NAME_SIZE];
	u32 bootready;
};

struct ca_dsp_status_t pedsp0_status = {"empty\0", "empty\0", 0};
struct ca_dsp_status_t pedsp1_status = {"empty\0", "empty\0", 0};
#if defined(CONFIG_ARCH_CA_MERCURY)
struct ca_dsp_status_t pedsp2_status = {"empty\0", "empty\0", 0};
#endif

int pe_notify_appname_cb(struct ca_ipc_addr peer, unsigned short msg_no,
			 unsigned short trans_id, const void *msg_data,
			 unsigned short *msg_size)
{
	CA_DEBUG("%s %d\n", __func__, __LINE__);
	//pr_err("cpu_id = 0x%x trans_id 0x%x  , msg_data = %s msg_size =%d\n",
	//	 peer.cpu_id,trans_id, msg_data, *msg_size);

	if (*msg_size > NAME_SIZE) {
		pr_err("IPC error : %s message size biger than buffer\n", __func__);
		return 1;
	}

	if (peer.cpu_id == CA_IPC_CPU_PE0) {
		memcpy_fromio(pedsp0_status.name, msg_data, *msg_size);
		pedsp0_status.name[*msg_size] = '\0';
	} else if (peer.cpu_id == CA_IPC_CPU_PE1) {
		memcpy_fromio(pedsp1_status.name, msg_data, *msg_size);
		pedsp1_status.name[*msg_size] = '\0';
#if defined(CONFIG_ARCH_CA_MERCURY)
	} else if (peer.cpu_id == CA_IPC_CPU_PE2) {
		memcpy_fromio(pedsp2_status.name, msg_data, *msg_size);
		pedsp2_status.name[*msg_size] = '\0';
#endif
	}

	return 0;
}

int pe_notify_version_cb(struct ca_ipc_addr peer, unsigned short msg_no,
			 unsigned short trans_id, const void *msg_data,
			 unsigned short *msg_size)
{
	CA_DEBUG("%s %d\n", __func__, __LINE__);
	//pr_err("cpu_id = 0x%x trans_id 0x%x  , msg_data = %s msg_size =%d\n",
	//	 peer.cpu_id,trans_id, msg_data, *msg_size);

	if (*msg_size > NAME_SIZE) {
		pr_err("IPC error : %s message size biger than buffer\n", __func__);
		return 1;
	}

	if (peer.cpu_id == CA_IPC_CPU_PE0) {
		memcpy_fromio(pedsp0_status.version, msg_data, *msg_size);
		pedsp0_status.version[*msg_size] = '\0';
	} else if (peer.cpu_id == CA_IPC_CPU_PE1) {
		memcpy_fromio(pedsp1_status.version, msg_data, *msg_size);
		pedsp1_status.version[*msg_size] = '\0';
#if defined(CONFIG_ARCH_CA_MERCURY)
	} else if (peer.cpu_id == CA_IPC_CPU_PE2) {
		memcpy_fromio(pedsp2_status.version, msg_data, *msg_size);
		pedsp2_status.version[*msg_size] = '\0';
#endif
	}

	return 0;
}

int pe_notify_ready_cb(struct ca_ipc_addr peer, unsigned short msg_no,
		       unsigned short trans_id, const void *msg_data,
		       unsigned short *msg_size)
{
	CA_DEBUG("%s %d\n", __func__, __LINE__);
	//pr_err("cpu_id = 0x%x trans_id 0x%x\n", peer.cpu_id, trans_id);

	if (peer.cpu_id == CA_IPC_CPU_PE0) {
		pedsp0_status.bootready = 1;
	} else if (peer.cpu_id == CA_IPC_CPU_PE1) {
		pedsp1_status.bootready = 1;
#if defined(CONFIG_ARCH_CA_MERCURY)
	} else if (peer.cpu_id == CA_IPC_CPU_PE2) {
		pedsp2_status.bootready = 1;
#endif
	}

	return 0;
}

struct ca_ipc_msg_handle pe_notify[] = {
	{ .msg_no = CA_SYSTEM_APP_NAME, .proc = pe_notify_appname_cb },
	{ .msg_no = CA_SYSTEM_VERSION, .proc = pe_notify_version_cb },
	{ .msg_no = CA_SYSTEM_READY_NOTIFY, .proc = pe_notify_ready_cb }
};

static int init_pe_notify(void)
{
	int rc;

	rc = ca_ipc_msg_handle_register(CA_IPC_SESSION_SYSTEM, pe_notify, 3);
	if (rc != CA_IPC_OK) {
		pr_err("%s Register Failed :%d\n", __FILE__, __LINE__);
		return 1;
	}

	return 0;
}
#endif	/* CONFIG_ARCH_CA_MERCURY */

static int ca_ipc_probe(struct platform_device *pdev)
{
	int rc;
	int ret = -1;
	struct resource mem_resource;
	const struct of_device_id *match;
	struct device_node *np;
	unsigned long shm_size;
	unsigned int cpu_id_len = 0;
	const __be32 *this_CPU;

	module_context = kmalloc(sizeof(*module_context), GFP_KERNEL);
	module_context->addr.cpu_id = CPU_ARM;

	/* assign DT node pointer */
	np = pdev->dev.of_node;

	/* search DT for a match */
	match = of_match_device(ca_ipc_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	this_CPU = of_get_property(pdev->dev.of_node, "pe-ipc-cpuid", &cpu_id_len);
	if (!this_CPU) {
		pr_err("IPC: missing reg property 'pe-ipc-cpuid'.\n");
		return -EINVAL;
	}

	if (be32_to_cpup(this_CPU) == CPU_ARM) {
		pr_err("IPC: host cpu is ARM , this_CPU=0x%x\n", be32_to_cpup(this_CPU));
		module_context->addr.cpu_id = CPU_ARM;
	} else if (be32_to_cpup(this_CPU) == CPU_RCPU0) {
		pr_err("IPC: host cpu is PE0 , this_CPU=0x%x\n", be32_to_cpup(this_CPU));
		module_context->addr.cpu_id = CPU_RCPU0;
	} else if (be32_to_cpup(this_CPU) == CPU_RCPU1) {
		pr_err("IPC: host cpu is PE1 , this_CPU=0x%x\n", be32_to_cpup(this_CPU));
		module_context->addr.cpu_id = CPU_RCPU1;
#if defined(CONFIG_ARCH_CA_MERCURY)
	} else if (be32_to_cpup(this_CPU) == CPU_RCPU2) {
		pr_err("IPC: host cpu is PE2 , this_CPU=0x%x\n", be32_to_cpup(this_CPU));
		module_context->addr.cpu_id = CPU_RCPU2;
	} else if (be32_to_cpup(this_CPU) == CPU_RCPU3) {
		pr_err("IPC: host cpu is PE3 , this_CPU=0x%x\n", be32_to_cpup(this_CPU));
		module_context->addr.cpu_id = CPU_RCPU3;
#endif
	}

	//pr_err("IPC: reg 0x%x\n",mem_resource.start);
	module_context->irq_reg_to_pe = of_iomap(np, 0);
	WARN_ON(!module_context->irq_reg_to_pe);
#if !defined(CONFIG_ARCH_CA_MERCURY)
	module_context->irq_reg_from_pe = of_iomap(np, 1);
	WARN_ON(!module_context->irq_reg_from_pe);
#else
	module_context->irq_reg_from_pe = of_iomap(np, 0);
	WARN_ON(!module_context->irq_reg_from_pe);
#endif

	if (!module_context->irq_reg_to_pe || !module_context->irq_reg_from_pe)
		return -ENOMEM;

	pr_err("IPC: irq_reg_from_pe map reg 0x%p\n", module_context->irq_reg_from_pe);

#if defined(CONFIG_ARCH_CA_MERCURY)
	pr_err("IPC: irq_reg_from_pe map reg 0x%p\n", module_context->irq_reg_from_pe);
#endif

	/* get "shared memory" from DT and convert to platform mem address resource */
	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np) {
		pr_err("No %s specified\n", "memory-region");
		return ret;
	}

	ret = of_address_to_resource(np, 0, &mem_resource);
	if (ret) {
		pr_err("%s: of_address_to_resource(CA_IPC_MEM_RESOURCE_SHAREMEM) return %d\n",
		       __func__, ret);
		return ret;
	}

	shm_size = resource_size(&mem_resource);
	module_context->root_list = devm_ioremap(&pdev->dev, mem_resource.start, shm_size);
	pr_err("IPC: list physical start from: 0x%lx\n", (unsigned long)mem_resource.start);
	pr_err("IPC: list virtual start from: 0x%lx\n", (unsigned long)module_context->root_list);
	pr_err("IPC: list size: 0x%lx\n", shm_size);

	if (!module_context->root_list)
		return -ENOMEM;

	memset_io((void *)&module_context->root_list[0],
		  0xFF, shm_size); //let't shm set to un-init states

	module_context->arm_pe0 = &module_context->root_list[0];

	pr_err("IPC: Debug:...Clearing arm_pe0 memory.... from 0x%lx\n",
	       (unsigned long)(module_context->arm_pe0));
	memset_io((void *)module_context->arm_pe0, 0x0, IPC_LIST_SIZE);

	module_context->arm_pe1 = &module_context->root_list[1];

	pr_err("IPC: Debug:...Clearing arm_pe1 memory.... from 0x%lx\n",
	       (unsigned long)(module_context->arm_pe1));
	memset_io((void *)module_context->arm_pe1, 0x0, IPC_LIST_SIZE);

	module_context->pe0_arm = &module_context->root_list[2];
	module_context->pe1_arm = &module_context->root_list[3];

#if defined(CONFIG_ARCH_CA_MERCURY)
	module_context->arm_pe2 = &module_context->root_list[4];
	pr_err("IPC: Debug:...Clearing arm_pe2 memory.... from 0x%lx\n",
	       (unsigned long)(module_context->arm_pe2));
	memset_io((void *)module_context->arm_pe2, 0x0, IPC_LIST_SIZE);
	module_context->pe2_arm = &module_context->root_list[5];

	module_context->arm_pe3 = &module_context->root_list[6];
	pr_err("IPC: Debug:...Clearing arm_pe3 memory.... from 0x%lx\n",
	       (unsigned long)(module_context->arm_pe3));
	memset_io((void *)module_context->arm_pe3, 0x0, IPC_LIST_SIZE);
	module_context->pe3_arm = &module_context->root_list[7];
#endif

	spin_lock_init(&module_context->lock);
	spin_lock_init(&module_context->sync_v2_lock);
#if defined(CA_IPC_TASKLET)
	tasklet_init(&module_context->tasklet_pe0, do_message_pe0, 0);
	tasklet_init(&module_context->tasklet_pe1, do_message_pe1, 1);
#else
	INIT_WORK(&module_context->work_pe0, do_message_pe0);
	INIT_WORK(&module_context->work_pe1, do_message_pe1);
#endif

#if defined(CONFIG_ARCH_CA_MERCURY)
#if defined(CA_IPC_TASKLET)
	tasklet_init(&module_context->tasklet_pe2, do_message_pe2, 2);
	tasklet_init(&module_context->tasklet_pe3, do_message_pe3, 3);
#else
	INIT_WORK(&module_context->work_pe2, do_message_pe2);
	INIT_WORK(&module_context->work_pe3, do_message_pe3);
#endif
#endif

	//CA_DEBUG("cpu_id[%d] memory address 0x%lx\n", module_context->addr.cpu_id,
	//		   (unsigned long)(module_context->host2pe0));
	//CA_DEBUG("Remap address = %lx\n",(unsigned long)(module_context->mbox_addr));

	//module_context->addr.session_id = 0;

	//module_context->wait_queue0_count = module_context->wait_queue1_count = 0;

	initial_list_ctrl(module_context->arm_pe0);
	initial_list_ctrl(module_context->arm_pe1);

	initial_offload_send_offset(module_context->pe0_arm);
	initial_offload_send_offset(module_context->pe1_arm);

#if defined(CONFIG_ARCH_CA_MERCURY)
	initial_list_ctrl(module_context->arm_pe2);
	initial_offload_send_offset(module_context->pe2_arm);

	initial_list_ctrl(module_context->arm_pe3);
	initial_offload_send_offset(module_context->pe3_arm);
#endif

	INIT_LIST_HEAD(&module_context->session_list);

	pr_err("IPC: version is %x\n", module_context->arm_pe0->list_ctrl.version);
	np = pdev->dev.of_node;
	/* get "interrupts" property from DT */
	module_context->mbx_irq = irq_of_parse_and_map(np, 0);
	pr_err("IPC: irq = %d\n", module_context->mbx_irq);

	//module_context->pe0_current_offset = sizeof(struct list_info);
	//module_context->pe1_current_offset = sizeof(struct list_info);

	rc = request_irq(module_context->mbx_irq, list_proc, 0, "ca_soft-ipc", NULL);
	if (rc > 0) {
		pr_err("request irq failed[%d]", rc);
		kfree(module_context);
		return rc;
	}

#ifdef CA_IPC_TEST
	init_receiver();
#endif
#if defined(CONFIG_ARCH_CA_MERCURY)
	init_pe_notify();
#endif

	return 0;
}

static int ca_ipc_remove(struct platform_device *op)
{
	if (module_context->addr.cpu_id == 0) {
		iounmap(module_context->root_list);
		//iounmap(module_context->mbox_addr);
		kfree((void *)module_context->root_list);
	}

	//free_irq(module_context->mbx_irq, NULL);
	kfree(module_context);

	return 0;
}

static struct platform_driver ca_ipc_driver = {
	.probe = ca_ipc_probe,
	.remove = ca_ipc_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "ca_ipc",
		.of_match_table = of_match_ptr(ca_ipc_of_match),
	},
};

static int __init ca_ipc_init(void)
{
	return platform_driver_register(&ca_ipc_driver);
}

static void __exit ca_ipc_exit(void)
{
	platform_driver_unregister(&ca_ipc_driver);
}

module_init(ca_ipc_init);
module_exit(ca_ipc_exit);

#if defined(CONFIG_ARCH_CA_MERCURY)
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int pedsp0_proc_name_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp0_proc_name_r %s\n", pedsp0_status.name);
	//seq_puts(s, pedsp0_status.name);
	seq_printf(s, "%s\n", pedsp0_status.name);
	//pr_err("pedsp0_proc_name_r\n");
	return 0;
}

static int pedsp0_proc_open_name(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp0_proc_name_r, NULL);
}

static const struct proc_ops pedsp0_proc_name_fops = {
	.proc_open		= pedsp0_proc_open_name,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp0_proc_version_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp0_proc_version_r %s\n", pedsp0_status.version);
	//seq_puts(s, pedsp0_status.version);
	seq_printf(s, "%s\n", pedsp0_status.version);
	return 0;
}

static int pedsp0_proc_open_version(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp0_proc_version_r, NULL);
}

static const struct proc_ops pedsp0_proc_version_fops = {
	.proc_open		= pedsp0_proc_open_version,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp0_proc_status_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp0_proc_status_r %s\n", pedsp0_status.version);
	if (pedsp0_status.bootready == 1)
		seq_puts(s, "BootComplete\n");
	else
		seq_puts(s, "resetting\n");

	//seq_puts(s, pedsp0_status.version);
	return 0;
}

static int pedsp0_proc_open_status(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp0_proc_status_r, NULL);
}

static const struct proc_ops pedsp0_proc_status_fops = {
	.proc_open		= pedsp0_proc_open_status,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp1_proc_name_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp1_proc_name_r %s\n", pedsp1_status.name);
	//seq_puts(s, pedsp1_status.name);
	seq_printf(s, "%s\n", pedsp1_status.name);
	//pr_err("pedsp1_proc_name_r\n");
	return 0;
}

static int pedsp1_proc_open_name(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp1_proc_name_r, NULL);
}

static const struct proc_ops pedsp1_proc_name_fops = {
	.proc_open		= pedsp1_proc_open_name,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp1_proc_version_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp1_proc_version_r %s\n", pedsp1_status.version);
	//seq_puts(s, pedsp1_status.version);
	seq_printf(s, "%s\n", pedsp1_status.version);
	return 0;
}

static int pedsp1_proc_open_version(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp1_proc_version_r, NULL);
}

static const struct proc_ops pedsp1_proc_version_fops = {
	.proc_open		= pedsp1_proc_open_version,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp1_proc_status_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp1_proc_status_r %s\n", pedsp1_status.version);
	if (pedsp1_status.bootready == 1)
		seq_puts(s, "BootComplete\n");
	else
		seq_puts(s, "resetting\n");

	//seq_puts(s, pedsp1_status.version);
	return 0;
}

static int pedsp1_proc_open_status(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp1_proc_status_r, NULL);
}

static const struct proc_ops pedsp1_proc_status_fops = {
	.proc_open		= pedsp1_proc_open_status,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

#if defined(CONFIG_ARCH_CA_MERCURY)
static int pedsp2_proc_name_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp2_proc_name_r %s\n", pedsp2_status.name);
	//seq_puts(s, pedsp2_status.name);
	seq_printf(s, "%s\n", pedsp2_status.name);
	//pr_err("pedsp2_proc_name_r\n");
	return 0;
}

static int pedsp2_proc_open_name(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp2_proc_name_r, NULL);
}

static const struct proc_ops pedsp2_proc_name_fops = {
	.proc_open		= pedsp2_proc_open_name,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp2_proc_version_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp2_proc_version_r %s\n", pedsp2_status.version);
	//seq_puts(s, pedsp2_status.version);
	seq_printf(s, "%s\n", pedsp2_status.version);
	return 0;
}

static int pedsp2_proc_open_version(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp2_proc_version_r, NULL);
}

static const struct proc_ops pedsp2_proc_version_fops = {
	.proc_open		= pedsp2_proc_open_version,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int pedsp2_proc_status_r(struct seq_file *s, void *v)
{
	//pr_err("pedsp2_proc_status_r %s\n", pedsp2_status.version);
	if (pedsp2_status.bootready == 1)
		seq_puts(s, "BootComplete\n");
	else
		seq_puts(s, "resetting\n");

	//seq_puts(s, pedsp2_status.version);
	return 0;
}

static int pedsp2_proc_open_status(struct inode *inode, struct file *file)
{
	return single_open(file, pedsp2_proc_status_r, NULL);
}

static const struct proc_ops pedsp2_proc_status_fops = {
	.proc_open		= pedsp2_proc_open_status,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};
#endif	/* CONFIG_ARCH_CA_MERCURY */

static int __init proc_pestatus_init(void)
{
	struct proc_dir_entry *proc_pedsp_dev_dir;
	struct proc_dir_entry *entry;

	proc_pedsp_dev_dir = proc_mkdir("pedsp0Info", NULL);
	if (!proc_pedsp_dev_dir) {
		pr_err("create proc pedsp0Info failed!\n");
		return 1;
	}

	proc_pedsp_dev_dir = proc_mkdir("pedsp1Info", NULL);
	if (!proc_pedsp_dev_dir) {
		pr_err("create proc pedsp1Info failed!\n");
		return 1;
	}

#if defined(CONFIG_ARCH_CA_MERCURY)
	proc_pedsp_dev_dir = proc_mkdir("pedsp2Info", NULL);
	if (!proc_pedsp_dev_dir) {
		pr_err("create proc pedsp2Info failed!\n");
		return 1;
	}
#endif

	/* Subitems of /proc/pedsp0Info directory */
	entry = proc_create("pedsp0Info/name", 0, NULL, &pedsp0_proc_name_fops);
	if (!entry) {
		pr_err("create proc pedsp0/name failed!\n");
		return 1;
	}

	entry = proc_create("pedsp0Info/version", 0, NULL, &pedsp0_proc_version_fops);
	if (!entry) {
		pr_err("create proc pedsp0/name failed!\n");
		return 1;
	}

	entry = proc_create("pedsp0Info/status", 0, NULL, &pedsp0_proc_status_fops);
	if (!entry) {
		pr_err("create proc pedsp0/name failed!\n");
		return 1;
	}

	/* Subitems of /proc/pedsp1Info directory */
	entry = proc_create("pedsp1Info/name", 0, NULL, &pedsp1_proc_name_fops);
	if (!entry) {
		pr_err("create proc pedsp1/name failed!\n");
		return 1;
	}

	entry = proc_create("pedsp1Info/version", 0, NULL, &pedsp1_proc_version_fops);
	if (!entry) {
		pr_err("create proc pedsp1/name failed!\n");
		return 1;
	}

	entry = proc_create("pedsp1Info/status", 0, NULL, &pedsp1_proc_status_fops);
	if (!entry) {
		pr_err("create proc pedsp1/name failed!\n");
		return 1;
	}

#if defined(CONFIG_ARCH_CA_MERCURY)
	/* Subitems of /proc/pedsp2Info directory */
	entry = proc_create("pedsp2Info/name", 0, NULL, &pedsp2_proc_name_fops);
	if (!entry) {
		pr_err("create proc pedsp2/name failed!\n");
		return 1;
	}

	entry = proc_create("pedsp2Info/version", 0, NULL, &pedsp2_proc_version_fops);
	if (!entry) {
		pr_err("create proc pedsp2/name failed!\n");
		return 1;
	}

	entry = proc_create("pedsp2Info/status", 0, NULL, &pedsp2_proc_status_fops);
	if (!entry) {
		pr_err("create proc pedsp2/name failed!\n");
		return 1;
	}
#endif

	return 0;
}
fs_initcall(proc_pestatus_init);
#endif	/* CONFIG_ARCH_CA_MERCURY */
