/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * search.c: The search results of the overview. Port of js/ui/search.js and
 * of the `Highlighter` class of js/misc/util.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A search provider is the #ShellSearchProvider interface instead of a
 *    duck-typed object. The asynchronous methods answer a #ShellSearchRequest
 *    instead of returning a promise (GJS cannot implement a virtual function
 *    that takes a #GAsyncReadyCallback).
 *  - The state the view kept on the provider (`provider.display`,
 *    `provider.searchInProgress`) is kept by the view.
 *  - The view does not create the applications provider (it is still
 *    implemented in app-display.c, which this module does not use): it is
 *    handed to the view when it is created and registered before the remote
 *    providers, as before.
 *  - `Main.overview.toggle ()`, which activating a result and the launch
 *    button of a section called, is the `overview-toggle-requested` signal of
 *    the view.
 *  - The errors of the providers (a wrong number of metas, ...) are logged
 *    with g_warning () and g_message (); a canceled request is never logged.
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <math.h>
#include <meta/compositor.h>
#include <meta/meta-later.h>

#include "misc/parental-controls-manager.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-hooks.h"
#include "ui/animation.h"
#include "ui/environment.h"
#include "ui/icon-grid.h"
#include "ui/remote-search.h"
#include "ui/search.h"

#define SEARCH_PROVIDERS_SCHEMA "org.gnome.desktop.search-providers"

#define MAX_LIST_SEARCH_RESULTS_ROWS 5
#define SEARCH_SPINNER_SIZE 64
#define LIST_SEARCH_RESULT_ICON_SIZE 24
#define PROVIDER_ICON_SIZE 32
#define SEARCH_TIMEOUT_MS 150

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static char **
strv_new_empty (void)
{
  return g_new0 (char *, 1);
}

static char **
strv_dup_or_empty (const char * const *strv)
{
  return strv != NULL ? g_strdupv ((char **) strv) : strv_new_empty ();
}

/* The first @n items of a NULL terminated array */
static char **
strv_dup_first (const char * const *strv,
                guint               n)
{
  guint length = g_strv_length ((char **) strv);
  char **copy;

  n = MIN (n, length);
  copy = g_new0 (char *, n + 1);
  for (guint i = 0; i < n; i++)
    copy[i] = g_strdup (strv[i]);

  return copy;
}

static gboolean
error_is_cancelled (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* ------------------------------------------------------------------------ */
/* ShellHighlighter                                                          */
/* ------------------------------------------------------------------------ */

struct _ShellHighlighter
{
  GObject parent;

  GRegex *regex;
};

G_DEFINE_FINAL_TYPE (ShellHighlighter, shell_highlighter, G_TYPE_OBJECT)

static void
shell_highlighter_finalize (GObject *object)
{
  ShellHighlighter *self = SHELL_HIGHLIGHTER (object);

  g_clear_pointer (&self->regex, g_regex_unref);

  G_OBJECT_CLASS (shell_highlighter_parent_class)->finalize (object);
}

static void
shell_highlighter_class_init (ShellHighlighterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = shell_highlighter_finalize;
}

static void
shell_highlighter_init (ShellHighlighter *self)
{
}

/**
 * shell_highlighter_new:
 * @terms: (nullable) (array zero-terminated=1): the terms to highlight
 *
 * Creates a highlighter which highlights the given terms in text using
 * markup (`new Highlighter (terms)`). The terms are matched without regard to
 * case.
 *
 * Returns: (transfer full): a new #ShellHighlighter
 */
ShellHighlighter *
shell_highlighter_new (const char * const *terms)
{
  g_autoptr (ShellHighlighter) self = g_object_new (SHELL_TYPE_HIGHLIGHTER, NULL);
  g_autoptr (GString) pattern = NULL;

  if (terms == NULL)
    return g_steal_pointer (&self);

  for (int i = 0; terms[i] != NULL; i++)
    {
      g_autofree char *escaped = g_regex_escape_string (terms[i], -1);

      if (*escaped == '\0')
        continue;

      if (pattern == NULL)
        pattern = g_string_new ("(");
      else
        g_string_append_c (pattern, '|');

      g_string_append (pattern, escaped);
    }

  if (pattern != NULL)
    {
      g_autoptr (GError) error = NULL;

      g_string_append_c (pattern, ')');
      self->regex = g_regex_new (pattern->str, G_REGEX_CASELESS, 0, &error);
      if (self->regex == NULL)
        g_warning ("Could not highlight the search terms: %s", error->message);
    }

  return g_steal_pointer (&self);
}

/**
 * shell_highlighter_highlight:
 * @self: a #ShellHighlighter
 * @text: the text to highlight the terms of @self in
 *
 * Highlights all the occurences of the terms of @self in @text using markup
 * (`highlight ()`). The rest of the text is escaped.
 *
 * Returns: (transfer full): the markup
 */
char *
shell_highlighter_highlight (ShellHighlighter *self,
                             const char       *text)
{
  g_autoptr (GString) markup = NULL;
  g_autoptr (GMatchInfo) match_info = NULL;
  int last_match_end = 0;
  g_autofree char *rest = NULL;

  g_return_val_if_fail (SHELL_IS_HIGHLIGHTER (self), NULL);
  g_return_val_if_fail (text != NULL, NULL);

  if (self->regex == NULL)
    return g_markup_escape_text (text, -1);

  markup = g_string_new (NULL);

  g_regex_match (self->regex, text, 0, &match_info);
  while (g_match_info_matches (match_info))
    {
      g_autofree char *unmatched = NULL;
      g_autofree char *matched = NULL;
      int start, end;

      if (!g_match_info_fetch_pos (match_info, 0, &start, &end) || end <= start)
        break;

      if (start > last_match_end)
        {
          unmatched = g_markup_escape_text (text + last_match_end,
                                            start - last_match_end);
          g_string_append (markup, unmatched);
        }

      matched = g_markup_escape_text (text + start, end - start);
      g_string_append_printf (markup, "<b>%s</b>", matched);
      last_match_end = end;

      if (!g_match_info_next (match_info, NULL))
        break;
    }

  rest = g_markup_escape_text (text + last_match_end, -1);
  g_string_append (markup, rest);

  return g_string_free (g_steal_pointer (&markup), FALSE);
}

/* ------------------------------------------------------------------------ */
/* ShellSearchResultMeta                                                     */
/* ------------------------------------------------------------------------ */

enum
{
  META_PROP_0,

  META_PROP_ID,
  META_PROP_NAME,
  META_PROP_DESCRIPTION,
  META_PROP_CLIPBOARD_TEXT,

  META_N_PROPS
};

static GParamSpec *meta_props[META_N_PROPS] = { NULL, };

struct _ShellSearchResultMeta
{
  GObject parent;

  char *id;
  char *name;
  char *description;
  char *clipboard_text;

  ShellSearchResultMetaCreateIconFunc create_icon_func;
  gpointer create_icon_data;
  GDestroyNotify create_icon_destroy;
};

G_DEFINE_FINAL_TYPE (ShellSearchResultMeta, shell_search_result_meta, G_TYPE_OBJECT)

static void
shell_search_result_meta_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellSearchResultMeta *self = SHELL_SEARCH_RESULT_META (object);

  switch (prop_id)
    {
    case META_PROP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case META_PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;

    case META_PROP_DESCRIPTION:
      g_free (self->description);
      self->description = g_value_dup_string (value);
      break;

    case META_PROP_CLIPBOARD_TEXT:
      g_free (self->clipboard_text);
      self->clipboard_text = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_result_meta_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellSearchResultMeta *self = SHELL_SEARCH_RESULT_META (object);

  switch (prop_id)
    {
    case META_PROP_ID:
      g_value_set_string (value, self->id);
      break;

    case META_PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case META_PROP_DESCRIPTION:
      g_value_set_string (value, self->description);
      break;

    case META_PROP_CLIPBOARD_TEXT:
      g_value_set_string (value, self->clipboard_text);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_result_meta_dispose (GObject *object)
{
  ShellSearchResultMeta *self = SHELL_SEARCH_RESULT_META (object);

  shell_search_result_meta_set_create_icon_func (self, NULL, NULL, NULL);

  G_OBJECT_CLASS (shell_search_result_meta_parent_class)->dispose (object);
}

static void
shell_search_result_meta_finalize (GObject *object)
{
  ShellSearchResultMeta *self = SHELL_SEARCH_RESULT_META (object);

  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->description, g_free);
  g_clear_pointer (&self->clipboard_text, g_free);

  G_OBJECT_CLASS (shell_search_result_meta_parent_class)->finalize (object);
}

static void
shell_search_result_meta_class_init (ShellSearchResultMetaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_search_result_meta_set_property;
  object_class->get_property = shell_search_result_meta_get_property;
  object_class->dispose = shell_search_result_meta_dispose;
  object_class->finalize = shell_search_result_meta_finalize;

  /**
   * ShellSearchResultMeta:id:
   *
   * The identifier of the result, unique within its provider.
   */
  meta_props[META_PROP_ID] =
    g_param_spec_string ("id", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchResultMeta:name:
   *
   * The human readable name of the result.
   */
  meta_props[META_PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchResultMeta:description:
   *
   * A description which helps finding the right result, or %NULL. Only the
   * first line is shown.
   */
  meta_props[META_PROP_DESCRIPTION] =
    g_param_spec_string ("description", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchResultMeta:clipboard-text:
   *
   * Text copied to the clipboard when the result is activated, or %NULL
   * (`clipboardText`).
   */
  meta_props[META_PROP_CLIPBOARD_TEXT] =
    g_param_spec_string ("clipboard-text", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, META_N_PROPS, meta_props);
}

static void
shell_search_result_meta_init (ShellSearchResultMeta *self)
{
}

/**
 * shell_search_result_meta_new:
 * @id: the identifier of the result
 * @name: the human readable name of the result
 *
 * Creates the metadata of a result. Set the optional parts with
 * shell_search_result_meta_set_description (),
 * shell_search_result_meta_set_clipboard_text () and
 * shell_search_result_meta_set_create_icon_func ().
 *
 * Returns: (transfer full): a new #ShellSearchResultMeta
 */
ShellSearchResultMeta *
shell_search_result_meta_new (const char *id,
                              const char *name)
{
  return g_object_new (SHELL_TYPE_SEARCH_RESULT_META,
                       "id", id,
                       "name", name,
                       NULL);
}

/**
 * shell_search_result_meta_get_id:
 * @self: a #ShellSearchResultMeta
 *
 * Returns: (transfer none) (nullable): the identifier of the result
 */
const char *
shell_search_result_meta_get_id (ShellSearchResultMeta *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT_META (self), NULL);

  return self->id;
}

/**
 * shell_search_result_meta_get_name:
 * @self: a #ShellSearchResultMeta
 *
 * Returns: (transfer none) (nullable): the name of the result
 */
const char *
shell_search_result_meta_get_name (ShellSearchResultMeta *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT_META (self), NULL);

  return self->name;
}

/**
 * shell_search_result_meta_get_description:
 * @self: a #ShellSearchResultMeta
 *
 * Returns: (transfer none) (nullable): the description of the result
 */
const char *
shell_search_result_meta_get_description (ShellSearchResultMeta *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT_META (self), NULL);

  return self->description;
}

/**
 * shell_search_result_meta_set_description:
 * @self: a #ShellSearchResultMeta
 * @description: (nullable): the description of the result
 */
void
shell_search_result_meta_set_description (ShellSearchResultMeta *self,
                                          const char            *description)
{
  g_return_if_fail (SHELL_IS_SEARCH_RESULT_META (self));

  if (g_set_str (&self->description, description))
    g_object_notify_by_pspec (G_OBJECT (self), meta_props[META_PROP_DESCRIPTION]);
}

/**
 * shell_search_result_meta_get_clipboard_text:
 * @self: a #ShellSearchResultMeta
 *
 * Returns: (transfer none) (nullable): the text copied to the clipboard when
 *   the result is activated
 */
const char *
shell_search_result_meta_get_clipboard_text (ShellSearchResultMeta *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT_META (self), NULL);

  return self->clipboard_text;
}

/**
 * shell_search_result_meta_set_clipboard_text:
 * @self: a #ShellSearchResultMeta
 * @clipboard_text: (nullable): the text copied to the clipboard when the
 *   result is activated
 */
void
shell_search_result_meta_set_clipboard_text (ShellSearchResultMeta *self,
                                             const char            *clipboard_text)
{
  g_return_if_fail (SHELL_IS_SEARCH_RESULT_META (self));

  if (g_set_str (&self->clipboard_text, clipboard_text))
    g_object_notify_by_pspec (G_OBJECT (self), meta_props[META_PROP_CLIPBOARD_TEXT]);
}

/**
 * shell_search_result_meta_set_create_icon_func:
 * @self: a #ShellSearchResultMeta
 * @func: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   makes the icon of the result
 * @user_data: data for @func
 * @destroy: called on @user_data when @func is replaced or @self is disposed
 *
 * Sets the function which makes the icon or thumbnail of the result
 * (`createIcon`).
 */
void
shell_search_result_meta_set_create_icon_func (ShellSearchResultMeta                *self,
                                               ShellSearchResultMetaCreateIconFunc   func,
                                               gpointer                              user_data,
                                               GDestroyNotify                        destroy)
{
  GDestroyNotify old_destroy;
  gpointer old_data;

  g_return_if_fail (SHELL_IS_SEARCH_RESULT_META (self));

  old_destroy = self->create_icon_destroy;
  old_data = self->create_icon_data;

  self->create_icon_func = func;
  self->create_icon_data = user_data;
  self->create_icon_destroy = destroy;

  if (old_destroy != NULL)
    old_destroy (old_data);
}

/**
 * shell_search_result_meta_create_icon:
 * @self: a #ShellSearchResultMeta
 * @size: the size of the icon in logical pixels
 *
 * Makes the icon of the result with the function set with
 * shell_search_result_meta_set_create_icon_func () (`createIcon ()`).
 *
 * Returns: (transfer full) (nullable): the icon actor, or %NULL if there is
 *   no function or the result has no icon
 */
ClutterActor *
shell_search_result_meta_create_icon (ShellSearchResultMeta *self,
                                      int                    size)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT_META (self), NULL);

  if (self->create_icon_func == NULL)
    return NULL;

  return self->create_icon_func (size, self->create_icon_data);
}

/* ------------------------------------------------------------------------ */
/* ShellSearchRequest                                                        */
/* ------------------------------------------------------------------------ */

typedef enum
{
  REQUEST_KIND_RESULTS,
  REQUEST_KIND_METAS,
} RequestKind;

typedef struct
{
  ShellSearchResultMeta **metas; /* NULL terminated */
  gsize n_metas;
} MetasResult;

struct _ShellSearchRequest
{
  GObject parent;

  GTask *task;
  GCancellable *cancellable;
  RequestKind kind;
};

G_DEFINE_FINAL_TYPE (ShellSearchRequest, shell_search_request, G_TYPE_OBJECT)

