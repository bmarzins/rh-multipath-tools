/*
 * Copyright (c) 2005 Christophe Varoqui
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/mman.h>

#include "memory.h"

#include "log_pthread.h"
#include "log.h"
#include "lock.h"
#include "util.h"

static pthread_t log_thr;

static pthread_mutex_t logq_lock;
static pthread_mutex_t logev_lock;
static pthread_cond_t logev_cond;

static int logq_running;
static int log_messages_pending;

void log_safe (int prio, const char * fmt, va_list ap)
{
	if (prio > LOG_DEBUG)
		prio = LOG_DEBUG;

	if (log_thr == (pthread_t)0) {
		vsyslog(prio, fmt, ap);
		return;
	}

	pthread_mutex_lock(&logq_lock);
	log_enqueue(prio, fmt, ap);
	pthread_mutex_unlock(&logq_lock);

	pthread_mutex_lock(&logev_lock);
	log_messages_pending = 1;
	pthread_cond_signal(&logev_cond);
	pthread_mutex_unlock(&logev_lock);
}

static void flush_logqueue (void)
{
	int empty;

	do {
		pthread_mutex_lock(&logq_lock);
		empty = log_dequeue(la->buff);
		pthread_mutex_unlock(&logq_lock);
		if (!empty)
			log_syslog(la->buff);
	} while (empty == 0);
}

static void cleanup_log_thread(__attribute((unused)) void *arg)
{
	logdbg(stderr, "log thread exiting");
	pthread_mutex_lock(&logev_lock);
	logq_running = 0;
	pthread_mutex_unlock(&logev_lock);
}

static void * log_thread (__attribute__((unused)) void * et)
{
	int running;

	pthread_mutex_lock(&logev_lock);
	running = logq_running;
	if (!running)
		logq_running = 1;
	pthread_cond_signal(&logev_cond);
	pthread_mutex_unlock(&logev_lock);
	if (running)
		/* already started */
		return NULL;
	pthread_cleanup_push(cleanup_log_thread, NULL);

	mlockall(MCL_CURRENT | MCL_FUTURE);
	logdbg(stderr,"enter log_thread\n");

	while (1) {
		pthread_mutex_lock(&logev_lock);
		pthread_cleanup_push(cleanup_mutex, &logev_lock);
		while (!log_messages_pending)
			/* this is a cancellation point */
			pthread_cond_wait(&logev_cond, &logev_lock);
		log_messages_pending = 0;
		pthread_cleanup_pop(1);

		flush_logqueue();
	}
	pthread_cleanup_pop(1);
	return NULL;
}

void log_thread_start (pthread_attr_t *attr)
{
	bool err = false;

	logdbg(stderr,"enter log_thread_start\n");

	if (log_init("multipathd", 0)) {
		fprintf(stderr,"can't initialize log buffer\n");
		exit(1);
	}

	pthread_mutex_lock(&logev_lock);
	pthread_cleanup_push(cleanup_mutex, &logev_lock);
	if (pthread_create(&log_thr, attr, log_thread, NULL)) {
		err = true;
	}

	/* wait for thread startup */
	while (!logq_running)
		pthread_cond_wait(&logev_cond, &logev_lock);
	pthread_cleanup_pop(1);

	if (err) {
		fprintf(stderr,"can't start log thread\n");
		exit(1);
	}

	return;
}

void log_thread_reset (void)
{
	logdbg(stderr,"resetting log\n");

	pthread_mutex_lock(&logq_lock);
	log_reset("multipathd");
	pthread_mutex_unlock(&logq_lock);
}

void log_thread_stop (void)
{
	int running;

	if (!la)
		return;

	logdbg(stderr,"enter log_thread_stop\n");

	pthread_mutex_lock(&logev_lock);
	pthread_cleanup_push(cleanup_mutex, &logev_lock);
	running = logq_running;
	if (running) {
		pthread_cancel(log_thr);
		pthread_cond_signal(&logev_cond);
	}
	pthread_cleanup_pop(1);

	if (running)
		pthread_join(log_thr, NULL);


	flush_logqueue();
	log_close();
}
