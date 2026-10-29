/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.search and the shell.SearchProvider handle type (docs/api/search.md):
 * sections of results that an extension adds to the search of the overview.
 *
 * A provider is a #ShellLuaSearchProvider, an implementation of the
 * #ShellSearchProvider interface of src/ui/search.h that the
 * #ShellSearchResultsView consumes like the providers of installed
 * applications (src/ui/remote-search.c):
 *
 *  - get_initial_result_set () (also used for get_subsearch_result_set (), the
 *    provider always searches for the whole terms) calls `on_search (provider,
 *    terms, respond)`; `respond (results)` answers the #ShellSearchRequest
 *    once, immediately or later. The request is answered with a cancelled
 *    error when the view cancels it (new terms) or the provider is destroyed,
 *    with no results when the extension raised an error or did not answer in
 *    ANSWER_TIMEOUT seconds. The `respond` of a search that is not the
 *    current one does nothing.
 *  - get_result_metas () builds the metas from the results of the last
 *    answer, with the icon made by a create-icon function of the meta.
 *  - activate_result () and launch_search () call `on_activate` and
 *    `on_launch`.
 *
 * The provider has a title and an icon (the header of its list, see
 * ShellSearchProviderInterface::get_title) and an owner (the uuid of the
 * extension, which places the section after those of the shell and makes the
 * view ask for the metas again for every search). The context of the extension
 * owns the provider: at disable, or on `destroy ()`, it is removed from the
 * search controller and its section goes away.
 */

#include "config.h"

#include <string.h>

#include "shell-lua-api-private.h"
#include "shell-lua-api-resolve.h"
#include "st.h"
#include "ui/main.h"
#include "ui/overview.h"
#include "ui/search-controller.h"
#include "ui/search.h"

#define PROVIDER_TYPE "shell.SearchProvider"
#define RESPOND_METATABLE "shell.SearchRespond"

/* Live providers of one extension */
#define MAX_PROVIDERS 8
/* Results kept of one answer */
#define MAX_RESULTS 32
/* Seconds the extension has to answer a search */
#define ANSWER_TIMEOUT 10

#define DEFAULT_ICON_NAME "application-x-addon-symbolic"

/* ------------------------------------------------------------------------ */
/* The results of an answer                                                  */
/* ------------------------------------------------------------------------ */

typedef struct
{
  char *id;
  char *name;
  char *description;
  char *icon_name;
  char *icon_file;       /* an absolute path */
  char *clipboard_text;
} ResultData;

static void
result_data_free (gpointer data)
{
  ResultData *result = data;

  if (result == NULL)
    return;

  g_free (result->id);
  g_free (result->name);
  g_free (result->description);
  g_free (result->icon_name);
  g_free (result->icon_file);
  g_free (result->clipboard_text);
  g_free (result);
}

/* ------------------------------------------------------------------------ */
/* The search in progress                                                    */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GWeakRef provider;
  ShellSearchRequest *request;   /* NULL once answered */
  GCancellable *cancellable;
  gulong cancel_id;
  guint timeout_id;
} Pending;

static Pending *
pending_new (ShellSearchRequest *request,
             GCancellable       *cancellable)
{
  Pending *pending = g_rc_box_new0 (Pending);

  g_weak_ref_init (&pending->provider, NULL);
  pending->request = g_object_ref (request);
  pending->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;

  return pending;
}

static void
pending_clear (gpointer data)
{
  Pending *pending = data;

  g_clear_handle_id (&pending->timeout_id, g_source_remove);
  if (pending->request != NULL)
    {
      /* Dropped without an answer: the request says so itself */
      g_clear_object (&pending->request);
    }
  g_clear_object (&pending->cancellable);
  g_weak_ref_clear (&pending->provider);
}

static Pending *
pending_acquire (Pending *pending)
{
  return g_rc_box_acquire (pending);
}

static void
pending_release (gpointer pending)
{
  g_rc_box_release_full (pending, pending_clear);
}

