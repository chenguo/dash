/* By Chen Guo
   Manages states of variables at different points in time. */

#ifndef STATES_H
#define STATES_H

#include "dgraph.h"

struct var_acc
{
  struct var_acc *next;
  struct dg_node *node;
  union node *arg;
};

/* A state of a variable. */
struct var_state
{
  struct var_state *next;       /* Next state. */
  struct var_state *prev;       /* Previous state. */
  char *val;                    /* Value of this state. */
  int accessors;                /* Number of accessors of state. */
  struct var_acc *acc_list;     /* List of waiting accessors. Only populated
                                   while VAL is null. */
};

/* Modified var entry from dash. */
struct var2
{
  struct var2 *next;            /* Next entry in hash list. */
  int flags;                    /* Flags, from var.h. */
  char *name;                   /* Var name. */
  void (*func)(const char *);   /* Function to be called to set/unset var. */
  struct var_state *head;       /* Head of state list. */
  struct var_state *tail;       /* Tail of state list. */
};

/* List of states (of different variables). */
struct state_list
{
  struct var_state *state;
  struct state_list *next;
};

void initvar2 (void);
struct var_state * create_state (char *);
void write_state (struct var_state *, char *);
void queue_state (struct dg_node *, struct var_state *);
struct var_state * read_state (char *name);

#endif
