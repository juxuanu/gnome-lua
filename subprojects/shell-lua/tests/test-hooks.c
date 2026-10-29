/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellHookRegistry (C API) and shell.hooks (Lua API).
 */

#include "test-utils.h"

#include "shell-hooks.h"

/* ------------------------------------------------------------------------ */
/* A recording listener                                                      */
/* ------------------------------------------------------------------------ */

typedef struct _Recorder Recorder;

struct _Recorder {
  const char       *tag;
  GString          *log;            /* shared: tags in call order */
  ShellHookVerdict  verdict;
  const char       *amend_key;      /* if set, amend {key: value} */
  gint64            amend_value;
  guint             calls;
  guint             destroyed;
  gboolean          amend_was_null;
  ShellHookPhase    last_phase;
  GVariant         *last_payload;   /* ref */
  char             *last_name;

  /* optional side effects, run from inside the listener */
  ShellHookRegistry *registry;
  guint              disconnect_id;      /* disconnected when called */
  guint              disconnect_self_id; /* ditto */
  const char        *connect_name;       /* connect a new listener when called */
  Recorder          *connect_to;
  guint              connected_id;
};

static ShellHookVerdict
recorder_listener (const char     *name,
                   ShellHookPhase  phase,
                   GVariant       *payload,
                   GVariantDict   *amend,
                   gpointer        user_data)
{
  Recorder *r = user_data;

  r->calls++;
  g_string_append (r->log, r->tag);
  r->last_phase = phase;
  r->amend_was_null = (amend == NULL);
  g_free (r->last_name);
  r->last_name = g_strdup (name);
  g_clear_pointer (&r->last_payload, g_variant_unref);
  r->last_payload = g_variant_ref (payload);

  if (amend != NULL && r->amend_key != NULL)
    g_variant_dict_insert (amend, r->amend_key, "x", r->amend_value);

  if (r->disconnect_id != 0)
    shell_hook_registry_disconnect (r->registry, r->disconnect_id);
  if (r->disconnect_self_id != 0)
    shell_hook_registry_disconnect (r->registry, r->disconnect_self_id);
  if (r->connect_to != NULL && r->connected_id == 0)
    r->connected_id = shell_hook_registry_connect (r->registry, r->connect_name, phase,
                                                   recorder_listener, r->connect_to, NULL);

  return r->verdict;
}

static void
recorder_destroy (gpointer data)
{
  Recorder *r = data;

  r->destroyed++;
}

static void
recorder_init (Recorder *r, const char *tag, GString *log)
{
  memset (r, 0, sizeof *r);
  r->tag = tag;
  r->log = log;
  r->verdict = SHELL_HOOK_VERDICT_CONTINUE;
}

static void
recorder_clear (Recorder *r)
{
  g_clear_pointer (&r->last_payload, g_variant_unref);
  g_free (r->last_name);
}

static guint
connect_recorder (ShellHookRegistry *reg, const char *name, ShellHookPhase phase, Recorder *r)
{
  guint id = shell_hook_registry_connect (reg, name, phase, recorder_listener, r, recorder_destroy);

  g_assert_cmpuint (id, !=, 0);
  return id;
}

static gint64
lookup_int (GVariant *dict, const char *key)
{
  gint64 value = -1;

  g_assert_nonnull (dict);
  if (!g_variant_lookup (dict, key, "x", &value))
    g_error ("key '%s' missing or not an int64 in %s", key, g_variant_print (dict, TRUE));
  return value;
}

/* ------------------------------------------------------------------------ */
/* Declarations                                                              */
/* ------------------------------------------------------------------------ */

static void
on_declared (ShellHookRegistry *reg, const char *name, GString *log)
{
  g_string_append_printf (log, "%s;", name);
}

static void
test_declare_and_query (void)
{
  ShellHookRegistry *reg = g_object_new (SHELL_TYPE_HOOK_REGISTRY, NULL);
  GString *declared = g_string_new (NULL);
  g_auto (GStrv) names = NULL;

  g_signal_connect (reg, "declared", G_CALLBACK (on_declared), declared);

  g_assert_false (shell_hook_registry_is_declared (reg, "window.focused"));
  g_assert_false (shell_hook_registry_is_cancellable (reg, "window.focused"));
  g_assert_null (shell_hook_registry_get_doc (reg, "window.focused"));
  g_assert_null (shell_hook_registry_get_schema (reg, "window.focused"));
  names = shell_hook_registry_list (reg);
  g_assert_cmpuint (g_strv_length (names), ==, 0);
  g_clear_pointer (&names, g_strfreev);

  /* declared out of order; listed sorted */
  shell_hook_registry_declare (reg, "window.focused", "window:o title:s", FALSE, "A window got focus");
  shell_hook_registry_declare (reg, "overview.showing", NULL, TRUE, NULL);
  shell_hook_registry_declare (reg, "a1.b2-c3", "", FALSE, NULL);

  g_assert_true (shell_hook_registry_is_declared (reg, "window.focused"));
  g_assert_true (shell_hook_registry_is_declared (reg, "overview.showing"));
  g_assert_false (shell_hook_registry_is_cancellable (reg, "window.focused"));
  g_assert_true (shell_hook_registry_is_cancellable (reg, "overview.showing"));
  g_assert_cmpstr (shell_hook_registry_get_doc (reg, "window.focused"), ==, "A window got focus");
  g_assert_null (shell_hook_registry_get_doc (reg, "overview.showing"));
  g_assert_cmpstr (shell_hook_registry_get_schema (reg, "window.focused"), ==, "window:o title:s");
  g_assert_cmpstr (shell_hook_registry_get_schema (reg, "overview.showing"), ==, "");
  g_assert_false (shell_hook_registry_is_declared (reg, "window.unfocused"));

  names = shell_hook_registry_list (reg);
  g_assert_cmpuint (g_strv_length (names), ==, 3);
  g_assert_cmpstr (names[0], ==, "a1.b2-c3");
  g_assert_cmpstr (names[1], ==, "overview.showing");
  g_assert_cmpstr (names[2], ==, "window.focused");
  g_assert_cmpstr (declared->str, ==, "window.focused;overview.showing;a1.b2-c3;");

  /* redeclaring identically is a no-op (no signal, no critical) */
  shell_hook_registry_declare (reg, "window.focused", "window:o title:s", FALSE, "A window got focus");
  shell_hook_registry_declare (reg, "overview.showing", "", TRUE, NULL);
  g_assert_cmpstr (declared->str, ==, "window.focused;overview.showing;a1.b2-c3;");

  /* a missing doc can be filled in later, an existing one is kept */
  shell_hook_registry_declare (reg, "overview.showing", "", TRUE, "Late doc");
  g_assert_cmpstr (shell_hook_registry_get_doc (reg, "overview.showing"), ==, "Late doc");
  shell_hook_registry_declare (reg, "overview.showing", "", TRUE, "Other doc");
  g_assert_cmpstr (shell_hook_registry_get_doc (reg, "overview.showing"), ==, "Late doc");

  /* redeclaring differently is a programmer error and changes nothing */
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Hook 'overview.showing' redeclared with a different cancellable flag");
  shell_hook_registry_declare (reg, "overview.showing", "", FALSE, NULL);
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Hook 'window.focused' redeclared with a different schema");
  shell_hook_registry_declare (reg, "window.focused", "other", FALSE, NULL);
  g_test_assert_expected_messages ();
  g_assert_true (shell_hook_registry_is_cancellable (reg, "overview.showing"));
  g_assert_cmpstr (shell_hook_registry_get_schema (reg, "window.focused"), ==, "window:o title:s");

  g_string_free (declared, TRUE);
  g_object_unref (reg);
}

