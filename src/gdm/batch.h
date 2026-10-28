/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * batch.h: Chains of tasks that wait for each other through holds. Port of
 * js/gdm/batch.js.
 *
 * Copyright 2011 Red Hat, Inc
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _ShellBatchHold ShellBatchHold;

/**
 * ShellBatchTaskFunc:
 * @user_data: (closure): the data given to shell_batch_task_new ()
 *
 * The work of a #ShellBatchTask.
 *
 * Returns: (transfer none) (nullable): a #ShellBatchHold the following tasks
 *   wait for, or %NULL
 */
typedef ShellBatchHold *(*ShellBatchTaskFunc) (gpointer user_data);

#define SHELL_TYPE_BATCH_TASK (shell_batch_task_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellBatchTask, shell_batch_task,
                          SHELL, BATCH_TASK, GObject)

struct _ShellBatchTaskClass
{
  GObjectClass parent_class;

  ShellBatchHold *(* run) (ShellBatchTask *task);
};

ShellBatchTask *shell_batch_task_new (ShellBatchTaskFunc func,
                                      gpointer           user_data,
                                      GDestroyNotify     destroy);

ShellBatchHold *shell_batch_task_run (ShellBatchTask *task);


#define SHELL_TYPE_BATCH_HOLD (shell_batch_hold_get_type ())
G_DECLARE_FINAL_TYPE (ShellBatchHold, shell_batch_hold,
                      SHELL, BATCH_HOLD, ShellBatchTask)

ShellBatchHold *shell_batch_hold_new (void);

void     shell_batch_hold_acquire             (ShellBatchHold *hold);
void     shell_batch_hold_acquire_until_after (ShellBatchHold *hold,
                                               ShellBatchHold *other);
void     shell_batch_hold_release             (ShellBatchHold *hold);
gboolean shell_batch_hold_is_acquired         (ShellBatchHold *hold);


#define SHELL_TYPE_BATCH (shell_batch_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellBatch, shell_batch,
                          SHELL, BATCH, ShellBatchTask)

struct _ShellBatchClass
{
  ShellBatchTaskClass parent_class;

  void (* process) (ShellBatch *batch);
};

ShellBatchHold *shell_batch_run    (ShellBatch *batch);
void            shell_batch_cancel (ShellBatch *batch);


#define SHELL_TYPE_CONSECUTIVE_BATCH (shell_consecutive_batch_get_type ())
G_DECLARE_FINAL_TYPE (ShellConsecutiveBatch, shell_consecutive_batch,
                      SHELL, CONSECUTIVE_BATCH, ShellBatch)

ShellConsecutiveBatch *shell_consecutive_batch_new (ShellBatchTask * const *tasks,
                                                    gsize                   n_tasks);


#define SHELL_TYPE_CONCURRENT_BATCH (shell_concurrent_batch_get_type ())
G_DECLARE_FINAL_TYPE (ShellConcurrentBatch, shell_concurrent_batch,
                      SHELL, CONCURRENT_BATCH, ShellBatch)

ShellConcurrentBatch *shell_concurrent_batch_new (ShellBatchTask * const *tasks,
                                                  gsize                   n_tasks);

G_END_DECLS
