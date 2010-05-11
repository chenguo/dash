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

static int dg_file_check (struct dg_node *, struct dg_node *);
static int dg_dep_add (struct dg_node *, struct dg_node *);
static struct dg_file * dg_node_files (union node *);
static struct dg_node * dg_node_create (union node *);
static void dg_graph_remove (struct dg_node *);
static void dg_frontier_set_eof (void);
static void dg_frontier_add (struct dg_node *);
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
  frontier->eof = 0;
  pthread_mutex_init (&frontier->dg_lock, NULL);
  pthread_cond_init (&frontier->dg_cond, NULL);
}


/* Lock down graph. */
void
dg_graph_lock(void)
{
  pthread_mutex_lock(&frontier->dg_lock);
}


/* Unlock graph. */
void
dg_graph_unlock(void)
{
  pthread_mutex_unlock(&frontier->dg_lock);
}


/* Return a process in the frontier. */
struct dg_list *
dg_graph_run (void)
{
  TRACE(("DG GRAPH RUN\n"));
  dg_graph_lock ();

  /* Blocks until there's nodes in the graph. */
  while (frontier->run_next == NULL)
    {
      //TRACE(("DG GRAPH RUN: cond wait\n"));
      pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
      //TRACE(("DG GRAPH RUN: cond wait complete\n"));
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
  TRACE(("DG GRAPH ADD type %d\n", new_cmd->type));
  dg_graph_lock ();
  if (new_cmd == NEOF)
    {
      dg_frontier_set_eof ();
      return;
    }

  /* Create a node for this command. */
  struct dg_node *new_node = dg_node_create (new_cmd);

  /* Step through frontier nodes. */
  struct dg_list *iter = frontier->run_list;
  while (iter)
    {
      /* Follow frontier node and check for dependencies. Don't use += operator
         here, for some reason it sets dependencies to 0. */
      new_node->dependencies += dg_dep_add (new_node, iter->node);
      iter = iter->next;
    }
  TRACE(("DG GRAPH ADD: %p: deps %d\n", new_node, new_node->dependencies));

  /* If no file access dependencies, this is a frontier node. */
  if (new_node->dependencies == 0)
    dg_frontier_add (new_node);

  dg_graph_unlock ();
}


/* Add a node specifically as a dependency of a particular graph node. This
   is used for if statements. */
static void
dg_graph_post_add (union node *post_cmd, struct dg_node *graph_node)
{
  TRACE(("DG GRAPH POST ADD %p\n", graph_node));

  /* Create a node for this command. */
  struct dg_node *new_node = dg_node_create (post_cmd);

  new_node->dependencies += dg_dep_add (new_node, graph_node);
  TRACE(("DG GRAPH POST ADD: %p: type %d deps %d\n", new_node, new_node->command->type, new_node->dependencies));
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
      /* Decrement dependency count. */
      iter->node->dependencies--;
      TRACE(("DG GRAPH REMOVE: %p dep: type: %d: %p dep %d\n", graph_node, iter->node->command->type, iter->node, iter->node->dependencies));
      /* If no more dependencies, add to frontier. */
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


/* Cross check file lists for access conflicts. */
static int
dg_file_check (struct dg_node *node1, struct dg_node *node2)
{
  TRACE(("DG FILE CHECK %p:%d %p:%d\n", node1, node1->dependencies, node2, node2->dependencies));
  struct dg_file *files1 = node1->files;
  struct dg_file *files2 = node2->files;

  int mult_read = NO_CLASH;
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
  TRACE(("DG DEP ADD  %p:%d %p:%d\n", new_node, new_node->dependencies, node, node->dependencies));
  /* Establish dependency. */
  int file_access = dg_file_check (new_node, node);
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
          iter->next = malloc (sizeof (struct dg_list));
          iter = iter->next;
          iter->node = new_node;
          iter->next = NULL;
          deps++;
        }
    }
  else if (file_access == WRITE_COLLISION)
    {
      node->dependents = malloc (sizeof (struct dg_list));
      node->dependents->node = new_node;
      node->dependents->next = NULL;
      deps++;
    }
  return deps;
}


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

  TRACE(("DG NODE CREATE: %p file list: ", new_node));
  struct dg_file *iter = new_node->files;
  while (iter)
    {
      TRACE(("%s  ", iter->name));
      iter = iter->next;
    }
  TRACE(("\n"));

  return new_node;
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
    {
      /* Variable assignment. */
      struct dg_file *file = malloc (sizeof *file);

      char *cmdstr = n->nvar.com->ncmd.assign->narg.text;
      file->name_size = strchr (cmdstr, '=') - cmdstr + 1;
      file->name = malloc (file->name_size);
      file->name[0] = '$';
      strncpy (file->name + 1, cmdstr, file->name_size);
      file->name[file->name_size - 1] = '\0';
      file->rw = WRITE_ACCESS;
      TRACE(("DG NODE FILES: %s\n", file->name));

      struct nodelist *nodes= n->nvar.com->ncmd.assign->narg.backquote;
      /* TODO: handle files access to command that writes shellvar. */

      return file;
    }
  case NBACKGND:
    return dg_node_files (n->nredir.n);
  case NSEMI:
    {
      /* Resolve ch1. */
      struct dg_file *file = dg_node_files (n->nbinary.ch1);

      /* Resolve ch2. */
      if (file)
        {
          struct dg_file *iter;

          /* Find end of LL. */
          iter = file;
          while (iter->next)
            iter = iter->next;

          iter->next = dg_node_files (n->nbinary.ch2);
          return file;
        }
      else
        return dg_node_files (n->nbinary.ch2);
      break;
    }
  case NIF:
    {
      /* Resolve test part. */
      struct dg_file *file = dg_node_files (n->nif.test);
      /* Resolve if part. */
      struct dg_file *iter = NULL;
      if (file)
        {
          iter = file;
          while (iter->next)
            iter = iter->next;
          iter->next = dg_node_files (n->nif.ifpart);
        }
      else
        file = dg_node_files (n->nif.ifpart);
      /* Resolve else part. */
      if (file)
        {
          if (!iter)
            iter = file;
          while (iter->next)
            iter = iter->next;
          iter->next = dg_node_files (n->nif.elsepart);
        }
      else
        file = dg_node_files (n->nif.elsepart);
      return file;
      break;
    }
  case NTO:
  case NCLOBBER:
  case NFROM:
  case NFROMTO:
  case NAPPEND:
    {
      /* File node. Jackpot. */
      struct dg_file *file = malloc (sizeof *file);

      /* Get name and name size. */
      char *fname = n->nfile.fname->narg.text;
      file->name_size = strlen (fname) + 1;
      file->name = malloc (file->name_size);
      strncpy (file->name, fname, strlen (fname));
      file->name[file->name_size - 1] = '\0';
      TRACE(("DG NODE FILES: %s\n", file->name));

      /* RW access status. */
      if (n->type == NFROM)
        file->rw = READ_ACCESS;
      else if (n->type == NTO || n->type == NCLOBBER
               || n->type == NAPPEND || n->type == NFROMTO)
        file->rw = WRITE_ACCESS;

      /* Resolve next file. */
      file->next = dg_node_files (n->nfile.next);

      return file;
    }
  default:
    break;
  }

  return NULL;
}


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
}