static void
test_declare_bad_names (void)
{
  ShellHookRegistry *reg = g_object_new (SHELL_TYPE_HOOK_REGISTRY, NULL);
  static const char *const bad[] = {
    "", "nodot", "Foo.bar", "foo.Bar", ".x", "x.", "x..y", "1x.y", "x.1y", "x_y.z", "x.y_z",
    "-x.y", "x.-y", "x y.z", "x.y z", "window.focused!", "über.x",
  };
  g_auto (GStrv) names = NULL;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Invalid hook name *");
      shell_hook_registry_declare (reg, bad[i], "", FALSE, NULL);
      g_assert_false (shell_hook_registry_is_declared (reg, bad[i]));
    }
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Invalid hook name '(null)'*");
  shell_hook_registry_declare (reg, NULL, "", FALSE, NULL);
  g_test_assert_expected_messages ();

  names = shell_hook_registry_list (reg);
  g_assert_cmpuint (g_strv_length (names), ==, 0);

  /* good ones */
  shell_hook_registry_declare (reg, "shell.started", "", FALSE, NULL);
  shell_hook_registry_declare (reg, "a.b", "", FALSE, NULL);
  shell_hook_registry_declare (reg, "session.mode-changed", "mode:s", FALSE, NULL);
  g_assert_true (shell_hook_registry_is_declared (reg, "shell.started"));
  g_assert_true (shell_hook_registry_is_declared (reg, "a.b"));
  g_assert_true (shell_hook_registry_is_declared (reg, "session.mode-changed"));

  g_object_unref (reg);
}

static void
test_default_registry (void)
{
  ShellHookRegistry *a = shell_hook_registry_get_default ();

  g_assert_true (SHELL_IS_HOOK_REGISTRY (a));
  g_assert_true (a == shell_hook_registry_get_default ());
}

static void
test_enums (void)
{
  GEnumClass *verdicts = g_type_class_ref (SHELL_TYPE_HOOK_VERDICT);
  GEnumClass *phases = g_type_class_ref (SHELL_TYPE_HOOK_PHASE);

  g_assert_cmpstr (g_enum_get_value (verdicts, SHELL_HOOK_VERDICT_CANCEL)->value_nick, ==, "cancel");
  g_assert_cmpstr (g_enum_get_value (verdicts, SHELL_HOOK_VERDICT_CONTINUE)->value_nick, ==, "continue");
  g_assert_cmpstr (g_enum_get_value (phases, SHELL_HOOK_PHASE_BEFORE)->value_nick, ==, "before");
  g_assert_cmpstr (g_enum_get_value (phases, SHELL_HOOK_PHASE_AFTER)->value_nick, ==, "after");
  g_type_class_unref (verdicts);
  g_type_class_unref (phases);
}

/* ------------------------------------------------------------------------ */
/* Connecting                                                                */
/* ------------------------------------------------------------------------ */

static void
test_connect_refused (void)
{
  ShellHookRegistry *reg = g_object_new (SHELL_TYPE_HOOK_REGISTRY, NULL);
  GString *log = g_string_new (NULL);
  Recorder r, r2;
  guint id, a, b;

  recorder_init (&r, "r", log);
  shell_hook_registry_declare (reg, "test.plain", "", FALSE, NULL);

  /* BEFORE on a hook that cannot be cancelled */
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "Hook 'test.plain' is not cancellable*");
  id = shell_hook_registry_connect (reg, "test.plain", SHELL_HOOK_PHASE_BEFORE,
                                    recorder_listener, &r, recorder_destroy);
  g_assert_cmpuint (id, ==, 0);

  /* undeclared and invalid names */
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "Cannot connect to undeclared hook 'test.missing'");
  id = shell_hook_registry_connect (reg, "test.missing", SHELL_HOOK_PHASE_AFTER,
                                    recorder_listener, &r, recorder_destroy);
  g_assert_cmpuint (id, ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "Cannot connect to hook with invalid name 'Bad Name'");
  id = shell_hook_registry_connect (reg, "Bad Name", SHELL_HOOK_PHASE_AFTER,
                                    recorder_listener, &r, recorder_destroy);
  g_assert_cmpuint (id, ==, 0);
  g_test_assert_expected_messages ();

  /* a refused connection never calls the destroy notify: the caller keeps ownership */
  g_assert_cmpuint (r.destroyed, ==, 0);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 0);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_BEFORE), ==, 0);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.missing", SHELL_HOOK_PHASE_AFTER), ==, 0);

  /* ids are distinct and non-zero */
  recorder_init (&r2, "r2", log);
  a = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r);
  b = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r2);
  g_assert_cmpuint (a, !=, b);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 2);

  /* the registry releases the listeners it still holds when it goes away */
  g_object_unref (reg);
  g_assert_cmpuint (r.destroyed, ==, 1);
  g_assert_cmpuint (r2.destroyed, ==, 1);
  recorder_clear (&r2);
  recorder_clear (&r);
  g_string_free (log, TRUE);
}

/* ------------------------------------------------------------------------ */
/* Emission                                                                  */
/* ------------------------------------------------------------------------ */

static ShellHookRegistry *
registry_with (const char *name, gboolean cancellable)
{
  ShellHookRegistry *reg = g_object_new (SHELL_TYPE_HOOK_REGISTRY, NULL);

  shell_hook_registry_declare (reg, name, "title:s n:x", cancellable, NULL);
  return reg;
}

