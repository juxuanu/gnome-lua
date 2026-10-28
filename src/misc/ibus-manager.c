/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ibus-manager.c: The connection of the shell to the IBus daemon: spawning
 * it, the list of engines, the panel service and the active engine. Port of
 * js/misc/ibusManager.js.
 *
 * The candidate popup is not owned here anymore (it is still JS): it follows
 * the #ShellIBusManager:panel-service property and closes on
 * #ShellIBusManager::engine-changed, both wired up by the input source
 * manager.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "misc/ibus-manager.h"
#include "shell-global.h"
#include "shell-util.h"

#define IBUS_SYSTEMD_SERVICE "org.freedesktop.IBus.session.GNOME.service"

#define TYPING_BOOSTER_ENGINE "typing-booster"

#define FALLBACK_ENGINE_ID "xkb:us::eng"

/* This is the longest we'll keep the keyboard frozen until an input source
 * is active, in milliseconds. */
#define MAX_INPUT_SOURCE_ACTIVATION_TIME 4000

/* In seconds */
#define PRELOAD_ENGINES_DELAY_TIME 30

enum
{
  PROP_0,

  PROP_PANEL_SERVICE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  READY,
  PROPERTIES_REGISTERED,
  PROPERTY_UPDATED,
  SET_CONTENT_TYPE,
  SET_CURSOR_LOCATION,
  FOCUS_IN,
  FOCUS_OUT,
  ENGINE_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellIBusManager
{
  GObject parent;

  IBusBus *ibus;
  IBusPanelService *panel_service;
  GHashTable *engines;            /* name -> IBusEngineDesc, owned */
  GCancellable *cancellable;      /* of the pending operations of this connection */

  gboolean ready;
  gulong register_properties_id;
  char *current_engine_name;

  guint preload_engines_id;
  char **preload_engines;

  gboolean ibus_is_systemd_service;
  gboolean shutting_down;

  gboolean osk_completion;
  char *pre_osk_engine;
};

G_DEFINE_FINAL_TYPE (ShellIBusManager, shell_ibus_manager, G_TYPE_OBJECT)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (IBusPanelService, g_object_unref)

/* Returns whether the error was logged, i.e. it was not a cancellation */
static gboolean
log_error_unless_cancelled (GError *error)
{
  if (g_dbus_error_is_remote_error (error))
    g_dbus_error_strip_remote_error (error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return FALSE;

  g_warning ("%s", error->message);
  return TRUE;
}

static void
on_child_exited (GPid     pid,
                 int      wait_status,
                 gpointer user_data)
{
  g_spawn_close_pid (pid);
}

static void
spawn_daemon (const char * const *extra_args)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_autoptr (GAppLaunchContext) launch_context = NULL;
  g_auto (GStrv) env = NULL;
  g_autoptr (GError) error = NULL;
  GPid pid;
  int i;

  g_ptr_array_add (argv, (char *) "ibus-daemon");
  g_ptr_array_add (argv, (char *) "--panel");
  g_ptr_array_add (argv, (char *) "disable");
  for (i = 0; extra_args != NULL && extra_args[i] != NULL; i++)
    g_ptr_array_add (argv, (char *) extra_args[i]);
  g_ptr_array_add (argv, NULL);

  launch_context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  env = g_app_launch_context_get_environment (launch_context);

  /* Use DO_NOT_REAP_CHILD to avoid a double-fork internally since
   * ibus-daemon refuses to start with init as its parent. */
  pid = shell_util_spawn_async (NULL,
                                (const char * const *) argv->pdata,
                                (const char * const *) env,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                &error);
  if (pid == 0)
    {
      g_message ("Failed to launch ibus-daemon: %s", error->message);
      return;
    }

  g_child_watch_add (pid, on_child_exited, NULL);
}

typedef struct
{
  ShellIBusManager *self;
  char **args;
} SpawnData;

static void
on_systemd_unit_checked (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  SpawnData *data = user_data;
  g_autoptr (GError) error = NULL;

  data->self->ibus_is_systemd_service =
    shell_util_systemd_unit_exists_finish (res, &error);

  if (!data->self->ibus_is_systemd_service && !data->self->shutting_down)
    spawn_daemon ((const char * const *) data->args);

  g_strfreev (data->args);
  g_object_unref (data->self);
  g_free (data);
}

/*
 * Spawns ibus-daemon with @args unless IBus runs as a systemd service. Takes
 * the ownership of @args.
 */
static void
maybe_spawn_daemon (ShellIBusManager *self,
                    char            **args)
{
  SpawnData *data;

  if (self->ibus_is_systemd_service)
    {
      g_strfreev (args);
      return;
    }

  data = g_new0 (SpawnData, 1);
  data->self = g_object_ref (self);
  data->args = args;

  shell_util_systemd_unit_exists (IBUS_SYSTEMD_SERVICE, NULL,
                                  on_systemd_unit_checked, data);
}

static void
set_panel_service (ShellIBusManager *self,
                   IBusPanelService *panel_service)
{
  if (self->panel_service == panel_service)
    return;

  g_set_object (&self->panel_service, panel_service);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PANEL_SERVICE]);
}

