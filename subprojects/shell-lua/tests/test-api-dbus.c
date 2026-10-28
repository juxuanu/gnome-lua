/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.dbus (docs/api/dbus.md) against a private session bus (GTestDBus).
 *
 * The test process owns a small service (org.example.TestService) in C; Lua
 * proxies call it, and Lua also exports and owns names of its own, which Lua
 * proxies of the same state call back. System bus: DBUS_SYSTEM_BUS_ADDRESS
 * points nowhere, so shell.dbus.system() exercises the "no connection" path.
 */

#include "test-utils.h"

#include "shell-hooks.h"
#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"

/* ------------------------------------------------------------------------ */
/* Waiting on Lua state                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
  ShellLuaState *state;
  const char    *expr;
} LuaCond;

static gboolean
pred_lua (gpointer data)
{
  LuaCond *c = data;

  g_autofree char *expr = g_strdup_printf ("not not (%s)", c->expr);

  return tu_eval_bool (c->state, expr);
}

#define wait_lua(state, expr) G_STMT_START { \
    LuaCond _c = { (state), (expr) }; \
    if (!tu_wait_for (pred_lua, &_c, 5000)) \
      g_error ("%s:%d: timed out waiting for: %s", __FILE__, __LINE__, (expr)); \
  } G_STMT_END

static ShellLuaState *
new_state (void)
{
  return tu_state_new ("dbus", SHELL_LUA_STATE_PRIVILEGED);
}

/* ------------------------------------------------------------------------ */
/* The service the Lua side talks to                                         */
/* ------------------------------------------------------------------------ */

static const char service_xml[] =
  "<node><interface name='org.example.Test'>"
  "  <method name='Echo'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
  "  <method name='Add'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "    <arg type='i' direction='out'/></method>"
  "  <method name='EchoV'><arg type='v' direction='in'/><arg type='v' direction='out'/></method>"
  "  <method name='EchoDict'><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method>"
  "  <method name='EchoBytes'><arg type='ay' direction='in'/><arg type='ay' direction='out'/></method>"
  "  <method name='EchoPairs'><arg type='a{uv}' direction='in'/><arg type='a{uv}' direction='out'/></method>"
  "  <method name='EchoStruct'><arg type='(ii)' direction='in'/><arg type='s' direction='in'/>"
  "    <arg type='(ii)' direction='out'/><arg type='s' direction='out'/></method>"
  "  <method name='U'><arg type='u' direction='in'/><arg type='u' direction='out'/></method>"
  "  <method name='Nothing'/>"
  "  <method name='Fail'/>"
  "  <method name='Slow'><arg type='s' direction='out'/></method>"
  "  <signal name='Ping'><arg type='s'/></signal>"
  "  <signal name='Pong'><arg type='u'/><arg type='as'/></signal>"
  "  <property name='Name' type='s' access='read'/>"
  "  <property name='Level' type='u' access='readwrite'/>"
  "  <property name='Anything' type='v' access='readwrite'/>"
  "</interface></node>";

#define SERVICE_NAME "org.example.TestService"
#define SERVICE_PATH "/org/example/Test"
#define SERVICE_IFACE "org.example.Test"

static struct {
  GDBusNodeInfo         *node;
  guint                  owner_id;
  guint                  registration_id;
  GDBusConnection       *connection;
  gboolean               acquired;
  gboolean               lost;
  char                  *last_call;       /* "Method params" of the last call */
  GDBusMethodInvocation *slow;
  GHashTable            *properties;      /* name -> GVariant */
} service;

static void
service_emit_properties_changed (const char *name, GVariant *value)
{
  GVariantBuilder changed;

  g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&changed, "{sv}", name, value);
  g_dbus_connection_emit_signal (service.connection, NULL, SERVICE_PATH,
                                 "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", SERVICE_IFACE, &changed, NULL),
                                 NULL);
}

static void
service_method_call (GDBusConnection       *connection,
                     const char            *sender,
                     const char            *object_path,
                     const char            *interface_name,
                     const char            *method_name,
                     GVariant              *parameters,
                     GDBusMethodInvocation *invocation,
                     gpointer               user_data)
{
  g_autofree char *printed = g_variant_print (parameters, TRUE);

  g_free (service.last_call);
  service.last_call = g_strdup_printf ("%s %s", method_name, printed);

  if (g_str_equal (method_name, "Add"))
    {
      int a, b;

      g_variant_get (parameters, "(ii)", &a, &b);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(i)", a + b));
    }
  else if (g_str_equal (method_name, "Fail"))
    {
      g_dbus_method_invocation_return_dbus_error (invocation, "org.example.Error.Nope",
                                                  "this always fails");
    }
  else if (g_str_equal (method_name, "Slow"))
    {
      g_assert_null (service.slow);
      service.slow = g_object_ref (invocation);
    }
  else if (g_str_equal (method_name, "Nothing"))
    {
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else
    {
      /* every Echo*, U: the reply is the request */
      g_dbus_method_invocation_return_value (invocation, parameters);
    }
}

static GVariant *
service_get_property (GDBusConnection  *connection,
                      const char       *sender,
                      const char       *object_path,
                      const char       *interface_name,
                      const char       *property_name,
                      GError          **error,
                      gpointer          user_data)
{
  GVariant *value = g_hash_table_lookup (service.properties, property_name);

  g_assert_nonnull (value);
  return g_variant_ref (value);
}

static gboolean
service_set_property (GDBusConnection  *connection,
                      const char       *sender,
                      const char       *object_path,
                      const char       *interface_name,
                      const char       *property_name,
                      GVariant         *value,
                      GError          **error,
                      gpointer          user_data)
{
  g_autofree char *printed = g_variant_print (value, TRUE);

  g_free (service.last_call);
  service.last_call = g_strdup_printf ("Set:%s %s", property_name, printed);
  g_hash_table_insert (service.properties, g_strdup (property_name), g_variant_ref_sink (value));
  service_emit_properties_changed (property_name, value);
  return TRUE;
}

static const GDBusInterfaceVTable service_vtable = {
  service_method_call, service_get_property, service_set_property, { 0 }
};

static void
service_bus_acquired (GDBusConnection *connection,
                      const char      *name,
                      gpointer         user_data)
{
  service.connection = connection;
  service.registration_id = g_dbus_connection_register_object (connection, SERVICE_PATH,
                                                               service.node->interfaces[0],
                                                               &service_vtable, NULL, NULL, NULL);
  g_assert_cmpuint (service.registration_id, !=, 0);
}

static void
service_name_acquired (GDBusConnection *connection,
                       const char      *name,
                       gpointer         user_data)
{
  service.acquired = TRUE;
}

static void
service_name_lost (GDBusConnection *connection,
                   const char      *name,
                   gpointer         user_data)
{
  service.acquired = FALSE;
  service.lost = TRUE;
}

static gboolean
pred_service_acquired (gpointer data)
{
  return service.acquired;
}

static void
service_start (void)
{
  g_assert_null (service.node);
  service.node = g_dbus_node_info_new_for_xml (service_xml, NULL);
  g_assert_nonnull (service.node);
  service.properties = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              (GDestroyNotify) g_variant_unref);
  g_hash_table_insert (service.properties, g_strdup ("Name"),
                       g_variant_ref_sink (g_variant_new_string ("initial")));
  g_hash_table_insert (service.properties, g_strdup ("Level"),
                       g_variant_ref_sink (g_variant_new_uint32 (3)));
  g_hash_table_insert (service.properties, g_strdup ("Anything"),
                       g_variant_ref_sink (g_variant_new_variant (g_variant_new_string ("x"))));

  service.lost = FALSE;
  service.owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, SERVICE_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                     service_bus_acquired, service_name_acquired,
                                     service_name_lost, NULL, NULL);
  g_assert_true (tu_wait_for (pred_service_acquired, NULL, 5000));
}

static void
service_stop (void)
{
  if (service.slow != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (service.slow, "org.example.Error.Late", "late");
      g_clear_object (&service.slow);
    }
  if (service.registration_id != 0)
    g_dbus_connection_unregister_object (service.connection, service.registration_id);
  service.registration_id = 0;
  if (service.owner_id != 0)
    g_bus_unown_name (service.owner_id);
  service.owner_id = 0;
  service.acquired = FALSE;
  service.connection = NULL;
  g_clear_pointer (&service.node, g_dbus_node_info_unref);
  g_clear_pointer (&service.properties, g_hash_table_unref);
  g_clear_pointer (&service.last_call, g_free);
}