static void
test_emit_before_order_and_amendments (void)
{
  ShellHookRegistry *reg = registry_with ("test.showing", TRUE);
  GString *log = g_string_new (NULL);
  Recorder r1, r2, r3, after;
  GVariant *payload = g_variant_ref_sink (g_variant_new_parsed ("{'title': <'t'>}"));
  GVariant *amendments = NULL;

  recorder_init (&r1, "1", log);
  recorder_init (&r2, "2", log);
  recorder_init (&r3, "3", log);
  recorder_init (&after, "A", log);
  r1.amend_key = "a";  r1.amend_value = 1;
  r2.amend_key = "a";  r2.amend_value = 2;     /* later listeners win on conflicts */
  r3.amend_key = "c";  r3.amend_value = 3;
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &r1);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &r2);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_AFTER, &after);   /* not run by emit_before */
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &r3);

  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.showing", payload, &amendments),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_cmpstr (log->str, ==, "123");
  g_assert_nonnull (amendments);
  g_assert_true (g_variant_is_of_type (amendments, G_VARIANT_TYPE_VARDICT));
  g_assert_cmpint (lookup_int (amendments, "a"), ==, 2);
  g_assert_cmpint (lookup_int (amendments, "c"), ==, 3);
  g_assert_cmpuint (g_variant_n_children (amendments), ==, 2);
  g_variant_unref (amendments);

  /* listeners see the payload and the phase; BEFORE listeners get a dictionary */
  g_assert_cmpstr (r1.last_name, ==, "test.showing");
  g_assert_cmpint (r1.last_phase, ==, SHELL_HOOK_PHASE_BEFORE);
  g_assert_false (r1.amend_was_null);
  g_assert_true (g_variant_equal (r1.last_payload, payload));

  /* a NULL payload is an empty a{sv}; no amendments gives NULL; the out
   * parameter is optional */
  r1.amend_key = r2.amend_key = r3.amend_key = NULL;
  g_string_truncate (log, 0);
  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.showing", NULL, &amendments),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_null (amendments);
  g_assert_cmpstr (log->str, ==, "123");
  g_assert_true (g_variant_is_of_type (r1.last_payload, G_VARIANT_TYPE_VARDICT));
  g_assert_cmpuint (g_variant_n_children (r1.last_payload), ==, 0);
  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.showing", payload, NULL),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);

  /* no listeners at all */
  {
    ShellHookRegistry *empty = registry_with ("test.showing", TRUE);

    amendments = NULL;
    g_assert_cmpint (shell_hook_registry_emit_before (empty, "test.showing", NULL, &amendments),
                     ==, SHELL_HOOK_VERDICT_CONTINUE);
    g_assert_null (amendments);
    g_object_unref (empty);
  }

  g_variant_unref (payload);
  recorder_clear (&r1);
  recorder_clear (&r2);
  recorder_clear (&r3);
  recorder_clear (&after);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_emit_before_cancel (void)
{
  ShellHookRegistry *reg = registry_with ("test.showing", TRUE);
  GString *log = g_string_new (NULL);
  Recorder r1, r2, r3;
  GVariant *amendments = NULL;

  recorder_init (&r1, "1", log);
  recorder_init (&r2, "2", log);
  recorder_init (&r3, "3", log);
  r1.amend_key = "a";  r1.amend_value = 1;
  r2.verdict = SHELL_HOOK_VERDICT_CANCEL;
  r3.amend_key = "c";  r3.amend_value = 3;
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &r1);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &r2);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &r3);

  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.showing", NULL, &amendments),
                   ==, SHELL_HOOK_VERDICT_CANCEL);
  g_assert_cmpstr (log->str, ==, "12");      /* the first CANCEL stops the rest */
  g_assert_cmpuint (r3.calls, ==, 0);
  g_assert_nonnull (amendments);
  g_assert_cmpint (lookup_int (amendments, "a"), ==, 1);
  g_assert_false (g_variant_lookup (amendments, "c", "x", NULL));
  g_variant_unref (amendments);

  recorder_clear (&r1);
  recorder_clear (&r2);
  recorder_clear (&r3);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_emit_after (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  GString *log = g_string_new (NULL);
  Recorder r1, r2;
  GVariant *payload = g_variant_ref_sink (g_variant_new_parsed ("{'n': <int64 5>}"));

  recorder_init (&r1, "1", log);
  recorder_init (&r2, "2", log);
  r1.verdict = SHELL_HOOK_VERDICT_CANCEL;   /* ignored in the AFTER phase */
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r1);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r2);

  shell_hook_registry_emit_after (reg, "test.plain", payload);
  g_assert_cmpstr (log->str, ==, "12");
  g_assert_cmpint (r1.last_phase, ==, SHELL_HOOK_PHASE_AFTER);
  g_assert_true (r1.amend_was_null);
  g_assert_cmpint (lookup_int (r2.last_payload, "n"), ==, 5);

  /* NULL payload -> empty dictionary; floating payloads are consumed */
  shell_hook_registry_emit_after (reg, "test.plain", NULL);
  g_assert_cmpuint (g_variant_n_children (r1.last_payload), ==, 0);
  shell_hook_registry_emit_after (reg, "test.plain", g_variant_new_parsed ("{'n': <int64 6>}"));
  g_assert_cmpint (lookup_int (r1.last_payload, "n"), ==, 6);
  g_assert_cmpuint (r1.calls, ==, 3);

  g_variant_unref (payload);
  recorder_clear (&r1);
  recorder_clear (&r2);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_emit_misuse_warns (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  GString *log = g_string_new (NULL);
  Recorder r;
  GVariant *amendments = (GVariant *) 0x1;
  GVariant *bad = g_variant_ref_sink (g_variant_new_int32 (1));

  recorder_init (&r, "r", log);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "Emitting undeclared hook 'test.missing'");
  shell_hook_registry_emit_after (reg, "test.missing", NULL);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "Emitting undeclared hook 'test.missing'");
  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.missing", NULL, &amendments),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_null (amendments);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "Hook 'test.plain' is not cancellable; use emit_after()");
  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.plain", NULL, NULL),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);

  /* a payload that is not a{sv} */
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "*assertion*failed*");
  shell_hook_registry_emit_after (reg, "test.plain", bad);

  g_test_assert_expected_messages ();
  g_assert_cmpuint (r.calls, ==, 0);

  g_variant_unref (bad);
  recorder_clear (&r);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

typedef struct {
  GString *log;
  guint    count;
  char    *name;
  int      phase;
  GVariant *payload;
} EmittedProbe;

static void
on_emitted (ShellHookRegistry *reg, const char *name, ShellHookPhase phase, GVariant *payload,
            EmittedProbe *probe)
{
  probe->count++;
  g_free (probe->name);
  probe->name = g_strdup (name);
  probe->phase = phase;
  g_clear_pointer (&probe->payload, g_variant_unref);
  probe->payload = g_variant_ref (payload);
  g_string_append (probe->log, "E");
}

