#include <libdevmapper.h>
#include "defaults.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include "vector.h"
#include "checkers.h"
#include "structs.h"
#include "structs_vec.h"
#include <libudev.h>

#include "prio.h"
#include <unistd.h>
#include "devmapper.h"
#include "debug.h"
#include "config.h"
#include "switchgroup.h"
#include "discovery.h"
#include "configure.h"
#include "dmparser.h"
#include <ctype.h>
#include "propsel.h"
#include "util.h"
#include "unaligned.h"

#include "mpath_persist.h"
#include "mpathpr.h"
#include "mpath_pr_ioctl.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define __STDC_FORMAT_MACROS 1

extern struct udev *udev;

struct config *
mpath_lib_init (void)
{
	struct config *conf;

	conf = load_config(DEFAULT_CONFIGFILE);
	if (!conf) {
		condlog(0, "Failed to initialize multipath config.");
		return NULL;
	}
	conf->force_sync = 1;
	set_max_fds(conf->max_fds);

	return conf;
}

int
mpath_lib_exit (struct config *conf)
{
	dm_lib_release();
	dm_lib_exit();
	cleanup_prio();
	cleanup_checkers();
	free_config(conf);
	conf = NULL;
	return 0;
}

static int
updatepaths (struct multipath * mpp)
{
	int i, j;
	struct pathgroup * pgp;
	struct path * pp;
	struct config *conf;

	if (!mpp->pg)
		return 0;

	vector_foreach_slot (mpp->pg, pgp, i){
		if (!pgp->paths)
			continue;

		vector_foreach_slot (pgp->paths, pp, j){
			if (!strlen(pp->dev)){
				/*
				 * path is not in sysfs anymore
				 */
				pp->state = PATH_DOWN;
				continue;
			}
			pp->mpp = mpp;
			if (pp->udev == NULL) {
				pp->udev = get_udev_device(pp->dev_t, DEV_DEVT);
				if (pp->udev == NULL) {
					pp->state = PATH_DOWN;
					continue;
				}
				conf = get_multipath_config();
				pathinfo(pp, conf, DI_SYSFS|DI_CHECKER);
				put_multipath_config(conf);
				continue;
			}
			if (pp->state == PATH_UNCHECKED ||
					pp->state == PATH_WILD) {
				conf = get_multipath_config();
				pathinfo(pp, conf, DI_CHECKER);
				put_multipath_config(conf);
			}
		}
	}
	return 0;
}

static int mpath_prout_common(struct multipath *mpp,int rq_servact,
			      int rq_scope, unsigned int rq_type,
			      struct prout_param_descriptor* paramp, int noisy,
			      struct path **pptr);

int
mpath_prin_activepath (struct multipath *mpp, int rq_servact,
	struct prin_resp * resp, int noisy)
{
	int i,j, ret = MPATH_PR_DMMP_ERROR;
	struct pathgroup *pgp = NULL;
	struct path *pp = NULL;

	vector_foreach_slot (mpp->pg, pgp, j){
		vector_foreach_slot (pgp->paths, pp, i){
			if (!((pp->state == PATH_UP) ||
			      (pp->state == PATH_GHOST))){
				condlog(3, "%s: %s not available. Skip.",
					mpp->wwid, pp->dev);
				condlog(3, "%s: status = %d.",
					mpp->wwid, pp->state);
				continue;
			}

			condlog(3, "%s: sending pr in command to %s ",
				mpp->wwid, pp->dev);
			ret = prin_do_scsi_ioctl(pp->dev, rq_servact, resp,
						 noisy);
			switch(ret)
			{
				case MPATH_PR_SUCCESS:
				case MPATH_PR_ILLEGAL_REQ:
					return ret;
				default:
					continue;
			}
		}
	}
	return (ret == MPATH_PR_RETRYABLE_ERROR) ? MPATH_PR_OTHER : ret;
}

static vector curmp;
static vector pathvec;

static void __mpath_persistent_reserve_free_vecs(vector curmp, vector pathvec)
{
	free_multipathvec(curmp, KEEP_PATHS);
	free_pathvec(pathvec, FREE_PATHS);
}

void mpath_persistent_reserve_free_vecs(void)
{
	__mpath_persistent_reserve_free_vecs(curmp, pathvec);
	curmp = pathvec = NULL;
}

