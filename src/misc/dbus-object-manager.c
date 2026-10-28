/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dbus-object-manager.c: Tracks the objects of a service implementing
 * org.freedesktop.DBus.ObjectManager. Port of js/misc/objectManager.js.
 *
 * Like the JS this is not a #GDBusObjectManagerClient: only the interfaces
 * the caller described (as introspection XML, in
 * #ShellDBusObjectManager:known-interfaces) are tracked, each with a plain
 * #GDBusProxy created with the matching #GDBusInterfaceInfo. Where the JS
 * took an `onLoaded` callback, the #ShellDBusObjectManager::loaded signal
 * is emitted, and the proxies are returned as #GDBusProxy (so their
 * properties are read with g_dbus_proxy_get_cached_property (), there are
 * no property wrappers).
 *
 * Differences with the JS version:
 *
 *  - ::interface-removed is not emitted for an interface that was never
 *    tracked (the JS emitted it with an undefined proxy);
 *  - a proxy for an object path and interface that is already tracked is
 *    dropped instead of being added twice;
 *  - nothing is emitted after ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "misc/dbus-object-manager.h"
#include "shell-global.h"

/* Specified in the D-Bus specification here:
 * http://dbus.freedesktop.org/doc/dbus-specification.html#standard-interfaces-objectmanager */
#define OBJECT_MANAGER_IFACE_XML \
  "<node>" \
  "<interface name='org.freedesktop.DBus.ObjectManager'>" \
  "  <method name='GetManagedObjects'>" \
  "    <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>" \
  "  </method>" \
  "  <signal name='InterfacesAdded'>" \
  "    <arg name='objectPath' type='o'/>" \
  "    <arg name='interfaces' type='a{sa{sv}}'/>" \
  "  </signal>" \
  "  <signal name='InterfacesRemoved'>" \
  "    <arg name='objectPath' type='o'/>" \
  "    <arg name='interfaces' type='as'/>" \
  "  </signal>" \
  "</interface>" \
  "</node>"

#define OBJECT_MANAGER_IFACE_NAME "org.freedesktop.DBus.ObjectManager"

enum
{
  PROP_0,

  PROP_CONNECTION,
  PROP_NAME,
  PROP_OBJECT_PATH,
  PROP_KNOWN_INTERFACES,
  PROP_CANCELLABLE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  OBJECT_ADDED,
  OBJECT_REMOVED,
  INTERFACE_ADDED,
  INTERFACE_REMOVED,
  LOADED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellDBusObjectManager
{
  GObject parent;

  GDBusConnection *connection;
  char *name;
  char *object_path;
  char **known_interfaces;
  GCancellable *external_cancellable;  /* nullable, as given by the caller */

  GCancellable *cancellable;
  gulong external_cancelled_id;
  gboolean shutting_down;

  GDBusNodeInfo *manager_node_info;
  GDBusProxy *manager_proxy;

  GHashTable *interface_infos;  /* interface name -> GDBusInterfaceInfo */
  GHashTable *objects;          /* object path -> (interface name -> GDBusProxy) */
  GHashTable *interfaces;       /* interface name -> GPtrArray of GDBusProxy (unowned) */
};

G_DEFINE_FINAL_TYPE (ShellDBusObjectManager, shell_dbus_object_manager, G_TYPE_OBJECT)

/* A set of asynchronous additions whose completion completes the load */
typedef struct
{
  ShellDBusObjectManager *manager;
  unsigned int pending;
} LoadOp;

typedef struct
{
  ShellDBusObjectManager *manager;
  char *object_path;
  char *interface_name;
  LoadOp *load_op;  /* nullable */
} AddInterfaceData;

static void
complete_load (ShellDBusObjectManager *self)
{
  if (self->shutting_down)
    return;

  g_signal_emit (self, signals[LOADED], 0);
}

static void
load_op_unref (LoadOp *op)
{
  if (--op->pending > 0)
    return;

  complete_load (op->manager);
  g_object_unref (op->manager);
  g_free (op);
}

static void
add_interface_data_free (AddInterfaceData *data)
{
  if (data->load_op != NULL)
    load_op_unref (data->load_op);

  g_object_unref (data->manager);
  g_free (data->object_path);
  g_free (data->interface_name);
  g_free (data);
}

static gboolean
should_stop (ShellDBusObjectManager *self,
             GError                 *error)
{
  return self->shutting_down ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
interface_proxy_ready (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  AddInterfaceData *data = user_data;
  ShellDBusObjectManager *self = data->manager;
  g_autoptr (GDBusProxy) proxy = NULL;
  g_autoptr (GError) error = NULL;
  GHashTable *object;
  GPtrArray *proxies;
  gboolean is_new_object;

  proxy = g_dbus_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      if (!should_stop (self, error))
        {
          g_warning ("could not initialize proxy for interface %s: %s",
                     data->interface_name, error->message);
        }
      add_interface_data_free (data);
      return;
    }

  if (self->shutting_down)
    {
      add_interface_data_free (data);
      return;
    }

  object = g_hash_table_lookup (self->objects, data->object_path);
  if (object != NULL && g_hash_table_contains (object, data->interface_name))
    {
      add_interface_data_free (data);
      return;
    }

  is_new_object = object == NULL;
  if (is_new_object)
    {
      object = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      g_free, g_object_unref);
      g_hash_table_insert (self->objects, g_strdup (data->object_path), object);
    }

