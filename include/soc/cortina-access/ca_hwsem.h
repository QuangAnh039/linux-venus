/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CA_HWSEN_H__
#define __CA_HWSEM_H__

enum ca_sem_id {
	CA_SEM_IPC = 0,
	CA_SEM_FDB,
	CA_SEM_DUMY1,
	CA_SEM_DUMY2,
};

/* API prototype */

int ca_sem_lock(enum ca_sem_id semid, unsigned long flags);
int ca_sem_unlock(enum ca_sem_id semid, unsigned long flags);

#endif // __CA_HWSEM_H__

