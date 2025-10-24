// File:	thread-worker.c
// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"
#include <pthread.h>
#include <time.h>

// Global counter for total context switches and
// average turn around and response time
long tot_cntx_switches = 0;
double avg_turn_time = 0;
double avg_resp_time = 0;
int threadID = 0;
ucontext_t schedCtx;
tcb *current = NULL;
minHeap rq;
minHeap mlfq[NUMQUEUES]; 

// INITAILIZE ALL YOUR OTHER VARIABLES HERE
// YOUR CODE HERE

int worker_create(worker_t *thread, pthread_attr_t *attr,
				  void *(*function)(void *), void *arg)
{
	void *stackAddress = NULL;
	size_t stackSize = 0;

	pthread_attr_getstack(attr, &stackAddress, &stackSize);
	if (stackSize == 0)
	{
		stackSize = 2048 * 32;
	}
	stackAddress = malloc(stackSize);
	if (!stackAddress)
	{
		return -1;
	}

	ucontext_t epicContext;
	getcontext(&epicContext);
	epicContext.uc_stack.ss_sp = stackAddress;
	epicContext.uc_stack.ss_size = stackSize;
	epicContext.uc_stack.ss_flags = 0;
	makecontext(&epicContext, (void (*)(void))function, 1, arg);

	tcb *block = malloc(sizeof(tcb));
	if (block)
	{
		block->tID = threadID++;
		block->state = READY;
		block->context = epicContext;
		block->stack = stackAddress;
		block->next = NULL;
		*thread = block->tID;
	}
	else
	{
		return -1;
	}
	enqueue(&rq, block);

	return 0;
};

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield()
{
	current->state = READY;
	enqueue(&rq, current);
	swapcontext(&current->context, &schedCtx);

	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr)
{
	current->state = FINISHED;
	current->retValue = value_ptr;
	setcontext(&schedCtx);
};

/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr)
{
	tcb *block = searchByTID(&rq, thread);
	if (!block)
	{
		return -1;
	}
	while (block->state != FINISHED)
	{
		worker_yield();
	}
	*value_ptr = block->retValue;

	removeNode(&rq, block->tID);
	free(block->stack);
	free(block);

	return 0;
};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex,
					  const pthread_mutexattr_t *mutexattr)
{
	if (mutex)
	{
		mutex->locked = 0;
		return 0;
	}
	else
	{
		return -1;
	}
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex)
{
	// - use the built-in test-and-set atomic function to test the mutex
	while (__atomic_test_and_set(&mutex->locked, __ATOMIC_SEQ_CST))
	{
		current->state = BLOCKED;
		removeNode(&rq, current->tID);
		enqueue(&mutex->blockList, current);
		setcontext(&schedCtx);
	}
	return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex)
{
	__atomic_clear(&mutex->locked, __ATOMIC_SEQ_CST);
	if (mutex->blockList.threads != 0)
	{
		tcb *pop = dequeue(&mutex->blockList);
		pop->state = READY;
		enqueue(&rq, pop);
	}

	return 0;
};

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex)
{
	if (mutex->locked == 1)
	{
		return -1;
	}
	if (mutex->blockList.threads != 0)
	{
		return -1;
	}
	free(mutex);

	return 0;
};

/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
static void sched_psjf()
{
	tot_cntx_switches++;

    // If there's a currently running thread, put it back if it's not finished
    if (current && current->state == RUNNING) {
        current->state = READY;
        enqueue(&rq, current);
    }

    // INITIALIZING
    tcb *next = dequeue(&rq);

    if (!next) {
        printf("No threads to schedule (PSJF).\n");
        return;
    }

    next->state = RUNNING;
    current = next;

    // JUMP TO NEXT THREAD
    setcontext(&next->context);
	
}