  g_hash_table_insert (object, g_strdup (data->interface_name),
                       g_object_ref (proxy));

  proxies = g_hash_table_lookup (self->interfaces, data->interface_name);
  if (proxies == NULL)
    {
      proxies = g_ptr_array_new ();
      g_hash_table_insert (self->interfaces, g_strdup (data->interface_name),
                           proxies);
    }
  g_ptr_array_add (proxies, proxy);

  if (is_new_object)
    g_signal_emit (self, signals[OBJECT_ADDED], 0, data->object_path);

  g_signal_emit (self, signals[INTERFACE_ADDED], 0,
                 data->interface_name, proxy);

  add_interface_data_free (data);
}

static void
add_interface (ShellDBusObjectManager *self,
               const char             *object_path,
               const char             *interface_name,
               LoadOp                 *load_op)
{
  GDBusInterfaceInfo *info;
  AddInterfaceData *data;

  info = g_hash_table_lookup (self->interface_infos, interface_name);
  if (info == NULL)
    return;

  data = g_new0 (AddInterfaceData, 1);
  data->manager = g_object_ref (self);
  data->object_path = g_strdup (object_path);
  data->interface_name = g_strdup (interface_name);
  data->load_op = load_op;
  if (load_op != NULL)
    load_op->pending++;

  g_dbus_proxy_new (self->connection,
                    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                    info,
                    self->name,
                    object_path,
                    interface_name,
                    self->cancellable,
                    interface_proxy_ready,
                    data);
}

static void
remove_interface (ShellDBusObjectManager *self,
                  const char             *object_path,
                  const char             *interface_name)
{
  GHashTable *object;
  GPtrArray *proxies;
  g_autoptr (GDBusProxy) proxy = NULL;

  object = g_hash_table_lookup (self->objects, object_path);
  if (object == NULL)
    return;

  proxy = g_hash_table_lookup (object, interface_name);
  if (proxy == NULL)
    return;
  g_object_ref (proxy);

  proxies = g_hash_table_lookup (self->interfaces, interface_name);
  if (proxies != NULL)
    {
      g_ptr_array_remove (proxies, proxy);

      if (proxies->len == 0)
        g_hash_table_remove (self->interfaces, interface_name);
    }

  g_signal_emit (self, signals[INTERFACE_REMOVED], 0, interface_name, proxy);

  /* The object may have gone with a handler of the signal above */
  object = g_hash_table_lookup (self->objects, object_path);
  if (object == NULL)
    return;

  g_hash_table_remove (object, interface_name);

  if (g_hash_table_size (object) == 0)
    {
      g_autofree char *path = g_strdup (object_path);

      g_hash_table_remove (self->objects, path);
      g_signal_emit (self, signals[OBJECT_REMOVED], 0, path);
    }
}

