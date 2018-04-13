/**
 * threadpool.c
 *
 * This file will contain your implementation of a threadpool.
 */

 // followed a tutorial na khrub

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "threadpool.h"

// _threadpool is the internal threadpool structure that is
// cast to type "threadpool" before it given out to callers

typedef struct work_st{
	void (*routine) (void*);
	void * arg;
	struct work_st* next;
} work_t;

typedef struct _threadpool_st {
	//we'll be using queue for this
   // you should fill in this structure with whatever you need
	int threads_act; //active threads
	int size;			//queue size
	pthread_t *threads;
	work_t* head;	//queue head
	work_t* tail;		//queue tail
	pthread_mutex_t lock_q;	//queue lock
	pthread_cond_t non_empt_q; //check queue empty
	pthread_cond_t empt_q; //check queue empty
	int shutdown;
	int reject;
	int rem_threads; //remaning threads
} _threadpool;

/* This function is the work function of the thread */
void* worker_thread(threadpool p) {
	_threadpool * pool = (_threadpool *) p;
	work_t* cur;

	while(1) {
		// wait for a signal
		// l
		// mark itself as busy
		// run a given function
		//

		pool->size = pool->size;
		pthread_mutex_lock(&(pool->lock_q));

		if (pool->size == 0){
			pthread_cond_wait(&(pool->non_empt_q), &(pool->lock_q));
		}


		while( pool->size == 0) {
			if(pool->shutdown) {
				pthread_mutex_unlock(&(pool->lock_q));
				pthread_exit(NULL);
			}
		}

		pthread_mutex_unlock(&(pool->lock_q));

		cur = pool->head;

		pool->size--;
		pool->rem_threads++;

		if(pool->size == 0) {
			pool->head = NULL;
			pool->tail = NULL;
		}
		else pool->head = cur->next;

		if(pool->size == 0 && ! pool->shutdown) pthread_cond_signal(&(pool->empt_q));
		pthread_mutex_unlock(&(pool->lock_q));
		(cur->routine) (cur->arg);
		pthread_mutex_lock(&(pool->lock_q));

		free(cur);

		if (pool->rem_threads == 1){
			pthread_mutex_unlock(&(pool->lock_q));
			pthread_cond_signal(&(pool->empt_q));
		}
		else pthread_mutex_unlock(&(pool->lock_q));
	}
}

threadpool create_threadpool(int num_threads_in_pool) {
  _threadpool *pool;

  if ((num_threads_in_pool <= 0) || (num_threads_in_pool > MAXT_IN_POOL)) return NULL;

  pool = (_threadpool *) malloc(sizeof(_threadpool));
  if (pool == NULL) {
    fprintf(stderr, "Cant create threadpool\n");
    return NULL;
  }

  pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads_in_pool);

  if (!pool->threads){
    fprintf(stderr, "Cant create threadpool\n");
    return NULL;
  }

  pool->head = NULL;
  pool->tail = NULL;
  pool->size = 0;
  pool->reject = 0;
  pool->shutdown  = 0;
  pool->threads_act = num_threads_in_pool;
  pool->rem_threads = num_threads_in_pool;

  if (pthread_cond_init(&pool->empt_q, NULL)){
    fprintf(stderr, "CV initiation error\n");
    return NULL;
  }
  if (pthread_mutex_init(&pool->lock_q, NULL)){
    fprintf(stderr, "Mutex error\n");
    return NULL;
  }
  if (pthread_cond_init(&pool->non_empt_q, NULL)){
    fprintf(stderr, "CV initiation error\n");
    return NULL;
  }

  for (int i = 0; i < pool->threads_act;i++){
    if (pthread_create(&(pool->threads[i]), NULL, worker_thread, pool)){
      fprintf(stderr, "Thread couldn't initialize\n");
      return NULL;
    }
  }
  return (threadpool) pool;
}


void dispatch(threadpool from_me, dispatch_fn dispatch_to_here, void *arg) {
  _threadpool *pool = (_threadpool *) from_me;

	// add your code here to dispatch a thread
	work_t *cur;

	//make a work queue element.
	cur = (work_t*) malloc(sizeof(work_t));
	if(cur == NULL) {
		fprintf(stderr, "Out of memory creating a work struct!\n");
		return;
	}

	cur->routine = dispatch_to_here;
	cur->arg = arg;
	cur->next = NULL;

	pthread_mutex_lock(&(pool->lock_q));

	if(pool->reject) {
		free(cur);
		return;
	}
	if(pool->size == 0) {
		pool->head = cur;
		pool->tail = cur;
	} else {
		pool->tail->next = cur;
		pool->tail = cur;
	}
	pool->size++;
	pool->rem_threads--;

	pthread_mutex_unlock(&(pool->lock_q));
	pthread_cond_signal(&(pool->non_empt_q));
	pthread_mutex_lock(&(pool->lock_q));

	if (pool->rem_threads == 0) pthread_cond_wait(&(pool->empt_q), &(pool->lock_q));
	pthread_mutex_unlock(&(pool->lock_q));
}

void destroy_threadpool(threadpool destroyme) {
	_threadpool *pool = (_threadpool *) destroyme;

	// add your code here to kill a threadpool
	free(pool->threads);
	pthread_mutex_destroy(&(pool->lock_q));
	pthread_cond_destroy(&(pool->non_empt_q));
	pthread_cond_destroy(&(pool->empt_q));
	return;
}