static void
test_emitted_signal (void)
{
  ShellHookRegistry *reg = registry_with ("test.showing", TRUE);
  GString *log = g_string_new (NULL);
  EmittedProbe probe = { log, 0, NULL, -1, NULL };
  Recorder before, after;
  GVariant *payload = g_variant_ref_sink (g_variant_new_parsed ("{'title': <'t'>}"));

  recorder_init (&before, "b", log);
  recorder_init (&after, "a", log);
  g_signal_connect (reg, "emitted", G_CALLBACK (on_emitted), &probe);

  /* emitted even with no listeners, before the listeners run */
  shell_hook_registry_emit_after (reg, "test.showing", payload);
  g_assert_cmpuint (probe.count, ==, 1);
  g_assert_cmpstr (probe.name, ==, "test.showing");
  g_assert_cmpint (probe.phase, ==, SHELL_HOOK_PHASE_AFTER);
  g_assert_true (g_variant_equal (probe.payload, payload));

  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &before);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_AFTER, &after);
  g_string_truncate (log, 0);
  shell_hook_registry_emit_before (reg, "test.showing", payload, NULL);
  g_assert_cmpuint (probe.count, ==, 2);
  g_assert_cmpint (probe.phase, ==, SHELL_HOOK_PHASE_BEFORE);
  g_assert_true (g_variant_equal (probe.payload, payload));
  shell_hook_registry_emit_after (reg, "test.showing", payload);
  g_assert_cmpuint (probe.count, ==, 3);
  g_assert_cmpstr (log->str, ==, "EbEa");

  /* NULL payload shows up as an empty a{sv} */
  shell_hook_registry_emit_after (reg, "test.showing", NULL);
  g_assert_true (g_variant_is_of_type (probe.payload, G_VARIANT_TYPE_VARDICT));
  g_assert_cmpuint (g_variant_n_children (probe.payload), ==, 0);

  g_free (probe.name);
  g_clear_pointer (&probe.payload, g_variant_unref);
  g_variant_unref (payload);
  recorder_clear (&before);
  recorder_clear (&after);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

/* ------------------------------------------------------------------------ */
/* Disconnecting                                                             */
/* ------------------------------------------------------------------------ */

static void
test_disconnect_during_emission (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  GString *log = g_string_new (NULL);
  Recorder r1, r2, r3;
  guint id1, id2, id3;

  recorder_init (&r1, "1", log);
  recorder_init (&r2, "2", log);
  recorder_init (&r3, "3", log);
  id1 = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r1);
  id2 = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r2);
  id3 = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r3);

  /* r1 removes r2 (not yet called) and itself */
  r1.registry = reg;
  r1.disconnect_id = id2;
  r1.disconnect_self_id = id1;

  shell_hook_registry_emit_after (reg, "test.plain", NULL);
  g_assert_cmpstr (log->str, ==, "13");            /* r2 was skipped */
  /* the destroy notifies ran, once each, when the emission let go */
  g_assert_cmpuint (r1.destroyed, ==, 1);
  g_assert_cmpuint (r2.destroyed, ==, 1);
  g_assert_cmpuint (r3.destroyed, ==, 0);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 1);

  /* later emissions only reach r3; disconnecting twice, 0 and unknown ids are harmless */
  g_string_truncate (log, 0);
  shell_hook_registry_emit_after (reg, "test.plain", NULL);
  g_assert_cmpstr (log->str, ==, "3");
  shell_hook_registry_disconnect (reg, id1);
  shell_hook_registry_disconnect (reg, id2);
  shell_hook_registry_disconnect (reg, 0);
  shell_hook_registry_disconnect (reg, 123456);
  g_assert_cmpuint (r1.destroyed, ==, 1);
  g_assert_cmpuint (r2.destroyed, ==, 1);

  /* outside an emission the notify runs immediately */
  shell_hook_registry_disconnect (reg, id3);
  g_assert_cmpuint (r3.destroyed, ==, 1);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 0);

  recorder_clear (&r1);
  recorder_clear (&r2);
  recorder_clear (&r3);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_connect_during_emission (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  GString *log = g_string_new (NULL);
  Recorder r1, late;

  recorder_init (&r1, "1", log);
  recorder_init (&late, "L", log);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r1);
  r1.registry = reg;
  r1.connect_name = "test.plain";
  r1.connect_to = &late;

  /* a listener connected from inside a listener is not part of that emission */
  shell_hook_registry_emit_after (reg, "test.plain", NULL);
  g_assert_cmpstr (log->str, ==, "1");
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 2);
  shell_hook_registry_emit_after (reg, "test.plain", NULL);
  g_assert_cmpstr (log->str, ==, "11L");

  recorder_clear (&r1);
  recorder_clear (&late);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_destroy_on_finalize (void)
{
  ShellHookRegistry *reg = registry_with ("test.showing", TRUE);
  GString *log = g_string_new (NULL);
  Recorder before, after, gone;
  guint id;

  recorder_init (&before, "b", log);
  recorder_init (&after, "a", log);
  recorder_init (&gone, "g", log);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &before);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_AFTER, &after);
  id = connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_AFTER, &gone);
  shell_hook_registry_disconnect (reg, id);
  g_assert_cmpuint (gone.destroyed, ==, 1);

  g_object_unref (reg);
  g_assert_cmpuint (before.destroyed, ==, 1);
  g_assert_cmpuint (after.destroyed, ==, 1);
  g_assert_cmpuint (gone.destroyed, ==, 1);

  recorder_clear (&before);
  recorder_clear (&after);
  recorder_clear (&gone);
  g_string_free (log, TRUE);
}

/* ------------------------------------------------------------------------ */
/* Replay                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  guint   calls;          /* how often the registry asked */
  int     n_items;        /* how many payloads it returns, {'n': i} */
  gboolean floating;      /* return floating variants */
  gboolean any_name;      /* the hook is not test.plain */
  ShellHookRegistry *connect_in_replay;   /* connect another listener when asked */
  Recorder          *late;
} ReplaySource;

static GPtrArray *
replay_items (ShellHookRegistry *reg,
              const char        *name,
              gpointer           user_data)
{
  ReplaySource *src = user_data;
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  int i;

  src->calls++;
  if (!src->any_name)
    g_assert_cmpstr (name, ==, "test.plain");
  for (i = 1; i <= src->n_items; i++)
    {
      GVariant *v = g_variant_new_parsed ("{'n': <int64 %x>}", (gint64) i);

      g_ptr_array_add (items, src->floating ? v : g_variant_ref_sink (v));
    }
  if (src->connect_in_replay != NULL)
    {
      connect_recorder (src->connect_in_replay, "test.plain", SHELL_HOOK_PHASE_AFTER, src->late);
      src->connect_in_replay = NULL;
    }
  return items;
}

