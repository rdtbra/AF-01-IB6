/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		sch.c
 *	DESCRIPTION:	Voluntary thread scheduler
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 */

#include "../jrd/ib_stdio.h"
#include <stdlib.h>
#include "../jrd/common.h"
#include "../jrd/thd.h"
#include "../jrd/isc.h"
#include "../jrd/gds.h"
#include "../jrd/gds_proto.h"
#include "../jrd/isc_s_proto.h"
#include "../jrd/sch_proto.h"
#include "../jrd/thd_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/iberr_proto.h"
#include "../jrd/gdsassert.h"

#ifdef	WIN_NT
#include <windows.h>
#endif

#ifdef APOLLO
#include <apollo/base.h>
#include <apollo/error.h>
#include <apollo/task.h>
#include <apollo/pfm.h>
#include <apollo/errlog.h>
#endif

/* must be careful with alignment on structures like this that are
   not run through the ALL routine */

typedef struct thread {
    struct thread	*thread_next;		/* Next thread to be scheduled */
    struct thread	*thread_prior;		/* Prior thread */
    EVENT_T		thread_stall [1];	/* Generic event to stall thread */
    SLONG		thread_id;		/* Current thread id */
    USHORT		thread_count;		/* AST disable count */
    USHORT		thread_flags;		/* Flags */
} *THREAD;

#define THREAD_hiber		1	/* Thread is hibernating */
#define THREAD_ast_disabled	2	/* Disable AST delivery */
#define THREAD_ast_active	4	/* Disable AST preemption while AST active */
#define	THREAD_ast_pending	8	/* AST waiting to be delivered */

static THREAD	alloc_thread (void);
static BOOLEAN	ast_enable (void);
static void	ast_disable (void);
static void	cleanup (void *);
static void	mutex_bugcheck (TEXT *, int);
static BOOLEAN	schedule (void);
static BOOLEAN	schedule_active (int);
static void	stall (THREAD);
static void	stall_ast (THREAD);

static THREAD	free_threads = NULL;
static THREAD	active_thread = NULL;
static THREAD	ast_thread = NULL;
static MUTX_T	thread_mutex [1];
static USHORT	init_flag=FALSE, multi_threaded=FALSE, enabled=FALSE;

#ifdef NETWARE_386
static USHORT	yield_count = 0;
#define YIELD_LIMIT      10
#define ABORT	exit (FINI_ERROR)
#else
#define ABORT	abort()
#endif

#ifdef VMS
int API_ROUTINE gds__ast_active (void)
{
/**************************************
 *
 *	g d s _ $ a s t _ a c t i v e
 *
 **************************************
 *
 * Functional description
 *	An asynchronous operation has completed, wake somebody
 *	up, if necessary.
 *
 **************************************/

return THREAD_ast_active();
}

void API_ROUTINE gds__completion_ast (
    int		*arg)    
{
/**************************************
 *
 *	g d s _ $ c o m p l e t i o n _ a s t
 *
 **************************************
 *
 * Functional description
 *	An asynchronous operation has completed, wake somebody
 *	up, if necessary.
 *
 **************************************/

THREAD_completion_ast();
sys$wake (0,0);
}
#endif

int API_ROUTINE gds__thread_enable (
  int enable_flag)
{
/**************************************
 *
 *	g d s _ $ t h r e a d _ e n a b l e
 *
 **************************************
 *
 * Functional description
 *	Check-in with thread traffic cop.
 *
 **************************************/

  /* RDT: 20260318 - com -1, enable_flag o if vai ser validado como true, então estamos habilitando 
     o sistema de threads. Em seguida SCH_init é chamado. */	
  if (enable_flag)
  {
    enabled = TRUE;
    SCH_init();
	/* RDT: 20260318 - Agora, com enable_flag negativo, chamaremos THD_init */
    if (enable_flag < 0 && !multi_threaded)
    {
	  multi_threaded = TRUE;
	  THD_INIT;
	}
  }

  return enabled;
}

void API_ROUTINE gds__thread_enter (void)
{
/* RDT: 20230618 - Pelo que vi esta função é chamada quando não temos SUPERSERVER definido. 
   Porém, no geral ela chamará SCH_enter também. O contexto pode ser diferente. Aos poucos
   vamos entender, creio */
/**************************************
 *
 *	g d s _ $ t h r e a d _ e n t e r
 *
 **************************************
 *
 * Functional description
 *	Check-in with thread traffic cop.
 *
 **************************************/

  SCH_enter();
}

