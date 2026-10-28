/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * components.c: The component manager of the shell. Port of
 * js/ui/components.js.
 *
 * Differences from the JS module:
 *
 *  - The components are enabled synchronously: the JS imported the module of
 *    a component with a dynamic `import ()`, which is why `_sessionModeUpdated
 *    ()` was asynchronous. Every component is a C class now, so the table of
 *    names below replaces both the import and `NATIVE_COMPONENTS`.
 *  - A name that is not in the table is a message, not a rejected promise that
 *    was silently ignored.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include "ui/components.h"
#include "ui/components/automount-manager.h"
#include "ui/components/autorun-manager.h"
#include "ui/components/keyring.h"
#include "ui/components/polkit-agent.h"
#ifdef HAVE_NETWORKMANAGER
#include "ui/components/network-agent.h"
#endif
#include "ui/main.h"

typedef struct
{
  const char *name;
  GObject *(* create) (void);
  void     (* enable) (GObject *component);
  void     (* disable) (GObject *component);
} ComponentInfo;

#define DEFINE_COMPONENT_OPS(Type, TYPE, type)                              \
  static void                                                               \
  type##_enable_op (GObject *component)                                     \
  {                                                                         \
    shell_##type##_enable (SHELL_##TYPE (component));                       \
  }                                                                         \
  static void                                                               \
  type##_disable_op (GObject *component)                                    \
  {                                                                         \
    shell_##type##_disable (SHELL_##TYPE (component));                      \
  }

DEFINE_COMPONENT_OPS (ShellAutomountManager, AUTOMOUNT_MANAGER, automount_manager)
DEFINE_COMPONENT_OPS (ShellAutorunManager, AUTORUN_MANAGER, autorun_manager)
DEFINE_COMPONENT_OPS (ShellKeyringComponent, KEYRING_COMPONENT, keyring_component)
DEFINE_COMPONENT_OPS (ShellPolkitAgentComponent, POLKIT_AGENT_COMPONENT, polkit_agent_component)
#ifdef HAVE_NETWORKMANAGER
DEFINE_COMPONENT_OPS (ShellNetworkAgentComponent, NETWORK_AGENT_COMPONENT, network_agent_component)
#endif

static GObject *
create_automount_manager (void)
{
  return G_OBJECT (shell_automount_manager_new ());
}

static GObject *
create_autorun_manager (void)
{
  return G_OBJECT (shell_autorun_manager_new (shell_main_get_message_tray (shell_main_get_default ())));
}

static GObject *
create_keyring_component (void)
{
  return G_OBJECT (shell_keyring_component_new ());
}

static GObject *
create_polkit_agent_component (void)
{
  return G_OBJECT (shell_polkit_agent_component_new ());
}

#ifdef HAVE_NETWORKMANAGER
static GObject *
create_network_agent_component (void)
{
  return G_OBJECT (shell_network_agent_component_new ());
}
#endif

/* The names the session modes use (data/ and ui/session-mode.c) */
static const ComponentInfo components[] = {
  { "automountManager", create_automount_manager,
    automount_manager_enable_op, automount_manager_disable_op },
  { "autorunManager", create_autorun_manager,
    autorun_manager_enable_op, autorun_manager_disable_op },
#ifdef HAVE_NETWORKMANAGER
  { "networkAgent", create_network_agent_component,
    network_agent_component_enable_op, network_agent_component_disable_op },
#endif
  { "keyring", create_keyring_component,
    keyring_component_enable_op, keyring_component_disable_op },
  { "polkitAgent", create_polkit_agent_component,
    polkit_agent_component_enable_op, polkit_agent_component_disable_op },
};

struct _ShellComponentManager
{
  GObject parent;

  ShellSessionMode *session_mode;  /* strong */
  gulong session_updated_id;

  GHashTable *all_components;      /* name -> GObject, both strong */
  GStrv enabled_components;
};

G_DEFINE_FINAL_TYPE (ShellComponentManager, shell_component_manager, G_TYPE_OBJECT)

static const ComponentInfo *
lookup_component (const char *name)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (components); i++)
    {
      if (strcmp (components[i].name, name) == 0)
        return &components[i];
    }

  return NULL;
}

/* _ensureComponent () */
static GObject *
ensure_component (ShellComponentManager *self,
                  const ComponentInfo   *info)
{
  GObject *component = g_hash_table_lookup (self->all_components, info->name);

  if (component != NULL)
    return component;

  if (shell_session_mode_is_locked (self->session_mode))
    return NULL;

  component = info->create ();
  g_hash_table_insert (self->all_components, g_strdup (info->name), component);

  return component;
}