static void
managed_objects_ready (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  ShellDBusObjectManager *self = user_data;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) objects = NULL;
  GVariantIter object_iter;
  const char *object_path;
  GVariant *interfaces;
  LoadOp *load_op;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
  if (result == NULL)
    {
      if (!should_stop (self, error))
        {
          g_warning ("could not get remote objects for service %s path %s: %s",
                     self->name, self->object_path, error->message);
          complete_load (self);
        }
      g_object_unref (self);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (self);
      return;
    }

  /* Completes the load when the last proxy is ready, whatever happened to it */
  load_op = g_new0 (LoadOp, 1);
  load_op->manager = g_object_ref (self);
  load_op->pending = 1;

  objects = g_variant_get_child_value (result, 0);
  g_variant_iter_init (&object_iter, objects);
  while (g_variant_iter_next (&object_iter, "{&o@a{sa{sv}}}",
                              &object_path, &interfaces))
    {
      GVariantIter interface_iter;
      const char *interface_name;
      GVariant *properties;

      g_variant_iter_init (&interface_iter, interfaces);
      while (g_variant_iter_next (&interface_iter, "{&s@a{sv}}",
                                  &interface_name, &properties))
        {
          add_interface (self, object_path, interface_name, load_op);
          g_variant_unref (properties);
        }

      g_variant_unref (interfaces);
    }

  load_op_unref (load_op);
  g_object_unref (self);
}

static void
on_name_appeared (ShellDBusObjectManager *self)
{
  g_dbus_proxy_call (self->manager_proxy,
                     "GetManagedObjects",
                     NULL,
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     self->cancellable,
                     managed_objects_ready,
                     g_object_ref (self));
}

static void
on_name_vanished (ShellDBusObjectManager *self)
{
  g_autoptr (GPtrArray) removals = g_ptr_array_new_full (0, g_free);
  GHashTableIter object_iter;
  gpointer object_path, object;
  unsigned int i;

  /* Collect first: removing modifies the tables */
  g_hash_table_iter_init (&object_iter, self->objects);
  while (g_hash_table_iter_next (&object_iter, &object_path, &object))
    {
      GHashTableIter interface_iter;
      gpointer interface_name;

      g_hash_table_iter_init (&interface_iter, object);
      while (g_hash_table_iter_next (&interface_iter, &interface_name, NULL))
        {
          g_ptr_array_add (removals, g_strdup (object_path));
          g_ptr_array_add (removals, g_strdup (interface_name));
        }
    }

  for (i = 0; i < removals->len; i += 2)
    {
      remove_interface (self,
                        g_ptr_array_index (removals, i),
                        g_ptr_array_index (removals, i + 1));
    }
}

static void
on_name_owner_changed (ShellDBusObjectManager *self)
{
  g_autofree char *name_owner = NULL;

  if (self->shutting_down)
    return;

  name_owner = g_dbus_proxy_get_name_owner (self->manager_proxy);
  if (name_owner != NULL)
    on_name_appeared (self);
  else
    on_name_vanished (self);
}

static void
on_manager_signal (GDBusProxy             *proxy,
                   const char             *sender_name,
                   const char             *signal_name,
                   GVariant               *parameters,
                   ShellDBusObjectManager *self)
{
  if (self->shutting_down)
    return;

  if (g_str_equal (signal_name, "InterfacesAdded"))
    {
      const char *object_path;
      g_autoptr (GVariant) interfaces = NULL;
      GVariantIter iter;
      const char *interface_name;
      GVariant *properties;

      g_variant_get (parameters, "(&o@a{sa{sv}})", &object_path, &interfaces);

      g_variant_iter_init (&iter, interfaces);
      while (g_variant_iter_next (&iter, "{&s@a{sv}}", &interface_name, &properties))
        {
          add_interface (self, object_path, interface_name, NULL);
          g_variant_unref (properties);
        }
    }
  else if (g_str_equal (signal_name, "InterfacesRemoved"))
    {
      const char *object_path;
      g_autofree const char **interface_names = NULL;
      int i;

      g_variant_get (parameters, "(&o^a&s)", &object_path, &interface_names);

      for (i = 0; interface_names[i] != NULL; i++)
        remove_interface (self, object_path, interface_names[i]);
    }
}