void API_ROUTINE gds__thread_exit (void)
{
/* RDT: 20230620 - gds__thread_exit apenas chama SCH_exit. */
/**************************************
 *
 *	g d s _ $ t h r e a d _ e x i t
 *
 **************************************
 *
 * Functional description
 *	Check-out with thread traffic cop.
 *
 **************************************/
  /* RDT: 20230620 - chamar a verdadeira função, que retirará a thread da lista circular. */
  SCH_exit ();
}

#ifdef VMS
int API_ROUTINE gds__thread_wait (
    FPTR_INT	entrypoint,
    SLONG	arg)
{
/**************************************
 *
 *	g d s _ $ t h r e a d _ w a i t
 *
 **************************************
 *
 * Functional description
 *	Stall a thread with a callback to determine runnability.
 *
 **************************************/

return thread_wait (entrypoint, arg);
}
#endif

void SCH_abort (void)
{
/**************************************
 *
 *	S C H _ a b o r t
 *
 **************************************
 *
 * Functional description
 *	Try to abort the current thread.  If the thread is active,
 *	unlink it.
 *
 **************************************/
THREAD	*ptr, thread;
SLONG	id;
int	mutex_state;

/* If threading isn't active, don't sweat it */

if (!active_thread)
    return;

/* See if we can find thread.  If not, don't worry about it */

id = THD_get_thread_id();
for (ptr = &active_thread; thread = *ptr; ptr = &thread->thread_next)
    {
    if (thread->thread_id == id)
	break;
    if (thread->thread_next == active_thread)
	return;
    }

/* If we're the active thread, do a normal thread exit */

if (thread == active_thread)
    {
    SCH_exit();
    return;
    }

/* We're on the list but not active.  Remove from list */

if (mutex_state = THD_mutex_lock (thread_mutex))
    mutex_bugcheck ("mutex lock", mutex_state);
thread->thread_prior->thread_next = thread->thread_next;
thread->thread_next->thread_prior = thread->thread_prior;
thread->thread_next = free_threads;
free_threads = thread;
if (mutex_state = THD_mutex_unlock (thread_mutex))
    mutex_bugcheck ("mutex unlock", mutex_state);
}

void SCH_ast (
    enum ast_t	action)
{
/**************************************
 *
 *	S C H _ a s t
 *
 **************************************
 *
 * Functional description
 *	Control AST delivery for threaded platforms.
 *	In particular, make AST delivery non-preemptible
 *	to avoid nasty race conditions.
 *
 *	In case you're wondering: AST = Asynchronous System Trap
 *
 **************************************/
int	mutex_state;
THREAD	thread;

if (!ast_thread && !(action == AST_alloc || action == AST_disable ||
		     action == AST_enable))
    {
    /* Better be an AST thread before we do anything to it! */
    assert (FALSE);
    return;
    }

if (ast_thread && action == AST_check)
    if (!(ast_thread->thread_flags & THREAD_ast_pending) ||
	ast_thread->thread_count > 1)
	return;

if (!init_flag)
    SCH_init();

if (mutex_state = THD_mutex_lock (thread_mutex))
    mutex_bugcheck ("mutex lock", mutex_state);

switch (action)
    {
    /* Check into thread scheduler as AST thread */
       
    case AST_alloc:
	ast_thread = alloc_thread();
	ast_thread->thread_flags = THREAD_ast_disabled;
	ast_thread->thread_prior = ast_thread->thread_next = ast_thread;
	break;

    case AST_init:
	ast_thread->thread_id = THD_get_thread_id();
	break;
	
    /* Check out of thread scheduler as AST thread */
	
    case AST_fini:
	ast_thread->thread_next = free_threads;
	free_threads = ast_thread;
	ast_thread = NULL;
	break;
	
    /* Disable AST delivery if not an AST thread */
	
    case AST_disable:
	ast_disable();
	break;

    /* Reenable AST delivery if not an AST thread */
	
    case AST_enable:
	ast_enable();
	break;

    /* Active thread allows a pending AST to be delivered
       and waits */

    case AST_check:
	if (ast_enable())
	    stall (active_thread);
	else
	    ast_disable();
	break;

    /* Request AST delivery and prevent thread scheduling */
	
    case AST_enter:
	if (ast_thread->thread_flags & THREAD_ast_disabled)
	    {
	    ast_thread->thread_flags |= THREAD_ast_pending;
	    stall_ast (ast_thread);
	    }
	ast_thread->thread_flags |= THREAD_ast_active;
	break;

    /* AST delivery complete; resume thread scheduling */
	
    case AST_exit:
	ast_thread->thread_flags &= ~(THREAD_ast_active | THREAD_ast_pending);
	if (active_thread)
       	    ISC_event_post (active_thread->thread_stall);

	/* Post non-active threads that have requested AST disabling */

	for (thread = ast_thread->thread_next; thread != ast_thread; thread = thread->thread_next)
	    ISC_event_post (thread->thread_stall);
	break;
    }

if (mutex_state = THD_mutex_unlock (thread_mutex))
    mutex_bugcheck ("mutex unlock", mutex_state);
}

