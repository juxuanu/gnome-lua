/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dbus-utils.c: Helpers for the D-Bus interfaces of the shell. Port of
 * js/misc/dbusUtils.js, plus the DBusSenderChecker of js/misc/util.js.
 *
 * The interface XML files are read from the gnome-shell-dbus-interfaces
 * gresource, which is registered the first time one is needed (the JS did the
 * same, so that the tools can use it without the shell: GNOME_SHELL_DATADIR
 * overrides the installation directory).
 *
 * js/misc/dbusUtils.js is not deleted yet: the out-of-process helpers
 * (js/portalHelper and the js/dbusServices processes) run in plain gjs,
 * without libshell, and still bundle it. They go away in M5.
 *
 * Differences with the JS version:
 *
 *  - loading an interface that is not in the gresource logs a message and
 *    returns %NULL, as before; a missing gresource file is a warning (the JS
 *    threw while importing the module);
 *  - shell_dbus_load_sub_interface_xml () returns %NULL if @iface is not in
 *    the file (the JS cut the XML from its start to the first `</interface>`);
 *  - #ShellDBusSenderChecker is a GObject with a GTask based
 *    shell_dbus_sender_checker_check_invocation_async (); the checks that wait
 *    for the owners of the allowed names fail once ShellGlobal::shutdown has
 *    been emitted, and the JS DBusSenderChecker.destroy () is the disposal of
 *    the object. The JS DBusSenderChecker passed `Gio.DBus.unwatch_name ()`
 *    the array indexes instead of the watch ids; this one unwatches the names.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <meta/meta-context.h>

#include "misc/dbus-utils.h"
#include "shell-global.h"

#define IFACE_RESOURCE_FILE "gnome-shell-dbus-interfaces.gresource"
#define IFACE_RESOURCE_PREFIX "/org/gnome/shell/dbus-interfaces/"

static gboolean
ensure_iface_resource (void)
{
  static gsize state = 0;

  if (g_once_init_enter (&state))
    {
      /* Don't use the ShellGlobal datadir so that the function is usable from
       * tests and tools */
      const char *dir = g_getenv ("GNOME_SHELL_DATADIR");
      g_autofree char *path = NULL;
      g_autoptr (GResource) resource = NULL;
      g_autoptr (GError) error = NULL;
      gsize result = 2;

      if (dir == NULL || *dir == '\0')
        dir = GNOME_SHELL_DATADIR;

      path = g_build_filename (dir, IFACE_RESOURCE_FILE, NULL);
      resource = g_resource_load (path, &error);
      if (resource != NULL)
        {
          g_resources_register (resource);
          result = 1;
        }
      else
        {
          g_warning ("Failed to load %s: %s", path, error->message);
        }

      g_once_init_leave (&state, result);
    }

  return state == 1;
}

/**
 * shell_dbus_load_interface_xml:
 * @iface: the interface name
 *
 * Reads `/org/gnome/shell/dbus-interfaces/@iface.xml` from the D-Bus
 * interfaces gresource.
 *
 * Returns: (transfer full) (nullable): the XML, or %NULL if it is not found
 */