static void
service_reply_slow (const char *text)
{
  g_assert_nonnull (service.slow);
  g_dbus_method_invocation_return_value (service.slow, g_variant_new ("(s)", text));
  g_clear_object (&service.slow);
}

static void
service_emit (const char *signal_name, GVariant *parameters)
{
  g_dbus_connection_emit_signal (service.connection, NULL, SERVICE_PATH, SERVICE_IFACE,
                                 signal_name, parameters, NULL);
}

static gboolean
pred_slow_pending (gpointer data)
{
  return service.slow != NULL;
}

/* The Lua preamble: a proxy of the service in the privileged state. */
#define PREAMBLE \
  "bus = shell.dbus.session()\n" \
  "svc = bus:proxy{ name = 'org.example.TestService', path = '/org/example/Test',\n" \
  "                 interface = 'org.example.Test' }\n"

/* ------------------------------------------------------------------------ */
/* Entry points, capability, misuse                                          */
/* ------------------------------------------------------------------------ */

static void
test_entry_points (void)
{
  ShellLuaState *s = new_state ();

  tu_assert_eval_true (s, "type(shell.dbus.session) == 'function' and type(shell.dbus.system) == 'function' "
                          "and type(shell.dbus.variant) == 'function'");
  tu_do (s, "bus = shell.dbus.session() sys = shell.dbus.system()");
  tu_assert_eval_true (s, "tostring(bus):find('^<shell%.Bus ') ~= nil");
  tu_assert_eval_true (s, "rawequal(bus, shell.dbus.session()) and rawequal(sys, shell.dbus.system())");
  tu_assert_eval_true (s, "bus ~= sys and bus.alive and sys.alive");

  /* a bus has no properties of its own, and no I/O to create */
  tu_assert_eval_true (s, "bus.name == nil and bus.path == nil");
  g_object_unref (s);
}

static void
test_capability_gate (void)
{
  ShellLuaState *sbx = tu_state_new ("sandbox", SHELL_LUA_STATE_SANDBOXED);

  /* a sandboxed state with no extension has no capabilities at all */
  tu_assert_fails (sbx, "shell.dbus.session()", SHELL_LUA_ERROR_RUNTIME,
                   "shell.dbus.session() requires the \"dbus\" capability");
  tu_assert_fails (sbx, "shell.dbus.system()", SHELL_LUA_ERROR_RUNTIME,
                   "shell.dbus.system() requires the \"dbus\" capability");
  tu_assert_fails (sbx, "shell.dbus.variant('u', 1)", SHELL_LUA_ERROR_RUNTIME,
                   "shell.dbus.variant() requires the \"dbus\" capability");
  g_object_unref (sbx);
}

