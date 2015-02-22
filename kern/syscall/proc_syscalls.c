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
#include "opt-A2.h"

#if OPT_A2
#include <spl.h>
#include <synch.h>
#include <mips/trapframe.h>

int 
sys_fork(struct trapframe *tf, pid_t *retval) 
{
  // disable interrupts : makes sure addr space doesn't change before copying
  int spl = splhigh();

  // create a new process based on the current process
  // copies the address as well
  struct proc* new_proc = proc_create_forked();
  // Out of memory
  if (new_proc == NULL) {
    splx(spl);
    return ENOMEM;
  }

  // set its parent pid to the pid of the current process
  KASSERT(new_proc->info != NULL);
  KASSERT(curproc->info != NULL);

  new_proc->info->parent_pid = curproc->info->pid; 

  // TODO: Check if there are too many processes for both user and system

  // make a copy of the trap frame so its child has a copy of it 
  // if parent returns to child before child thread executes
  // the forked thread will free this ( goes against RAII but there's no choice :() )
  // maybe use locks + conditional variable to do this?

  struct trapframe *tf_copy = kmalloc(sizeof(struct trapframe));
  if (tf_copy == NULL) {
    splx(spl); 
    return ENOMEM;
  }
  *tf_copy = *tf;

  // create a new thread to enter the forked process

  int err = thread_fork(
      "[forked process]",
      new_proc,
      (void *)enter_forked_process,
      tf_copy, 
      0);

  if (err) {
      splx(spl);
      kfree(tf_copy);
      return err;
  }

  // may need to copy more stuff - file table??

  // set the return value of the parent to the PID of the forked process
  *retval = new_proc->info->pid;

  // lower spl
  splx(spl);
  return 0;
}
#endif

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;

#if OPT_A2
  pid_t cur_pid = p->info->pid;
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

#if OPT_A2
  // Remove it from the process info table
  // handles cleanup of unused process information
  proc_table_process_exited(cur_pid, exitcode);
#endif

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{

#if OPT_A2
  KASSERT(curproc != NULL);
  *retval = curproc->info->pid;
#else
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
#endif

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

  if (options != 0) {
    return(EINVAL);
  }

#if OPT_A2
  struct proc_info *child_proc_info = proc_table_get_process_info(pid);

  // The pid argument named a nonexistent process.
  if (child_proc_info == NULL) {
    return ESRCH;
  }

  // The pid argument named a process that the current process 
  // was not interested in or that has not yet exited.
  if (child_proc_info->parent_pid != curproc->info->pid) {
    return ECHILD;
  }

lock_acquire(child_proc_info->lock);
  // called before a child has exited, so it must block until the child exits
  while (child_proc_info->status != _PROC_EXITED) {
    cv_wait(child_proc_info->exited_cv, child_proc_info->lock);
  }

  KASSERT(child_proc_info->status == _PROC_EXITED);
lock_release(child_proc_info->lock);

  // child should have exited at this point, 
  // with its information still stored in its process table

  exitstatus = _MKWAIT_EXIT(child_proc_info->exit_code);
#else
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.
     Fix this!
  */

  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}
