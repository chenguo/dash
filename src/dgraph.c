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


static void dg_graph_add_node (struct dg_node *, struct dg_fnode *);
static int dg_dep_check (struct dg_node *, struct dg_node *);
static int dg_dep_add (struct dg_node *, struct dg_node *);
static struct dg_file * dg_node_files (union node *);
static struct dg_node * dg_node_create (union node *, int flag,
                                        struct dg_fnode *);
static void dg_graph_remove (struct dg_node *);
static void dg_frontier_node_proc (struct dg_fnode *);
static void dg_frontier_set_eof (void);
static void dg_frontier_add (struct dg_node *);
static void free_command (union node *);
static void free_deps (struct dg_list *);
static void free_files (struct dg_file *);
static void free_nodelist (struct nodelist *);
static struct nodelist * node_list (union node *);

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

/* Free graph. */
void
dg_graph_free (void)
{
  /* TODO: FILL OUT. */
}

/* Return a process in the frontier. */
struct dg_fnode *
dg_graph_run (void)
{
  LOCK_GRAPH;
  /* Blocks until there's nodes in the graph. */
  while (frontier->run_next == NULL)
    {
      TRACE(("DG GRAPH RUN wait\n"));
      pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
    }
  struct dg_fnode *ret = frontier->run_next;
  TRACE(("DG GRAPH RUN ret type %d\n", ret->node->command->type));
  if (frontier->run_next)
    {
      frontier->run_next = frontier->run_next->next;
      TRACE(("DG GRAPH RUN incr run_next %p\n", frontier->run_next));
    }
  UNLOCK_GRAPH;
  return ret;
}

/* Wrap a command with a graph node and add to graph. */
static void
dg_graph_add (union node *new_cmd)
{
  TRACE(("DG GRAPH ADD type %d\n", new_cmd->type));
  LOCK_GRAPH;
  struct dg_node *new_node = dg_node_create (new_cmd, FREE_CMD, NULL);
  dg_graph_add_node (new_node, frontier->run_list);
  UNLOCK_GRAPH;
}

/* Add GRAPH_NODE to directed graph, beginning the check for dependencies with
   FRONTIER_NODE. */
