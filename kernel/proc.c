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

extern void add_to_last(int *first, int new_last);
extern void remove_proc_from_list(int *first, int remove_proc);
extern void add_to_cpu(struct proc *p, int running_cpu);

extern char trampoline[]; // trampoline.S

extern uint64 cas(volatile void *addr, int expected , int newval);

int next_unused = -1;     
int next_sleeping = -1;
int next_zombie = -1;

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

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
{printf("got procinit\n");
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  int index = 0;
  for(p = proc; p < &proc[NPROC]; p++, index++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
      p->proc_index = index;
      p->next_proc = -1;
      add_to_last(&next_unused, index);
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
{printf("got allocproc\n");
  struct proc *p;
  int proc_to_init = next_unused;
  if (proc_to_init > -1)
  {
    p = &proc[next_unused];
    acquire(&p->lock);
    printf("accquire lock in allocproc pid %d\n", p->pid);
    while(cas(&next_unused, proc_to_init, proc[proc_to_init].next_proc));
    goto found;
  }
  return 0;
  
 /* for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;*/

found:
  p->pid = allocpid();
  p->state = USED;
  p->next_proc = -1;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    printf("release lock in allocproc pid %d\n", p->pid);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    printf("release lock in allocproc pid %d\n", p->pid);
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
{printf("got freeproc\n");
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);

  remove_proc_from_list(&next_zombie, p->proc_index);
  //proc[p->proc_index].next_proc = -1;
  add_to_last(&next_unused, p->proc_index); // Check if the index doesnt change

  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->running_cpu = -1;
  p->next_proc = -1;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{printf("got pageta\n");
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
{printf("got freepage\n");
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
{printf("got userinit\n");
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

  int first_proc = cpus[0].first_proc;
  int last_proc = cpus[0].last_proc;
  while (cas(&cpus[0].first_proc, first_proc, p->proc_index));
  while (cas(&cpus[0].last_proc, last_proc, p->proc_index));

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{printf("got grow\n");
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
{printf("got fork\n");
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
    printf("release lock in fork pid %d\n", np->pid);
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

  printf("release lock in fork pid %d\n", np->pid);
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  printf("accquire lock in fork pid %d\n", np->pid);
  np->state = RUNNABLE;
  np->running_cpu = p->running_cpu;
  add_to_cpu(np, p->running_cpu);
  
  printf("release lock in fork pid %d\n", np->pid);
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{printf("got reap\n");
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
{printf("got exit\n");
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
  printf("accquire lock in exit pid %d\n", p->pid);  
  p->xstate = status;
  p->state = ZOMBIE;
  add_to_last(&next_zombie, p->proc_index);
  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{printf("got wait\n");
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
        printf("accquire lock in wait pid %d\n", p->pid);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            printf("release lock in wait pid %d\n", np->pid);
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          
          printf("release lock in wait pid %d\n", np->pid);
          release(&np->lock);
          
          printf("release wait lock in wait\n");
          release(&wait_lock);
          return pid;
        }
        printf("release lock in wait pid %d\n", np->pid);
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
{printf("got scheduler\n");
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    if (c->first_proc > -1)
    { 
      p = &proc[c->first_proc];
      acquire(&p->lock);
      printf("accquire lock in sched pid %d\n", p->pid);
      while (cas(&p->state, RUNNABLE, RUNNING));
      if (c->first_proc == c->last_proc)
      {
        c->first_proc = -1;
        c->last_proc = -1;
      }
      else c->first_proc = p->next_proc;
      printf("pid %d\n", p->pid);
      p->next_proc = -1;                  // Reset next_proc

      c->proc = p;
      swtch(&c->context, &p->context);
      
      c->proc = 0;
      release(&p->lock);
      printf("scheduler release pid %d\n", p->pid);
    }
   /* for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }*/
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
{printf("got sched\n");
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
  printf("accquire lock in yield pid %d\n", p->pid);
  p->state = RUNNABLE;
  add_to_cpu(p, p->running_cpu);
  sched();
  printf("release lock in yield pid %d\n", p->pid);
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{printf("got forkret\n");
  static int first = 1;

  // Still holding p->lock from scheduler.
  printf("release lock in forkret pid %d\n", myproc()->pid);
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
{printf("got sleep\n");
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  printf("got sleep pid %d\n", p->pid);
  acquire(&p->lock);  //DOC: sleeplock1
  printf("accquire lock in sleep pid %d\n", p->pid);
  release(lk);
  
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  add_to_last(&next_sleeping, p->proc_index);
  printf("sleeps %d\n", next_sleeping);
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  printf("release lock in sleep pid %d\n", p->pid);
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{printf("got wake\n");
  struct proc *p;
  int next_proc = next_sleeping;
  
  while (next_proc > -1)
  {printf("wake next %d\n",next_proc);
    p = &proc[next_proc];  // 1 2 3 -1
    next_proc = p->next_proc;
   // if(p != myproc()){
      acquire(&p->lock);
      if (p->chan == chan){
        p->state = RUNNABLE;
        remove_proc_from_list(&next_sleeping, p->proc_index);
        add_to_cpu(p, p->running_cpu);
      }
      release(&p->lock);
   // }
  }
  
  // for(p = proc; p < &proc[NPROC]; p++) {
  //   if(p != myproc()){
  //     acquire(&p->lock);
  //     if(p->state == SLEEPING && p->chan == chan) {
  //       p->state = RUNNABLE;
  //     }
  //     release(&p->lock);
  //   }
  //}
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{printf("got kill\n");
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    printf("accquire lock in kill pid %d\n", p->pid);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      printf("release lock in kill pid %d\n", p->pid);
      release(&p->lock);
      return 0;
    }
    printf("release lock in kill pid %d\n", p->pid);
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{//printf("got copyout\n");
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
{printf("got copyin\n");
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

/*void
new_head(int new_head, int old_head)
{
  struct proc *p_O_head;
  struct proc *p_N_head;
  do {
    p_O_head = &proc[old_head];
    p_N_head = &proc[new_head]; 
  } while (cas(&p_O_head->next_proc, old_head, p_N_head));
}*/

int
set_cpu(int cpu_num){
  struct proc *p = myproc();
  struct cpu *c = &cpus[cpu_num]; 
  int old_last;
  p->running_cpu = cpu_num;
  old_last = c->last_proc;
  if(!cas(&c->last_proc, old_last, p->pid)){
    proc[old_last].next_proc = p->pid; 
    yield();
    return cpu_num;
  }
  return -1;
}

int get_cpu(){
  return cpuid();
}

void
add_to_last(int *first, int new_last){
  if (*first != -1)
  {  
    struct proc *pred = &proc[*first];
    int last_index;
    do
    {
      acquire(&pred->lock);
      last_index = pred->next_proc;
      release(&pred->lock);
      pred = &proc[last_index];
    } while (cas(&pred->next_proc, -1, new_last));
  }
  else if(cas(first, -1, new_last)) 
          add_to_last(first, new_last);
}

void
remove_proc_from_list(int *first, int remove_proc){
  printf("got remove\n");
  int new_next = proc[remove_proc].next_proc;
  struct proc *pred = &proc[*first];
  int last_index;
  if (*first == remove_proc)
    while (cas(first, remove_proc, pred->next_proc));
  else
    do
      {
        acquire(&pred->lock);
        printf("acquire lock in remove pid %d\n", pred->pid);
        last_index = pred->next_proc;
        printf("release lock in remove pid %d\n", pred->pid);
        release(&pred->lock);
        pred = &proc[last_index];
      } while (cas(&pred->next_proc, remove_proc, new_next));  // could be a problem when someone will try also to remove new_next from the list
  while (cas(&proc[remove_proc].next_proc, new_next, -1));   // Reset the next field of the removed process
}

int
proc_index(int pid){                           // instead we could make a field in the proc struct that saves the position
  int index = 0;
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++, index++)
  {
    if (p->pid == pid)
    {
      return index;
    }
  }
  return -1;
}

void
add_to_cpu(struct proc *p, int running_cpu){
  int proc_index = p->proc_index;
  int prev_last = cpus[running_cpu].last_proc;
  do
  {
    prev_last = cpus[running_cpu].last_proc;
    if (cpus[running_cpu].first_proc == -1){              // If there is no one in the ready queue set the proc to be the first proc the cpu run (and last aswell)
      cpus[running_cpu].first_proc = proc_index;
    }
    else {                                                // Oterwise set the last proc to point on the next proc to run and update last_proc field
      proc[prev_last].next_proc = proc_index;
    }
  } while(cas(&cpus[running_cpu].last_proc, prev_last, proc_index));
  printf("cpus first proc is %d\n", cpus[running_cpu].first_proc);
  printf("cpus last proc is %d\n", cpus[running_cpu].last_proc);
                                                       // Find index of process in proc
}