static void
test_misuse (void)
{
  ShellLuaState *s = new_state ();

  tu_do (s, PREAMBLE);

  /* proxy{...} */
  tu_assert_fails (s, "bus:proxy{ path = '/a', interface = 'a.b' }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'name'");
  tu_assert_fails (s, "bus:proxy{ name = 'a.b', interface = 'a.b' }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'path'");
  tu_assert_fails (s, "bus:proxy{ name = 'a.b', path = '/a' }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'interface'");
  tu_assert_fails (s, "bus:proxy{ name = 'nodots', path = '/a', interface = 'a.b' }",
                   SHELL_LUA_ERROR_RUNTIME, "not a valid bus name");
  tu_assert_fails (s, "bus:proxy{ name = 'a.b', path = 'a', interface = 'a.b' }",
                   SHELL_LUA_ERROR_RUNTIME, "not a valid object path");
  tu_assert_fails (s, "bus:proxy{ name = 'a.b', path = '/a', interface = 'x' }",
                   SHELL_LUA_ERROR_RUNTIME, "not a valid interface name");
  tu_assert_fails (s, "bus:proxy{ name = 'a.b', path = '/a', interface = 'a.b', extra = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "unknown field 'extra'");
  tu_assert_fails (s, "bus:proxy('a.b')", SHELL_LUA_ERROR_RUNTIME, "options table expected");

  /* the shell's own names are reached through shell.* */
  tu_assert_fails (s, "bus:proxy{ name = 'org.gnome.Shell', path = '/a', interface = 'a.b' }",
                   SHELL_LUA_ERROR_RUNTIME, "reserved");
  tu_assert_fails (s, "bus:proxy{ name = 'org.gnome.Shell.Extensions', path = '/a', interface = 'a.b' }",
                   SHELL_LUA_ERROR_RUNTIME, "reserved");
  tu_assert_fails (s, "bus:proxy{ name = 'a.b', path = '/a', interface = 'org.gnome.Shell.Extensions' }",
                   SHELL_LUA_ERROR_RUNTIME, "reserved");
  tu_do (s, "bus:proxy{ name = 'org.gnome.Mutter.DisplayConfig', path = '/a', interface = 'a.b' }");
  tu_do (s, "bus:proxy{ name = 'org.gnome.ShellExtra', path = '/a', interface = 'a.b' }");

  /* call{...}: everything is checked before anything is sent */
  tu_assert_fails (s, "svc:call{}", SHELL_LUA_ERROR_RUNTIME, "missing required field 'method'");
  tu_assert_fails (s, "svc:call{ method = 'a b' }", SHELL_LUA_ERROR_RUNTIME, "not a valid D-Bus method name");
  tu_assert_fails (s, "svc:call{ method = 'Echo', args = { 'x' } }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'signature' is required when 'args' is not empty");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 's', args = {} }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'args' has 0 elements but signature 's' has 1");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 's', args = { 'a', 'b' } }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'args' has 2 elements but signature 's' has 1");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = '', args = { 'a' } }", SHELL_LUA_ERROR_RUNTIME,
                   "signature is empty");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 'ms', args = { 'a' } }", SHELL_LUA_ERROR_RUNTIME,
                   "not a valid D-Bus signature");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 'h', args = { 1 } }", SHELL_LUA_ERROR_RUNTIME,
                   "file descriptors");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 'ss', args = { 'a' } }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'args' has 1 elements but signature 'ss' has 2");
  tu_assert_fails (s, "svc:call{ method = 'U', signature = 'u', args = { -1 } }", SHELL_LUA_ERROR_RUNTIME,
                   "out of range");
  tu_assert_fails (s, "svc:call{ method = 'U', signature = 'u', args = { 1.5 } }", SHELL_LUA_ERROR_RUNTIME,
                   "non-integral");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 's', args = { 5 } }", SHELL_LUA_ERROR_RUNTIME,
                   "expected string");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 's', args = { true } }", SHELL_LUA_ERROR_RUNTIME,
                   "expected string");
  tu_assert_fails (s, "svc:call{ method = 'EchoV', signature = 'v', args = { shell.dbus.session } }",
                   SHELL_LUA_ERROR_RUNTIME, "cannot convert Lua function");
  tu_assert_fails (s, "svc:call{ method = 'Echo', signature = 'o', args = { 'not a path' } }",
                   SHELL_LUA_ERROR_RUNTIME, "not a valid D-Bus object path");
  tu_assert_fails (s, "svc:call{ method = 'Echo', timeout_ms = 0 }", SHELL_LUA_ERROR_RUNTIME,
                   "'timeout_ms' must be between 1 and 60000");
  tu_assert_fails (s, "svc:call{ method = 'Echo', timeout_ms = 60001 }", SHELL_LUA_ERROR_RUNTIME,
                   "'timeout_ms' must be between 1 and 60000");
  tu_assert_fails (s, "svc:call{ method = 'Echo', on_reply = 5 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'on_reply' must be a function");
  tu_assert_fails (s, "svc:call{ method = 'Echo', bogus = true }", SHELL_LUA_ERROR_RUNTIME,
                   "unknown field 'bogus'");
  tu_assert_fails (s, "svc:call{ method = 'U', signature = 'u', args = { shell.dbus.variant('u', 1) } }",
                   SHELL_LUA_ERROR_RUNTIME, "only valid where the type is 'v'");

  /* properties */
  tu_assert_fails (s, "svc:get_property{}", SHELL_LUA_ERROR_RUNTIME, "missing required field 'name'");
  tu_assert_fails (s, "svc:get_property{ name = 'x y' }", SHELL_LUA_ERROR_RUNTIME, "not a valid property name");
  tu_assert_fails (s, "svc:get_all{ name = 'Name' }", SHELL_LUA_ERROR_RUNTIME, "unknown field 'name'");
  tu_assert_fails (s, "svc:set_property{ name = 'Level', value = 1 }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'signature'");
  tu_assert_fails (s, "svc:set_property{ name = 'Level', signature = 'uu', value = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "exactly one complete D-Bus type");
  tu_assert_fails (s, "svc:set_property{ name = 'Level', signature = 'u' }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'value'");
  tu_assert_fails (s, "svc:set_property{ name = 'Level', signature = 'mu', value = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "not a valid D-Bus signature");
  tu_assert_fails (s, "svc:set_property{ name = 'Level', signature = 'u', value = 'x' }",
                   SHELL_LUA_ERROR_RUNTIME, "expected integer");

  /* signals */
  tu_assert_fails (s, "svc:on_signal('Ping')", SHELL_LUA_ERROR_RUNTIME, "function expected");
  tu_assert_fails (s, "svc:on_signal('no good', function() end)", SHELL_LUA_ERROR_RUNTIME,
                   "not a valid D-Bus signal name");
  tu_assert_fails (s, "svc:on_properties_changed(5)", SHELL_LUA_ERROR_RUNTIME, "function expected");

  /* names */
  tu_assert_fails (s, "bus:watch_name{ name = 'org.example.X' }", SHELL_LUA_ERROR_RUNTIME,
                   "at least one of 'on_appeared' and 'on_vanished'");
  tu_assert_fails (s, "bus:watch_name{ name = 'x', on_vanished = function() end }", SHELL_LUA_ERROR_RUNTIME,
                   "not a valid bus name");
  tu_assert_fails (s, "bus:own_name{ name = 'org.gnome.Foo' }", SHELL_LUA_ERROR_RUNTIME, "reserved namespace");
  tu_assert_fails (s, "bus:own_name{ name = 'org.freedesktop.Foo' }", SHELL_LUA_ERROR_RUNTIME,
                   "reserved namespace");
  tu_assert_fails (s, "bus:own_name{ name = ':1.5' }", SHELL_LUA_ERROR_RUNTIME, "well-known");
  tu_assert_fails (s, "bus:own_name{ name = 'org.example.Y', replace = 'yes' }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'replace' must be a boolean");

  /* variant() */
  tu_assert_fails (s, "shell.dbus.variant('ii', 1)", SHELL_LUA_ERROR_RUNTIME, "exactly one complete");
  tu_assert_fails (s, "shell.dbus.variant('mi', 1)", SHELL_LUA_ERROR_RUNTIME, "not a valid D-Bus signature");
  tu_assert_fails (s, "shell.dbus.variant('h', 1)", SHELL_LUA_ERROR_RUNTIME, "file descriptors");
  tu_assert_fails (s, "shell.dbus.variant('y', 256)", SHELL_LUA_ERROR_RUNTIME, "out of range");
  tu_assert_fails (s, "shell.dbus.variant('u')", SHELL_LUA_ERROR_RUNTIME, "value expected");
  tu_assert_eval_true (s, "type(shell.dbus.variant('u', 5)) == 'userdata'");
  tu_assert_eval_true (s, "tostring(shell.dbus.variant('a{sv}', { a = 1 })) == \"<variant 'a{sv}'>\"");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Calls                                                                     */
/* ------------------------------------------------------------------------ */

static void
test_call_reply (void)
{
  ShellLuaState *s = new_state ();

  service_start ();
  tu_do (s, PREAMBLE);
  tu_assert_eval_true (s, "svc.name == 'org.example.TestService' and svc.path == '/org/example/Test' "
                          "and svc.interface == 'org.example.Test' and svc.bus == 'session'");
  tu_assert_eval_true (s, "shell.dbus.system():proxy{ name = 'a.b', path = '/', interface = 'a.b' }.bus == 'system'");

  tu_do (s,
         "echo, add, nothing, sync_seen = nil, nil, nil, false\n"
         "c1 = svc:call{ method = 'Echo', signature = 's', args = { 'hello' },\n"
         "               on_reply = function(...) echo = table.pack(...) end }\n"
         "sync_seen = echo ~= nil\n"
         "c2 = svc:call{ method = 'Add', signature = 'ii', args = { 40, 2 },\n"
         "               on_reply = function(...) add = table.pack(...) end }\n"
         "c3 = svc:call{ method = 'Nothing', on_reply = function(...) nothing = table.pack(...) end }\n");
  tu_assert_eval_true (s, "tostring(c1):find('^<shell%.DBusCall ') ~= nil and c1.pending == true");
  tu_assert_eval_true (s, "sync_seen == false");       /* callbacks come from the main loop */

  wait_lua (s, "echo ~= nil and add ~= nil and nothing ~= nil");
  tu_assert_eval_true (s, "echo.n == 1 and echo[1] == 'hello'");
  tu_assert_eval_true (s, "add.n == 1 and add[1] == 42 and math.type(add[1]) == 'integer'");
  tu_assert_eval_true (s, "nothing.n == 0");
  tu_assert_eval_true (s, "c1.pending == false and c2.pending == false and c3.pending == false");
  tu_do (s, "c1:cancel() c1:cancel()");                 /* harmless on a finished call */

  /* the shell.Connection-free handles stay usable after completion */
  tu_assert_eval_true (s, "c1.alive");

  /* fire and forget: no reply is awaited, the service still sees the call */
  tu_do (s, "ff = svc:call{ method = 'Echo', signature = 's', args = { 'bye' } }");
  tu_assert_eval_true (s, "tostring(ff):find('^<shell%.DBusCall ') ~= nil");
  tu_spin (150);
  g_assert_cmpstr (service.last_call, ==, "Echo ('bye',)");

  g_object_unref (s);
  service_stop ();
}

static void
test_call_errors (void)
{
  ShellLuaState *s = new_state ();

  service_start ();
  tu_do (s, PREAMBLE
         "err = {}\n"
         "local function failing(key, proxy, method, extra)\n"
         "  local o = { method = method, on_error = function(e) err[key] = e end,\n"
         "              on_reply = function() err[key] = 'replied' end }\n"
         "  for k, v in pairs(extra or {}) do o[k] = v end\n"
         "  proxy:call(o)\n"
         "end\n"
         "nobody = bus:proxy{ name = 'org.example.Nobody', path = '/', interface = 'org.example.Nobody' }\n"
         "failing('unknown', nobody, 'Hello')\n"
         "failing('remote', svc, 'Fail')\n"
         "failing('nomethod', svc, 'Missing')\n"
         "failing('timeout', svc, 'Slow', { timeout_ms = 60 })\n"
         "sys = shell.dbus.system():proxy{ name = 'org.example.Nobody', path = '/', interface = 'org.example.Nobody' }\n"
         "failing('system', sys, 'Hello')\n");

  wait_lua (s, "err.unknown and err.remote and err.nomethod and err.timeout and err.system");

  tu_assert_eval_true (s, "err.unknown.name == 'org.freedesktop.DBus.Error.ServiceUnknown' "
                          "and err.unknown.remote == true and type(err.unknown.message) == 'string'");
  tu_assert_eval_str (s, "err.remote.name", "org.example.Error.Nope");
  tu_assert_eval_str (s, "err.remote.message", "this always fails");
  tu_assert_eval_str (s, "tostring(err.remote.remote)", "true");
  tu_assert_eval_str (s, "err.nomethod.name", "org.freedesktop.DBus.Error.UnknownMethod");
  tu_assert_eval_str (s, "tostring(err.nomethod.remote)", "true");
  /* the service never answered: a local failure, with the standard name */
  tu_assert_eval_str (s, "err.timeout.name", "org.freedesktop.DBus.Error.NoReply");
  tu_assert_eval_str (s, "tostring(err.timeout.remote)", "false");
  tu_assert_eval_str (s, "err.system.name", "org.freedesktop.DBus.Error.Disconnected");
  tu_assert_eval_str (s, "tostring(err.system.remote)", "false");

  /* a late reply to a call that already timed out is ignored */
  g_assert_true (tu_wait_for (pred_slow_pending, NULL, 2000));
  service_reply_slow ("too late");
  tu_spin (50);
  tu_assert_eval_str (s, "err.timeout.name", "org.freedesktop.DBus.Error.NoReply");

  /* no on_error: a warning in the extension's log domain, no error counted */
  g_test_expect_message ("shell-lua/dbus", G_LOG_LEVEL_WARNING,
                         "*org.example.Test.Fail failed: org.example.Error.Nope: this always fails*");
  tu_do (s, "quiet = svc:call{ method = 'Fail', on_reply = function() end }");
  tu_spin (200);   /* the reply, hence the warning, comes from the main loop */
  g_test_assert_expected_messages ();
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 0);

  g_object_unref (s);
  service_stop ();
}

static void
test_call_cancel (void)
{
  ShellLuaState *s = new_state ();

  service_start ();
  tu_do (s, PREAMBLE
         "replies = 0\n"
         "c = svc:call{ method = 'Slow', timeout_ms = 20000, on_reply = function() replies = replies + 1 end,\n"
         "              on_error = function() replies = replies + 1 end }\n");
  g_assert_true (tu_wait_for (pred_slow_pending, NULL, 2000));
  tu_assert_eval_true (s, "c.pending == true");
  tu_do (s, "c:cancel()");
  tu_assert_eval_true (s, "c.pending == false");
  service_reply_slow ("ignored");
  tu_spin (100);
  tu_assert_eval_true (s, "replies == 0");

  g_object_unref (s);
  service_stop ();
}

/* ------------------------------------------------------------------------ */
/* The wire types                                                            */
/* ------------------------------------------------------------------------ */

static void
test_mapping (void)
{
  ShellLuaState *s = new_state ();

  service_start ();
  tu_do (s, PREAMBLE
         "got = {}\n"
         "local function call(key, method, sig, args)\n"
         "  svc:call{ method = method, signature = sig, args = args,\n"
         "            on_reply = function(...) got[key] = table.pack(...) end,\n"
         "            on_error = function(e) got[key] = { error = e.name .. ': ' .. e.message } end }\n"
         "end\n"
         "call('u', 'U', 'u', { 4000000000 })\n"
         "call('float', 'U', 'u', { 7.0 })\n"
         "call('v_int', 'EchoV', 'v', { 5 })\n"
         "call('v_typed', 'EchoV', 'v', { shell.dbus.variant('y', 5) })\n"
         "call('v_str', 'EchoV', 'v', { 'text' })\n"
         "call('v_seq', 'EchoV', 'v', { { 1, 'two', true } })\n"
         "call('dict', 'EchoDict', 'a{sv}', { { n = 1, u = shell.dbus.variant('u', 2), s = 'x',\n"
         "                                       nested = { a = 1.5 }, list = { shell.dbus.variant('q', 3) } } })\n"
         "call('bytes', 'EchoBytes', 'ay', { 'a\\0b' })\n"
         "call('pairs', 'EchoPairs', 'a{uv}', { { { 1, 'one' }, { 2, shell.dbus.variant('d', 2) } } })\n"
         "call('struct', 'EchoStruct', '(ii)s', { { 1, 2 }, 'z' })\n");

  wait_lua (s, "got.u and got.float and got.v_int and got.v_typed and got.v_str and got.v_seq and got.dict "
               "and got.bytes and got.pairs and got.struct");

  /* integer types are exact: 4000000000 fits a 'u', and a float with an integral value is accepted */
  tu_assert_eval_true (s, "got.u[1] == 4000000000 and got.float[1] == 7");
  /* a plain Lua integer in a `v` position is an int64, the typed one keeps its type; replies unwrap `v` */
  tu_assert_eval_true (s, "got.v_int[1] == 5 and got.v_typed[1] == 5 and got.v_str[1] == 'text'");
  tu_assert_eval_true (s, "#got.v_seq[1] == 3 and got.v_seq[1][1] == 1 and got.v_seq[1][2] == 'two' "
                          "and got.v_seq[1][3] == true");
  tu_assert_eval_true (s, "got.dict[1].n == 1 and got.dict[1].u == 2 and got.dict[1].s == 'x' "
                          "and got.dict[1].nested.a == 1.5 and got.dict[1].list[1] == 3");
  tu_assert_eval_true (s, "got.bytes[1] == 'a\\0b'");
  /* a{uv} is a sequence of {key, value} pairs in both directions */
  tu_assert_eval_true (s, "#got.pairs[1] == 2 and got.pairs[1][1][1] == 1 and got.pairs[1][1][2] == 'one' "
                          "and got.pairs[1][2][1] == 2 and got.pairs[1][2][2] == 2.0");
  tu_assert_eval_true (s, "got.struct.n == 2 and got.struct[1][1] == 1 and got.struct[1][2] == 2 "
                          "and got.struct[2] == 'z'");

  /* what the service saw on the wire */
  tu_do (s, "svc:call{ method = 'EchoV', signature = 'v', args = { shell.dbus.variant('u', 5) },\n"
            "          on_reply = function() done = true end }");
  wait_lua (s, "done == true");
  g_assert_cmpstr (service.last_call, ==, "EchoV (<uint32 5>,)");
  tu_do (s, "done = false svc:call{ method = 'EchoV', signature = 'v', args = { 5 },\n"
            "          on_reply = function() done = true end }");
  wait_lua (s, "done == true");
  g_assert_cmpstr (service.last_call, ==, "EchoV (<int64 5>,)");
  tu_do (s, "done = false svc:call{ method = 'EchoDict', signature = 'a{sv}',\n"
            "          args = { { u = shell.dbus.variant('u', 2), n = 1 } },\n"
            "          on_reply = function() done = true end }");
  wait_lua (s, "done == true");
  g_assert_cmpstr (service.last_call, ==, "EchoDict ({'n': <int64 1>, 'u': <uint32 2>},)");
  tu_do (s, "done = false svc:call{ method = 'EchoBytes', signature = 'ay', args = { 'ab' },\n"
            "          on_reply = function() done = true end }");
  wait_lua (s, "done == true");
  g_assert_cmpstr (service.last_call, ==, "EchoBytes ([byte 0x61, 0x62],)");

  g_object_unref (s);
  service_stop ();
}

/* ------------------------------------------------------------------------ */
/* Properties                                                                */
/* ------------------------------------------------------------------------ */

static void
test_properties (void)
{
  ShellLuaState *s = new_state ();

  service_start ();
  tu_do (s, PREAMBLE
         "r = {}\n"
         "svc:get_property{ name = 'Name', on_reply = function(...) r.name = table.pack(...) end }\n"
         "svc:get_property{ name = 'Level', on_reply = function(...) r.level = table.pack(...) end }\n"
         "svc:get_property{ name = 'Nope', on_error = function(e) r.nope = e.name end }\n"
         "svc:get_all{ on_reply = function(...) r.all = table.pack(...) end }\n");
  wait_lua (s, "r.name and r.level and r.nope and r.all");
  tu_assert_eval_true (s, "r.name.n == 1 and r.name[1] == 'initial' and r.level[1] == 3");
  tu_assert_eval_true (s, "r.nope == 'org.freedesktop.DBus.Error.InvalidArgs' "
                          "or r.nope:find('^org%.freedesktop%.DBus%.Error%.') ~= nil");
  tu_assert_eval_true (s, "r.all.n == 1 and r.all[1].Name == 'initial' and r.all[1].Level == 3 "
                          "and r.all[1].Anything == 'x'");

  /* set_property: the signature is the wire type, there is no default */
  tu_do (s, "svc:set_property{ name = 'Level', signature = 'u', value = 9, on_done = function(...) r.set = select('#', ...) end }");
  wait_lua (s, "r.set ~= nil");
  tu_assert_eval_true (s, "r.set == 0");
  g_assert_cmpstr (service.last_call, ==, "Set:Level uint32 9");
  tu_do (s, "r.set = nil svc:set_property{ name = 'Anything', signature = 'v', value = shell.dbus.variant('y', 7),\n"
            "                              on_done = function() r.set = true end }");
  wait_lua (s, "r.set == true");
  g_assert_cmpstr (service.last_call, ==, "Set:Anything <byte 0x07>");
  /* the wire type must be exact: a string is not the `v` the property has */
  tu_do (s, "r.err = nil svc:set_property{ name = 'Anything', signature = 's', value = 'text',\n"
            "                              on_done = function() r.err = 'done' end,\n"
            "                              on_error = function(e) r.err = e.name end }");
  wait_lua (s, "r.err ~= nil");
  tu_assert_eval_str (s, "r.err", "org.freedesktop.DBus.Error.InvalidArgs");
  tu_do (s, "r.err = nil svc:set_property{ name = 'Name', signature = 's', value = 'x',\n"
            "                              on_done = function() r.err = 'done' end,\n"
            "                              on_error = function(e) r.err = e.name end }");
  wait_lua (s, "r.err ~= nil");
  tu_assert_eval_true (s, "r.err:find('^org%.freedesktop%.DBus%.Error%.') ~= nil");   /* read-only */

  /* PropertiesChanged of this interface, with changed values and invalidated names */
  tu_do (s, "changes = {}\n"
            "conn = svc:on_properties_changed(function(changed, invalidated)\n"
            "  changes[#changes + 1] = { changed = changed, invalidated = invalidated }\n"
            "end)\n");
  tu_assert_eval_true (s, "tostring(conn):find('^<shell%.Connection ') ~= nil and conn:is_connected()");
  {
    GVariantBuilder changed;
    const char *invalidated[] = { "Name", NULL };

    g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&changed, "{sv}", "Level", g_variant_new_uint32 (11));
    g_variant_builder_add (&changed, "{sv}", "Anything", g_variant_new_variant (g_variant_new_boolean (TRUE)));
    /* another interface's change must not be seen */
    tu_spin (50);   /* the subscription is made once the connection is up */
    g_dbus_connection_emit_signal (service.connection, NULL, SERVICE_PATH,
                                   "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                   g_variant_new ("(sa{sv}^as)", "org.example.Other", &changed, invalidated),
                                   NULL);
    g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&changed, "{sv}", "Level", g_variant_new_uint32 (11));
    g_variant_builder_add (&changed, "{sv}", "Anything", g_variant_new_variant (g_variant_new_boolean (TRUE)));
    g_dbus_connection_emit_signal (service.connection, NULL, SERVICE_PATH,
                                   "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                   g_variant_new ("(sa{sv}^as)", SERVICE_IFACE, &changed, invalidated),
                                   NULL);
  }
  wait_lua (s, "#changes >= 1");
  tu_spin (50);
  tu_assert_eval_true (s, "#changes == 1 and changes[1].changed.Level == 11 and changes[1].changed.Anything == true "
                          "and #changes[1].invalidated == 1 and changes[1].invalidated[1] == 'Name'");
  tu_do (s, "conn:disconnect()");
  tu_assert_eval_true (s, "not conn:is_connected()");

  g_object_unref (s);
  service_stop ();
}