THREAD SCH_current_thread (void)
{
/**************************************
 *
 *	S C H _ c u r r e n t _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *	Return id of current thread.  If scheduling is not active,
 *	return NULL.
 *
 **************************************/

return active_thread;
}

void SCH_enter (void)
{
/* RDT: 20230618 - Este parece ser o ponto onde o scheduler interno de threads é
   iniciado. */	
/**************************************
 *
 *	S C H _ e n t e r
 *
 **************************************
 *
 * Functional description
 *	A thread wants to enter the access method and submit to our scheduling.
 *	Humor him.
 *
 **************************************/
  THREAD thread, prior;
  int    mutex_state;

#ifdef NeXT
  return;
#endif

  /* Special case single thread case */
  if (!multi_threaded)
  {
    if (active_thread || ast_thread)
      multi_threaded = TRUE;
    else
      if (free_threads)
      {
        thread = active_thread = free_threads;
        free_threads = NULL;
        thread->thread_next = thread->thread_prior = thread;
        thread->thread_flags = 0;
        thread->thread_id = THD_get_thread_id();
        return;
      }
  }

  if (!init_flag)
    SCH_init();

  /* Get mutex on scheduler data structures to prevent tragic misunderstandings */
  if (mutex_state = THD_mutex_lock (thread_mutex))
    mutex_bugcheck ("mutex lock", mutex_state);

  thread = alloc_thread();
  thread->thread_id = THD_get_thread_id();

  /* Link thread block into circular list of active threads */
  if (active_thread)
  {
    /* The calling thread should NOT be the active_thread 
       This is to prevent deadlock by the same thread */
    assert (thread->thread_id != active_thread->thread_id);
    thread->thread_next = active_thread;
    thread->thread_prior = prior = active_thread->thread_prior;
    active_thread->thread_prior = thread;
    prior->thread_next = thread;
  }
  else
  {
    thread->thread_next = thread->thread_prior = thread;
    active_thread = thread;
  }

  if (active_thread->thread_flags & THREAD_hiber)
    /* RDT: 20230618 - Deve executar a thread. */
    /* RDT: 20230620 - Não é verdade, chamar schedule vai justamente colocar outra thread como ativa. */
    schedule();

  /* RDT: 20230618 - Em seguida parece que vai parar a execução da thread, mas preciso estudar os dois mecanismos. */ 
  /* RDT: 20230620 - É o contrário aqui também, ou seja, stall vai esperar esta thread se tornar a thread ativa. Neste momento,
     stall retornará */
  stall (thread);
	
  if (mutex_state = THD_mutex_unlock (thread_mutex))
    mutex_bugcheck ("mutex unlock", mutex_state);
}

