#ifndef UXLSNR_H_INCLUDED
#define UXLSNR_H_INCLUDED

#include <stdbool.h>

bool waiting_clients(void);
void uxsock_cleanup(void *arg);
void *uxsock_listen(int n_socks, long *ux_sock, void *trigger_data);
int init_wait_fd(void);
void cleanup_wait_fd(void);
void wait_for_event(struct list_head *wait_node, struct list_head *wait_list);
void wakeup_event_waiter(struct list_head *wait_list);

#endif