/* Stops the timeout and the watch on the cancellable */
static void
pending_detach (Pending *pending)
{
  g_clear_handle_id (&pending->timeout_id, g_source_remove);
  if (pending->cancel_id != 0 && pending->cancellable != NULL)
    g_cancellable_disconnect (pending->cancellable, pending->cancel_id);
  pending->cancel_id = 0;
}

static gboolean
pending_is_open (Pending *pending)
{
  return pending->request != NULL;
}

/* Answers the request with the identifiers of the results */
static void
pending_answer (Pending            *pending,
                const char * const *ids)
{
  g_autoptr (ShellSearchRequest) request = NULL;

  if (pending->request == NULL)
    return;

  pending_detach (pending);
  request = g_steal_pointer (&pending->request);
  shell_search_request_return_results (request, ids);
}

/* Answers the request as cancelled: nothing is logged and the section is not
 * updated by it */
static void
pending_cancel (Pending *pending)
{
  g_autoptr (ShellSearchRequest) request = NULL;

  if (pending->request == NULL)
    return;

  pending_detach (pending);
  request = g_steal_pointer (&pending->request);
  shell_search_request_return_gerror (request,
                                      g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                           "The search was cancelled"));
}

static gboolean
pending_cancel_idle_cb (gpointer data)
{
  pending_cancel (data);
  return G_SOURCE_REMOVE;
}

/* Called by the cancellable, possibly from inside the cancelling code of the
 * view: the answer follows from an idle */
static void
pending_cancelled_cb (GCancellable *cancellable,
                      gpointer      data)
{
  g_idle_add_full (G_PRIORITY_DEFAULT, pending_cancel_idle_cb,
                   pending_acquire (data), pending_release);
}

/* ------------------------------------------------------------------------ */
/* ShellLuaSearchProvider                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_SEARCH_PROVIDER (shell_lua_search_provider_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaSearchProvider, shell_lua_search_provider,
                      SHELL, LUA_SEARCH_PROVIDER, GObject)

struct _ShellLuaSearchProvider
{
  GObject parent;

  char *id;             /* the id the extension gave */
  char *full_id;        /* <uuid>:<id>, the id the search view knows */
  char *owner;          /* the uuid, "" for a state without an extension */
  char *title;
  GIcon *icon;

  ShellLuaCallback *on_search;
  ShellLuaCallback *on_activate;
  ShellLuaCallback *on_launch;

  GWeakRef controller;
  gboolean destroyed;

  Pending *pending;     /* the current search */
  GHashTable *results;  /* id -> ResultData, of the last answer */
};

static void shell_lua_search_provider_iface_init (ShellSearchProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellLuaSearchProvider, shell_lua_search_provider, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_SEARCH_PROVIDER,
                                                      shell_lua_search_provider_iface_init))

enum
{
  /* Overridden properties of ShellSearchProvider */
  PROP_ID = 1,
  PROP_APP_INFO,
  PROP_CAN_LAUNCH_SEARCH,
  PROP_IS_REMOTE_PROVIDER,
  PROP_MAX_RESULTS,
};

/* full_id -> the provider (not owned): the ids are unique and counted per
 * extension */
static GHashTable *live_providers = NULL;

static void
live_providers_remove (ShellLuaSearchProvider *self)
{
  if (live_providers != NULL && g_hash_table_lookup (live_providers, self->full_id) == self)
    g_hash_table_remove (live_providers, self->full_id);
}

static guint
live_providers_count (const char *owner)
{
  GHashTableIter iter;
  gpointer value;
  guint n = 0;

  if (live_providers == NULL)
    return 0;

  g_hash_table_iter_init (&iter, live_providers);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      if (g_str_equal (SHELL_LUA_SEARCH_PROVIDER (value)->owner, owner))
        n++;
    }

  return n;
}

static void
provider_drop_pending (ShellLuaSearchProvider *self)
{
  Pending *pending = g_steal_pointer (&self->pending);

  if (pending == NULL)
    return;

  pending_cancel (pending);
  pending_release (pending);
}