void SCH_exit (void)
{
/* RDT: 20230620 - Esta função remove a thead da lista circular e chama schedule. */
/**************************************
 *
 *	S C H _ e x i t
 *
 **************************************
 *
 * Functional description
 *	Thread is done in access method, remove data structure from
 *	scheduler, and release thread block.
 *
 **************************************/
  THREAD thread, prior, next;
  int    mutex_state;

#ifdef NeXT
  return;
#endif

  SCH_validate();

  if (!multi_threaded && !ast_thread)
  {
    free_threads = active_thread;
    active_thread = NULL;
    free_threads->thread_next = NULL;
    return;
  }

  if (mutex_state = THD_mutex_lock (thread_mutex))
    mutex_bugcheck ("mutex lock", mutex_state);

  ast_enable(); /* Reenable AST delivery */

  thread = active_thread;

  if (thread == thread->thread_next)
    active_thread = NULL;
  else
  {
    next = thread->thread_next;
    active_thread = prior = thread->thread_prior;
    prior->thread_next = next;
    next->thread_prior = prior;
  }

  thread->thread_next = free_threads;
  free_threads = thread;
	
  /* RDT: 20230620 - Lembrar que schedule vai chamar SetEvent. */
  schedule();

  if (mutex_state = THD_mutex_unlock (thread_mutex))
    mutex_bugcheck ("mutex unlock", mutex_state);
}

void SCH_hiber (void)
{
/**************************************
 *
 *	S C H _ h i b e r 
 *
 **************************************
 *
 * Functional description
 *	Go to sleep until woken by another thread.  See also
 *	SCH_wake.
 *
 **************************************/

schedule_active (TRUE);
}

void SCH_init (void)
/* RDT: 20260318 - Inicialização do scheduler de threads. */
{
/**************************************
 *
 *	S C H _ i n i t
 *
 **************************************
 *
 * Functional description
 *	Initialize the thread scheduler.
 *
 **************************************/
  int mutex_state;

  /* RDT: 20260318 - Não inicializar se já foi inicializado. :) */
  if (init_flag)
    return;

  init_flag = TRUE;
  /* RDT: 20260318 - Registrar função de cleanup. */
  gds__register_cleanup (cleanup, NULL_PTR);

  /* RDT: 20260318 - Tem um mutex que está sendo inicializado também. */	
  if (mutex_state = THD_mutex_init (thread_mutex))
    mutex_bugcheck ("mutex init", mutex_state);
}

int SCH_schedule (void)
{
/**************************************
 *
 *	S C H _ s c h e d u l e
 *
 **************************************
 *
 * Functional description
 *	Voluntarily relinquish control so that others may run.
 *	If a context switch actually happened, return TRUE.
 *
 **************************************/

return schedule_active (FALSE);
}

BOOLEAN SCH_thread_enter_check (void)
{
/**************************************
 *
 *	S C H _ t h r e a d _ e n t e r _ c h e c k
 *
 **************************************
 *
 * Functional description
 *	Check if thread is active thread, if so return TRUE
 * else return FALSE.
 *
 **************************************/

/* if active thread is not null and thread_id matches the we are the
   active thread */
if ((active_thread) && (active_thread->thread_id == THD_get_thread_id()))
        return (TRUE);

return (FALSE);

}

BOOLEAN SCH_validate (void)
{
/**************************************
 *
 *	S C H _ v a l i d a t e
 *
 **************************************
 *
 * Functional description
 *	Check integrity of thread system (assume thread is entered).
 *
 **************************************/

if (!init_flag || !active_thread)
    {
    gds__log ("SCH_validate -- not entered");
    if (getenv ("ISC_PUNT"))
	ABORT;
    return FALSE;
    }

if (!multi_threaded)
    return TRUE;

if (active_thread->thread_id != THD_get_thread_id())
    {
    gds__log ("SCH_validate -- wrong thread");
    return FALSE;
    }

return TRUE;
}

void SCH_wake (
    THREAD	thread)
{
/**************************************
 *
 *	S C H _ w a k e
 *
 **************************************
 *
 * Functional description
 *	Take thread out of hibernation.
 *
 **************************************/

thread->thread_flags &= ~THREAD_hiber;
ISC_event_post (thread->thread_stall);
}

static THREAD alloc_thread (void)
{
/**************************************
 *
 *	a l l o c _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *	Allocate a thread block.
 *
 **************************************/
THREAD	thread;

/* Find a useable thread block.  If there isn't one, allocate one */

if (thread = free_threads)
    free_threads = thread->thread_next;
else
    {
    thread = (THREAD) gds__alloc ((SLONG) sizeof (struct thread));
    /* FREE: unknown */
    if (!thread)	/* NOMEM: bugcheck */
	mutex_bugcheck ("Out of memory", 0);	/* no real error handling */
#ifdef DEBUG_GDS_ALLOC
    /* There is one thread structure allocated per thread, and this
     * is never freed except by process exit
     */
    gds_alloc_flag_unfreed ((void *)thread);
#endif
    ISC_event_init (thread->thread_stall, 0, 0);
    }

thread->thread_flags = thread->thread_count = 0;
return thread;
}

