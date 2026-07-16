#ifndef _RPROC_SUBSYS_NOTIFIER_H_
#define _RPROC_SUBSYS_NOTIFIER_H_

enum dummy_subsys_notif_type {
       SUBSYS_BEFORE_SHUTDOWN,
       SUBSYS_AFTER_SHUTDOWN,
       SUBSYS_BEFORE_POWERUP,
       SUBSYS_AFTER_POWERUP,
       SUBSYS_RAMDUMP_NOTIFICATION,
       SUBSYS_POWERUP_FAILURE,
       SUBSYS_PROXY_VOTE,
       SUBSYS_PROXY_UNVOTE,
       SUBSYS_SOC_RESET,
       SUBSYS_PREPARE_FOR_FATAL_SHUTDOWN,
       SUBSYS_NOTIF_TYPE_COUNT
};

static inline int
rproc_register_subsys_notifier(const char *name,
			       struct notifier_block *nb,
			       struct notifier_block *atomic_nb)
{
	return 0;
}

static inline int
rproc_unregister_subsys_notifier(const char *name,
				 struct notifier_block *nb,
				 struct notifier_block *atomic_nb)
{
	return 0;
}

#endif /* _RPROC_SUBSYS_NOTIFIER_H */