static void
dg_graph_add_node (struct dg_node *new_node, struct dg_fnode *frontier_node)
{
  /* Step through frontier nodes and resolve dependencies. */
  struct dg_fnode *iter = frontier_node;
  int new_deps = 0;
  for (; iter; iter = iter->next)
    {
      new_deps = dg_dep_add (new_node, iter->node);
      new_node->dependencies += new_deps;
      /* If we hit a loop, and we depend on it, stop here. */
      if ((iter->type == DG_NWHILE || iter->type == DG_NUNTIL ||
           iter->type == DG_NFOR) && new_deps != 0)
        return;
    }
  TRACE(("DG GRAPH ADD NODE %p: deps %d\n", new_node, new_node->dependencies));
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
  for (; iter; iter = iter->next)
    {
      iter->node->dependencies--;
      if (iter->node->dependencies == 0)
        dg_frontier_add (iter->node);
    }
  if ((graph_node->flag & FREE_CMD) == FREE_CMD)
    free_command (graph_node->command);
  free_deps (graph_node->dependents);
  free_files (graph_node->files);
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
dg_node_create (union node *new_cmd, int flag, struct dg_fnode *parent)
{
  TRACE(("DG NODE CREATE type %d, parent %p\n", new_cmd->type, parent));
  struct dg_node *new_node = malloc (sizeof *new_node);
  new_node->dependents = NULL;
  new_node->dependencies = 0;
  new_node->command = new_cmd;
  new_node->files = dg_node_files (new_cmd);
  new_node->flag = flag;
  TRACE(("DG NODE CREATE flag %x\n", flag));
  new_node->parent = parent;

  TRACE(("DG NODE CREATE: files: "));
  struct dg_file *file = new_node->files;
  for (; file; file = file->next)
    TRACE(("%s ", file->name));
  TRACE(("\n"));

  return new_node;
}

/* Cross check file lists for access conflicts. */
static int
dg_dep_check (struct dg_node *node1, struct dg_node *node2)
{
  TRACE(("DG DEP CHECK, files1 %p files2 %p\n", node1->files, node2->files));
  struct dg_file *files1 = node1->files;
  struct dg_file *files2 = node2->files;
  int collision = NO_CLASH;
  for (; files1; files1 = files1->next, files2 = node2->files)
    for (; files2; files2 = files2->next)
     {
      TRACE(("DG DEP CHECK %s and %s\n", files1->name, files2->name));
      /* If same file is accessed. TODO: globbing abs path instead of file name. */
      if (strcmp (files1->name, files2->name) == 0)
        {
          TRACE(("DG DEP CHECK %s and %s match\n", files1->name, files2->name));
          if (files1->rw == WRITE_ACCESS || files2->rw == WRITE_ACCESS)
            return WRITE_COLLISION;
          else
            collision = CONCURRENT_READ;
        }
     }
  /* Either no files in common or concurrent read. */
  TRACE(("DG DEP CHECK ret %d\n", collision));
  return collision;
}

/* Create a dependent linked list node. */
static struct dg_list *
dg_dep_create (struct dg_node *dep)
{
  struct dg_list *ret = malloc (sizeof (ret));
  ret->node = dep;
  ret->next = NULL;
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
      do
        {
          /* Check if NEW_NODE is already a dependent of NODE. */
          if (new_node == iter->node)
            return 0;
          /* Recursive call on dependent. */
          deps += dg_dep_add (new_node, iter->node);
          /*if (iter->next)
            iter = iter->next;
          else
            break;*/
        }
      while (iter->next && (iter = iter->next));
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

/* Resolve file access for a variable.
   TODO: could this be cleaned up? */
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

/* Resolve file access for a regular file. 
   TODO: could this be cleaned up? */
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

/* Construct file access list for a graph node's command.
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
    return dg_file_var (n);
  case NPIPE:
    {
      /* TODO: resolve files. */
      struct dg_file *flist, *ret;
      struct nodelist *iter = n->npipe.cmdlist;
      for (; iter; iter = iter->next)
        {
          flist = dg_node_files (iter->n);
          ret = dg_file_append (flist, ret);
        }
      return ret;
    }
  case NBACKGND:
    return dg_node_files (n->nredir.n);
  case NAND:
  case NOR:
  case NWHILE:
  case NUNTIL:
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
  case NNOT:
    return dg_node_files (n->nnot.com);
  default:
    break;
  }
  return NULL;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *  Functions that manage the frontier.
 *  dg_frontier_add
 *  dg_frontier_node_proc
 *  dg_frontier_add_eof
 *  dg_frontier_set_eof
 *  dg_frontier_nonempty
 *  dg_frontier_done
 *  dg_frontier_remove
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Add a node to frontier. */
static void
dg_frontier_add (struct dg_node *graph_node)
{
  TRACE(("DG FRONTIER ADD %p\n", graph_node));
  struct dg_fnode *new_tail = malloc (sizeof *new_tail);
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
  new_tail->node = graph_node;
  new_tail->next = NULL;
  new_tail->active = 0;
  new_tail->status = 0;
  /* Special processing of new frontier node. */
  dg_frontier_node_proc (new_tail);

  /* Send a signal to wake up blocked dg_run threads. */
  pthread_cond_broadcast (&frontier->dg_cond);
}

/* Expand a portion of a frontier node. */
static void
dg_frontier_expand (struct dg_fnode *parent, union node *n,
                    struct dg_fnode *check_start, int last_flag)
{
  if (!n) return;
  /* Create a node list. For each node: create node, add to frontier.
     For last node, set as TEST_CMD. */
  struct nodelist *commands = node_list (n);
  struct dg_node *new_node;
  struct nodelist *iter= commands;
  for (; iter->next; iter = iter->next)
    {
      new_node = dg_node_create (iter->n, KEEP_CMD, parent);
      dg_graph_add_node (new_node, parent);//check_start);
      parent->active++;
    }
  new_node = dg_node_create (iter->n, KEEP_CMD | last_flag, parent);
  dg_graph_add_node (new_node, parent);//check_start);
  parent->active++;
  free_nodelist (commands);
}

/* Recheck a compound node's dependencies after a portion of it has been
   expanded. */