static void
metas_result_free (MetasResult *result)
{
  for (gsize i = 0; i < result->n_metas; i++)
    g_object_unref (result->metas[i]);

  g_free (result->metas);
  g_free (result);
}

static void
shell_search_request_dispose (GObject *object)
{
  ShellSearchRequest *self = SHELL_SEARCH_REQUEST (object);

  /* The provider dropped the request without answering it */
  if (self->task != NULL)
    {
      g_autoptr (GTask) task = g_steal_pointer (&self->task);

      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "The search provider did not answer the request");
    }

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_search_request_parent_class)->dispose (object);
}

static void
shell_search_request_class_init (ShellSearchRequestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_search_request_dispose;
}

static void
shell_search_request_init (ShellSearchRequest *self)
{
}

static ShellSearchRequest *
shell_search_request_new (GTask       *task,
                          RequestKind  kind)
{
  ShellSearchRequest *self = g_object_new (SHELL_TYPE_SEARCH_REQUEST, NULL);

  self->task = g_object_ref (task);
  self->kind = kind;
  self->cancellable = g_task_get_cancellable (task) != NULL
    ? g_object_ref (g_task_get_cancellable (task))
    : NULL;

  return self;
}

/**
 * shell_search_request_get_cancellable:
 * @self: a #ShellSearchRequest
 *
 * Returns: (transfer none) (nullable): the cancellable of the request, which
 *   is cancelled when the terms of the search change
 */
GCancellable *
shell_search_request_get_cancellable (ShellSearchRequest *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_REQUEST (self), NULL);

  return self->cancellable;
}

/**
 * shell_search_request_return_results:
 * @self: a #ShellSearchRequest
 * @results: (array zero-terminated=1): the identifiers of the results
 *
 * Answers a request for a set of results (`getInitialResultSet ()` and
 * `getSubsearchResultSet ()`).
 */
void
shell_search_request_return_results (ShellSearchRequest *self,
                                     const char * const *results)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_REQUEST (self));
  g_return_if_fail (self->task != NULL);
  g_return_if_fail (self->kind == REQUEST_KIND_RESULTS);

  task = g_steal_pointer (&self->task);
  g_task_return_pointer (task, strv_dup_or_empty (results),
                         (GDestroyNotify) g_strfreev);
}

/**
 * shell_search_request_return_metas:
 * @self: a #ShellSearchRequest
 * @metas: (array length=n_metas) (transfer none): the metas of the results,
 *   in the order of the requested identifiers
 * @n_metas: the number of metas
 *
 * Answers a request for the metas of results (`getResultMetas ()`).
 */
void
shell_search_request_return_metas (ShellSearchRequest    *self,
                                   ShellSearchResultMeta **metas,
                                   gsize                  n_metas)
{
  g_autoptr (GTask) task = NULL;
  MetasResult *result;

  g_return_if_fail (SHELL_IS_SEARCH_REQUEST (self));
  g_return_if_fail (self->task != NULL);
  g_return_if_fail (self->kind == REQUEST_KIND_METAS);

  for (gsize i = 0; i < n_metas; i++)
    {
      if (!SHELL_IS_SEARCH_RESULT_META (metas[i]))
        {
          shell_search_request_return_error (self, "A result meta is not a ShellSearchResultMeta");
          return;
        }
    }

  result = g_new0 (MetasResult, 1);
  result->metas = g_new0 (ShellSearchResultMeta *, n_metas + 1);
  for (gsize i = 0; i < n_metas; i++)
    result->metas[result->n_metas++] = g_object_ref (metas[i]);

  task = g_steal_pointer (&self->task);
  g_task_return_pointer (task, result, (GDestroyNotify) metas_result_free);
}

/**
 * shell_search_request_return_error:
 * @self: a #ShellSearchRequest
 * @message: the message of the error
 *
 * Answers the request with an error, such as when the provider cannot be
 * reached.
 */
void
shell_search_request_return_error (ShellSearchRequest *self,
                                   const char         *message)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_REQUEST (self));
  g_return_if_fail (self->task != NULL);

  task = g_steal_pointer (&self->task);
  g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

/**
 * shell_search_request_return_gerror: (skip)
 * @self: a #ShellSearchRequest
 * @error: (transfer full): the error
 *
 * Answers the request with @error. A %G_IO_ERROR_CANCELLED error is not
 * reported.
 */
void
shell_search_request_return_gerror (ShellSearchRequest *self,
                                    GError             *error)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_REQUEST (self));
  g_return_if_fail (self->task != NULL);
  g_return_if_fail (error != NULL);

  task = g_steal_pointer (&self->task);
  g_task_return_error (task, error);
}

/* ------------------------------------------------------------------------ */
/* ShellSearchProvider                                                       */
/* ------------------------------------------------------------------------ */

G_DEFINE_INTERFACE (ShellSearchProvider, shell_search_provider, G_TYPE_OBJECT)

static void
provider_default_request (ShellSearchProvider *self,
                          ShellSearchRequest  *request)
{
  g_autofree char *message =
    g_strdup_printf ("Search provider %s does not search", G_OBJECT_TYPE_NAME (self));

  shell_search_request_return_error (request, message);
}

static void
shell_search_provider_real_get_initial_result_set (ShellSearchProvider *self,
                                                   const char * const  *terms,
                                                   GCancellable        *cancellable,
                                                   ShellSearchRequest  *request)
{
  provider_default_request (self, request);
}

static void
shell_search_provider_real_get_subsearch_result_set (ShellSearchProvider *self,
                                                     const char * const  *previous_results,
                                                     const char * const  *terms,
                                                     GCancellable        *cancellable,
                                                     ShellSearchRequest  *request)
{
  provider_default_request (self, request);
}

static void
shell_search_provider_real_get_result_metas (ShellSearchProvider *self,
                                             const char * const  *ids,
                                             GCancellable        *cancellable,
                                             ShellSearchRequest  *request)
{
  provider_default_request (self, request);
}

static void
shell_search_provider_real_activate_result (ShellSearchProvider *self,
                                            const char          *id,
                                            const char * const  *terms)
{
}

static void
shell_search_provider_real_launch_search (ShellSearchProvider *self,
                                          const char * const  *terms)
{
}

static char **
shell_search_provider_real_filter_results (ShellSearchProvider *self,
                                           const char * const  *results,
                                           int                  max_number)
{
  guint n_results = g_strv_length ((char **) results);

  if (n_results <= (guint) MAX (max_number, 0))
    return g_strdupv ((char **) results);

  return strv_dup_first (results, MAX (max_number, 0));
}

static ClutterActor *
shell_search_provider_real_create_result_object (ShellSearchProvider   *self,
                                                 ShellSearchResultMeta *meta)
{
  return NULL;
}

static void
shell_search_provider_default_init (ShellSearchProviderInterface *iface)
{
  iface->get_initial_result_set = shell_search_provider_real_get_initial_result_set;
  iface->get_subsearch_result_set = shell_search_provider_real_get_subsearch_result_set;
  iface->get_result_metas = shell_search_provider_real_get_result_metas;
  iface->activate_result = shell_search_provider_real_activate_result;
  iface->launch_search = shell_search_provider_real_launch_search;
  iface->filter_results = shell_search_provider_real_filter_results;
  iface->create_result_object = shell_search_provider_real_create_result_object;

  /**
   * ShellSearchProvider:id:
   *
   * The identifier of the provider, unique among the providers (`id`).
   */
  g_object_interface_install_property (iface,
    g_param_spec_string ("id", NULL, NULL, NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * ShellSearchProvider:app-info:
   *
   * The application the provider searches in, or %NULL (`appInfo`). The
   * results of a provider with an application are shown in a list headed by
   * the application, those of a provider without one in a grid.
   */
  g_object_interface_install_property (iface,
    g_param_spec_object ("app-info", NULL, NULL, G_TYPE_APP_INFO,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * ShellSearchProvider:can-launch-search:
   *
   * Whether the provider can show all the results of a search in its
   * application (`canLaunchSearch`).
   */
  g_object_interface_install_property (iface,
    g_param_spec_boolean ("can-launch-search", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * ShellSearchProvider:is-remote-provider:
   *
   * Whether the provider is a D-Bus search provider of an installed
   * application, which the search view reloads when the installed
   * applications or the search settings change (`isRemoteProvider`).
   */
  g_object_interface_install_property (iface,
    g_param_spec_boolean ("is-remote-provider", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * ShellSearchProvider:max-results:
   *
   * The maximum number of results shown in the grid of a provider without an
   * application, or -1 for as many as fit (`maxResults`).
   */
  g_object_interface_install_property (iface,
    g_param_spec_int ("max-results", NULL, NULL, -1, G_MAXINT, -1,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

/**
 * shell_search_provider_get_id:
 * @self: a #ShellSearchProvider
 *
 * Returns: (transfer full) (nullable): the identifier of the provider
 */
char *
shell_search_provider_get_id (ShellSearchProvider *self)
{
  char *id = NULL;

  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), NULL);

  g_object_get (self, "id", &id, NULL);
  return id;
}

/**
 * shell_search_provider_get_app_info:
 * @self: a #ShellSearchProvider
 *
 * Returns: (transfer full) (nullable): the application of the provider
 */
GAppInfo *
shell_search_provider_get_app_info (ShellSearchProvider *self)
{
  GAppInfo *app_info = NULL;

  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), NULL);

  g_object_get (self, "app-info", &app_info, NULL);
  return app_info;
}

/**
 * shell_search_provider_get_can_launch_search:
 * @self: a #ShellSearchProvider
 *
 * Returns: whether the provider can show all the results of a search in its
 *   application
 */
gboolean
shell_search_provider_get_can_launch_search (ShellSearchProvider *self)
{
  gboolean can_launch_search = FALSE;

  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), FALSE);

  g_object_get (self, "can-launch-search", &can_launch_search, NULL);
  return can_launch_search;
}

/**
 * shell_search_provider_get_is_remote_provider:
 * @self: a #ShellSearchProvider
 *
 * Returns: whether the provider is a D-Bus search provider
 */
gboolean
shell_search_provider_get_is_remote_provider (ShellSearchProvider *self)
{
  gboolean is_remote_provider = FALSE;

  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), FALSE);

  g_object_get (self, "is-remote-provider", &is_remote_provider, NULL);
  return is_remote_provider;
}

/**
 * shell_search_provider_get_max_results:
 * @self: a #ShellSearchProvider
 *
 * Returns: the maximum number of results of a grid, -1 for no maximum
 */
int
shell_search_provider_get_max_results (ShellSearchProvider *self)
{
  int max_results = -1;

  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), -1);

  g_object_get (self, "max-results", &max_results, NULL);
  return max_results;
}

static GTask *
provider_start_request (ShellSearchProvider *self,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data,
                        gpointer             source_tag,
                        RequestKind          kind,
                        ShellSearchRequest **request)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  g_task_set_source_tag (task, source_tag);
  *request = shell_search_request_new (task, kind);

  return task;
}

/**
 * shell_search_provider_get_initial_result_set_async:
 * @self: a #ShellSearchProvider
 * @terms: (array zero-terminated=1): the search terms
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the results are
 *   known
 * @user_data: data for @callback
 *
 * Asks the provider for the results of a search for @terms
 * (`getInitialResultSet ()`).
 */
void
shell_search_provider_get_initial_result_set_async (ShellSearchProvider *self,
                                                    const char * const  *terms,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data)
{
  ShellSearchProviderInterface *iface;
  g_autoptr (ShellSearchRequest) request = NULL;
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (self));

  task = provider_start_request (self, cancellable, callback, user_data,
                                 shell_search_provider_get_initial_result_set_async,
                                 REQUEST_KIND_RESULTS, &request);

  iface = SHELL_SEARCH_PROVIDER_GET_IFACE (self);
  iface->get_initial_result_set (self, terms, cancellable, request);
}

/**
 * shell_search_provider_get_initial_result_set_finish:
 * @self: a #ShellSearchProvider
 * @result: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (array zero-terminated=1): the identifiers of the
 *   results, or %NULL with @error set
 */
char **
shell_search_provider_get_initial_result_set_finish (ShellSearchProvider *self,
                                                     GAsyncResult        *result,
                                                     GError             **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * shell_search_provider_get_subsearch_result_set_async:
 * @self: a #ShellSearchProvider
 * @previous_results: (array zero-terminated=1): the results of the previous
 *   search
 * @terms: (array zero-terminated=1): the search terms, which narrow those of
 *   the previous search
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the results are
 *   known
 * @user_data: data for @callback
 *
 * Asks the provider for the results of a search which narrows the previous
 * one (`getSubsearchResultSet ()`).
 */
void
shell_search_provider_get_subsearch_result_set_async (ShellSearchProvider *self,
                                                      const char * const  *previous_results,
                                                      const char * const  *terms,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data)
{
  ShellSearchProviderInterface *iface;
  g_autoptr (ShellSearchRequest) request = NULL;
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (self));

  task = provider_start_request (self, cancellable, callback, user_data,
                                 shell_search_provider_get_subsearch_result_set_async,
                                 REQUEST_KIND_RESULTS, &request);

  iface = SHELL_SEARCH_PROVIDER_GET_IFACE (self);
  iface->get_subsearch_result_set (self, previous_results, terms, cancellable, request);
}

/**
 * shell_search_provider_get_subsearch_result_set_finish:
 * @self: a #ShellSearchProvider
 * @result: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (array zero-terminated=1): the identifiers of the
 *   results, or %NULL with @error set
 */
char **
shell_search_provider_get_subsearch_result_set_finish (ShellSearchProvider *self,
                                                       GAsyncResult        *result,
                                                       GError             **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * shell_search_provider_get_result_metas_async:
 * @self: a #ShellSearchProvider
 * @ids: (array zero-terminated=1): the identifiers of the results
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the metas are
 *   known
 * @user_data: data for @callback
 *
 * Asks the provider for the metadata used to show results
 * (`getResultMetas ()`).
 */
void
shell_search_provider_get_result_metas_async (ShellSearchProvider *self,
                                              const char * const  *ids,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data)
{
  ShellSearchProviderInterface *iface;
  g_autoptr (ShellSearchRequest) request = NULL;
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (self));

  task = provider_start_request (self, cancellable, callback, user_data,
                                 shell_search_provider_get_result_metas_async,
                                 REQUEST_KIND_METAS, &request);

  iface = SHELL_SEARCH_PROVIDER_GET_IFACE (self);
  iface->get_result_metas (self, ids, cancellable, request);
}

/**
 * shell_search_provider_get_result_metas_finish:
 * @self: a #ShellSearchProvider
 * @result: a #GAsyncResult
 * @n_metas: (out): return location for the number of metas
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (array length=n_metas): the metas of the
 *   results, or %NULL with @error set
 */
ShellSearchResultMeta **
shell_search_provider_get_result_metas_finish (ShellSearchProvider *self,
                                               GAsyncResult        *result,
                                               gsize               *n_metas,
                                               GError             **error)
{
  MetasResult *metas_result;
  ShellSearchResultMeta **metas;

  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  metas_result = g_task_propagate_pointer (G_TASK (result), error);
  if (metas_result == NULL)
    {
      if (n_metas != NULL)
        *n_metas = 0;
      return NULL;
    }

  metas = metas_result->metas;
  if (n_metas != NULL)
    *n_metas = metas_result->n_metas;

  g_free (metas_result);
  return metas;
}

