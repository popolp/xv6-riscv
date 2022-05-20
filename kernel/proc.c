#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern uint64 cas(volatile void *addr, int expected , int newval);

extern void add_to_last(int *first, int insert_proc, struct spinlock *link);
extern int remove_proc_from_list(int *first, int remove_proc, struct spinlock *link);
extern int get_cpu();
extern int least_used_cpu(int running_cpu);
extern int steal_process(int cpu_id);
extern void print_list(int *first);

int next_zombie = -1;
int next_unused = -1;
int next_sleeping = -1;
int cpus_ready[NCPU];
int cpus_runs[NCPU];
uint64 cpus_count[NCPU];

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

struct spinlock link_lock_zombie;
struct spinlock link_lock_unused;
struct spinlock link_lock_sleep;
struct spinlock link_lock[NCPU];

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  int index = 0;
  for(p = proc; p < &proc[NPROC]; p++, index++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
      p->proc_index = index;
      p->next_proc = -1;
      add_to_last(&next_unused, index, &link_lock_unused);
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  do {
    pid = nextpid;
  } while (cas(&nextpid, pid, pid + 1));
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  int proc_to_init = next_unused;
  if (proc_to_init > -1){
    p = &proc[proc_to_init];
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      if(remove_proc_from_list(&next_unused, proc_to_init, &link_lock_unused))
        goto found;
      else {
        release(&p->lock);
        return allocproc();
      }
    }
    else {
      release(&p->lock);
      return allocproc();
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  remove_proc_from_list(&next_zombie, p->proc_index, &link_lock_zombie);
  add_to_last(&next_unused, p->proc_index, &link_lock_unused);
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  for (int i = 0; i < 8; i++)
  {
    cpus_ready[i] = -1;
    cpus_runs[i] = 0;
    cpus_count[i] = 0;
  }
  while (cas(&cpus_ready[0], -1, p->proc_index));

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  np->running_cpu = least_used_cpu(p->running_cpu);
  add_to_last(&cpus_ready[np->running_cpu], np->proc_index, &link_lock[np->running_cpu]);
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  add_to_last(&next_zombie, p->proc_index, &link_lock_zombie);

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  int proc_to_run;
  int cpu_id = get_cpu();
  
  cpus_runs[cpu_id] = 1;

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    if (cpu_id > -1)
    {    
      acquire(&link_lock[cpu_id]);
      proc_to_run = cpus_ready[cpu_id];
      
      if (proc_to_run != -1)
      {
        p = &proc[proc_to_run];
        acquire(&p->list_lock);
        cpus_ready[cpu_id] = p->next_proc;
        p->next_proc = -1;
        release(&p->list_lock);
      }
      release(&link_lock[cpu_id]);

      if (proc_to_run == -1)
      {
        #ifdef ON
        proc_to_run = steal_process(cpu_id);
        #endif
      }
      
      if (proc_to_run > -1)
      { 
        p = &proc[proc_to_run];
        acquire(&p->lock);

        if (p->state == RUNNABLE)
        {        
          p->state = RUNNING;

          c->proc = p;
          swtch(&c->context, &p->context);
          
          c->proc = 0;
        }
        release(&p->lock);
        
      }
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  add_to_last(&cpus_ready[p->running_cpu], p->proc_index, &link_lock[p->running_cpu]);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  add_to_last(&next_sleeping, p->proc_index, &link_lock_sleep);
  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *curr, *pred, *removed_proc;
  pred = 0;
  int removed = 0;
  int sleepLocked = 1;
  int next_proc = -1;
  acquire(&link_lock_sleep);
  if (next_sleeping != -1){
    next_proc = next_sleeping; 
    curr = &proc[next_proc];
    acquire(&curr->list_lock); 
    acquire(&curr->lock);
  }

  while (next_proc > -1)
  {
    next_proc = curr->next_proc;
    if(curr->state == SLEEPING && curr->chan == chan) {
      removed = 1;
      removed_proc = curr;
      if (curr->proc_index == next_sleeping)
      {
        next_sleeping = next_proc;
      }
      else {
        pred->next_proc = curr->next_proc;
        if (sleepLocked)
        {
          release(&link_lock_sleep);
          sleepLocked = 0;
        }        
      }
      curr->state = RUNNABLE;
      curr->next_proc = -1;  
      curr->running_cpu = least_used_cpu(curr->running_cpu);
      add_to_last(&cpus_ready[curr->running_cpu], curr->proc_index, &link_lock[curr->running_cpu]);    
    }
    else removed = 0;

    if (pred != 0 && !removed)
    { 
      release(&pred->lock);
      release(&pred->list_lock);
    }
    if (!removed){
      pred = curr;
      if (sleepLocked)
      {
        release(&link_lock_sleep);
        sleepLocked = 0;
      }
    }
      
    if (next_proc != -1)
    {
      curr = &proc[next_proc];
      acquire(&curr->list_lock);
      acquire(&curr->lock);
    }
    if (removed)
    {
      release(&removed_proc->lock);
      release(&removed_proc->list_lock);
    }

  }

  if (pred != 0){
      release(&pred->lock);
      release(&pred->list_lock);
  }
  if(sleepLocked)
    release(&link_lock_sleep);
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
        remove_proc_from_list(&next_sleeping, p->proc_index, &link_lock_sleep);
        release(&p->lock);
        add_to_last(&cpus_ready[p->running_cpu], p->proc_index, &link_lock[p->running_cpu]);
      }
     else release(&p->lock);
    return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

void add_to_last(int *first, int insert_proc, struct spinlock *link) {

  acquire(link);
  if(*first == -1){
    *first = insert_proc;
    proc[insert_proc].next_proc = -1;
    release(link);
    return;
  }

  struct proc *curr = &proc[*first];
  acquire(&curr->list_lock); 
  release(link);
  
  struct proc *pred = curr;
  while (curr->next_proc != -1) {
    if (curr->proc_index == insert_proc)
    {
      release(&curr->list_lock);
      return;
    }
    
    pred = curr;
    curr = &proc[curr->next_proc];
    acquire(&curr->list_lock);
    release(&pred->list_lock);
  }
  curr->next_proc = insert_proc;
  proc[insert_proc].next_proc = -1;
  release(&curr->list_lock);
}

int remove_proc_from_list(int *first, int remove_proc, struct spinlock *link) {
  struct proc *pred, *curr;
  acquire(link);
  if (*first == -1)
  {
    release(link);
    return 0;
  }
  curr = &proc[*first];
  acquire(&curr->list_lock);
  if (remove_proc == *first)
  {
    *first = curr->next_proc;
    curr->next_proc = -1;
    release(&curr->list_lock);
    release(link);
    return 1;
  }
  
  release(link);
  pred = curr;
  curr = &proc[pred->next_proc];
  acquire(&curr->list_lock);
  if (remove_proc == curr->proc_index)
  {
      pred->next_proc = curr->next_proc;
      curr->next_proc = -1;
      release(&pred->list_lock);
      release(&curr->list_lock);
    return 1;
  }

  while (curr->next_proc > -1)
  {
    if (remove_proc == curr->next_proc)
    {
      curr->next_proc = proc[curr->next_proc].next_proc;      
      proc[curr->next_proc].next_proc = -1;
      
      release(&pred->list_lock);
      release(&curr->list_lock);
      return 1;
    }
    
    release(&pred->list_lock);
    pred = curr;
    curr = &proc[curr->next_proc];
    acquire(&curr->list_lock);
  }
  release(&pred->list_lock);
  release(&curr->list_lock);
  return 0;
  }

int get_cpu(){
  int cpu_id = cpuid();
  if (cpu_id < 0 || cpu_id > NCPU)
    return -1;
  return cpu_id;
}

int
least_used_cpu(int running_cpu){

  int min_cpu = 0;
  uint64 min_count = cpus_count[0];
  #ifdef ON
  for (int cpu = 1; cpu < NCPU; cpu++)
  {
    if (cpus_runs[cpu])
      if (cpus_count[cpu] < min_count){
        min_cpu = cpu; 
        min_count = cpus_count[cpu];
      }
  }
  #else 
  min_cpu = running_cpu;
  min_count = cpus_count[min_cpu];
  #endif
  do
  {
    min_count = cpus_count[min_cpu];
  } while (cas(&cpus_count[min_cpu], min_count, min_count + 1));
  return min_cpu;
}

int
steal_process(int cpu_id){
  int steal_proc = -1;
  struct proc *p;
  uint64 count;
  for (int cpu = 0; cpu < NCPU; cpu++)
  {
    if (cpu != cpu_id)
    {
      acquire(&link_lock[cpu]);
      steal_proc = cpus_ready[cpu];
      if (steal_proc != -1)
      {
        p = &proc[steal_proc];
        acquire(&p->list_lock);
        cpus_ready[cpu] = proc[steal_proc].next_proc; 
        p->next_proc = -1;
        p->running_cpu = cpu_id;
        release(&p->list_lock);
        release(&link_lock[cpu]);
        do
        {
          count = cpus_count[cpu_id];
        } while (cas(&cpus_count[cpu_id], count, count + 1));
        return steal_proc;
      }
      release(&link_lock[cpu]);
    }
  }
  return steal_proc;
}