static void
test_replay_basic (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 3 };
  GString *log = g_string_new (NULL);
  Recorder old, fresh;

  recorder_init (&old, "o", log);
  recorder_init (&fresh, "f", log);

  /* a listener that was there before the replay was set gets none */
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &old);
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  tu_spin (20);
  g_assert_cmpuint (old.calls, ==, 0);
  g_assert_cmpuint (src.calls, ==, 0);

  /* never synchronously inside connect () */
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &fresh);
  g_assert_cmpuint (fresh.calls, ==, 0);
  g_assert_cmpuint (src.calls, ==, 0);

  /* from an idle, to the new listener only, one call per payload in order */
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_cmpuint (src.calls, ==, 1);
  g_assert_cmpuint (fresh.calls, ==, 3);
  g_assert_cmpuint (old.calls, ==, 0);
  g_assert_cmpstr (log->str, ==, "fff");
  g_assert_cmpint (fresh.last_phase, ==, SHELL_HOOK_PHASE_AFTER);
  g_assert_true (fresh.amend_was_null);
  g_assert_cmpstr (fresh.last_name, ==, "test.plain");
  g_assert_cmpint (lookup_int (fresh.last_payload, "n"), ==, 3);

  /* the replay happened once: live emissions reach both */
  shell_hook_registry_emit_after (reg, "test.plain", NULL);
  g_assert_cmpuint (src.calls, ==, 1);
  g_assert_cmpstr (log->str, ==, "fffof");

  recorder_clear (&old);
  recorder_clear (&fresh);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_replay_floating_payloads (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 2, .floating = TRUE };
  GString *log = g_string_new (NULL);
  Recorder r;

  recorder_init (&r, "r", log);
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r);
  tu_spin (20);
  g_assert_cmpuint (r.calls, ==, 2);
  g_assert_cmpint (lookup_int (r.last_payload, "n"), ==, 2);

  recorder_clear (&r);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_replay_covers_live_emissions (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 1 };
  GString *log = g_string_new (NULL);
  Recorder other, pending;

  recorder_init (&other, "o", log);
  recorder_init (&pending, "p", log);
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &other);
  tu_spin (20);   /* other: connected after set_replay, replayed (1 call) */
  g_string_truncate (log, 0);
  other.calls = 0;

  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &pending);
  /* between the connection and the idle: the live emission reaches the other
   * listener, the pending one gets the replay instead */
  shell_hook_registry_emit_after (reg, "test.plain", g_variant_new_parsed ("{'n': <int64 99>}"));
  g_assert_cmpstr (log->str, ==, "o");
  g_assert_cmpuint (pending.calls, ==, 0);
  tu_spin (20);
  g_assert_cmpstr (log->str, ==, "op");
  g_assert_cmpuint (pending.calls, ==, 1);
  g_assert_cmpint (lookup_int (pending.last_payload, "n"), ==, 1);

  recorder_clear (&other);
  recorder_clear (&pending);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_replay_disconnected_before_idle (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 2 };
  GString *log = g_string_new (NULL);
  Recorder r;
  guint id;

  recorder_init (&r, "r", log);
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  id = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r);
  shell_hook_registry_disconnect (reg, id);
  g_assert_cmpuint (r.destroyed, ==, 1);
  tu_spin (20);
  g_assert_cmpuint (r.calls, ==, 0);
  g_assert_cmpuint (src.calls, ==, 0);

  recorder_clear (&r);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_replay_disconnect_from_the_replay (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 3 };
  GString *log = g_string_new (NULL);
  Recorder r;
  guint id;

  recorder_init (&r, "r", log);
  r.registry = reg;
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  id = connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r);
  r.disconnect_self_id = id;
  tu_spin (20);

  /* the first call disconnected the listener: the rest is not delivered, the
   * destroy notify ran once */
  g_assert_cmpuint (r.calls, ==, 1);
  g_assert_cmpuint (r.destroyed, ==, 1);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 0);

  recorder_clear (&r);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_replay_connect_from_the_replay (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 1 };
  GString *log = g_string_new (NULL);
  Recorder first, late;

  recorder_init (&first, "1", log);
  recorder_init (&late, "L", log);
  src.connect_in_replay = reg;
  src.late = &late;
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &first);

  /* the listener connected by the replay function gets its own replay later,
   * and the first one is not told twice */
  tu_spin (50);
  g_assert_cmpstr (log->str, ==, "1L");
  g_assert_cmpuint (src.calls, ==, 2);

  recorder_clear (&first);
  recorder_clear (&late);
  g_string_free (log, TRUE);
  g_object_unref (reg);
}

static void
test_replay_not_for_before_and_not_an_emission (void)
{
  ShellHookRegistry *reg = registry_with ("test.showing", TRUE);
  ReplaySource src = { .n_items = 2 };
  EmittedProbe probe = { g_string_new (NULL), 0, NULL, 0, NULL };
  GString *log = g_string_new (NULL);
  Recorder before, after, later;
  gulong handler;

  recorder_init (&before, "b", log);
  recorder_init (&after, "a", log);
  recorder_init (&later, "l", log);
  handler = g_signal_connect (reg, "emitted", G_CALLBACK (on_emitted), &probe);

  src.any_name = TRUE;
  shell_hook_registry_set_replay (reg, "test.showing", replay_items, &src);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_BEFORE, &before);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_AFTER, &after);
  tu_spin (20);

  /* only the "after" listener, and the replay is not an emission */
  g_assert_cmpuint (before.calls, ==, 0);
  g_assert_cmpuint (after.calls, ==, 2);
  g_assert_cmpuint (src.calls, ==, 1);
  g_assert_cmpuint (probe.count, ==, 0);

  /* removing the replay: later listeners get nothing */
  shell_hook_registry_set_replay (reg, "test.showing", NULL, NULL);
  connect_recorder (reg, "test.showing", SHELL_HOOK_PHASE_AFTER, &later);
  tu_spin (20);
  g_assert_cmpuint (later.calls, ==, 0);

  g_signal_handler_disconnect (reg, handler);
  g_clear_pointer (&probe.name, g_free);
  g_string_free (probe.log, TRUE);
  g_object_unref (reg);
  recorder_clear (&before);
  recorder_clear (&after);
  recorder_clear (&later);
  g_string_free (log, TRUE);
}

static void
test_replay_registry_finalized_with_pending (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);
  ReplaySource src = { .n_items = 2 };
  GString *log = g_string_new (NULL);
  Recorder r;

  recorder_init (&r, "r", log);
  shell_hook_registry_set_replay (reg, "test.plain", replay_items, &src);
  connect_recorder (reg, "test.plain", SHELL_HOOK_PHASE_AFTER, &r);

  /* the registry goes away before the idle: nothing runs, nothing leaks */
  g_object_unref (reg);
  g_assert_cmpuint (r.destroyed, ==, 1);
  tu_spin (20);
  g_assert_cmpuint (r.calls, ==, 0);
  g_assert_cmpuint (src.calls, ==, 0);

  recorder_clear (&r);
  g_string_free (log, TRUE);
}

static void
test_replay_undeclared_warns (void)
{
  ShellHookRegistry *reg = registry_with ("test.plain", FALSE);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*undeclared hook 'no.such'*");
  shell_hook_registry_set_replay (reg, "no.such", replay_items, NULL);
  g_test_assert_expected_messages ();
  g_object_unref (reg);
}

/* ------------------------------------------------------------------------ */
/* shell.hooks from Lua                                                      */
/* ------------------------------------------------------------------------ */

static void
declare_lua_hooks (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();

  shell_hook_registry_declare (reg, "test.plain", "n:x label:s", FALSE, "A plain hook");
  shell_hook_registry_declare (reg, "test.showing", "title:s", TRUE, "A cancellable hook");
  shell_hook_registry_declare (reg, "test.lua-on", "", FALSE, NULL);
  shell_hook_registry_declare (reg, "test.lua-cancel", "", TRUE, NULL);
  shell_hook_registry_declare (reg, "test.lua-amend", "", TRUE, NULL);
  shell_hook_registry_declare (reg, "test.lua-errors", "", TRUE, NULL);
  shell_hook_registry_declare (reg, "test.lua-self", "", FALSE, NULL);
  shell_hook_registry_declare (reg, "test.lua-sandbox", "", FALSE, NULL);
  shell_hook_registry_declare (reg, "test.lua-mixed", "", TRUE, NULL);
  shell_hook_registry_declare (reg, "test.lua-replay", "n:x", FALSE, NULL);
}