static BOOLEAN ast_enable (void)
{
/**************************************
 *
 *	a s t _ e n a b l e
 *
 **************************************
 *
 * Functional description
 *	Enables AST delivery and returns
 *	TRUE is an AST is deliverable.
 *
 **************************************/

if (!ast_thread)
    return FALSE;

if (ast_thread->thread_flags & THREAD_ast_active &&
    ast_thread->thread_id == THD_get_thread_id())
    return FALSE;

if (!ast_thread->thread_count || !--ast_thread->thread_count)
    {
    ast_thread->thread_flags &= ~THREAD_ast_disabled;
    if (ast_thread->thread_flags & THREAD_ast_pending)
	{
	ast_thread->thread_flags |= THREAD_ast_active;
    	ISC_event_post (ast_thread->thread_stall);
	return TRUE;
	}
    }

return FALSE;
}

static void ast_disable (void)
{
/**************************************
 *
 *	a s t _ d i s a b l e
 *
 **************************************
 *
 * Functional description
 *	Disables AST delivery and waits
 *	until an active AST completes
 *	before returning.
 *
 **************************************/
THREAD	thread;

if (!ast_thread)
    return;

if (ast_thread->thread_flags & THREAD_ast_active)
    {
    if (ast_thread->thread_id == THD_get_thread_id())
       	return;
    else
	{
	if (active_thread && active_thread->thread_id == THD_get_thread_id())
	    {
	    stall (active_thread);
	    return;
	    }
	else
	    {
	    thread = alloc_thread();
	    stall_ast (thread);
	    thread->thread_next = free_threads;
	    free_threads = thread;
	    }
	}
    }

ast_thread->thread_flags |= THREAD_ast_disabled;
++ast_thread->thread_count;
}

static void cleanup (
    void	*arg)
{
/**************************************
 *
 *	c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Exit handler for image exit.
 *
 **************************************/

THREAD	temp_thread;

if (init_flag == FALSE)
    return;

#ifdef APOLLO
init_flag = multi_threaded = enabled = FALSE;
active_thread = free_threads = ast_thread = NULL;
#endif

/* this is added to make sure that we release the memory
 * we have alloacted for the thread event handler through
 * ISC_event_handle () (CreateEvent) */

#ifdef SUPERCLIENT
/* use locks */
THD_mutex_lock (thread_mutex);

if (init_flag == FALSE)
    return;

/* loop through the list of active threads and free the events */
if (temp_thread = active_thread)
    {
    /* reach to the starting of the list */
    while (temp_thread != temp_thread->thread_prior)
	temp_thread = temp_thread->thread_prior;
    /* now we are at the begining of the list. Now start freeing the
     * EVENT structures and move to the next */
    do
        {
        ISC_event_fini (temp_thread->thread_stall);
	/* the thread structures are freed as a part of the 
	 * gds_alloc cleanup, so do not worry about them here
	 */
	}
    while (temp_thread->thread_next != temp_thread
	    && (temp_thread = temp_thread->thread_next));

    }

/* loop through the list of free threads and free the events */
if (temp_thread = free_threads)
    {
    /* reach to the starting of the list */
    while (temp_thread != temp_thread->thread_prior)
	temp_thread = temp_thread->thread_prior;
    /* now we are at the begining of the list. Now start freeing the
     * EVENT structures and move to the next */
    do
        {
        ISC_event_fini (temp_thread->thread_stall);
	/* the thread structures are freed as a part of the 
	 * gds_alloc cleanup, so do not worry about them here
	 */
	}
    while (temp_thread->thread_next != temp_thread
	    && (temp_thread = temp_thread->thread_next));

			
    }

THD_mutex_unlock (thread_mutex);

/* add the the destroy for the thread_mutex */
THD_mutex_destroy (thread_mutex);
#endif /* SUPERCLIENT */

init_flag = FALSE;
}