static void
manager_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  ShellDBusObjectManager *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *name_owner = NULL;

  self->manager_proxy = g_dbus_proxy_new_finish (res, &error);
  if (self->manager_proxy == NULL)
    {
      if (!should_stop (self, error))
        {
          g_warning ("could not initialize object manager for object %s: %s",
                     self->name, error->message);
          complete_load (self);
        }
      g_object_unref (self);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (self);
      return;
    }

  g_signal_connect_object (self->manager_proxy, "g-signal",
                           G_CALLBACK (on_manager_signal), self, 0);

  if (g_hash_table_size (self->interface_infos) == 0)
    {
      complete_load (self);
      g_object_unref (self);
      return;
    }

  g_signal_connect_swapped (self->manager_proxy, "notify::g-name-owner",
                            G_CALLBACK (on_name_owner_changed), self);

  name_owner = g_dbus_proxy_get_name_owner (self->manager_proxy);
  if (name_owner != NULL)
    on_name_appeared (self);

  g_object_unref (self);
}

static void
register_interfaces (ShellDBusObjectManager *self)
{
  int i;

  for (i = 0; self->known_interfaces != NULL && self->known_interfaces[i] != NULL; i++)
    {
      g_autoptr (GDBusNodeInfo) node = NULL;
      g_autoptr (GError) error = NULL;

      node = g_dbus_node_info_new_for_xml (self->known_interfaces[i], &error);
      if (node == NULL)
        {
          g_warning ("Could not parse the interface description: %s",
                     error->message);
          continue;
        }

      if (node->interfaces == NULL || node->interfaces[0] == NULL)
        {
          g_warning ("The interface description has no interface");
          continue;
        }

      g_hash_table_insert (self->interface_infos,
                           g_strdup (node->interfaces[0]->name),
                           g_dbus_interface_info_ref (node->interfaces[0]));
    }
}

static void
on_external_cancelled (GCancellable *external,
                       GCancellable *cancellable)
{
  g_cancellable_cancel (cancellable);
}

static void
on_shutdown (ShellGlobal            *global,
             ShellDBusObjectManager *self)
{
  /* Cancels the pending calls, nothing is emitted from now on */
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);

  if (self->manager_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->manager_proxy, self);
}

/**
 * shell_dbus_object_manager_new:
 * @connection: the #GDBusConnection of the service
 * @name: the well-known or unique bus name of the service
 * @object_path: the path of the object implementing
 *   org.freedesktop.DBus.ObjectManager
 * @known_interfaces: (array zero-terminated=1) (element-type utf8) (nullable):
 *   introspection XML (a `<node>` with one `<interface>`) for each interface
 *   to track; objects' other interfaces are ignored
 * @cancellable: (nullable): a #GCancellable to stop loading
 *
 * Starts tracking the objects of a service. The initial objects are known
 * when #ShellDBusObjectManager::loaded is emitted. The service is not
 * activated, if it is not running yet its objects are picked up when it
 * appears on the bus.
 *
 * Returns: (transfer full): a new #ShellDBusObjectManager
 */
ShellDBusObjectManager *
shell_dbus_object_manager_new (GDBusConnection    *connection,
                               const char         *name,
                               const char         *object_path,
                               const char * const *known_interfaces,
                               GCancellable       *cancellable)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);

  return g_object_new (SHELL_TYPE_DBUS_OBJECT_MANAGER,
                       "connection", connection,
                       "name", name,
                       "object-path", object_path,
                       "known-interfaces", known_interfaces,
                       "cancellable", cancellable,
                       NULL);
}

/**
 * shell_dbus_object_manager_get_proxy:
 * @manager: a #ShellDBusObjectManager
 * @object_path: an object path
 * @interface_name: an interface name
 *
 * Gets the proxy for an interface of an object.
 *
 * Returns: (transfer none) (nullable): the #GDBusProxy, or %NULL if the object
 *   or its interface is not tracked
 */
