/* By Chen Guo. */
#ifndef DGRAPH_H
#define DGRAPH_H

#include <stdbool.h>
#include <pthread.h>

/* Linked list of command nodes. */
struct dg_list
{
  struct dg_node *node;        /* Node for dependent command. */
  struct dg_list *next;        /* Next node in list. */
};

/* Dependency types. */
enum { REG_FILE, CONTINUE, BREAK, WILDCARD };

/* File dependencies of a command. */
struct f_dep
{
  int type;                    /* Type. */
  char *name;                  /* Name of file. */
  int name_size;               /* Length of the file name. */
  bool write;                  /* Read/write, true if write. */
};

/* File dependency. */
struct dg_file
{
  int type;
  struct dg_file *next;
  union
    {
      int nest;                /* For CONTINUE/BREAK. */
      struct f_dep *file;      /* For regular files. */
    };
};


/* Graph node flags. */
#define KEEP_CMD     0x00
#define FREE_CMD     0x01
#define TEST_CMD     0x02
#define BODY_CMD     0x04
#define TEST_STATUS  0x08
#define BODY_STATUS  0x10
#define CANCELLED    0x20

struct dg_fnode;

/* Node of directed command graph. */
/* TODO: make a union. For example, regular commands don't need parent,
   and only loop commands need nest level and iteration number. */
struct dg_node
{
  struct dg_list *dependents;  /* Commands dependent on this one. */
  struct dg_file *files;        /* Files/vars this command reads/writes. */
  int dependencies;            /* Number of blocking commands. */
  union node *command;         /* Command to evaluate. */
  struct dg_fnode *parent;     /* Parent command (IF, WHILE, etc). */
  int nest;                    /* Loop nest level. 0 is base. */
  unsigned long iteration;     /* Iteration number of the parent loop. */
  int flag;                    /* Flags:
                                    KEEP_CMD: part of node tree, don't free.
                                    FREE_CMD: free commnd.
                                    TEST_CMD: part of test condition.
                                    BODY_CMD: body command.
                                    TEST_STATUS: status is test status.
                                    BODY_STATUS: body status. */
};

/* Frontier node types. */
enum
{
  DG_NCMD,                     /* Regular command. */
  DG_NAND,                     /* && */
  DG_NOR,                      /* || */
  DG_NIF,                      /* IF */
  DG_NWHILE,                   /* WHILE */
  DG_NUNTIL,                   /* UNTIL */
  DG_NFOR                      /* FOR */
};

/* Frontier node. */
struct dg_fnode
{
  int type;                    /* Type. */
  struct dg_node *node;        /* Graph node. */
  struct dg_fnode *next;       /* Next frontier node. */
  struct dg_fnode *prev;       /* Previous frontier node. */
  int status;                  /* Return status. */
  int active;                  /* Active nested commands. */
  unsigned long iteration;     /* Iteration number of loop command. */
};

/* Frontier of directed command graph. */
struct dg_frontier
{
  struct dg_fnode *run_list;   /* Running/runnable commands. */
  struct dg_fnode *run_next;   /* Next non-running runnable command. */
  struct dg_fnode *tail;       /* Last element in LL. */
  int eof;                     /* EOF flag. */
  pthread_mutex_t dg_lock;     /* Lock for directed graph. */
  pthread_cond_t dg_cond;      /* Run conditional variable */
};

void dg_graph_init (void);
struct dg_fnode *dg_graph_run (void);
void dg_node_dep_decr (struct dg_node *);
void dg_frontier_nonempty (void);
void dg_frontier_remove (struct dg_fnode *);
void node_proc (union node *);

#endif