/**
 * shell_search_provider_activate_result:
 * @self: a #ShellSearchProvider
 * @id: the identifier of the result
 * @terms: (array zero-terminated=1): the search terms
 *
 * Tells the provider that the user activated a result
 * (`activateResult ()`).
 */
void
shell_search_provider_activate_result (ShellSearchProvider *self,
                                       const char          *id,
                                       const char * const  *terms)
{
  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (self));

  SHELL_SEARCH_PROVIDER_GET_IFACE (self)->activate_result (self, id, terms);
}

/**
 * shell_search_provider_launch_search:
 * @self: a #ShellSearchProvider
 * @terms: (array zero-terminated=1): the search terms
 *
 * Asks the provider to show all the results of a search in its application
 * (`launchSearch ()`).
 */
void
shell_search_provider_launch_search (ShellSearchProvider *self,
                                     const char * const  *terms)
{
  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (self));

  SHELL_SEARCH_PROVIDER_GET_IFACE (self)->launch_search (self, terms);
}

/**
 * shell_search_provider_filter_results:
 * @self: a #ShellSearchProvider
 * @results: (array zero-terminated=1): the identifiers of the results
 * @max_number: the number of results to show
 *
 * Limits @results to what is shown (`filterResults ()`).
 *
 * Returns: (transfer full) (array zero-terminated=1): the results to show
 */
char **
shell_search_provider_filter_results (ShellSearchProvider *self,
                                      const char * const  *results,
                                      int                  max_number)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), NULL);

  return SHELL_SEARCH_PROVIDER_GET_IFACE (self)->filter_results (self, results, max_number);
}

/**
 * shell_search_provider_create_result_object:
 * @self: a #ShellSearchProvider
 * @meta: the meta of the result
 *
 * Lets the provider make the actor that shows a result (`createResultObject ()`).
 *
 * Returns: (transfer full) (nullable): the actor, or %NULL to use the default
 *   result of the section
 */
ClutterActor *
shell_search_provider_create_result_object (ShellSearchProvider   *self,
                                            ShellSearchResultMeta *meta)
{
  ClutterActor *actor;

  g_return_val_if_fail (SHELL_IS_SEARCH_PROVIDER (self), NULL);

  actor = SHELL_SEARCH_PROVIDER_GET_IFACE (self)->create_result_object (self, meta);
  if (actor != NULL && g_object_is_floating (actor))
    g_object_ref_sink (actor);

  return actor;
}

/* ------------------------------------------------------------------------ */
/* ShellSearchEntry                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  ENTRY_ACTIVATE_NEW_INSTANCE,
  ENTRY_LAST_SIGNAL
};

static guint entry_signals[ENTRY_LAST_SIGNAL] = { 0, };

struct _ShellSearchEntry
{
  StEntry parent;
};

G_DEFINE_FINAL_TYPE (ShellSearchEntry, shell_search_entry, ST_TYPE_ENTRY)

static gboolean
activate_new_instance_binding_cb (GObject             *object,
                                  const char          *action_name,
                                  guint                key_val,
                                  ClutterModifierType  modifiers,
                                  gpointer             user_data)
{
  g_signal_emit (object, entry_signals[ENTRY_ACTIVATE_NEW_INSTANCE], 0);
  return CLUTTER_EVENT_STOP;
}

static void
shell_search_entry_class_init (ShellSearchEntryClass *klass)
{
  ClutterBindingPool *binding_pool;

  /**
   * ShellSearchEntry::activate-new-instance:
   * @self: the entry
   *
   * Emitted when Ctrl+Enter is pressed: the default result should be
   * activated as in a new window.
   */
  entry_signals[ENTRY_ACTIVATE_NEW_INSTANCE] =
    g_signal_new ("activate-new-instance",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool,
                                       "activate-new-instance",
                                       CLUTTER_KEY_Return,
                                       CLUTTER_CONTROL_MASK,
                                       G_CALLBACK (activate_new_instance_binding_cb),
                                       NULL,
                                       NULL);
}

static void
shell_search_entry_init (ShellSearchEntry *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellSearchResultsView, as far as the results need it                     */
/* ------------------------------------------------------------------------ */

static void request_overview_toggle (ShellSearchResultsView *view);

/* ------------------------------------------------------------------------ */
/* ShellSearchResult                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  RESULT_PROP_0,

  RESULT_PROP_PROVIDER,
  RESULT_PROP_META_INFO,
  RESULT_PROP_RESULTS_VIEW,

  RESULT_N_PROPS
};

static GParamSpec *result_props[RESULT_N_PROPS] = { NULL, };

typedef struct
{
  ShellSearchProvider *provider;
  ShellSearchResultMeta *meta_info;
  GWeakRef results_view;
} ShellSearchResultPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellSearchResult, shell_search_result, ST_TYPE_BUTTON)

static void
shell_search_result_real_activate (ShellSearchResult *self)
{
  ShellSearchResultPrivate *priv = shell_search_result_get_instance_private (self);
  g_autoptr (ShellSearchResultsView) view = g_weak_ref_get (&priv->results_view);
  g_auto (GStrv) terms = NULL;

  if (view != NULL)
    terms = shell_search_results_view_get_terms (view);

  if (priv->provider != NULL && priv->meta_info != NULL)
    shell_search_provider_activate_result (priv->provider,
                                           shell_search_result_meta_get_id (priv->meta_info),
                                           (const char * const *) terms);

  if (priv->meta_info != NULL &&
      shell_search_result_meta_get_clipboard_text (priv->meta_info) != NULL &&
      *shell_search_result_meta_get_clipboard_text (priv->meta_info) != '\0')
    {
      st_clipboard_set_text (st_clipboard_get_default (),
                             ST_CLIPBOARD_TYPE_CLIPBOARD,
                             shell_search_result_meta_get_clipboard_text (priv->meta_info));
    }

  if (view != NULL)
    request_overview_toggle (view);
}

static void
shell_search_result_clicked (StButton *button,
                             int       clicked_button)
{
  shell_search_result_activate (SHELL_SEARCH_RESULT (button));
}

static void
shell_search_result_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellSearchResult *self = SHELL_SEARCH_RESULT (object);
  ShellSearchResultPrivate *priv = shell_search_result_get_instance_private (self);

  switch (prop_id)
    {
    case RESULT_PROP_PROVIDER:
      g_set_object (&priv->provider, g_value_get_object (value));
      break;

    case RESULT_PROP_META_INFO:
      g_set_object (&priv->meta_info, g_value_get_object (value));
      break;

    case RESULT_PROP_RESULTS_VIEW:
      g_weak_ref_set (&priv->results_view, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_result_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellSearchResult *self = SHELL_SEARCH_RESULT (object);
  ShellSearchResultPrivate *priv = shell_search_result_get_instance_private (self);

  switch (prop_id)
    {
    case RESULT_PROP_PROVIDER:
      g_value_set_object (value, priv->provider);
      break;

    case RESULT_PROP_META_INFO:
      g_value_set_object (value, priv->meta_info);
      break;

    case RESULT_PROP_RESULTS_VIEW:
      g_value_take_object (value, g_weak_ref_get (&priv->results_view));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_result_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_search_result_parent_class)->constructed (object);

  clutter_actor_set_reactive (CLUTTER_ACTOR (object), TRUE);
  st_widget_set_can_focus (ST_WIDGET (object), TRUE);
  st_widget_set_track_hover (ST_WIDGET (object), TRUE);
}

static void
shell_search_result_dispose (GObject *object)
{
  ShellSearchResult *self = SHELL_SEARCH_RESULT (object);
  ShellSearchResultPrivate *priv = shell_search_result_get_instance_private (self);

  g_clear_object (&priv->provider);
  g_clear_object (&priv->meta_info);
  g_weak_ref_set (&priv->results_view, NULL);

  G_OBJECT_CLASS (shell_search_result_parent_class)->dispose (object);
}

static void
shell_search_result_finalize (GObject *object)
{
  ShellSearchResult *self = SHELL_SEARCH_RESULT (object);
  ShellSearchResultPrivate *priv = shell_search_result_get_instance_private (self);

  g_weak_ref_clear (&priv->results_view);

  G_OBJECT_CLASS (shell_search_result_parent_class)->finalize (object);
}

static void
shell_search_result_class_init (ShellSearchResultClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->constructed = shell_search_result_constructed;
  object_class->set_property = shell_search_result_set_property;
  object_class->get_property = shell_search_result_get_property;
  object_class->dispose = shell_search_result_dispose;
  object_class->finalize = shell_search_result_finalize;

  button_class->clicked = shell_search_result_clicked;

  klass->activate = shell_search_result_real_activate;

  /**
   * ShellSearchResult:provider:
   *
   * The provider of the result (`provider`).
   */
  result_props[RESULT_PROP_PROVIDER] =
    g_param_spec_object ("provider", NULL, NULL,
                         SHELL_TYPE_SEARCH_PROVIDER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchResult:meta-info:
   *
   * The meta of the result (`metaInfo`).
   */
  result_props[RESULT_PROP_META_INFO] =
    g_param_spec_object ("meta-info", NULL, NULL,
                         SHELL_TYPE_SEARCH_RESULT_META,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchResult:results-view:
   *
   * The view the result is shown in, or %NULL for a result a provider makes
   * itself (`_resultsView`). It is held weakly.
   */
  result_props[RESULT_PROP_RESULTS_VIEW] =
    g_param_spec_object ("results-view", NULL, NULL,
                         SHELL_TYPE_SEARCH_RESULTS_VIEW,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, RESULT_N_PROPS, result_props);
}

static void
shell_search_result_init (ShellSearchResult *self)
{
  ShellSearchResultPrivate *priv = shell_search_result_get_instance_private (self);

  g_weak_ref_init (&priv->results_view, NULL);
}

/**
 * shell_search_result_get_provider:
 * @self: a #ShellSearchResult
 *
 * Returns: (transfer none) (nullable): the provider of the result
 */
ShellSearchProvider *
shell_search_result_get_provider (ShellSearchResult *self)
{
  ShellSearchResultPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT (self), NULL);

  priv = shell_search_result_get_instance_private (self);
  return priv->provider;
}

/**
 * shell_search_result_get_meta_info:
 * @self: a #ShellSearchResult
 *
 * Returns: (transfer none) (nullable): the meta of the result
 */
ShellSearchResultMeta *
shell_search_result_get_meta_info (ShellSearchResult *self)
{
  ShellSearchResultPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT (self), NULL);

  priv = shell_search_result_get_instance_private (self);
  return priv->meta_info;
}

/**
 * shell_search_result_get_results_view:
 * @self: a #ShellSearchResult
 *
 * Returns: (transfer full) (nullable): the view the result is shown in
 */
ShellSearchResultsView *
shell_search_result_get_results_view (ShellSearchResult *self)
{
  ShellSearchResultPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SEARCH_RESULT (self), NULL);

  priv = shell_search_result_get_instance_private (self);
  return g_weak_ref_get (&priv->results_view);
}

/**
 * shell_search_result_activate:
 * @self: a #ShellSearchResult
 *
 * Activates the result (`activate ()`): the `activate` virtual function.
 */
void
shell_search_result_activate (ShellSearchResult *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_RESULT (self));

  SHELL_SEARCH_RESULT_GET_CLASS (self)->activate (self);
}

/* ------------------------------------------------------------------------ */
/* ShellListSearchResult                                                     */
/* ------------------------------------------------------------------------ */

struct _ShellListSearchResult
{
  ShellSearchResult parent;

  StLabel *description_label; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellListSearchResult, shell_list_search_result, SHELL_TYPE_SEARCH_RESULT)

static void
list_search_result_highlight_terms (ShellListSearchResult *self)
{
  ShellSearchResultMeta *meta = shell_search_result_get_meta_info (SHELL_SEARCH_RESULT (self));
  g_autoptr (ShellSearchResultsView) view =
    shell_search_result_get_results_view (SHELL_SEARCH_RESULT (self));
  const char *description = meta != NULL ? shell_search_result_meta_get_description (meta) : NULL;
  g_autofree char *first_line = NULL;
  g_autofree char *markup = NULL;
  const char *newline;

  if (self->description_label == NULL || description == NULL)
    return;

  newline = strchr (description, '\n');
  first_line = newline != NULL ? g_strndup (description, newline - description)
                               : g_strdup (description);

  if (view == NULL)
    {
      st_label_set_text (self->description_label, first_line);
      return;
    }

  markup = shell_search_results_view_highlight_terms (view, first_line);
  clutter_text_set_markup (st_label_get_clutter_text (self->description_label), markup);
}

static void
list_search_result_terms_changed_cb (ShellSearchResultsView *view,
                                     ShellListSearchResult  *self)
{
  list_search_result_highlight_terms (self);
}

static void
shell_list_search_result_constructed (GObject *object)
{
  ShellListSearchResult *self = SHELL_LIST_SEARCH_RESULT (object);
  ShellSearchResult *result = SHELL_SEARCH_RESULT (self);
  ShellSearchResultMeta *meta;
  g_autoptr (ShellSearchResultsView) view = NULL;
  ClutterActor *content, *title_box, *icon, *title;
  const char *description;

  G_OBJECT_CLASS (shell_list_search_result_parent_class)->constructed (object);

  meta = shell_search_result_get_meta_info (result);
  view = shell_search_result_get_results_view (result);

  st_widget_set_style_class_name (ST_WIDGET (self), "list-search-result");

  content = g_object_new (ST_TYPE_BOX_LAYOUT,
                          "style-class", "list-search-result-content",
                          "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                          "x-align", CLUTTER_ACTOR_ALIGN_START,
                          "x-expand", TRUE,
                          "y-expand", TRUE,
                          NULL);
  st_bin_set_child (ST_BIN (self), content);

  title_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "style-class", "list-search-result-title",
                            "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            NULL);
  clutter_actor_add_child (content, title_box);

  /* An icon for, or thumbnail of, content */
  icon = meta != NULL ? shell_search_result_meta_create_icon (meta, LIST_SEARCH_RESULT_ICON_SIZE)
                      : NULL;
  if (icon != NULL)
    {
      /* The icon is ours, floating or not */
      gboolean floating = g_object_is_floating (icon);

      clutter_actor_add_child (title_box, icon);
      if (!floating)
        g_object_unref (icon);
    }

  title = g_object_new (ST_TYPE_LABEL,
                        "text", meta != NULL ? shell_search_result_meta_get_name (meta) : NULL,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (title_box, title);
  st_widget_set_label_actor (ST_WIDGET (self), title);

  description = meta != NULL ? shell_search_result_meta_get_description (meta) : NULL;
  if (description != NULL && *description != '\0')
    {
      AtkObject *accessible, *description_accessible;

      self->description_label = g_object_new (ST_TYPE_LABEL,
                                              "style-class", "list-search-result-description",
                                              "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                              NULL);
      g_object_add_weak_pointer (G_OBJECT (self->description_label),
                                 (gpointer *) &self->description_label);
      clutter_actor_add_child (content, CLUTTER_ACTOR (self->description_label));

      accessible = clutter_actor_get_accessible (CLUTTER_ACTOR (self));
      description_accessible =
        clutter_actor_get_accessible (CLUTTER_ACTOR (self->description_label));
      atk_object_add_relationship (accessible, ATK_RELATION_DESCRIBED_BY,
                                   description_accessible);

      if (view != NULL)
        g_signal_connect_object (view, "terms-changed",
                                 G_CALLBACK (list_search_result_terms_changed_cb),
                                 self, 0);

      list_search_result_highlight_terms (self);
    }
}

static void
shell_list_search_result_dispose (GObject *object)
{
  ShellListSearchResult *self = SHELL_LIST_SEARCH_RESULT (object);

  if (self->description_label != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->description_label),
                                    (gpointer *) &self->description_label);
      self->description_label = NULL;
    }

  G_OBJECT_CLASS (shell_list_search_result_parent_class)->dispose (object);
}