GDBusProxy *
shell_dbus_object_manager_get_proxy (ShellDBusObjectManager *manager,
                                     const char             *object_path,
                                     const char             *interface_name)
{
  GHashTable *object;

  g_return_val_if_fail (SHELL_IS_DBUS_OBJECT_MANAGER (manager), NULL);

  object = g_hash_table_lookup (manager->objects, object_path);
  if (object == NULL)
    return NULL;

  return g_hash_table_lookup (object, interface_name);
}

/**
 * shell_dbus_object_manager_get_proxies_for_interface:
 * @manager: a #ShellDBusObjectManager
 * @interface_name: an interface name
 *
 * Gets the proxies of all tracked objects that have an interface.
 *
 * Returns: (transfer container) (element-type GDBusProxy): the proxies, in the
 *   order the objects were added
 */
GList *
shell_dbus_object_manager_get_proxies_for_interface (ShellDBusObjectManager *manager,
                                                     const char             *interface_name)
{
  GPtrArray *proxies;
  GList *list = NULL;
  unsigned int i;

  g_return_val_if_fail (SHELL_IS_DBUS_OBJECT_MANAGER (manager), NULL);

  proxies = g_hash_table_lookup (manager->interfaces, interface_name);
  if (proxies == NULL)
    return NULL;

  for (i = 0; i < proxies->len; i++)
    list = g_list_prepend (list, g_ptr_array_index (proxies, i));

  return g_list_reverse (list);
}

static void
shell_dbus_object_manager_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellDBusObjectManager *self = SHELL_DBUS_OBJECT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_set_object (&self->connection, g_value_get_object (value));
      break;

    case PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;

    case PROP_OBJECT_PATH:
      g_free (self->object_path);
      self->object_path = g_value_dup_string (value);
      break;

    case PROP_KNOWN_INTERFACES:
      g_strfreev (self->known_interfaces);
      self->known_interfaces = g_value_dup_boxed (value);
      break;

    case PROP_CANCELLABLE:
      g_set_object (&self->external_cancellable, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_object_manager_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellDBusObjectManager *self = SHELL_DBUS_OBJECT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_value_set_object (value, self->connection);
      break;

    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, self->object_path);
      break;

    case PROP_KNOWN_INTERFACES:
      g_value_set_boxed (value, self->known_interfaces);
      break;

    case PROP_CANCELLABLE:
      g_value_set_object (value, self->external_cancellable);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_object_manager_constructed (GObject *object)
{
  ShellDBusObjectManager *self = SHELL_DBUS_OBJECT_MANAGER (object);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_dbus_object_manager_parent_class)->constructed (object);

  if (self->external_cancellable != NULL)
    {
      self->external_cancelled_id =
        g_cancellable_connect (self->external_cancellable,
                               G_CALLBACK (on_external_cancelled),
                               g_object_ref (self->cancellable),
                               g_object_unref);
    }

  self->manager_node_info = g_dbus_node_info_new_for_xml (OBJECT_MANAGER_IFACE_XML,
                                                          &error);
  g_assert (self->manager_node_info != NULL);

  register_interfaces (self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  g_dbus_proxy_new (self->connection,
                    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                    self->manager_node_info->interfaces[0],
                    self->name,
                    self->object_path,
                    OBJECT_MANAGER_IFACE_NAME,
                    self->cancellable,
                    manager_proxy_ready,
                    g_object_ref (self));
}

static void
shell_dbus_object_manager_dispose (GObject *object)
{
  ShellDBusObjectManager *self = SHELL_DBUS_OBJECT_MANAGER (object);

  self->shutting_down = TRUE;

  if (self->external_cancelled_id != 0)
    {
      g_cancellable_disconnect (self->external_cancellable,
                                self->external_cancelled_id);
      self->external_cancelled_id = 0;
    }

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->manager_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->manager_proxy, self);

  g_clear_pointer (&self->interfaces, g_hash_table_unref);
  g_clear_pointer (&self->objects, g_hash_table_unref);
  g_clear_object (&self->manager_proxy);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->external_cancellable);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (shell_dbus_object_manager_parent_class)->dispose (object);
}