/* ------------------------------------------------------------------------ */
/* Signals                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_signals (void)
{
  ShellLuaState *s = new_state ();
  const char *words[] = { "a", "b", NULL };

  service_start ();
  tu_do (s, PREAMBLE
         "pings, any = {}, {}\n"
         "c1 = svc:on_signal('Ping', function(name, text) pings[#pings + 1] = { name, text } end)\n"
         "c2 = svc:on_signal(nil, function(...) any[#any + 1] = table.pack(...) end)\n"
         "ready = false shell.timer.after{ ms = 100, callback = function() ready = true end }\n");
  wait_lua (s, "ready");

  service_emit ("Ping", g_variant_new ("(s)", "hello"));
  service_emit ("Pong", g_variant_new ("(u^as)", 7, words));
  wait_lua (s, "#any == 2 and #pings == 1");
  tu_assert_eval_true (s, "pings[1][1] == 'Ping' and pings[1][2] == 'hello'");
  tu_assert_eval_true (s, "any[1].n == 2 and any[1][1] == 'Ping' and any[1][2] == 'hello'");
  tu_assert_eval_true (s, "any[2].n == 3 and any[2][1] == 'Pong' and any[2][2] == 7 "
                          "and #any[2][3] == 2 and any[2][3][2] == 'b'");

  tu_do (s, "c1:disconnect()");
  service_emit ("Ping", g_variant_new ("(s)", "again"));
  wait_lua (s, "#any == 3");
  tu_assert_eval_true (s, "#pings == 1");

  /* a subscription removed before the connection was up never subscribes */
  tu_do (s, "late = 0 c3 = svc:on_signal('Ping', function() late = late + 1 end) c3:disconnect()");
  tu_spin (100);
  service_emit ("Ping", g_variant_new ("(s)", "third"));
  wait_lua (s, "#any == 4");
  tu_assert_eval_true (s, "late == 0");

  g_object_unref (s);
  service_stop ();
}

