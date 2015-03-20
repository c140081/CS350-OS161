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
#include <vm.h>
#include <vfs.h>
#include <test.h>
#include <kern/fcntl.h>
#include <limits.h>

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

int sys_execv(const char *program, char **args) {
    //kprintf("running execv\n");
    int result;
    int argc =0;
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
    struct addrspace *old_as;
    old_as = curproc_getas();
    if (program == NULL) {
        //no valid program name given
        return (ENOENT);
    }
    //TODO how to check for EISDIR??
    
    //save the program name onto heap so we can keep it
    size_t programLen = strlen(program)+1;
    char *newProgram = kmalloc(sizeof(char*)*programLen);
    result = copyinstr((userptr_t)program, newProgram, programLen, NULL);//final param just leave as NULL as found in copystr
    if(newProgram== NULL) {
        //didnt copy over for some reason, we know that program isnt null
        return(ENOMEM);//assume it was a memory issue
    }
    if(result) {
        return result; //something else went wrong in the copying process
    }
    
    //count the args
    //my interpretation of ARG_MAX indicates that 1024 is the maximum individual argument size
    //also 64 is the max number of args
    while(args[argc] != NULL) {
        if(strlen(args[argc]) >1024) {
            return (E2BIG);
        }
        argc++;
    }
    if(argc >64) {
        return (E2BIG);
    }
    
    //save args like I did for program
    char **newArgs= kmalloc((argc+1)*sizeof(char));//+1 because of null terminator
    for(int i=0; i<argc; i++) {
        newArgs[i]=kmalloc((strlen(args[i])+1)*sizeof(char));
        result=copyinstr((userptr_t)args[i], newArgs[i], strlen(args[i])+1, NULL);
        if(result) {
            return result;
        }
    }
    newArgs[argc] = NULL; //null terminate our new list of args
    
    
    //from runprogram....
    
    /* Open the file. */
	result = vfs_open(newProgram, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	//KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}
	
	
	//force stackptr to be 8-byte aligned
	//kprintf("aligning stackptr\n");
	while(stackptr %8 !=0) {
	    stackptr--;
	}
	
	//easiest to just build this thing backwards
	vaddr_t argptr[argc+1];
	for (int i=argc-1; i>=0; i--) {
	    //stackptr-=ROUNDUP(strlen(args[i])+1,4);//not necessary but makes alignment so much easier when rounded
	    stackptr-=strlen(newArgs[i])+1;//scratch that, just re-align it later!
	    //kprintf("copyoutstr on %s\n", newArgs[i]);
	    //kprintf("size is %d\n", strlen(newArgs[i]));
	    result=copyoutstr(newArgs[i], (userptr_t)stackptr, strlen(newArgs[i])+1, NULL);
	    if(result) {
	        return result;
	    }
	    argptr[i]=stackptr;
	}
	
	//realign the values
	while(stackptr %4 !=0) {
	    stackptr--;
	}
	argptr[argc]=0;//null out
	//now put addresses onto the stack!
	for(int i=argc; i>=0; i--) {
	    stackptr-=ROUNDUP(sizeof(vaddr_t),4);
	    result=copyout(&argptr[i], (userptr_t)stackptr, sizeof(vaddr_t));
	    if(result) {
	        return result;
	    }
	}
	
	//finally destroy the old as
	as_destroy(old_as);
	/* Warp to user mode. */
	enter_new_process(argc, (userptr_t)stackptr,
			  stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}