static void
shell_dbus_object_manager_finalize (GObject *object)
{
  ShellDBusObjectManager *self = SHELL_DBUS_OBJECT_MANAGER (object);

  g_clear_pointer (&self->interface_infos, g_hash_table_unref);
  g_clear_pointer (&self->manager_node_info, g_dbus_node_info_unref);
  g_clear_pointer (&self->known_interfaces, g_strfreev);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->object_path, g_free);

  G_OBJECT_CLASS (shell_dbus_object_manager_parent_class)->finalize (object);
}

static void
shell_dbus_object_manager_class_init (ShellDBusObjectManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_dbus_object_manager_set_property;
  object_class->get_property = shell_dbus_object_manager_get_property;
  object_class->constructed = shell_dbus_object_manager_constructed;
  object_class->dispose = shell_dbus_object_manager_dispose;
  object_class->finalize = shell_dbus_object_manager_finalize;

  /**
   * ShellDBusObjectManager:connection:
   *
   * The connection the service is on.
   */
  props[PROP_CONNECTION] =
    g_param_spec_object ("connection", NULL, NULL,
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusObjectManager:name:
   *
   * The bus name of the service.
   */
  props[PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusObjectManager:object-path:
   *
   * The path of the object implementing org.freedesktop.DBus.ObjectManager.
   */
  props[PROP_OBJECT_PATH] =
    g_param_spec_string ("object-path", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusObjectManager:known-interfaces:
   *
   * Introspection XML of the interfaces to track.
   */
  props[PROP_KNOWN_INTERFACES] =
    g_param_spec_boxed ("known-interfaces", NULL, NULL,
                        G_TYPE_STRV,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusObjectManager:cancellable:
   *
   * A #GCancellable that stops loading and creating proxies when cancelled.
   */
  props[PROP_CANCELLABLE] =
    g_param_spec_object ("cancellable", NULL, NULL,
                         G_TYPE_CANCELLABLE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellDBusObjectManager::object-added:
   * @manager: the #ShellDBusObjectManager
   * @object_path: the path of the object
   *
   * Emitted when the first tracked interface of an object appeared, just
   * before ::interface-added.
   */
  signals[OBJECT_ADDED] =
    g_signal_new ("object-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  /**
   * ShellDBusObjectManager::object-removed:
   * @manager: the #ShellDBusObjectManager
   * @object_path: the path of the object
   *
   * Emitted when the last tracked interface of an object went away.
   */
  signals[OBJECT_REMOVED] =
    g_signal_new ("object-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  /**
   * ShellDBusObjectManager::interface-added:
   * @manager: the #ShellDBusObjectManager
   * @interface_name: the name of the interface
   * @proxy: the proxy for the interface of the new object
   *
   * Emitted when a tracked interface of an object appeared.
   */
  signals[INTERFACE_ADDED] =
    g_signal_new ("interface-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,
                  G_TYPE_DBUS_PROXY);

  /**
   * ShellDBusObjectManager::interface-removed:
   * @manager: the #ShellDBusObjectManager
   * @interface_name: the name of the interface
   * @proxy: the proxy for the interface of the object that went away
   *
   * Emitted when a tracked interface of an object went away, or when the
   * service left the bus.
   */
  signals[INTERFACE_REMOVED] =
    g_signal_new ("interface-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,
                  G_TYPE_DBUS_PROXY);

  /**
   * ShellDBusObjectManager::loaded:
   * @manager: the #ShellDBusObjectManager
   *
   * Emitted once the objects that exist when the manager started have been
   * added (or loading failed), the former `onLoaded` callback. It is emitted
   * later than the construction, so a handler connected right after that is
   * always called. If the service is not running, it is emitted when it
   * appears.
   */
  signals[LOADED] =
    g_signal_new ("loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_dbus_object_manager_init (ShellDBusObjectManager *self)
{
  self->cancellable = g_cancellable_new ();

  self->interface_infos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                 (GDestroyNotify) g_dbus_interface_info_unref);
  self->objects = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify) g_hash_table_unref);
  self->interfaces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify) g_ptr_array_unref);
}
