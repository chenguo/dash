#include <alloca.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"

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

static int dg_file_check (struct dg_node *, struct dg_node *);
static int dg_dep_add (struct dg_node *, struct dg_node *);
static struct dg_file * dg_node_files (union node *);
static struct dg_node * dg_node_create (union node *);
void dg_graph_add (union node *);
static void dg_graph_remove (struct dg_node *);
static void dg_frontier_add (struct dg_node *);
void dg_frontier_remove (struct dg_list *);
void dg_frontier_run ();
static void free_command (union node *);


static struct dg_frontier *frontier;

enum {
	READ_ACCESS,
	WRITE_ACCESS
};

enum {
	NO_CLASH,
	CONCURRENT_READ,
	WRITE_COLLISION
};

/* Initialize graph. */
void
dg_graph_init (void)
{
  TRACE(("DG GRAPH INIT\n"));
  frontier = malloc (sizeof *frontier);
  frontier->run_list = NULL;
  frontier->run_next = NULL;
  frontier->tail = NULL;
  pthread_mutex_init (&frontier->dg_lock, NULL);
  pthread_cond_init (&frontier->dg_cond, NULL);
}


/* Lock down graph. */
void
dg_graph_lock(void) {
	pthread_mutex_lock(&frontier->dg_lock);
}


/* Unlock graph. */
void
dg_graph_unlock(void) {
	pthread_mutex_unlock(&frontier->dg_lock);
}


/* Return a process in the frontier. */
struct dg_list *
dg_graph_run (void)
{
  dg_graph_lock ();
  TRACE(("DG GRAPH RUN\n"));

  /* Blocks until there's nodes in the graph. */
  while (frontier->run_next == NULL)
  {
    TRACE(("DG_GRAPH_RUN: cond wait\n"));
    pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
    TRACE(("DG_GRAPH_RUN: cond wait complete\n"));
  }

  struct dg_list *ret = frontier->run_next;
  if (frontier->run_next)
    frontier->run_next = frontier->run_next->next;
  dg_graph_unlock ();
  return ret;
}


/* Add a new command to the directed graph. */
void
dg_graph_add (union node *new_cmd)
{
	dg_graph_lock ();

	TRACE(("DG GRAPH ADD\n"));
	//TRACE(("DG NODE CREATE n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
	//new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
	//new_cmd->nredir.n->ncmd.args->narg.next->narg.next));

	/* Create a node for this command. */
	struct dg_node *new_node = dg_node_create (new_cmd);

	/* Step through frontier nodes. */
	struct dg_list *iter = frontier->run_list;

	while (iter)
	{
		/* Follow frontier node and check for dependencies. */
		new_node->dependencies += dg_dep_add (new_node, iter->node);

		/* Increment to next frontier node. */
		iter = iter->next;
	}
	TRACE(("DG GRAPH ADD: deps %d\n", new_node->dependencies));
	//TRACE(("DG NODE CREATE n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
	//new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
	//new_cmd->nredir.n->ncmd.args->narg.next->narg.next));

	/* If no file access dependencies, this is a frontier node. */
	if (new_node->dependencies == 0)
		dg_frontier_add (new_node);

	dg_graph_unlock ();
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
      /* Decrement dependency count. */
      iter->node->dependencies--;

      /* If no more dependencies, add to frontier. */
      if (!iter->node->dependencies)
        dg_frontier_add (iter->node);

      iter = iter->next;
    }

  free_command (graph_node->command);
  if (graph_node->dependents)
    free (graph_node->dependents);
  if (graph_node->files)
    free (graph_node->files);
  free (graph_node);
}