static void
shell_list_search_result_class_init (ShellListSearchResultClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_list_search_result_constructed;
  object_class->dispose = shell_list_search_result_dispose;
}

static void
shell_list_search_result_init (ShellListSearchResult *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellGridSearchResult                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellBaseIcon *icon; /* weak */
} ShellGridSearchResultPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellGridSearchResult, shell_grid_search_result,
                            SHELL_TYPE_SEARCH_RESULT)

static ClutterActor *
grid_result_create_icon_cb (int      size,
                            gpointer user_data)
{
  return shell_search_result_meta_create_icon (user_data, size);
}

static void
shell_grid_search_result_constructed (GObject *object)
{
  ShellGridSearchResult *self = SHELL_GRID_SEARCH_RESULT (object);
  ShellGridSearchResultPrivate *priv = shell_grid_search_result_get_instance_private (self);
  ShellSearchResultMeta *meta;
  ClutterActor *content;
  StLabel *label;

  G_OBJECT_CLASS (shell_grid_search_result_parent_class)->constructed (object);

  meta = shell_search_result_get_meta_info (SHELL_SEARCH_RESULT (self));

  st_widget_set_style_class_name (ST_WIDGET (self), "grid-search-result");

  priv->icon = g_object_new (SHELL_TYPE_BASE_ICON,
                             "label-text", meta != NULL ? shell_search_result_meta_get_name (meta) : NULL,
                             NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
  if (meta != NULL)
    shell_base_icon_set_create_icon_func (priv->icon, grid_result_create_icon_cb,
                                          g_object_ref (meta), g_object_unref);

  content = g_object_new (ST_TYPE_BIN,
                          "child", priv->icon,
                          "x-align", CLUTTER_ACTOR_ALIGN_START,
                          "x-expand", TRUE,
                          "y-expand", TRUE,
                          NULL);
  st_bin_set_child (ST_BIN (self), content);

  label = shell_base_icon_get_label (priv->icon);
  if (label != NULL)
    st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (label));
}

static void
shell_grid_search_result_dispose (GObject *object)
{
  ShellGridSearchResult *self = SHELL_GRID_SEARCH_RESULT (object);
  ShellGridSearchResultPrivate *priv = shell_grid_search_result_get_instance_private (self);

  if (priv->icon != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
      priv->icon = NULL;
    }

  G_OBJECT_CLASS (shell_grid_search_result_parent_class)->dispose (object);
}

static void
shell_grid_search_result_class_init (ShellGridSearchResultClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_grid_search_result_constructed;
  object_class->dispose = shell_grid_search_result_dispose;
}

static void
shell_grid_search_result_init (ShellGridSearchResult *self)
{
}

/**
 * shell_grid_search_result_get_icon:
 * @self: a #ShellGridSearchResult
 *
 * Returns: (transfer none) (nullable): the icon of the result (`icon`)
 */
ShellBaseIcon *
shell_grid_search_result_get_icon (ShellGridSearchResult *self)
{
  ShellGridSearchResultPrivate *priv;

  g_return_val_if_fail (SHELL_IS_GRID_SEARCH_RESULT (self), NULL);

  priv = shell_grid_search_result_get_instance_private (self);
  return priv->icon;
}

/* ------------------------------------------------------------------------ */
/* ShellSearchResultsBase (private)                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_RESULTS_BASE (shell_search_results_base_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellSearchResultsBase, shell_search_results_base,
                          SHELL, SEARCH_RESULTS_BASE, StBoxLayout)

struct _ShellSearchResultsBaseClass
{
  StBoxLayoutClass parent_class;

  void          (* before_update_search)      (ShellSearchResultsBase *self,
                                               const char * const     *results,
                                               const char * const     *terms);
  void          (* set_more_count)            (ShellSearchResultsBase *self,
                                               int                     count);
  int           (* get_max_displayed_results) (ShellSearchResultsBase *self);
  void          (* clear_result_display)      (ShellSearchResultsBase *self);
  ClutterActor *(* create_result_display)     (ShellSearchResultsBase *self,
                                               ShellSearchResultMeta  *meta);
  void          (* add_item)                  (ShellSearchResultsBase *self,
                                               ClutterActor           *display);
  ClutterActor *(* get_first_result)          (ShellSearchResultsBase *self);
};

enum
{
  BASE_PROP_0,

  BASE_PROP_PROVIDER,
  BASE_PROP_RESULTS_VIEW,
  BASE_PROP_FOCUS_CHILD,

  BASE_N_PROPS
};

static GParamSpec *base_props[BASE_N_PROPS] = { NULL, };

typedef struct
{
  ShellSearchProvider *provider;
  GWeakRef results_view;

  char **terms;
  ClutterActor *focus_child; /* weak */

  StBin *result_display_bin; /* weak */
  GHashTable *result_displays; /* result id -> ClutterActor, strong */
  GCancellable *cancellable;

  gboolean destroyed;
} ShellSearchResultsBasePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellSearchResultsBase, shell_search_results_base,
                                     ST_TYPE_BOX_LAYOUT)

typedef struct
{
  char **results;      /* what is shown */
  guint more_count;
  char **metas_needed;
  GCancellable *cancellable;
} UpdateData;

static void
update_data_free (UpdateData *data)
{
  g_strfreev (data->results);
  g_strfreev (data->metas_needed);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static ShellSearchProvider *
base_get_provider (ShellSearchResultsBase *self)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  return priv->provider;
}

/* Returns the view with a new reference */
static ShellSearchResultsView *
base_get_results_view (ShellSearchResultsBase *self)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  return g_weak_ref_get (&priv->results_view);
}

static const char * const *
base_get_terms (ShellSearchResultsBase *self)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);
  static const char * const no_terms[] = { NULL };

  return priv->terms != NULL ? (const char * const *) priv->terms : no_terms;
}

static void
base_key_focus_in (ShellSearchResultsBase *self,
                   ClutterActor           *actor)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  if (priv->focus_child == actor)
    return;

  g_set_weak_pointer (&priv->focus_child, actor);
  g_object_notify_by_pspec (G_OBJECT (self), base_props[BASE_PROP_FOCUS_CHILD]);
}

static void
key_focus_in_cb (ClutterActor           *actor,
                 ShellSearchResultsBase *self)
{
  base_key_focus_in (self, actor);
}

static void
search_results_base_clear (ShellSearchResultsBase *self)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);
  GList *displays;

  g_cancellable_cancel (priv->cancellable);

  displays = g_hash_table_get_values (priv->result_displays);
  g_list_foreach (displays, (GFunc) g_object_ref, NULL);
  g_hash_table_remove_all (priv->result_displays);
  for (GList *l = displays; l != NULL; l = l->next)
    clutter_actor_destroy (l->data);
  g_list_free_full (displays, g_object_unref);

  SHELL_SEARCH_RESULTS_BASE_GET_CLASS (self)->clear_result_display (self);
  clutter_actor_hide (CLUTTER_ACTOR (self));
}

static void
search_results_base_cancel (ShellSearchResultsBase *self)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  g_cancellable_cancel (priv->cancellable);
}

static ClutterActor *
search_results_base_get_first_result (ShellSearchResultsBase *self)
{
  return SHELL_SEARCH_RESULTS_BASE_GET_CLASS (self)->get_first_result (self);
}

static ClutterActor *
real_create_result_display (ShellSearchResultsBase *self,
                            ShellSearchResultMeta  *meta)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  return shell_search_provider_create_result_object (priv->provider, meta);
}

/* Creates the result actors of the metas and shows the results */
static void
search_results_base_apply (ShellSearchResultsBase *self,
                           UpdateData             *data)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);
  ShellSearchResultsBaseClass *klass = SHELL_SEARCH_RESULTS_BASE_GET_CLASS (self);

  /* To avoid CSS transitions causing flickering when the first search result
   * stays the same, we hide the content while filling in the results. */
  clutter_actor_hide (CLUTTER_ACTOR (self));
  klass->clear_result_display (self);

  for (int i = 0; data->results[i] != NULL; i++)
    {
      ClutterActor *display = g_hash_table_lookup (priv->result_displays, data->results[i]);

      if (display != NULL)
        klass->add_item (self, display);
    }

  klass->set_more_count (self,
                         shell_search_provider_get_can_launch_search (priv->provider)
                           ? data->more_count : 0);
  clutter_actor_show (CLUTTER_ACTOR (self));
}

static void
search_results_base_metas_ready_cb (GObject      *source,
                                    GAsyncResult *res,
                                    gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellSearchResultsBase *self = g_task_get_source_object (task);
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);
  ShellSearchResultsBaseClass *klass = SHELL_SEARCH_RESULTS_BASE_GET_CLASS (self);
  UpdateData *data = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  g_autofree char *provider_id = NULL;
  ShellSearchResultMeta **metas;
  gsize n_metas = 0;
  gboolean ok = FALSE;

  metas = shell_search_provider_get_result_metas_finish (SHELL_SEARCH_PROVIDER (source),
                                                         res, &n_metas, &error);

  if (priv->destroyed)
    {
      for (gsize i = 0; metas != NULL && i < n_metas; i++)
        g_object_unref (metas[i]);
      g_free (metas);
      g_task_return_boolean (task, TRUE);
      return;
    }

  provider_id = shell_search_provider_get_id (priv->provider);

  if (metas == NULL)
    {
      if (!error_is_cancelled (error))
        g_message ("Search provider %s could not get the result metas: %s",
                   provider_id, error->message);
    }
  else if (g_cancellable_is_cancelled (data->cancellable))
    {
      /* Replaced by a newer request while the answer was on its way */
    }
  else if (n_metas != g_strv_length (data->metas_needed))
    {
      g_warning ("Wrong number of result metas returned by search provider %s: "
                 "expected %u but got %" G_GSIZE_FORMAT,
                 provider_id, g_strv_length (data->metas_needed), n_metas);
    }
  else
    {
      gboolean valid = TRUE;

      for (gsize i = 0; i < n_metas; i++)
        {
          const char *id = shell_search_result_meta_get_id (metas[i]);
          const char *name = shell_search_result_meta_get_name (metas[i]);

          if (id == NULL || *id == '\0' || name == NULL || *name == '\0')
            valid = FALSE;
        }

      if (!valid)
        {
          g_warning ("Invalid result meta returned from search provider %s", provider_id);
        }
      else
        {
          for (gsize i = 0; i < n_metas; i++)
            {
              ClutterActor *display = klass->create_result_display (self, metas[i]);

              if (display == NULL)
                continue;

              g_signal_connect_object (display, "key-focus-in",
                                       G_CALLBACK (key_focus_in_cb), self, 0);
              g_hash_table_replace (priv->result_displays,
                                    g_strdup (data->metas_needed[i]),
                                    display);
            }

          ok = TRUE;
        }
    }

  for (gsize i = 0; metas != NULL && i < n_metas; i++)
    g_object_unref (metas[i]);
  g_free (metas);

  if (ok)
    search_results_base_apply (self, data);
  else
    klass->clear_result_display (self);

  g_task_return_boolean (task, TRUE);
}

/* Shows the results @provider_results of a search for @terms. Errors are
 * handled here; the callback is called when the section is up to date. */
static void
search_results_base_do_update_search (ShellSearchResultsBase *self,
                                      const char * const     *provider_results,
                                      const char * const     *terms,
                                      GAsyncReadyCallback     callback,
                                      gpointer                user_data)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);
  ShellSearchResultsBaseClass *klass = SHELL_SEARCH_RESULTS_BASE_GET_CLASS (self);
  g_autoptr (GTask) task = NULL;
  g_autoptr (GPtrArray) needed = NULL;
  UpdateData *data;
  int max_results;
  guint n_provider_results;

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, search_results_base_do_update_search);

  g_strfreev (priv->terms);
  priv->terms = strv_dup_or_empty (terms);

  n_provider_results = provider_results != NULL ? g_strv_length ((char **) provider_results) : 0;
  if (n_provider_results == 0 || priv->destroyed)
    {
      if (!priv->destroyed)
        {
          klass->clear_result_display (self);
          clutter_actor_hide (CLUTTER_ACTOR (self));
        }

      g_task_return_boolean (task, TRUE);
      return;
    }

  data = g_new0 (UpdateData, 1);
  g_task_set_task_data (task, data, (GDestroyNotify) update_data_free);

  max_results = klass->get_max_displayed_results (self);
  if (max_results > -1)
    data->results = shell_search_provider_filter_results (priv->provider, provider_results,
                                                          max_results);
  else
    data->results = g_strdupv ((char **) provider_results);
  if (data->results == NULL)
    data->results = strv_new_empty ();

  data->more_count = MAX ((int) n_provider_results - (int) g_strv_length (data->results), 0);

  needed = g_ptr_array_new ();
  for (int i = 0; data->results[i] != NULL; i++)
    {
      if (!g_hash_table_contains (priv->result_displays, data->results[i]))
        g_ptr_array_add (needed, data->results[i]);
    }

  if (needed->len == 0)
    {
      search_results_base_apply (self, data);
      g_task_return_boolean (task, TRUE);
      return;
    }

  data->metas_needed = g_new0 (char *, needed->len + 1);
  for (guint i = 0; i < needed->len; i++)
    data->metas_needed[i] = g_strdup (g_ptr_array_index (needed, i));

  g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);
  priv->cancellable = g_cancellable_new ();
  data->cancellable = g_object_ref (priv->cancellable);

  shell_search_provider_get_result_metas_async (priv->provider,
                                                (const char * const *) data->metas_needed,
                                                data->cancellable,
                                                search_results_base_metas_ready_cb,
                                                g_object_ref (task));
}