static void
clear (ShellIBusManager *self)
{
  if (self->cancellable != NULL)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  g_clear_handle_id (&self->preload_engines_id, g_source_remove);
  g_clear_pointer (&self->preload_engines, g_strfreev);

  if (self->panel_service != NULL)
    {
      g_autoptr (IBusPanelService) panel_service =
        g_steal_pointer (&self->panel_service);

      ibus_object_destroy (IBUS_OBJECT (panel_service));
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PANEL_SERVICE]);
    }

  g_hash_table_remove_all (self->engines);
  self->ready = FALSE;
  self->register_properties_id = 0;
  g_clear_pointer (&self->current_engine_name, g_free);

  g_signal_emit (self, signals[READY], 0, FALSE);
}

static void
update_readiness (ShellIBusManager *self)
{
  self->ready = g_hash_table_size (self->engines) > 0 &&
                self->panel_service != NULL;

  g_signal_emit (self, signals[READY], 0, self->ready);
}

static void
on_register_properties (IBusPanelService *panel_service,
                        IBusPropList     *prop_list,
                        ShellIBusManager *self)
{
  if (ibus_prop_list_get (prop_list, 0) == NULL)
    return;

  g_signal_handler_disconnect (self->panel_service, self->register_properties_id);
  self->register_properties_id = 0;

  g_signal_emit (self, signals[PROPERTIES_REGISTERED], 0,
                 self->current_engine_name, prop_list);
}

static void
engine_changed (ShellIBusManager *self,
                const char       *engine_name)
{
  if (!self->ready)
    return;

  g_free (self->current_engine_name);
  self->current_engine_name = g_strdup (engine_name);

  g_signal_emit (self, signals[ENGINE_CHANGED], 0, engine_name);

  if (self->register_properties_id != 0)
    return;

  self->register_properties_id =
    g_signal_connect (self->panel_service, "register-properties",
                      G_CALLBACK (on_register_properties), self);
}

static void
on_global_engine_changed (IBusBus          *bus,
                          const char       *engine_name,
                          ShellIBusManager *self)
{
  engine_changed (self, engine_name);
}

static void
on_update_property (IBusPanelService *panel_service,
                    IBusProperty     *prop,
                    ShellIBusManager *self)
{
  g_signal_emit (self, signals[PROPERTY_UPDATED], 0,
                 self->current_engine_name, prop);
}

static void
on_set_cursor_location (IBusPanelService *panel_service,
                        int               x,
                        int               y,
                        int               width,
                        int               height,
                        ShellIBusManager *self)
{
  g_signal_emit (self, signals[SET_CURSOR_LOCATION], 0, x, y, width, height);
}

static void
on_focus_in (IBusPanelService *panel_service,
             const char       *path,
             ShellIBusManager *self)
{
  if (!g_str_has_suffix (path, "/InputContext_1"))
    g_signal_emit (self, signals[FOCUS_IN], 0);
}

static void
on_focus_out (IBusPanelService *panel_service,
              const char       *path,
              ShellIBusManager *self)
{
  g_signal_emit (self, signals[FOCUS_OUT], 0);
}

#if IBUS_CHECK_VERSION (1, 5, 10)
static void
on_set_content_type (IBusPanelService *panel_service,
                     unsigned int      purpose,
                     unsigned int      hints,
                     ShellIBusManager *self)
{
  g_signal_emit (self, signals[SET_CONTENT_TYPE], 0, purpose, hints);
}
#endif

