/* By Chen Guo */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"
#include "shell.h"
#include "show.h"
#include "var.h"

#include "states.h"

/* From dash. Why 39? */
#define VTABSIZE 39

static struct var2 *vartab[VTABSIZE];

static char * copy_name (char *);
static struct var2 ** hashstate (const char *);
static struct var2 ** findvar (struct var2 **, const char *);
static pthread_mutex_t var_lock;

#define LOCK_VAR {pthread_mutex_lock (&var_lock);}
#define UNLOCK_VAR {pthread_mutex_unlock (&var_lock);}

void
initvar2 (void)
{
  pthread_mutex_init (&var_lock, NULL);
  LOCK_VAR;

  int i;
  for (i = 0; i < VTABSIZE; i++)
    vartab[i] = NULL;

  /* TODO: set up built ins. */
  UNLOCK_VAR;
}

/* Create a new variable state. */
struct var_state *
create_state (char *assignstr)
{
  /* Extract variable name. */
  char *eqp = strchr (assignstr, '=');
  int namelen = (eqp)? eqp - assignstr : strlen (assignstr);
  char *name = malloc (namelen + 1);
  memcpy (name, assignstr, namelen);
  name[namelen] = '\0';

  TRACE(("CREATE STATE for %s\n", name));

  /* Create new state. */
  struct var_state *new_state = malloc (sizeof *new_state);

  new_state->next = NULL;
  new_state->prev = NULL;
  new_state->val = NULL;
  new_state->accessors = 0;
  new_state->acc_list = 0;

  LOCK_VAR;

  /* Find the variable that's being written to. If variable doesn't
     exist, allocate it. */
  struct var2 **vpp = hashstate (name);
  struct var2 *vp = *findvar (vpp, name);
  if (!vp)
    {
      vp = (struct var2 *) malloc (sizeof (struct var2));
      vp->next = *vpp;
      vp->flags = 0;
      vp->name = name;
      vp->func = NULL;
      vp->head = NULL;
      vp->tail = NULL;
      *vpp = vp;
      TRACE(("Create new var %p.\n", vp));
    }
  else
    {
      vp = *vpp;
      free (name);
    }

  /* Append new state. */
  if (vp->head == NULL)
    {
      TRACE(("CREATE STATE: new head, tail\n"));
      vp->head = new_state;
    }
  else
    {
      /* If old state has no accessors, it is obsolete. Remove it.
         TODO: remove it. It's only safe to do this once the sequential
         assignment queue has been constructed. It's safe to clean up
         $y -> $y, but not $y -> $x -> $y, since the $x can refer to the
         older state of $y. */
      vp->tail->next = new_state;
      new_state->prev = vp->tail;
    }
  vp->tail = new_state;
  UNLOCK_VAR;
  TRACE(("CREATE STATE state: %p\n", new_state));
  return new_state;
}

/* Write variable state. */
void
write_state (struct var_state *state, char *val)
{
  LOCK_VAR;
  /* Write value to the state. */
  int valsize = strlen (val);
  state->val = malloc (valsize + 1);
  memcpy (state->val, val, valsize);
  state->val[valsize] = '\0'; 

  TRACE(("WRITE STATE new val %s, state %p\n", state->val, state));

  /* Unblock waiting accessors, then free the list. */
  struct var_acc *iter = state->acc_list;
  for (; iter; iter = iter->next)
    dg_node_dep_decr (iter->node);
  /* TODO: free the list. */
  UNLOCK_VAR;
}

/* Attach an accessor to a state. */
void
queue_state (struct dg_node *graph_node, struct var_state *state)
{
  LOCK_VAR;
  /* If VAL isn't set, queue GRAPH_NODE to be notified when
     VAL is set. */
  if (!state->val)
    {
      struct var_acc *acc = (struct var_acc *) malloc (sizeof *acc);
      acc->next = state->acc_list;
      acc->node = graph_node;
      state->acc_list = acc;
    }
  state->accessors++;
  UNLOCK_VAR;
}

/* Read variable state. Return NULL if variable does not exist. */
struct var_state *
read_state (char *name)
{
  LOCK_VAR;

  TRACE(("READ STATE: read %s\n", name));
  struct var2 *var = *findvar (hashstate (name), name);
  struct var_state *ret = var->tail;
  TRACE(("READ STATE: state %p\n", ret));
  if (ret)
    TRACE(("READ STATE: read %s\n", ret->val));

  UNLOCK_VAR;
  return ret;
}

/* Extract variable name from a string. */
static char *
copy_name (char *arg_str)
{
  TRACE(("COPY NAME\n"));
  int len = 1;
  char *iter = arg_str;
  while (isalnum (*iter) || *iter == '_')
    {
	TRACE(("ITER: %c\n", *iter));
      len++;
      iter++;
    }

  char *ret = malloc (len);
  strncpy (ret, arg_str, len - 1);
  ret[len - 1] = '\0';
  return ret;
}

/* Utility functions taken from dash's var.c. */
static struct var2 **
hashstate (const char *p)
{
  TRACE(("HASH STATE\n"));
  unsigned int hashval;

  hashval = ((unsigned char) *p) << 4;
  while (*p && *p != '=')
    hashval += (unsigned char) *p++;
  return &vartab[hashval % VTABSIZE];
}

static struct var2 **
findvar (struct var2 **vpp, const char *name)
{
  TRACE(("FIND VAR\n"));
  for (; *vpp; vpp = &(*vpp)->next)
    if (varcmp ((*vpp)->name, name) == 0)
      break;
  return vpp;
}