static void
test_lua_on_and_disconnect (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s,
         "got = nil; calls = 0\n"
         "conn = shell.hooks.on('test.lua-on', function(p) got = p; calls = calls + 1 end)\n");
  tu_assert_eval_str (s, "type(conn)", "userdata");
  tu_assert_eval_true (s, "tostring(conn):find('^<shell.Connection') ~= nil");
  tu_assert_eval_true (s, "conn:is_connected()");
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.lua-on", SHELL_HOOK_PHASE_AFTER), ==, 1);

  /* payload arrives as a table (a{sv} mapping) */
  tu_do (s, "shell.hooks.emit_after('test.lua-on', {n = 7, label = 'hi', nested = {1, 2}, ok = true, r = 0.5})");
  tu_assert_eval_true (s, "calls == 1 and got.n == 7 and got.label == 'hi' and got.nested[2] == 2 and "
                          "got.ok == true and got.r == 0.5");
  /* no payload: an empty table */
  tu_do (s, "shell.hooks.emit_after('test.lua-on')");
  tu_assert_eval_true (s, "calls == 2 and type(got) == 'table' and next(got) == nil");
  /* the same through C */
  shell_hook_registry_emit_after (reg, "test.lua-on",
                                  g_variant_new_parsed ("{'who': <'c'>, 'n': <int32 3>}"));
  tu_assert_eval_true (s, "calls == 3 and got.who == 'c' and got.n == 3");

  /* disconnect: no more calls; repeating it is harmless */
  tu_do (s, "conn:disconnect()");
  tu_assert_eval_true (s, "conn:is_connected() == false");
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.lua-on", SHELL_HOOK_PHASE_AFTER), ==, 0);
  tu_do (s, "conn:disconnect()");
  tu_do (s, "shell.hooks.emit_after('test.lua-on', {})");
  tu_assert_eval_true (s, "calls == 3");

  g_object_unref (s);
}

static GPtrArray *
lua_replay_items (ShellHookRegistry *reg,
                  const char        *name,
                  gpointer           user_data)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);

  g_ptr_array_add (items, g_variant_ref_sink (g_variant_new_parsed ("{'n': <int64 1>}")));
  g_ptr_array_add (items, g_variant_ref_sink (g_variant_new_parsed ("{'n': <int64 2>}")));
  return items;
}

static void
test_lua_replay (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  shell_hook_registry_set_replay (reg, "test.lua-replay", lua_replay_items, NULL);

  /* never inside shell.hooks.on (); then one call per payload, in order, for
   * this listener only */
  tu_do (s,
         "seen = {}; other = {}\n"
         "first = shell.hooks.on('test.lua-replay', function(p) other[#other + 1] = p.n end)\n");
  tu_spin (20);
  tu_do (s,
         "assert(#other == 2)\n"
         "second = shell.hooks.on('test.lua-replay', function(p) seen[#seen + 1] = p.n end)\n"
         "assert(#seen == 0, 'no replay from inside on ()')\n");
  tu_spin (20);
  tu_assert_eval_true (s, "#seen == 2 and seen[1] == 1 and seen[2] == 2");
  tu_assert_eval_true (s, "#other == 2");

  /* a connection that is disconnected at once gets nothing */
  tu_do (s,
         "seen = {}\n"
         "third = shell.hooks.on('test.lua-replay', function(p) seen[#seen + 1] = p.n end)\n"
         "third:disconnect()\n");
  tu_spin (20);
  tu_assert_eval_true (s, "#seen == 0");

  shell_hook_registry_set_replay (reg, "test.lua-replay", NULL, NULL);
  g_object_unref (s);
}

static void
test_lua_before_cancel (void)
{
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  tu_assert_eval_true (s, "type(shell.hooks.CANCEL) == 'userdata' and "
                          "rawequal(shell.hooks.CANCEL, shell.hooks.CANCEL)");
  tu_do (s,
         "order = {}\n"
         "shell.hooks.before('test.lua-cancel', function(p) order[#order + 1] = 'a' end)\n"
         "shell.hooks.before('test.lua-cancel', function(p) order[#order + 1] = 'b'; return shell.hooks.CANCEL end)\n"
         "shell.hooks.before('test.lua-cancel', function(p) order[#order + 1] = 'c' end)\n"
         "cancelled, amend = shell.hooks.emit_before('test.lua-cancel', {x = 1})\n");
  tu_assert_eval_true (s, "cancelled == true and amend == nil and table.concat(order) == 'ab'");

  /* returning anything else continues */
  tu_do (s,
         "order = {}\n"
         "shell.hooks.before('test.showing', function() order[#order + 1] = 'x'; return true end)\n"
         "shell.hooks.before('test.showing', function() order[#order + 1] = 'y'; return 'no' end)\n"
         "shell.hooks.before('test.showing', function() order[#order + 1] = 'z'; return 42 end)\n"
         "cancelled, amend = shell.hooks.emit_before('test.showing', {title = 't'})\n");
  tu_assert_eval_true (s, "cancelled == false and amend == nil and table.concat(order) == 'xyz'");

  g_object_unref (s);
}

static void
test_lua_before_amendments (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s,
         "seen = nil\n"
         "shell.hooks.before('test.lua-amend', function(p) seen = p; return {title = 'new', n = 5, list = {1, 2}} end)\n"
         "shell.hooks.before('test.lua-amend', function(p) return {n = 6, extra = true} end)\n"
         "cancelled, amend = shell.hooks.emit_before('test.lua-amend', {title = 'old', keep = 1})\n");
  tu_assert_eval_true (s, "cancelled == false");
  tu_assert_eval_true (s, "seen.title == 'old' and seen.keep == 1");
  /* the amendments of all listeners are merged, later ones win */
  tu_assert_eval_true (s, "amend.title == 'new' and amend.n == 6 and amend.extra == true and "
                          "amend.list[1] == 1 and amend.list[2] == 2 and amend.keep == nil");

  /* the same observed through C */
  {
    GVariant *amendments = NULL;
    ShellHookVerdict v = shell_hook_registry_emit_before (reg, "test.lua-amend", NULL, &amendments);

    g_assert_cmpint (v, ==, SHELL_HOOK_VERDICT_CONTINUE);
    g_assert_nonnull (amendments);
    g_assert_cmpint (lookup_int (amendments, "n"), ==, 6);
    g_variant_unref (amendments);
  }

  /* an amendments table that cannot be represented contributes nothing and
   * does not stop emission; the error is counted */
  tu_do (s, "shell.hooks.before('test.lua-amend', function() return {bad = print} end)");
  {
    GVariant *amendments = NULL;

    shell_hook_registry_emit_before (reg, "test.lua-amend", NULL, &amendments);
    g_assert_nonnull (amendments);
    g_assert_cmpint (lookup_int (amendments, "n"), ==, 6);
    g_variant_unref (amendments);
    g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);
  }

  g_object_unref (s);
}

