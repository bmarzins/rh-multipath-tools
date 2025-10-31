#ifndef MAIN_H
#define MAIN_H

#define MAPGCINT 5

enum daemon_status {
	DAEMON_INIT,
	DAEMON_START,
	DAEMON_CONFIGURE,
	DAEMON_IDLE,
	DAEMON_RUNNING,
	DAEMON_SHUTDOWN,
};

struct prout_param_descriptor;
struct prin_resp;

extern pid_t daemon_pid;
extern int uxsock_timeout;

void exit_daemon(void);
const char * daemon_status(void);
enum daemon_status wait_for_state_change_if(enum daemon_status oldstate,
					    unsigned long ms);
int need_to_delay_reconfig (struct vectors *);
int reconfigure (struct vectors *);
int ev_add_path (struct path *, struct vectors *, int);
int ev_remove_path (struct path *, struct vectors *, int);
int ev_add_map (char *, const char *, struct vectors *);
int ev_remove_map (char *, char *, int, struct vectors *);
int flush_map(struct multipath *, struct vectors *, int);
int set_config_state(enum daemon_status);
void * mpath_alloc_prin_response(int prin_sa);
void handle_signals(bool);
int __setup_multipath (struct vectors * vecs, struct multipath * mpp,
		       int reset);
#define setup_multipath(vecs, mpp) __setup_multipath(vecs, mpp, 1)
int update_multipath (struct vectors *vecs, char *mapname, int reset);
int update_path_groups(struct multipath *mpp, struct vectors *vecs,
		       int refresh);

void handle_path_wwid_change(struct path *pp, struct vectors *vecs);
bool check_path_wwid_change(struct path *pp);
void set_pr(struct multipath *mpp);
void unset_pr(struct multipath *mpp);
void pr_register_active_paths(struct multipath *mpp, bool check_active_nr);
#endif /* MAIN_H */