static void mutex_bugcheck (
    TEXT	*string,
    int		mutex_state)
{
/**************************************
 *
 *	m u t e x _ b u g c h e c k
 *
 **************************************
 *
 * Functional description
 *	There has been a bugcheck during a mutex operation.
 *	Post the bugcheck.
 *
 **************************************/
#ifdef APOLLO
TEXT		*subsys_p, *module_p, *code_p, flags [4];
SSHORT		subsys_l, module_l, code_l, errcode;
status_$t	apollo_status;
#endif
TEXT		msg [128];

sprintf (msg, "SCH: %s error, status = %d", string, mutex_state);
gds__log (msg);

#ifdef NETWARE_386
ConsolePrintf ("%s\n", msg);
#else
ib_fprintf (ib_stderr, "%s\n", msg);
#endif

#ifdef APOLLO
apollo_status.all = mutex_state;
error_$find_text (apollo_status, &subsys_p, &subsys_l, &module_p,
                  &module_l, &code_p, &code_l);
if (subsys_l && code_l)
    ib_fprintf (ib_stderr, "%.*s (%.*s/%.*s)\n", code_l, code_p, subsys_l, subsys_p, module_l, module_p);
else
    ib_fprintf (ib_stderr, "Domain status %X %.*s (%.*s/%.*s)", mutex_state, code_l, code_p, subsys_l, subsys_p, 
		module_l, module_p);
#endif

ABORT;
}

static BOOLEAN schedule (void)
{
/* RDT: 20230618 - E esta parece ser a função que irá disparar o scheduler. Na verdade ela vai circular a lista de threads 
   procurando pela próxima thread a ativar. Isso me dá idéia de que o interbase usa threads do so, mesmo que esteja 
   controlando a execução das threads via sistema operacional. */
/**************************************
 *
 *	s c h e d u l e
 *
 **************************************
 *
 * Functional description
 *	Loop thru active thread to find the next runable task.  If we find one,
 *	set "active_tasks" to point to it and return TRUE.  Otherwise simply
 *	return FALSE.
 *
 **************************************/
  THREAD thread;

#ifdef NETWARE_386
  /* Be a good citizen, yield CPU periodically */
  yield_count++;
  if (yield_count > YIELD_LIMIT)
  {
    yield_count = 0;
    ThreadSwitch();
  }
#endif

  if (!active_thread)
    return FALSE;

  thread = active_thread;

  /* RDT: 20230618 - Este loop percorre a lista de threads verificando, a partir da thread atual, 
     qual a próxima que dever ser selecionada. */
  for (;;)
  {
    thread = thread->thread_next;
    /* RDT: 20230618 - Caso a thread não tenha flag THREAD_hiber, será selecionada */
    if (!(thread->thread_flags & THREAD_hiber))
      break;
    if (thread == active_thread)
      return FALSE;
  }

  /* RDT: 20230618 - A thread se tornará a thread ativa. */
  active_thread = thread;
  /* RDT: 20230618 - Talvez aqui esteja parte da resposta de como funciona o verdadeiro scheduler! 
     ISC - deve ser Interbase Signal Controler. */
  ISC_event_post (active_thread->thread_stall);

  return TRUE;
}

static BOOLEAN schedule_active (
    int		hiber_flag)
{
/**************************************
 *
 *	s c h e d u l e _ a c t i v e
 *
 **************************************
 *
 * Functional description
 *	Voluntarily relinquish control so that others may run.
 *	If a context switch actually happened, return TRUE.
 *
 **************************************/
THREAD	thread;
int	mutex_state;
BOOLEAN	ret;

if (!active_thread || !multi_threaded)
    return FALSE;

if (mutex_state = THD_mutex_lock (thread_mutex))
    mutex_bugcheck ("mutex lock", mutex_state);

/* Take this opportunity to check for pending ASTs
   and deliver them. */

if (ast_enable())
    stall (active_thread);
else
    ast_disable();

if (hiber_flag)
    active_thread->thread_flags |= THREAD_hiber;
thread = active_thread;
schedule();
if (thread == active_thread && !(thread->thread_flags & THREAD_hiber))
    ret = FALSE;
else
    {
    ast_enable();
    stall (thread);
    ret = TRUE;
    }

if (mutex_state = THD_mutex_unlock (thread_mutex))
    mutex_bugcheck ("mutex unlock", mutex_state);

return ret;
}

