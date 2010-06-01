/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>


#include "shell.h"
#include "main.h"
#include "mail.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
#include "expand.h"
#include "eval.h"
#include "jobs.h"
#include "input.h"
#include "trap.h"
#include "var.h"
#include "show.h"
#include "memalloc.h"
#include "error.h"
#include "init.h"
#include "mystring.h"
#include "exec.h"
#include "cd.h"

#ifdef HETIO
#include "hetio.h"
#endif

#define PROFILE 0

int rootpid;
int shlvl;
#ifdef __GLIBC__
int *dash_errno;
#endif
#if PROFILE
short profile_buf[16384];
extern int etext();
#endif

// Thread local storage stuff
pthread_key_t tlskey;

void tlsDestroy(void* tlsmemory) {
	free(tlsmemory);
}

STATIC void read_profile(const char *);
STATIC char *find_dot_file(char *);
static int cmdloop(int);
static void *parseloop(void *);
static void *jobloop(void *);
static void *evaltree_thread(void *);
static void node_process(union node *node);
int main(int, char **);

struct et_args
{
	union node* node;
	struct dg_list* fnode;
};

/*
 * Main routine.  We initialize things, parse the arguments, execute
 * profiles if we're a login shell, and then call cmdloop to execute
 * commands.  The setjmp call sets up the location to jump to when an
 * exception occurs.  When an exception occurs the variable "state"
 * is used to figure out how far we had gotten.
 */

int
main(int argc, char **argv)
{
	char *shinit;
	volatile int state;
	struct jmploc jmploc;
	struct stackmark smark;
	int login;

#ifdef __GLIBC__
	dash_errno = __errno_location();
#endif

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif
	state = 0;
	if (unlikely(setjmp(jmploc.loc))) {
		int e;
		int s;

		reset();

		e = exception;
		if (e == EXERROR)
			exitstatus = 2;

		s = state;
		if (e == EXEXIT || s == 0 || iflag == 0 || shlvl)
			exitshell();
		if (e == EXINT
#if ATTY
		 && (! attyset() || equal(termval(), "emacs"))
#endif
		 ) {
			out2c('\n');
#ifdef FLUSHERR
			flushout(out2);
#endif
		}
		popstackmark(&smark);
		FORCEINTON;				/* enable interrupts */
		if (s == 1)
			goto state1;
		else if (s == 2)
			goto state2;
		else if (s == 3)
			goto state3;
		else
			goto state4;
	}
	handler = &jmploc;
#ifdef DEBUG
	opentrace();
	trputs("Shell args:  ");  trargs(argv);
#endif
	rootpid = getpid();
	init();
	setstackmark(&smark);
	login = procargs(argc, argv);
	if (login) {
		state = 1;
		read_profile("/etc/profile");
state1:
		state = 2;
		read_profile("$HOME/.profile");
	}
state2:
	state = 3;
	if (
#ifndef linux
		getuid() == geteuid() && getgid() == getegid() &&
#endif
		iflag
	) {
		if ((shinit = lookupvar("ENV")) != NULL && *shinit != '\0') {
			read_profile(shinit);
		}
	}
	popstackmark(&smark);
state3:
	state = 4;
	if (minusc)
		evalstring(minusc, 0);

	if (sflag || minusc == NULL) {
state4:	/* XXX ??? - why isn't this before the "if" statement */
		cmdloop(1);
TRACE(("CMDLOOP ret\n"));
	}
#if PROFILE
	monitor(0);
#endif
#if GPROF
	{
		extern void _mcleanup(void);
		_mcleanup();
	}
#endif
TRACE(("EXIT SHELL\n"));
	exitshell();
	/* NOTREACHED */
}


/*
 * Read and execute commands.  "Top" is nonzero for the top level command
 * loop; it turns on prompting if the shell is interactive.
 */

static int
cmdloop(int top)
{
	union node *n;
	pthread_t thread;
	int numeof = 0;
	struct stackmark smark;
	struct dg_list *frontier_node;

	TRACE(("cmdloop(%d) called\n", &top));
#ifdef HETIO
	if(iflag && top)
		hetio_init();
#endif

	setstackmark(&smark);

	dg_graph_init();
	/* Start parseloop. */
	pthread_create (&thread, NULL, parseloop, (void *) &top);
	pthread_detach (thread);

	/* Start jobloop. */
	pthread_create (&thread, NULL, jobloop, NULL);

	while (1) {
		frontier_node = dg_graph_run();
		if (frontier_node) {
			TRACE(("CMDLOOP: pulled %d\n", frontier_node->node->command->type));
			n = frontier_node->node->command;
		}
		else {
			TRACE(("CMDLOOP: pulled null\n"));
			continue;
		}

		/* showtree(n); DEBUG */
		if (n == NEOF) {
			if (!top || numeof >= 50)
				break;
			if (!stoppedjobs()) {
				if (!Iflag)
					break;
				out2str("\nUse \"exit\" to leave shell.\n");
			}
			numeof++;
		} else if (nflag == 0) {
			job_warning = (job_warning == 2) ? 1 : 0;
			numeof = 0;
			if (n->type != NVAR) {
				evaltree(n, 0, frontier_node);
			}
			else {
				pthread_t thread;
				struct et_args *args = malloc (sizeof *args);
				args->node = n->nvar.com;
				args->fnode = frontier_node;
				pthread_key_create(&tlskey, tlsDestroy);
				pthread_create (&thread, NULL, evaltree_thread,
						args);
				pthread_detach (thread);
			}
		}
	}
	popstackmark(&smark);

	pthread_cancel (thread);
	pthread_join (thread, NULL);

	return 0;
}