/* Add NEOF node to frontier. */
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
  TRACE(("DG FRONTER SET: run list %p\n", frontier->run_list));
  if (frontier->run_list == NULL)
    dg_frontier_add_eof ();
}

/* Wait until frontier has commands. */
void
dg_frontier_nonempty (void)
{
  dg_graph_lock ();

  if (!frontier->run_list)
    {
      //TRACE(("DG FRONTIER NONEMPTY: cond wait\n"));
      pthread_cond_wait (&frontier->dg_cond, &frontier->dg_lock);
      //TRACE(("DG FRONTIER NONEMPTY: cond wait complete\n"));
    }

  dg_graph_unlock ();
  return;
}


/* Extra processing for removing NIF from frontier. */
static void
dg_frontier_remove_nif (struct dg_list *rem, int status)
{
  /* Preserve dependents. These are commands after the if with file
     collisions. */
  struct dg_list *post_cmd = rem->node->dependents;
  while (post_cmd)
    {
      post_cmd->node->dependencies--;
      post_cmd = post_cmd->next;
    }
  post_cmd = rem->node->dependents;
  rem->node->dependents = NULL;

  /* Set either if or else part to run. */
  if (status == 0)
    TRACE(("DG FRONTIER REMOVE: if status true.\n"));
  else
    TRACE(("DG FRONTIER REMOVE: if status false.\n"));
  union node *run = (status == 0)?
    rem->node->command->nif.ifpart : rem->node->command->nif.elsepart;
  node_proc (run, rem->node);

  /* Append post commands. */
  while (post_cmd)
    {
      post_cmd->node->dependencies = dg_dep_add (post_cmd->node, rem->node);
      if (post_cmd->node->dependencies == 0)
        dg_frontier_add (post_cmd->node);
      post_cmd = post_cmd->next;
    }
}


