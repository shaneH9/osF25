// File:	worker_t.h

// List all group member's name:
// username of iLab:
// iLab Server:

#ifndef WORKER_T_H
#define WORKER_T_H

#define _GNU_SOURCE

/* To use Linux pthread Library in Benchmark, you have to comment the USE_WORKERS macro */
// #define USE_WORKERS 1

/* Targeted latency in milliseconds */
#define TARGET_LATENCY 20

/* Minimum scheduling granularity in milliseconds */
#define MIN_SCHED_GRN 1

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

typedef enum s
{
    READY = 0,
    RUNNING = 1,
    BLOCKED = 2,
    FINISHED = 3
} status;

typedef struct TCB
{
    int tID;
    status state;
    ucontext_t context;
    void *stack;
    int priority;
    int pc;
    struct TCB *next;
    void *retValue;
    long timeQuant;
} tcb;

/* define your data structures here: */
// Feel free to add your own auxiliary data structures (linked list or queue etc...)

typedef struct MH
{
    tcb **arr;
    int threads;
    int threshold;
} minHeap;

/* mutex struct definition */
typedef struct worker_mutex_t
{
    int locked;
    struct worker_mutex_t *next;
    minHeap blockList;
} worker_mutex_t;

int heapResize(minHeap *h)
{
    int doubleThreshold = h->threshold * 2;
    tcb **newArr = realloc(h->arr, doubleThreshold * sizeof(tcb *));
    if (!newArr)
    {
        return -1;
    }

    h->arr = newArr;
    h->threshold = doubleThreshold;
    return 0;
}

int enqueue(minHeap *h, tcb *node)
{
    if (h->threads == h->threshold)
    {
        if (heapResize(h) == -1)
        {
            return -1;
        }
    }

    int idx = h->threads;
    h->arr[idx] = node;

    while (idx > 0)
    {
        int parent = (idx - 1) / 2;
        if (h->arr[idx]->timeQuant >= h->arr[parent]->timeQuant)
        {
            break;
        }

        tcb *temp = h->arr[idx];
        h->arr[idx] = h->arr[parent];
        h->arr[parent] = temp;

        idx = parent;
    }

    h->threads++;
    return 0;
}

tcb *dequeue(minHeap *h)
{
    if (h->threads == 0)
        return NULL;

    tcb *minNode = h->arr[0];
    h->threads--;
    h->arr[0] = h->arr[h->threads];

    int index = 0;
    int left = 2 * index + 1;

    while (left < h->threads)
    {
        int right = 2 * index + 2;
        int smallest = index;

        if (h->arr[left]->timeQuant < h->arr[smallest]->timeQuant)
            smallest = left;
        if (right < h->threads && h->arr[right]->timeQuant < h->arr[smallest]->timeQuant)
            smallest = right;

        if (smallest == index)
            break;

        tcb *temp = h->arr[index];
        h->arr[index] = h->arr[smallest];
        h->arr[smallest] = temp;

        index = smallest;
        left = 2 * index + 1;
    }

    return minNode;
}

tcb* searchByTID(minHeap *h, int tID)
{
    for (int i = 0; i < h->threads; i++)
    {
        if (h->arr[i]->tID == tID)
            return h->arr[i];  
    }
    return NULL;  
}

int removeNode(minHeap *h, int tID)
{
    tcb *node = searchByTID(h, tID);
    if (!node)
        return -1;  

    int index = -1;
    for (int i = 0; i < h->threads; i++) {
        if (h->arr[i] == node) {
            index = i;
            break;
        }
    }
    if (index == -1) return -1; 

    h->threads--;
    if (index == h->threads)
        return 0;  

    h->arr[index] = h->arr[h->threads];

    int idx = index;
    int left = 2 * idx + 1;
    while (left < h->threads) {
        int right = 2 * idx + 2;
        int smallest = idx;

        if (h->arr[left]->timeQuant < h->arr[smallest]->timeQuant)
            smallest = left;
        if (right < h->threads && h->arr[right]->timeQuant < h->arr[smallest]->timeQuant)
            smallest = right;

        if (smallest == idx) break;

        tcb *temp = h->arr[idx];
        h->arr[idx] = h->arr[smallest];
        h->arr[smallest] = temp;

        idx = smallest;
        left = 2 * idx + 1;
    }

    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (h->arr[idx]->timeQuant >= h->arr[parent]->timeQuant)
            break;

        tcb *temp = h->arr[idx];
        h->arr[idx] = h->arr[parent];
        h->arr[parent] = temp;

        idx = parent;
    }

    return 0;
}

/* Function Declarations: */

/* create a new thread */
int worker_create(worker_t *thread, pthread_attr_t *attr, void *(*function)(void *), void *arg);

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
