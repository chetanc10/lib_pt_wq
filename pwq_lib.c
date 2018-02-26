
#define _GNU_SOURCE             /* See feature_test_macros(7) */

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "pwq.h"

/*#include "wlc_log.h"*/
/* TODO: When integrating to wlc, include wlc_log.h and 
 * remove macro definition below */
#include <syslog.h>
#define wlc_log syslog

/**
 * @function           : pw_assert
 * @brief              : test a cond for PT_SUCCESS case and if failure,
 *                       do_this action and print errstr
 * @input              : lvl - syslog level as specific by caller
 *                       cond - 0 for success or non-zero for failure
 *                       ret - return value holder
 *                       do_this - action to be taken for failure case
 *                       (errstr, ...) - err string like we use for printf
 * @output             : none
 * @return             : none
 */
#define pw_assert(lvl, cond, ret, do_this, errstr, ...) \
	if ((ret = (cond)) != PT_SUCCESS) { \
		wlc_log (lvl, errstr, ##__VA_ARGS__); \
		do_this; \
	}

#define pw_assert_warn(cond, ret, do_this, errstr, ...) \
	pw_assert (LOG_WARNING, cond, ret, do_this, errstr, ##__VA_ARGS__)

#define pw_assert_err(cond, ret, do_this, errstr, ...) \
	pw_assert (LOG_ERR, cond, ret, do_this, errstr, ##__VA_ARGS__)

/**
 * @function           : pw_worker_thread_fn
 * @brief              : waits till it's workq signalled new work addition
 *                       dequeues the work if it wins mutex, does the work
 *                       and stores the staus and goes back to wait
 * @input              : void *arg - pointer this worker context
 * @output             : none
 * @return             : arg with latest members for killer/waiter to process
 */
static inline pw_work_t *_pw_dequeue_wq (pw_workq_t *wq)
{
	int ret = 0;
	pw_work_t *w;

	pw_assert_err (pthread_mutex_lock (&wq->mutex) == 0, \
			ret, return NULL, "wq-mutex: %s", strerror (errno));
	pw_assert_err (pthread_cond_wait (&wq->cond, &wq->mutex) == 0, \
			ret, return NULL, "wq-cond: %s", strerror (errno));
	if (!list_empty (&wq->free_welt_head.entry)) {
		/* dequeue from first as enqueue happens from last */
		w = list_first_entry (&wq->free_welt_head.entry, pw_work_t, entry);
	}
	pthread_mutex_unlock (&wq->mutex);

	return w;
}

/**
 * @function           : pw_worker_thread_fn
 * @brief              : waits till it's workq signalled new work addition
 *                       dequeues the work if it wins mutex, does the work
 *                       and stores the staus and goes back to wait
 * @input              : void *arg - pointer this worker context
 * @output             : none
 * @return             : arg with latest members for killer/waiter to process
 */
void *pw_worker_thread_fn (void *arg)
{
	pw_worker_t *worker = (pw_worker_t *)arg;
	pw_work_t *w;
	int ret = 0;
	char tname[64];

	sprintf (tname, "%s_%d", worker->parent->name, worker->tidx);

	if (is_worker_bound (worker)) {
		cpu_set_t cps;
		ret = pthread_setaffinity_np (worker->tid, sizeof (cpu_set_t), &cps);
		pw_assert_warn (ret, ret, ret = 0, \
				"binding worker %s to cpu: %d.. defaulting to -1\n", \
				tname, worker->cpu_id);
		worker->cpu_id = -1;
	}

	wlc_log (LOG_NOTICE, "Worker thread started... pthreadid: %lu " \
			"cpu: %d worker_idx: %d pool: %s\n", \
			worker->tid, worker->cpu_id, worker->tidx, worker->parent->name);

	while (1) { /* TODO: do better MnC of all these threads */
		/*TODO: handle local q also later.. do global workq for now */
		while ((w = _pw_dequeue_wq (worker->g_workq)) != NULL) {
			wlc_log (LOG_DEBUG, "thread: %s coudln't get work!", tname);
		}
		/* process the work now */
		w->status = w->fn (w->work_code, w->client_info, w->varp);
		/* now that the work's handled, move the node to free list */
		list_del (&w->entry);
		list_add_tail (&w->entry, &worker->g_workq->free_welt_head.entry);
	}

	pthread_exit (arg);
}

int pw_queue_work_on (pw_pool_t *pq, int cpu_id, int work_code, pw_worker_fn_t fn, uint64_t client_info, ...)
{
	int ret = 0;
	va_list varp;
	pw_workq_t *wq = NULL;
	pw_work_t *w = NULL;

	pw_assert_err (work_code != 0, ret, return ret, \
			"work_code can never be 0!");
#if 1
	pw_assert_err (pq != NULL, ret, return ret, \
			"pool-work-queue holder 'pq' can never be NULL!");
	/* Once integration test's completed for new app features, define as 0 */
	pw_assert_err (fn != NULL, ret, return ret, \
			"work fn not specified for work_code: %d", work_code);
	pw_assert_err (client_info != 0, ret, return ret, \
			"client_info not specified for work_code: %d", work_code);
#endif

	/*TODO: handle specific cpu ids later*/
	pw_assert_warn (cpu_id != -1, ret, \
			cpu_id = -1 /*make it -1 for now*/, "Invalid cpu_id: %d", cpu_id);

	wq = &pq->g_workq;

	va_start (varp, client_info);
	pthread_mutex_lock (&wq->mutex);
	pw_assert_err (!list_empty (&wq->free_welt_head.entry), \
			ret, goto release, "No free node to queue work!");

	w = list_first_entry (&wq->free_welt_head.entry, pw_work_t, entry);

	/*Got it! Update workq element with the given details*/
	w->work_code = work_code;
	va_copy (w->varp, varp);
	w->client_info = client_info;
	w->fn = fn;
	w->status = 0;

	list_del (&w->entry);
	list_add_tail (&w->entry, &wq->free_welt_head.entry);

	/*Signal on this wq that new work is available*/
	pthread_cond_signal (&wq->cond);

release:
	pthread_mutex_unlock (&wq->mutex);
	va_end (varp);

	return ret;
}

/**
 * @function           : __pw_destroy_worker
 * @brief              : destroys a worker context and kills the thread
 * @input              : pw_worker_t *worker - worker context pointer
 * @output             : none
 * @return             : none
 */
static inline void __pw_destroy_worker (pw_worker_t *worker)
{
	pthread_kill (worker->tid, SIGKILL);

	memset (worker, 0, sizeof (*worker));
}

/**
 * @function           : _pw_destroy_workers
 * @brief              : destroys all workers context in the pool
 * @input              : pw_pool_t *pwp - pw pool context pointer
 * @output             : none
 * @return             : none
 */
static inline void _pw_destroy_workers (pw_pool_t *pwp)
{
	int i = 0;

	if (!pwp->workers) return;

	for (i = 0; i < pwp->nWorkers; i++) {
		__pw_destroy_worker (pwp->workers + i);
	}

	free (pwp->workers);
}

/**
 * @function           : __pw_destroy_workq
 * @brief              : destroys a workq context in the pool
 * @input              : pw_pool_t *wq - workq context pointer
 * @output             : none
 * @return             : none
 */
static inline void __pw_destroy_workq (pw_workq_t *wq)
{
	if (!wq) return;

	free (wq->work_arr);
	pthread_mutex_destroy (&wq->mutex);
	pthread_cond_destroy (&wq->cond);

	memset (wq, 0, sizeof (*wq));
}

/**
 * @function           : _pw_destroy_l_workqs
 * @brief              : later!
 * @input              : pw_pool_t *pwp - pw pool context pointer
 * @output             : none
 * @return             : none
 */
static inline void _pw_destroy_l_workqs (pw_pool_t *pwp)
{
	return;// TODO: One day, this will be in the game!
}

/**
 * @function           : _pw_setup_worker
 * @brief              : setup a worker thread context and instantiate it
 * @input              : pw_pool_t *pwp - to link with parent
 *                       int tidx - index of worker assigned by pw-setup
 * @output             : none
 * @return             : 0 for SUCCESS
 *                       < 0 for FAILURE
 */
static inline int _pw_setup_worker (pw_pool_t *pwp, int tidx)
{
	int ret = 0;
	pw_worker_t *worker = pwp->workers + tidx;

	memset (worker, 0, sizeof (*worker));

	worker->cpu_id = -1;
	worker->tidx = tidx;
	/*TODO: what about l_workq? later...*/
	worker->g_workq = &pwp->g_workq;
	worker->parent = pwp;
	worker->arg = worker;

	pw_assert_err (pthread_create (&worker->tid, NULL, \
				pw_worker_thread_fn, worker), \
			ret, memset (worker, 0, sizeof (*worker)), \
			"pthread: %s", strerror (errno));

	return ret;
}

/**
 * @function           : _pw_setup_worker
 * @brief              : setup a worker thread context and instantiate it
 * @input              : pw_pool_t *pwp - to link with parent
 *                       uint32_t workq_sz - number of work elements in queue
 * @output             : pw_workq_t wq - workq ptr to be allocated & updated
 * @return             : 0 for SUCCESS
 *                       < 0 for FAILURE
 */
static inline int __pw_setup_workq (pw_workq_t *wq, pw_pool_t *pwp, uint32_t workq_sz)
{
	int ret = 0;
	int i = 0;
	pw_work_t *w = NULL;
	struct list_head *head;

	pw_assert_err (pthread_cond_init (&wq->cond, NULL), \
			ret, return ret, "wq-cond: %s", strerror (errno));

	pw_assert_err (pthread_mutex_init (&wq->mutex, NULL), \
			ret, goto clean_cond, "wq-mutex: %s", strerror (errno));

	w = calloc (workq_sz, sizeof (*w));
	pw_assert_err (w != NULL, ret, goto clean_mutex, \
			"calloc w: %s", strerror (errno));
	wq->work_arr = w;
	head = &wq->free_welt_head.entry;
	INIT_LIST_HEAD (head);
	for (i = 0; i < workq_sz; i++) {
		list_add_tail (&w[i].entry, head);
	}

	INIT_LIST_HEAD (&wq->busy_welt_head.entry);

	wq->cpu_id = -1;
	wq->works_done = 0;
	wq->workq_sz = workq_sz;

	return ret;

clean_mutex:
	pthread_mutex_destroy (&wq->mutex);
clean_cond:
	pthread_cond_destroy (&wq->cond);

	return ret;
}

/**
 * @function           : _pw_setup_l_workqs
 * @brief              : later
 * @input              : pw_pool_t *pwp - to link with parent
 *                       uint32_t workq_sz - number of work elements in queue
 * @output             : updated pwp->l_workq with setup queue info
 * @return             : 0 for SUCCESS
 *                       < 0 for FAILURE
 */
static inline int _pw_setup_l_workqs (pw_pool_t *pwp, uint32_t workq_sz)
{
	int ret = 0;
	/*TODO: return 0 as l_workq is not used elsewhere now*/
	return ret;
}

int pw_setup_pool (char *name, uint32_t nWorkers, uint32_t l_workq_sz, uint32_t g_workq_sz, pw_pool_t *pwp)
{
	int ret = 0;
	int i = 0;
	pw_worker_t *workers;

	pw_assert_err ((!name || !strlen(name)), \
			ret, return ret, "Need valid pool name");
	pw_assert_err ((!pwp), ret, return ret, "Need valid pool pointer");
	pw_assert_err ((!nWorkers || (nWorkers > MAX_PT_POOL_SIZE)), \
			ret, return ret, \
			"nWorkers: %d; expected range 0 < nWorkers < %u", \
			nWorkers, MAX_PT_POOL_SIZE);
	pw_assert_err ((!g_workq_sz || (g_workq_sz > MAX_GWQ_SIZE)), \
			ret, return ret, \
			"g_workq_sz: %d; expected range 0 < g_workq_sz < %u", \
			g_workq_sz, MAX_GWQ_SIZE);
	pw_assert_err ((!l_workq_sz || (l_workq_sz > MAX_LWQ_SIZE)), \
			ret, return ret, \
			"l_workq_sz: %d; expected range 0 < l_workq_sz < %u", \
			l_workq_sz, MAX_LWQ_SIZE);

	memset (pwp, 0, sizeof (*pwp));
	strcpy (pwp->name, name);

	pwp->g_workq_sz = g_workq_sz;
	pw_assert_err (__pw_setup_workq (&pwp->g_workq, pwp, g_workq_sz), \
			ret, goto release_pw, "Failed g_workq setup!");
	pwp->l_workq_sz = l_workq_sz;
	pw_assert_err (_pw_setup_l_workqs (pwp, l_workq_sz), \
			ret, goto release_pw, "Failed l_workqs setup!");
	
	pwp->nWorkers = nWorkers;
	workers = malloc (nWorkers * sizeof (*workers));
	pw_assert_err ((workers != NULL), ret, goto release_pw, \
			"malloc %d workers for %s: %s", nWorkers, name, strerror (errno));
	pwp->workers = workers;
	for (i = 0;	i < nWorkers; i++) {
		if (_pw_setup_worker (pwp, i) != PT_SUCCESS) {
			goto release_pw;
		}
	}

release_pw:
	if (ret != PT_SUCCESS) {
		pw_destroy_pool (pwp);
	}

	return ret;
}

void pw_destroy_pool (pw_pool_t *pwp)
{
	_pw_destroy_workers (pwp);
	__pw_destroy_workq (&pwp->g_workq);
	_pw_destroy_l_workqs (pwp);
	memset (pwp, 0, sizeof (*pwp));
}