/* The search is over for this provider: ownership ended or the object dies */
static void
provider_shutdown (ShellLuaSearchProvider *self)
{
  g_autoptr (ShellSearchController) controller = NULL;

  if (self->destroyed)
    return;

  self->destroyed = TRUE;
  live_providers_remove (self);
  provider_drop_pending (self);

  controller = g_weak_ref_get (&self->controller);
  if (controller != NULL)
    shell_search_controller_remove_provider (controller, SHELL_SEARCH_PROVIDER (self));
  g_weak_ref_set (&self->controller, NULL);

  g_clear_pointer (&self->results, g_hash_table_unref);
}

static void
shell_lua_search_provider_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_ID:
      g_value_set_string (value, self->full_id);
      break;

    case PROP_APP_INFO:
      g_value_set_object (value, NULL);
      break;

    case PROP_CAN_LAUNCH_SEARCH:
      g_value_set_boolean (value, self->on_launch != NULL);
      break;

    case PROP_IS_REMOTE_PROVIDER:
      g_value_set_boolean (value, FALSE);
      break;

    case PROP_MAX_RESULTS:
      g_value_set_int (value, -1);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_search_provider_dispose (GObject *object)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (object);

  provider_shutdown (self);

  G_OBJECT_CLASS (shell_lua_search_provider_parent_class)->dispose (object);
}

static void
shell_lua_search_provider_finalize (GObject *object)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (object);

  g_clear_pointer (&self->on_search, shell_lua_callback_unref);
  g_clear_pointer (&self->on_activate, shell_lua_callback_unref);
  g_clear_pointer (&self->on_launch, shell_lua_callback_unref);
  g_clear_object (&self->icon);
  g_free (self->id);
  g_free (self->full_id);
  g_free (self->owner);
  g_free (self->title);
  g_weak_ref_clear (&self->controller);

  G_OBJECT_CLASS (shell_lua_search_provider_parent_class)->finalize (object);
}

static void
shell_lua_search_provider_class_init (ShellLuaSearchProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_search_provider_get_property;
  object_class->dispose = shell_lua_search_provider_dispose;
  object_class->finalize = shell_lua_search_provider_finalize;

  g_object_class_override_property (object_class, PROP_ID, "id");
  g_object_class_override_property (object_class, PROP_APP_INFO, "app-info");
  g_object_class_override_property (object_class, PROP_CAN_LAUNCH_SEARCH, "can-launch-search");
  g_object_class_override_property (object_class, PROP_IS_REMOTE_PROVIDER, "is-remote-provider");
  g_object_class_override_property (object_class, PROP_MAX_RESULTS, "max-results");
}

static void
shell_lua_search_provider_init (ShellLuaSearchProvider *self)
{
  g_weak_ref_init (&self->controller, NULL);
  self->results = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, result_data_free);
}

/* ------------------------------------------------------------------------ */
/* The interface                                                             */
/* ------------------------------------------------------------------------ */

/* Arguments of on_search (after the handle): the terms and `respond` */
typedef struct
{
  const char * const *terms;
  Pending *pending;
} SearchArgs;

static void
push_terms (lua_State          *L,
            const char * const *terms)
{
  guint n = terms != NULL ? g_strv_length ((char **) terms) : 0;

  lua_createtable (L, n, 0);
  for (guint i = 0; i < n; i++)
    {
      lua_pushstring (L, terms[i]);
      lua_rawseti (L, -2, i + 1);
    }
}

/* The upvalue of `respond`: a reference to the search it answers */
typedef struct
{
  Pending *pending;
} RespondData;

static int l_respond (lua_State *L);

static void
push_search_args (lua_State *L,
                  gpointer   data)
{
  SearchArgs *args = data;
  RespondData *respond;

  push_terms (L, args->terms);

  respond = lua_newuserdatauv (L, sizeof (RespondData), 0);
  respond->pending = pending_acquire (args->pending);
  luaL_setmetatable (L, RESPOND_METATABLE);
  lua_pushcclosure (L, l_respond, 1);
}

static gboolean
pending_timeout_cb (gpointer data)
{
  Pending *pending = data;
  g_autoptr (ShellLuaSearchProvider) self = g_weak_ref_get (&pending->provider);

  pending->timeout_id = 0;

  if (pending_is_open (pending))
    {
      g_message ("The search provider %s did not answer in %d seconds",
                 self != NULL ? self->full_id : "?", ANSWER_TIMEOUT);
      pending_answer (pending, NULL);
    }

  return G_SOURCE_REMOVE;
}