/* ------------------------------------------------------------------------ */
/* Name watches                                                              */
/* ------------------------------------------------------------------------ */

static void
test_watch_name (void)
{
  ShellLuaState *s = new_state ();

  tu_do (s, PREAMBLE
         "ev = {}\n"
         "w = bus:watch_name{ name = 'org.example.TestService',\n"
         "  on_appeared = function(owner) ev[#ev + 1] = 'appeared:' .. tostring(owner):sub(1, 1) end,\n"
         "  on_vanished = function(...) ev[#ev + 1] = 'vanished:' .. select('#', ...) end }\n"
         "only_vanished = {}\n"
         "w2 = bus:watch_name{ name = 'org.example.Nobody',\n"
         "  on_vanished = function() only_vanished[#only_vanished + 1] = true end }\n");
  tu_assert_eval_true (s, "tostring(w):find('^<shell%.Connection ') ~= nil");

  /* no owner yet: on_vanished at once (from the main loop) */
  wait_lua (s, "ev[1] == 'vanished:0' and #only_vanished == 1");

  service_start ();
  wait_lua (s, "ev[2] == 'appeared::'");       /* the owner is a unique name */
  service_stop ();
  wait_lua (s, "ev[3] == 'vanished:0'");

  /* a name that already has an owner calls on_appeared when the watch starts */
  service_start ();
  wait_lua (s, "ev[4] == 'appeared::'");
  tu_do (s, "ev2 = {} w3 = bus:watch_name{ name = 'org.example.TestService',\n"
            "  on_appeared = function(owner) ev2[#ev2 + 1] = owner end }");
  wait_lua (s, "#ev2 == 1 and ev2[1]:sub(1, 1) == ':'");

  /* disconnect stops the callbacks */
  tu_do (s, "w:disconnect() w3:disconnect()");
  tu_assert_eval_true (s, "not w:is_connected()");
  service_stop ();
  tu_spin (100);
  tu_assert_eval_true (s, "#ev == 4 and #ev2 == 1");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Owning names and exporting objects                                        */
/* ------------------------------------------------------------------------ */

#define EXPORT_XML \
  "[[<node><interface name='org.example.Lua'>" \
  "  <method name='Hello'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>" \
  "  <method name='Pair'><arg type='i' direction='out'/><arg type='s' direction='out'/></method>" \
  "  <method name='Nothing'/>" \
  "  <method name='Refuse'><arg type='s' direction='out'/></method>" \
  "  <method name='Boom'/>" \
  "  <method name='TooMany'><arg type='s' direction='out'/></method>" \
  "  <method name='BadType'><arg type='u' direction='out'/></method>" \
  "  <method name='Declared'/>" \
  "  <signal name='Changed'><arg type='s'/><arg type='u'/></signal>" \
  "  <property name='Name' type='s' access='read'/>" \
  "  <property name='Count' type='u' access='read'/>" \
  "  <property name='Tags' type='as' access='read'/>" \
  "  <property name='Extra' type='a{sv}' access='read'/>" \
  "</interface></node>]]"

static void
test_own_and_export (void)
{
  ShellLuaState *s = new_state ();

  tu_do (s, "bus = shell.dbus.session()\n"
            "acquired, lost = 0, 0\n"
            "owner = bus:own_name{ name = 'org.example.LuaService', allow_replacement = true,\n"
            "  on_acquired = function(...) acquired = acquired + 1 args_acq = select('#', ...) end,\n"
            "  on_lost = function() lost = lost + 1 end }\n"
            "obj = bus:export{ path = '/org/example/Lua', interface_xml = " EXPORT_XML ",\n"
            "  methods = {\n"
            "    Hello = function(who) return 'hello ' .. who end,\n"
            "    Pair = function() return 5, 'five' end,\n"
            "    Nothing = function() end,\n"
            "    Refuse = function() return nil, 'org.example.Error.NotReady', 'later' end,\n"
            "    Boom = function() error('boom') end,\n"
            "    TooMany = function() return 'a', 'b' end,\n"
            "    BadType = function() return 'not a number' end,\n"
            "  },\n"
            "  properties = { Name = 'lua', Count = 2, Tags = { 'x', 'y' } } }\n");
  tu_assert_eval_true (s, "tostring(owner):find('^<shell%.BusName ') ~= nil and owner.name == 'org.example.LuaService'");
  tu_assert_eval_true (s, "tostring(obj):find('^<shell%.DBusObject ') ~= nil");
  tu_assert_eval_true (s, "obj.path == '/org/example/Lua' and obj.interface == 'org.example.Lua'");
  tu_assert_eval_true (s, "owner.acquired == false");        /* from the main loop */
  wait_lua (s, "acquired == 1");
  tu_assert_eval_true (s, "owner.acquired == true and args_acq == 0");

  tu_do (s, "me = bus:proxy{ name = 'org.example.LuaService', path = '/org/example/Lua',\n"
            "                interface = 'org.example.Lua' }\n"
            "res = {}\n"
            "function call(key, method, sig, args)\n"
            "  me:call{ method = method, signature = sig, args = args,\n"
            "           on_reply = function(...) res[key] = table.pack(...) end,\n"
            "           on_error = function(e) res[key] = { error = e.name, message = e.message, remote = e.remote } end }\n"
            "end\n"
            "call('hello', 'Hello', 's', { 'you' })\n"
            "call('pair', 'Pair')\n"
            "call('nothing', 'Nothing')\n"
            "call('refuse', 'Refuse')\n"
            "call('declared', 'Declared')\n"
            "call('unknown', 'NoSuchMethod')\n");
  wait_lua (s, "res.hello and res.pair and res.nothing and res.refuse and res.declared and res.unknown");
  tu_assert_eval_true (s, "res.hello.n == 1 and res.hello[1] == 'hello you'");
  tu_assert_eval_true (s, "res.pair.n == 2 and res.pair[1] == 5 and res.pair[2] == 'five'");
  tu_assert_eval_true (s, "res.nothing.n == 0");
  tu_assert_eval_true (s, "res.refuse.error == 'org.example.Error.NotReady' and res.refuse.message == 'later' "
                          "and res.refuse.remote == true");
  /* declared in the XML but no function: UnknownMethod */
  tu_assert_eval_str (s, "res.declared.error", "org.freedesktop.DBus.Error.UnknownMethod");
  tu_assert_eval_str (s, "res.unknown.error", "org.freedesktop.DBus.Error.UnknownMethod");

  /* a Lua error, a wrong count and a wrong type: Failed to the peer, a counted error here */
  tu_do (s, "call('boom', 'Boom') call('many', 'TooMany') call('bad', 'BadType')");
  wait_lua (s, "res.boom and res.many and res.bad");
  tu_assert_eval_str (s, "res.boom.error", "org.freedesktop.DBus.Error.Failed");
  tu_assert_eval_str (s, "res.many.error", "org.freedesktop.DBus.Error.Failed");
  tu_assert_eval_str (s, "res.bad.error", "org.freedesktop.DBus.Error.Failed");
  tu_assert_eval_true (s, "not res.boom.message:find('boom') and not res.boom.message:find('traceback')");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 3);

  /* properties are the ones given, defaults for the rest */
  tu_do (s, "props = nil me:get_all{ on_reply = function(p) props = p end }");
  wait_lua (s, "props ~= nil");
  tu_assert_eval_true (s, "props.Name == 'lua' and props.Count == 2 and #props.Tags == 2 and props.Tags[2] == 'y'");
  tu_assert_eval_true (s, "next(props.Extra) == nil");       /* default: empty */

  /* set_property emits PropertiesChanged and updates what Get returns; emit_signal reaches on_signal */
  tu_do (s, "changed, sig, p2 = nil, nil, nil\n"
            "keep1 = me:on_properties_changed(function(c, inv) changed = c end)\n"
            "keep2 = me:on_signal('Changed', function(name, text, n) sig = { name, text, n } end)\n"
            "ready = false shell.timer.after{ ms = 100, callback = function() ready = true end }\n");
  wait_lua (s, "ready");
  tu_do (s, "obj:set_property('Count', 7) obj:set_property('Name', 'renamed')\n"
            "obj:emit_signal{ name = 'Changed', args = { 'why', 3 } }\n"
            "me:get_property{ name = 'Count', on_reply = function(v) p2 = v end }");
  wait_lua (s, "changed ~= nil and sig ~= nil and p2 ~= nil");
  tu_assert_eval_true (s, "p2 == 7 and sig[1] == 'Changed' and sig[2] == 'why' and sig[3] == 3");

  /* misuse */
  tu_assert_fails (s, "obj:set_property('Nope', 1)", SHELL_LUA_ERROR_RUNTIME, "has no property 'Nope'");
  tu_assert_fails (s, "obj:set_property('Count', 'x')", SHELL_LUA_ERROR_RUNTIME, "expected integer");
  tu_assert_fails (s, "obj:set_property('Count', -1)", SHELL_LUA_ERROR_RUNTIME, "out of range");
  tu_assert_fails (s, "obj:emit_signal{ name = 'Nope' }", SHELL_LUA_ERROR_RUNTIME, "has no signal 'Nope'");
  tu_assert_fails (s, "obj:emit_signal{ name = 'Changed', args = { 'x' } }", SHELL_LUA_ERROR_RUNTIME,
                   "takes 2 arguments, field 'args' has 1");
  tu_assert_fails (s, "obj:emit_signal{ name = 'Changed', args = { 'x', 'y' } }", SHELL_LUA_ERROR_RUNTIME,
                   "expected integer");

  /* the name is taken from us by a process that asks to replace it */
  {
    g_autoptr (GError) error = NULL;
    g_autofree char *address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, NULL, &error);
    g_autoptr (GDBusConnection) other = NULL;
    g_autoptr (GVariant) reply = NULL;
    guint32 code = 0;

    g_assert_no_error (error);
    other = g_dbus_connection_new_for_address_sync (address,
                                                    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                    NULL, NULL, &error);
    g_assert_no_error (error);
    /* REPLACE_EXISTING (2): the daemon answers without our main loop */
    reply = g_dbus_connection_call_sync (other, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                         "org.freedesktop.DBus", "RequestName",
                                         g_variant_new ("(su)", "org.example.LuaService", 2),
                                         NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error (error);
    g_variant_get (reply, "(u)", &code);
    g_assert_cmpuint (code, ==, 1);
    wait_lua (s, "lost == 1");
    tu_assert_eval_true (s, "owner.acquired == false");
    g_dbus_connection_close_sync (other, NULL, NULL);
  }

  /* destroy() unexports and releases */
  tu_do (s, "obj:destroy() obj:destroy() owner:destroy() owner:release()");
  tu_assert_eval_true (s, "not obj.alive and not owner.alive");
  tu_assert_fails (s, "obj:set_property('Count', 1)", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  tu_do (s, "gone = nil me:call{ method = 'Nothing', on_error = function(e) gone = e.name end }");
  wait_lua (s, "gone ~= nil");
  tu_assert_eval_true (s, "gone == 'org.freedesktop.DBus.Error.ServiceUnknown'");

  g_object_unref (s);
}

static void
test_export_validation (void)
{
  ShellLuaState *s = new_state ();
  const char *base = "[[<node><interface name='org.example.V'><method name='M'/>"
                     "<property name='P' type='u' access='read'/></interface></node>]]";
  g_autofree char *cmd = NULL;

  tu_do (s, "bus = shell.dbus.session()");
  g_free (cmd);
  cmd = g_strdup_printf ("bus:export{ path = '/v', interface_xml = %s }", base);
  tu_do (s, cmd);

  tu_assert_fails (s, "bus:export{ interface_xml = '<node/>' }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'path'");
  tu_assert_fails (s, "bus:export{ path = '/v' }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'interface_xml'");
  tu_assert_fails (s, "bus:export{ path = 'v', interface_xml = '<node/>' }", SHELL_LUA_ERROR_RUNTIME,
                   "not a valid object path");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = '<node' }", SHELL_LUA_ERROR_RUNTIME,
                   "is not valid");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = '<node/>' }", SHELL_LUA_ERROR_RUNTIME,
                   "exactly one <interface>");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='a.b'/>"
                      "<interface name='a.c'/></node>]] }", SHELL_LUA_ERROR_RUNTIME,
                   "exactly one <interface>");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='a.b'/>"
                      "<node name='child'/></node>]] }", SHELL_LUA_ERROR_RUNTIME, "child <node>");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='a.b'>"
                      "<property name='W' type='u' access='readwrite'/></interface></node>]] }",
                   SHELL_LUA_ERROR_RUNTIME, "must be read-only");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='a.b'>"
                      "<property name='W' type='u' access='write'/></interface></node>]] }",
                   SHELL_LUA_ERROR_RUNTIME, "must be read-only");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='a.b'>"
                      "<method name='M'><arg type='h' direction='in'/></method></interface></node>]] }",
                   SHELL_LUA_ERROR_RUNTIME, "file descriptors");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='org.gnome.Foo'/></node>]] }",
                   SHELL_LUA_ERROR_RUNTIME, "reserved interface");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='org.gnome.Shell.Foo'/></node>]] }",
                   SHELL_LUA_ERROR_RUNTIME, "reserved interface");
  tu_assert_fails (s, "bus:export{ path = '/v', interface_xml = [[<node><interface name='org.freedesktop.Foo'/></node>]] }",
                   SHELL_LUA_ERROR_RUNTIME, "reserved interface");

  g_free (cmd);
  cmd = g_strdup_printf ("bus:export{ path = '/v', interface_xml = %s, methods = { Nope = function() end } }", base);
  tu_assert_fails (s, cmd, SHELL_LUA_ERROR_RUNTIME, "names 'Nope', which interface_xml does not declare");
  g_free (cmd);
  cmd = g_strdup_printf ("bus:export{ path = '/v', interface_xml = %s, methods = { M = 5 } }", base);
  tu_assert_fails (s, cmd, SHELL_LUA_ERROR_RUNTIME, "'methods.M' must be a function");
  g_free (cmd);
  cmd = g_strdup_printf ("bus:export{ path = '/v', interface_xml = %s, properties = { Q = 1 } }", base);
  tu_assert_fails (s, cmd, SHELL_LUA_ERROR_RUNTIME, "names 'Q', which interface_xml does not declare");
  g_free (cmd);
  cmd = g_strdup_printf ("bus:export{ path = '/v', interface_xml = %s, properties = { P = 'x' } }", base);
  tu_assert_fails (s, cmd, SHELL_LUA_ERROR_RUNTIME, "expected integer");
  g_free (cmd);
  cmd = g_strdup_printf ("bus:export{ path = '/v', interface_xml = %s, extra = 1 }", base);
  tu_assert_fails (s, cmd, SHELL_LUA_ERROR_RUNTIME, "unknown field 'extra'");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Teardown with an extension                                                */
