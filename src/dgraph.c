/* By Chen Guo. */

#include <alloca.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"

#include "eval.h"
#include "shell.h"
#include "memalloc.h"
#include "nodes.h"
#include "show.h"
#include "parser.h"

/* Implementation is a directed graph, nodes representing a
   running command points to commands that much wait for the
   running command to finish.

   frontier node: nodes representing runnable commands, either
	 currently running or not.
   frontier: structure holding runnables list and run next.
   runnables list: LL of frontier nodes.
   run next: pointer to next non-running frontier node.

   dependents: a node's dependents is a list of nodes representing
	 commands that must wait for the node to finish before running.
 */

static void dg_graph_add_node (struct dg_node *);
static int dg_dep_check (struct dg_node *, struct dg_node *);
static int dg_dep_add (struct dg_node *, struct dg_node *);
static struct dg_file * dg_node_files (union node *);
static struct dg_node * dg_node_create (union node *);
static void dg_graph_remove (struct dg_node *);
static void dg_frontier_set_eof (void);
static void dg_frontier_add (struct dg_node *);
static void free_command (union node *);

static struct dg_frontier *frontier;

enum
{
  READ_ACCESS,
  WRITE_ACCESS
};

enum
{
  NO_CLASH,
  CONCURRENT_READ,
  WRITE_COLLISION
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 
 *  General graph operations:
 *  dg_graph_init
 *  dg_graph_destroy
 *  LOCK_GRAPH, UNLOCK_GRAPH
 *  dg_graph_run
 *  dg_graph_add
 *  dg_graph_insert
 *  dg_graph_remove
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Initialize graph. */
void
dg_graph_init (void)
{
  TRACE(("DG GRAPH INIT\n"));
  frontier = malloc (sizeof *frontier);
  frontier->run_list = NULL;
  frontier->run_next = NULL;
  frontier->tail = NULL;
  frontier->eof = 0;
  /* Initialize mutex to be recursive. */
  pthread_mutexattr_t *attr = malloc (sizeof (*attr));
  pthread_mutexattr_init (attr);
  pthread_mutexattr_settype (attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init (&frontier->dg_lock, attr);
  pthread_cond_init (&frontier->dg_cond, NULL);
}

#define LOCK_GRAPH {pthread_mutex_lock(&frontier->dg_lock);}
#define UNLOCK_GRAPH {pthread_mutex_unlock(&frontier->dg_lock);}

/* TODO: Destroy graph. */
void
dg_graph_free (void)
{}

/* Return a process in the frontier. */
struct dg_list *
dg_graph_run (void)
{
  TRACE(("DG GRAPH RUN\n"));
  LOCK_GRAPH;
  /* Blocks until there's nodes in the graph. */
  while (frontier->run_next == NULL)
    pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
  struct dg_list *ret = frontier->run_next;
  if (frontier->run_next)
    frontier->run_next = frontier->run_next->next;
  UNLOCK_GRAPH;
  return ret;
}

/* Wrap a command with a graph node and add to graph. */
void
dg_graph_add (union node *new_cmd)
{
  TRACE(("DG GRAPH ADD type %d\n", new_cmd->type));
  LOCK_GRAPH;
  if (new_cmd == NEOF)
    {
      dg_frontier_set_eof ();
      return;
    }
  /* Create a node for this command. */
  struct dg_node *new_node = dg_node_create (new_cmd);
  /* Add node to graph. */
  dg_graph_add_node (new_node);
  UNLOCK_GRAPH;
}

/* Add GRAPH_NODE to directed graph. */
static void
dg_graph_add_node (struct dg_node *new_node)
{
  /* Step through frontier nodes. */
  struct dg_list *iter = frontier->run_list;
  while (iter)
    {
      /* Follow frontier node and check for dependencies. */
      new_node->dependencies += dg_dep_add (new_node, iter->node);
      iter = iter->next;
    }
  if (new_node->dependencies == 0)
    dg_frontier_add (new_node);
}

/* Expand GRAPH_NODE's complex command into simple commands, while
   maintaining execution order, such that the expanded commands run before
   GRAPH_NODE's dependents. */
static void
dg_graph_expand ()
{
}

/* Insert a node as the first dependent of a particular graph node, ahead
   of other already existing dependents. */
static void
dg_graph_insert (union node *cmd, struct dg_node *graph_node)
{
  TRACE(("DG GRAPH INSERT\n"));
  /* Create new node for this command. */
  struct dg_node *new_node = dg_node_create (cmd);
  new_node->dependencies += dg_dep_add (new_node, graph_node);
  if (new_node->dependencies == 0)
    dg_frontier_add (new_node);
}

/* Remove a node from directed graph. This removed node is a command
   that has finished executing, thus we can be sure this node has
   only dependents, no dependencies. */
static void
dg_graph_remove (struct dg_node *graph_node)
{
  TRACE(("DG GRAPH REMOVE %p\n", graph_node));
  /* Step through dependents. */
  struct dg_list *iter = graph_node->dependents;
  while (iter)
    {
      iter->node->dependencies--;
      if (iter->node->dependencies == 0)
        dg_frontier_add (iter->node);
      iter = iter->next;
    }
  free_command (graph_node->command);
  /* OOPS only freeing first node of these lists.
     TODO: properly free linked lists. */
  if (graph_node->dependents)
    free (graph_node->dependents);
  if (graph_node->files)
    free (graph_node->files);
  free (graph_node);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *  Functions that deal with graph node creation and dependency checking.
 *  dg_node_create
 *  dg_dep_check
 *  dg_dep_create
 *  dg_dep_add
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Create a node for NEW_CMD. */
static struct dg_node *
dg_node_create (union node *new_cmd)
{
  TRACE(("DG NODE CREATE type %d\n", new_cmd->type));
  struct dg_node *new_node = malloc (sizeof *new_node);
  new_node->dependents = NULL;
  new_node->dependencies = 0;
  new_node->command = new_cmd;
  new_node->files = dg_node_files (new_cmd);
  return new_node;
}

/* Cross check file lists for access conflicts. */
static int
dg_dep_check (struct dg_node *node1, struct dg_node *node2)
{
  TRACE(("DG FILE CHECK\n"));
  struct dg_file *files1 = node1->files;
  struct dg_file *files2 = node2->files;
  int collision = NO_CLASH;
  while (files1)
    {
      while (files2)
        {
          TRACE(("CHECKING %s and %s\n", files1->name, files2->name));
          /* If same file is accessed. */
          if (strcmp (files1->name, files2->name) == 0)
            {
              if (files1->rw == WRITE_ACCESS || files2->rw == WRITE_ACCESS)
                return WRITE_COLLISION;
             else
                collision = CONCURRENT_READ;
            }
          files2 = files2->next;
        }
      files1 = files1->next;
      files2 = node2->files;
    }
  /* Either no files in common or concurrent read. */
  TRACE(("DG FILE CHECK ret %d\n", collision));
  return collision;
}

/* Create a dependent linked list node. */
static struct dg_list *
dg_dep_create (struct dg_node *dep)
{
  struct dg_list *ret = malloc (sizeof (ret));
  ret->node = dep;
  ret->next = NULL;
  ret->prev = NULL;
  return ret;
}

/* Check if NEW_NODE is a dependent of NODE. If so, recursive call
   on NODE's dependents, or add as dependent to NODE as necessary.
   Returns total number of dependencies originating from NODE. */
static int
dg_dep_add (struct dg_node *new_node, struct dg_node *node)
{
  TRACE(("DG DEP ADD  %p:%d %p:%d\n", new_node, new_node->dependencies, node, node->dependencies));
  /* Establish dependency. */
  int file_access = dg_dep_check (new_node, node);
  if (file_access == NO_CLASH)
    return 0;
  /* Check dependency on node's dependents. */
  int deps = 0;
  struct dg_list *iter = node->dependents;
  if (iter)
    {
      while (1)
        {
          /* Check if NEW_NODE is already a dependent of NODE. */
          if (new_node == iter->node)
            return 0;
          /* Recursive call on dependent. */
          deps += dg_dep_add (new_node, iter->node);
          if (iter->next)
            iter = iter->next;
          else
            break;
        }
      /* If no depedencies found, or NEW_NODE holdes NEOF, add NEW_NODE. */
      if (deps == 0 && file_access == WRITE_COLLISION)
        {
          iter->next = dg_dep_create (new_node);
          deps++;
        }
    }
  else if (file_access == WRITE_COLLISION)
    {
      node->dependents = dg_dep_create (new_node);
      deps++;
    }
  return deps;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *  Functions that create a graph node's file list.
 *  dg_file_append
 *  dg_file_var
 *  dg_file_reg
 *  dg_node_files
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Append LIST1 to the end of LIST2. If LIST1 is NULL, LIST2 is returned. */
static struct dg_file *
dg_file_append (struct dg_file *list1, struct dg_file *list2)
{
  if (!list1)
    return list2;
  struct dg_file *iter = list1;
  while (iter->next)
    iter = iter->next;
  iter->next = list2;
  return list1;
}

/* Resolve file access for a variable. */
static struct dg_file *
dg_file_var (union node *n)
{
  struct dg_file *file = malloc (sizeof *file);
  char *cmdstr = n->nvar.com->ncmd.assign->narg.text;
  file->name_size = strchr (cmdstr, '=') - cmdstr + 1;
  file->name = malloc (file->name_size);
  file->name[0] = '$';
  strncpy (file->name + 1, cmdstr, file->name_size);
  file->name[file->name_size - 1] = '\0';
  file->rw = WRITE_ACCESS;
  TRACE(("DG FILE VAR: %s\n", file->name));

  /* TODO: handle files access to command that writes shellvar. */
  //struct nodelist *nodes= n->nvar.com->ncmd.assign->narg.backquote;
  return file;
}

/* Resolve file access for a regular file. */
static struct dg_file *
dg_file_reg (union node *n)
{
  struct dg_file *file = malloc (sizeof *file);
  char *fname = n->nfile.fname->narg.text;
  file->name_size = strlen (fname) + 1;
  file->name = malloc (file->name_size);
  strncpy (file->name, fname, strlen (fname));
  file->name[file->name_size - 1] = '\0';
  TRACE(("DG FILE REG: %s\n", file->name));

  if (n->type == NFROM)
    file->rw = READ_ACCESS;
  else 
    file->rw = WRITE_ACCESS;
  file->next = dg_node_files (n->nfile.next);
  return file;
}

/* Construct file access list for a command.
   TODO: Only add a file to the list ONCE. */
static struct dg_file *
dg_node_files (union node *n)
{
  if (!n)
    return NULL;
  TRACE(("DG NODE FILES node type %d\n", n->type));

  switch (n->type) {
  case NCMD:
    /* ncmd.assign should be NULL, vars handled with NVAR. */
    /* TODO: process ncmd.args for common commands like echo. */
    if (n->ncmd.redirect)
      return dg_node_files (n->ncmd.redirect);
    break;
  case NVAR:
    /* Variable assignment. */
    return dg_file_var (n);
  case NBACKGND:
    return dg_node_files (n->nredir.n);
  case NSEMI:
    {
      struct dg_file *file1 = dg_node_files (n->nbinary.ch1);
      struct dg_file *file2 = dg_node_files (n->nbinary.ch2);
      return dg_file_append (file1, file2);
    }
  case NIF:
    {
      struct dg_file *test = dg_node_files (n->nif.test);
      struct dg_file *ifpart = dg_node_files (n->nif.ifpart);
      struct dg_file *elsepart = dg_node_files (n->nif.elsepart);
      ifpart = dg_file_append (ifpart, elsepart);
      return dg_file_append (test, ifpart);
    }
  case NTO:
  case NCLOBBER:
  case NFROM:
  case NFROMTO:
  case NAPPEND:
    return dg_file_reg (n);
  default:
    break;
  }
  return NULL;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *  Functions that manage the frontier.
 *  dg_frontier_add
 *  dg_frontier_add_eof
 *  dg_frontier_set_eof
 *  dg_frontier_nonempty
 *  dg_frontier_remove_nandnor
 *  dg_frontier_remove_nif
 *  dg_frontier_remove
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Add a node to frontier. */
static void
dg_frontier_add (struct dg_node *graph_node)
{
  TRACE(("DG FRONTIER ADD %p\n", graph_node));
  /* Allocate new runnables node. */
  struct dg_list *new_tail = malloc (sizeof *new_tail);
  if (frontier->tail)
    {
      TRACE(("DG FRONTIER ADD non-empty\n"));
      frontier->tail->next = new_tail;
      new_tail->prev = frontier->tail;
      frontier->tail = frontier->tail->next;
      if (!frontier->run_next)
        frontier->run_next = new_tail;
    }
  else
    {
      TRACE(("DG FRONTIER ADD empty\n"));
      frontier->run_list = new_tail;
      frontier->run_next = new_tail;
      frontier->tail = new_tail;
      new_tail->prev = NULL;
    }
  /* Fill out node. */
  frontier->tail->node = graph_node;
  frontier->tail->next = NULL;
  /* Send a signal to wake up blocked dg_run threads. */
  pthread_cond_broadcast (&frontier->dg_cond);
}

/* Add NEOF node to frontier. THIS SHOULD ONLY BE CALLED WHEN THE FRONTIER IS
   EMPTY. */
static void
dg_frontier_add_eof (void)
{
  TRACE(("DG FRONTIER ADD EOF\n"));
  frontier->run_next = malloc (sizeof *frontier->run_next);
  frontier->run_next->node = malloc (sizeof *frontier->run_next->node);
  frontier->run_next->node->command = NEOF;
  pthread_cond_broadcast (&frontier->dg_cond);
}

/* Set eof flag in frontier. */
static void
dg_frontier_set_eof (void)
{
  TRACE(("DG FRONTIER SET EOF\n"));
  frontier->eof = 1;
  if (frontier->run_list == NULL)
    dg_frontier_add_eof ();
}

/* Wait until frontier has commands. */
void
dg_frontier_nonempty (void)
{
  LOCK_GRAPH;
  if (!frontier->run_list)
    pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
  UNLOCK_GRAPH;
  return;
}

/* Detach dependents, preserving them for later use. */
static struct dg_list *
dg_frontier_dep_detach (struct dg_node *node)
{
  TRACE (("DG FRONTIER DEP DETACH.\n"));
  struct dg_list *deps = node->dependents;
  while (deps)
    {
      deps->node->dependencies--;
      deps = deps->next;
    }
  deps = node->dependents;
  node->dependents = NULL;
  return deps;
}

/* Attach DEPS as dependents to NODE. */
static void
dg_frontier_dep_attach (struct dg_list *deps, struct dg_node *node)
{
  TRACE(("DG FRONTIER DEP ATTACH.\n"));
  while (deps)
    {
      deps->node->dependencies = dg_dep_add (deps->node, node);
      if (deps->node->dependencies == 0)
        dg_frontier_add (deps->node);
      deps = deps->next;
    }
}

/* Extra processing for removing NIF from frontier. */
static void
dg_frontier_remove_nandnor (struct dg_node *node, int status)
{
  TRACE(("DG FRONTIER REMOVE NANDNOR.\n"));
  struct dg_list *deps = dg_frontier_dep_detach (node);
  if (status == 0 && node->command->type == NAND)
    node_proc (node->command->nbinary.ch2, node);
  else if (status != 0 && node->command->type == NOR)
    node_proc (node->command->nbinary.ch2, node);
  dg_frontier_dep_attach (deps, node);
}


/* Extra processing for removing NIF from frontier. */
static void
dg_frontier_remove_nif (struct dg_node *node, int status)
{
  TRACE(("DG FRONTIER REMOVE NIF.\n"));
  struct dg_list *deps = dg_frontier_dep_detach (node);
  if (status == 0)
    node_proc (node->command->nif.ifpart, node);
  else
    node_proc (node->command->nif.elsepart, node);
  dg_frontier_dep_attach (deps, node);
}

/* Remove the runnables list node corresponding to a frontier
   node that has completed execution. */
void
dg_frontier_remove (struct dg_list *rem, int status)
{
  LOCK_GRAPH;
  TRACE (("DG FRONTIER REMOVE %p, status 0x%x\n", rem->node, status));

  if (rem->node->command->type == NIF)
    dg_frontier_remove_nif (rem->node, status);
  else if (rem->node->command->type == NAND || rem->node->command->type == NOR)
    dg_frontier_remove_nandnor (rem->node, status);

  if (rem->prev)
    {
      /* Node is not first in LL. */
      rem->prev->next = rem->next;
      if (rem->next)
        rem->next->prev = rem->prev;
    }
  else
    {
      TRACE(("DG FRONTIER REMOVE: %p: new runlist %p\n", rem->node, rem->next));
      /* Node is first in LL. */
      frontier->run_list = rem->next;
      if (rem->next)
        rem->next->prev = NULL;
    }
  if (frontier->tail == rem)
    {
      TRACE(("DG FRONTIER REMOVE: %p: new tail %p\n", rem->node, rem->prev));
      /* Node is last in LL. */
      frontier->tail = rem->prev;
    }

  dg_graph_remove (rem->node);
  free (rem);
  if (!frontier->run_list && frontier->eof)
    dg_frontier_add_eof ();
  UNLOCK_GRAPH;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *  Misc functions that support the graph.
 *  free_command
 *  node_wrap_nbackgnd
 *  node_wrap_nvar
 *  node_proc
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Free node tree returned by parsecmd. */
static void
free_command (union node *node)
{
  switch (node->type) {
  case NCMD:
    TRACE(("FREE_COMMAND: NCMD\n"));
    if (node->ncmd.assign)
      free_command (node->ncmd.assign);
    if (node->ncmd.args)
      free_command (node->ncmd.args);
    if (node->ncmd.redirect)
      free_command (node->ncmd.redirect);
    break;
  case NVAR:
    TRACE(("FREE_COMMAND: NVAR\n"));
    if (node->nvar.com)
      free_command (node->nvar.com);
    break;
  case NPIPE:
    TRACE(("FREE_COMMAND: NPIPE\n"));
    if (node->npipe.cmdlist)
      ;/*TODO: free nodelist. */
    break;
  case NREDIR:
  case NBACKGND:
  case NSUBSHELL:
    TRACE(("FREE_COMMAND: NREDIR\n"));
    if (node->nredir.n)
      free_command (node->nredir.n);
    if (node->nredir.redirect)
      free_command (node->nredir.redirect);
    break;
  case NAND:
  case NOR:
    TRACE(("FREE_COMMAND: NAND/NOR\n"));
    if (node->nbinary.ch1)
      free_command (node->nbinary.ch1);
    break;
  case NSEMI:
  case NWHILE:
  case NUNTIL:
    TRACE(("FREE_COMMAND: NBINARY\n"));
    if (node->nbinary.ch1)
      free_command (node->nbinary.ch1);
    if (node->nbinary.ch2)
      free_command (node->nbinary.ch2);
    break;
  case NIF:
    TRACE(("FREE_COMMAND: NIF\n"));
    if (node->nif.test)
      free_command (node->nif.test);
    /*if (node->nif.ifpart)
      free_command (node->nif.ifpart);
    if (node->nif.elsepart)
      free_command (node->nif.elsepart);*/
    break;
  case NFOR:
    TRACE(("FREE_COMMAND: NFOR\n"));
    if (node->nfor.args)
      free_command (node->nfor.args);
    if (node->nfor.body)
      free_command (node->nfor.body);
    if (node->nfor.var)
      free (node->nfor.var);
    break;
  case NCASE:
    TRACE(("FREE_COMMAND: NCASE\n"));
    if (node->ncase.expr)
      free_command (node->ncase.expr);
    if (node->ncase.cases)
      free_command (node->ncase.cases);
    break;
  case NCLIST:
    TRACE(("FREE_COMMAND: NCLIST\n"));
    if (node->nclist.next)
      free_command (node->nclist.next);
    if (node->nclist.pattern)
      free_command (node->nclist.pattern);
    if (node->nclist.body)
      free_command (node->nclist.body);
    break;
  case NDEFUN:
  case NARG:
    TRACE(("FREE_COMMAND: NARG\n"));
    if (node->narg.next)
      free_command (node->narg.next);
    if (node->narg.text)
      free (node->narg.text);
    if (node->narg.backquote)
      ;/* TODO: free nodelist. */
    break;
  case NTO:
  case NCLOBBER:
  case NFROM:
  case NFROMTO:
  case NAPPEND:
    TRACE(("FREE_COMMAND: NFILE\n"));
    if (node->nfile.next)
      free_command (node->nfile.next);
    if (node->nfile.fname)
      free_command (node->nfile.fname);
    if (node->nfile.expfname)
      ;/* Taken care of by eval. */
   break;
  case NTOFD:
  case NFROMFD:
    TRACE(("FREE_COMMAND: NDUP\n"));
    if (node->ndup.next)
      free_command (node->ndup.next);
    if (node->ndup.vname)
      free_command (node->ndup.vname);
    break;
  case NHERE:
  case NXHERE:
    TRACE(("FREE_COMMAND: NHERE\n"));
    if (node->nhere.next);
      free_command (node->nhere.next);
    if (node->nhere.doc)
      free_command (node->nhere.doc);
    break;
  case NNOT:
    TRACE(("FREE_COMMAND: NNOT\n"));
    if (node->nnot.com)
      free_command (node->nnot.com);
    break;
  default:
    break;
  }
  free (node);
}

/* Wrap N with NBACKGND node. */
static union node *
node_wrap_nbackgnd (union node *n)
{
  union node *nwrap = (union node *) malloc (sizeof (struct nredir));
  nwrap->type = NBACKGND;
  nwrap->nredir.n = n;
  nwrap->nredir.redirect = NULL;
  return nwrap;
}

/* Wrap N with NVAR node. */
static union node *
node_wrap_nvar (union node *n)
{
  union node *nwrap = (union node *) malloc (sizeof (struct nvar));
  nwrap->type = NVAR;	
  nwrap->nvar.com = n;
  return nwrap;
}

/* Add a command into the graph. */
static void
node_proc_add (union node *n, struct dg_node *graph_node)
{
  /* Straight add. */
  if (!graph_node)
    dg_graph_add (n);
  /* Insert in place of GRAPH_NODE. */
  else
    dg_graph_insert (n, graph_node);
}

/* Process a command node for adding into the graph. */
static void
node_proc_ncmd (union node *n, struct dg_node *graph_node)
{
  TRACE(("NODE PROC NCMD\n"));
  if (n->ncmd.args && n->ncmd.args->narg.text)
    {
      TRACE(("NODE PROC: NCMD: ARGS %s\n", n->ncmd.args->narg.text));
      /* Check for commands we do NOT backgound on.
         This includes: cd and exit.
         TODO: think of more. */
      if (strcmp (n->ncmd.args->narg.text, "cd") != 0 &&
          strcmp (n->ncmd.args->narg.text, "exit") != 0)
        n = node_wrap_nbackgnd (n);
    }
  else if (n->ncmd.assign && n->ncmd.assign->narg.text)
    n = node_wrap_nvar (n);
  /* Evaluate cd and exit, otherwise send to graph. */
  if (n->type == NCMD)
    evaltree (n, 0, NULL);
  else
    node_proc_add (n, graph_node);
}

/* Process an AND or OR node for adding into the graph. */
static void
node_proc_nandnor (union node *n, struct dg_node *graph_node)
{

}

/* Process an IF node for adding into the graph. */
static void
node_proc_nif (union node *n, struct dg_node *graph_node)
{
  TRACE(("NODE PROC NIF\n"));
  /* For now, the slow (but safe) way. TODO: do this better. */
  if (n->nif.test && n->nif.test->type != NBACKGND)
    {
      union node *nwrap = (union node *) malloc (sizeof (struct nredir));
      nwrap->type = NBACKGND;
      nwrap->nredir.n = n->nif.test;
      nwrap->nredir.redirect = NULL;
      n->nif.test = nwrap;
    }
  node_proc_add (n, graph_node);
}

/* Process a WHILE or UNTIL node for adding into the graph. */
static void
node_proc_nwhilenuntil (union node *n, struct dg_node *graph_node)
{

}

/* Process node tree returned by parsecmd. */
void
node_proc (union node *n, struct dg_node *graph_node)
{
  /* Special case: EOF. */
  if (n == NEOF)
    {
      TRACE(("NODE PROC: NEOF\n"));
      dg_frontier_set_eof ();
      pthread_exit (NULL);
    }    
  else if (!n)
    return;

  switch (n->type) {
  case NCMD:
    node_proc_ncmd (n, graph_node);
    break;
  case NAND:
  case NOR:
    node_proc_nandnor (n, graph_node);
    break;
  case NSEMI:
    TRACE(("NODE PROC: NSEMI\n"));
    if (n->nbinary.ch1)  
      node_proc (n->nbinary.ch1, graph_node);
    if (n->nbinary.ch2)
      node_proc (n->nbinary.ch2, graph_node);
    break;
  case NWHILE:
  case NUNTIL:
    node_proc_nwhilenuntil (n, graph_node);
    break;
  case NIF:
    node_proc_nif (n, graph_node);
    break;
  default:
    /* Pass straight through to graph. */
    node_proc_add (n, graph_node);
    break;
  }
}