static void
provider_start_search (ShellLuaSearchProvider *self,
                       const char * const     *terms,
                       GCancellable           *cancellable,
                       ShellSearchRequest     *request)
{
  Pending *pending;
  SearchArgs args = { terms, NULL };

  /* A new search cancels the previous one */
  provider_drop_pending (self);

  if (self->destroyed)
    {
      shell_search_request_return_results (request, NULL);
      return;
    }

  pending = pending_new (request, cancellable);
  g_weak_ref_set (&pending->provider, self);
  self->pending = pending_acquire (pending);

  if (cancellable != NULL)
    pending->cancel_id = g_cancellable_connect (cancellable, G_CALLBACK (pending_cancelled_cb),
                                                pending_acquire (pending), pending_release);
  if (pending_is_open (pending))
    pending->timeout_id = g_timeout_add_seconds (ANSWER_TIMEOUT, pending_timeout_cb, pending);

  args.pending = pending;
  if (!shell_lua_callback_emit (self->on_search, 2, push_search_args, &args))
    {
      /* An error in the callback, or the extension is being disabled */
      pending_answer (pending, NULL);
    }

  pending_release (pending);
}

static void
lua_provider_get_initial_result_set (ShellSearchProvider *provider,
                                     const char * const  *terms,
                                     GCancellable        *cancellable,
                                     ShellSearchRequest  *request)
{
  provider_start_search (SHELL_LUA_SEARCH_PROVIDER (provider), terms, cancellable, request);
}

static void
lua_provider_get_subsearch_result_set (ShellSearchProvider *provider,
                                       const char * const  *previous_results,
                                       const char * const  *terms,
                                       GCancellable        *cancellable,
                                       ShellSearchRequest  *request)
{
  /* The extension always searches for the whole terms */
  provider_start_search (SHELL_LUA_SEARCH_PROVIDER (provider), terms, cancellable, request);
}

static ClutterActor *
create_result_icon (int      size,
                    gpointer user_data)
{
  const ResultData *result = user_data;

  if (result->icon_name != NULL)
    return g_object_new (ST_TYPE_ICON,
                         "icon-name", result->icon_name,
                         "icon-size", size,
                         NULL);

  if (result->icon_file != NULL)
    {
      g_autoptr (GFile) file = g_file_new_for_path (result->icon_file);
      g_autoptr (GIcon) icon = g_file_icon_new (file);

      return g_object_new (ST_TYPE_ICON,
                           "gicon", icon,
                           "icon-size", size,
                           NULL);
    }

  return NULL;
}

static ShellSearchResultMeta *
make_meta (const ResultData *result)
{
  ShellSearchResultMeta *meta = shell_search_result_meta_new (result->id, result->name);
  ResultData *icon_data;

  if (result->description != NULL)
    shell_search_result_meta_set_description (meta, result->description);
  if (result->clipboard_text != NULL)
    shell_search_result_meta_set_clipboard_text (meta, result->clipboard_text);

  if (result->icon_name != NULL || result->icon_file != NULL)
    {
      /* The icon function outlives the answer it was made from */
      icon_data = g_new0 (ResultData, 1);
      icon_data->icon_name = g_strdup (result->icon_name);
      icon_data->icon_file = g_strdup (result->icon_file);
      shell_search_result_meta_set_create_icon_func (meta, create_result_icon, icon_data,
                                                     result_data_free);
    }

  return meta;
}

static void
lua_provider_get_result_metas (ShellSearchProvider *provider,
                               const char * const  *ids,
                               GCancellable        *cancellable,
                               ShellSearchRequest  *request)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (provider);
  g_autoptr (GPtrArray) metas = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; ids[i] != NULL; i++)
    {
      const ResultData *result = self->results != NULL ? g_hash_table_lookup (self->results, ids[i]) : NULL;

      if (result == NULL)
        {
          /* A newer answer replaced the one the identifiers are from */
          shell_search_request_return_gerror (request,
                                              g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                                   "The results are not current any more"));
          return;
        }

      g_ptr_array_add (metas, make_meta (result));
    }

  shell_search_request_return_metas (request, (ShellSearchResultMeta **) metas->pdata, metas->len);
}