/* ------------------------------------------------------------------------ */

static void
write_ext (const char *root, const char *uuid, const char *capabilities, const char *lua)
{
  g_autofree char *dir = g_build_filename (root, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Test\", \"shell-api\": \"1\"%s}",
                                               uuid, capabilities);

  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", lua);
}

static gboolean
name_has_owner (const char *name)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_autoptr (GVariant) reply = NULL;
  gboolean has_owner = FALSE;

  g_assert_no_error (error);
  reply = g_dbus_connection_call_sync (connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus", "NameHasOwner",
                                       g_variant_new ("(s)", name), NULL,
                                       G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  g_assert_no_error (error);
  g_variant_get (reply, "(b)", &has_owner);
  return has_owner;
}

static gboolean
pred_name_has_owner (gpointer data)
{
  return name_has_owner (data);
}

static void
test_extension_capability_and_teardown (void)
{
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *user = g_build_filename (tmp, "user", NULL);
  g_autoptr (GFile) dir = NULL;
  g_autoptr (GError) error = NULL;
  GFile *dirs[1];
  ShellLuaExtensionManager *manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  ShellLuaExtension *with;

  g_assert_cmpint (g_mkdir_with_parents (user, 0700), ==, 0);
  dir = g_file_new_for_path (user);
  dirs[0] = dir;
  shell_lua_extension_manager_set_search_dirs (manager, dirs, 1);

  write_ext (user, "dbus@test", ", \"capabilities\": [\"dbus\"]",
             "local shell = require('shell')\n"
             "return { enable = function()\n"
             "  local bus = shell.dbus.session()\n"
             "  calls = 0\n"
             "  slow = bus:proxy{ name = 'org.example.TestService', path = '/org/example/Test', interface = 'org.example.Test' }\n"
             "  slow:call{ method = 'Slow', timeout_ms = 30000, on_reply = function() calls = calls + 1 end,\n"
             "             on_error = function() calls = calls + 1 end }\n"
             "  sub = slow:on_signal('Ping', function() calls = calls + 1 end)\n"
             "  watch = bus:watch_name{ name = 'org.example.TestService', on_appeared = function() calls = calls + 1 end,\n"
             "                          on_vanished = function() calls = calls + 1 end }\n"
             "  owner = bus:own_name{ name = 'org.example.Teardown' }\n"
             "  obj = bus:export{ path = '/org/example/Teardown', interface_xml = [[<node><interface name='org.example.T'>\n"
             "    <method name='M'/></interface></node>]], methods = { M = function() calls = calls + 1 end } }\n"
             "end }");
  shell_lua_extension_manager_scan (manager);
  with = shell_lua_extension_manager_lookup (manager, "dbus@test");
  g_assert_nonnull (with);

  /* without the capability, enable() fails on the first call */
  write_ext (user, "nodbus2@test", "",
             "local shell = require('shell')\n"
             "return { enable = function() shell.dbus.session() end }");
  shell_lua_extension_manager_scan (manager);
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[nodbus2@test] *requires the \"dbus\" capability*");
  g_assert_false (shell_lua_extension_manager_enable (manager, "nodbus2@test", &error));
  g_assert_nonnull (error);
  g_assert_nonnull (strstr (error->message, "requires the \"dbus\" capability"));
  g_clear_error (&error);
  g_test_assert_expected_messages ();

  service_start ();
  g_assert_true (shell_lua_extension_manager_enable (manager, "dbus@test", &error));
  g_assert_no_error (error);

  g_assert_true (tu_wait_for (pred_slow_pending, NULL, 3000));
  g_assert_true (tu_wait_for (pred_name_has_owner, (gpointer) "org.example.Teardown", 3000));
  tu_spin (100);

  {
    ShellLuaState *state = shell_lua_extension_get_lua_state (with);
    gint64 before = tu_eval_int (state, "calls");

    g_assert_cmpint (before, >=, 1);   /* the watch saw the service */

    g_assert_true (shell_lua_extension_manager_disable (manager, "dbus@test", &error));
    g_assert_no_error (error);
  }

  /* nothing of the extension is left: the name is released, and the late
   * reply and the signals find nobody to call (and must not crash) */
  tu_spin (100);
  service_reply_slow ("after disable");
  service_emit ("Ping", g_variant_new ("(s)", "after disable"));
  tu_spin (100);
  g_assert_false (pred_name_has_owner ((gpointer) "org.example.Teardown"));

  service_stop ();
  g_object_unref (manager);
  tu_rm_rf (tmp);
}