static int __mpath_persistent_reserve_init_vecs(vector *curmp_p,
						vector *pathvec_p, int verbose)
{
	struct config *conf = get_multipath_config();

	conf->verbosity = verbose;
	put_multipath_config(conf);

	if (*curmp_p)
		return MPATH_PR_SUCCESS;
	/*
	 * allocate core vectors to store paths and multipaths
	 */
	*curmp_p = vector_alloc ();
	*pathvec_p = vector_alloc ();

	if (!*curmp_p || !*pathvec_p){
		condlog (0, "vector allocation failed.");
		goto err;
	}

	if (dm_get_maps(*curmp_p))
		goto err;

	return MPATH_PR_SUCCESS;

err:
	__mpath_persistent_reserve_free_vecs(*curmp_p, *pathvec_p);
	*curmp_p = *pathvec_p = NULL;
	return MPATH_PR_DMMP_ERROR;
}

int mpath_persistent_reserve_init_vecs(int verbose)
{
	return __mpath_persistent_reserve_init_vecs(&curmp, &pathvec, verbose);
}

static int mpath_get_map(vector curmp, vector pathvec, int fd, char **palias,
			 struct multipath **pmpp)
{
	int ret = MPATH_PR_DMMP_ERROR;
	struct stat info;
	int major, minor;
	char *alias;
	struct multipath *mpp;

	if (fstat(fd, &info) != 0){
		condlog(0, "stat error fd=%d", fd);
		return MPATH_PR_FILE_ERROR;
	}
	if(!S_ISBLK(info.st_mode)){
		condlog(3, "Failed to get major:minor. fd=%d", fd);
		return MPATH_PR_FILE_ERROR;
	}

	major = major(info.st_rdev);
	minor = minor(info.st_rdev);
	condlog(4, "Device  %d:%d", major, minor);

	/* get alias from major:minor*/
	alias = dm_mapname(major, minor);
	if (!alias){
		condlog(0, "%d:%d failed to get device alias.", major, minor);
		return MPATH_PR_DMMP_ERROR;
	}

	condlog(3, "alias = %s", alias);

	if (dm_map_present(alias) && dm_is_mpath(alias) != 1){
		condlog(3, "%s: not a multipath device.", alias);
		goto out;
	}

	/* get info of all paths from the dm device     */
	if (get_mpvec(curmp, pathvec, alias)){
		condlog(0, "%s: failed to get device info.", alias);
		goto out;
	}

	mpp = find_mp_by_alias(curmp, alias);

	if (!mpp) {
		condlog(0, "%s: devmap not registered.", alias);
		goto out;
	}

	ret = MPATH_PR_SUCCESS;
	if (pmpp)
		*pmpp = mpp;
	if (palias) {
		*palias = alias;
		alias = NULL;
	}
out:
	FREE(alias);
	return ret;
}

static int do_mpath_persistent_reserve_in (vector curmp, vector pathvec,
	int fd, int rq_servact, struct prin_resp *resp, int noisy)
{
	struct multipath *mpp;
	int ret;

	ret = mpath_get_map(curmp, pathvec, fd, NULL, &mpp);
	if (ret != MPATH_PR_SUCCESS)
		return ret;

	ret = mpath_prin_activepath(mpp, rq_servact, resp, noisy);

	return ret;
}


int __mpath_persistent_reserve_in (int fd, int rq_servact,
	struct prin_resp *resp, int noisy)
{
	return do_mpath_persistent_reserve_in(curmp, pathvec, fd, rq_servact,
					      resp, noisy);
}

static int do_mpath_persistent_reserve_out(vector curmp, vector pathvec, int fd,
	int rq_servact, int rq_scope, unsigned int rq_type,
	struct prout_param_descriptor *paramp, int noisy)
{
	struct multipath *mpp;
	char *alias;
	int ret;
	uint64_t prkey;
	struct config *conf;

	ret = mpath_get_map(curmp, pathvec, fd, &alias, &mpp);
	if (ret != MPATH_PR_SUCCESS)
		return ret;

	conf = get_multipath_config();
	select_reservation_key(conf, mpp);
	select_all_tg_pt(conf, mpp);
	select_skip_kpartx(conf, mpp);
	put_multipath_config(conf);

	memcpy(&prkey, paramp->sa_key, 8);
	if (mpp->prkey_source == PRKEY_SOURCE_FILE && prkey &&
	    (rq_servact == MPATH_PROUT_REG_IGN_SA ||
	     (rq_servact == MPATH_PROUT_REG_SA &&
	      (!get_be64(mpp->reservation_key) ||
	       memcmp(paramp->key, &mpp->reservation_key, 8) == 0)))) {
		memcpy(&mpp->reservation_key, paramp->sa_key, 8);
		if (update_prkey_flags(alias, get_be64(mpp->reservation_key),
				       paramp->sa_flags)) {
			condlog(0, "%s: failed to set prkey for multipathd.",
				alias);
			ret = MPATH_PR_DMMP_ERROR;
			goto out1;
		}
	}

