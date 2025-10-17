// File:	worker_t.h

// List all group member's name:
// username of iLab:
// iLab Server:

#ifndef WORKER_T_H
#define WORKER_T_H

#define _GNU_SOURCE

/* To use Linux pthread Library in Benchmark, you have to comment the USE_WORKERS macro */
//#define USE_WORKERS 1

/* Targeted latency in milliseconds */
#define TARGET_LATENCY   20  

/* Minimum scheduling granularity in milliseconds */
#define MIN_SCHED_GRN    1

/* Time slice quantum in milliseconds */
#define QUANTUM 10

/* include lib header files that you need here: */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

typedef uint worker_t;

typedef enum s { 
    READY = 0,
    RUNNING = 1,
    BLOCKED = 2
} status;

typedef struct TCB {
	int tID;
	status state;
	ucontext_t context;
	void* stack;
	int priority;
    int pc;
    struct TCB* next;
    void* retValue;
} tcb; 

/* mutex struct definition */
typedef struct worker_mutex_t {
	/* add something here */

	// YOUR CODE HERE
} worker_mutex_t;

/* define your data structures here: */
// Feel free to add your own auxiliary data structures (linked list or queue etc...)

typedef struct RQ {
    tcb* head;
    tcb* tail;
    int size;
} runQueue;

//runqueue functions
void enqueue(runQueue* rq, tcb* node){
    if(rq->head == NULL){
        rq->head = node;
    }
    else{
        rq->tail->next = node;
        rq->tail = node;
    }
    rq->size++;
}

tcb* dequeue(runQueue* rq){
    if(rq->head == NULL)
    {
        return NULL;
    }
    tcb* node = rq->head;
    rq->head = rq->head->next;
    if(rq->head == NULL)
    {
        rq->tail = NULL;
    }
    node->next = NULL;
    rq->size--;
    return node;
}

tcb* removeNode(runQueue *rq, int tID) {
    if (rq->head == NULL) return NULL;
    tcb* prev = NULL;
    tcb* curr = rq->head;
    while (curr != NULL) {
        if (curr->tID == tID) {
            if (prev == NULL) {
                rq->head = curr->next;
            } else {
                prev->next = curr->next;
            }
            if (curr == rq->tail) {
                rq->tail = prev;
            }
            curr->next = NULL;
            rq->size--;
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }
    return NULL;
}

/* Function Declarations: */

/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, void
    *(*function)(void*), void * arg);

/* give CPU pocession to other user level worker threads voluntarily */
int worker_yield();

/* terminate a thread */
void worker_exit(void *value_ptr);

/* wait for thread termination */
int worker_join(worker_t thread, void **value_ptr);

/* initial the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t
    *mutexattr);

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex);

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex);

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex);


/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void);

#ifdef USE_WORKERS
#define pthread_t worker_t
#define pthread_mutex_t worker_mutex_t
#define pthread_create worker_create
#define pthread_exit worker_exit
#define pthread_join worker_join
#define pthread_mutex_init worker_mutex_init
#define pthread_mutex_lock worker_mutex_lock
#define pthread_mutex_unlock worker_mutex_unlock
#define pthread_mutex_destroy worker_mutex_destroy
#endif

#endif