static void
search_results_base_update_search_async (ShellSearchResultsBase *self,
                                         const char * const     *provider_results,
                                         const char * const     *terms,
                                         GAsyncReadyCallback     callback,
                                         gpointer                user_data)
{
  ShellSearchResultsBaseClass *klass = SHELL_SEARCH_RESULTS_BASE_GET_CLASS (self);

  if (klass->before_update_search != NULL)
    klass->before_update_search (self, provider_results, terms);

  search_results_base_do_update_search (self, provider_results, terms, callback, user_data);
}

static void
shell_search_results_base_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellSearchResultsBase *self = SHELL_SEARCH_RESULTS_BASE (object);
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  switch (prop_id)
    {
    case BASE_PROP_PROVIDER:
      g_set_object (&priv->provider, g_value_get_object (value));
      break;

    case BASE_PROP_RESULTS_VIEW:
      g_weak_ref_set (&priv->results_view, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_results_base_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellSearchResultsBase *self = SHELL_SEARCH_RESULTS_BASE (object);
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  switch (prop_id)
    {
    case BASE_PROP_PROVIDER:
      g_value_set_object (value, priv->provider);
      break;

    case BASE_PROP_RESULTS_VIEW:
      g_value_take_object (value, g_weak_ref_get (&priv->results_view));
      break;

    case BASE_PROP_FOCUS_CHILD:
      g_value_set_object (value, priv->focus_child);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_results_base_constructed (GObject *object)
{
  ShellSearchResultsBase *self = SHELL_SEARCH_RESULTS_BASE (object);
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);
  ClutterActor *separator;

  G_OBJECT_CLASS (shell_search_results_base_parent_class)->constructed (object);

  /* The layout manager of a St.BoxLayout exists from constructed () on */
  st_widget_set_style_class_name (ST_WIDGET (self), "search-section");
  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);

  priv->result_display_bin = ST_BIN (st_bin_new ());
  g_object_add_weak_pointer (G_OBJECT (priv->result_display_bin),
                             (gpointer *) &priv->result_display_bin);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (priv->result_display_bin));

  separator = g_object_new (ST_TYPE_WIDGET,
                            "style-class", "search-section-separator",
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), separator);
}

static void
shell_search_results_base_dispose (GObject *object)
{
  ShellSearchResultsBase *self = SHELL_SEARCH_RESULTS_BASE (object);
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  priv->destroyed = TRUE;
  g_cancellable_cancel (priv->cancellable);
  g_clear_pointer (&priv->terms, g_strfreev);

  if (priv->result_display_bin != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (priv->result_display_bin),
                                    (gpointer *) &priv->result_display_bin);
      priv->result_display_bin = NULL;
    }

  g_clear_weak_pointer (&priv->focus_child);

  G_OBJECT_CLASS (shell_search_results_base_parent_class)->dispose (object);

  g_hash_table_remove_all (priv->result_displays);
  g_clear_object (&priv->provider);
  g_weak_ref_set (&priv->results_view, NULL);
}

static void
shell_search_results_base_finalize (GObject *object)
{
  ShellSearchResultsBase *self = SHELL_SEARCH_RESULTS_BASE (object);
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  g_clear_pointer (&priv->result_displays, g_hash_table_unref);
  g_clear_object (&priv->cancellable);
  g_weak_ref_clear (&priv->results_view);

  G_OBJECT_CLASS (shell_search_results_base_parent_class)->finalize (object);
}

static void
shell_search_results_base_class_init (ShellSearchResultsBaseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_search_results_base_constructed;
  object_class->set_property = shell_search_results_base_set_property;
  object_class->get_property = shell_search_results_base_get_property;
  object_class->dispose = shell_search_results_base_dispose;
  object_class->finalize = shell_search_results_base_finalize;

  klass->create_result_display = real_create_result_display;

  base_props[BASE_PROP_PROVIDER] =
    g_param_spec_object ("provider", NULL, NULL,
                         SHELL_TYPE_SEARCH_PROVIDER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  base_props[BASE_PROP_RESULTS_VIEW] =
    g_param_spec_object ("results-view", NULL, NULL,
                         SHELL_TYPE_SEARCH_RESULTS_VIEW,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  base_props[BASE_PROP_FOCUS_CHILD] =
    g_param_spec_object ("focus-child", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, BASE_N_PROPS, base_props);
}

static void
shell_search_results_base_init (ShellSearchResultsBase *self)
{
  ShellSearchResultsBasePrivate *priv = shell_search_results_base_get_instance_private (self);

  g_weak_ref_init (&priv->results_view, NULL);
  priv->result_displays = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_object_unref);
  priv->cancellable = g_cancellable_new ();
}

/* ------------------------------------------------------------------------ */
/* ShellListSearchResults (private)                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LIST_SEARCH_RESULTS (shell_list_search_results_get_type ())
G_DECLARE_FINAL_TYPE (ShellListSearchResults, shell_list_search_results,
                      SHELL, LIST_SEARCH_RESULTS, ShellSearchResultsBase)

struct _ShellListSearchResults
{
  ShellSearchResultsBase parent;

  ClutterActor *content;      /* weak */
  ClutterActor *info_content; /* weak, the content of the provider's button */
  StLabel *more_label;        /* weak */
};

G_DEFINE_FINAL_TYPE (ShellListSearchResults, shell_list_search_results,
                     SHELL_TYPE_SEARCH_RESULTS_BASE)

static void
list_results_animate_launch (ShellListSearchResults *self)
{
  g_autoptr (GAppInfo) app_info =
    shell_search_provider_get_app_info (base_get_provider (SHELL_SEARCH_RESULTS_BASE (self)));
  ShellAppSystem *app_system = shell_app_system_get_default ();
  ShellApp *app;

  if (app_info == NULL || g_app_info_get_id (app_info) == NULL || self->info_content == NULL)
    return;

  app = shell_app_system_lookup_app (app_system, g_app_info_get_id (app_info));
  if (app != NULL && shell_app_get_state (app) == SHELL_APP_STATE_STOPPED)
    shell_icon_grid_zoom_out_actor (self->info_content);
}

static void
provider_info_clicked_cb (StButton               *button,
                          int                     clicked_button,
                          ShellListSearchResults *self)
{
  ShellSearchResultsBase *base = SHELL_SEARCH_RESULTS_BASE (self);
  g_autoptr (ShellSearchResultsView) view = base_get_results_view (base);

  list_results_animate_launch (self);
  shell_search_provider_launch_search (base_get_provider (base), base_get_terms (base));

  if (view != NULL)
    request_overview_toggle (view);
}

static void
list_results_set_more_count (ShellSearchResultsBase *base,
                             int                     count)
{
  ShellListSearchResults *self = SHELL_LIST_SEARCH_RESULTS (base);
  g_autofree char *text = NULL;

  if (self->more_label == NULL)
    return;

  text = g_strdup_printf (ngettext ("%d more", "%d more", count), count);
  st_label_set_text (self->more_label, text);
  g_object_set (self->more_label, "visible", count > 0, NULL);
}

static int
list_results_get_max_displayed_results (ShellSearchResultsBase *base)
{
  return MAX_LIST_SEARCH_RESULTS_ROWS;
}

static void
list_results_clear_result_display (ShellSearchResultsBase *base)
{
  ShellListSearchResults *self = SHELL_LIST_SEARCH_RESULTS (base);

  if (self->content != NULL)
    clutter_actor_remove_all_children (self->content);
}

static ClutterActor *
list_results_create_result_display (ShellSearchResultsBase *base,
                                    ShellSearchResultMeta  *meta)
{
  ShellSearchResultsBaseClass *parent_class =
    SHELL_SEARCH_RESULTS_BASE_CLASS (shell_list_search_results_parent_class);
  g_autoptr (ShellSearchResultsView) view = base_get_results_view (base);
  ClutterActor *display = parent_class->create_result_display (base, meta);

  /* Returns a new reference, not a floating one */

  if (display != NULL)
    return display;

  return g_object_ref_sink (g_object_new (SHELL_TYPE_LIST_SEARCH_RESULT,
                                          "provider", base_get_provider (base),
                                          "meta-info", meta,
                                          "results-view", view,
                                          NULL));
}

static void
list_results_add_item (ShellSearchResultsBase *base,
                       ClutterActor           *display)
{
  ShellListSearchResults *self = SHELL_LIST_SEARCH_RESULTS (base);

  if (self->content != NULL)
    clutter_actor_add_child (self->content, display);
}

static ClutterActor *
list_results_get_first_result (ShellSearchResultsBase *base)
{
  ShellListSearchResults *self = SHELL_LIST_SEARCH_RESULTS (base);

  if (self->content != NULL && clutter_actor_get_n_children (self->content) > 0)
    return clutter_actor_get_child_at_index (self->content, 0);

  return NULL;
}

static void
shell_list_search_results_constructed (GObject *object)
{
  ShellListSearchResults *self = SHELL_LIST_SEARCH_RESULTS (object);
  ShellSearchResultsBase *base = SHELL_SEARCH_RESULTS_BASE (self);
  ShellSearchResultsBasePrivate *base_priv = shell_search_results_base_get_instance_private (base);
  g_autoptr (GAppInfo) app_info = NULL;
  ClutterActor *container, *info, *icon, *details_box, *name_label, *more_label;

  G_OBJECT_CLASS (shell_list_search_results_parent_class)->constructed (object);

  container = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "style-class", "search-section-content",
                            "x-expand", TRUE,
                            NULL);

  app_info = shell_search_provider_get_app_info (base_priv->provider);
  if (app_info != NULL)
    {
      info = g_object_new (ST_TYPE_BUTTON,
                           "style-class", "search-provider-icon",
                           "reactive", TRUE,
                           "can-focus", TRUE,
                           "accessible-name", g_app_info_get_name (app_info),
                           "track-hover", TRUE,
                           "y-align", CLUTTER_ACTOR_ALIGN_START,
                           NULL);
      g_signal_connect_object (info, "key-focus-in",
                               G_CALLBACK (key_focus_in_cb), self, 0);
      g_signal_connect_object (info, "clicked",
                               G_CALLBACK (provider_info_clicked_cb), self, 0);

      self->info_content = g_object_new (ST_TYPE_BOX_LAYOUT,
                                         "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                         "style-class", "list-search-provider-content",
                                         NULL);
      g_object_add_weak_pointer (G_OBJECT (self->info_content),
                                 (gpointer *) &self->info_content);
      st_bin_set_child (ST_BIN (info), self->info_content);

      icon = g_object_new (ST_TYPE_ICON,
                           "icon-size", PROVIDER_ICON_SIZE,
                           "gicon", g_app_info_get_icon (app_info),
                           NULL);

      details_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                  "style-class", "list-search-provider-details",
                                  "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                  "x-expand", TRUE,
                                  "y-expand", FALSE,
                                  NULL);

      name_label = g_object_new (ST_TYPE_LABEL,
                                 "text", g_app_info_get_name (app_info),
                                 "x-align", CLUTTER_ACTOR_ALIGN_START,
                                 "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 "y-expand", TRUE,
                                 NULL);

      more_label = g_object_new (ST_TYPE_LABEL,
                                 "x-align", CLUTTER_ACTOR_ALIGN_START,
                                 NULL);
      self->more_label = ST_LABEL (more_label);
      g_object_add_weak_pointer (G_OBJECT (self->more_label), (gpointer *) &self->more_label);

      clutter_actor_add_child (details_box, name_label);
      clutter_actor_add_child (details_box, more_label);

      clutter_actor_add_child (self->info_content, icon);
      clutter_actor_add_child (self->info_content, details_box);

      clutter_actor_add_child (container, info);
    }

  self->content = g_object_new (ST_TYPE_BOX_LAYOUT,
                                "style-class", "list-search-results",
                                "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                "x-expand", TRUE,
                                NULL);
  g_object_add_weak_pointer (G_OBJECT (self->content), (gpointer *) &self->content);
  clutter_actor_add_child (container, self->content);

  st_bin_set_child (base_priv->result_display_bin, container);
}

static void
shell_list_search_results_dispose (GObject *object)
{
  ShellListSearchResults *self = SHELL_LIST_SEARCH_RESULTS (object);

  g_clear_weak_pointer (&self->content);
  g_clear_weak_pointer (&self->info_content);
  g_clear_weak_pointer (&self->more_label);

  G_OBJECT_CLASS (shell_list_search_results_parent_class)->dispose (object);
}

static void
shell_list_search_results_class_init (ShellListSearchResultsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSearchResultsBaseClass *base_class = SHELL_SEARCH_RESULTS_BASE_CLASS (klass);

  object_class->constructed = shell_list_search_results_constructed;
  object_class->dispose = shell_list_search_results_dispose;

  base_class->set_more_count = list_results_set_more_count;
  base_class->get_max_displayed_results = list_results_get_max_displayed_results;
  base_class->clear_result_display = list_results_clear_result_display;
  base_class->create_result_display = list_results_create_result_display;
  base_class->add_item = list_results_add_item;
  base_class->get_first_result = list_results_get_first_result;
}

