/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * batch.c: Chains of tasks that wait for each other through holds. Port of
 * js/gdm/batch.js.
 *
 * In order for transformation animations to look good, they need to be
 * incremental and have some order to them (e.g., fade out hidden items,
 * then shrink to close the void left over). Chaining animations in this way can
 * be error-prone and wordy using just ease() callbacks.
 *
 * The classes in this file help with this:
 *
 * - #ShellBatchTask: encapsulates schedulable work.
 *
 * - #ShellConsecutiveBatch: runs a series of tasks in order and completes
 *   when the last in the series finishes.
 *
 * - #ShellConcurrentBatch: runs a set of tasks at the same time and completes
 *   when the last to finish completes.
 *
 * - #ShellBatchHold: prevents a batch from completing the pending task until
 *   the hold is released.
 *
 * Batches are tasks themselves, so they can be nested. Where the JS took
 * plain functions among the tasks, wrap them with shell_batch_task_new ().
 *
 * Copyright 2011 Red Hat, Inc
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gdm/batch.h"

/* ------------------------------------------------------------------------ */
/* ShellBatchTask                                                            */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellBatchTaskFunc func;
  gpointer user_data;
  GDestroyNotify destroy;
} ShellBatchTaskPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellBatchTask, shell_batch_task, G_TYPE_OBJECT)

static ShellBatchHold *
shell_batch_task_real_run (ShellBatchTask *task)
{
  ShellBatchTaskPrivate *priv = shell_batch_task_get_instance_private (task);
  ShellBatchHold *hold;

  if (priv->func == NULL)
    return NULL;

  hold = priv->func (priv->user_data);

  return hold != NULL ? g_object_ref (hold) : NULL;
}

static void
shell_batch_task_finalize (GObject *object)
{
  ShellBatchTask *task = SHELL_BATCH_TASK (object);
  ShellBatchTaskPrivate *priv = shell_batch_task_get_instance_private (task);

  if (priv->destroy != NULL)
    priv->destroy (priv->user_data);
  priv->destroy = NULL;

  G_OBJECT_CLASS (shell_batch_task_parent_class)->finalize (object);
}

static void
shell_batch_task_class_init (ShellBatchTaskClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = shell_batch_task_finalize;

  klass->run = shell_batch_task_real_run;
}

static void
shell_batch_task_init (ShellBatchTask *task)
{
}

/**
 * shell_batch_task_new:
 * @func: (scope notified) (closure user_data) (destroy destroy) (nullable):
 *   the work, it returns a #ShellBatchHold to make the batch wait, or %NULL
 * @user_data: data for @func
 * @destroy: called with @user_data when the task is finalized
 *
 * Creates a task that runs @func (the `Task` of js/gdm/batch.js).
 *
 * Returns: (transfer full): a new #ShellBatchTask
 */
ShellBatchTask *
shell_batch_task_new (ShellBatchTaskFunc func,
                      gpointer           user_data,
                      GDestroyNotify     destroy)
{
  ShellBatchTask *task = g_object_new (SHELL_TYPE_BATCH_TASK, NULL);
  ShellBatchTaskPrivate *priv = shell_batch_task_get_instance_private (task);

  priv->func = func;
  priv->user_data = user_data;
  priv->destroy = destroy;

  return task;
}

/**
 * shell_batch_task_run:
 * @task: a #ShellBatchTask
 *
 * Runs the task.
 *
 * Returns: (transfer full) (nullable): the hold to wait for, if any
 */
ShellBatchHold *
shell_batch_task_run (ShellBatchTask *task)
{
  g_return_val_if_fail (SHELL_IS_BATCH_TASK (task), NULL);

  return SHELL_BATCH_TASK_GET_CLASS (task)->run (task);
}

/* ------------------------------------------------------------------------ */
/* ShellBatchHold                                                            */
/* ------------------------------------------------------------------------ */

enum
{
  RELEASE,

  N_HOLD_SIGNALS
};

static guint hold_signals[N_HOLD_SIGNALS] = { 0, };

struct _ShellBatchHold
{
  ShellBatchTask parent;