/* ------------------------------------------------------------------------ */
/* The fixture of the conformance test (tests/api/fixtures/dbus@test)        */
/* ------------------------------------------------------------------------ */

/* A stand-in for the shell.test API of src/shell-test-runner.c */
typedef struct {
  char     *name;
  gboolean  ok;
  char     *message;
} Report;

static GPtrArray  *reports = NULL;
static GHashTable *done_labels = NULL;

static void
report_free (gpointer data)
{
  Report *r = data;

  g_free (r->name);
  g_free (r->message);
  g_free (r);
}

static int
cf_report (lua_State *L)
{
  Report *r = g_new0 (Report, 1);

  r->name = g_strdup (luaL_checkstring (L, 1));
  r->ok = lua_toboolean (L, 2);
  r->message = g_strdup (luaL_optstring (L, 3, NULL));
  g_ptr_array_add (reports, r);
  return 0;
}

static int
cf_done (lua_State *L)
{
  g_hash_table_add (done_labels, g_strdup (luaL_optstring (L, 1, "main")));
  return 0;
}

static void
test_api_register (ShellLuaState *state, lua_State *L)
{
  lua_newtable (L);
  lua_pushcfunction (L, cf_report);
  lua_setfield (L, -2, "report");
  lua_pushcfunction (L, cf_done);
  lua_setfield (L, -2, "done");
  lua_setfield (L, -2, "test");
}

