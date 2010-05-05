#include <pthread.h>

/* Linked list of command nodes. */
struct dg_list
{
  struct dg_node *node;        /* Node for dependent command. */
  struct dg_list *next;        /* Next node in list. */
  struct dg_list *prev;        /* Previous node in list. */
};

/* File/var dependencies of a command. */
struct dg_file
{
  char *file;                  /* Name of file or var. */
  int name_size;               /* Length of the file name. */
  int rw;                      /* Read/write access. */
  struct dg_file *next;        /* Next file dependency. */
};

/* Node of directed command graph. */
struct dg_node
{
  struct dg_list *dependents;  /* Commands dependent on this one. */
  struct dg_file *files;       /* Files/vars this command reads/writes. */
  int dependencies;            /* Number of blocking commands. */
  union node *command;         /* Command to evaluate. */
};


/* Frontier of directed command graph. */
struct dg_frontier
{
  struct dg_list *run_list;    /* Running/runnable commands. */
  struct dg_list *run_next;    /* Next non-running runnable command. */
  struct dg_list *tail;        /* Last element in LL. */
  pthread_mutex_t dg_lock;     /* Lock for directed graph. */
  pthread_cond_t dg_cond;     /* Run conditional variable */
};

void dg_graph_init (void);
void dg_graph_lock (void);
void dg_graph_unlock (void);
void dg_graph_add (union node *);
struct dg_list *dg_graph_run (void);
void dg_frontier_remove (struct dg_list *);