	if (memcmp(paramp->key, &mpp->reservation_key, 8) &&
	    memcmp(paramp->sa_key, &mpp->reservation_key, 8) &&
	    (prkey || rq_servact != MPATH_PROUT_REG_IGN_SA)) {
		condlog(0, "%s: configured reservation key doesn't match: 0x%" PRIx64, alias, get_be64(mpp->reservation_key));
		ret = MPATH_PR_SYNTAX_ERROR;
		goto out1;
	}

	switch(rq_servact)
	{
	case MPATH_PROUT_REG_SA:
	case MPATH_PROUT_REG_IGN_SA:
		ret= mpath_prout_reg(mpp, rq_servact, rq_scope, rq_type, paramp, noisy);
		break;
	case MPATH_PROUT_RES_SA :
	case MPATH_PROUT_PREE_SA :
	case MPATH_PROUT_PREE_AB_SA :
	case MPATH_PROUT_CLEAR_SA:
		ret = mpath_prout_common(mpp, rq_servact, rq_scope, rq_type,
					 paramp, noisy, NULL);
		break;
	case MPATH_PROUT_REL_SA:
		ret = mpath_prout_rel(mpp, rq_servact, rq_scope, rq_type, paramp, noisy);
		break;
	default:
		ret = MPATH_PR_OTHER;
		goto out1;
	}

	if ((ret == MPATH_PR_SUCCESS) && ((rq_servact == MPATH_PROUT_REG_SA) ||
				(rq_servact ==  MPATH_PROUT_REG_IGN_SA)))
	{
		if (prkey == 0) {
			update_prflag(alias, 0);
			update_prkey(alias, 0);
		} else
			update_prflag(alias, 1);
	} else if ((ret == MPATH_PR_SUCCESS) && (rq_servact == MPATH_PROUT_CLEAR_SA)) {
		update_prflag(alias, 0);
		update_prkey(alias, 0);
	}
out1:
	FREE(alias);
	return ret;
}


int __mpath_persistent_reserve_out ( int fd, int rq_servact, int rq_scope,
	unsigned int rq_type, struct prout_param_descriptor *paramp, int noisy)
{
	return do_mpath_persistent_reserve_out(curmp, pathvec, fd, rq_servact,
					       rq_scope, rq_type, paramp,
					       noisy);
}

int mpath_persistent_reserve_in (int fd, int rq_servact,
	struct prin_resp *resp, int noisy, int verbose)
{
	vector curmp = NULL, pathvec;
	int ret = __mpath_persistent_reserve_init_vecs(&curmp, &pathvec,
						       verbose);

	if (ret != MPATH_PR_SUCCESS)
		return ret;
	ret = do_mpath_persistent_reserve_in(curmp, pathvec, fd, rq_servact,
					     resp, noisy);
	__mpath_persistent_reserve_free_vecs(curmp, pathvec);
	return ret;
}

int mpath_persistent_reserve_out ( int fd, int rq_servact, int rq_scope,
	unsigned int rq_type, struct prout_param_descriptor *paramp, int noisy, int verbose)
{
	vector curmp = NULL, pathvec;
	int ret = __mpath_persistent_reserve_init_vecs(&curmp, &pathvec,
						       verbose);

	if (ret != MPATH_PR_SUCCESS)
		return ret;
	ret = do_mpath_persistent_reserve_out(curmp, pathvec, fd, rq_servact,
					      rq_scope, rq_type, paramp, noisy);
	__mpath_persistent_reserve_free_vecs(curmp, pathvec);
	return ret;
}

int
get_mpvec (vector curmp, vector pathvec, char * refwwid)
{
	int i;
	struct multipath *mpp;
	char params[PARAMS_SIZE], status[PARAMS_SIZE];

	vector_foreach_slot (curmp, mpp, i){
		/*
		 * discard out of scope maps
		 */
		if (!mpp->alias) {
			condlog(0, "%s: map with empty alias!", __func__);
			continue;
		}

		if (mpp->pg != NULL)
			/* Already seen this one */
			continue;

		if (refwwid && strncmp (mpp->alias, refwwid, WWID_SIZE - 1))
			continue;

		dm_get_map(mpp->alias, &mpp->size, params);
		condlog(3, "params = %s", params);
		dm_get_status(mpp->alias, status);
		condlog(3, "status = %s", status);
		disassemble_map (pathvec, params, mpp, 0);

		/*
		 * disassemble_map() can add new paths to pathvec.
		 * If not in "fast list mode", we need to fetch information
		 * about them
		 */
		updatepaths(mpp);
		disassemble_status (status, mpp);

	}
	return MPATH_PR_SUCCESS ;
}