/* _enableComponent () */
static void
enable_component (ShellComponentManager *self,
                  const char            *name)
{
  const ComponentInfo *info = lookup_component (name);
  GObject *component;

  if (info == NULL)
    {
      g_message ("Unknown shell component '%s'", name);
      return;
    }

  component = ensure_component (self, info);
  if (component != NULL)
    info->enable (component);
}

/* _disableComponent () */
static void
disable_component (ShellComponentManager *self,
                   const char            *name)
{
  const ComponentInfo *info = lookup_component (name);
  GObject *component = g_hash_table_lookup (self->all_components, name);

  if (info == NULL || component == NULL)
    return;

  info->disable (component);
}

/* _sessionModeUpdated () */
static void
session_mode_updated (ShellComponentManager *self)
{
  g_auto (GStrv) new_enabled = NULL;
  guint i;

  g_object_get (self->session_mode, "components", &new_enabled, NULL);
  if (new_enabled == NULL)
    new_enabled = g_new0 (char *, 1);

  for (i = 0; new_enabled[i] != NULL; i++)
    {
      if (!g_strv_contains ((const char * const *) self->enabled_components,
                            new_enabled[i]))
        enable_component (self, new_enabled[i]);
    }

  for (i = 0; self->enabled_components[i] != NULL; i++)
    {
      if (!g_strv_contains ((const char * const *) new_enabled,
                            self->enabled_components[i]))
        disable_component (self, self->enabled_components[i]);
    }

  g_strfreev (self->enabled_components);
  self->enabled_components = g_steal_pointer (&new_enabled);
}

static void
session_mode_updated_cb (ShellSessionMode      *session_mode,
                         ShellComponentManager *self)
{
  session_mode_updated (self);
}

static void
shell_component_manager_constructed (GObject *object)
{
  ShellComponentManager *self = SHELL_COMPONENT_MANAGER (object);

  G_OBJECT_CLASS (shell_component_manager_parent_class)->constructed (object);

  self->session_updated_id =
    g_signal_connect (self->session_mode, "updated",
                      G_CALLBACK (session_mode_updated_cb), self);

  session_mode_updated (self);
}

static void
shell_component_manager_dispose (GObject *object)
{
  ShellComponentManager *self = SHELL_COMPONENT_MANAGER (object);

  if (self->session_mode != NULL)
    g_clear_signal_handler (&self->session_updated_id, self->session_mode);
  g_clear_object (&self->session_mode);
  g_clear_pointer (&self->all_components, g_hash_table_unref);

  G_OBJECT_CLASS (shell_component_manager_parent_class)->dispose (object);
}

static void
shell_component_manager_finalize (GObject *object)
{
  ShellComponentManager *self = SHELL_COMPONENT_MANAGER (object);

  g_strfreev (self->enabled_components);

  G_OBJECT_CLASS (shell_component_manager_parent_class)->finalize (object);
}

enum
{
  PROP_0,

  PROP_SESSION_MODE,

  N_PROPS
};

static void
shell_component_manager_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellComponentManager *self = SHELL_COMPONENT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_SESSION_MODE:
      self->session_mode = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_component_manager_class_init (ShellComponentManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_component_manager_constructed;
  object_class->dispose = shell_component_manager_dispose;
  object_class->finalize = shell_component_manager_finalize;
  object_class->set_property = shell_component_manager_set_property;

  g_object_class_install_property (object_class, PROP_SESSION_MODE,
                                   g_param_spec_object ("session-mode", NULL, NULL,
                                                        SHELL_TYPE_SESSION_MODE,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
shell_component_manager_init (ShellComponentManager *self)
{
  self->all_components = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
  self->enabled_components = g_new0 (char *, 1);
}

/**
 * shell_component_manager_new:
 * @session_mode: the session mode whose `components` field is followed
 *
 * Creates the component manager (`new ComponentManager ()`). The components
 * of the current session mode are enabled right away.
 *
 * Returns: (transfer full): a new #ShellComponentManager
 */
ShellComponentManager *
shell_component_manager_new (ShellSessionMode *session_mode)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (session_mode), NULL);

  return g_object_new (SHELL_TYPE_COMPONENT_MANAGER,
                       "session-mode", session_mode,
                       NULL);
}
