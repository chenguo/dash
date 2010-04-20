#include <alloca.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"

#include "shell.h"
#include "memalloc.h"
#include "nodes.h"
#include "show.h"

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

static int dg_file_check (struct dg_node *, struct dg_node *);
static int dg_dep_add (struct dg_node *, struct dg_node *);
static struct dg_file * dg_node_files (union node *);
static struct dg_node * dg_node_create (union node *);
void dg_graph_add (union node *);
void dg_graph_remove (struct dg_node *);
static void dg_frontier_add (struct dg_node *);
void dg_frontier_remove (union node *);
union node * dg_frontier_run (void);


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

/* Initialize graph. */
void
dg_graph_init (void)
{
  frontier = malloc (sizeof *frontier);
  frontier->run_list = NULL;
  frontier->run_next = NULL;
  frontier->tail = NULL;
}


/* Cross check file lists for access conflicts. */
static int
dg_file_check (struct dg_node *node1, struct dg_node *node2)
{
  struct dg_file *files1 = node1->files;
  struct dg_file *files2 = node2->files;

  int mult_read = NO_CLASH;
  while (files1)
    {
      while (files2)
        {
          /* If same file is accessed. */
          if (strcmp (files1->file, files2->file) == 0)
            {
              if (files1->rw == WRITE_ACCESS || files2->rw == WRITE_ACCESS)
                return WRITE_COLLISION;
              else
                mult_read = CONCURRENT_READ;
            } 

          files2 = files2->next;
        }

      files1 = files1->next;
      files2 = node2->files;
    }

  /* Either no files in common or concurrent read. */
  return mult_read;
}

/* Check if NEW_NODE is a dependent of NODE. If so, recursive call
   on NODE's dependents, or add as dependent to NODE as necessary.
   Returns total number of dependencies originating from NODE. */
static int
dg_dep_add (struct dg_node *new_node, struct dg_node *node)
{
  /* Establish dependency. */
  int file_access = dg_file_check (new_node, node);
  if (file_access == NO_CLASH)
    return 0;

  int deps = 0;
  struct dg_list *iter = node->dependents;

  /* Check dependency on node's dependents. */
  if (iter == NULL)
    return 0;

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

  /* If no depedencies found, add NEW_NODE as one. */
  if (deps == 0 && file_access == WRITE_COLLISION)
    {
      iter->next = malloc (sizeof (struct dg_list));
      iter = iter->next;
      iter->node = new_node;
      iter->next = NULL;
      deps++;
    }

  return deps;
}

/* Construct file access list for a command. */
static struct dg_file *
dg_node_files (union node *redir)
{
  if (!redir)
    return NULL;

  struct dg_file *files = malloc (sizeof *files);
  struct dg_file *iter = files;
  while (1)
    {
      iter->file = redir->nfile.fname->narg.text;

      /* Only handle these for now. */
      if (redir->type == NFROM)
        iter->rw = CONCURRENT_READ;
      else if (redir->type == NTO || redir->type == NCLOBBER
               || redir->type == NAPPEND)
        iter->rw = WRITE_COLLISION;

      if (redir->nfile.next)
        {
          iter->next = malloc (sizeof *iter->next);
          iter = iter->next;
          redir = redir->nfile.next;
        }
      else
        break;
    }
  return files;
}

/* Create a node for NEW_CMD. */
static struct dg_node *
dg_node_create (union node *new_cmd)
{
  struct dg_node *new_node = malloc (sizeof *new_node);
  new_node->dependents = NULL;
  new_node->files = dg_node_files (new_cmd->nredir.redirect);
  new_node->dependencies = 0;
  new_node->command = (union node *) stalloc (sizeof (struct nredir));
  *new_node->command = *new_cmd;

  return new_node;
}

/* Add a new command to the directed graph. */
void
dg_graph_add (union node *new_cmd)
{
TRACE(("GRAPH ADD\n"));
  /* Create a node for this command. */
  struct dg_node *new_node = dg_node_create (new_cmd);

  /* Step through frontier nodes. */
  struct dg_list *iter = frontier->run_list;

  while (iter)
    {
      /* Follow frontier node and check for dependencies. */
      new_node->dependencies += dg_dep_add (new_node, iter->node);
TRACE(("Deps %u\n", new_node->dependencies));

      /* Increment to next frontier node. */
      iter = iter->next;
    }
TRACE(("GOT HERE\n"));

  /* If no file access dependencies, this is a frontier node. */
  if (new_node->dependencies == 0)
    dg_frontier_add (new_node);
}


/* Remove a node from directed graph. This removed node is a command
   that has finished executing, thus we can be sure this node has
   only dependents, no dependencies. */
void
dg_graph_remove (struct dg_node *graph_node)
{
  /* Step through dependents. */
  struct dg_list *iter = graph_node->dependents;
  while (iter)
    {
      /* Decrement dependency count. */
      iter->node->dependencies--;

      /* If no more dependencies, add to frontier. */
      if (!iter->node->dependencies)
        dg_frontier_add (iter->node);
    }

  free (graph_node->dependents);
  free (graph_node->files);
  free (graph_node);
}


/* Add a node to frontier. */
static void
dg_frontier_add (struct dg_node *graph_node)
{
TRACE(("FRONTIER ADD\n"));
  /* Allocate new runnables node. */
  struct dg_list *new_tail = malloc (sizeof *new_tail);

  if (frontier->tail)
    {
      /* Add new tail to LL. */
      frontier->tail->next = new_tail;

      /* Point to new tail. */
      frontier->tail = frontier->tail->next; 

      /* Set run next if not set. */
      if (!frontier->run_next)
        frontier->run_next = new_tail;
    }
  else
    {
      /* Frontier is currently empty. */
      frontier->run_list = new_tail;
      frontier->run_next = new_tail;
      frontier->tail = new_tail;
    }

  /* Fill out node. */
  frontier->tail->node = graph_node;
  frontier->tail->next = NULL;
}


/* Remove the runnables list node corresponding to a frontier
   node that has completed execution. */
void
dg_frontier_remove (union node *cmd)
{
  struct dg_list *iter = frontier->run_list;

  if (!iter)
    return;

  /* Special case, if cmd is first node. */
  if (iter->node->command == cmd)
    {
      frontier->run_list = iter->next;

      /* Free iter node... Also free struct node? */
      dg_graph_remove (iter->node);
      free (iter);
      return;
    }

  /* tmp_iter points at first LL node. */
  struct dg_list *tmp_iter = iter;
  /* iter points at second LL node. */
  iter = iter->next;

  /* Step through runnables in run list. */
  while (iter != frontier->run_next)
    {
      if (iter->node->command == cmd)
        {
          /* Found finished command. */
          tmp_iter->next = iter->next;
          dg_graph_remove (iter->node);
          free (iter);
          return; 
        }
      else
        {
          tmp_iter = iter;
          iter = iter->next;
        }
    }    
}

union node *
dg_frontier_run (void)
{
TRACE(("FRNOTIER RUN\n"));
  if (frontier->run_next)
    {
      union node *ret = frontier->run_next->node->command;
      frontier->run_next = frontier->run_next->next;
      return ret;
    }
  else
    return NULL;
}