static void
on_list_engines_done (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (ShellIBusManager) self = user_data;
  g_autoptr (GError) error = NULL;
  GList *engines, *l;

  engines = ibus_bus_list_engines_async_finish (IBUS_BUS (source), res, &error);
  if (error != NULL)
    {
      if (log_error_unless_cancelled (error) && !self->shutting_down)
        clear (self);
      return;
    }

  if (self->shutting_down)
    {
      g_list_free_full (engines, g_object_unref);
      return;
    }

  for (l = engines; l != NULL; l = l->next)
    {
      IBusEngineDesc *desc = l->data;

      g_hash_table_replace (self->engines,
                            g_strdup (ibus_engine_desc_get_name (desc)),
                            g_object_ref (desc));
    }
  g_list_free_full (engines, g_object_unref);

  update_readiness (self);
}

static void
on_get_global_engine_done (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (ShellIBusManager) self = user_data;
  g_autoptr (GError) error = NULL;
  IBusEngineDesc *engine;

  /* There is no engine, or the call failed: nothing to do */
  engine = ibus_bus_get_global_engine_async_finish (IBUS_BUS (source), res, &error);
  if (engine == NULL || self->shutting_down)
    return;

  engine_changed (self, ibus_engine_desc_get_name (engine));
}

static void
on_request_name_done (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (ShellIBusManager) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (IBusPanelService) panel_service = NULL;
  GCancellable *cancellable;

  ibus_bus_request_name_async_finish (IBUS_BUS (source), res, &error);
  if (error != NULL)
    {
      if (log_error_unless_cancelled (error) && !self->shutting_down)
        clear (self);
      return;
    }

  if (self->shutting_down)
    return;

  panel_service = ibus_panel_service_new (ibus_bus_get_connection (self->ibus));
  if (g_object_is_floating (panel_service))
    g_object_ref_sink (panel_service);
  set_panel_service (self, panel_service);

  g_signal_connect_object (panel_service, "update-property",
                           G_CALLBACK (on_update_property), self, 0);
  g_signal_connect_object (panel_service, "set-cursor-location",
                           G_CALLBACK (on_set_cursor_location), self, 0);
  g_signal_connect_object (panel_service, "focus-in",
                           G_CALLBACK (on_focus_in), self, 0);
  g_signal_connect_object (panel_service, "focus-out",
                           G_CALLBACK (on_focus_out), self, 0);

#if IBUS_CHECK_VERSION (1, 5, 10)
  /* IBus versions older than 1.5.10 have a bug which causes spurious
   * set-content-type emissions when switching input focus that temporarily
   * lose purpose and hints defeating its intended semantics and confusing
   * users. We thus don't use it in that case. */
  g_signal_connect_object (panel_service, "set-content-type",
                           G_CALLBACK (on_set_content_type), self, 0);
#endif

  update_readiness (self);

  /* If an engine is already active we need to get its properties */
  cancellable = self->cancellable;
  ibus_bus_get_global_engine_async (self->ibus, -1, cancellable,
                                    on_get_global_engine_done,
                                    g_object_ref (self));
}

static void
on_connected (IBusBus          *bus,
              ShellIBusManager *self)
{
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  ibus_bus_list_engines_async (self->ibus, -1, self->cancellable,
                               on_list_engines_done, g_object_ref (self));
  ibus_bus_request_name_async (self->ibus, IBUS_SERVICE_PANEL,
                               IBUS_BUS_NAME_FLAG_REPLACE_EXISTING, -1,
                               self->cancellable,
                               on_request_name_done, g_object_ref (self));
}

static void
on_disconnected (IBusBus          *bus,
                 ShellIBusManager *self)
{
  clear (self);
}

typedef struct
{
  GCancellable *cancellable;
  gboolean fallback;
} SetEngineData;

static void
set_engine_data_free (SetEngineData *data)
{
  g_clear_object (&data->cancellable);
  g_free (data);
}