static void
push_activate_args (lua_State *L,
                    gpointer   data)
{
  const char * const *terms = ((gpointer *) data)[1];

  lua_pushstring (L, ((gpointer *) data)[0]);
  push_terms (L, terms);
}

static void
lua_provider_activate_result (ShellSearchProvider *provider,
                              const char          *id,
                              const char * const  *terms)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (provider);
  gpointer args[2] = { (gpointer) id, (gpointer) terms };

  if (self->destroyed)
    return;

  shell_lua_callback_emit (self->on_activate, 2, push_activate_args, args);
}

static void
push_launch_args (lua_State *L,
                  gpointer   data)
{
  push_terms (L, data);
}

static void
lua_provider_launch_search (ShellSearchProvider *provider,
                            const char * const  *terms)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (provider);

  if (self->destroyed || self->on_launch == NULL)
    return;

  shell_lua_callback_emit (self->on_launch, 1, push_launch_args, (gpointer) terms);
}

static char *
lua_provider_get_title (ShellSearchProvider *provider)
{
  return g_strdup (SHELL_LUA_SEARCH_PROVIDER (provider)->title);
}

static GIcon *
lua_provider_get_icon (ShellSearchProvider *provider)
{
  ShellLuaSearchProvider *self = SHELL_LUA_SEARCH_PROVIDER (provider);

  return self->icon != NULL ? g_object_ref (self->icon) : NULL;
}

static char *
lua_provider_get_owner (ShellSearchProvider *provider)
{
  return g_strdup (SHELL_LUA_SEARCH_PROVIDER (provider)->owner);
}

static void
shell_lua_search_provider_iface_init (ShellSearchProviderInterface *iface)
{
  iface->get_initial_result_set = lua_provider_get_initial_result_set;
  iface->get_subsearch_result_set = lua_provider_get_subsearch_result_set;
  iface->get_result_metas = lua_provider_get_result_metas;
  iface->activate_result = lua_provider_activate_result;
  iface->launch_search = lua_provider_launch_search;
  iface->get_title = lua_provider_get_title;
  iface->get_icon = lua_provider_get_icon;
  iface->get_owner = lua_provider_get_owner;
}

/* ------------------------------------------------------------------------ */
/* respond (results)                                                         */
/* ------------------------------------------------------------------------ */

static const char * const result_fields[] = {
  "id", "name", "description", "icon_name", "icon_file", "clipboard_text", NULL
};

/* The string field of the table at @tbl (an absolute index), or NULL if
 * absent; raises if it is not a string. The field is read without
 * metamethods, so that the table keeps the string alive for the pointer. */
static const char *
raw_string (lua_State  *L,
            int         tbl,
            const char *field)
{
  const char *value;

  lua_pushstring (L, field);
  lua_rawget (L, tbl);
  if (lua_isnil (L, -1))
    {
      lua_pop (L, 1);
      return NULL;
    }
  if (lua_type (L, -1) != LUA_TSTRING)
    luaL_error (L, "field '%s' must be a string, got %s", field, luaL_typename (L, -1));

  value = lua_tostring (L, -1);
  lua_pop (L, 1);
  return value;
}

static gboolean
has_image_suffix (const char *path)
{
  gsize len = strlen (path);

  return len > 4 &&
         (g_ascii_strcasecmp (path + len - 4, ".svg") == 0 ||
          g_ascii_strcasecmp (path + len - 4, ".png") == 0);
}

/* Reads the sequence at index 2 into the GPtrArray of the light userdata at 1
 * (ResultData; whatever was read stays there when the function raises). Every
 * value of an item is checked before the item is allocated, and nothing between
 * the allocation and the insertion can raise, so nothing leaks. */