static void
shell_list_search_results_init (ShellListSearchResults *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellGridSearchResultsLayout (private)                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_GRID_SEARCH_RESULTS_LAYOUT (shell_grid_search_results_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellGridSearchResultsLayout, shell_grid_search_results_layout,
                      SHELL, GRID_SEARCH_RESULTS_LAYOUT, ClutterLayoutManager)

enum
{
  LAYOUT_PROP_0,

  LAYOUT_PROP_SPACING,

  LAYOUT_N_PROPS
};

static GParamSpec *layout_props[LAYOUT_N_PROPS] = { NULL, };

struct _ShellGridSearchResultsLayout
{
  ClutterLayoutManager parent;

  ClutterActor *container; /* weak */
  int spacing;
};

G_DEFINE_FINAL_TYPE (ShellGridSearchResultsLayout, shell_grid_search_results_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
grid_layout_set_container (ClutterLayoutManager *manager,
                           ClutterActor         *container)
{
  ShellGridSearchResultsLayout *self = SHELL_GRID_SEARCH_RESULTS_LAYOUT (manager);

  g_set_weak_pointer (&self->container, container);
}

static void
grid_layout_get_preferred_width (ClutterLayoutManager *manager,
                                 ClutterActor         *container,
                                 float                 for_height,
                                 float                *min_width_p,
                                 float                *natural_width_p)
{
  ShellGridSearchResultsLayout *self = SHELL_GRID_SEARCH_RESULTS_LAYOUT (manager);
  float min_width = 0;
  float natural_width = 0;
  gboolean first = TRUE;

  for (ClutterActor *child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float child_min_width, child_natural_width;

      if (!clutter_actor_is_visible (child))
        continue;

      clutter_actor_get_preferred_width (child, for_height,
                                         &child_min_width, &child_natural_width);

      min_width = MAX (min_width, child_min_width);
      natural_width += child_natural_width;

      if (first)
        first = FALSE;
      else
        natural_width += self->spacing;
    }

  if (min_width_p)
    *min_width_p = min_width;
  if (natural_width_p)
    *natural_width_p = natural_width;
}

static void
grid_layout_get_preferred_height (ClutterLayoutManager *manager,
                                  ClutterActor         *container,
                                  float                 for_width,
                                  float                *min_height_p,
                                  float                *natural_height_p)
{
  float min_height = 0;
  float natural_height = 0;

  for (ClutterActor *child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float child_min_height, child_natural_height;

      if (!clutter_actor_is_visible (child))
        continue;

      clutter_actor_get_preferred_height (child, for_width,
                                          &child_min_height, &child_natural_height);

      min_height = MAX (min_height, child_min_height);
      natural_height = MAX (natural_height, child_natural_height);
    }

  if (min_height_p)
    *min_height_p = min_height;
  if (natural_height_p)
    *natural_height_p = natural_height;
}

static void
grid_layout_allocate (ClutterLayoutManager   *manager,
                      ClutterActor           *container,
                      const ClutterActorBox  *allocation)
{
  ShellGridSearchResultsLayout *self = SHELL_GRID_SEARCH_RESULTS_LAYOUT (manager);
  float width = clutter_actor_box_get_width (allocation);
  gboolean ltr = clutter_actor_get_text_direction (container) != CLUTTER_TEXT_DIRECTION_RTL;
  float accumulated_width = 0;
  gboolean first = TRUE;

  for (ClutterActor *child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      ClutterActorBox child_box;
      float child_width, child_height;
      float x;

      if (!clutter_actor_is_visible (child))
        continue;

      if (first)
        first = FALSE;
      else
        accumulated_width += self->spacing;

      /* The minimum sizes, as the JS took the first element of both */
      clutter_actor_get_preferred_width (child, -1, &child_width, NULL);
      clutter_actor_get_preferred_height (child, -1, &child_height, NULL);

      if (ltr)
        x = accumulated_width;
      else
        x = width - accumulated_width - child_width;

      accumulated_width += child_width;

      if (accumulated_width <= width)
        clutter_actor_box_init_rect (&child_box, x, 0, child_width, child_height);
      else
        clutter_actor_box_init_rect (&child_box, x, 0, 0, 0);

      clutter_actor_allocate (child, &child_box);
      if (ST_IS_WIDGET (child))
        st_widget_set_can_focus (ST_WIDGET (child),
                                 clutter_actor_box_get_area (&child_box) > 0);
    }
}

static void
grid_layout_set_spacing (ShellGridSearchResultsLayout *self,
                         int                           spacing)
{
  if (self->spacing == spacing)
    return;

  self->spacing = spacing;
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
  g_object_notify_by_pspec (G_OBJECT (self), layout_props[LAYOUT_PROP_SPACING]);
}

static int
grid_layout_columns_for_width (ShellGridSearchResultsLayout *self,
                               float                         width)
{
  float min_width = 0;
  int n_cols = 0;

  if (self->container == NULL)
    return -1;

  clutter_layout_manager_get_preferred_width (CLUTTER_LAYOUT_MANAGER (self),
                                              self->container, -1, &min_width, NULL);
  if (min_width == 0)
    return -1;

  while (width > min_width)
    {
      width -= min_width;
      if (n_cols > 0)
        width -= self->spacing;
      n_cols++;
    }

  return n_cols;
}

static void
shell_grid_search_results_layout_set_property (GObject      *object,
                                               guint         prop_id,
                                               const GValue *value,
                                               GParamSpec   *pspec)
{
  ShellGridSearchResultsLayout *self = SHELL_GRID_SEARCH_RESULTS_LAYOUT (object);

  switch (prop_id)
    {
    case LAYOUT_PROP_SPACING:
      grid_layout_set_spacing (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_grid_search_results_layout_get_property (GObject    *object,
                                               guint       prop_id,
                                               GValue     *value,
                                               GParamSpec *pspec)
{
  ShellGridSearchResultsLayout *self = SHELL_GRID_SEARCH_RESULTS_LAYOUT (object);

  switch (prop_id)
    {
    case LAYOUT_PROP_SPACING:
      g_value_set_int (value, self->spacing);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_grid_search_results_layout_dispose (GObject *object)
{
  ShellGridSearchResultsLayout *self = SHELL_GRID_SEARCH_RESULTS_LAYOUT (object);

  g_clear_weak_pointer (&self->container);

  G_OBJECT_CLASS (shell_grid_search_results_layout_parent_class)->dispose (object);
}

static void
shell_grid_search_results_layout_class_init (ShellGridSearchResultsLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->set_property = shell_grid_search_results_layout_set_property;
  object_class->get_property = shell_grid_search_results_layout_get_property;
  object_class->dispose = shell_grid_search_results_layout_dispose;

  layout_class->set_container = grid_layout_set_container;
  layout_class->get_preferred_width = grid_layout_get_preferred_width;
  layout_class->get_preferred_height = grid_layout_get_preferred_height;
  layout_class->allocate = grid_layout_allocate;

  layout_props[LAYOUT_PROP_SPACING] =
    g_param_spec_int ("spacing", NULL, NULL,
                      0, G_MAXINT32, 0,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAYOUT_N_PROPS, layout_props);
}

static void
shell_grid_search_results_layout_init (ShellGridSearchResultsLayout *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellGridSearchResults (private)                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_GRID_SEARCH_RESULTS (shell_grid_search_results_get_type ())
G_DECLARE_FINAL_TYPE (ShellGridSearchResults, shell_grid_search_results,
                      SHELL, GRID_SEARCH_RESULTS, ShellSearchResultsBase)

struct _ShellGridSearchResults
{
  ShellSearchResultsBase parent;

  ClutterActor *grid; /* weak */
  ShellGridSearchResultsLayout *layout; /* weak, owned by the grid */
  int max_results;

  gulong notify_allocation_id;
  unsigned int update_search_later;
  MetaLaters *laters; /* weak, where update_search_later was added */
  char **last_results;
  char **last_terms;
};

G_DEFINE_FINAL_TYPE (ShellGridSearchResults, shell_grid_search_results,
                     SHELL_TYPE_SEARCH_RESULTS_BASE)

static void
grid_results_clear_later (ShellGridSearchResults *self)
{
  if (self->update_search_later != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->update_search_later);

  self->update_search_later = 0;
  g_clear_weak_pointer (&self->laters);
}

static gboolean
grid_results_update_search_later_cb (gpointer user_data)
{
  ShellGridSearchResults *self = user_data;

  self->update_search_later = 0;
  g_clear_weak_pointer (&self->laters);

  search_results_base_do_update_search (SHELL_SEARCH_RESULTS_BASE (self),
                                        (const char * const *) self->last_results,
                                        (const char * const *) self->last_terms,
                                        NULL, NULL);

  return G_SOURCE_REMOVE;
}

static void
grid_results_notify_allocation_cb (GObject    *object,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (object);
  MetaLaters *laters;

  if (self->update_search_later != 0)
    return;

  laters = meta_compositor_get_laters (shell_global_get_compositor (shell_global_get ()));
  g_set_weak_pointer (&self->laters, laters);
  self->update_search_later = meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                                               grid_results_update_search_later_cb,
                                               self, NULL);
}

static void
grid_results_before_update_search (ShellSearchResultsBase *base,
                                   const char * const     *results,
                                   const char * const     *terms)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (base);

  if (self->notify_allocation_id != 0)
    {
      g_signal_handler_disconnect (self, self->notify_allocation_id);
      self->notify_allocation_id = 0;
    }

  grid_results_clear_later (self);

  g_strfreev (self->last_results);
  g_strfreev (self->last_terms);
  self->last_results = strv_dup_or_empty (results);
  self->last_terms = strv_dup_or_empty (terms);

  /* Make sure the maximum number of results calculated by
   * get_max_displayed_results () is updated after width changes. */
  self->notify_allocation_id =
    g_signal_connect (self, "notify::allocation",
                      G_CALLBACK (grid_results_notify_allocation_cb), NULL);
}

static int
grid_results_get_max_displayed_results (ShellSearchResultsBase *base)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (base);
  ClutterActorBox box;
  float width;
  int n_cols;

  clutter_actor_get_allocation_box (CLUTTER_ACTOR (self), &box);
  width = clutter_actor_box_get_width (&box);
  if (width == 0 || self->layout == NULL)
    return self->max_results;

  n_cols = grid_layout_columns_for_width (self->layout, width);
  if (n_cols < 0)
    return self->max_results;

  if (self->max_results < 0)
    return n_cols;

  return MIN (n_cols, self->max_results);
}

static void
grid_results_clear_result_display (ShellSearchResultsBase *base)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (base);

  if (self->grid != NULL)
    clutter_actor_remove_all_children (self->grid);
}

static ClutterActor *
grid_results_create_result_display (ShellSearchResultsBase *base,
                                    ShellSearchResultMeta  *meta)
{
  ShellSearchResultsBaseClass *parent_class =
    SHELL_SEARCH_RESULTS_BASE_CLASS (shell_grid_search_results_parent_class);
  g_autoptr (ShellSearchResultsView) view = base_get_results_view (base);
  ClutterActor *display = parent_class->create_result_display (base, meta);

  if (display != NULL)
    return display;

  return g_object_ref_sink (g_object_new (SHELL_TYPE_GRID_SEARCH_RESULT,
                                          "provider", base_get_provider (base),
                                          "meta-info", meta,
                                          "results-view", view,
                                          NULL));
}

static void
grid_results_add_item (ShellSearchResultsBase *base,
                       ClutterActor           *display)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (base);

  if (self->grid != NULL)
    clutter_actor_add_child (self->grid, display);
}

static ClutterActor *
grid_results_get_first_result (ShellSearchResultsBase *base)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (base);

  if (self->grid == NULL)
    return NULL;

  for (ClutterActor *child = clutter_actor_get_first_child (self->grid);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      if (clutter_actor_is_visible (child))
        return child;
    }

  return NULL;
}

static void
grid_results_set_more_count (ShellSearchResultsBase *base,
                             int                     count)
{
}

static void
grid_results_style_changed_cb (StWidget               *grid,
                               ShellGridSearchResults *self)
{
  StThemeNode *node = st_widget_get_theme_node (grid);

  if (self->layout != NULL)
    grid_layout_set_spacing (self->layout, (int) st_theme_node_get_length (node, "spacing"));
}

static void
shell_grid_search_results_constructed (GObject *object)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (object);
  ShellSearchResultsBase *base = SHELL_SEARCH_RESULTS_BASE (self);
  ShellSearchResultsBasePrivate *base_priv = shell_search_results_base_get_instance_private (base);
  ClutterLayoutManager *layout;
  ClutterActor *bin;

  G_OBJECT_CLASS (shell_grid_search_results_parent_class)->constructed (object);

  layout = g_object_new (SHELL_TYPE_GRID_SEARCH_RESULTS_LAYOUT, NULL);
  self->grid = g_object_new (ST_TYPE_WIDGET,
                             "style-class", "grid-search-results",
                             "layout-manager", layout,
                             NULL);
  g_object_add_weak_pointer (G_OBJECT (self->grid), (gpointer *) &self->grid);
  g_set_weak_pointer (&self->layout, SHELL_GRID_SEARCH_RESULTS_LAYOUT (layout));

  g_signal_connect_object (self->grid, "style-changed",
                           G_CALLBACK (grid_results_style_changed_cb), self, 0);

  bin = g_object_new (ST_TYPE_BIN,
                      "child", self->grid,
                      "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                      NULL);
  st_bin_set_child (base_priv->result_display_bin, bin);

  self->max_results = shell_search_provider_get_max_results (base_priv->provider);
}

static void
shell_grid_search_results_dispose (GObject *object)
{
  ShellGridSearchResults *self = SHELL_GRID_SEARCH_RESULTS (object);

  if (self->notify_allocation_id != 0)
    {
      g_signal_handler_disconnect (self, self->notify_allocation_id);
      self->notify_allocation_id = 0;
    }

  grid_results_clear_later (self);

  g_clear_weak_pointer (&self->grid);
  g_clear_weak_pointer (&self->layout);
  g_clear_pointer (&self->last_results, g_strfreev);
  g_clear_pointer (&self->last_terms, g_strfreev);

  G_OBJECT_CLASS (shell_grid_search_results_parent_class)->dispose (object);
}

static void
shell_grid_search_results_class_init (ShellGridSearchResultsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSearchResultsBaseClass *base_class = SHELL_SEARCH_RESULTS_BASE_CLASS (klass);

  object_class->constructed = shell_grid_search_results_constructed;
  object_class->dispose = shell_grid_search_results_dispose;

  base_class->before_update_search = grid_results_before_update_search;
  base_class->set_more_count = grid_results_set_more_count;
  base_class->get_max_displayed_results = grid_results_get_max_displayed_results;
  base_class->clear_result_display = grid_results_clear_result_display;
  base_class->create_result_display = grid_results_create_result_display;
  base_class->add_item = grid_results_add_item;
  base_class->get_first_result = grid_results_get_first_result;
}

static void
shell_grid_search_results_init (ShellGridSearchResults *self)
{
  self->max_results = -1;
}

/* ------------------------------------------------------------------------ */
/* ShellSearchMaxWidthBox (private)                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_MAX_WIDTH_BOX (shell_search_max_width_box_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchMaxWidthBox, shell_search_max_width_box,
                      SHELL, SEARCH_MAX_WIDTH_BOX, StBoxLayout)

struct _ShellSearchMaxWidthBox
{
  StBoxLayout parent;
};

G_DEFINE_FINAL_TYPE (ShellSearchMaxWidthBox, shell_search_max_width_box, ST_TYPE_BOX_LAYOUT)

static void
shell_search_max_width_box_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_search_max_width_box_parent_class)->constructed (object);

  /* The layout manager of a St.BoxLayout exists from constructed () on */
  st_box_layout_set_orientation (ST_BOX_LAYOUT (object), CLUTTER_ORIENTATION_VERTICAL);
}

static void
shell_search_max_width_box_allocate (ClutterActor          *actor,
                                     const ClutterActorBox *box)
{
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  int max_width = st_theme_node_get_max_width (theme_node);
  float avail_width = box->x2 - box->x1;
  ClutterActorBox adjusted_box = *box;

  if (avail_width > max_width)
    {
      float excess_width = avail_width - max_width;

      adjusted_box.x1 += floorf (excess_width / 2);
      adjusted_box.x2 -= floorf (excess_width / 2);
    }

  CLUTTER_ACTOR_CLASS (shell_search_max_width_box_parent_class)->allocate (actor, &adjusted_box);
}

static void
shell_search_max_width_box_class_init (ShellSearchMaxWidthBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_search_max_width_box_constructed;

  actor_class->allocate = shell_search_max_width_box_allocate;
}

static void
shell_search_max_width_box_init (ShellSearchMaxWidthBox *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellSearchResultsView                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  VIEW_PROP_0,

  VIEW_PROP_APPLICATION_PROVIDER,
  VIEW_PROP_TERMS,

  VIEW_N_PROPS
};

static GParamSpec *view_props[VIEW_N_PROPS] = { NULL, };

enum
{
  VIEW_TERMS_CHANGED,
  VIEW_OVERVIEW_TOGGLE_REQUESTED,
  VIEW_LAST_SIGNAL
};

static guint view_signals[VIEW_LAST_SIGNAL] = { 0, };

/* What the view knows about a registered provider; the JS kept it on the
 * provider object (`display`, `searchInProgress`). */
typedef struct
{
  ShellSearchProvider *provider;
  ClutterActor *display; /* weak, the section of the provider */
  gboolean search_in_progress;
} ProviderEntry;

struct _ShellSearchResultsView
{
  StBoxLayout parent;

  StScrollView *scroll_view; /* weak */
  ClutterActor *content;     /* weak */
  ClutterActor *status_container; /* weak */
  ShellSpinner *status_spinner;   /* weak */
  StLabel *status_text;           /* weak */

  gboolean highlight_default;
  ClutterActor *default_result; /* weak */
  gboolean starting_search;

  char **terms;
  gboolean is_sub_search;
  GHashTable *results; /* provider id -> strv */
  GPtrArray *providers; /* ProviderEntry */

  ShellHighlighter *highlighter;

  GSettings *search_settings;
  unsigned int search_timeout_id;
  GCancellable *cancellable;

  ShellSearchProvider *application_provider;

  gboolean shutting_down;
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellSearchResultsView, shell_search_results_view, ST_TYPE_BOX_LAYOUT)

static ProviderEntry *
provider_entry_new (ShellSearchProvider *provider)
{
  ProviderEntry *entry = g_rc_box_new0 (ProviderEntry);

  entry->provider = g_object_ref (provider);

  return entry;
}

static void
provider_entry_clear (gpointer data)
{
  ProviderEntry *entry = data;

  g_clear_weak_pointer (&entry->display);
  g_clear_object (&entry->provider);
}

static ProviderEntry *
provider_entry_acquire (ProviderEntry *entry)
{
  return g_rc_box_acquire (entry);
}

static void
provider_entry_release (ProviderEntry *entry)
{
  g_rc_box_release_full (entry, provider_entry_clear);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ProviderEntry, provider_entry_release)

static ProviderEntry *
view_find_entry (ShellSearchResultsView *self,
                 ShellSearchProvider    *provider,
                 guint                  *index)
{
  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);

      if (entry->provider == provider)
        {
          if (index != NULL)
            *index = i;
          return entry;
        }
    }

  return NULL;
}