static gboolean
pred_done (gpointer data)
{
  return g_hash_table_contains (done_labels, data);
}

/* The notification service of the shell, as far as the fixture asks */
static const char notifications_xml[] =
  "<node><interface name='org.freedesktop.Notifications'>"
  "  <method name='GetServerInformation'>"
  "    <arg type='s' direction='out'/><arg type='s' direction='out'/>"
  "    <arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
  "  <method name='GetCapabilities'><arg type='as' direction='out'/></method>"
  "</interface></node>";

static void
notifications_method_call (GDBusConnection       *connection,
                           const char            *sender,
                           const char            *object_path,
                           const char            *interface_name,
                           const char            *method_name,
                           GVariant              *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer               user_data)
{
  const char *caps[] = { "body", "actions", NULL };

  if (g_str_equal (method_name, "GetServerInformation"))
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(ssss)", "Test Shell", "gnome-lua", "1", "1.2"));
  else
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(^as)", caps));
}

static const GDBusInterfaceVTable notifications_vtable = { notifications_method_call, NULL, NULL, { 0 } };

static gboolean notifications_acquired = FALSE;

static void
notifications_bus_acquired (GDBusConnection *connection,
                            const char      *name,
                            gpointer         user_data)
{
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (notifications_xml, NULL);

  g_dbus_connection_register_object (connection, "/org/freedesktop/Notifications",
                                     node->interfaces[0], &notifications_vtable, NULL, NULL, NULL);
  g_dbus_node_info_unref (node);
}

static void
notifications_name_acquired (GDBusConnection *connection,
                             const char      *name,
                             gpointer         user_data)
{
  notifications_acquired = TRUE;
}

static gboolean
pred_notifications (gpointer data)
{
  return notifications_acquired;
}

static void
check_reports (const char *what,
               guint       minimum)
{
  GString *failures = g_string_new (NULL);
  guint i;

  for (i = 0; i < reports->len; i++)
    {
      Report *r = g_ptr_array_index (reports, i);

      if (!r->ok)
        g_string_append_printf (failures, "\n  %s: %s", r->name, r->message != NULL ? r->message : "failed");
    }
  if (failures->len > 0)
    g_error ("%s: checks failed (of %u):%s", what, reports->len, failures->str);
  g_assert_cmpuint (reports->len, >=, minimum);
  g_string_free (failures, TRUE);
}

static void
test_fixture (void)
{
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *user = g_build_filename (tmp, "user", NULL);
  g_autofree char *src = g_build_filename (g_getenv ("G_TEST_SRCDIR"), "..", "..", "..", "tests", "api",
                                           "fixtures", NULL);
  g_autoptr (GFile) dir = NULL;
  g_autoptr (GError) error = NULL;
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellLuaExtensionManager *manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  const char *uuids[] = { "dbus@test", "dbus-nocap@test" };
  GFile *dirs[1];
  guint notifications_id;
  guint cycle, i;

  for (i = 0; i < G_N_ELEMENTS (uuids); i++)
    {
      g_autofree char *metadata_path = g_build_filename (src, uuids[i], "metadata.json", NULL);
      g_autofree char *lua_path = g_build_filename (src, uuids[i], "extension.lua", NULL);
      g_autofree char *metadata = NULL;
      g_autofree char *lua = NULL;
      g_autofree char *dest = g_build_filename (user, uuids[i], NULL);

      g_assert_true (g_file_get_contents (metadata_path, &metadata, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (g_file_get_contents (lua_path, &lua, NULL, &error));
      g_assert_no_error (error);
      tu_write_file (dest, "metadata.json", metadata);
      tu_write_file (dest, "extension.lua", lua);
    }
  dir = g_file_new_for_path (user);
  dirs[0] = dir;
  shell_lua_extension_manager_set_search_dirs (manager, dirs, 1);
  shell_lua_extension_manager_scan (manager);
  g_assert_nonnull (shell_lua_extension_manager_lookup (manager, "dbus@test"));

  reports = g_ptr_array_new_with_free_func (report_free);
  done_labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  shell_hook_registry_declare (registry, "test.step", "step:s", FALSE, "A step of the script");

  notifications_id = g_bus_own_name (G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
                                     G_BUS_NAME_OWNER_FLAGS_NONE, notifications_bus_acquired,
                                     notifications_name_acquired, NULL, NULL, NULL);
  g_assert_true (tu_wait_for (pred_notifications, NULL, 5000));

  /* without the capability, nothing of shell.dbus works */
  g_assert_true (shell_lua_extension_manager_enable (manager, "dbus-nocap@test", &error));
  g_assert_no_error (error);
  g_assert_true (tu_wait_for (pred_done, (gpointer) "main", 5000));
  check_reports ("dbus-nocap@test", 2);
  g_assert_true (shell_lua_extension_manager_disable (manager, "dbus-nocap@test", &error));
  g_ptr_array_set_size (reports, 0);
  g_hash_table_remove_all (done_labels);

  /* the conformance fixture, twice */
  for (cycle = 0; cycle < 2; cycle++)
    {
      ShellLuaExtension *ext = shell_lua_extension_manager_lookup (manager, "dbus@test");
      GVariantBuilder step;
      guint n_before;

      g_assert_true (shell_lua_extension_manager_enable (manager, "dbus@test", &error));
      g_assert_no_error (error);
      g_assert_true (tu_wait_for (pred_done, (gpointer) "main", 20000));
      check_reports ("dbus@test", 14);
      g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);

      /* the errors it makes on purpose: one each, logged with the extension's id */
      g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[dbus@test] *secret-traceback-marker*");
      g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[dbus@test] *returned 2 values*");
      g_variant_builder_init (&step, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&step, "{sv}", "step", g_variant_new_string ("method-errors"));
      shell_hook_registry_emit_after (registry, "test.step", g_variant_builder_end (&step));
      g_assert_true (tu_wait_for (pred_done, (gpointer) "method-errors", 10000));
      g_test_assert_expected_messages ();
      check_reports ("dbus@test, method-errors", 16);
      g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 2);

      g_assert_true (shell_lua_extension_manager_disable (manager, "dbus@test", &error));
      g_assert_no_error (error);

      /* nothing runs any more, and the name is gone */
      n_before = reports->len;
      tu_spin (300);
      g_assert_cmpuint (reports->len, ==, n_before);
      g_assert_false (name_has_owner ("org.example.ApiTest"));

      g_ptr_array_set_size (reports, 0);
      g_hash_table_remove_all (done_labels);
    }

  g_bus_unown_name (notifications_id);
  g_object_unref (manager);
  g_clear_pointer (&reports, g_ptr_array_unref);
  g_clear_pointer (&done_labels, g_hash_table_unref);
  tu_rm_rf (tmp);
}

/* ------------------------------------------------------------------------ */

int
main (int argc, char **argv)
{
  GTestDBus *bus;
  int result;

  g_test_init (&argc, &argv, NULL);

  shell_lua_api_register ("zz-test", test_api_register);

  /* the system bus must not exist */
  g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/shell-lua-test-system-bus", TRUE);
  bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);

  g_test_add_func ("/shell-lua/dbus/entry-points", test_entry_points);
  g_test_add_func ("/shell-lua/dbus/capability-gate", test_capability_gate);
  g_test_add_func ("/shell-lua/dbus/misuse", test_misuse);
  g_test_add_func ("/shell-lua/dbus/call/reply", test_call_reply);
  g_test_add_func ("/shell-lua/dbus/call/errors", test_call_errors);
  g_test_add_func ("/shell-lua/dbus/call/cancel", test_call_cancel);
  g_test_add_func ("/shell-lua/dbus/mapping", test_mapping);
  g_test_add_func ("/shell-lua/dbus/properties", test_properties);
  g_test_add_func ("/shell-lua/dbus/signals", test_signals);
  g_test_add_func ("/shell-lua/dbus/watch-name", test_watch_name);
  g_test_add_func ("/shell-lua/dbus/own-name-and-export", test_own_and_export);
  g_test_add_func ("/shell-lua/dbus/export-validation", test_export_validation);
  g_test_add_func ("/shell-lua/dbus/extension-teardown", test_extension_capability_and_teardown);
  g_test_add_func ("/shell-lua/dbus/fixture", test_fixture);

  result = g_test_run ();

  g_test_dbus_down (bus);
  g_object_unref (bus);
  return result;
}
