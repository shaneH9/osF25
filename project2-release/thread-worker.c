// File:	thread-worker.c
// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

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
/* If a currently running thread is present and is still runnable, re-enqueue it.
       We assume the timer handler already incremented current->timeQuant before jumping here. */
    if (current) {
        if (current->state == RUNNING) {
            current->state = READY;
            /* place it back in the runqueue so the heap orders by timeQuant */
            enqueue(&rq, current);
            current = NULL;
        } else if (current->state == FINISHED) {
            /* thread finished — cleanup its stack here if needed (keep TCB for join) */
            if (current->stack) { free(current->stack); current->stack = NULL; }
            current = NULL;
        } else {
            /* other states (BLOCKED/READY) — do nothing special */
            current = NULL;
        }
    }

    /* pick the thread with smallest timeQuant from min-heap rq */
    tcb *next = dequeue(&rq);
    if (!next) {
        /* no runnable thread */
        return;
    }

    /* schedule it */
    next->state = RUNNING;
    current = next;

    /* count context switch */
    tot_cntx_switches++;

    /* switch to the chosen thread context; when it yields/exits/preempted control returns here */
    if (swapcontext(&schedCtx, &next->context) == -1) {
        perror("swapcontext in sched_psjf");
        exit(1);
    }
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
  /* Behavior:
       - Each mlfq[level] is treated as a runqueue (we use your minHeap array).
       - For a thread at level L we give timeslice = (1 << L) * 1 QUANTUM (i.e., 2^L quantums).
       - If a thread uses up its timeslice it is demoted (priority++), unless already at lowest level.
       - If a thread yields before its timeslice exhausted it stays at same level.
    */

    /* If current exists, handle its state first (it was preempted or returned here) */
    if (current) {
        if (current->state == RUNNING) {
            /* The timer or preemption placed us here; update and decide whether to demote/enqueue */
            /* current->timeQuant holds elapsed QUANTUM units for this thread globally; but we track the timeslice via pc */
            if (current->pc > 0) {
                /* used some of its timeslice; decrease remaining and, if still remaining, keep same priority */
                current->pc -= 1;
                if (current->pc > 0) {
                    /* Put it back to same priority queue as READY */
                    current->state = READY;
                    if (current->priority < 0) current->priority = 0;
                    if (current->priority >= NUMQUEUES) current->priority = NUMQUEUES-1;
                    enqueue(&mlfq[current->priority], current);
                } else {
                    /* timeslice exhausted -> demote */
                    if (current->priority < NUMQUEUES - 1)
                        current->priority += 1;
                    current->state = READY;
                    /* reset pc later when it's picked */
                    current->pc = 0;
                    enqueue(&mlfq[current->priority], current);
                }
            } else {
                /* If pc == 0 (no allocated timeslice) just re-enqueue at same priority */
                current->state = READY;
                if (current->priority < 0) current->priority = 0;
                if (current->priority >= NUMQUEUES) current->priority = NUMQUEUES-1;
                enqueue(&mlfq[current->priority], current);
            }
            current = NULL;
        } else if (current->state == FINISHED) {
            /* cleanup stack (join expects to cleanup TCB) */
            if (current->stack) { free(current->stack); current->stack = NULL; }
            current = NULL;
        } else {
            /* BLOCKED/READY -> leave it be */
            current = NULL;
        }
    }

    /* find highest-priority non-empty queue (priority 0 is highest) */
    int chosen_level = -1;
    for (int lvl = 0; lvl < NUMQUEUES; ++lvl) {
        if (mlfq[lvl].threads > 0) { chosen_level = lvl; break; }
    }
    if (chosen_level == -1) {
        /* no runnable threads */
        return;
    }

    /* pop next from that level's heap */
    tcb *next = dequeue(&mlfq[chosen_level]);
    if (!next) return;

    /* determine timeslice in QUANTUM units: 2^level */
    int timeslice_quanta = 1 << chosen_level;
    if (timeslice_quanta <= 0) timeslice_quanta = 1;

    next->priority = chosen_level;
    next->pc = timeslice_quanta; /* remaining quantums for its current timeslice */
    next->state = RUNNING;
    current = next;

    /* context switch counts */
    tot_cntx_switches++;

    /* switch to chosen thread */
    if (swapcontext(&schedCtx, &next->context) == -1) {
        perror("swapcontext in sched_mlfq");
        exit(1);
    }
	
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
	  /* Behavior:
       - Each mlfq[level] is treated as a runqueue (we use your minHeap array).
       - For a thread at level L we give timeslice = (1 << L) * 1 QUANTUM (i.e., 2^L quantums).
       - If a thread uses up its timeslice it is demoted (priority++), unless already at lowest level.
       - If a thread yields before its timeslice exhausted it stays at same level.
    */

    /* If current exists, handle its state first (it was preempted or returned here) */
    if (current) {
        if (current->state == RUNNING) {
            /* The timer or preemption placed us here; update and decide whether to demote/enqueue */
            /* current->timeQuant holds elapsed QUANTUM units for this thread globally; but we track the timeslice via pc */
            if (current->pc > 0) {
                /* used some of its timeslice; decrease remaining and, if still remaining, keep same priority */
                current->pc -= 1;
                if (current->pc > 0) {
                    /* Put it back to same priority queue as READY */
                    current->state = READY;
                    if (current->priority < 0) current->priority = 0;
                    if (current->priority >= NUMQUEUES) current->priority = NUMQUEUES-1;
                    enqueue(&mlfq[current->priority], current);
                } else {
                    /* timeslice exhausted -> demote */
                    if (current->priority < NUMQUEUES - 1)
                        current->priority += 1;
                    current->state = READY;
                    /* reset pc later when it's picked */
                    current->pc = 0;
                    enqueue(&mlfq[current->priority], current);
                }
            } else {
                /* If pc == 0 (no allocated timeslice) just re-enqueue at same priority */
                current->state = READY;
                if (current->priority < 0) current->priority = 0;
                if (current->priority >= NUMQUEUES) current->priority = NUMQUEUES-1;
                enqueue(&mlfq[current->priority], current);
            }
            current = NULL;
        } else if (current->state == FINISHED) {
            /* cleanup stack (join expects to cleanup TCB) */
            if (current->stack) { free(current->stack); current->stack = NULL; }
            current = NULL;
        } else {
            /* BLOCKED/READY -> leave it be */
            current = NULL;
        }
    }

    /* find highest-priority non-empty queue (priority 0 is highest) */
    int chosen_level = -1;
    for (int lvl = 0; lvl < NUMQUEUES; ++lvl) {
        if (mlfq[lvl].threads > 0) { chosen_level = lvl; break; }
    }
    if (chosen_level == -1) {
        /* no runnable threads */
        return;
    }

    /* pop next from that level's heap */
    tcb *next = dequeue(&mlfq[chosen_level]);
    if (!next) return;

    /* determine timeslice in QUANTUM units: 2^level */
    int timeslice_quanta = 1 << chosen_level;
    if (timeslice_quanta <= 0) timeslice_quanta = 1;

    next->priority = chosen_level;
    next->pc = timeslice_quanta; /* remaining quantums for its current timeslice */
    next->state = RUNNING;
    current = next;

    /* context switch counts */
    tot_cntx_switches++;

    /* switch to chosen thread */
    if (swapcontext(&schedCtx, &next->context) == -1) {
        perror("swapcontext in sched_mlfq");
        exit(1);
    }

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
	struct itimerval timerOff = {0};	
    memset(&timerOff, 0, sizeof(timerOff));
    setitimer(ITIMER_VIRTUAL, &timerOff, NULL); 

    //CHECKING IF CURRENT THREAD IS YIELDED 
    if(current){
        if (current->state = RUNNING){
            current->state = READY;
            enqueue(&rq, current); 
        }
        else if(current->state = FINISHED){
            if(current->stack){
                free(current->stack);
                current->stack = NULL;
            }
            current = NULL; 
        }
        else{
            current = NULL; 
        }
    }


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