char *
shell_dbus_load_interface_xml (const char *iface)
{
  g_autofree char *path = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;
  const char *data;
  gsize size;

  g_return_val_if_fail (iface != NULL, NULL);

  if (!ensure_iface_resource ())
    return NULL;

  path = g_strconcat (IFACE_RESOURCE_PREFIX, iface, ".xml", NULL);
  bytes = g_resources_lookup_data (path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  if (bytes == NULL)
    {
      g_message ("Failed to load D-Bus interface %s", iface);
      return NULL;
    }

  data = g_bytes_get_data (bytes, &size);
  return g_strndup (data, size);
}

/**
 * shell_dbus_load_sub_interface_xml:
 * @iface: the interface name
 * @iface_file: the name of the interface file to read it from
 *
 * Reads the file for @iface_file and cuts the definition of the interface
 * @iface out of it, wrapped in a `<node>`.
 *
 * Returns: (transfer full) (nullable): the XML, or %NULL if it is not found
 */
char *
shell_dbus_load_sub_interface_xml (const char *iface,
                                   const char *iface_file)
{
  static const char xml_header[] =
    "<!DOCTYPE node PUBLIC\n"
    "'-//freedesktop//DTD D-BUS Object Introspection 1.0//EN'\n"
    "'http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd'>\n"
    "<node>\n";
  static const char xml_footer[] = "</node>";
  static const char iface_stop_tag[] = "</interface>";
  g_autofree char *xml = NULL;
  g_autofree char *iface_start_tag = NULL;
  g_autofree char *iface_xml = NULL;
  const char *start, *end;

  g_return_val_if_fail (iface != NULL, NULL);
  g_return_val_if_fail (iface_file != NULL, NULL);

  xml = shell_dbus_load_interface_xml (iface_file);
  if (xml == NULL)
    return NULL;

  iface_start_tag = g_strdup_printf ("<interface name=\"%s\">", iface);
  start = strstr (xml, iface_start_tag);
  if (start == NULL)
    {
      g_message ("Failed to find D-Bus interface %s in %s", iface, iface_file);
      return NULL;
    }

  end = strstr (start + 1, iface_stop_tag);
  if (end == NULL)
    {
      g_message ("Failed to find the end of D-Bus interface %s in %s", iface, iface_file);
      return NULL;
    }
  end += strlen (iface_stop_tag);

  iface_xml = g_strndup (start, end - start);
  return g_strconcat (xml_header, iface_xml, xml_footer, NULL);
}

/**
 * shell_dbus_get_interface_info:
 * @iface: the interface name, which is also the name of the interface file
 *
 * Parses the XML of shell_dbus_load_interface_xml () and returns the
 * definition of @iface (or the first interface of the file, if it has none
 * of that name): `Gio.DBusInterfaceInfo.new_for_xml (loadInterfaceXML (iface))`.
 *
 * Returns: (transfer full) (nullable): the #GDBusInterfaceInfo, or %NULL if the
 *   file is not found or cannot be parsed
 */
GDBusInterfaceInfo *
shell_dbus_get_interface_info (const char *iface)
{
  g_autofree char *xml = NULL;
  g_autoptr (GDBusNodeInfo) node = NULL;
  g_autoptr (GError) error = NULL;
  GDBusInterfaceInfo *info;

  g_return_val_if_fail (iface != NULL, NULL);

  xml = shell_dbus_load_interface_xml (iface);
  if (xml == NULL)
    return NULL;

  node = g_dbus_node_info_new_for_xml (xml, &error);
  if (node == NULL)
    {
      g_warning ("Failed to parse D-Bus interface %s: %s", iface, error->message);
      return NULL;
    }

  info = g_dbus_node_info_lookup_interface (node, iface);
  if (info == NULL && node->interfaces != NULL)
    info = node->interfaces[0];
  if (info == NULL)
    return NULL;

  return g_dbus_interface_info_ref (info);
}

/**
 * shell_dbus_emit_signal_to_destination:
 * @skeleton: the exported D-Bus object
 * @destination: (nullable): the unique or well-known bus name to send the
 *   signal to, or %NULL to broadcast it
 * @signal_name: the name of the signal
 * @params: (nullable): the parameters of the signal
 *
 * Emits a D-Bus signal of the interface and object path of @skeleton to a
 * single bus name.
 */
void
shell_dbus_emit_signal_to_destination (GDBusInterfaceSkeleton *skeleton,
                                       const char             *destination,
                                       const char             *signal_name,
                                       GVariant               *params)
{
  GDBusConnection *connection;
  GDBusInterfaceInfo *info;
  g_autoptr (GError) error = NULL;

  g_return_if_fail (G_IS_DBUS_INTERFACE_SKELETON (skeleton));
  g_return_if_fail (signal_name != NULL);

  connection = g_dbus_interface_skeleton_get_connection (skeleton);
  if (connection == NULL)
    {
      if (params != NULL && g_variant_is_floating (params))
        g_variant_unref (g_variant_ref_sink (params));
      return;
    }

  info = g_dbus_interface_skeleton_get_info (skeleton);
  if (!g_dbus_connection_emit_signal (connection,
                                      destination,
                                      g_dbus_interface_skeleton_get_object_path (skeleton),
                                      info != NULL ? info->name : NULL,
                                      signal_name,
                                      params,
                                      &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

/* ------------------------------------------------------------------------
 * ShellDBusSenderChecker
 * ------------------------------------------------------------------------ */

enum
{
  CHECKER_PROP_0,

  CHECKER_PROP_ALLOW_LIST,

  CHECKER_N_PROPS
};

static GParamSpec *checker_props[CHECKER_N_PROPS] = { NULL, };

struct _ShellDBusSenderChecker
{
  GObject parent;

  char **allow_list;
  GHashTable *allowlist_map;        /* well-known name -> owner */
  GHashTable *uninitialized_names;  /* set of well-known names */
  GArray *watch_ids;
  GQueue pending_checks;            /* of GTask */
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellDBusSenderChecker, shell_dbus_sender_checker, G_TYPE_OBJECT)

static gboolean
is_unsafe_mode (void)
{
  gboolean unsafe_mode = FALSE;

  g_object_get (shell_global_get_context (shell_global_get ()),
                "unsafe-mode", &unsafe_mode, NULL);
  return unsafe_mode;
}

static gboolean
is_sender_allowed (ShellDBusSenderChecker *self,
                   const char             *sender)
{
  GHashTableIter iter;
  gpointer owner;

  g_hash_table_iter_init (&iter, self->allowlist_map);
  while (g_hash_table_iter_next (&iter, NULL, &owner))
    {
      if (g_strcmp0 (owner, sender) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
resolve_check (ShellDBusSenderChecker *self,
               GTask                  *task)
{
  GDBusMethodInvocation *invocation = g_task_get_task_data (task);

  if (self->shutting_down)
    g_task_return_new_error_literal (task, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                     "The shell is shutting down");
  else if (is_sender_allowed (self, g_dbus_method_invocation_get_sender (invocation)))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_new_error (task, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                             "%s is not allowed",
                             g_dbus_method_invocation_get_method_name (invocation));
}

static void
flush_pending_checks (ShellDBusSenderChecker *self)
{
  GTask *task;

  while ((task = g_queue_pop_head (&self->pending_checks)) != NULL)
    {
      resolve_check (self, task);
      g_object_unref (task);
    }
}

static void
check_and_resolve_initialized (ShellDBusSenderChecker *self,
                               const char             *name)
{
  if (g_hash_table_remove (self->uninitialized_names, name) &&
      g_hash_table_size (self->uninitialized_names) == 0)
    flush_pending_checks (self);
}

static void
on_allowlist_name_appeared (GDBusConnection *connection,
                            const char      *name,
                            const char      *name_owner,
                            gpointer         user_data)
{
  ShellDBusSenderChecker *self = user_data;

  g_hash_table_replace (self->allowlist_map, g_strdup (name), g_strdup (name_owner));
  check_and_resolve_initialized (self, name);
}

static void
on_allowlist_name_vanished (GDBusConnection *connection,
                            const char      *name,
                            gpointer         user_data)
{
  ShellDBusSenderChecker *self = user_data;

  g_hash_table_remove (self->allowlist_map, name);
  check_and_resolve_initialized (self, name);
}

static void
on_shutdown (ShellGlobal            *global,
             ShellDBusSenderChecker *self)
{
  self->shutting_down = TRUE;
  flush_pending_checks (self);
}

/**
 * shell_dbus_sender_checker_check_invocation_async:
 * @self: a #ShellDBusSenderChecker
 * @invocation: the method invocation to check
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the check is done
 * @user_data: user data for @callback
 *
 * Checks whether the bus name that invoked @invocation is the owner of one
 * of the allowed names. The check waits until the owners of all of them are
 * known; it always passes while the shell is in unsafe mode.
 */
void
shell_dbus_sender_checker_check_invocation_async (ShellDBusSenderChecker *self,
                                                  GDBusMethodInvocation  *invocation,
                                                  GCancellable           *cancellable,
                                                  GAsyncReadyCallback     callback,
                                                  gpointer                user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_DBUS_SENDER_CHECKER (self));
  g_return_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_dbus_sender_checker_check_invocation_async);
  g_task_set_task_data (task, g_object_ref (invocation), g_object_unref);

  if (is_unsafe_mode () && !self->shutting_down)
    g_task_return_boolean (task, TRUE);
  else if (!self->shutting_down && g_hash_table_size (self->uninitialized_names) > 0)
    g_queue_push_tail (&self->pending_checks, g_steal_pointer (&task));
  else
    resolve_check (self, task);
}

/**
 * shell_dbus_sender_checker_check_invocation_finish:
 * @self: a #ShellDBusSenderChecker
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Returns: %TRUE if the sender is allowed, %FALSE with a
 *   %G_DBUS_ERROR_ACCESS_DENIED @error (to return with
 *   g_dbus_method_invocation_return_gerror ()) if it is not
 */
gboolean
shell_dbus_sender_checker_check_invocation_finish (ShellDBusSenderChecker *self,
                                                   GAsyncResult           *result,
                                                   GError                **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
shell_dbus_sender_checker_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellDBusSenderChecker *self = SHELL_DBUS_SENDER_CHECKER (object);

  switch (prop_id)
    {
    case CHECKER_PROP_ALLOW_LIST:
      g_strfreev (self->allow_list);
      self->allow_list = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_sender_checker_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellDBusSenderChecker *self = SHELL_DBUS_SENDER_CHECKER (object);

  switch (prop_id)
    {
    case CHECKER_PROP_ALLOW_LIST:
      g_value_set_boxed (value, self->allow_list);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_sender_checker_constructed (GObject *object)
{
  ShellDBusSenderChecker *self = SHELL_DBUS_SENDER_CHECKER (object);
  guint i;

  G_OBJECT_CLASS (shell_dbus_sender_checker_parent_class)->constructed (object);

  if (self->allow_list == NULL)
    self->allow_list = g_new0 (char *, 1);

  for (i = 0; self->allow_list[i] != NULL; i++)
    g_hash_table_add (self->uninitialized_names, g_strdup (self->allow_list[i]));

  for (i = 0; self->allow_list[i] != NULL; i++)
    {
      guint watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION, self->allow_list[i],
                                         G_BUS_NAME_WATCHER_FLAGS_NONE,
                                         on_allowlist_name_appeared,
                                         on_allowlist_name_vanished,
                                         self, NULL);

      g_array_append_val (self->watch_ids, watch_id);
    }

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);
}

static void
shell_dbus_sender_checker_dispose (GObject *object)
{
  ShellDBusSenderChecker *self = SHELL_DBUS_SENDER_CHECKER (object);
  guint i;

  self->shutting_down = TRUE;
  flush_pending_checks (self);

  for (i = 0; i < self->watch_ids->len; i++)
    g_bus_unwatch_name (g_array_index (self->watch_ids, guint, i));
  g_array_set_size (self->watch_ids, 0);

  G_OBJECT_CLASS (shell_dbus_sender_checker_parent_class)->dispose (object);
}

static void
shell_dbus_sender_checker_finalize (GObject *object)
{
  ShellDBusSenderChecker *self = SHELL_DBUS_SENDER_CHECKER (object);

  g_strfreev (self->allow_list);
  g_clear_pointer (&self->allowlist_map, g_hash_table_unref);
  g_clear_pointer (&self->uninitialized_names, g_hash_table_unref);
  g_clear_pointer (&self->watch_ids, g_array_unref);

  G_OBJECT_CLASS (shell_dbus_sender_checker_parent_class)->finalize (object);
}

static void
shell_dbus_sender_checker_class_init (ShellDBusSenderCheckerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_dbus_sender_checker_set_property;
  object_class->get_property = shell_dbus_sender_checker_get_property;
  object_class->constructed = shell_dbus_sender_checker_constructed;
  object_class->dispose = shell_dbus_sender_checker_dispose;
  object_class->finalize = shell_dbus_sender_checker_finalize;

  /**
   * ShellDBusSenderChecker:allow-list:
   *
   * The well-known names whose owners may call the checked methods.
   */
  checker_props[CHECKER_PROP_ALLOW_LIST] =
    g_param_spec_boxed ("allow-list", NULL, NULL,
                        G_TYPE_STRV,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, CHECKER_N_PROPS, checker_props);
}

static void
shell_dbus_sender_checker_init (ShellDBusSenderChecker *self)
{
  self->allowlist_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->uninitialized_names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->watch_ids = g_array_new (FALSE, FALSE, sizeof (guint));
  g_queue_init (&self->pending_checks);
}

/**
 * shell_dbus_sender_checker_new:
 * @allow_list: (array zero-terminated=1) (element-type utf8): the allowed
 *   well-known names
 *
 * Returns: (transfer full): a new #ShellDBusSenderChecker
 */
ShellDBusSenderChecker *
shell_dbus_sender_checker_new (const char * const *allow_list)
{
  return g_object_new (SHELL_TYPE_DBUS_SENDER_CHECKER,
                       "allow-list", allow_list,
                       NULL);
}
