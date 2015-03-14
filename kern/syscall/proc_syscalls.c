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
#include <array.h>

#include <kern/fcntl.h>
#include <vfs.h>

static void copy_argv_to_kern(char **argv, char **argv_kern, int arg_num) {

  for (int idx = 0; idx < arg_num; idx ++) {

    DEBUG(DB_PROC_SYSCALL, "---------------\n");
    DEBUG(DB_PROC_SYSCALL, "argv[%d]: %s \n", idx, argv[idx]);

    int char_length = strlen(argv[idx]);
    DEBUG(DB_PROC_SYSCALL, "length of argv[%d]: %d \n", idx, char_length);

    void *usersrc = argv[idx];

    // copy it into kernel space

    argv_kern[idx] = kmalloc( (char_length + 1) * sizeof(char));

    size_t result_str_len;
    copyinstr(usersrc, argv_kern[idx], char_length * sizeof(char), &result_str_len);
  }
}

static void copy_argv_to_user_stack(char **argv_kern, int num_args, vaddr_t *stackptr){

  int char_addresses[num_args];

  // copy each string to the user stack
  for (int idx = num_args-1; idx >= 0; idx --) {
    DEBUG(DB_PROC_SYSCALL, "--------\n");

    const char *str = argv_kern[idx];
    int str_len = strlen(str) + 1; // includes null terminator
    DEBUG(DB_PROC_SYSCALL, "str_len: %d\n", str_len);  

    // pad by 4 bits so that its pointers are bit aligned
    int padding = 0;
    if (str_len % 4 != 0) {
      padding = 4 - str_len % 4;
    }

    DEBUG(DB_PROC_SYSCALL, "Padding: %d \n", padding);

    *stackptr -= (str_len + padding);
    
    size_t bytes_copied;
    copyoutstr(str, (userptr_t) *stackptr, (size_t)str_len, &bytes_copied);

    char_addresses[idx] = *stackptr;

    DEBUG(DB_PROC_SYSCALL, "Bytes Copied: %d \n", bytes_copied);
  }

  // copy each char pointer to the user stack
  for (int idx = num_args-1; idx >= 0; idx --) {
      // copy char pointer
      *stackptr -= 4;
      copyout((void *)&char_addresses[idx], (userptr_t)*stackptr, 4);
  }

  // copy pointer to char pointers (char ** argv)
  copyout((void *)stackptr, (userptr_t)(*stackptr - 4), 4);
  *stackptr -= 4;
};

int
sys_execv(struct trapframe *tf, pid_t *retval) {
  int spl = splhigh();

  *retval = 0;
  int result;
  vaddr_t entrypoint, stackptr;

  char *progname = (char *)tf->tf_a0;
  struct vnode *v = curproc->p_cwd;

  // Open the file using the current working directory. 
  result = vfs_open(progname, O_RDONLY, 0, &v);
  if (result) {
    return result;
  }

  char **argv = (char **)tf->tf_a1;
  
  int argc = 0;
  while (argv[argc] != NULL) {
    argc ++;
  }

  char **argv_kern = kmalloc(argc * sizeof(char *));

  copy_argv_to_kern(argv, argv_kern, argc);

  // replace the address space of the calling process 
  // with a new address space containing a new program.
  struct addrspace *as;
  as_deactivate();
  as = curproc_setas(NULL);
  as_destroy(as); // destroy the old address space

  struct addrspace *entering_as = as_create();
  if (entering_as ==NULL) {
    kfree(argv_kern);
    vfs_close(v);
    return ENOMEM;
  }
  // use the newly created address space
  curproc_setas(entering_as);
  as_activate();

  // Load the executable.
  result = load_elf(v, &entrypoint);
  if (result) {
    kfree(argv_kern);
    vfs_close(v);
    return result;
  }

  // Done with the file now.
  vfs_close(v);

  // Define the user stack in the address space
  result = as_define_stack(curproc->p_addrspace, &stackptr);
  if (result) {
    kfree(argv_kern);
    return result;
  }

  // copy the strings to the user stack
  copy_argv_to_user_stack(argv_kern, argc, &stackptr);

  char **argv_user = (char **)stackptr;

  kfree(argv_kern);
  splx(spl);

  enter_new_process(argc - 1 /*argc*/, (userptr_t) *argv_user /*userspace addr of argv*/,
        stackptr, entrypoint);

  // should not reach here
  panic("Should be in the execv function");
  return 0;
}

int 
sys_fork(struct trapframe *tf, pid_t *retval) 
{
  // disable interrupts : makes sure addr space doesn't change before copying
  int spl = splhigh();
  int err;

  // create a new process based on the current process
  // copies the address as well
  struct proc* new_proc = proc_create_runprogram("[Forked]");
  // Out of memory
  if (new_proc == NULL) {
    splx(spl);
    return ENOMEM;
  }

  // copy the current process's address space
  struct addrspace *cur_addrspace = curproc_getas();
  struct addrspace *as;
  err = as_copy(cur_addrspace, &as);
  if (err) {
    return ENOMEM;
  }
  new_proc->p_addrspace = as;

  // set its parent pid to the pid of the current process
  KASSERT(new_proc->info != NULL);
  KASSERT(curproc->info != NULL);

  new_proc->info->parent_pid = curproc->info->pid; 

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

  err = thread_fork(
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