static void
request_overview_toggle (ShellSearchResultsView *view)
{
  g_signal_emit (view, view_signals[VIEW_OVERVIEW_TOGGLE_REQUESTED], 0);
}

static void
view_clear_search_timeout (ShellSearchResultsView *self)
{
  g_clear_handle_id (&self->search_timeout_id, g_source_remove);
}

static void
view_set_selected (ShellSearchResultsView *self,
                   ClutterActor           *result,
                   gboolean                selected)
{
  if (result == NULL || !ST_IS_WIDGET (result))
    return;

  if (selected)
    {
      st_widget_add_style_pseudo_class (ST_WIDGET (result), "selected");
      if (self->scroll_view != NULL)
        shell_animation_ensure_actor_visible_in_scroll_view (self->scroll_view, result);
    }
  else
    {
      st_widget_remove_style_pseudo_class (ST_WIDGET (result), "selected");
    }
}

static void
view_clear_display (ShellSearchResultsView *self)
{
  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);

      if (entry->display != NULL)
        search_results_base_clear (SHELL_SEARCH_RESULTS_BASE (entry->display));
    }
}

static gboolean
view_get_search_in_progress (ShellSearchResultsView *self)
{
  if (self->starting_search)
    return TRUE;

  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);

      if (entry->search_in_progress)
        return TRUE;
    }

  return FALSE;
}

static void
view_update_search_progress (ShellSearchResultsView *self)
{
  gboolean have_results = FALSE;

  if (self->destroyed || self->scroll_view == NULL || self->status_container == NULL)
    return;

  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);

      if (entry->display != NULL &&
          search_results_base_get_first_result (SHELL_SEARCH_RESULTS_BASE (entry->display)) != NULL)
        {
          have_results = TRUE;
          break;
        }
    }

  g_object_set (self->scroll_view, "visible", have_results, NULL);
  g_object_set (self->status_container, "visible", !have_results, NULL);

  if (!have_results)
    {
      if (view_get_search_in_progress (self))
        {
          shell_spinner_play (self->status_spinner);
          st_label_set_text (self->status_text, _("Searching"));
        }
      else
        {
          shell_spinner_stop (self->status_spinner);
          st_label_set_text (self->status_text, _("No results"));
        }
    }
}

static void
view_maybe_set_initial_selection (ShellSearchResultsView *self)
{
  ClutterActor *new_default_result = NULL;

  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);
      ClutterActor *first_result;

      if (entry->display == NULL || !clutter_actor_is_visible (entry->display))
        continue;

      first_result = search_results_base_get_first_result (SHELL_SEARCH_RESULTS_BASE (entry->display));
      if (first_result != NULL)
        {
          new_default_result = first_result;
          break; /* select this one! */
        }
    }

  if (new_default_result != self->default_result)
    {
      view_set_selected (self, self->default_result, FALSE);
      view_set_selected (self, new_default_result, self->highlight_default);

      g_set_weak_pointer (&self->default_result, new_default_result);
    }
}

static void
view_focus_child_changed_cb (GObject                *display,
                             GParamSpec             *pspec,
                             ShellSearchResultsView *self)
{
  g_autoptr (ClutterActor) focus_child = NULL;

  g_object_get (display, "focus-child", &focus_child, NULL);
  if (focus_child != NULL && self->scroll_view != NULL)
    shell_animation_ensure_actor_visible_in_scroll_view (self->scroll_view, focus_child);
}

static void
view_ensure_provider_display (ShellSearchResultsView *self,
                              ProviderEntry          *entry)
{
  g_autoptr (GAppInfo) app_info = NULL;
  ClutterActor *display;

  if (entry->display != NULL)
    return;

  app_info = shell_search_provider_get_app_info (entry->provider);
  if (app_info != NULL)
    display = g_object_new (SHELL_TYPE_LIST_SEARCH_RESULTS,
                            "provider", entry->provider,
                            "results-view", self,
                            NULL);
  else
    display = g_object_new (SHELL_TYPE_GRID_SEARCH_RESULTS,
                            "provider", entry->provider,
                            "results-view", self,
                            NULL);

  g_signal_connect_object (display, "notify::focus-child",
                           G_CALLBACK (view_focus_child_changed_cb), self, 0);
  clutter_actor_hide (display);
  clutter_actor_add_child (self->content, display);
  g_set_weak_pointer (&entry->display, display);
}

/**
 * shell_search_results_view_register_provider:
 * @self: a #ShellSearchResultsView
 * @provider: a #ShellSearchProvider
 *
 * Adds a provider to the search (`_registerProvider ()`), unless parental
 * controls hide its application. Providers are searched, and their results
 * shown, in the order they are registered.
 */
void
shell_search_results_view_register_provider (ShellSearchResultsView *self,
                                             ShellSearchProvider    *provider)
{
  g_autoptr (GAppInfo) app_info = NULL;
  ProviderEntry *entry;

  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));
  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (provider));

  if (self->destroyed || view_find_entry (self, provider, NULL) != NULL)
    return;

  /* Filter out unwanted providers. */
  app_info = shell_search_provider_get_app_info (provider);
  if (app_info != NULL &&
      !shell_parental_controls_manager_should_show_app (shell_parental_controls_manager_get_default (),
                                                        app_info))
    return;

  entry = provider_entry_new (provider);
  g_ptr_array_add (self->providers, entry);
  view_ensure_provider_display (self, entry);
}

/**
 * shell_search_results_view_unregister_provider:
 * @self: a #ShellSearchResultsView
 * @provider: a #ShellSearchProvider
 *
 * Removes a provider from the search (`_unregisterProvider ()`).
 */
void
shell_search_results_view_unregister_provider (ShellSearchResultsView *self,
                                               ShellSearchProvider    *provider)
{
  g_autoptr (ProviderEntry) entry = NULL;
  guint index;

  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));
  g_return_if_fail (SHELL_IS_SEARCH_PROVIDER (provider));

  if (view_find_entry (self, provider, &index) == NULL)
    return;

  entry = provider_entry_acquire (g_ptr_array_index (self->providers, index));
  g_ptr_array_remove_index (self->providers, index);

  if (entry->display != NULL)
    clutter_actor_destroy (entry->display);
}

static void
view_reload_remote_providers (ShellSearchResultsView *self)
{
  g_autoptr (GPtrArray) remote_providers = g_ptr_array_new_with_free_func (g_object_unref);
  GList *providers;

  if (self->destroyed)
    return;

  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);

      if (shell_search_provider_get_is_remote_provider (entry->provider))
        g_ptr_array_add (remote_providers, g_object_ref (entry->provider));
    }

  for (guint i = 0; i < remote_providers->len; i++)
    shell_search_results_view_unregister_provider (self, g_ptr_array_index (remote_providers, i));

  providers = shell_remote_search_load_providers (self->search_settings);
  for (GList *l = providers; l != NULL; l = l->next)
    shell_search_results_view_register_provider (self, l->data);

  g_list_free_full (providers, g_object_unref);
}

static void
reload_remote_providers_cb (gpointer instance,
                            gpointer user_data)
{
  view_reload_remote_providers (SHELL_SEARCH_RESULTS_VIEW (user_data));
}

static void
reload_remote_providers_changed_cb (GSettings  *settings,
                                    const char *key,
                                    gpointer    user_data)
{
  view_reload_remote_providers (SHELL_SEARCH_RESULTS_VIEW (user_data));
}

static void
view_reset (ShellSearchResultsView *self)
{
  g_strfreev (self->terms);
  self->terms = strv_new_empty ();
  g_hash_table_remove_all (self->results);
  view_clear_display (self);
  view_clear_search_timeout (self);
  g_set_weak_pointer (&self->default_result, NULL);
  self->starting_search = FALSE;

  view_update_search_progress (self);
}

typedef struct
{
  GWeakRef view;
  ProviderEntry *entry;
  char **terms;
  gboolean subsearch;
} ProviderSearchData;

static void
provider_search_data_free (ProviderSearchData *data)
{
  g_weak_ref_clear (&data->view);
  provider_entry_release (data->entry);
  g_strfreev (data->terms);
  g_free (data);
}

static void
view_update_results_done_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  ProviderSearchData *data = user_data;
  g_autoptr (ShellSearchResultsView) self = g_weak_ref_get (&data->view);

  g_task_propagate_boolean (G_TASK (res), NULL);

  if (self != NULL && !self->destroyed && !self->shutting_down)
    {
      data->entry->search_in_progress = FALSE;

      view_maybe_set_initial_selection (self);
      view_update_search_progress (self);
    }

  provider_search_data_free (data);
}

static void
view_update_results (ShellSearchResultsView *self,
                     ProviderSearchData     *data,
                     const char * const     *results)
{
  ProviderEntry *entry = data->entry;

  if (entry->display == NULL)
    {
      /* The provider was unregistered while it searched */
      entry->search_in_progress = FALSE;
      view_maybe_set_initial_selection (self);
      view_update_search_progress (self);
      provider_search_data_free (data);
      return;
    }

  search_results_base_update_search_async (SHELL_SEARCH_RESULTS_BASE (entry->display),
                                           results, (const char * const *) data->terms,
                                           view_update_results_done_cb, data);
}

static void
view_provider_results_ready_cb (GObject      *source,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  ProviderSearchData *data = user_data;
  ShellSearchProvider *provider = SHELL_SEARCH_PROVIDER (source);
  g_autoptr (ShellSearchResultsView) self = g_weak_ref_get (&data->view);
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) results = NULL;
  g_autofree char *provider_id = NULL;

  if (data->subsearch)
    results = shell_search_provider_get_subsearch_result_set_finish (provider, res, &error);
  else
    results = shell_search_provider_get_initial_result_set_finish (provider, res, &error);

  if (self == NULL || self->destroyed || self->shutting_down)
    {
      provider_search_data_free (data);
      return;
    }

  if (results == NULL)
    {
      if (!error_is_cancelled (error))
        {
          g_autofree char *id = shell_search_provider_get_id (provider);

          g_message ("Error from search provider %s: %s", id, error->message);
        }

      results = strv_new_empty ();
    }

  provider_id = shell_search_provider_get_id (provider);
  g_hash_table_replace (self->results, g_strdup (provider_id), g_strdupv (results));

  view_update_results (self, data, (const char * const *) results);
}

static void
view_do_provider_search (ShellSearchResultsView *self,
                         ProviderEntry          *entry,
                         char                  **previous_results)
{
  ProviderSearchData *data = g_new0 (ProviderSearchData, 1);
  const char * const *terms = (const char * const *) self->terms;

  entry->search_in_progress = TRUE;

  g_weak_ref_init (&data->view, self);
  data->entry = provider_entry_acquire (entry);
  data->terms = strv_dup_or_empty (terms);
  data->subsearch = self->is_sub_search && previous_results != NULL;

  if (data->subsearch)
    shell_search_provider_get_subsearch_result_set_async (entry->provider,
                                                          (const char * const *) previous_results,
                                                          (const char * const *) data->terms,
                                                          self->cancellable,
                                                          view_provider_results_ready_cb,
                                                          data);
  else
    shell_search_provider_get_initial_result_set_async (entry->provider,
                                                        (const char * const *) data->terms,
                                                        self->cancellable,
                                                        view_provider_results_ready_cb,
                                                        data);
}

/* Lua extensions (gnome-lua): "search.query" */
static void
view_emit_query_hook (ShellSearchResultsView *self)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  g_autofree char *query = g_strjoinv (" ", self->terms);
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "query", g_variant_new_string (query));
  g_variant_builder_add (&builder, "{sv}", "terms",
                         g_variant_new_strv ((const char * const *) self->terms, -1));

  shell_hook_registry_emit_after (registry, "search.query", g_variant_builder_end (&builder));
}

static void
view_do_search (ShellSearchResultsView *self)
{
  g_autoptr (GHashTable) previous_results = NULL;
  g_autoptr (GPtrArray) entries = NULL;

  self->starting_search = FALSE;

  view_emit_query_hook (self);

  previous_results = g_steal_pointer (&self->results);
  self->results = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, (GDestroyNotify) g_strfreev);

  /* The providers can be registered or unregistered while they are asked */
  entries = g_ptr_array_new_with_free_func ((GDestroyNotify) provider_entry_release);
  for (guint i = 0; i < self->providers->len; i++)
    g_ptr_array_add (entries, provider_entry_acquire (g_ptr_array_index (self->providers, i)));

  for (guint i = 0; i < entries->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (entries, i);
      g_autofree char *id = shell_search_provider_get_id (entry->provider);

      view_do_provider_search (self, entry,
                               id != NULL ? g_hash_table_lookup (previous_results, id) : NULL);
    }

  view_update_search_progress (self);

  view_clear_search_timeout (self);
}

static void
view_search_timeout_cb (gpointer user_data)
{
  ShellSearchResultsView *self = user_data;

  self->search_timeout_id = 0;
  view_do_search (self);
}