  int acquisitions;
};

G_DEFINE_FINAL_TYPE (ShellBatchHold, shell_batch_hold, SHELL_TYPE_BATCH_TASK)

static ShellBatchHold *
shell_batch_hold_run (ShellBatchTask *task)
{
  return g_object_ref (SHELL_BATCH_HOLD (task));
}

static void
shell_batch_hold_class_init (ShellBatchHoldClass *klass)
{
  ShellBatchTaskClass *task_class = SHELL_BATCH_TASK_CLASS (klass);

  task_class->run = shell_batch_hold_run;

  /**
   * ShellBatchHold::release:
   * @hold: the #ShellBatchHold
   *
   * Emitted when the last acquisition of the hold is released.
   */
  hold_signals[RELEASE] =
    g_signal_new ("release",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_batch_hold_init (ShellBatchHold *hold)
{
  hold->acquisitions = 1;
}

/**
 * shell_batch_hold_new:
 *
 * Creates a hold, acquired once.
 *
 * Returns: (transfer full): a new #ShellBatchHold
 */
ShellBatchHold *
shell_batch_hold_new (void)
{
  return g_object_new (SHELL_TYPE_BATCH_HOLD, NULL);
}

/**
 * shell_batch_hold_acquire:
 * @hold: a #ShellBatchHold
 *
 * Acquires the hold once more. A hold that was released cannot be acquired
 * again.
 */
void
shell_batch_hold_acquire (ShellBatchHold *hold)
{
  g_return_if_fail (SHELL_IS_BATCH_HOLD (hold));
  g_return_if_fail (hold->acquisitions > 0);

  hold->acquisitions++;
}

static void
on_other_hold_released (ShellBatchHold *other,
                        ShellBatchHold *hold)
{
  g_autoptr (ShellBatchHold) keep = g_object_ref (hold);

  g_signal_handlers_disconnect_by_func (other, on_other_hold_released, hold);
  shell_batch_hold_release (hold);
}

static void
unref_data (gpointer  data,
            GClosure *closure)
{
  g_object_unref (data);
}

/**
 * shell_batch_hold_acquire_until_after:
 * @hold: a #ShellBatchHold
 * @other: another #ShellBatchHold
 *
 * Makes @hold wait for @other: if @other is still acquired, @hold is acquired
 * once more and released when @other is.
 */
void
shell_batch_hold_acquire_until_after (ShellBatchHold *hold,
                                      ShellBatchHold *other)
{
  g_return_if_fail (SHELL_IS_BATCH_HOLD (hold));
  g_return_if_fail (SHELL_IS_BATCH_HOLD (other));

  if (!shell_batch_hold_is_acquired (other))
    return;

  shell_batch_hold_acquire (hold);
  g_signal_connect_data (other, "release",
                         G_CALLBACK (on_other_hold_released),
                         g_object_ref (hold), unref_data, 0);
}

/**
 * shell_batch_hold_release:
 * @hold: a #ShellBatchHold
 *
 * Releases the hold once; ::release is emitted when no acquisition is left.
 */
void
shell_batch_hold_release (ShellBatchHold *hold)
{
  g_return_if_fail (SHELL_IS_BATCH_HOLD (hold));

  hold->acquisitions--;

  if (hold->acquisitions == 0)
    {
      g_autoptr (ShellBatchHold) keep = g_object_ref (hold);

      g_signal_emit (hold, hold_signals[RELEASE], 0);
    }
}

/**
 * shell_batch_hold_is_acquired:
 * @hold: a #ShellBatchHold
 *
 * Returns: whether the hold has not been released completely
 */
gboolean
shell_batch_hold_is_acquired (ShellBatchHold *hold)
{
  g_return_val_if_fail (SHELL_IS_BATCH_HOLD (hold), FALSE);

  return hold->acquisitions > 0;
}

/* ------------------------------------------------------------------------ */
/* ShellBatch                                                                */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GPtrArray *tasks;        /* ShellBatchTask */
  int current_index;
  ShellBatchHold *hold;
} ShellBatchPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellBatch, shell_batch, SHELL_TYPE_BATCH_TASK)

