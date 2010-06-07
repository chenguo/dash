#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "nodes.h"
#include "dgraph.h"
#include "shell.h"

static struct dg_file * dep_cont (union node *, int, struct dg_node *);
static struct dg_file * dep_break (union node *, int, struct dg_node *);

/* Parse file arguments for common UNIX commands. Also decipher
   BREAK and CONTINUE commands. */
struct dg_file *
arg_files (union node *n, int nest, struct dg_node *graph_node)
{
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
      ret->name = NULL;
      ret->flag = CONTINUE;
      ret->name_size = graph_node->nest - cont_arg + 1;
      if (ret->name_size < 1)
        ret->name_size = 1;
      ret->next = NULL;
      TRACE(("DEP CONT: FILE DEP RETURNED\n"));
      return ret;
    }
  else
    return NULL;
}

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
      ret->name = NULL;
      ret->flag = BREAK;
      ret->name_size = graph_node->nest - break_arg + 1;
      if (ret->name_size < 1)
        ret->name_size = 1;
      ret->next = NULL;
      TRACE(("DEP BREAK: FILE DEP RETUEND\n"));
      return ret;
    }
  else
    return NULL;
  return NULL;
}