/* Cross check file lists for access conflicts. */
static int
dg_file_check (struct dg_node *node1, struct dg_node *node2)
{
  TRACE(("DG FILE CHECK %p %p\n", node1, node2));
  struct dg_file *files1 = node1->files;
  struct dg_file *files2 = node2->files;

  int mult_read = NO_CLASH;
  while (files1)
    {
      while (files2)
        {
          TRACE(("CHECKING %s and %s\n", files1->file, files2->file));
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
  TRACE(("DG FILE CHECK ret %d\n", mult_read));
  return mult_read;
}


/* Check if NEW_NODE is a dependent of NODE. If so, recursive call
   on NODE's dependents, or add as dependent to NODE as necessary.
   Returns total number of dependencies originating from NODE. */
static int
dg_dep_add (struct dg_node *new_node, struct dg_node *node)
{
  TRACE(("DG DEP ADD  %p %p\n", new_node, node));
  /* Establish dependency. */
  int file_access;

  if (new_node->command == NEOF)
    file_access = WRITE_COLLISION;
  else
    {
      file_access = dg_file_check (new_node, node);
      if (file_access == NO_CLASH)
        return 0;
    }

  int deps = 0;
  struct dg_list *iter = node->dependents;

  /* Check dependency on node's dependents. */
  if (iter)
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
  else if (file_access == WRITE_COLLISION)
    {
      node->dependents = malloc (sizeof (struct dg_list));
      node->dependents->node = new_node;
      node->dependents->next = NULL;
      deps++;
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


/* Create a node for NEW_CMD. */
static struct dg_node *
dg_node_create (union node *new_cmd)
{
  TRACE(("DG NODE CREATE\n"));
  //TRACE(("DG NODE CREATE n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
  //new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
  //new_cmd->nredir.n->ncmd.args->narg.next->narg.next));
  struct dg_node *new_node = malloc (sizeof *new_node);
  new_node->dependents = NULL;
  new_node->dependencies = 0;
  new_node->command = new_cmd;

  union node *flist;
  if (new_cmd->type == NBACKGND)
    {
      flist = new_cmd->nredir.n->ncmd.redirect;

      new_node->files = dg_node_files (flist);
    }
  else if (new_cmd->type == NVAR)
    {
      /* NVAR writes to a variable. */
      struct dg_file *var = malloc (sizeof *var);
      char *cmdstr = new_cmd->nvar.com->ncmd.assign->narg.text;
      var->name_size = strchr (cmdstr, '=') - cmdstr + 1;
      var->file = malloc (var->name_size);
      strncpy (var->file + 1, cmdstr, var->name_size);
      *(var->file) = '$';
      *(var->file + var->name_size) = '\0';
      var->rw = WRITE_ACCESS;
      TRACE(("NVAR: %s\n", var->file));

      struct nodelist *nodes= new_cmd->nvar.com->ncmd.assign->narg.backquote;
      if (nodes)
        TRACE(("NODES NODES NODES\n"));

    }
  else
    ;/* Other node types, such as loops, not yet supported. */

  //TRACE(("DG NODE CREATE n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
  //new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
  //new_cmd->nredir.n->ncmd.args->narg.next->narg.next));
  return new_node;
}


/* Construct file access list for a command. */
static struct dg_file *
dg_node_files (union node *redir)
{
  TRACE(("DG NODE FILES\n"));
  if (!redir)
    return NULL;

  struct dg_file *files = malloc (sizeof *files);
  struct dg_file *iter = files;
  while (1)
    {
      char *file = redir->nfile.fname->narg.text;
      iter->name_size = strlen (file) + 1;
      iter->file = malloc (iter->name_size);
      strncpy (iter->file, file, strlen (file));
      iter->file[iter->name_size - 1] = '\0';
      TRACE(("DG NODE FILES: %s\n", iter->file));

      /* Only handle these for now. */
      if (redir->type == NFROM)
        iter->rw = READ_ACCESS;
      else if (redir->type == NTO || redir->type == NCLOBBER
               || redir->type == NAPPEND)
        iter->rw = WRITE_ACCESS;

      if (redir->nfile.next)
        {
          iter->next = malloc (sizeof *iter->next);
          iter = iter->next;
          redir = redir->nfile.next;
        }
      else
        {
          iter->next = NULL;
          break;
        }
    }
  return files;
}


/* Add a node to frontier. */
static void
dg_frontier_add (struct dg_node *graph_node)
{
  TRACE(("DG FRONTIER ADD\n"));
  //union node* new_cmd = graph_node->command;
  //TRACE(("DG FRONTIER ADD n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
  //new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
  //new_cmd->nredir.n->ncmd.args->narg.next->narg.next));

  /* Allocate new runnables node. */
  struct dg_list *new_tail = malloc (sizeof *new_tail);

  if (frontier->tail)
    {
      TRACE(("DG FRONTIER ADD non-empty\n"));
      //TRACE(("DG FRONTIER ADD n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
      //new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
      //new_cmd->nredir.n->ncmd.args->narg.next->narg.next));
      /* Add new tail to LL. */
      frontier->tail->next = new_tail;

      /* Point new tail to old tail. */
      new_tail->prev = frontier->tail;

      /* Set tail to new tail. */
      frontier->tail = frontier->tail->next;

      /* Set run next if not set. */
      if (!frontier->run_next)
        frontier->run_next = new_tail;
    }
  else
    {
      TRACE(("DG FRONTIER ADD empty\n"));
      //TRACE(("DG FRONTIER ADD n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
      //new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
      //new_cmd->nredir.n->ncmd.args->narg.next->narg.next));
      /* Frontier is currently empty. */
      frontier->run_list = new_tail;
      frontier->run_next = new_tail;
      frontier->tail = new_tail;
      new_tail->prev = NULL;
    }

  /* Fill out node. */
  frontier->tail->node = graph_node;
  frontier->tail->next = NULL;


  // Send a signal to wake up blocked dg_run threads
  pthread_cond_broadcast (&frontier->dg_cond);

  //TRACE(("DG FRONTIER ADD n %p redir %p, args %p, args2 %p, args3 %p\n", new_cmd, new_cmd->nredir.n,
  //new_cmd->nredir.n->ncmd.args, new_cmd->nredir.n->ncmd.args->narg.next,
  //new_cmd->nredir.n->ncmd.args->narg.next->narg.next));
}


/* Wait until frontier has commands. */
void
dg_frontier_nonempty ()
{
  dg_graph_lock ();

  if (!frontier->run_list)
    {
      TRACE(("DG_FRONTIER_NONEMPTY: cond wait\n"));
      pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
      TRACE(("DG_FRONTIER_NONEMPTY: cond wait complete\n"));
    }

  dg_graph_unlock ();
  return;
}

/* Remove the runnables list node corresponding to a frontier
   node that has completed execution. */
void
dg_frontier_remove (struct dg_list *rem)
{
	dg_graph_lock();

	TRACE (("DG_FRONTIER_REMOVE rem %p\n", rem));

	/* Do NOT remove EOF from frontier. */
	if (rem->node->command == NEOF) {
		dg_graph_unlock();
		return;
	}

	TRACE(("DG_FRONTIER_REMOVE: still here\n"));
	if (rem->prev)
	{
		/* Node is not first in LL. */
		rem->prev->next = rem->next;

		if (rem->next)
			rem->next->prev = rem->prev;
	}
	else
	{
		TRACE(("DG_FRONTIER_REMOVE: new runlist %p\n", rem->next));
		/* Node is first in LL. */
		frontier->run_list = rem->next;

		if (rem->next)
			rem->next->prev = NULL;
	}

	if (frontier->tail == rem)
	{
		TRACE(("DG_FRONTIER_REMOVE: new tail %p\n", rem->prev));
		/* Node is last in LL. */
		frontier->tail = rem->prev;
	}

	dg_graph_remove (rem->node);
	free (rem);

	dg_graph_unlock();
}


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
    if (node->nif.ifpart)
      free_command (node->nif.ifpart);
    if (node->nif.elsepart)
      free_command (node->nif.elsepart);
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
