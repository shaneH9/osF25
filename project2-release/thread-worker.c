// File:	thread-worker.c
// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"
#include <pthread.h>

//Global counter for total context switches and 
//average turn around and response time
long tot_cntx_switches=0;
double avg_turn_time=0;
double avg_resp_time=0;
int threadID = -1;
ucontext_t schedCtx;
tcb* current = NULL;
runQueue rq = { .head = NULL, .tail = NULL, .size = 0 };


// INITAILIZE ALL YOUR OTHER VARIABLES HERE
// YOUR CODE HERE


int worker_create(worker_t * thread, pthread_attr_t * attr, 
                      void *(*function)(void*), void * arg) {
		void* stackAddress = NULL; 
		size_t stackSize = 0;

		pthread_attr_getstack(attr, &stackAddress, &stackSize);
		if(stackSize == 0)
		{
			stackSize = 2048;
		}
		stackAddress = malloc(stackSize);
		if(!stackAddress)
		{
			return -1;
		}

		ucontext_t epicContext;
		getcontext(&epicContext);
		epicContext.uc_stack.ss_sp = stackAddress;
    	epicContext.uc_stack.ss_size = stackSize;
    	epicContext.uc_stack.ss_flags = 0;
		makecontext(&epicContext, (void (*)(void))function, 1, arg);

		tcb* block = malloc(sizeof(tcb));
		if(block){
			block->tID = threadID++;
    		block->state = READY;
    		block->context = epicContext;
    		block->stack = stackAddress;
    		block->next = NULL;
			*thread = block->tID;
			current = block;
		}
		else{
			return -1;
		}
		enqueue(&rq,block);

    return 0;
};

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield() {
	current->state = 0;
	getcontext(&current->context);
	enqueue(&rq, current);
	setcontext(&schedCtx);

	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr) {
	current->state = BLOCKED;
	current->retValue = value_ptr;
	removeNode(&rq, current->tID);
	setcontext(&schedCtx);
};


/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr) {
	
	// - wait for a specific thread to terminate
	// - de-allocate any dynamic memory created by the joining thread
  
	// YOUR CODE HERE
	return 0;
};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, 
                          const pthread_mutexattr_t *mutexattr) {
	//- initialize data structures for this mutex

	// YOUR CODE HERE
	return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex) {

        // - use the built-in test-and-set atomic function to test the mutex
        // - if the mutex is acquired successfully, enter the critical section
        // - if acquiring mutex fails, push current thread into block list and
        // context switch to the scheduler thread

        // YOUR CODE HERE
        return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex) {
	// - release mutex and make it available again. 
	// - put threads in block list to run queue 
	// so that they could compete for mutex later.

	// YOUR CODE HERE
	return 0;
};


/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex) {
	// - de-allocate dynamic memory created in worker_mutex_init

	return 0;
};

/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
static void sched_psjf() {
	// - your own implementation of PSJF
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE
}


/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq() {
	// - your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE

	/* Step-by-step guidances */
	// Step1: Calculate the time current thread actually ran
	// Step2.1: If current thread uses up its allotment, demote it to the low priority queue (Rule 4)
	// Step2.2: Otherwise, push the thread back to its origin queue
	// Step3: If time period S passes, promote all threads to the topmost queue (Rule 5)
	// Step4: Apply RR on the topmost queue with entries and run next thread
}

/* Completely fair scheduling algorithm */
static void sched_cfs(){
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
}

//we need to do this i just commented it out for now
/* scheduler */
// static void schedule() {
// 	// - every time a timer interrupt occurs, your worker thread library 
// 	// should be contexted switched from a thread context to this 
// 	// schedule() function
	
// 	//YOUR CODE HERE

// 	// - invoke scheduling algorithms according to the policy (PSJF or MLFQ or CFS)
// #if defined(PSJF)
//     	sched_psjf();
// #elif defined(MLFQ)
// 	sched_mlfq();
// #elif defined(CFS)
//     	sched_cfs();  
// #else
// 	# error "Define one of PSJF, MLFQ, or CFS when compiling. e.g. make SCHED=MLFQ"
// #endif
// }



//DO NOT MODIFY THIS FUNCTION
/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void) {

       fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
       fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
       fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}


// Feel free to add any other functions you need

// YOUR CODE HERE

