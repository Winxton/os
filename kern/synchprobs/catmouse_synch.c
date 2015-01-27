#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>

/* 
 * This simple default synchronization mechanism allows only creature at a time to
 * eat.   The globalCatMouseSem is used as a a lock.   We use a semaphore
 * rather than a lock so that this code will work even before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */

static struct semaphore *num_cats_eating = NULL;
static struct semaphore *num_mice_eating = NULL;

// used for the conditional variables for both mice and cats
static struct lock *mutex = NULL;
// checks whether a mouse can start eating
static struct cv *mice_cv = NULL;
// checks whether a cat can start eating
static struct cv *cat_cv = NULL;

// a CV for EACH bowl
// used to make sure only ONE bowl is being consumed at once
static struct cv **bowl_cvs = NULL;

/* 
 * The CatMouse simulation will call this function once before any cat or
 * mouse tries to each.
 *
 * You can use it to initialize synchronization and other variables.
 * 
 * parameters: the number of bowls
 */
void
catmouse_sync_init(int bowls)
{
  /* replace this default implementation with your own implementation of catmouse_sync_init */

  (void)bowls; /* keep the compiler from complaining about unused parameters */

  // keeps a count of the number of mice currently eating
  num_mice_eating = sem_create("num_mice_eating",0);
  if (num_mice_eating == NULL) {
    panic("could not create global num_mice_eating synchronization semaphore");
  }

  // keeps a count of the number of cats currently eating
  num_cats_eating = sem_create("num_cats_eating",0);
  if (num_cats_eating == NULL) {
    panic("could not create global num_cats_eating synchronization semaphore");
  }

  mutex = lock_create("mutex");
  if (mutex == NULL) {
    panic("mutex lock_create failed\n");
  }

  mice_cv = cv_create("mice_cv");
  if (mice_cv == NULL) {
    panic("mice_cv: cv_create failed\n");
  }

  cat_cv = cv_create("cat_cv");
  if (cat_cv == NULL) {
    panic("cat_cv: cv_create failed\n");
  }

  // create bowl cvs
  bowl_cvs = malloc(bowls * sizeof(struct cv *));
  for (int i=0; i<bowls; i++) {
    bowl_cvs[i] = cv_create("bowl_cv");;
  }

  return;
}

/* 
 * The CatMouse simulation will call this function once after all cat
 * and mouse simulations are finished.
 *
 * You can use it to clean up any synchronization and other variables.
 *
 * parameters: the number of bowls
 */
void
catmouse_sync_cleanup(int bowls)
{
  /* replace this default implementation with your own implementation of catmouse_sync_cleanup */
  (void)bowls; /* keep the compiler from complaining about unused parameters */
  KASSERT(num_cats_eating != NULL);
  sem_destroy(num_cats_eating);

  KASSERT(num_mice_eating != NULL);
  sem_destroy(num_mice_eating);

  lock_destroy(mutex);
  cv_destroy(mice_cv);
  cv_destroy(cat_cv);

  KASSERT(bowl_cvs != NULL);
  // remove bowl cvs
  for (int i=0; i<bowls; i++) {
    cv_destroy(bowl_cvs[i]);
  }
  free(bowl_cvs);
}


/*
 * The CatMouse simulation will call this function each time a cat wants
 * to eat, before it eats.
 * This function should cause the calling thread (a cat simulation thread)
 * to block until it is OK for a cat to eat at the specified bowl.
 *
 * parameter: the number of the bowl at which the cat is trying to eat
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
cat_before_eating(unsigned int bowl) 
{
  /* replace this default implementation with your own implementation of cat_before_eating */
  (void)bowl;  /* keep the compiler from complaining about an unused parameter */
  KASSERT(num_mice_eating != NULL && num_cats_eating != NULL);

  // critical section: this is needed for the conditional variable check on the number of eating mice
  lock_acquire(mutex);

    // oh no! a mouse is currently eating
    while (num_mice_eating->sem_count > 0) {
      cv_wait(cat_cv, mutex); // this would release the lock and sleep until awaken
    }
    
    // increase the number of cats that are eating
    V(num_cats_eating);

  lock_release(mutex);
}

/*
 * The CatMouse simulation will call this function each time a cat finishes
 * eating.
 *
 * You can use this function to wake up other creatures that may have been
 * waiting to eat until this cat finished.
 *
 * parameter: the number of the bowl at which the cat is finishing eating.
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
cat_after_eating(unsigned int bowl) 
{
  /* replace this default implementation with your own implementation of cat_after_eating */
  (void)bowl;  /* keep the compiler from complaining about an unused parameter */
  KASSERT(num_mice_eating != NULL && num_cats_eating != NULL);

  lock_acquire(mutex);
    
    // decrease number of eating cats
    P(num_cats_eating);

    // no more cats are eating, 
    // so I broadcast to all waiting mice that they can start eating
    if (num_cats_eating->sem_count == 0) {
      cv_broadcast(mice_cv, mutex);
    }

  lock_release(mutex);
}

/*
 * The CatMouse simulation will call this function each time a mouse wants
 * to eat, before it eats.
 * This function should cause the calling thread (a mouse simulation thread)
 * to block until it is OK for a mouse to eat at the specified bowl.
 *
 * parameter: the number of the bowl at which the mouse is trying to eat
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
mouse_before_eating(unsigned int bowl) 
{
  /* replace this default implementation with your own implementation of mouse_before_eating */
  (void)bowl;  /* keep the compiler from complaining about an unused parameter */
  KASSERT(num_mice_eating != NULL && num_cats_eating != NULL);

  lock_acquire(mutex);

    // oh no! a cat is currently eating, I must wait!
    while (num_cats_eating->sem_count > 0) {
      cv_wait(mice_cv, mutex); // this would release the lock and sleep until awaken
    }

    // increase the number of mice that are eating
    V(num_mice_eating);

  lock_release(mutex);
}

/*
 * The CatMouse simulation will call this function each time a mouse finishes
 * eating.
 *
 * You can use this function to wake up other creatures that may have been
 * waiting to eat until this mouse finished.
 *
 * parameter: the number of the bowl at which the mouse is finishing eating.
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
mouse_after_eating(unsigned int bowl) 
{
  /* replace this default implementation with your own implementation of mouse_after_eating */
  (void)bowl;  /* keep the compiler from complaining about an unused parameter */
  KASSERT(num_mice_eating != NULL && num_cats_eating != NULL);

  lock_acquire(mutex);

    // decrease number of eating mice
    P(num_mice_eating);

    // if no more mice are eating, then I broadcast to all waiting cats that they can start eating
    if (num_mice_eating->sem_count == 0) {
      cv_broadcast(cat_cv, mutex);
    }

  lock_release(mutex);
}