/* Remove the runnables list node corresponding to a frontier
   node that has completed execution. */
void
dg_frontier_remove (struct dg_list *rem, int status)
{
  dg_graph_lock();
  TRACE (("DG FRONTIER REMOVE %p, status 0x%x\n", rem->node, status));

  if (rem->node->command->type == NIF)
    dg_frontier_remove_nif (rem, status);

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


/* Process node tree returned by parsecmd. */
void
node_proc (union node *n, struct dg_node *graph_node)
{
#define GRAPH_ADD(n, graph_node)\
{\
  if (graph_node) dg_graph_post_add (n, graph_node);\
  else dg_graph_add (n);\
}

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
    TRACE(("NODE PROC: NCMD\n"));
    int wrap = 0;
    if (n->ncmd.args && n->ncmd.args->narg.text)
      {
        TRACE(("NODE PROC: NCMD: ARGS %s\n", n->ncmd.args->narg.text));
	
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
        TRACE(("NODE PROC: NCMD: ASSIGN next %p\n", n->ncmd.assign->narg.next));
        TRACE(("NODE PROC: NCMD: ASSIGN text %s\n", n->ncmd.assign->narg.text));
        TRACE(("NODE PROC: NCMD: ASSIGN bqte %p\n", n->ncmd.assign->narg.backquote));
        struct nodelist *tmp = n->ncmd.assign->narg.backquote;
        while (tmp)
          {
            TRACE(("NODE PROC: NODELIST: type %d\n", tmp->n->type));
            if (tmp->n->type == NCMD)
              TRACE(("NODR PROC: NCMD: ARGS text %s\n", tmp->n->ncmd.args->narg.text));
            tmp = tmp->next;
          }
        wrap = 2;
      }
    else
      wrap = 0;

    if (wrap == 1)
      {
        TRACE(("NODE PROC: wrapping with NBACKGND\n"));
        union node *nwrap = (union node *) malloc (sizeof (struct nredir));
        nwrap->type = NBACKGND;
        nwrap->nredir.n = n;
        nwrap->nredir.redirect = NULL;
        n = nwrap;
      }
    else if (wrap == 2)
      {
        TRACE(("NODE PROC: wrapping with NVAR\n"));
        union node *nwrap = (union node *) malloc (sizeof (struct nvar));
        nwrap->type = NVAR;	
        nwrap->nvar.com = n;
        n = nwrap;
      }					

    if (n->type != NCMD)
      {
	TRACE(("NODE PROC: nodetype: %i\n", n->type));
        GRAPH_ADD(n, graph_node);
      }
    else
      {
        TRACE(("NODE PROC: cd or exit\n"));
        /* NCMD: for now, only cd and exit. */
        evaltree (n, 0, NULL);
      }
    break;
  case NVAR:
    TRACE(("NODE PROC: NVAR\n"));
    TRACE(("NODE PROC: this shouldn't happen!\n"));
    break;
  case NPIPE:
    TRACE(("NODE PROC: NPIPE\n"));
    break;
  case NREDIR:
  case NBACKGND:
  case NSUBSHELL:
    TRACE(("NODE PROC: NREDIR\n"));
    break;
  case NAND:
  case NOR:
  case NSEMI:
    TRACE(("NODE PROC: NSEMI\n"));
    if (n->nbinary.ch1)  
      node_proc (n->nbinary.ch1, graph_node);
    if (n->nbinary.ch2)
      node_proc (n->nbinary.ch2, graph_node);
    break;
  case NWHILE:
  case NUNTIL:
    TRACE(("NODE PROC: NBINARY\n"));
    break;
  case NIF:
    TRACE(("NODE PROC: NIF\n"));
    union node *nwrap;
    /* Wrap test part as background. */
    if (n->nif.test && n->nif.test->type != NBACKGND)
      {
        nwrap = (union node *) malloc (sizeof (struct nredir));
        nwrap->type = NBACKGND;
        nwrap->nredir.n = n->nif.test;
        nwrap->nredir.redirect = NULL;
        n->nif.test = nwrap;
      }
    GRAPH_ADD(n, graph_node);
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