static void
test_lua_list (void)
{
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s,
         "hooks = shell.hooks.list()\n"
         "by_name = {}\n"
         "for i, h in ipairs(hooks) do by_name[h.name] = h end\n");
  tu_assert_eval_true (s, "type(hooks) == 'table' and #hooks >= 2");
  tu_assert_eval_true (s, "by_name['test.plain'].cancellable == false and "
                          "by_name['test.plain'].doc == 'A plain hook' and "
                          "by_name['test.plain'].schema == 'n:x label:s'");
  tu_assert_eval_true (s, "by_name['test.showing'].cancellable == true and "
                          "by_name['test.showing'].doc == 'A cancellable hook' and "
                          "by_name['test.showing'].schema == 'title:s'");
  /* no doc: the field is absent */
  tu_assert_eval_true (s, "by_name['test.lua-on'].doc == nil and by_name['test.lua-on'].schema == ''");
  /* sorted by name */
  tu_assert_eval_true (s, "(function() for i = 2, #hooks do "
                          "if not (hooks[i - 1].name < hooks[i].name) then return false end end return true end)()");

  g_object_unref (s);
}

static void
test_lua_argument_errors (void)
{
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  tu_assert_fails (s, "shell.hooks.on('nope.nothing', function() end)", SHELL_LUA_ERROR_RUNTIME,
                   "unknown hook 'nope.nothing'");
  tu_assert_fails (s, "shell.hooks.before('nope.nothing', function() end)", SHELL_LUA_ERROR_RUNTIME,
                   "unknown hook 'nope.nothing'");
  tu_assert_fails (s, "shell.hooks.before('test.plain', function() end)", SHELL_LUA_ERROR_RUNTIME,
                   "hook 'test.plain' is not cancellable");
  tu_assert_fails (s, "shell.hooks.on('test.plain', 5)", SHELL_LUA_ERROR_RUNTIME, "function expected");
  tu_assert_fails (s, "shell.hooks.on({}, function() end)", SHELL_LUA_ERROR_RUNTIME, "bad argument #1");
  tu_assert_fails (s, "shell.hooks.on('test.plain')", SHELL_LUA_ERROR_RUNTIME, "function expected");
  tu_assert_fails (s, "shell.hooks.emit_before('nope.nothing', {})", SHELL_LUA_ERROR_RUNTIME,
                   "unknown hook 'nope.nothing'");
  tu_assert_fails (s, "shell.hooks.emit_after('nope.nothing', {})", SHELL_LUA_ERROR_RUNTIME,
                   "unknown hook 'nope.nothing'");
  tu_assert_fails (s, "shell.hooks.emit_after('test.plain', 5)", SHELL_LUA_ERROR_RUNTIME,
                   "table expected");
  tu_assert_fails (s, "shell.hooks.emit_after('test.plain', {bad = print})", SHELL_LUA_ERROR_RUNTIME,
                   "cannot convert Lua function for field value.bad");
  g_assert_cmpuint (shell_hook_registry_count_listeners (shell_hook_registry_get_default (),
                                                         "test.plain", SHELL_HOOK_PHASE_AFTER), ==, 0);

  g_object_unref (s);
}

static void
test_lua_sandboxed_cannot_emit (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_assert_fails (s, "shell.hooks.emit_after('test.plain', {})", SHELL_LUA_ERROR_RUNTIME,
                   "only available in privileged states");
  tu_assert_fails (s, "shell.hooks.emit_before('test.showing', {})", SHELL_LUA_ERROR_RUNTIME,
                   "only available in privileged states");

  /* sandboxed code can still listen, and is disconnected when its state goes away */
  tu_do (s, "hits = 0; shell.hooks.on('test.lua-sandbox', function(p) hits = hits + p.n end)");
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.lua-sandbox", SHELL_HOOK_PHASE_AFTER), ==, 1);
  shell_hook_registry_emit_after (reg, "test.lua-sandbox", g_variant_new_parsed ("{'n': <int64 4>}"));
  shell_hook_registry_emit_after (reg, "test.lua-sandbox", g_variant_new_parsed ("{'n': <int64 5>}"));
  tu_assert_eval_str (s, "hits", "9");

  g_object_unref (s);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.lua-sandbox", SHELL_HOOK_PHASE_AFTER), ==, 0);
  /* nothing left behind to call into the dead state */
  shell_hook_registry_emit_after (reg, "test.lua-sandbox", g_variant_new_parsed ("{'n': <int64 1>}"));
}

static void
test_lua_listener_errors (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);
  GVariant *amendments = NULL;

  tu_do (s,
         "order = {}\n"
         "shell.hooks.before('test.lua-errors', function() order[#order + 1] = 'a'; error('boom') end)\n"
         "shell.hooks.before('test.lua-errors', function() order[#order + 1] = 'b'; return {ok = 1} end)\n"
         "shell.hooks.on('test.lua-errors', function() error({}) end)\n");

  /* a failing listener neither cancels nor stops the others */
  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.lua-errors", NULL, &amendments),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_nonnull (amendments);
  g_assert_cmpint (lookup_int (amendments, "ok"), ==, 1);
  g_variant_unref (amendments);
  tu_assert_eval_str (s, "table.concat(order)", "ab");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);
  shell_hook_registry_emit_after (reg, "test.lua-errors", NULL);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 2);

  g_object_unref (s);
}

static void
test_lua_disconnect_from_listener (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s,
         "log = {}\n"
         "c2 = nil\n"
         "c1 = shell.hooks.on('test.lua-self', function() log[#log + 1] = '1'; c1:disconnect(); c2:disconnect() end)\n"
         "c2 = shell.hooks.on('test.lua-self', function() log[#log + 1] = '2' end)\n"
         "c3 = shell.hooks.on('test.lua-self', function() log[#log + 1] = '3' end)\n");
  shell_hook_registry_emit_after (reg, "test.lua-self", NULL);
  tu_assert_eval_str (s, "table.concat(log)", "13");
  shell_hook_registry_emit_after (reg, "test.lua-self", NULL);
  tu_assert_eval_str (s, "table.concat(log)", "133");
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.lua-self", SHELL_HOOK_PHASE_AFTER), ==, 1);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 0);

  g_object_unref (s);
  g_assert_cmpuint (shell_hook_registry_count_listeners (reg, "test.lua-self", SHELL_HOOK_PHASE_AFTER), ==, 0);
}