static ShellBatchHold *
batch_run_task (ShellBatch *batch)
{
  ShellBatchPrivate *priv = shell_batch_get_instance_private (batch);
  g_autoptr (ShellBatchTask) task = NULL;

  if (priv->current_index < 0 || priv->current_index >= (int) priv->tasks->len)
    return NULL;

  task = g_object_ref (g_ptr_array_index (priv->tasks, priv->current_index));

  return shell_batch_task_run (task);
}

static void
batch_next_task (ShellBatch *batch)
{
  ShellBatchPrivate *priv = shell_batch_get_instance_private (batch);
  g_autoptr (ShellBatch) keep = g_object_ref (batch);

  priv->current_index++;

  /* if the entire batch of tasks is finished, release
   * the hold and notify anyone waiting on the batch */
  if (priv->current_index >= (int) priv->tasks->len)
    {
      shell_batch_hold_release (priv->hold);
      return;
    }

  SHELL_BATCH_GET_CLASS (batch)->process (batch);
}

/**
 * shell_batch_run:
 * @batch: a #ShellBatch
 *
 * Starts the tasks of the batch.
 *
 * Returns: (transfer none): the hold that is released when all the tasks are
 *   done (it can be released already)
 */
ShellBatchHold *
shell_batch_run (ShellBatch *batch)
{
  ShellBatchPrivate *priv;
  g_autoptr (ShellBatch) keep = NULL;

  g_return_val_if_fail (SHELL_IS_BATCH (batch), NULL);

  priv = shell_batch_get_instance_private (batch);
  keep = g_object_ref (batch);

  /* acquire a hold to get released when the entire
   * batch of tasks is finished */
  g_clear_object (&priv->hold);
  priv->hold = shell_batch_hold_new ();
  priv->current_index = 0;
  SHELL_BATCH_GET_CLASS (batch)->process (batch);

  return priv->hold;
}

/**
 * shell_batch_cancel:
 * @batch: a #ShellBatch
 *
 * Drops the tasks that did not start yet. The one that is running finishes.
 */
void
shell_batch_cancel (ShellBatch *batch)
{
  ShellBatchPrivate *priv;

  g_return_if_fail (SHELL_IS_BATCH (batch));

  priv = shell_batch_get_instance_private (batch);

  if (priv->tasks->len > (guint) (priv->current_index + 1))
    g_ptr_array_set_size (priv->tasks, priv->current_index + 1);
}

static ShellBatchHold *
shell_batch_real_run (ShellBatchTask *task)
{
  ShellBatchHold *hold = shell_batch_run (SHELL_BATCH (task));

  return hold != NULL ? g_object_ref (hold) : NULL;
}

static void
shell_batch_dispose (GObject *object)
{
  ShellBatchPrivate *priv = shell_batch_get_instance_private (SHELL_BATCH (object));

  g_clear_object (&priv->hold);
  g_clear_pointer (&priv->tasks, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_batch_parent_class)->dispose (object);
}

static void
shell_batch_class_init (ShellBatchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellBatchTaskClass *task_class = SHELL_BATCH_TASK_CLASS (klass);

  object_class->dispose = shell_batch_dispose;

  task_class->run = shell_batch_real_run;
}

static void
shell_batch_init (ShellBatch *batch)
{
  ShellBatchPrivate *priv = shell_batch_get_instance_private (batch);

  priv->tasks = g_ptr_array_new_with_free_func (g_object_unref);
  priv->current_index = -1;
}

static void
batch_add_tasks (ShellBatch          *batch,
                 ShellBatchTask * const *tasks,
                 gsize                n_tasks)
{
  ShellBatchPrivate *priv = shell_batch_get_instance_private (batch);
  gsize i;

  for (i = 0; i < n_tasks; i++)
    {
      g_return_if_fail (SHELL_IS_BATCH_TASK (tasks[i]));
      g_ptr_array_add (priv->tasks, g_object_ref (tasks[i]));
    }
}

/* ------------------------------------------------------------------------ */
/* ShellConsecutiveBatch                                                     */
/* ------------------------------------------------------------------------ */