static void stall (
  THREAD thread)
{
/* RDT: 20230620 - stall é parar! em português. A ideia aqui é que a thread entre em um loop,
   esperando para se tornar a thread ativa, e, neste momento o loop é interrompido. ast_disable
   é executado, neste momento, mas não vou me preocupar com esta chamada agora. */
/**************************************
 *
 *	s t a l l
 *
 **************************************
 *
 * Functional description
 *	Stall until our thread is made active.
 *
 **************************************/
  SLONG value;
  EVENT ptr;
  int   mutex_state;

  if (thread != active_thread || thread->thread_flags & THREAD_hiber ||
     (ast_thread && ast_thread->thread_flags & THREAD_ast_active))
    for (;;)
    {
      /* RDT: 20230620 - Limpar o evento, já que vamos sinalizá-lo novamente logo abaixo. */
      value = ISC_event_clear (thread->thread_stall);

      /* RDT: 20230620 - Aguardar até o momento em que esta thread se torne a thread ativa. 
         Isto deve ocorrer por alguma outra thread executando a função SCH_enter + schedule, 
         que vimos que percorrerá a lista circular de threads, tornando alguma ativa. Quando 
         esta thread for a ativa, saímos do loop. */
      if (thread == active_thread && !(thread->thread_flags & THREAD_hiber) &&
         (!ast_thread || !(ast_thread->thread_flags & THREAD_ast_active)))
        break;

      /* RDT: 20230620 - Esta thread não é a ativa no contexto do Interbase, então, ela 
         vai entrar em estado de espera novamente, chamando ISC_event_wait abaixo. */
      if (mutex_state = THD_mutex_unlock (thread_mutex))
          mutex_bugcheck ("mutex unlock", mutex_state);
        ptr = thread->thread_stall;
	    
      /* RDT: 20230620 - A thread vai entrar em estado de espera novamente. Esta claro que é 
         uma estratégia de um scheduler 'distribuído'. */
      ISC_event_wait (1, &ptr, &value, 0, NULL_PTR, NULL_PTR);
      if (mutex_state = THD_mutex_lock (thread_mutex))
        mutex_bugcheck ("mutex lock", mutex_state);
    }

  /* Explicitly disable AST delivery for active thread */
  ast_disable();
}

static void stall_ast (
    THREAD	thread)
{
/**************************************
 *
 *	s t a l l _ a s t
 *
 **************************************
 *
 * Functional description
 *	If this the AST thread then stall until AST delivery
 *	is reenabled. Otherwise, this is a normal thread (though
 *	not the active thread) and it wants to wait until
 *	AST is complete.
 *
 **************************************/
SLONG	value;
EVENT	ptr;
int	mutex_state;

if (thread == ast_thread)
    {
    if (ast_thread->thread_flags & THREAD_ast_disabled)
    	for (;;)
	    {
	    value = ISC_event_clear (thread->thread_stall);
	    if (!(ast_thread->thread_flags & THREAD_ast_disabled))
	    	break;
	    if (mutex_state = THD_mutex_unlock (thread_mutex))
	    	mutex_bugcheck ("mutex unlock", mutex_state);
	    ptr = thread->thread_stall;
	    ISC_event_wait (1, &ptr, &value, 0, NULL_PTR, NULL_PTR);
	    if (mutex_state = THD_mutex_lock (thread_mutex))
	    	mutex_bugcheck ("mutex lock", mutex_state);
	    }
    }
else
    {
    /* Link thread block into ast thread queue */

    thread->thread_next = ast_thread->thread_next;
    thread->thread_prior = ast_thread;
    ast_thread->thread_next->thread_prior = thread;
    ast_thread->thread_next = thread;

    /* Wait for AST delivery to complete */

    if (ast_thread->thread_flags & THREAD_ast_active)
    	for (;;)
	    {
	    value = ISC_event_clear (thread->thread_stall);
	    if (!(ast_thread->thread_flags & THREAD_ast_active))
	    	break;
	    if (mutex_state = THD_mutex_unlock (thread_mutex))
	    	mutex_bugcheck ("mutex unlock", mutex_state);
	    ptr = thread->thread_stall;
	    ISC_event_wait (1, &ptr, &value, 0, NULL_PTR, NULL_PTR);
	    if (mutex_state = THD_mutex_lock (thread_mutex))
	    	mutex_bugcheck ("mutex lock", mutex_state);
	    }
    /* Unlink thread block from ast thread queue */

    thread->thread_prior->thread_next = thread->thread_next;
    thread->thread_next->thread_prior = thread->thread_prior;
    }
}