static void
test_lua_mixed_with_c_listeners (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);
  GString *log = g_string_new (NULL);
  Recorder c1, c2;
  GVariant *amendments = NULL;
  guint id1, id2;

  recorder_init (&c1, "C1", log);
  recorder_init (&c2, "C2", log);
  c1.amend_key = "from_c";  c1.amend_value = 10;

  id1 = connect_recorder (reg, "test.lua-mixed", SHELL_HOOK_PHASE_BEFORE, &c1);
  tu_do (s, "shell.hooks.before('test.lua-mixed', function(p) return {from_lua = 20} end)");
  id2 = connect_recorder (reg, "test.lua-mixed", SHELL_HOOK_PHASE_BEFORE, &c2);

  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.lua-mixed", NULL, &amendments),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_cmpstr (log->str, ==, "C1C2");
  g_assert_cmpint (lookup_int (amendments, "from_c"), ==, 10);
  g_assert_cmpint (lookup_int (amendments, "from_lua"), ==, 20);
  g_variant_unref (amendments);

  /* a Lua CANCEL stops later C listeners */
  tu_do (s, "shell.hooks.before('test.lua-mixed', function() return shell.hooks.CANCEL end)");
  shell_hook_registry_disconnect (reg, id2);
  id2 = connect_recorder (reg, "test.lua-mixed", SHELL_HOOK_PHASE_BEFORE, &c2);
  g_string_truncate (log, 0);
  g_assert_cmpint (shell_hook_registry_emit_before (reg, "test.lua-mixed", NULL, NULL),
                   ==, SHELL_HOOK_VERDICT_CANCEL);
  g_assert_cmpstr (log->str, ==, "C1");

  shell_hook_registry_disconnect (reg, id1);
  shell_hook_registry_disconnect (reg, id2);
  recorder_clear (&c1);
  recorder_clear (&c2);
  g_string_free (log, TRUE);
  g_object_unref (s);
}

static void
test_lua_emit_payload_signal (void)
{
  ShellHookRegistry *reg = shell_hook_registry_get_default ();
  ShellLuaState *s = tu_state_new ("hk", SHELL_LUA_STATE_PRIVILEGED);
  EmittedProbe probe = { g_string_new (NULL), 0, NULL, -1, NULL };
  gulong handler = g_signal_connect (reg, "emitted", G_CALLBACK (on_emitted), &probe);
  gint64 n = 0;
  const char *label = NULL;

  tu_do (s, "shell.hooks.emit_after('test.plain', {n = 7, label = 'x'})");
  g_assert_cmpuint (probe.count, ==, 1);
  g_assert_cmpstr (probe.name, ==, "test.plain");
  g_assert_cmpint (probe.phase, ==, SHELL_HOOK_PHASE_AFTER);
  g_assert_true (g_variant_is_of_type (probe.payload, G_VARIANT_TYPE_VARDICT));
  g_assert_true (g_variant_lookup (probe.payload, "n", "x", &n));
  g_assert_cmpint (n, ==, 7);
  g_assert_true (g_variant_lookup (probe.payload, "label", "&s", &label));
  g_assert_cmpstr (label, ==, "x");

  tu_do (s, "shell.hooks.emit_before('test.showing', {title = 'T'})");
  g_assert_cmpuint (probe.count, ==, 2);
  g_assert_cmpint (probe.phase, ==, SHELL_HOOK_PHASE_BEFORE);

  g_signal_handler_disconnect (reg, handler);
  g_free (probe.name);
  g_clear_pointer (&probe.payload, g_variant_unref);
  g_string_free (probe.log, TRUE);
  g_object_unref (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  declare_lua_hooks ();

  g_test_add_func ("/shell-lua/hooks/declare/query", test_declare_and_query);
  g_test_add_func ("/shell-lua/hooks/declare/bad-names", test_declare_bad_names);
  g_test_add_func ("/shell-lua/hooks/declare/default-registry", test_default_registry);
  g_test_add_func ("/shell-lua/hooks/declare/enums", test_enums);
  g_test_add_func ("/shell-lua/hooks/connect/refused", test_connect_refused);
  g_test_add_func ("/shell-lua/hooks/emit/before-order-and-amendments", test_emit_before_order_and_amendments);
  g_test_add_func ("/shell-lua/hooks/emit/before-cancel", test_emit_before_cancel);
  g_test_add_func ("/shell-lua/hooks/emit/after", test_emit_after);
  g_test_add_func ("/shell-lua/hooks/emit/misuse-warns", test_emit_misuse_warns);
  g_test_add_func ("/shell-lua/hooks/emit/emitted-signal", test_emitted_signal);
  g_test_add_func ("/shell-lua/hooks/disconnect/during-emission", test_disconnect_during_emission);
  g_test_add_func ("/shell-lua/hooks/connect/during-emission", test_connect_during_emission);
  g_test_add_func ("/shell-lua/hooks/disconnect/destroy-on-finalize", test_destroy_on_finalize);
  g_test_add_func ("/shell-lua/hooks/replay/basic", test_replay_basic);
  g_test_add_func ("/shell-lua/hooks/replay/floating-payloads", test_replay_floating_payloads);
  g_test_add_func ("/shell-lua/hooks/replay/covers-live-emissions", test_replay_covers_live_emissions);
  g_test_add_func ("/shell-lua/hooks/replay/disconnected-before-idle", test_replay_disconnected_before_idle);
  g_test_add_func ("/shell-lua/hooks/replay/disconnect-from-the-replay", test_replay_disconnect_from_the_replay);
  g_test_add_func ("/shell-lua/hooks/replay/connect-from-the-replay", test_replay_connect_from_the_replay);
  g_test_add_func ("/shell-lua/hooks/replay/not-for-before", test_replay_not_for_before_and_not_an_emission);
  g_test_add_func ("/shell-lua/hooks/replay/registry-finalized", test_replay_registry_finalized_with_pending);
  g_test_add_func ("/shell-lua/hooks/replay/undeclared-warns", test_replay_undeclared_warns);
  g_test_add_func ("/shell-lua/hooks/lua/on-and-disconnect", test_lua_on_and_disconnect);
  g_test_add_func ("/shell-lua/hooks/lua/replay", test_lua_replay);
  g_test_add_func ("/shell-lua/hooks/lua/before-cancel", test_lua_before_cancel);
  g_test_add_func ("/shell-lua/hooks/lua/before-amendments", test_lua_before_amendments);
  g_test_add_func ("/shell-lua/hooks/lua/list", test_lua_list);
  g_test_add_func ("/shell-lua/hooks/lua/argument-errors", test_lua_argument_errors);
  g_test_add_func ("/shell-lua/hooks/lua/sandboxed-cannot-emit", test_lua_sandboxed_cannot_emit);
  g_test_add_func ("/shell-lua/hooks/lua/listener-errors", test_lua_listener_errors);
  g_test_add_func ("/shell-lua/hooks/lua/disconnect-from-listener", test_lua_disconnect_from_listener);
  g_test_add_func ("/shell-lua/hooks/lua/mixed-with-c-listeners", test_lua_mixed_with_c_listeners);
  g_test_add_func ("/shell-lua/hooks/lua/emit-payload-signal", test_lua_emit_payload_signal);

  return g_test_run ();
}