/**
 * shell_search_results_view_set_terms:
 * @self: a #ShellSearchResultsView
 * @terms: (array zero-terminated=1): the search terms, none to cancel the search
 *
 * Starts a search (after a short delay) for @terms (`setTerms ()`). A search
 * for the terms already searched for does nothing.
 */
void
shell_search_results_view_set_terms (ShellSearchResultsView *self,
                                     const char * const     *terms)
{
  g_autofree char *search_string = NULL;
  g_autofree char *previous_search_string = NULL;
  gboolean is_sub_search = FALSE;

  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));
  g_return_if_fail (terms != NULL);

  if (self->destroyed)
    return;

  /* Check for the case of making a duplicate previous search before
   * setting state of the current search or cancelling the search.
   * This will prevent incorrect state being as a result of a duplicate
   * search while the previous search is still active. */
  search_string = g_strjoinv (" ", (char **) terms);
  previous_search_string = g_strjoinv (" ", self->terms);
  if (g_strcmp0 (search_string, previous_search_string) == 0)
    return;

  self->starting_search = TRUE;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  if (g_strv_length ((char **) terms) == 0)
    {
      view_reset (self);
      return;
    }

  if (g_strv_length (self->terms) > 0)
    is_sub_search = g_str_has_prefix (search_string, previous_search_string);

  g_strfreev (self->terms);
  self->terms = g_strdupv ((char **) terms);
  self->is_sub_search = is_sub_search;
  view_update_search_progress (self);

  if (self->search_timeout_id == 0)
    self->search_timeout_id = g_timeout_add_once (SEARCH_TIMEOUT_MS, view_search_timeout_cb, self);

  g_clear_object (&self->highlighter);
  self->highlighter = shell_highlighter_new ((const char * const *) self->terms);

  g_signal_emit (self, view_signals[VIEW_TERMS_CHANGED], 0);
  g_object_notify_by_pspec (G_OBJECT (self), view_props[VIEW_PROP_TERMS]);
}

/**
 * shell_search_results_view_get_terms:
 * @self: a #ShellSearchResultsView
 *
 * Returns: (transfer full) (array zero-terminated=1): the terms searched for
 */
char **
shell_search_results_view_get_terms (ShellSearchResultsView *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self), NULL);

  return g_strdupv (self->terms);
}

/**
 * shell_search_results_view_get_search_in_progress:
 * @self: a #ShellSearchResultsView
 *
 * Returns: whether a search is starting or a provider is still searching
 *   (`searchInProgress`)
 */
gboolean
shell_search_results_view_get_search_in_progress (ShellSearchResultsView *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self), FALSE);

  return view_get_search_in_progress (self);
}

static void
view_activate_actor (ClutterActor *result)
{
  if (SHELL_IS_SEARCH_RESULT (result))
    shell_search_result_activate (SHELL_SEARCH_RESULT (result));
  else if (ST_IS_BUTTON (result))
    g_signal_emit_by_name (result, "clicked", 0);
}

/**
 * shell_search_results_view_activate_default:
 * @self: a #ShellSearchResultsView
 *
 * Activates the first result (`activateDefault ()`), forcing the search if
 * it has not started yet.
 */
void
shell_search_results_view_activate_default (ShellSearchResultsView *self)
{
  g_autoptr (ClutterActor) default_result = NULL;

  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));

  /* If we have a search queued up, force the search now. */
  if (self->search_timeout_id > 0)
    view_do_search (self);

  if (self->default_result != NULL)
    {
      /* Activating can destroy the result */
      default_result = g_object_ref (self->default_result);
      view_activate_actor (default_result);
    }
}

/**
 * shell_search_results_view_highlight_default:
 * @self: a #ShellSearchResultsView
 * @highlight: whether the first result is highlighted
 *
 * Highlights the first result as the one `Enter` activates
 * (`highlightDefault ()`).
 */
void
shell_search_results_view_highlight_default (ShellSearchResultsView *self,
                                             gboolean                highlight)
{
  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));

  self->highlight_default = highlight;
  view_set_selected (self, self->default_result, highlight);
}

/**
 * shell_search_results_view_popup_menu_default:
 * @self: a #ShellSearchResultsView
 *
 * Opens the menu of the first result (`popupMenuDefault ()`), forcing the
 * search if it has not started yet.
 */
void
shell_search_results_view_popup_menu_default (ShellSearchResultsView *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));

  /* If we have a search queued up, force the search now. */
  if (self->search_timeout_id > 0)
    view_do_search (self);

  if (self->default_result != NULL && ST_IS_WIDGET (self->default_result))
    st_widget_popup_menu (ST_WIDGET (self->default_result));
}

/**
 * shell_search_results_view_navigate_from_entry:
 * @self: a #ShellSearchResultsView
 * @direction: the direction of the keynav
 *
 * Moves the key focus from the search entry into the results
 * (`navigateFocus ()`).
 */
void
shell_search_results_view_navigate_from_entry (ShellSearchResultsView *self,
                                               StDirectionType         direction)
{
  gboolean rtl;

  g_return_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self));

  rtl = clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL;
  if (direction == ST_DIR_TAB_BACKWARD ||
      direction == (rtl ? ST_DIR_RIGHT : ST_DIR_LEFT) ||
      direction == ST_DIR_UP)
    {
      st_widget_navigate_focus (ST_WIDGET (self), NULL, direction, FALSE);
      return;
    }

  st_widget_navigate_focus (ST_WIDGET (self), self->default_result, direction, FALSE);
}

/**
 * shell_search_results_view_highlight_terms:
 * @self: a #ShellSearchResultsView
 * @description: (nullable): the text to highlight the terms in
 *
 * Returns: (transfer full): @description with the search terms in bold, as
 *   markup (`highlightTerms ()`)
 */
char *
shell_search_results_view_highlight_terms (ShellSearchResultsView *self,
                                           const char             *description)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_RESULTS_VIEW (self), NULL);

  if (description == NULL || *description == '\0')
    return g_strdup ("");

  return shell_highlighter_highlight (self->highlighter, description);
}

static void
view_on_shutdown (ShellGlobal            *global,
                  ShellSearchResultsView *self)
{
  /* Nothing may reach the results once the shell is going away */
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
  view_clear_search_timeout (self);

  for (guint i = 0; i < self->providers->len; i++)
    {
      ProviderEntry *entry = g_ptr_array_index (self->providers, i);

      if (entry->display != NULL)
        search_results_base_cancel (SHELL_SEARCH_RESULTS_BASE (entry->display));
    }
}

static void
shell_search_results_view_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellSearchResultsView *self = SHELL_SEARCH_RESULTS_VIEW (object);

  switch (prop_id)
    {
    case VIEW_PROP_APPLICATION_PROVIDER:
      g_set_object (&self->application_provider, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_results_view_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellSearchResultsView *self = SHELL_SEARCH_RESULTS_VIEW (object);

  switch (prop_id)
    {
    case VIEW_PROP_APPLICATION_PROVIDER:
      g_value_set_object (value, self->application_provider);
      break;

    case VIEW_PROP_TERMS:
      g_value_take_boxed (value, g_strdupv (self->terms));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_results_view_constructed (GObject *object)
{
  ShellSearchResultsView *self = SHELL_SEARCH_RESULTS_VIEW (object);
  ShellGlobal *global = shell_global_get ();
  ClutterActor *status_spinner;

  G_OBJECT_CLASS (shell_search_results_view_parent_class)->constructed (object);

  /* The layout manager of a St.BoxLayout exists from constructed () on */
  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_set_name (CLUTTER_ACTOR (self), "searchResults");
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);

  st_focus_manager_add_group (shell_global_get_focus_manager (global), ST_WIDGET (self));

  g_signal_connect_object (shell_parental_controls_manager_get_default (),
                           "app-filter-changed",
                           G_CALLBACK (reload_remote_providers_cb), self, 0);

  self->content = g_object_new (SHELL_TYPE_SEARCH_MAX_WIDTH_BOX,
                                "name", "searchResultsContent",
                                "x-expand", TRUE,
                                NULL);
  g_object_add_weak_pointer (G_OBJECT (self->content), (gpointer *) &self->content);

  self->scroll_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                                    "overlay-scrollbars", TRUE,
                                    "style-class", "search-display vfade",
                                    "x-expand", TRUE,
                                    "y-expand", TRUE,
                                    NULL);
  g_object_add_weak_pointer (G_OBJECT (self->scroll_view), (gpointer *) &self->scroll_view);
  st_scroll_view_set_child (self->scroll_view, ST_SCROLLABLE (self->content));

  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->scroll_view));

  self->status_container = g_object_new (ST_TYPE_BOX_LAYOUT,
                                         "style-class", "search-statusbox",
                                         "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                         "x-expand", TRUE,
                                         "y-expand", TRUE,
                                         "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                         "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                         NULL);
  g_object_add_weak_pointer (G_OBJECT (self->status_container),
                             (gpointer *) &self->status_container);

  status_spinner = CLUTTER_ACTOR (shell_spinner_new (SEARCH_SPINNER_SIZE, FALSE, FALSE));
  self->status_spinner = SHELL_SPINNER (status_spinner);
  g_object_add_weak_pointer (G_OBJECT (self->status_spinner), (gpointer *) &self->status_spinner);

  self->status_text = g_object_new (ST_TYPE_LABEL,
                                    "style-class", "search-statustext",
                                    NULL);
  g_object_add_weak_pointer (G_OBJECT (self->status_text), (gpointer *) &self->status_text);

  clutter_actor_add_child (self->status_container, status_spinner);
  clutter_actor_add_child (self->status_container, CLUTTER_ACTOR (self->status_text));

  clutter_actor_add_child (CLUTTER_ACTOR (self), self->status_container);

  self->highlighter = shell_highlighter_new (NULL);

  self->search_settings = g_settings_new (SEARCH_PROVIDERS_SCHEMA);
  g_signal_connect_object (self->search_settings, "changed::disabled",
                           G_CALLBACK (reload_remote_providers_changed_cb), self, 0);
  g_signal_connect_object (self->search_settings, "changed::enabled",
                           G_CALLBACK (reload_remote_providers_changed_cb), self, 0);
  g_signal_connect_object (self->search_settings, "changed::disable-external",
                           G_CALLBACK (reload_remote_providers_changed_cb), self, 0);
  g_signal_connect_object (self->search_settings, "changed::sort-order",
                           G_CALLBACK (reload_remote_providers_changed_cb), self, 0);

  g_signal_connect_object (global, "shutdown", G_CALLBACK (view_on_shutdown), self, 0);

  if (self->application_provider != NULL)
    shell_search_results_view_register_provider (self, self->application_provider);

  g_signal_connect_object (shell_app_system_get_default (), "installed-changed",
                           G_CALLBACK (reload_remote_providers_cb), self, 0);
  view_reload_remote_providers (self);

  view_update_search_progress (self);
}

static void
shell_search_results_view_dispose (GObject *object)
{
  ShellSearchResultsView *self = SHELL_SEARCH_RESULTS_VIEW (object);

  self->destroyed = TRUE;

  view_clear_search_timeout (self);
  g_cancellable_cancel (self->cancellable);

  g_clear_weak_pointer (&self->default_result);
  g_clear_weak_pointer (&self->scroll_view);
  g_clear_weak_pointer (&self->content);
  g_clear_weak_pointer (&self->status_container);
  g_clear_weak_pointer (&self->status_spinner);
  g_clear_weak_pointer (&self->status_text);

  if (self->providers != NULL)
    g_ptr_array_set_size (self->providers, 0);
  g_clear_object (&self->application_provider);
  g_clear_object (&self->search_settings);

  G_OBJECT_CLASS (shell_search_results_view_parent_class)->dispose (object);
}

static void
shell_search_results_view_finalize (GObject *object)
{
  ShellSearchResultsView *self = SHELL_SEARCH_RESULTS_VIEW (object);

  g_clear_pointer (&self->terms, g_strfreev);
  g_clear_pointer (&self->results, g_hash_table_unref);
  g_clear_pointer (&self->providers, g_ptr_array_unref);
  g_clear_object (&self->highlighter);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_search_results_view_parent_class)->finalize (object);
}

static void
shell_search_results_view_class_init (ShellSearchResultsViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellHookRegistry *registry;

  object_class->constructed = shell_search_results_view_constructed;
  object_class->set_property = shell_search_results_view_set_property;
  object_class->get_property = shell_search_results_view_get_property;
  object_class->dispose = shell_search_results_view_dispose;
  object_class->finalize = shell_search_results_view_finalize;

  /**
   * ShellSearchResultsView:application-provider:
   *
   * The provider of the applications, which is registered before the remote
   * providers. It is #ShellAppSearchProvider (app-display.c), which the view
   * does not create itself (`new AppDisplay.AppSearchProvider ()`).
   */
  view_props[VIEW_PROP_APPLICATION_PROVIDER] =
    g_param_spec_object ("application-provider", NULL, NULL,
                         SHELL_TYPE_SEARCH_PROVIDER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchResultsView:terms: (type GStrv)
   *
   * The terms searched for (`terms`).
   */
  view_props[VIEW_PROP_TERMS] =
    g_param_spec_boxed ("terms", NULL, NULL,
                        G_TYPE_STRV,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, VIEW_N_PROPS, view_props);

  /**
   * ShellSearchResultsView::terms-changed:
   * @self: the view
   *
   * Emitted when the terms of a search changed. Handlers highlight the
   * terms in the results.
   */
  view_signals[VIEW_TERMS_CHANGED] =
    g_signal_new ("terms-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellSearchResultsView::overview-toggle-requested:
   * @self: the view
   *
   * Emitted when a result was activated, or the search of a provider
   * launched, and the overview should be toggled (`Main.overview.toggle ()`).
   */
  view_signals[VIEW_OVERVIEW_TOGGLE_REQUESTED] =
    g_signal_new ("overview-toggle-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* Lua extensions (gnome-lua): the hook point of the search */
  registry = shell_hook_registry_get_default ();
  shell_hook_registry_declare (registry, "search.query",
                               "query:s terms:as", FALSE,
                               "The user changed the terms of the search");
}

static void
shell_search_results_view_init (ShellSearchResultsView *self)
{
  self->terms = strv_new_empty ();
  self->results = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, (GDestroyNotify) g_strfreev);
  self->providers = g_ptr_array_new_with_free_func ((GDestroyNotify) provider_entry_release);
  self->cancellable = g_cancellable_new ();
}

/**
 * shell_search_results_view_new:
 * @application_provider: (nullable): the provider of the applications
 *
 * Creates the view the results of all search providers are shown in
 * (`new SearchResultsView ()`). It registers @application_provider and the
 * D-Bus providers of the installed applications, and reloads those when the
 * installed applications or the search settings change.
 *
 * Returns: (transfer full): a new #ShellSearchResultsView
 */
ShellSearchResultsView *
shell_search_results_view_new (ShellSearchProvider *application_provider)
{
  return g_object_new (SHELL_TYPE_SEARCH_RESULTS_VIEW,
                       "application-provider", application_provider,
                       NULL);
}