struct _ShellConsecutiveBatch
{
  ShellBatch parent;
};

G_DEFINE_FINAL_TYPE (ShellConsecutiveBatch, shell_consecutive_batch, SHELL_TYPE_BATCH)

static void
on_consecutive_hold_released (ShellBatchHold *hold,
                              ShellBatch     *batch)
{
  g_autoptr (ShellBatch) keep = g_object_ref (batch);

  g_signal_handlers_disconnect_by_func (hold, on_consecutive_hold_released, batch);
  batch_next_task (batch);
}

static void
shell_consecutive_batch_process (ShellBatch *batch)
{
  g_autoptr (ShellBatchHold) hold = batch_run_task (batch);

  if (hold != NULL && shell_batch_hold_is_acquired (hold))
    {
      /* This task is inhibiting the batch. Wait on it
       * before processing the next one. */
      g_signal_connect_data (hold, "release",
                             G_CALLBACK (on_consecutive_hold_released),
                             g_object_ref (batch), unref_data, 0);
    }
  else
    {
      /* This task finished, process the next one */
      batch_next_task (batch);
    }
}

static void
shell_consecutive_batch_class_init (ShellConsecutiveBatchClass *klass)
{
  ShellBatchClass *batch_class = SHELL_BATCH_CLASS (klass);

  batch_class->process = shell_consecutive_batch_process;
}

static void
shell_consecutive_batch_init (ShellConsecutiveBatch *batch)
{
}

/**
 * shell_consecutive_batch_new:
 * @tasks: (array length=n_tasks) (transfer none): the tasks, run in order
 * @n_tasks: the number of @tasks
 *
 * Creates a batch that runs each task when the hold of the previous one is
 * released.
 *
 * Returns: (transfer full): a new #ShellConsecutiveBatch
 */
ShellConsecutiveBatch *
shell_consecutive_batch_new (ShellBatchTask * const *tasks,
                             gsize                   n_tasks)
{
  ShellConsecutiveBatch *batch = g_object_new (SHELL_TYPE_CONSECUTIVE_BATCH, NULL);

  batch_add_tasks (SHELL_BATCH (batch), tasks, n_tasks);

  return batch;
}

/* ------------------------------------------------------------------------ */
/* ShellConcurrentBatch                                                      */
/* ------------------------------------------------------------------------ */

struct _ShellConcurrentBatch
{
  ShellBatch parent;
};

G_DEFINE_FINAL_TYPE (ShellConcurrentBatch, shell_concurrent_batch, SHELL_TYPE_BATCH)

static void
shell_concurrent_batch_process (ShellBatch *batch)
{
  ShellBatchPrivate *priv = shell_batch_get_instance_private (batch);
  g_autoptr (ShellBatchHold) hold = batch_run_task (batch);

  if (hold != NULL)
    shell_batch_hold_acquire_until_after (priv->hold, hold);

  /* Regardless of the state of the just run task,
   * fire off the next one, so all the tasks can run
   * concurrently. */
  batch_next_task (batch);
}

static void
shell_concurrent_batch_class_init (ShellConcurrentBatchClass *klass)
{
  ShellBatchClass *batch_class = SHELL_BATCH_CLASS (klass);

  batch_class->process = shell_concurrent_batch_process;
}

static void
shell_concurrent_batch_init (ShellConcurrentBatch *batch)
{
}

/**
 * shell_concurrent_batch_new:
 * @tasks: (array length=n_tasks) (transfer none): the tasks, all started at once
 * @n_tasks: the number of @tasks
 *
 * Creates a batch that runs all its tasks at the same time; its hold is
 * released when the last of their holds is.
 *
 * Returns: (transfer full): a new #ShellConcurrentBatch
 */
ShellConcurrentBatch *
shell_concurrent_batch_new (ShellBatchTask * const *tasks,
                            gsize                   n_tasks)
{
  ShellConcurrentBatch *batch = g_object_new (SHELL_TYPE_CONCURRENT_BATCH, NULL);

  batch_add_tasks (SHELL_BATCH (batch), tasks, n_tasks);

  return batch;
}
