/* By Chen Guo. */

#include <pthread.h>

/* Linked list of command nodes. */
struct dg_list
{
  struct dg_node *node;        /* Node for dependent command. */
  struct dg_list *next;        /* Next node in list. */
};

/* File/var dependencies of a command. */
struct dg_file
{
  char *name;                  /* Name of file or var. */
  int name_size;               /* Length of the file name. */
  int rw;                      /* Read/write access. */
  struct dg_file *next;        /* Next file dependency. */
};

/* Graph node flags. */
#define KEEP_CMD     0
#define FREE_CMD     0b1
#define TEST_CMD     0b10
#define STATUS_CMD   0b100

struct dg_fnode;

/* Node of directed command graph. */
struct dg_node
{
  struct dg_list *dependents;  /* Commands dependent on this one. */
  struct dg_file *files;       /* Files/vars this command reads/writes. */
  int dependencies;            /* Number of blocking commands. */
  union node *command;         /* Command to evaluate. */
  struct dg_fnode *parent;     /* Parent to relay return status to. */
  int flag;                    /* Flags. */
};

/* Frontier node types. */
enum
{
  DG_NCMD,
  DG_NAND,
  DG_NOR,
  DG_NIF,
  DG_NWHILE,
  DG_NUNTIL,
  DG_NFOR
};

/* Frontier node for regular commands. */
struct dg_fnode
{
  int type;
  struct dg_node *node;
  struct dg_fnode *next;
  struct dg_fnode *prev;
  int active;
  int status;
};

/* TODO: loop commands and loop body commands need to be
   fleshed out. */
/* Frontier node for a loop command. */
struct dg_fnode_loop
{
  int type;
  struct dg_node *node;
  struct dg_fnode *next;
  struct dg_fnode *prev;
  int active;
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
void dg_frontier_nonempty (void);
void dg_frontier_remove (struct dg_fnode *);
void node_proc (union node *);
