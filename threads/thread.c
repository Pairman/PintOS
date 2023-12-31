#include "threads/thread.h"
#include <debug.h>
#include <fixed_point.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes sleeping a.k.a. blocked with ticks_sleep to come. */
static struct list sleep_list;

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long ticks;		/* Number of OS timer ticks. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */
static fixed_t load_avg;	/* System load average. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
	/* Initialize sleep_list. */
	list_init(&sleep_list);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/**
 * thread_tick - do thread ticking
 *
 * Called by the timer interrupt handler at each timer tick.
 * Thus, this function runs in an external interrupt context.
*/
void thread_tick(void)
{
	struct thread *current = thread_current();

	/* Update statistics. */
	ticks++;
	if (current == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (current->pagedir != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Update statistics for MLFQS. */
	if (thread_mlfqs) {
		/* recent_cpu is incremented each tick. */
		thread_mlfqs_increment_recent_cpu();
		/* load_avg and recent_cpu is updated each second. */
		if (ticks % TIMER_FREQ == 0)
			thread_mlfqs_update_recent_cpu();
		/* Priority is updated every 4th tick. */
		else if (ticks % 4 == 0 && current != idle_thread)
			thread_mlfqs_update_priority(current);
	}

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

	/* Yield the current thread if more prioritized one created. */
	if (priority > thread_current()->priority)
		thread_yield();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock (struct thread *t)
{
	enum intr_level old_level;
	bool aux;

	ASSERT(is_thread(t));

	old_level = intr_disable();

	ASSERT(t->status == THREAD_BLOCKED);
	/* Insert to ready_list with priority descending. */
	aux = true;
	list_insert_ordered(&ready_list, &t->elem, thread_cmp_priority, &aux);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/**
 * thread_cmp_ticks_sleep - compare two threads' ticks_sleep
 * 
 * @e1: pointer to sleepelem of one thread
 * @e2: pointer to sleepelem of another thread
 * @aux: pointer to direction of comparison in bool.
 *
 * Compare two threads' ticks_sleep.
*/
bool thread_cmp_ticks_sleep(const struct list_elem *e1,
			    const struct list_elem *e2, void *aux)
{
	struct thread *t1;
	struct thread *t2;

	/* Convert sleepelems to threads. */
	t1 = list_entry(e1, struct thread, sleepelem);
	t2 = list_entry(e2, struct thread, sleepelem);

	if (*(bool *)aux)
		return t1->ticks_sleep > t2->ticks_sleep;
	return t1->ticks_sleep < t2->ticks_sleep;
}

/**
 * thread_sleep - put the current thread to sleep
 *
 * @ticks: ticks to sleep until
 *
 * Put the current thread to sleep until OS ticks reaching the given ticks.
 * Must be called with interrupts turned off.
*/
void thread_sleep(int64_t ticks)
{
	struct thread *current;
	bool aux;

	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);

	current = thread_current();
	/* Set ticks_sleep. */
	current->ticks_sleep = ticks;
	/* Insert to sleep list with ticks_sleep ascending. */
	aux = false;
	list_insert_ordered(&sleep_list, &current->sleepelem,
			    thread_cmp_ticks_sleep, &aux);
	/* Block the current thread. */
	thread_block();
}

/**
 * thread_foreach_wake - wake threads if ticks_sleep reached the given ticks
 *
 * Wake threads in sleep_list if the given ticks has reached its ticks_sleep.
 * Must be called with interrupts turned off.
*/
void thread_foreach_wake(int64_t ticks)
{
	struct list_elem *e;
	struct list_elem *e_next;
	struct thread *t;

	ASSERT(intr_get_level() == INTR_OFF);

	e = list_begin(&sleep_list);
	while (e && e != list_end(&sleep_list)) {
		e_next = list_next(e);

		t = list_entry (e, struct thread, sleepelem);
		/* Wake up any thread with ticks_sleep reached. */
		if (t->ticks_sleep <= ticks) {
			/* Remove from sleep_list and reset ticks_sleep. */
			list_remove(e);
			t->ticks_sleep = 0;
			/* Unblock this thread. */
			thread_unblock(t);
		}
		/* Stop since no one left since ticks_sleep is ascending. */
		else {
			break;
		}

		e = e_next;
	}
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/**
 * thread_from_tid - return the thread with the given tid
 *
 * @tid: the tid
 *
 * Return the thread with the given tid.
 * Must be called with interrupts turned off.
*/
struct thread *thread_from_tid(tid_t tid)
{
	struct list_elem *e;
	struct thread *t;

	ASSERT(intr_get_level() == INTR_OFF);

	for (e = list_begin(&all_list); e != list_end(&all_list);
	     e = list_next(e)) {
		t = list_entry(e, struct thread, allelem);
		if (t->tid == tid)
			return t;
	}

	/* Return NULL if no corresponding thread exists. */
	return NULL;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/**
 * thread_yield - yield the cpu
 *
 * Yields the CPU. The current thread is not put to sleep and
 * may be scheduled again immediately at the scheduler's whim.
*/
void thread_yield(void)
{
	enum intr_level old_level;
	struct thread *current;
	bool aux;
  
	ASSERT(!intr_context());

	current = thread_current();
	old_level = intr_disable ();
	if (current != idle_thread) {
		/* Insert to ready_list with priority descending. */
		aux = true;
		list_insert_ordered(&ready_list, &current->elem,
				    thread_cmp_priority, &aux);
	}
	current->status = THREAD_READY;
	schedule();
	intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/**
 * thread_cmp_priority - compare two threads' priorities
 * 
 * @e1: pointer to elem of one thread
 * @e2: pointer to elem of another thread
 * @aux: pointer to direction of comparison in bool.
 *
 * Compare two threads' priorities.
*/
bool thread_cmp_priority(const struct list_elem *e1,
			    const struct list_elem *e2, void *aux)
{
	struct thread *t1;
	struct thread *t2;

	/* Convert sleepelems to threads. */
	t1 = list_entry(e1, struct thread, elem);
	t2 = list_entry(e2, struct thread, elem);

	if (*(bool *)aux)
		return t1->priority > t2->priority;
	return t1->priority < t2->priority;
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority (int new_priority)
{
	enum intr_level old_level;
	struct thread *current;

	if (thread_mlfqs)
		return;

	old_level = intr_disable();
	current = thread_current();
	current->base_priority = new_priority;
	/* Update priority of the thread if the new one is higher */
	/* or holding no locks thus no donation to account. */
	if (list_empty(&current->locks) || new_priority > current->priority) {
		current->priority = new_priority;
		/* Yield for more prioritized threads if any. */
		thread_yield();
	}

	intr_set_level(old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/**
 * thread_update_priority - update priority of the thread
 *
 * @t: pointer to the thread
 *
 * Update priority of the given thread.
*/
void thread_update_priority(struct thread *t)
{
	enum intr_level old_level;
	int lock_priority;
	bool aux;

	old_level = intr_disable();

	/* Fallback to base_priority as default. */
	t->priority = t->base_priority;
	/* Check if its locks has higher priority. */
	if (!list_empty(&t->locks)) {
		/* Sort the list in case priorities of locks are updated. */
		aux = true;
		list_sort(&t->locks, lock_cmp_priority, &aux);
		/* Get the max priority of its locks. */
		lock_priority = list_entry(list_front(&t->locks), struct lock,
					   elem)->priority;
		/* Update if the lock has higher priority. */
		if (lock_priority > t->priority)
			t->priority = lock_priority;
	}

	intr_set_level(old_level);
}

/**
 * thread_donate_priority - let the current thread donate priority
 *
 * @t: pointer to the thread
 *
 * Let the current thread donate its priority to the given thread.
*/
void thread_donate_priority(struct thread *t)
{
	enum intr_level old_level;
	int old_priority;
	bool aux;

	old_level = intr_disable();

	/* Update priority of the thread. */
	old_priority = t->priority;
	thread_update_priority(t);
	/* Re-insert to ready_list if priority changed. */
	if (t->status == THREAD_READY && t->priority != old_priority) {
		list_remove(&t->elem);
		aux = true;
		list_insert_ordered(&ready_list, &t->elem, thread_cmp_priority,
				    &aux);
	}

	intr_set_level(old_level);
}

/**
 * thread_hold_lock - let the current thread hold the lock
 *
 * @lock: pointer to the lock
 *
 * Let the current thread hold the given lock.
*/
void thread_hold_lock(struct lock *lock)
{
	enum intr_level old_level;
	struct thread *current;
	bool aux;

	old_level = intr_disable();

	current = thread_current();
	/* Insert to locks list with priority descending. */
	aux = true;
	list_insert_ordered(&current->locks, &lock->elem,
			    lock_cmp_priority, &aux);
	/* Get the donated priority for the current thread. */
	if (lock->priority > current->priority) {
		current->priority = lock->priority;
		thread_yield();
	}

	intr_set_level(old_level);
}

/**
 * thread_release_lock - let the current thread release the lock
 *
 * @lock: pointer to the lock
 *
 * Let the current thread release the given lock.
*/
void thread_release_lock(struct lock *lock)
{
	enum intr_level old_level;

	old_level = intr_disable();
	/* Remove from locks list. */
	list_remove(&lock->elem);
	/* Update priority of the current thread in case of donation. */
	thread_update_priority(thread_current());
	intr_set_level(old_level);
}

/**
 * thread_set_nice - set niceness of the current thread
 *
 * @nice: the niceness
 *
 * Set niceness of the current thread.
*/
void thread_set_nice(int nice)
{
	struct thread *current = thread_current();
	int old_priority;

	/* Set niceness. */
	current->nice = nice;
	/* Update priority and yield if lowered. */
	old_priority = current->priority;
	thread_mlfqs_update_priority(current);
	if (current->priority < old_priority)
		thread_yield();
}

/**
 * thread_get_nice - get niceness of the current thread
 *
 * Get niceness of the current thread.
*/
int thread_get_nice(void)
{
	return thread_current()->nice;
}

/**
 * thread_get_load_avg - get 100 times the system load_avg
 *
 * Get 100 times the system load average.
*/
int thread_get_load_avg(void)
{
	return FP_RND(FP_MULI(load_avg, 100));
}

/**
 * thread_get_recent_cpu - get 100 times recent_cpu of the current thread
 *
 * Get 100 times the recent cpu time of the current thread.
*/
int thread_get_recent_cpu(void)
{
	return FP_RND(FP_MULI(thread_current()->recent_cpu, 100));
}

/**
 * thread_mlfqs_update_priority - update priority of the given thread
 *
 * @t: the thread
 *
 * Update the priority of the given thread for MLFQS.
*/
void thread_mlfqs_update_priority(struct thread *t)
{
	ASSERT(thread_mlfqs);
	ASSERT(t != idle_thread);

	/* priority = PRI_MAX - (recent_cpu / 4) - (nice * 2). */
	t->priority = FP_INT(FP_SUBI(FP_ISUB(PRI_MAX,
					     FP_DIVI(t->recent_cpu, 4)),
					     2 * t->nice));
	/* Boundary check. */
	t->priority = MIN(MAX(t->priority, FP_FIX(PRI_MIN)), FP_FIX(PRI_MAX));
}

/**
 * thread_mlfqs_increment_recent_cpu - increment recent_cpu of current
 *
 * Increment recent_cpu of the current thread for MLFQS.
*/
void thread_mlfqs_increment_recent_cpu(void)
{
	struct thread *current;

	ASSERT(thread_mlfqs);
	ASSERT(intr_context());

	current = thread_current();
	if (current == idle_thread)
		return;
	/* Increament if thread not idle but running or ready. */
	current->recent_cpu = FP_ADDI(current->recent_cpu, 1);
}

/**
 * thread_mlfqs_update_load_avg - update load_avg
 *
 * Update load_avg of the OS for MLFQS.
*/
void thread_mlfqs_update_load_avg(void)
{
	size_t ready_threads;

	ASSERT(thread_mlfqs);
	ASSERT(intr_context());

	/* Number of threads in running or ready state. */
	ready_threads = list_size(&ready_list)
		    + (thread_current() != idle_thread);
	/* load_avg = (59/60)*load_avg + (1/60)*ready_threads. */
	load_avg = FP_ADD(FP_DIVI(FP_MULI(load_avg, 59), 60),
			  FP_IDIVI(ready_threads, 60));
}

/**
 * thread_mlfqs_update_recent_cpu - update recent_cpu
 *
 * Update recent_cpu of every non-idle thread for MLFQS.
*/
void thread_mlfqs_update_recent_cpu(void)
{
	struct list_elem *e;
	struct thread *t;

	ASSERT(thread_mlfqs);
	ASSERT(intr_context());

	/* Update load_avg. */
	thread_mlfqs_update_load_avg();

	/* Update recent_cpu time and priority for every non-idle thread.*/
	/* recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice. */
	for (e = list_begin(&all_list); e != list_end(&all_list);
	     e = list_next(e)) {
		t = list_entry(e, struct thread, allelem);
		if (t != idle_thread) {
			t->recent_cpu = FP_ADDI(FP_MUL(FP_DIV(
						FP_MULI(load_avg, 2),
						FP_ADDI(FP_MULI(load_avg, 2),
						1)), t->recent_cpu), t->nice);
			thread_mlfqs_update_priority(t);
		}
	}
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/**
 * init_thread- basic initialization of a thread
 *
 * @t: pointer to the thread
 * @name: pointer to the name
 * @priority: the priority
 *
 * Does basic initialization of a thread with
 * the given name and priority and being blocked.
*/
static void init_thread(struct thread *t, const char *name, int priority)
{
	enum intr_level old_level;
	bool aux;

	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->stack = (uint8_t *)t + PGSIZE;
	/* Initialize priority. */
	t->priority = t->base_priority = priority;
	/* Initialize locks list. */
	list_init(&t->locks);

	t->magic = THREAD_MAGIC;

	old_level = intr_disable();
	/* Insert to all_list. */
	aux = true;
	list_insert_ordered(&all_list, &t->allelem, thread_cmp_priority, &aux);
	intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