/*
 * If you are changing the key registered to a device, and that device is
 * holding the reservation on a path that couldn't get its key updated,
 * either because it is down or no longer part of the multipath device,
 * you need to preempt the reservation to a usable path with the new key
 */
void preempt_missing_path(struct multipath *mpp, uint8_t *key, uint8_t *sa_key,
			  int noisy)
{
	uint8_t zero[8] = {0};
	struct prin_resp resp = {0};
	int rq_scope;
	unsigned int rq_type;
	struct prout_param_descriptor paramp = {0};
	int status;

	/*
	 * If you previously didn't have a key registered or you didn't
	 * switch to a different key, there's no need to preempt. Also, you
	 * can't preempt if you no longer have a registered key
	 */
	if (memcmp(key, zero, 8) == 0 || memcmp(sa_key, zero, 8) == 0 ||
	    memcmp(key, sa_key, 8) == 0)
		return;

	status = mpath_prin_activepath (mpp, MPATH_PRIN_RRES_SA, &resp, noisy);
	if (status != MPATH_PR_SUCCESS) {
		condlog(0, "%s: register: pr in read reservation command failed.", mpp->wwid);
		return;
	}
	/* If there is no reservation, there's nothing to preempt */
	if (!resp.prin_descriptor.prin_readresv.additional_length)
		return;
	/*
	 * If there reservation is not held by the old key, you don't
	 * want to preempt it
	 */
	if (memcmp(key, resp.prin_descriptor.prin_readresv.key, 8) != 0)
		return;
	/* Assume this key is being held by an inaccessable path on this
	 * node. libmpathpersist has never worked if multiple nodes share
	 * the same reservation key for a device
	 */
	rq_type = resp.prin_descriptor.prin_readresv.scope_type & MPATH_PR_TYPE_MASK;
	rq_scope = (resp.prin_descriptor.prin_readresv.scope_type & MPATH_PR_SCOPE_MASK) >> 4;
	memcpy(paramp.key, sa_key, 8);
	memcpy(paramp.sa_key, key, 8);
	status = mpath_prout_common(mpp, MPATH_PROUT_PREE_SA, rq_scope, rq_type,
				    &paramp, noisy, NULL);
	if (status != MPATH_PR_SUCCESS)
		condlog(0, "%s: register: pr preempt command failed.",
			mpp->wwid);
}