static void
on_set_global_engine_done (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  GTask *task = user_data;
  ShellIBusManager *self = g_task_get_source_object (task);
  SetEngineData *data = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;

  ibus_bus_set_global_engine_async_finish (IBUS_BUS (source), res, &error);
  if (error != NULL)
    {
      if (!data->fallback)
        {
          /* A cancellation means the connection went away: give up */
          if (log_error_unless_cancelled (error))
            {
              data->fallback = TRUE;
              ibus_bus_set_global_engine_async (self->ibus,
                                                FALLBACK_ENGINE_ID,
                                                MAX_INPUT_SOURCE_ACTIVATION_TIME,
                                                data->cancellable,
                                                on_set_global_engine_done,
                                                task);
              return;
            }
        }
      else
        {
          log_error_unless_cancelled (error);
        }
    }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/*
 * Sets the global engine, falling back to the US layout if that fails. Always
 * succeeds from the caller's point of view: failures are logged.
 */
static void
set_engine_internal_async (ShellIBusManager    *self,
                           const char          *id,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  GTask *task = g_task_new (self, NULL, callback, user_data);
  SetEngineData *data = g_new0 (SetEngineData, 1);

  g_task_set_source_tag (task, set_engine_internal_async);
  g_task_set_check_cancellable (task, FALSE);

  g_set_object (&data->cancellable, self->cancellable);
  g_task_set_task_data (task, data, (GDestroyNotify) set_engine_data_free);

  /* Send id even if id == current_engine_name because 'properties-registered'
   * signal can be emitted while the input sources are not known yet on a
   * lock screen. */
  if (!self->ready)
    {
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }

  ibus_bus_set_global_engine_async (self->ibus, id,
                                    MAX_INPUT_SOURCE_ACTIVATION_TIME,
                                    data->cancellable,
                                    on_set_global_engine_done,
                                    task);
}

typedef struct
{
  gboolean enabled;
} CompletionData;

static void
on_completion_engine_set (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  GTask *task = user_data;
  ShellIBusManager *self = g_task_get_source_object (task);
  CompletionData *data = g_task_get_task_data (task);

  /* Nothing may complete into JS while the shell is going away */
  if (self->shutting_down)
    {
      g_object_unref (task);
      return;
    }

  if (!data->enabled)
    g_clear_pointer (&self->pre_osk_engine, g_free);

  g_task_return_boolean (task, self->osk_completion);
  g_object_unref (task);
}

/**
 * shell_ibus_manager_set_completion_enabled_async:
 * @self: a #ShellIBusManager
 * @enabled: whether completion should be enabled
 * @cancellable: (nullable): unused
 * @callback: (scope async): called when the engine was switched
 * @user_data: (closure): data for @callback
 *
 * Enables or disables the word completion of the on-screen keyboard, which
 * is done by the typing-booster engine. It needs the engine to be available
 * and the current engine to be an XKB one; disabling brings the previous
 * engine back.
 */
void
shell_ibus_manager_set_completion_enabled_async (ShellIBusManager    *self,
                                                 gboolean             enabled,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  CompletionData *data;

  g_return_if_fail (SHELL_IS_IBUS_MANAGER (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_ibus_manager_set_completion_enabled_async);

  data = g_new0 (CompletionData, 1);
  data->enabled = enabled;
  g_task_set_task_data (task, data, g_free);

  /* Needs typing-booster available */
  if (enabled && !g_hash_table_contains (self->engines, TYPING_BOOSTER_ENGINE))
    {
      g_task_return_boolean (task, FALSE);
      return;
    }

  /* Can do only on xkb engines */
  if (enabled &&
      (self->current_engine_name == NULL ||
       !g_str_has_prefix (self->current_engine_name, "xkb:")))
    {
      g_task_return_boolean (task, FALSE);
      return;
    }

  if (self->osk_completion == enabled)
    {
      g_task_return_boolean (task, enabled);
      return;
    }

  self->osk_completion = enabled;

  if (enabled)
    {
      g_free (self->pre_osk_engine);
      self->pre_osk_engine = g_strdup (self->current_engine_name);
      set_engine_internal_async (self, TYPING_BOOSTER_ENGINE,
                                 on_completion_engine_set,
                                 g_steal_pointer (&task));
    }
  else if (self->pre_osk_engine != NULL)
    {
      g_autofree char *engine = g_strdup (self->pre_osk_engine);

      set_engine_internal_async (self, engine,
                                 on_completion_engine_set,
                                 g_steal_pointer (&task));
    }
  else
    {
      g_task_return_boolean (task, self->osk_completion);
    }
}

/**
 * shell_ibus_manager_set_completion_enabled_finish:
 * @self: a #ShellIBusManager
 * @res: the #GAsyncResult passed to the callback
 *
 * Returns: whether the completion is enabled
 */
gboolean
shell_ibus_manager_set_completion_enabled_finish (ShellIBusManager *self,
                                                  GAsyncResult     *res)
{
  g_return_val_if_fail (SHELL_IS_IBUS_MANAGER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);

  return g_task_propagate_boolean (G_TASK (res), NULL);
}

/**
 * shell_ibus_manager_get_default:
 *
 * Gets the #ShellIBusManager singleton. It connects to the IBus daemon,
 * starting it first unless IBus runs as a systemd service.
 *
 * Returns: (transfer none): the #ShellIBusManager
 */
ShellIBusManager *
shell_ibus_manager_get_default (void)
{
  static ShellIBusManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_IBUS_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_ibus_manager_get_panel_service:
 * @self: a #ShellIBusManager
 *
 * Gets the panel service, which exists while connected to the daemon and
 * after the shell got the panel name.
 *
 * Returns: (transfer none) (nullable): the #IBusPanelService
 */
IBusPanelService *
shell_ibus_manager_get_panel_service (ShellIBusManager *self)
{
  g_return_val_if_fail (SHELL_IS_IBUS_MANAGER (self), NULL);

  return self->panel_service;
}

/**
 * shell_ibus_manager_get_ready:
 * @self: a #ShellIBusManager
 *
 * Returns: whether the engines are listed and the panel service exists, i.e.
 *   the state last reported by #ShellIBusManager::ready
 */
gboolean
shell_ibus_manager_get_ready (ShellIBusManager *self)
{
  g_return_val_if_fail (SHELL_IS_IBUS_MANAGER (self), FALSE);

  return self->ready;
}

/**
 * shell_ibus_manager_restart_daemon:
 * @self: a #ShellIBusManager
 * @extra_args: (array zero-terminated=1) (element-type utf8) (nullable): more
 *   arguments for `ibus-daemon`, e.g. `--xim`
 *
 * Restarts the IBus daemon with `-r`, unless IBus runs as a systemd service.
 */
void
shell_ibus_manager_restart_daemon (ShellIBusManager  *self,
                                   const char * const *extra_args)
{
  g_autoptr (GPtrArray) args = NULL;
  int i;

  g_return_if_fail (SHELL_IS_IBUS_MANAGER (self));

  args = g_ptr_array_new ();
  g_ptr_array_add (args, g_strdup ("-r"));
  for (i = 0; extra_args != NULL && extra_args[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup (extra_args[i]));
  g_ptr_array_add (args, NULL);

  maybe_spawn_daemon (self, (char **) g_ptr_array_free (g_steal_pointer (&args), FALSE));
}

/**
 * shell_ibus_manager_activate_property:
 * @self: a #ShellIBusManager
 * @key: the key of the #IBusProperty
 * @state: the new #IBusPropState of the property
 *
 * Tells the engine that the user activated a property of its menu. Does
 * nothing if there is no panel service.
 */
void
shell_ibus_manager_activate_property (ShellIBusManager *self,
                                      const char       *key,
                                      unsigned int      state)
{
  g_return_if_fail (SHELL_IS_IBUS_MANAGER (self));
  g_return_if_fail (key != NULL);

  if (self->panel_service == NULL)
    return;

  ibus_panel_service_property_activate (self->panel_service, key, state);
}

/**
 * shell_ibus_manager_get_engine_desc:
 * @self: a #ShellIBusManager
 * @id: the engine name, e.g. `libpinyin`
 *
 * Gets the description of an engine.
 *
 * Returns: (transfer none) (nullable): the #IBusEngineDesc, or %NULL if IBus
 *   is not ready or has no such engine
 */
IBusEngineDesc *
shell_ibus_manager_get_engine_desc (ShellIBusManager *self,
                                    const char       *id)
{
  g_return_val_if_fail (SHELL_IS_IBUS_MANAGER (self), NULL);
  g_return_val_if_fail (id != NULL, NULL);

  if (!self->ready)
    return NULL;

  return g_hash_table_lookup (self->engines, id);
}

/**
 * shell_ibus_manager_set_engine:
 * @self: a #ShellIBusManager
 * @id: the engine name
 *
 * Makes @id the global engine, falling back to the US layout if that fails.
 * If the completion of the on-screen keyboard is enabled, @id is only
 * remembered as the engine to restore, and the completion is disabled if @id
 * is not an XKB engine since the two conflict.
 *
 * The operation is not waited for.
 */
void
shell_ibus_manager_set_engine (ShellIBusManager *self,
                               const char       *id)
{
  g_return_if_fail (SHELL_IS_IBUS_MANAGER (self));
  g_return_if_fail (id != NULL);

  if (self->osk_completion)
    {
      g_free (self->pre_osk_engine);
      self->pre_osk_engine = g_strdup (id);

      /* Non xkb engines conflict with completion */
      if (!g_str_has_prefix (id, "xkb:"))
        shell_ibus_manager_set_completion_enabled_async (self, FALSE, NULL, NULL, NULL);
    }
  else
    {
      set_engine_internal_async (self, id, NULL, NULL);
    }
}

static void
preload_engines_timeout (gpointer user_data)
{
  ShellIBusManager *self = user_data;

  self->preload_engines_id = 0;

  ibus_bus_preload_engines_async (self->ibus,
                                  (const char * const *) self->preload_engines,
                                  -1,
                                  self->cancellable,
                                  NULL, NULL);
}

/**
 * shell_ibus_manager_preload_engines:
 * @self: a #ShellIBusManager
 * @ids: (array zero-terminated=1) (element-type utf8): the engine names
 *
 * Asks the daemon to start the engines in 30 seconds, along with the typing
 * booster used by the completion of the on-screen keyboard. A pending call is
 * replaced. Does nothing if IBus is not ready.
 */
void
shell_ibus_manager_preload_engines (ShellIBusManager  *self,
                                    const char * const *ids)
{
  g_autoptr (GPtrArray) names = NULL;
  gboolean has_typing_booster = FALSE;
  int i;

  g_return_if_fail (SHELL_IS_IBUS_MANAGER (self));

  if (self->ibus == NULL || !self->ready)
    return;

  names = g_ptr_array_new ();
  for (i = 0; ids != NULL && ids[i] != NULL; i++)
    {
      if (g_strcmp0 (ids[i], TYPING_BOOSTER_ENGINE) == 0)
        has_typing_booster = TRUE;
      g_ptr_array_add (names, g_strdup (ids[i]));
    }

  if (!has_typing_booster)
    g_ptr_array_add (names, g_strdup (TYPING_BOOSTER_ENGINE));
  g_ptr_array_add (names, NULL);

  g_clear_handle_id (&self->preload_engines_id, g_source_remove);

  g_strfreev (self->preload_engines);
  self->preload_engines = (char **) g_ptr_array_free (g_steal_pointer (&names), FALSE);

  self->preload_engines_id =
    g_timeout_add_seconds_once (PRELOAD_ENGINES_DELAY_TIME,
                                preload_engines_timeout, self);
}

static void
shell_ibus_manager_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellIBusManager *self = SHELL_IBUS_MANAGER (object);

  switch (prop_id)
    {
    case PROP_PANEL_SERVICE:
      g_value_set_object (value, self->panel_service);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_shutdown (ShellGlobal      *global,
             ShellIBusManager *self)
{
  /* The pending calls are cancelled and the handlers that would emit into
   * JS are disconnected; the daemon keeps running. */
  self->shutting_down = TRUE;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  g_clear_handle_id (&self->preload_engines_id, g_source_remove);

  if (self->ibus != NULL)
    g_signal_handlers_disconnect_by_data (self->ibus, self);
  if (self->panel_service != NULL)
    g_signal_handlers_disconnect_by_data (self->panel_service, self);
  self->register_properties_id = 0;
}

static void
shell_ibus_manager_constructed (GObject *object)
{
  ShellIBusManager *self = SHELL_IBUS_MANAGER (object);

  G_OBJECT_CLASS (shell_ibus_manager_parent_class)->constructed (object);

  ibus_init ();

  self->ibus = ibus_bus_new_async ();
  g_signal_connect_object (self->ibus, "connected",
                           G_CALLBACK (on_connected), self, 0);
  g_signal_connect_object (self->ibus, "disconnected",
                           G_CALLBACK (on_disconnected), self, 0);

  /* Need to set this to get 'global-engine-changed' emissions */
  ibus_bus_set_watch_ibus_signal (self->ibus, TRUE);
  g_signal_connect_object (self->ibus, "global-engine-changed",
                           G_CALLBACK (on_global_engine_changed), self, 0);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  maybe_spawn_daemon (self, g_new0 (char *, 1));
}

static void
shell_ibus_manager_dispose (GObject *object)
{
  ShellIBusManager *self = SHELL_IBUS_MANAGER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_handle_id (&self->preload_engines_id, g_source_remove);

  if (self->panel_service != NULL)
    ibus_object_destroy (IBUS_OBJECT (self->panel_service));
  g_clear_object (&self->panel_service);

  g_clear_pointer (&self->engines, g_hash_table_unref);
  g_clear_object (&self->ibus);

  G_OBJECT_CLASS (shell_ibus_manager_parent_class)->dispose (object);
}

static void
shell_ibus_manager_finalize (GObject *object)
{
  ShellIBusManager *self = SHELL_IBUS_MANAGER (object);

  g_free (self->current_engine_name);
  g_free (self->pre_osk_engine);
  g_strfreev (self->preload_engines);

  G_OBJECT_CLASS (shell_ibus_manager_parent_class)->finalize (object);
}

static void
shell_ibus_manager_class_init (ShellIBusManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_ibus_manager_constructed;
  object_class->get_property = shell_ibus_manager_get_property;
  object_class->dispose = shell_ibus_manager_dispose;
  object_class->finalize = shell_ibus_manager_finalize;

  /**
   * ShellIBusManager:panel-service:
   *
   * The #IBusPanelService, or %NULL while not connected to the daemon. The
   * candidate popup talks to it.
   */
  props[PROP_PANEL_SERVICE] =
    g_param_spec_object ("panel-service", NULL, NULL,
                         IBUS_TYPE_PANEL_SERVICE,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellIBusManager::ready:
   * @self: the #ShellIBusManager
   * @ready: whether the engines are listed and the panel service exists
   *
   * Emitted when the readiness changed, and also when it was re-evaluated to
   * the same value.
   */
  signals[READY] =
    g_signal_new ("ready",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * ShellIBusManager::properties-registered:
   * @self: the #ShellIBusManager
   * @engine_name: the engine that registered the properties
   * @props: the #IBusPropList, which is not empty
   *
   * Emitted when the current engine registers the properties of its menu.
   */
  signals[PROPERTIES_REGISTERED] =
    g_signal_new ("properties-registered",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,
                  IBUS_TYPE_PROP_LIST);

  /**
   * ShellIBusManager::property-updated:
   * @self: the #ShellIBusManager
   * @engine_name: (nullable): the current engine
   * @prop: the updated #IBusProperty
   */
  signals[PROPERTY_UPDATED] =
    g_signal_new ("property-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,
                  IBUS_TYPE_PROPERTY);

  /**
   * ShellIBusManager::set-content-type:
   * @self: the #ShellIBusManager
   * @purpose: the #IBusInputPurpose
   * @hints: the #IBusInputHints
   */
  signals[SET_CONTENT_TYPE] =
    g_signal_new ("set-content-type",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_UINT,
                  G_TYPE_UINT);

  /**
   * ShellIBusManager::set-cursor-location:
   * @self: the #ShellIBusManager
   * @x: the x coordinate of the cursor rectangle
   * @y: the y coordinate of the cursor rectangle
   * @width: the width of the cursor rectangle
   * @height: the height of the cursor rectangle
   *
   * Emitted when an X11 client moves its text cursor. The coordinates were an
   * `{x, y, width, height}` object in the JS port.
   */
  signals[SET_CURSOR_LOCATION] =
    g_signal_new ("set-cursor-location",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);

  /**
   * ShellIBusManager::focus-in:
   * @self: the #ShellIBusManager
   *
   * Emitted when an input context other than the one of the shell gets the
   * focus.
   */
  signals[FOCUS_IN] =
    g_signal_new ("focus-in",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellIBusManager::focus-out:
   * @self: the #ShellIBusManager
   */
  signals[FOCUS_OUT] =
    g_signal_new ("focus-out",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellIBusManager::engine-changed:
   * @self: the #ShellIBusManager
   * @engine_name: the new global engine
   *
   * Emitted when the global engine changed while the manager is ready. The
   * candidate popup closes on it.
   */
  signals[ENGINE_CHANGED] =
    g_signal_new ("engine-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_ibus_manager_init (ShellIBusManager *self)
{
  self->engines = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, g_object_unref);
}
