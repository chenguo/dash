#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "nodes.h"
#include "dgraph.h"
#include "shell.h"
#include "states.h"

static void expandarg2 (union node *);
static bool arg_var (union node *, struct dg_node *);
static struct dg_file * dep_cont (union node *, int, struct dg_node *);
static struct dg_file * dep_break (union node *, int, struct dg_node *);

/* Process arguments for commands. Resolve BREAK and CONTINE, as
   well as variables. */
struct dg_file *
arg_proc (union node *n, int nest, struct dg_node *graph_node)
{
  TRACE(("ARG PROC\n"));
  /* Step through arg list, return WILDCARD if there are
     unresolvable variables. */
  if (arg_var (n, graph_node))
    {
      struct dg_file *ret = malloc (sizeof *ret);
      ret->type = WILDCARD;
      ret->next = NULL;
      ret->file = NULL;
      return ret;
    }
  expandarg2 (n);

  union node *narg = n->ncmd.args;
  //TRACE(("ARG FILES: command %s\n", narg->narg.text));
  if (strcmp (narg->narg.text, "continue") == 0)
    {
      n->type = NCONT;
      return dep_cont (narg->narg.next, nest, graph_node);
    }
  else if (strcmp (narg->narg.text, "break") == 0)
    {
      n->type = NBREAK;
      return dep_break (narg->narg.next, nest, graph_node);
    }
  else
    ;
  return NULL;
}

/* Scan for variables and try to resolve them. If var's value is not
   available, enqueue graph_node (if not already enqueued) and return
   true. */
/* TODO: finish this half written function. */
static bool
arg_var (union node *n, struct dg_node *graph_node)
{
  TRACE(("ARG VAR\n"));
  union node *iter = n->ncmd.args;
  struct var_state *state;
  for (; iter; iter = iter->narg.next)
    {
      char *scan = iter->narg.text;
      if (*scan == -126 && *(scan + 1) == 1)
        {
          state = read_state (scan+2);
          int argsize = strlen (state->val);
          char *new_val = malloc (argsize+1);
          strncpy (new_val, state->val, argsize);
          new_val[argsize] = '\0';
          iter->narg.text = new_val;
          TRACE(("NEW VAR: %s\n", iter->narg.text));
        }

    }

  

  return false;
}

/* Expand arguments.
   For now, mainly in the interest of getting a functional for, this function
   is grossly simplified to assume only the $x=value case. So fancy stuff like
   $x as the output of some command, or $x as $y, etc, are not supported. Even
   something like foo$x won't be expanded. Each NARG node, if it contains a
   variable, must contain NOTHING but that variable.

   Further, this function also assumes all variables requested have a value,
   which in reality once backquote commands are taken into account, wont be
   true. */
static void
expandarg2 (union node *n)
{
  union node *iter = n->ncmd.args;
  for (; iter; iter = iter->narg.next)
    {}
  

}

/* Handle CONTINUE. */
static struct dg_file *
dep_cont (union node *narg, int nest, struct dg_node *graph_node)
{
  int cont_arg;
  if (!narg)
    cont_arg = 1;
  else
    cont_arg = atoi (narg->narg.text);
  TRACE(("DEP CONT: continue %d, nested %d\n", cont_arg, nest));

  /* Check if the CONTINUE is buried. */
  if (cont_arg - nest > 0)
    {
      struct dg_file *ret = malloc (sizeof *ret);
      ret->type = CONTINUE;
      ret->next = NULL;
      ret->nest = graph_node->nest - cont_arg + 1;
      if (ret->nest < 1)
        ret->nest = 1;
      TRACE(("DEP CONT: FILE DEP RETURNED\n"));
      return ret;
    }
  else
    return NULL;
}

/* Handle BREAK. */
static struct dg_file *
dep_break (union node *narg, int nest, struct dg_node *graph_node)
{
  int break_arg;
  if (!narg)
    break_arg = 1;
  else
    break_arg = atoi (narg->narg.text);
  TRACE(("DEP BREAK: break %d, nested %d\n", break_arg, nest));

  /* Check if the BREAK is buried. */
  if (break_arg - nest > 0)
    {
      struct dg_file *ret = malloc (sizeof *ret);
      ret->type = BREAK;
      ret->next = NULL;
      ret->nest = graph_node->nest - break_arg + 1;
      if (ret->nest < 1)
        ret->nest = 1;
      TRACE(("DEP BREAK: FILE DEP RETUEND\n"));
      return ret;
    }
  else
    return NULL;
}