static void
dg_frontier_dep_recheck (struct dg_fnode *parent, struct dg_fnode *check_start)
{
  struct dg_list *deps = parent->node->dependents;
  struct dg_list *iter = deps;
  parent->node->dependents = NULL;
  for (; iter; iter = iter->next)
    {
      iter->node->dependencies--;
      dg_graph_add_node (iter->node, check_start);
    }
  free_deps (deps);
}

/* Process a GRAPH_NODE for addition into frontier. */
static void
dg_frontier_node_proc (struct dg_fnode *frontier_node)
{
  TRACE(("DG FRONTIER NODE PROC\n"));
  union node *command = frontier_node->node->command;

  switch (command->type) {
  case NAND:
  case NOR:
    if (command->type == NAND)
      frontier_node->type = DG_NAND;
    else
      frontier_node->type = DG_NOR;
    /* Free files. */
    free_files (frontier_node->node->files);
    frontier_node->node->files = NULL;
    /* Expand test command. */
    dg_frontier_expand (frontier_node, command->nbinary.ch1,
                        frontier_node, TEST_CMD);
    /* Build new file list and recheck dependents. */
    frontier_node->node->files = dg_node_files (command->nbinary.ch2);
    dg_frontier_dep_recheck (frontier_node, frontier_node);
    break;
  case NIF:
    frontier_node->type = DG_NIF;
    /* Free files. */
    free_files (frontier_node->node->files);
    frontier_node->node->files = NULL;
    /* Expand test command. */
    dg_frontier_expand (frontier_node, command->nif.test,
                        frontier_node, TEST_CMD);
    /* Build new file list and recheck dependents. */
    struct dg_file *list1 = dg_node_files (command->nif.ifpart);
    struct dg_file *list2 = dg_node_files (command->nif.elsepart);
    frontier_node->node->files = dg_file_append (list1, list2);
    dg_frontier_dep_recheck (frontier_node, frontier_node);
    break;
  case NWHILE:
  case NUNTIL:
    if (command->type == NWHILE)
      frontier_node->type = DG_NWHILE;
    else
      frontier_node->type = DG_NUNTIL;
    /* Save files and dependents. */
    struct dg_file *saved_files = frontier_node->node->files;
    struct dg_list *saved_deps = frontier_node->node->dependents;
    frontier_node->node->files = NULL;
    frontier_node->node->dependents = NULL;
    /* Expand test command. */
    dg_frontier_expand (frontier_node, command->nbinary.ch1,
                        frontier_node, TEST_CMD);
    /* Restore files and dependents. */
    frontier_node->node->files = saved_files;
    frontier_node->node->dependents = saved_deps;
    break;
  default:
    frontier_node->type = DG_NCMD;
    break;
  }
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

/* Process parent node based on return status. */
static void
dg_frontier_parent_proc (struct dg_fnode *rem)
{
  TRACE(("DG FRONTIER PARENT PROC %p, parent %p\n", rem->node, rem->node->parent));
  struct dg_fnode *check_start = NULL;
  union node *expand = NULL;

  struct dg_fnode *parent = rem->node->parent;
  if (parent) parent->active--;
  else return;
  if (rem->node->command->type == NNOT)
    rem->status = !rem->status;


  if ((rem->node->flag & STATUS_CMD) == STATUS_CMD)
    parent->status = rem->status;
  if ((rem->node->flag & TEST_CMD) != TEST_CMD)
    {
      if (parent->active == 0)
        dg_frontier_remove (parent);
      return;
    }

  switch (parent->type) {
  case DG_NAND:
  case DG_NOR:
    TRACE(("DG FRONTIER PARENT PROC: DG_NAND/DG_NOR\n"));
    check_start = frontier->tail;
    /* Free files. */
    free_files (parent->node->files);
    parent->node->files = NULL;
    /* Expand appropriate commands. */
    if ((rem->status == 0 && parent->type == DG_NAND) ||
        (rem->status != 0 && parent->type == DG_NOR))
      dg_frontier_expand (parent, parent->node->command->nbinary.ch2,
                          check_start, STATUS_CMD);
    /* Recheck dependents. */
    dg_frontier_dep_recheck (parent, parent);//check_start);
    /* Set status to DG_NCMD to label node removable. */
    parent->type = DG_NCMD;
    break;
  case DG_NIF:
    TRACE(("DG FRONTIER PARENT PROC: DG_NIF\n"));
    check_start = frontier->tail;
    /* Free files. */
    free_files (parent->node->files);
    parent->node->files = NULL;
    /* Expand appropriate commands. */
    expand = (rem->status == 0)? parent->node->command->nif.ifpart
                               : parent->node->command->nif.elsepart;
    dg_frontier_expand (parent, expand, check_start, STATUS_CMD);
    /* Recheck IF's dependents. */
    dg_frontier_dep_recheck (parent, parent);//check_start);
    /* Set status to DG_NCMD to label node removable. */
    parent->type = DG_NCMD;
    break;
  case DG_NWHILE:
  case DG_NUNTIL:
    TRACE(("DG FRONTIER PARENT PROC: DG_NWHILE/DG_NUNTIL\n"));
    check_start = frontier->tail;
    /* Save files and dependencies. */
    struct dg_file *saved_files = parent->node->files;
    struct dg_list *saved_deps = parent->node->dependents;
    parent->node->files = NULL;
    parent->node->dependents = NULL;
    /* Expand commands if appropriate. */
    if ((rem->status == 0 && parent->type == DG_NWHILE) ||
        (rem->status != 0 && parent->type == DG_NUNTIL))
      {
        dg_frontier_expand (parent, parent->node->command->nbinary.ch2,
                            check_start, STATUS_CMD);
        /* Recycle test condition. */
        dg_frontier_expand (parent, parent->node->command->nbinary.ch1,
                            check_start, TEST_CMD);
        /* Restore files and dependents. */
        parent->node->files = saved_files;
        parent->node->dependents = saved_deps;
      }
    else
      {
        /* Free saved files. Recheck dependents (this requires restoring
           them. */
        free_files (saved_files);
        parent->node->dependents = saved_deps;
        dg_frontier_dep_recheck (parent, parent);//check_start);
        /* Set status to DG_NCMD to label node removable. */
        parent->type = DG_NCMD;
      }
    break;
  default:
    parent->status = rem->status;
    if (parent->active == 0)
      dg_frontier_remove (parent);
    break;
  }
}

/* Remove the runnables list node corresponding to a frontier
   node that has completed execution. */
void
dg_frontier_remove (struct dg_fnode *rem)
{
  LOCK_GRAPH;
  TRACE (("DG FRONTIER REMOVE %p\n", rem->node));
  dg_frontier_parent_proc (rem);
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
  if (!node)
    return;

  switch (node->type) {
  case NCMD:
    TRACE(("FREE_COMMAND: NCMD\n"));
    free_command (node->ncmd.assign);
    free_command (node->ncmd.args);
    free_command (node->ncmd.redirect);
    break;
  case NVAR:
    TRACE(("FREE_COMMAND: NVAR\n"));
    free_command (node->nvar.com);
    break;
  case NPIPE:
    TRACE(("FREE_COMMAND: NPIPE\n"));
    /*TODO: free nodelist: node->npipe.cmdlist */
    break;
  case NREDIR:
  case NBACKGND:
  case NSUBSHELL:
    TRACE(("FREE_COMMAND: NREDIR\n"));
    free_command (node->nredir.n);
    free_command (node->nredir.redirect);
    break;
  case NAND:
  case NOR:
  case NSEMI:
  case NWHILE:
  case NUNTIL:
    TRACE(("FREE_COMMAND: NBINARY\n"));
    free_command (node->nbinary.ch1);
    free_command (node->nbinary.ch2);
    break;
  case NIF:
    TRACE(("FREE_COMMAND: NIF\n"));
    free_command (node->nif.test);
    free_command (node->nif.ifpart);
    free_command (node->nif.elsepart);
    break;
  case NFOR:
    TRACE(("FREE_COMMAND: NFOR\n"));
    free_command (node->nfor.args);
    free_command (node->nfor.body);
    free (node->nfor.var);
    break;
  case NCASE:
    TRACE(("FREE_COMMAND: NCASE\n"));
    free_command (node->ncase.expr);
    free_command (node->ncase.cases);
    break;
  case NCLIST:
    TRACE(("FREE_COMMAND: NCLIST\n"));
    free_command (node->nclist.next);
    free_command (node->nclist.pattern);
    free_command (node->nclist.body);
    break;
  case NDEFUN:
  case NARG:
    TRACE(("FREE_COMMAND: NARG\n"));
    free_command (node->narg.next);
    free (node->narg.text);
    /* TODO: free nodelist: node->narg.backquote */
    break;
  case NTO:
  case NCLOBBER:
  case NFROM:
  case NFROMTO:
  case NAPPEND:
    TRACE(("FREE_COMMAND: NFILE\n"));
    free_command (node->nfile.next);
    free_command (node->nfile.fname);
    break;
  case NTOFD:
  case NFROMFD:
    TRACE(("FREE_COMMAND: NDUP\n"));
    free_command (node->ndup.next);
    free_command (node->ndup.vname);
    break;
  case NHERE:
  case NXHERE:
    TRACE(("FREE_COMMAND: NHERE\n"));
    free_command (node->nhere.next);
    free_command (node->nhere.doc);
    break;
  case NNOT:
    TRACE(("FREE_COMMAND: NNOT\n"));
    free_command (node->nnot.com);
    break;
  default:
    break;
  }
  free (node);
}

/* Free dependents list. */
static void
free_deps (struct dg_list *deps)
{
  TRACE(("FREE DEPS\n"));
  struct dg_list *next;
  while (deps)
    {
      next = deps->next;
      free (deps);
      deps = next;
    }
}

/* Free file access list. */
static void
free_files (struct dg_file *files)
{
  TRACE(("FREE FILES\n"));
  struct dg_file *next;
  while (files)
    {
      next = files->next;
      free (files->name);
      free (files);
      files = next;
    }
}

/* Free a node list. */
static void
free_nodelist (struct nodelist *node_list)
{
  struct nodelist *next;
  while (node_list)
    {
      next = node_list->next;
      free (node_list);
      node_list = next;
    }
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

/* Create a nodelist. */
static struct nodelist *
node_list_create (union node *n)
{
  struct nodelist *ret = malloc (sizeof *ret);
  ret->n = n;
  ret->next = NULL;
  return ret;
}

/* Append two nodelists. */
static struct nodelist *
node_list_append (struct nodelist *list1, struct nodelist *list2)
{
  if (!list1)
    return list2;
  struct nodelist *iter = list1;
  while (iter->next)
    iter = iter->next;
  iter->next = list2;
  return list1;
}

static union node *
node_proc_ncmd (union node *n)
{
  TRACE(("NODE PROC NCMD\n"));
  if (n->ncmd.args && n->ncmd.args->narg.text)
    {
      TRACE(("NODE PROC: NCMD: ARGS %s\n", n->ncmd.args->narg.text));
      /* Check for commands we do NOT backgound on.
         This includes: cd and exit.
         TODO: handle more. */
      if (strcmp (n->ncmd.args->narg.text, "cd") != 0 &&
          strcmp (n->ncmd.args->narg.text, "exit") != 0)
        n = node_wrap_nbackgnd (n);
    }
  else if (n->ncmd.assign && n->ncmd.assign->narg.text)
    n = node_wrap_nvar (n);
  return n;
}

/* Create a list of nodes. */
static struct nodelist *
node_list (union node *n)
{
  if (n == NULL)
    return NULL;

  switch (n->type) {
  case NCMD:
    n = node_proc_ncmd (n);
    break;
  case NSEMI:
    {
      struct nodelist *list1 = node_list (n->nbinary.ch1);
      struct nodelist *list2 = node_list (n->nbinary.ch2);
      return node_list_append (list1, list2);
    }
  case NNOT:
    if (n->nnot.com->type == NCMD)
      n->nnot.com = node_proc_ncmd (n->nnot.com);
  default:
    break;
  }
  return node_list_create (n);
}

/* Process node tree returned by parsecmd. */
void
node_proc (union node *n)
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
    n = node_proc_ncmd (n);
    break;
  case NSEMI:
    TRACE(("NODE PROC: NSEMI\n"));
    node_proc (n->nbinary.ch1);
    node_proc (n->nbinary.ch2);
    return;
  case NNOT:
    /* TODO: handle NOT of non-regular commands. */
    if (n->nnot.com->type == NCMD)
      n->nnot.com = node_proc_ncmd (n->nnot.com);
    break;
  default:
    TRACE(("NODE PROC: default, type %d\n", n->type));
    /* Pass straight through to graph. */
    break;
  }
  dg_graph_add (n);
}