int mpath_prout_reg(struct multipath *mpp,int rq_servact, int rq_scope,
	unsigned int rq_type, struct prout_param_descriptor * paramp, int noisy)
{

	int i, j, k;
	struct pathgroup *pgp = NULL;
	struct path *pp = NULL;
	int rollback = 0;
	int active_pathcount=0;
	int rc;
	int count=0;
	int status = MPATH_PR_SUCCESS;
	int all_tg_pt;
	uint64_t sa_key = 0;

	if (!mpp)
		return MPATH_PR_DMMP_ERROR;

	all_tg_pt = (mpp->all_tg_pt == ALL_TG_PT_ON ||
		     paramp->sa_flags & MPATH_F_ALL_TG_PT_MASK);
	active_pathcount = pathcount(mpp, PATH_UP) + pathcount(mpp, PATH_GHOST);

	if (active_pathcount == 0) {
		condlog (0, "%s: no path available", mpp->wwid);
		return MPATH_PR_DMMP_ERROR;
	}

	struct threadinfo thread[active_pathcount];
	int hosts[active_pathcount];

	memset(thread, 0, sizeof(thread));

	/* init thread parameter */
	for (i =0; i< active_pathcount; i++){
		hosts[i] = -1;
		thread[i].param.rq_servact = rq_servact;
		thread[i].param.rq_scope = rq_scope;
		thread[i].param.rq_type = rq_type;
		thread[i].param.paramp = paramp;
		thread[i].param.noisy = noisy;
		thread[i].param.status = MPATH_PR_SKIP;

		condlog (3, "THREAD ID [%d] INFO]", i);
		condlog (3, "rq_servact=%d ", thread[i].param.rq_servact);
		condlog (3, "rq_scope=%d ", thread[i].param.rq_scope);
		condlog (3, "rq_type=%d ", thread[i].param.rq_type);
		condlog (3, "rkey=");
		condlog (3, "paramp->sa_flags =%02x ",
			 thread[i].param.paramp->sa_flags);
		condlog (3, "noisy=%d ", thread[i].param.noisy);
		condlog (3, "status=%d ", thread[i].param.status);
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	vector_foreach_slot (mpp->pg, pgp, j){
		vector_foreach_slot (pgp->paths, pp, i){
			if (!((pp->state == PATH_UP) || (pp->state == PATH_GHOST))){
				condlog (1, "%s: %s path not up. Skip.", mpp->wwid, pp->dev);
				continue;
			}
			if (all_tg_pt && pp->sg_id.host_no != -1) {
				for (k = 0; k < count; k++) {
					if (pp->sg_id.host_no == hosts[k]) {
						condlog(3, "%s: %s host %d matches skip.", pp->wwid, pp->dev, pp->sg_id.host_no);
						break;
					}
				}
				if (k < count)
					continue;
			}
			strlcpy(thread[count].param.dev, pp->dev,
				FILE_NAME_SIZE);

			if (count && (thread[count].param.paramp->sa_flags & MPATH_F_SPEC_I_PT_MASK)){
				/*
				 * Clearing SPEC_I_PT as transportids are already registered by now.
				 */
				thread[count].param.paramp->sa_flags &= (~MPATH_F_SPEC_I_PT_MASK);
			}

			condlog (3, "%s: sending pr out command to %s", mpp->wwid, pp->dev);

			rc = pthread_create(&thread[count].id, &attr, mpath_prout_pthread_fn, (void *)(&thread[count].param));
			if (rc){
				condlog (0, "%s: failed to create thread %d", mpp->wwid, rc);
				thread[count].param.status = MPATH_PR_THREAD_ERROR;
			}
			else
				hosts[count] = pp->sg_id.host_no;
			count = count + 1;
		}
	}
	for( i=0; i < count ; i++){
		if (thread[i].param.status != MPATH_PR_THREAD_ERROR) {
			rc = pthread_join(thread[i].id, NULL);
			if (rc){
				condlog (0, "%s: Thread[%d] failed to join thread %d", mpp->wwid, i, rc);
			}
		}
		if (!rollback && (thread[i].param.status == MPATH_PR_RESERV_CONFLICT)){
			rollback = 1;
			sa_key = get_unaligned_be64(&paramp->sa_key[0]);
			status = MPATH_PR_RESERV_CONFLICT ;
		}
		if (!rollback && (status == MPATH_PR_SUCCESS)){
			status = thread[i].param.status;
		}
	}
	if (rollback && ((rq_servact == MPATH_PROUT_REG_SA) && sa_key != 0 )){
		condlog (3, "%s: ERROR: initiating pr out rollback", mpp->wwid);
		memcpy(&paramp->key, &paramp->sa_key, 8);
		memset(&paramp->sa_key, 0, 8);
		for( i=0 ; i < count ; i++){
			if(thread[i].param.status == MPATH_PR_SUCCESS) {
				rc = pthread_create(&thread[i].id, &attr, mpath_prout_pthread_fn,
						(void *)(&thread[i].param));
				if (rc){
					condlog (0, "%s: failed to create thread for rollback. %d",  mpp->wwid, rc);
					thread[i].param.status = MPATH_PR_THREAD_ERROR;
				}
			} else
				thread[i].param.status = MPATH_PR_SKIP;
		}
		for(i=0; i < count ; i++){
			if (thread[i].param.status != MPATH_PR_SKIP &&
			    thread[i].param.status != MPATH_PR_THREAD_ERROR) {
				rc = pthread_join(thread[i].id, NULL);
				if (rc){
					condlog (3, "%s: failed to join thread while rolling back %d",
						 mpp->wwid, i);
				}
			}
		}
	}

	pthread_attr_destroy(&attr);
	if (status == MPATH_PR_SUCCESS)
		preempt_missing_path(mpp, paramp->key, paramp->sa_key, noisy);
	return (status == MPATH_PR_RETRYABLE_ERROR) ? MPATH_PR_OTHER : status;
}

void * mpath_prout_pthread_fn(void *p)
{
	int ret;
	struct prout_param * param = (struct prout_param *)p;

	ret = prout_do_scsi_ioctl( param->dev,param->rq_servact, param->rq_scope,
			param->rq_type, param->paramp, param->noisy);
	param->status = ret;
	pthread_exit(NULL);
}

static int mpath_prout_common(struct multipath *mpp,int rq_servact, int rq_scope,
			      unsigned int rq_type,
			      struct prout_param_descriptor* paramp, int noisy,
			      struct path **pptr)
{
	int i,j, ret;
	struct pathgroup *pgp = NULL;
	struct path *pp = NULL;
	bool found = false;

	vector_foreach_slot (mpp->pg, pgp, j){
		vector_foreach_slot (pgp->paths, pp, i){
			if (!((pp->state == PATH_UP) || (pp->state == PATH_GHOST))){
				condlog (1, "%s: %s path not up. Skip",
					 mpp->wwid, pp->dev);
				continue;
			}

			condlog (3, "%s: sending pr out command to %s", mpp->wwid, pp->dev);
			found = true;
			ret = prout_do_scsi_ioctl(pp->dev, rq_servact, rq_scope,
						  rq_type, paramp, noisy);
			if (ret == MPATH_PR_SUCCESS && pptr)
				*pptr = pp;
			if (ret != MPATH_PR_RETRYABLE_ERROR)
				return ret;
		}
	}
	if (found)
		return MPATH_PR_OTHER;
	condlog (0, "%s: no path available", mpp->wwid);
	return MPATH_PR_DMMP_ERROR;
}

int mpath_prout_rel(struct multipath *mpp,int rq_servact, int rq_scope,
	unsigned int rq_type, struct prout_param_descriptor * paramp, int noisy)
{
	int i, j;
	int num = 0;
	struct pathgroup *pgp = NULL;
	struct path *pp = NULL;
	int active_pathcount = 0;
	pthread_attr_t attr;
	int rc;
	int count = 0;
	int status = MPATH_PR_SUCCESS;
	struct prin_resp resp = {0};
	uint16_t udev_flags = (mpp->skip_kpartx)? MPATH_UDEV_NO_KPARTX_FLAG : 0;
	bool did_resume = false;
	bool all_threads_failed;

	if (!mpp)
		return MPATH_PR_DMMP_ERROR;

	active_pathcount = pathcount (mpp, PATH_UP) + pathcount (mpp, PATH_GHOST);

	if (active_pathcount == 0) {
		condlog (0, "%s: no path available", mpp->wwid);
		return MPATH_PR_DMMP_ERROR;
	}

	struct threadinfo thread[active_pathcount];
	memset(thread, 0, sizeof(thread));
	for (i = 0; i < active_pathcount; i++){
		thread[i].param.rq_servact = rq_servact;
		thread[i].param.rq_scope = rq_scope;
		thread[i].param.rq_type = rq_type;
		thread[i].param.paramp = paramp;
		thread[i].param.noisy = noisy;
		thread[i].param.status = MPATH_PR_SKIP;

		condlog (3, " path count = %d", i);
		condlog (3, "rq_servact=%d ", thread[i].param.rq_servact);
		condlog (3, "rq_scope=%d ", thread[i].param.rq_scope);
		condlog (3, "rq_type=%d ", thread[i].param.rq_type);
		condlog (3, "noisy=%d ", thread[i].param.noisy);
		condlog (3, "status=%d ", thread[i].param.status);
	}

	pthread_attr_init (&attr);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

	vector_foreach_slot (mpp->pg, pgp, j){
		vector_foreach_slot (pgp->paths, pp, i){
			if (!((pp->state == PATH_UP) || (pp->state == PATH_GHOST))){
				condlog (1, "%s: %s path not up.", mpp->wwid, pp->dev);
				continue;
			}

			strlcpy(thread[count].param.dev, pp->dev,
				FILE_NAME_SIZE);
			condlog (3, "%s: sending pr out command to %s", mpp->wwid, pp->dev);
			rc = pthread_create (&thread[count].id, &attr, mpath_prout_pthread_fn,
					(void *) (&thread[count].param));
			if (rc) {
				condlog (0, "%s: failed to create thread. %d",  mpp->wwid, rc);
				thread[count].param.status = MPATH_PR_THREAD_ERROR;
			}
			count = count + 1;
		}
	}
	pthread_attr_destroy (&attr);
	for (i = 0; i < count; i++){
		if (thread[i].param.status != MPATH_PR_THREAD_ERROR) {
			rc = pthread_join (thread[i].id, NULL);
			if (rc){
				condlog (1, "%s: failed to join thread.  %d",  mpp->wwid,  rc);
			}
		}
	}

	all_threads_failed = true;
	for (i = 0; i < count; i++){
		/*  check thread status here and return the status */

		if (thread[i].param.status == MPATH_PR_SUCCESS)
			all_threads_failed = false;
		else if (thread[i].param.status == MPATH_PR_RESERV_CONFLICT)
			status = MPATH_PR_RESERV_CONFLICT;
		else if (status == MPATH_PR_SUCCESS)
			status = thread[i].param.status;
	}
	if (all_threads_failed) {
		condlog(0, "%s: all threads failed to release reservation.",
			mpp->wwid);
		return status;
	}

	status = mpath_prin_activepath (mpp, MPATH_PRIN_RRES_SA, &resp, noisy);
	if (status != MPATH_PR_SUCCESS){
		condlog (0, "%s: pr in read reservation command failed.", mpp->wwid);
		return MPATH_PR_OTHER;
	}

	num = resp.prin_descriptor.prin_readresv.additional_length / 8;
	if (num == 0){
		condlog (2, "%s: Path holding reservation is released.", mpp->wwid);
		return MPATH_PR_SUCCESS;
	}
	if (!get_be64(mpp->reservation_key) ||
	    memcmp(&mpp->reservation_key, resp.prin_descriptor.prin_readresv.key, 8)) {
		condlog(2, "%s: Releasing key not holding reservation.", mpp->wwid);
		return MPATH_PR_SUCCESS;
	}

	condlog (2, "%s: Path holding reservation is not available.", mpp->wwid);
	/*
	 * Cannot free the reservation because the path that is holding it
	 * is not usable. Workaround this by:
	 * 1. Suspending the device
	 * 2. Preempting the reservation to move it to a usable path
	 *    (this removes the registered keys on all paths except the
	 *    preempting one. Since the device is suspended, no IO can
	 *    go to these unregistered paths and fail).
	 * 3. Releasing the reservation on the path that now holds it.
	 * 4. Resuming the device (since it no longer matters that most of
	 *    that paths no longer have a registered key)
	 * 5. Reregistering keys on all the paths
	 */

	if (!dm_simplecmd_noflush(DM_DEVICE_SUSPEND, mpp->alias, 0)) {
		condlog(0, "%s: release: failed to suspend dm device.", mpp->wwid);
		return MPATH_PR_OTHER;
	}

	memset(paramp, 0, sizeof(*paramp));
	memcpy(paramp->key, &mpp->reservation_key, 8);
	memcpy(paramp->sa_key, &mpp->reservation_key, 8);
	status = mpath_prout_common(mpp, MPATH_PROUT_PREE_SA, rq_scope, rq_type,
				    paramp, noisy, &pp);
	if (status != MPATH_PR_SUCCESS) {
		condlog(0, "%s: release: pr preempt command failed.", mpp->wwid);
		goto fail_resume;
	}

	memset(paramp, 0, sizeof(*paramp));
	memcpy(paramp->key, &mpp->reservation_key, 8);
	status = prout_do_scsi_ioctl(pp->dev, MPATH_PROUT_REL_SA, rq_scope,
				     rq_type, paramp, noisy);
	if (status != MPATH_PR_SUCCESS) {
		condlog(0, "%s: release on alternate path failed.", mpp->wwid);
		goto out_reregister;
	}

	if (!dm_simplecmd_noflush(DM_DEVICE_RESUME, mpp->alias, udev_flags)) {
		condlog(0, "%s release: failed to resume dm device.", mpp->wwid);
		/*
		 * leave status set to MPATH_PR_SUCCESS, we will have another
		 * chance to resume the device.
		 */
		goto out_reregister;
	}
	did_resume = true;

out_reregister:
	memset(paramp, 0, sizeof(*paramp));
	memcpy(paramp->sa_key, &mpp->reservation_key, 8);
	rc = mpath_prout_reg(mpp, MPATH_PROUT_REG_IGN_SA, rq_scope, rq_type,
			     paramp, noisy);
	if (rc != MPATH_PR_SUCCESS)
		condlog(0, "%s: release: failed to reregister paths.", mpp->wwid);

	/*
	 * If we failed releasing the reservation or resuming earlier
	 * try resuming now. Otherwise, return with the reregistering status
	 * This means we will report failure, even though the resevation
	 * has been released, since the keys were not reregistered.
	 */
	if (did_resume)
		return rc;
	else if (status == MPATH_PR_SUCCESS)
		status = rc;
fail_resume:
	if (!dm_simplecmd_noflush(DM_DEVICE_RESUME, mpp->alias, udev_flags)) {
		condlog(0, "%s: release: failed to resume dm device.", mpp->wwid);
		if (status == MPATH_PR_SUCCESS)
			status = MPATH_PR_OTHER;
	}
	return (status == MPATH_PR_RETRYABLE_ERROR) ? MPATH_PR_OTHER : status;
}

void * mpath_alloc_prin_response(int prin_sa)
{
	void * ptr = NULL;
	int size=0;
	switch (prin_sa)
	{
		case MPATH_PRIN_RKEY_SA:
			size = sizeof(struct prin_readdescr);
			break;
		case MPATH_PRIN_RRES_SA:
			size = sizeof(struct prin_resvdescr);
			break;
		case MPATH_PRIN_RCAP_SA:
			size=sizeof(struct prin_capdescr);
			break;
		case MPATH_PRIN_RFSTAT_SA:
			size = sizeof(struct print_fulldescr_list) +
				sizeof(struct prin_fulldescr *)*MPATH_MX_TIDS;
			break;
	}
	if (size > 0)
	{
		ptr = calloc(size, 1);
	}
	return ptr;
}

int update_map_pr(struct multipath *mpp, struct path *pp)
{
	int noisy=0;
	struct prin_resp *resp;
	unsigned int i;
	int ret = MPATH_PR_OTHER, isFound;
	bool was_set = (mpp->prflag == PRFLAG_SET);
	int verbose;
	struct config *conf;

	conf = get_multipath_config();
	verbose = conf->verbosity;
	put_multipath_config(conf);

	/* If pr is explicitly unset, it must be manually set */
	if (mpp->prflag == PRFLAG_UNSET)
		return MPATH_PR_SKIP;

	if (!get_be64(mpp->reservation_key))
	{
		/* Nothing to do. Assuming pr mgmt feature is disabled*/
		mpp->prflag = PRFLAG_UNSET;
		condlog(was_set ? 2 : 4, "%s: reservation_key not set in multipath.conf", mpp->alias);
		return MPATH_PR_SKIP;
	}

	resp = mpath_alloc_prin_response(MPATH_PRIN_RKEY_SA);
	if (!resp)
	{
		condlog(0,"%s : failed to alloc resp in update_map_pr", mpp->alias);
		return MPATH_PR_OTHER;
	}
	if (!pp && count_active_paths(mpp) == 0) {
		condlog(2, "%s: No available paths to check pr status",
			mpp->alias);
		goto out;
	}
	if (pp)
		ret = prin_do_scsi_ioctl(pp->dev, MPATH_PRIN_RKEY_SA, resp,
					 noisy);
	else
		ret = mpath_prin_activepath(mpp, MPATH_PRIN_RKEY_SA, resp,
					    noisy);

	if (ret != MPATH_PR_SUCCESS )
	{
		if (ret == MPATH_PR_ILLEGAL_REQ)
			mpp->prflag = PRFLAG_UNSET;
		condlog(0,"%s : pr in read keys service action failed Error=%d", mpp->alias, ret);
		goto out;
	}
	mpp->prflag = PRFLAG_UNSET;

	if (resp->prin_descriptor.prin_readkeys.additional_length == 0 )
	{
		condlog(was_set ? 1 : 3, "%s: No key found. Device may not be registered. ", mpp->alias);
		goto out;
	}

	condlog(3, "%s: Multipath reservation_key: 0x%" PRIx64 " ", mpp->alias,
		get_be64(mpp->reservation_key));

	isFound =0;
	for (i = 0; i < resp->prin_descriptor.prin_readkeys.additional_length/8; i++ )
	{
		if (verbose >= 3) {
			condlog(3, "%s: PR IN READKEYS[%d] reservation key:",
				mpp->alias, i);
			dumpHex((char *)&resp->prin_descriptor.prin_readkeys.key_list[i * 8], 8, 1);
		}

		if (!memcmp(&mpp->reservation_key, &resp->prin_descriptor.prin_readkeys.key_list[i * 8], 8)) {
			condlog(3, "%s: reservation key found in pr in readkeys response", mpp->alias);
			isFound =1;
		}
	}

	if (isFound)
	{
		mpp->prflag = PRFLAG_SET;
		condlog(was_set ? 3 : 2, "%s: key found. prflag set.",
			mpp->alias);
	} else
		condlog(was_set ? 1 : 3, "%s: key not found. prflag unset.",
			mpp->alias);

out:
	free(resp);
	return ret;
}
