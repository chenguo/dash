#include <stdlib.h>

#include "dgraph.h"


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
  if (file_access = NO_CLASH)
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

/* Add a node to the graph. */
void
dg_graph_add (struct dg_node *new_node)
{
  /* Step through frontier nodes. */
  struct dg_list *iter = frontier->run_list;

  new_node->dependencies = 0;
  while (iter)
    {
      /* Follow frontier node and check for dependencies. */
      new_node->dependencies += dg_dep_add (new_node, iter->node);

      /* Increment to next frontier node. */
      iter = iter->next;
    }

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
void
dg_frontier_add (struct dg_node *graph_node)
{
  /* Allocate new runnables node. */
  frontier->tail->next = malloc (sizeof (struct dg_list));

  /* Point to new tail. */
  frontier->tail = frontier->tail->next; 

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