static void *
parseloop (void *topp)
{
	int inter;
	int top = *(int *) topp;
	union node *n;

	TRACE(("PARSELOOP entered\n"));

	for (;;) {
		int skip;

		inter = 0;
		if (iflag && top) {
			inter++;
			chkmail();
		}

		/* parsecmd returns a node. Wrap decides whether or not
		   to wrap it with NBACKGND. */
		n = parsecmd(inter);

		TRACE(("NODE TYPE: %i\n", n->type));

		if (n == NEOF)
			TRACE(("PARSELOOP: parsecmd ret EOF\n"));
		else if (n)
			TRACE(("PARSELOOP: parsecmd ret type %d\n", n->type));
		else
			continue;

		node_process (n);

		skip = evalskip;
		if (skip) {
			evalskip = 0;
			break;
		}
	}

	TRACE(("PARSELOOP return.\n"));
	return NULL;
}


/* Continously show job statuses. */
static void *
jobloop (void *data)
{
  pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);

  /* The wait that showjobs calls wont block if there aren't any child
     processes. Thus, make sure frontier is non-empty, or else calling
     it is pointless. */
  while (1)
    {
      /* This function does a condwait if frontier is empty. */
      TRACE(("JOBLOOP: looping\n"));
      dg_frontier_nonempty ();
      showjobs(out2, SHOW_CHANGED);
      pthread_testcancel ();
    }

  return NULL;
}


/* Call evaltree in a thread. */
static void *
evaltree_thread (void *data)
{
	struct et_args *arg = (struct et_args *) data;
	TRACE(("EVALTREE_THREAD: call evaltree.\n"));

	// Allocate thread local storage and store var_queue data into it
	void* temp = malloc(sizeof(struct var_state*) + sizeof(pthread_cond_t*));
	pthread_setspecific(tlskey, temp);
	void* tls = pthread_getspecific(tlskey);
	*(struct var_state**)tls = arg->fnode->node->write_state;
	*(pthread_cond_t**)(tls+sizeof(struct var_state*)) = arg->fnode->node->wait_cond;
	evaltree (arg->node, 0, NULL);
	dg_frontier_remove (arg->fnode);
	free (arg);

	TRACE(("EVALTREE_THREAD: return.\n"));
	return NULL;
}