static int
parse_results (lua_State *L)
{
  GPtrArray *out = lua_touserdata (L, 1);
  lua_Integer n = 0;

  if (lua_type (L, 2) != LUA_TTABLE)
    return luaL_error (L, "results must be a sequence of tables, got %s", luaL_typename (L, 2));

  /* A sequence: the keys are 1..n */
  lua_pushnil (L);
  while (lua_next (L, 2))
    {
      if (!lua_isinteger (L, -2) || lua_tointeger (L, -2) < 1)
        return luaL_error (L, "results must be a sequence of tables (found the key '%s')",
                           lua_type (L, -2) == LUA_TSTRING ? lua_tostring (L, -2) : luaL_typename (L, -2));
      n = MAX (n, lua_tointeger (L, -2));
      lua_pop (L, 1);
    }

  for (lua_Integer i = 1; i <= MIN (n, MAX_RESULTS); i++)
    {
      ResultData *result;
      const char *id, *name, *description, *icon_name, *icon_file, *clipboard_text;
      const char *icon_path = NULL;

      lua_settop (L, 2);
      lua_rawgeti (L, 2, i);
      if (lua_type (L, 3) != LUA_TTABLE)
        return luaL_error (L, "results[%d] must be a table, got %s", (int) i, luaL_typename (L, 3));

      shell_lua_check_fields (L, 3, result_fields);

      id = raw_string (L, 3, "id");
      name = raw_string (L, 3, "name");
      description = raw_string (L, 3, "description");
      icon_name = raw_string (L, 3, "icon_name");
      icon_file = raw_string (L, 3, "icon_file");
      clipboard_text = raw_string (L, 3, "clipboard_text");

      if (id == NULL || id[0] == '\0')
        return luaL_error (L, "results[%d]: missing required field 'id' (a non-empty string)", (int) i);
      if (name == NULL || name[0] == '\0')
        return luaL_error (L, "results[%d]: missing required field 'name' (a non-empty string)", (int) i);
      if (icon_name != NULL && icon_file != NULL)
        return luaL_error (L, "results[%d]: only one of 'icon_name' and 'icon_file' can be given", (int) i);
      if (icon_name != NULL && icon_name[0] == '\0')
        return luaL_error (L, "results[%d]: field 'icon_name' must not be empty", (int) i);

      if (icon_file != NULL)
        {
          if (g_path_is_absolute (icon_file))
            {
              if (!has_image_suffix (icon_file))
                return luaL_error (L, "results[%d]: field 'icon_file' (%s): must be a .svg or .png file",
                                   (int) i, icon_file);
              icon_path = icon_file;
            }
          else
            {
              /* Raises for a bad or missing file; the string stays on the stack */
              icon_path = shell_lua_api_push_extension_file (L, "icon_file", icon_file);
            }
        }

      result = g_new0 (ResultData, 1);
      result->id = g_strdup (id);
      result->name = g_strdup (name);
      result->description = g_strdup (description);
      result->icon_name = g_strdup (icon_name);
      result->icon_file = g_strdup (icon_path);
      result->clipboard_text = g_strdup (clipboard_text);
      g_ptr_array_add (out, result);
    }

  return 0;
}

/* respond (results): answers the search it was made for */
static int
l_respond (lua_State *L)
{
  RespondData *data = lua_touserdata (L, lua_upvalueindex (1));
  Pending *pending = data->pending;
  ShellLuaSearchProvider *self = g_weak_ref_get (&pending->provider);
  GPtrArray *parsed;
  GHashTable *results;
  GPtrArray *ids;

  /* Not the current search any more, answered already, or the provider is
   * gone: nothing to do */
  if (self == NULL || self->destroyed || self->pending != pending || !pending_is_open (pending))
    {
      g_clear_object (&self);
      return 0;
    }

  parsed = g_ptr_array_new_with_free_func (result_data_free);
  lua_pushcfunction (L, parse_results);
  lua_pushlightuserdata (L, parsed);
  lua_pushvalue (L, 1);
  if (lua_pcall (L, 2, 0, 0) != LUA_OK)
    {
      /* The search is answered with no results, the error is the callback's */
      pending_answer (pending, NULL);
      g_ptr_array_unref (parsed);
      g_object_unref (self);
      return lua_error (L);
    }

  results = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, result_data_free);
  ids = g_ptr_array_new ();
  for (guint i = 0; i < parsed->len; i++)
    {
      ResultData *result = g_ptr_array_index (parsed, i);

      /* The first of two results with the same id stays */
      if (g_hash_table_contains (results, result->id))
        continue;

      g_hash_table_insert (results, result->id, result);
      g_ptr_array_index (parsed, i) = NULL;
      g_ptr_array_add (ids, result->id);
    }
  g_ptr_array_add (ids, NULL);
  g_ptr_array_unref (parsed);

  /* Before the answer: the view asks for the metas at once */
  g_clear_pointer (&self->results, g_hash_table_unref);
  self->results = results;

  pending_answer (pending, (const char * const *) ids->pdata);

  g_ptr_array_unref (ids);
  g_object_unref (self);
  return 0;
}

