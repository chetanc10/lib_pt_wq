
#ifndef __PTWQ_H
#define __PTWQ_H

#define _GNU_SOURCE             /* See feature_test_macros(7) */

#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#include "strum.h"
#include "list.h"

#define PT_SUCCESS 0
#define pt_true 1
#define pt_false 0

#include <syslog.h>

#if 0
#define logger syslog
#else
#define logger(lvl, fmt, ...) printf (fmt, ##__VA_ARGS__)
#endif

/**
 * @function           : pw_assert
 * @brief              : test a cond for pt_true case and if failure,
 *                       do_this action and print errstr
 * @input              : lvl - syslog level as specific by caller
 *                       expected - 0 for success or non-zero for failure
 *                       ret - return value holder
 *                       do_this - action to be taken for failure case
 *                       (errstr, ...) - err string like we use for printf
 * @output             : none
 * @return             : none
 */
#define pw_assert(lvl, expected, ret, do_this, errstr, ...) \
	if ((expected) != pt_true) { \
		logger (lvl, errstr"\n", ##__VA_ARGS__); \
		ret = !(expected); \
		do_this; \
	}

/* WARNING and ERR for syslog. Refer pw_assert for more */
#define pw_assert_warn(expected, ret, do_this, errstr, ...) \
	pw_assert (LOG_WARNING, expected, ret, do_this, errstr, ##__VA_ARGS__)
#define pw_assert_err(expected, ret, do_this, errstr, ...) \
	pw_assert (LOG_ERR, expected, ret, do_this, errstr, ##__VA_ARGS__)

/* Max number of worker pthreads allowed to be created suring pool-setup
 * Mind the ceiling value can't be > 128 */
#define MAX_PT_POOL_SIZE 32

#if (MAX_PT_POOL_SIZE > 128)
#pragma message (MAX_PT_POOL_SIZE: strum (MAX_PT_POOL_SIZE) > expected 128)
#endif

/* Max number of works in global workq - DO NOT MODIFY! */
#define MAX_GWQ_SIZE 128

/* Max number of works per worker's queue => max 512 waiting works per worker
 * TODO - advanced stuff later */
#define MAX_LWQ_SIZE (MAX_PT_POOL_SIZE << 2)

struct pwp;

typedef int (*pw_worker_fn_t) (uint64_t client_info, uint64_t arg);

/**
 * work structure used to enqueue/dequeue works for selected work queue
 * entry                - dlcl list entry linker
 * fn                   - function ptr to handle the work
 * client_info          - client info as pointer, sock-id, msg-qid, etc
 * arg                  - argument 
 * status               - work status - waiting/executing/done/failure/etc
 *                        if >= 0, check work_st_t enum
 *                        if < 0, done & failed - specific to fn why it's failed
 */
typedef struct pw_work {
	struct list_head    entry;
	uint64_t            arg;
	uint64_t            client_info;
	pw_worker_fn_t      fn;
	int                 status;
} pw_work_t;

/**
 * work queue management structure for selected work queue
 * work_arr             - pre-allocated work list with specified workq_sz
 * workq_sz             - number of works in this workq
 * mutex                - lock to manage the work pool
 * cpu_id               - cpu this queue is bould to;
 *                        -1 = unbound global workq
 *                        > 0 = cpu-bound workq
 * works_done           - number of works done on this workq
 * free_welt_idx        - free work element index in case this's bound workq
 *                        used by work producer
 * free_welt_head       - free work element head in case this's unbound workq
 *                        used by work producer
 * busy_welt_idx        - busy work element index in case this's bound workq
 *                        used by work consumer
 * busy_welt_head       - busy work element head in case this's unbound workq
 *                        used by work consumer
 */
typedef struct pw_workq_t {
	char                qname[32];
	pw_work_t           *work_arr;
	uint32_t            workq_sz;
	pthread_mutex_t     mutex;
	pthread_cond_t      cond;
	int                 cpu_id; //TODO - advanced stuff later; -1 for now
#define is_pw_workq_bound(pw_wq) (pw_wq->cpu_id != -1)
	uint32_t            works_done;
	union {
		uint32_t            free_welt_idx; //TODO - advanced stuff later
		pw_work_t           free_welt_head;
	};
	union {
		uint32_t            busy_welt_idx; //TODO - advanced stuff later
		pw_work_t           busy_welt_head;
	};
} pw_workq_t;

/* enum to define various states of any worker thread */
typedef enum pw_state_e {
	PT_INV = 0,
	PT_FREE,
	PT_BUSY,
} pw_state_t;

/**
 * pw_worker structure definition to hold each worker node details
 * entry                - dlcl list entry linker
 * state                - latest thread state with state machine updates
 * cpu_id               - cpu this queue is bould to;
 *                        -1 = unbound global workq
 *                        > 0 = cpu-bound workq
 * tid                  - pthread identifier known during thread creation
 * tidx                 - pthread index from 0 to N-1 as per order of creation
 * l_workq              - work queue array handled by this worker
 *                        if it's bound to same CPU as the work queue
 * g_workq              - global work queue array any worker can get work from
 * parent               - pw_pool to which we belong to
 * arg                  - this worker ptr for worker to know it's details 
 */
typedef struct pw_worker_s {
	struct list_head    entry; //TODO - advanced stuff later
	pw_state_t          state; //TODO - advanced stuff later
	char                tname[32];
	int                 cpu_id; //TODO - advanced stuff later; -1 for now
#define is_worker_bound(w) (w->cpu_id != -1)
	pthread_t           tid;
	int                 tidx;
	pw_workq_t          *l_workq; //TODO - advanced stuff later
	pw_workq_t          *g_workq;
	struct pwp      *parent;
	void                *arg;
} pw_worker_t;

/**
 * pwp ctx structure to hold the pool info
 * name                 - name of work queue
 * nWorkers             - number of workers to be created 
 * l_workq_sz           - number of works in worker specific bound queue
 * g_workq_sz           - number of works in any workq
 * g_workq              - unbound global workq
 * l_workq              - local workq's bound to each thread (and to CPU?)
 * workers              - array of workers moving between free/busy pools
 * pw_free_pool         - dlcl head of free workers pool
 * pw_busy_pool         - dlcl head of busy workers pool
 */
typedef struct pwp {
	char                name[32];
	uint32_t            nWorkers;
	uint32_t            l_workq_sz;
	uint32_t            g_workq_sz;
	pw_workq_t          g_workq;
	pw_workq_t          *l_workq; //TODO - advanced stuff later; unused for now
	pw_worker_t         *workers;
	pw_worker_t         pw_free_pool; //TODO - advanced stuff later
	pw_worker_t         pw_busy_pool; //TODO - advanced stuff later
} pw_pool_t;

/**
 * @function           : pw_queue_work_on
 * @brief              : add given work to cpu based bound/unbound workq
 * @input              : pw_pool_t *pq - pointer to pool-workq ctx holder
 *                       int cpu_id - cpu id to select a bound workq
 *                                    TODO: -1 for now
 *                       pw_worker_fn_t fn - function to do specific work
 *                       uint64_t arg - typecasted argument (pointer/int/float,etc)
 *                       XXX WARNING: if sizeof (data_param) > 8,
 *                                    MUST use it's address
 * @output             : depends on fn and number/type of outputs in va_list
 * @return             : > 0 on SUCCESS
 *                       < 0 on FAILURE
 */
int pw_queue_work_on (pw_pool_t *pq, int cpu_id, pw_worker_fn_t fn, uint64_t client_info, uint64_t arg);

/* queue work as unbound, let it be in global workq.
 * This is most used API to queue work without worrying about CPU affinities
 * Refer to pw_queue_work_on for further details.
 * */
#define pw_queue_work(pwp, fn, client, arg) pw_queue_work_on (pwp, -1, fn, client, arg)

/**
 * @function           : pw_remove_work
 * @brief              :
 * @input              :
 * @output             : none
 * @return             : 0 on SUCCESS
 *                       < 0 on FAILURE
 */
int pw_remove_work (/*TODO: check use-case & implement later*/);

/**
 * @function           : pw_check_work_st
 * @brief              :
 * @input              :
 * @output             : none
 * @return             : 0 on SUCCESS
 *                       < 0 on FAILURE
 */
int pw_check_work_st (/*TODO: check use-case & implement later*/);



/**
 * @function           : pw_setup_pool
 * @brief              : setup pthread pool with requested number of pthreads
 * @input              : char *name - name of the context for which
 *                                    workq's and worker pool to be setup
 *                       uint32_t nWorkers - number of worker pthreads
 *                       uint32_t l_workq_sz - works per worker's local workq
 *                       uint32_t g_workq_sz - works in global workq
 * @output             : pw_pool_t *pwp - pool holder
 * @return             : 0 on Success
 *                       -errno on Failure
 */
int pw_setup_pool (char *name, uint32_t nWorkers, uint32_t l_workq_sz, uint32_t g_workq_sz, pw_pool_t *pwp);

/**
 * @function           : pw_destroy_pool
 * @brief              : destroy an already allocated pthread pool
 * @input              : pw_pool_t *pwp - pool holder
 * @output             : none
 * @return             : none
 */
void pw_destroy_pool (pw_pool_t *pwp);

#endif /*__PTWQ_H*/

