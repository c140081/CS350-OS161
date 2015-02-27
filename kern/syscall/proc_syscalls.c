#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>//need to include this so I can use sizeof(struct trapframe)
#include <opt-A2.h>
#include <synch.h>

extern struct array *globalProcs;
  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {
  struct addrspace *as;
  struct proc *p = curproc;
  //assign the exitcode!
  #if OPT_A2
  struct lock *mainLock = grabLock();
  KASSERT(mainLock != NULL);
  lock_acquire(mainLock);
  int spot =findPInfo(p->pid);
  struct procInfo *thisPInfo; 
  thisPInfo = array_get(globalProcs, spot);
  thisPInfo->exitCode=_MKWAIT_EXIT(exitcode); //encode the exit code as discussed in the waitpid man pages
  checkChildren(spot);
  V(thisPInfo->procSem);
  lock_release(mainLock);
  #endif
  
  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* returns the current proc's pid                */
int
sys_getpid(pid_t *retval)
{
    //kprintf("sys_getpid\n");
  *retval = curproc->pid;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* grab the exit status */
  #if OPT_A2
  //TODO error checking
  struct procInfo *pI;
  int spot =findPInfo(pid);
  pI = array_get(globalProcs, spot);
  if(pI == NULL) {
    return (ESRCH);
  }
  if(pI->parent != curproc->pid) {
    return (ECHILD);
  }
  exitstatus = getExit(pid);
  
  #endif
  
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

/*
    Create a copy of the current process
    Return 0 for the child process and the PID of the child to the parent
*/
int sys_fork(struct trapframe *tf, pid_t *retval) {
    //kprintf("now running sys_fork \n");
    //create process structure for child
    struct proc* newProc;
    newProc = proc_create_runprogram("Process");
    //create and copy as from parent to child
    if(newProc == NULL) {
        return (ENOMEM);
    } else if(newProc == (struct proc *)ENPROC) {
        return (ENPROC);
    }
    as_copy(curproc->p_addrspace, &newProc->p_addrspace);//TODO verify usage
    
    //attach newly created as to child structure
    //newProc->p_addrspace = newAs;
    
    //create thread for child
    struct trapframe *newTf;
    newTf = kmalloc(sizeof(struct trapframe));
    //almost forgot to actually copy the tf!!
    //memcpy(newTf, tf, sizeof(struct trapframe));//deep memory copy of the exact trap frame!
    *newTf = *tf;
    //thread_fork found in kern/thread/thread.c
    //data2 seems to be just a spot to save arbitrary data after digging into switchframes... dead end after hitting sf_s2 in switchframe.c
    //just put 0 there for now, dont think we need anything
    KASSERT(newTf != NULL);
    //KASSERT(newProc->p_addrspace->as_pbase1 !=0);
    //kprintf("entering threadfork\n");
    thread_fork("Thread", newProc, (void *)enter_forked_process,newTf, 0);
    //kprintf("returning from threadfork\n");
    //child thread needs to put trapframe on the stack and modify it
    //call mips_usermode
    //^^^ both done in enter_forked_process
    
    *retval = newProc->pid;
    return 0;
}