static int
l_respond_gc (lua_State *L)
{
  RespondData *data = lua_touserdata (L, 1);

  if (data->pending != NULL)
    pending_release (g_steal_pointer (&data->pending));
  return 0;
}

/* ------------------------------------------------------------------------ */
/* shell.SearchProvider                                                      */
/* ------------------------------------------------------------------------ */

static void
get_provider_id (lua_State *L,
                 GObject   *object)
{
  lua_pushstring (L, SHELL_LUA_SEARCH_PROVIDER (object)->id);
}

static void
provider_destroy (GObject *object)
{
  provider_shutdown (SHELL_LUA_SEARCH_PROVIDER (object));
}

static const ShellLuaOwnOps provider_ops = {
  .destroy = provider_destroy,
};

static const ShellLuaHandleMethod provider_methods[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty provider_properties[] = {
  { NULL }
};

static const ShellLuaHandleVirtual provider_virtuals[] = {
  { "id", get_provider_id, NULL },
  { NULL }
};

static ShellLuaHandleType provider_type = {
  .name = PROVIDER_TYPE,
  .properties = provider_properties,
  .methods_ex = provider_methods,
  .virtuals = provider_virtuals,
};

/* ------------------------------------------------------------------------ */
/* shell.search.add_provider                                                 */
/* ------------------------------------------------------------------------ */

static const char * const provider_fields[] = {
  "id", "name", "icon_name", "icon_file", "on_search", "on_activate", "on_launch", NULL
};

static ShellSearchController *
get_controller (void)
{
  ShellMain *main = shell_main_get_default ();
  ShellOverview *overview = main != NULL ? shell_main_get_overview (main) : NULL;

  return overview != NULL ? shell_overview_get_search_controller (overview) : NULL;
}

/* Checks that the field is a function (or absent, if it is not required),
 * without taking a reference */
static void
check_function_field (lua_State  *L,
                      const char *field,
                      gboolean    required)
{
  lua_getfield (L, 1, field);
  if (lua_isnil (L, -1))
    {
      if (required)
        luaL_error (L, "missing required field '%s' (a function)", field);
    }
  else if (lua_type (L, -1) != LUA_TFUNCTION)
    {
      luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
    }
  lua_pop (L, 1);
}

static int
l_add_provider (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  ShellSearchController *controller;
  ShellLuaSearchProvider *provider;
  const char *id, *name, *icon_name, *icon_file;
  const char *icon_path = NULL;
  const char *uuid;
  char *full_id;
  int search_ref, activate_ref, launch_ref;

  shell_lua_require_capability (L, "search", "shell.search.add_provider");

  luaL_checktype (L, 1, LUA_TTABLE);
  shell_lua_check_fields (L, 1, provider_fields);

  id = raw_string (L, 1, "id");
  if (id == NULL || id[0] == '\0')
    return luaL_error (L, "missing required field 'id' (a non-empty string)");
  name = raw_string (L, 1, "name");
  if (name == NULL || name[0] == '\0')
    return luaL_error (L, "missing required field 'name' (a non-empty string)");

  icon_name = raw_string (L, 1, "icon_name");
  icon_file = raw_string (L, 1, "icon_file");
  if (icon_name != NULL && icon_file != NULL)
    return luaL_error (L, "only one of 'icon_name' and 'icon_file' can be given");
  if (icon_name != NULL && icon_name[0] == '\0')
    return luaL_error (L, "field 'icon_name' must not be empty");

  check_function_field (L, "on_search", TRUE);
  check_function_field (L, "on_activate", TRUE);
  check_function_field (L, "on_launch", FALSE);

  if (icon_file != NULL)
    icon_path = shell_lua_api_push_extension_file (L, "icon_file", icon_file);   /* kept on the stack */

  shell_lua_check_owned_capacity (L);

  /* Nothing below allocates before a call that can raise */
  uuid = extension != NULL ? shell_lua_extension_get_uuid (extension) : "";
  full_id = g_strdup_printf ("%s:%s", uuid, id);

  if (live_providers != NULL && g_hash_table_contains (live_providers, full_id))
    {
      g_free (full_id);
      return luaL_error (L, "a search provider with the id '%s' exists already", id);
    }
  if (live_providers_count (uuid) >= MAX_PROVIDERS)
    {
      g_free (full_id);
      return luaL_error (L, "too many search providers (limit %d)", MAX_PROVIDERS);
    }

  controller = get_controller ();
  if (controller == NULL)
    {
      g_free (full_id);
      lua_pushnil (L);
      lua_pushliteral (L, "the search is not available");
      return 2;
    }

  provider = g_object_new (SHELL_TYPE_LUA_SEARCH_PROVIDER, NULL);
  provider->id = g_strdup (id);
  provider->full_id = full_id;
  provider->owner = g_strdup (uuid);
  provider->title = g_strdup (name);
  if (icon_path != NULL)
    {
      g_autoptr (GFile) file = g_file_new_for_path (icon_path);

      provider->icon = g_file_icon_new (file);
    }
  else
    {
      provider->icon = g_themed_icon_new (icon_name != NULL ? icon_name : DEFAULT_ICON_NAME);
    }

  if (live_providers == NULL)
    live_providers = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (live_providers, provider->full_id, provider);

  shell_lua_own_object (L, G_OBJECT (provider), &provider_ops);   /* a reference of the context */

  search_ref = shell_lua_opt_callback (L, 1, "on_search", TRUE);
  activate_ref = shell_lua_opt_callback (L, 1, "on_activate", TRUE);
  launch_ref = shell_lua_opt_callback (L, 1, "on_launch", FALSE);

  provider->on_search = shell_lua_callback_ref (shell_lua_callback_new (L, G_OBJECT (provider), search_ref));
  provider->on_activate = shell_lua_callback_ref (shell_lua_callback_new (L, G_OBJECT (provider), activate_ref));
  if (launch_ref != LUA_NOREF)
    provider->on_launch = shell_lua_callback_ref (shell_lua_callback_new (L, G_OBJECT (provider), launch_ref));

  g_weak_ref_set (&provider->controller, controller);
  shell_search_controller_add_provider (controller, SHELL_SEARCH_PROVIDER (provider));

  shell_lua_handle_push (L, PROVIDER_TYPE, G_OBJECT (provider));
  g_object_unref (provider);   /* ours; the context and the view hold theirs */
  return 1;
}

static const luaL_Reg search_funcs[] = {
  { "add_provider", l_add_provider },
  { NULL, NULL }
};

static void
shell_lua_api_search_register (ShellLuaState *state,
                               lua_State     *L)
{
  provider_type.gtype = SHELL_TYPE_LUA_SEARCH_PROVIDER;
  shell_lua_handle_register_type (L, &provider_type);

  if (luaL_newmetatable (L, RESPOND_METATABLE))
    {
      lua_pushcfunction (L, l_respond_gc);
      lua_setfield (L, -2, "__gc");
      lua_pushboolean (L, 0);
      lua_setfield (L, -2, "__metatable");
    }
  lua_pop (L, 1);

  lua_newtable (L);
  luaL_setfuncs (L, search_funcs, 0);
  lua_setfield (L, -2, "search");
}

static void __attribute__ ((constructor))
shell_lua_api_search_init (void)
{
  shell_lua_api_register ("search", shell_lua_api_search_register);
}
