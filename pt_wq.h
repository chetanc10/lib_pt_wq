
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

struct pw_pool;

typedef int (*pt_worker_fn_t) (int work_code, uint64_t client_info, va_list varp);

/**
 * work structure used to enqueue/dequeue works for selected work queue
 * entry                - dlcl list entry linker
 * work_code            - defined by application and known to fn to do some work
 * fn                   - function ptr to handle the work
 * client_info          - client info as pointer, sock-id, msg-qid, etc
 * varp                 - va_list type of argument pointer list
 * status               - work status - waiting/executing/done/failure/etc
 *                        if >= 0, check work_st_t enum
 *                        if < 0, done & failed - specific to fn why it's failed
 */
typedef struct pt_work {
	struct list_head    entry;
	int                 work_code;
	va_list             varp;
	uint64_t            client_info;
	pt_worker_fn_t      fn;
	int                 status;
} pt_work_t;

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
typedef struct pt_workq_t {
	pt_work_t           *work_arr;
	uint32_t            workq_sz;
	pthread_mutex_t     mutex;
	pthread_cond_t      cond;
	int                 cpu_id; //TODO - advanced stuff later; -1 for now
#define is_pt_workq_bound(pt_wq) (pt_wq->cpu_id != -1)
	uint32_t            works_done;
	union {
		uint32_t            free_welt_idx; //TODO - advanced stuff later
		pt_work_t           free_welt_head;
	};
	union {
		uint32_t            busy_welt_idx; //TODO - advanced stuff later
		pt_work_t           busy_welt_head;
	};
} pt_workq_t;

/* enum to define various states of any worker thread */
typedef enum pt_state_e {
	PT_INV = 0,
	PT_FREE,
	PT_BUSY,
} pt_state_t;

/**
 * pt_worker structure definition to hold each worker node details
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
 * parent               - pt_pool to which we belong to
 * arg                  - this worker ptr for worker to know it's details 
 */
typedef struct pt_worker_s {
	struct list_head    entry; //TODO - advanced stuff later
	pt_state_t          state; //TODO - advanced stuff later
	int                 cpu_id; //TODO - advanced stuff later; -1 for now
#define is_worker_bound(w) (w->cpu_id != -1)
	pthread_t           tid;
	int                 tidx;
	pt_workq_t          *l_workq; //TODO - advanced stuff later
	pt_workq_t          *g_workq;
	struct pw_pool      *parent;
	void                *arg;
} pt_worker_t;

/**
 * pw_pool ctx structure to hold the pool info
 * name                 - name of work queue
 * nWorkers             - number of workers to be created 
 * l_workq_sz           - number of works in worker specific bound queue
 * g_workq_sz           - number of works in any workq
 * g_workq              - unbound global workq
 * l_workq              - local workq's bound to each thread (and to CPU?)
 * workers              - array of workers moving between free/busy pools
 * pt_free_pool         - dlcl head of free workers pool
 * pt_busy_pool         - dlcl head of busy workers pool
 */
typedef struct pw_pool {
	char                name[32];
	uint32_t            nWorkers;
	uint32_t            l_workq_sz;
	uint32_t            g_workq_sz;
	pt_workq_t          g_workq;
	pt_workq_t          *l_workq; //TODO - advanced stuff later; unused for now
	pt_worker_t         *workers;
	pt_worker_t         pt_free_pool; //TODO - advanced stuff later
	pt_worker_t         pt_busy_pool; //TODO - advanced stuff later
} pw_pool_t;

/**
 * @function           : pt_queue_work
 * @brief              : add given work to cpu based bound/unbound workq
 * @input              : pw_pool_t *pq - pointer to pool-workq ctx holder
 *                       int cpu_id - cpu id to select a bound workq
 *                                    TODO: -1 for now
 *                       int work_code - work code for the fn confirm
 *                                       the given work type // TODO
 *                       pt_worker_fn_t fn - function to do specific work
 *                                           as per work_code
 *                       ... - va_list inputs to process and take action
 * @output             : depends on fn and number/type of outputs in va_list
 * @return             : > 0 on SUCCESS
 *                       < 0 on FAILURE
 */
int pt_queue_work_on (pw_pool_t *pq, int cpu_id, int work_code, pt_worker_fn_t fn, uint64_t client_info, ...);

/* queue work as unbound, let it be in global workq.
 * Refer to pt_queue_work_on
 * */
#define pt_queue_work(wc, fn, client, ...) pt_queue_work_on (-1, wc, fn, client, ##__VA_ARGS__)

/**
 * @function           : pt_remove_work
 * @brief              :
 * @input              :
 * @output             : none
 * @return             : 0 on SUCCESS
 *                       < 0 on FAILURE
 */
int pt_remove_work (/*TODO: check use-case & implement later*/);

/**
 * @function           : pt_check_work_st
 * @brief              :
 * @input              :
 * @output             : none
 * @return             : 0 on SUCCESS
 *                       < 0 on FAILURE
 */
int pt_check_work_st (/*TODO: check use-case & implement later*/);



/**
 * @function           : pt_setup_pool
 * @brief              : setup pthread pool with requested number of pthreads
 * @input              : char *name - name of the context for which
 *                                    workq's and worker pool to be setup
 *                       uint32_t nWorkers - number of worker pthreads
 *                       uint32_t l_workq_sz - works per worker's local workq
 *                       uint32_t g_workq_sz - works in global workq
 * @output             : pw_pool_t *pw_pool - pool holder
 * @return             : 0 on Success
 *                       -errno on Failure
 */
int pt_setup_pool (char *name, uint32_t nWorkers, uint32_t l_workq_sz, uint32_t g_workq_sz, pw_pool_t *pw_pool);

/**
 * @function           : pt_destroy_pool
 * @brief              : destroy an already allocated pthread pool
 * @input              : pw_pool_t *pw_pool - pool holder
 * @output             : none
 * @return             : none
 */
void pt_destroy_pool (pw_pool_t *pw_pool);

#endif /*__PTWQ_H*/