/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq()
{
	// - your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	//intializing multi-queue
	


	/* Step-by-step guidances */
	// Step1: Calculate the time current thread actually ran
	// Step2.1: If current thread uses up its allotment, demote it to the low priority queue (Rule 4)
	// Step2.2: Otherwise, push the thread back to its origin queue
	// Step3: If time period S passes, promote all threads to the topmost queue (Rule 5)
	// Step4: Apply RR on the topmost queue with entries and run next thread
	
	init_mlfq(); 

    tot_cntx_switches++;

    // Step 1: If current thread is still running, decide whether to demote it
    if (current && current->state == RUNNING) {
        // Suppose we store priority in tcb->priority (0 = highest)
        if (current->timeQuant >= QUANTUM && current->priority < NUMQUEUES - 1) {
            current->priority++;
        }

        current->state = READY;
        enqueue(&mlfq[current->priority], current);
    }

    // Step 2: Find the highest non-empty queue
    
	int i=0; 
    for (i = 0; i < NUMQUEUES; i++) {
        if (mlfq[i].threads > 0)
            break;
    }

    if (i == NUMQUEUES) {
        printf("All queues empty (MLFQ)\n");
        return;
    }

    // Step 3: Select the next thread
    tcb *next = dequeue(&mlfq[i]);
    next->state = RUNNING;
    current = next;

    // Step 4: Give it a time slice
    current->timeQuant = QUANTUM;

    // Step 5: Run it
    setcontext(&next->context);
}

/* Completely fair scheduling algorithm */
static void sched_cfs()
{
	// - your own implementation of CFS
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE

	/* Step-by-step guidances */

	// Step1: Update current thread's vruntime by adding the time it actually ran
	// Step2: Insert current thread into the runqueue (min heap)
	// Step3: Pop the runqueue to get the thread with a minimum vruntime
	// Step4: Calculate time slice based on target_latency (TARGET_LATENCY), number of threads within the runqueue
	// Step5: If the ideal time slice is smaller than minimum_granularity (MIN_SCHED_GRN), use MIN_SCHED_GRN instead
	// Step5: Setup next time interrupt based on the time slice
	// Step6: Run the selected thread
    tot_cntx_switches++;

    // Step 1: Update current thread's virtual runtime
    if (current && current->state == RUNNING) {
        // Suppose the thread ran for one quantum
        current->timeQuant += QUANTUM;
        current->state = READY;
        enqueue(&rq, current);
    }

    // Step 2: Pick the thread with the smallest vruntime (timeQuant here)
    tcb *next = dequeue(&rq);
    if (!next) {
        printf("No threads in CFS ready queue.\n");
        return;
    }

    // Step 3: Compute ideal time slice based on number of threads
    long slice = TARGET_LATENCY / (rq.threads ? rq.threads : 1);
    if (slice < MIN_SCHED_GRN)
        slice = MIN_SCHED_GRN;

    next->state = RUNNING;
    current = next;
    current->timeQuant += slice;

    // Step 4: Switch to the selected thread
    setcontext(&next->context);

}

static void init_mlfq(){
	for(int i=0; i<NUMQUEUES; i++){
		initHeap(&mlfq[i], 50); 
	}
}

/* scheduler */
static void schedule()
{
	// - every time a timer interrupt occurs, your worker thread library
	// should be contexted switched from a thread context to this
	// schedule() function

	// YOUR CODE HERE

	// - invoke scheduling algorithms according to the policy (PSJF or MLFQ or CFS)
#if defined(PSJF)
	sched_psjf();
#elif defined(MLFQ)
	sched_mlfq();
#elif defined(CFS)
	sched_cfs();
#else
	// error: #Define one of PSJF, MLFQ, or CFS when compiling. e.g. make SCHED=MLFQ"
#endif
}

// DO NOT MODIFY THIS FUNCTION
/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void)
{

	fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
	fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
	fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}

int initHeap(minHeap *h, int capacity) {
    h->arr = malloc(sizeof(tcb*) * capacity);
    if (!h->arr) {
        return -1;
    }
    h->threads = 0;
    h->threshold = capacity;
	return 0;
}

int main() {
	initHeap(&rq, 50);
}