/* Process a node for addition into dgraph. */
static void
node_process(union node *n)
{
  /* Special case: EOF. */
  if (n == NEOF)
    {
      TRACE(("Nodetype: NEOF\n"));
      dg_graph_add (n);
      pthread_exit(NULL);
    }
  else if (!n)
    return;

  switch (n->type) {
  case NCMD:
    TRACE(("NODE_PROCESS: NCMD\n"));
    int wrap = 0;
    if (n->ncmd.args && n->ncmd.args->narg.text)
      {
        TRACE(("NCMD: ARGS %s\n", n->ncmd.args->narg.text));

        /* Check for commands we do NOT backgound on.
           This includes: cd and exit.
           TODO: think of more. */
        if (strcmp (n->ncmd.args->narg.text, "cd") == 0
            || strcmp (n->ncmd.args->narg.text, "exit") == 0)
          wrap = 0;
        else
          wrap = 1;
      }
    else if (n->ncmd.assign && n->ncmd.assign->narg.text)
      {
        TRACE(("NCMD: ASSIGN next %p\n", n->ncmd.assign->narg.next));
        TRACE(("NCMD: ASSIGN text %s\n", n->ncmd.assign->narg.text));
        TRACE(("NCMD: ASSIGN bqte %p\n", n->ncmd.assign->narg.backquote));
        struct nodelist *tmp = n->ncmd.assign->narg.backquote;
        while (tmp)
          {
            TRACE(("NODELIST: type %d\n", tmp->n->type));
            if (tmp->n->type == NCMD)
              TRACE(("NCMD: ARGS text %s\n", tmp->n->ncmd.args->narg.text));
            tmp = tmp->next;
          }
        wrap = 2;
      }
    else
      wrap = 0;

    if (wrap == 1)
      {
        TRACE(("Wrapping with NBACKGND\n"));
        union node *nwrap = (union node *) malloc (sizeof (struct nredir));
        nwrap->type = NBACKGND;
        nwrap->nredir.n = n;
        nwrap->nredir.redirect = NULL;
        n = nwrap;
      }
    else if (wrap == 2)
      {
        TRACE(("Wrapping with NVAR\n"));
        union node *nwrap = (union node *) malloc (sizeof (struct nvar));
        nwrap->type = NVAR;
        nwrap->nvar.com = n;
        n = nwrap;
      }

    if (n->type != NCMD)
      {
	TRACE(("Nodetype: %i\n", n->type));
        struct dg_node* new_node = dg_graph_add (n);

        // Process var_queues
        if (n->type == NVAR) {
        	const char* var_eq = n->nvar.com->ncmd.assign->narg.text;
			struct var* var = setvareq(var_eq, 0);
			var_queue_write(var, new_node);
			var_queue_dump(var);
        }
      }
    else
      /* NCMD: for now, only cd and exit. */
      evaltree (n, 0, NULL);
    break;
  case NVAR:
    TRACE(("NODE_PROCESS: NVAR\n"));
    break;
  case NPIPE:
    TRACE(("NODE_PROCESS: NPIPE\n"));
    break;
  case NREDIR:
  case NBACKGND:
  case NSUBSHELL:
    TRACE(("NODE_PROCESS: NREDIR\n"));
    break;
  case NAND:
  case NOR:
  case NSEMI:
    TRACE(("NODE_PROCESS: NSEMI\n"));
    if (n->nbinary.ch1)
      node_process (n->nbinary.ch1);
    if (n->nbinary.ch2)
      node_process (n->nbinary.ch2);
    break;
  case NWHILE:
  case NUNTIL:
    TRACE(("NODE_PROCESS: NBINARY\n"));
    break;
  case NIF:
    TRACE(("NODE_PROCESS: NIF\n"));
    if (n->nif.test)
      {
        TRACE(("NODE_PROCESS: nif.text->type %d\n", n->nif.test->type));
      }
    if (n->nif.ifpart)
      {
        TRACE(("NODE_PROCESS: nif.ifpart->type %d\n", n->nif.ifpart->type));
        TRACE(("NODE_PROCESS: nsemi 1 type %d\n", n->nif.ifpart->nbinary.ch1->type));
        TRACE(("NODE_PROCESS: nsemi 2 type %d\n", n->nif.ifpart->nbinary.ch2->type));
      }
    if (n->nif.elsepart)
      {
        TRACE(("NODE_PROCESS: nif.elsepart->type %d\n", n->nif.elsepart->type));
      }
    dg_graph_add (n);
    break;
  case NFOR:
    break;
  case NCASE:
    break;
  case NCLIST:
    break;
  case NDEFUN:
  case NARG:
    break;
  case NTO:
  case NCLOBBER:
  case NFROM:
  case NFROMTO:
  case NAPPEND:
    break;
  case NTOFD:
  case NFROMFD:
    break;
  case NHERE:
  case NXHERE:
    break;
  case NNOT:
    break;
  default:
    break;
  }
}


/*
 * Read /etc/profile or .profile.  Return on error.
 */

STATIC void
read_profile(const char *name)
{
	name = expandstr(name);
	if (setinputfile(name, INPUT_PUSH_FILE | INPUT_NOFILE_OK) < 0)
		return;

	cmdloop(0);
	popfile();
}



/*
 * Read a file containing shell functions.
 */

void
readcmdfile(char *name)
{
	setinputfile(name, INPUT_PUSH_FILE);
	cmdloop(0);
	popfile();
}



/*
 * Take commands from a file.  To be compatible we should do a path
 * search for the file, which is necessary to find sub-commands.
 */


STATIC char *
find_dot_file(char *basename)
{
	char *fullname;
	const char *path = pathval();
	struct stat statb;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/'))
		return basename;

	while ((fullname = padvance(&path, basename)) != NULL) {
		if ((stat(fullname, &statb) == 0) && S_ISREG(statb.st_mode)) {
			/*
			 * Don't bother freeing here, since it will
			 * be freed by the caller.
			 */
			return fullname;
		}
		stunalloc(fullname);
	}

	/* not found in the PATH */
	sh_error("%s: not found", basename);
	/* NOTREACHED */
}

int
dotcmd(int argc, char **argv)
{
	int status = 0;

	if (argc >= 2) {		/* That's what SVR2 does */
		char *fullname;

		fullname = find_dot_file(argv[1]);
		setinputfile(fullname, INPUT_PUSH_FILE);
		commandname = fullname;
		cmdloop(0);
		popfile();
		status = exitstatus;
	}
	return status;
}


int
exitcmd(int argc, char **argv)
{
	if (stoppedjobs())
		return 0;
	if (argc > 1)
		exitstatus = number(argv[1]);
	exraise(EXEXIT);
	/* NOTREACHED */
}
