/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * remote-search.c: The search providers of installed applications. Port of
 * js/ui/remoteSearch.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The proxies are gdbus-codegen proxies (`ShellDBusSearchProvider` and
 *    `ShellDBusSearchProvider2`) instead of a `Gio.DBusProxy` made from XML
 *    in the module. A call made before the proxy is ready waits for it.
 *  - loadRemoteSearchProviders () is shell_remote_search_load_providers ().
 *    It creates the providers once the disabled, enabled and sort order
 *    settings have been applied, so no proxy is made for a provider that is
 *    not used.
 *  - A provider that cannot be reached answers with an error (a search
 *    for results answers with no results, as before), logged with
 *    g_message () like the JS `log ()`; a canceled call is never logged.
 *  - The `icon-data` of a meta is checked before the pixbuf is made, a bad
 *    one gives a result without an icon instead of a critical warning.
 */

#include "config.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n-lib.h>

#include "search-provider-dbus.h"
#include "shell-global.h"
#include "shell-util.h"
#include "ui/remote-search.h"

#define KEY_FILE_GROUP "Shell Search Provider"

/* ------------------------------------------------------------------------ */
/* ShellRemoteSearchProvider                                                 */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_APP_INFO,
  PROP_BUS_NAME,
  PROP_OBJECT_PATH,
  PROP_AUTO_START,
  PROP_DEFAULT_ENABLED,

  /* Overridden properties of ShellSearchProvider */
  PROP_ID,
  PROP_CAN_LAUNCH_SEARCH,
  PROP_IS_REMOTE_PROVIDER,
  PROP_MAX_RESULTS,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

typedef enum
{
  CALL_INITIAL_RESULT_SET,
  CALL_SUBSEARCH_RESULT_SET,
  CALL_RESULT_METAS,
  CALL_ACTIVATE_RESULT,
  CALL_LAUNCH_SEARCH,
} CallKind;

/* A call to the provider, started as soon as the proxy is ready */
typedef struct
{
  CallKind kind;
  char *provider_id;
  ShellSearchRequest *request; /* nullable for activations and launches */
  GCancellable *cancellable;   /* nullable */
  char **previous_results;
  char **terms;                /* terms, or identifiers for the metas */
  char *identifier;
  uint32_t timestamp;
  gboolean version2;
} Call;

typedef struct
{
  GAppInfo *app_info;
  char *bus_name;
  char *object_path;
  gboolean auto_start;
  gboolean default_enabled;

  GDBusProxy *proxy;
  gboolean proxy_failed;
  GError *proxy_error;
  GCancellable *proxy_cancellable;
  GQueue waiting_calls; /* Call, waiting for the proxy */
} ShellRemoteSearchProviderPrivate;

static void shell_remote_search_provider_iface_init (ShellSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (ShellRemoteSearchProvider, shell_remote_search_provider, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (ShellRemoteSearchProvider)
                         G_IMPLEMENT_INTERFACE (SHELL_TYPE_SEARCH_PROVIDER,
                                                shell_remote_search_provider_iface_init))

struct _ShellRemoteSearchProvider2
{
  ShellRemoteSearchProvider parent;
};

G_DEFINE_FINAL_TYPE (ShellRemoteSearchProvider2, shell_remote_search_provider2,
                     SHELL_TYPE_REMOTE_SEARCH_PROVIDER)

static void
call_free (Call *call)
{
  g_free (call->provider_id);
  g_clear_object (&call->request);
  g_clear_object (&call->cancellable);
  g_strfreev (call->previous_results);
  g_strfreev (call->terms);
  g_free (call->identifier);
  g_free (call);
}

static Call *
call_new (ShellRemoteSearchProvider *self,
          CallKind                   kind)
{
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);
  Call *call = g_new0 (Call, 1);

  call->kind = kind;
  call->provider_id = g_strdup (g_app_info_get_id (priv->app_info));
  call->version2 = SHELL_IS_REMOTE_SEARCH_PROVIDER2 (self);

  return call;
}

static uint32_t
get_current_time (void)
{
  return shell_global_get_current_time (shell_global_get ());
}

/* ------------------------------------------------------------------------ */
/* Metas                                                                     */
/* ------------------------------------------------------------------------ */

static GIcon *
icon_from_icon_data (GVariant *dict)
{
  g_autoptr (GVariant) icon_data = g_variant_lookup_value (dict, "icon-data",
                                                           G_VARIANT_TYPE ("(iiibiiay)"));
  g_autoptr (GVariant) bytes_variant = NULL;
  gint32 width, height, rowstride, bits_per_sample, n_channels_unused;
  gboolean has_alpha;
  const guchar *data;
  gsize len = 0;
  gsize row_min, required;

  if (icon_data == NULL)
    return NULL;

  g_variant_get (icon_data, "(iiibii@ay)", &width, &height, &rowstride, &has_alpha,
                 &bits_per_sample, &n_channels_unused, &bytes_variant);

  data = g_variant_get_fixed_array (bytes_variant, &len, 1);

  /* The checks of shell_util_create_pixbuf_from_data (), which would
   * criticize the data of a provider */
  if (width <= 0 || height <= 0 || bits_per_sample != 8)
    return NULL;

  row_min = (gsize) width * (((has_alpha ? 4 : 3) * bits_per_sample + 7) / 8);
  if (rowstride < 0 || (gsize) rowstride < row_min)
    return NULL;

  required = (gsize) (height - 1) * rowstride + row_min;
  if (len < required)
    return NULL;

  return G_ICON (shell_util_create_pixbuf_from_data (g_memdup2 (data, len), len,
                                                     GDK_COLORSPACE_RGB, has_alpha,
                                                     bits_per_sample, width, height,
                                                     rowstride));
}

static ClutterActor *
create_icon_from_meta (int      size,
                       gpointer user_data)
{
  GVariant *dict = user_data;
  g_autoptr (GIcon) gicon = NULL;
  g_autoptr (GVariant) icon = g_variant_lookup_value (dict, "icon", NULL);
  const char *gicon_string = NULL;

  if (icon != NULL)
    {
      gicon = g_icon_deserialize (icon);
    }
  else if (g_variant_lookup (dict, "gicon", "&s", &gicon_string))
    {
      g_autoptr (GError) error = NULL;

      gicon = g_icon_new_for_string (gicon_string, &error);
      if (gicon == NULL)
        g_debug ("Could not make the icon %s of a search result: %s",
                 gicon_string, error->message);
    }
  else
    {
      gicon = icon_from_icon_data (dict);
    }

  if (gicon == NULL)
    return NULL;

  return g_object_new (ST_TYPE_ICON,
                       "gicon", gicon,
                       "icon-size", size,
                       NULL);
}

static ShellSearchResultMeta *
meta_from_variant (GVariant *dict)
{
  ShellSearchResultMeta *meta;
  const char *id = NULL, *name = NULL, *description = NULL, *clipboard_text = NULL;

  g_variant_lookup (dict, "id", "&s", &id);
  g_variant_lookup (dict, "name", "&s", &name);
  g_variant_lookup (dict, "description", "&s", &description);
  g_variant_lookup (dict, "clipboardText", "&s", &clipboard_text);

  meta = shell_search_result_meta_new (id, name);
  shell_search_result_meta_set_description (meta, description);
  shell_search_result_meta_set_clipboard_text (meta, clipboard_text);
  shell_search_result_meta_set_create_icon_func (meta, create_icon_from_meta,
                                                 g_variant_ref (dict),
                                                 (GDestroyNotify) g_variant_unref);

  return meta;
}

/* ------------------------------------------------------------------------ */
/* Calls                                                                     */
/* ------------------------------------------------------------------------ */

static void
log_call_error (const Call *call,
                const GError *error)
{
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  switch (call->kind)
    {
    case CALL_RESULT_METAS:
      g_message ("Received error from D-Bus search provider %s during GetResultMetas: %s",
                 call->provider_id, error->message);
      break;

    case CALL_ACTIVATE_RESULT:
      g_warning ("Could not activate a result of the D-Bus search provider %s: %s",
                 call->provider_id, error->message);
      break;

    case CALL_LAUNCH_SEARCH:
      g_warning ("Could not launch the search of the D-Bus search provider %s: %s",
                 call->provider_id, error->message);
      break;

    default:
      g_message ("Received error from D-Bus search provider %s: %s",
                 call->provider_id, error->message);
    }
}

static void
call_done_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data)
{
  Call *call = user_data;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) results = NULL;
  g_autoptr (GVariant) metas_variant = NULL;
  gboolean ok = FALSE;

  switch (call->kind)
    {
    case CALL_INITIAL_RESULT_SET:
      if (call->version2)
        ok = shell_dbus_search_provider2_call_get_initial_result_set_finish (SHELL_DBUS_SEARCH_PROVIDER2 (source),
                                                                             &results, res, &error);
      else
        ok = shell_dbus_search_provider_call_get_initial_result_set_finish (SHELL_DBUS_SEARCH_PROVIDER (source),
                                                                            &results, res, &error);
      break;

    case CALL_SUBSEARCH_RESULT_SET:
      if (call->version2)
        ok = shell_dbus_search_provider2_call_get_subsearch_result_set_finish (SHELL_DBUS_SEARCH_PROVIDER2 (source),
                                                                               &results, res, &error);
      else
        ok = shell_dbus_search_provider_call_get_subsearch_result_set_finish (SHELL_DBUS_SEARCH_PROVIDER (source),
                                                                              &results, res, &error);
      break;

    case CALL_RESULT_METAS:
      if (call->version2)
        ok = shell_dbus_search_provider2_call_get_result_metas_finish (SHELL_DBUS_SEARCH_PROVIDER2 (source),
                                                                       &metas_variant, res, &error);
      else
        ok = shell_dbus_search_provider_call_get_result_metas_finish (SHELL_DBUS_SEARCH_PROVIDER (source),
                                                                      &metas_variant, res, &error);
      break;

    case CALL_ACTIVATE_RESULT:
      if (call->version2)
        ok = shell_dbus_search_provider2_call_activate_result_finish (SHELL_DBUS_SEARCH_PROVIDER2 (source),
                                                                      res, &error);
      else
        ok = shell_dbus_search_provider_call_activate_result_finish (SHELL_DBUS_SEARCH_PROVIDER (source),
                                                                     res, &error);
      break;

    case CALL_LAUNCH_SEARCH:
      ok = shell_dbus_search_provider2_call_launch_search_finish (SHELL_DBUS_SEARCH_PROVIDER2 (source),
                                                                  res, &error);
      break;

    default:
      g_assert_not_reached ();
    }

  if (!ok)
    {
      g_dbus_error_strip_remote_error (error);
      log_call_error (call, error);

      if (call->request != NULL)
        {
          if (call->kind == CALL_RESULT_METAS || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              shell_search_request_return_gerror (call->request, g_steal_pointer (&error));
            }
          else
            {
              /* No results rather than an error, as the JS returned [] */
              const char * const no_results[] = { NULL };

              shell_search_request_return_results (call->request, no_results);
            }
        }
    }
  else if (call->request != NULL)
    {
      if (call->kind == CALL_RESULT_METAS)
        {
          g_autoptr (GPtrArray) metas = g_ptr_array_new_with_free_func (g_object_unref);
          GVariantIter iter;
          GVariant *dict;

          g_variant_iter_init (&iter, metas_variant);
          while ((dict = g_variant_iter_next_value (&iter)) != NULL)
            {
              g_ptr_array_add (metas, meta_from_variant (dict));
              g_variant_unref (dict);
            }

          shell_search_request_return_metas (call->request,
                                             (ShellSearchResultMeta **) metas->pdata,
                                             metas->len);
        }
      else
        {
          shell_search_request_return_results (call->request, (const char * const *) results);
        }
    }

  call_free (call);
}

static void
call_start (ShellRemoteSearchProvider *self,
            Call                      *call)
{
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);
  GCancellable *cancellable = call->cancellable != NULL ? call->cancellable
                                                        : priv->proxy_cancellable;

  switch (call->kind)
    {
    case CALL_INITIAL_RESULT_SET:
      if (call->version2)
        shell_dbus_search_provider2_call_get_initial_result_set (SHELL_DBUS_SEARCH_PROVIDER2 (priv->proxy),
                                                                 (const char * const *) call->terms,
                                                                 cancellable, call_done_cb, call);
      else
        shell_dbus_search_provider_call_get_initial_result_set (SHELL_DBUS_SEARCH_PROVIDER (priv->proxy),
                                                                (const char * const *) call->terms,
                                                                cancellable, call_done_cb, call);
      break;

    case CALL_SUBSEARCH_RESULT_SET:
      if (call->version2)
        shell_dbus_search_provider2_call_get_subsearch_result_set (SHELL_DBUS_SEARCH_PROVIDER2 (priv->proxy),
                                                                   (const char * const *) call->previous_results,
                                                                   (const char * const *) call->terms,
                                                                   cancellable, call_done_cb, call);
      else
        shell_dbus_search_provider_call_get_subsearch_result_set (SHELL_DBUS_SEARCH_PROVIDER (priv->proxy),
                                                                  (const char * const *) call->previous_results,
                                                                  (const char * const *) call->terms,
                                                                  cancellable, call_done_cb, call);
      break;

    case CALL_RESULT_METAS:
      if (call->version2)
        shell_dbus_search_provider2_call_get_result_metas (SHELL_DBUS_SEARCH_PROVIDER2 (priv->proxy),
                                                           (const char * const *) call->terms,
                                                           cancellable, call_done_cb, call);
      else
        shell_dbus_search_provider_call_get_result_metas (SHELL_DBUS_SEARCH_PROVIDER (priv->proxy),
                                                          (const char * const *) call->terms,
                                                          cancellable, call_done_cb, call);
      break;

    case CALL_ACTIVATE_RESULT:
      if (call->version2)
        shell_dbus_search_provider2_call_activate_result (SHELL_DBUS_SEARCH_PROVIDER2 (priv->proxy),
                                                          call->identifier,
                                                          (const char * const *) call->terms,
                                                          call->timestamp,
                                                          cancellable, call_done_cb, call);
      else
        shell_dbus_search_provider_call_activate_result (SHELL_DBUS_SEARCH_PROVIDER (priv->proxy),
                                                         call->identifier,
                                                         cancellable, call_done_cb, call);
      break;

    case CALL_LAUNCH_SEARCH:
      shell_dbus_search_provider2_call_launch_search (SHELL_DBUS_SEARCH_PROVIDER2 (priv->proxy),
                                                      (const char * const *) call->terms,
                                                      call->timestamp,
                                                      cancellable, call_done_cb, call);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
call_fail (Call *call,
           const GError *error)
{
  if (call->request != NULL)
    {
      if (call->kind == CALL_RESULT_METAS ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          shell_search_request_return_gerror (call->request, g_error_copy (error));
        }
      else
        {
          const char * const no_results[] = { NULL };

          shell_search_request_return_results (call->request, no_results);
        }
    }

  log_call_error (call, error);
  call_free (call);
}

/* Starts @call, or keeps it until the proxy is ready */
static void
queue_call (ShellRemoteSearchProvider *self,
            Call                      *call)
{
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  if (priv->proxy != NULL)
    call_start (self, call);
  else if (priv->proxy_failed)
    call_fail (call, priv->proxy_error);
  else
    g_queue_push_tail (&priv->waiting_calls, call);
}

typedef struct
{
  GWeakRef provider;
  gboolean version2;
} ProxyRequest;

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  ProxyRequest *request = user_data;
  g_autoptr (ShellRemoteSearchProvider) self = g_weak_ref_get (&request->provider);
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusProxy) proxy = NULL;
  ShellRemoteSearchProviderPrivate *priv;
  Call *call;

  if (request->version2)
    proxy = G_DBUS_PROXY (shell_dbus_search_provider2_proxy_new_for_bus_finish (res, &error));
  else
    proxy = G_DBUS_PROXY (shell_dbus_search_provider_proxy_new_for_bus_finish (res, &error));

  g_weak_ref_clear (&request->provider);
  g_free (request);

  if (self == NULL)
    return;

  priv = shell_remote_search_provider_get_instance_private (self);

  if (proxy == NULL)
    {
      priv->proxy_failed = TRUE;
      priv->proxy_error = g_steal_pointer (&error);
      if (priv->proxy_error == NULL)
        priv->proxy_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                 "Could not create the proxy");

      while ((call = g_queue_pop_head (&priv->waiting_calls)) != NULL)
        call_fail (call, priv->proxy_error);
      return;
    }

  priv->proxy = g_steal_pointer (&proxy);
  while ((call = g_queue_pop_head (&priv->waiting_calls)) != NULL)
    call_start (self, call);
}

/* ------------------------------------------------------------------------ */
/* ShellSearchProvider                                                       */
/* ------------------------------------------------------------------------ */

static void
remote_get_initial_result_set (ShellSearchProvider *provider,
                               const char * const  *terms,
                               GCancellable        *cancellable,
                               ShellSearchRequest  *request)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (provider);
  Call *call = call_new (self, CALL_INITIAL_RESULT_SET);

  call->request = g_object_ref (request);
  call->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  call->terms = g_strdupv ((char **) terms);

  queue_call (self, call);
}

static void
remote_get_subsearch_result_set (ShellSearchProvider *provider,
                                 const char * const  *previous_results,
                                 const char * const  *terms,
                                 GCancellable        *cancellable,
                                 ShellSearchRequest  *request)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (provider);
  Call *call = call_new (self, CALL_SUBSEARCH_RESULT_SET);

  call->request = g_object_ref (request);
  call->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  call->previous_results = g_strdupv ((char **) previous_results);
  call->terms = g_strdupv ((char **) terms);

  queue_call (self, call);
}

static void
remote_get_result_metas (ShellSearchProvider *provider,
                         const char * const  *ids,
                         GCancellable        *cancellable,
                         ShellSearchRequest  *request)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (provider);
  Call *call = call_new (self, CALL_RESULT_METAS);

  call->request = g_object_ref (request);
  call->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  call->terms = g_strdupv ((char **) ids);

  queue_call (self, call);
}

static void
remote_activate_result (ShellSearchProvider *provider,
                        const char          *id,
                        const char * const  *terms)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (provider);
  Call *call = call_new (self, CALL_ACTIVATE_RESULT);

  call->identifier = g_strdup (id);
  call->terms = terms != NULL ? g_strdupv ((char **) terms) : g_new0 (char *, 1);
  call->timestamp = get_current_time ();

  queue_call (self, call);
}

static void
remote_launch_search (ShellSearchProvider *provider,
                      const char * const  *terms)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (provider);
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  if (SHELL_IS_REMOTE_SEARCH_PROVIDER2 (self))
    {
      Call *call = call_new (self, CALL_LAUNCH_SEARCH);

      call->terms = terms != NULL ? g_strdupv ((char **) terms) : g_new0 (char *, 1);
      call->timestamp = get_current_time ();

      queue_call (self, call);
    }
  else
    {
      g_autoptr (GAppLaunchContext) context = NULL;
      g_autoptr (GError) error = NULL;

      /* the provider is not compatible with the new version of the interface, launch
       * the app itself but warn so we can catch the error in logs */
      g_message ("Search provider %s does not implement LaunchSearch",
                 g_app_info_get_id (priv->app_info));

      context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
      if (!g_app_info_launch (priv->app_info, NULL, context, &error))
        g_warning ("Could not launch %s: %s", g_app_info_get_id (priv->app_info), error->message);
    }
}

static char **
remote_filter_results (ShellSearchProvider *provider,
                       const char * const  *results,
                       int                  max_number)
{
  guint n_results = g_strv_length ((char **) results);
  g_autoptr (GPtrArray) filtered = NULL;
  guint n_regular = 0, n_special = 0;

  if (n_results <= (guint) MAX (max_number, 0))
    return g_strdupv ((char **) results);

  filtered = g_ptr_array_new_with_free_func (g_free);

  /* The first max_number regular results, then the first max_number results
   * that start with "special:" */
  for (guint i = 0; results[i] != NULL; i++)
    {
      if (!g_str_has_prefix (results[i], "special:") && n_regular < (guint) max_number)
        {
          g_ptr_array_add (filtered, g_strdup (results[i]));
          n_regular++;
        }
    }

  for (guint i = 0; results[i] != NULL; i++)
    {
      if (g_str_has_prefix (results[i], "special:") && n_special < (guint) max_number)
        {
          g_ptr_array_add (filtered, g_strdup (results[i]));
          n_special++;
        }
    }

  g_ptr_array_add (filtered, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&filtered), FALSE);
}

static void
shell_remote_search_provider_iface_init (ShellSearchProviderInterface *iface)
{
  iface->get_initial_result_set = remote_get_initial_result_set;
  iface->get_subsearch_result_set = remote_get_subsearch_result_set;
  iface->get_result_metas = remote_get_result_metas;
  iface->activate_result = remote_activate_result;
  iface->launch_search = remote_launch_search;
  iface->filter_results = remote_filter_results;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void
shell_remote_search_provider_set_property (GObject      *object,
                                           guint         prop_id,
                                           const GValue *value,
                                           GParamSpec   *pspec)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (object);
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_APP_INFO:
      g_set_object (&priv->app_info, g_value_get_object (value));
      break;

    case PROP_BUS_NAME:
      g_free (priv->bus_name);
      priv->bus_name = g_value_dup_string (value);
      break;

    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_AUTO_START:
      priv->auto_start = g_value_get_boolean (value);
      break;

    case PROP_DEFAULT_ENABLED:
      shell_remote_search_provider_set_default_enabled (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_remote_search_provider_get_property (GObject    *object,
                                           guint       prop_id,
                                           GValue     *value,
                                           GParamSpec *pspec)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (object);
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_APP_INFO:
      g_value_set_object (value, priv->app_info);
      break;

    case PROP_BUS_NAME:
      g_value_set_string (value, priv->bus_name);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_AUTO_START:
      g_value_set_boolean (value, priv->auto_start);
      break;

    case PROP_DEFAULT_ENABLED:
      g_value_set_boolean (value, priv->default_enabled);
      break;

    case PROP_ID:
      g_value_set_string (value, priv->app_info != NULL ? g_app_info_get_id (priv->app_info) : NULL);
      break;

    case PROP_CAN_LAUNCH_SEARCH:
      g_value_set_boolean (value, SHELL_IS_REMOTE_SEARCH_PROVIDER2 (self));
      break;

    case PROP_IS_REMOTE_PROVIDER:
      g_value_set_boolean (value, TRUE);
      break;

    case PROP_MAX_RESULTS:
      g_value_set_int (value, -1);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_shutdown (ShellGlobal              *global,
             ShellRemoteSearchProvider *self)
{
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  /* Nothing may reach the callers once the shell is going away */
  g_cancellable_cancel (priv->proxy_cancellable);
}

static void
shell_remote_search_provider_constructed (GObject *object)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (object);
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);
  GDBusProxyFlags flags = G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES;
  ProxyRequest *request;

  G_OBJECT_CLASS (shell_remote_search_provider_parent_class)->constructed (object);

  g_return_if_fail (G_IS_APP_INFO (priv->app_info));
  g_return_if_fail (priv->bus_name != NULL && priv->object_path != NULL);

  if (priv->auto_start)
    flags |= G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION;
  else
    flags |= G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START;

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  request = g_new0 (ProxyRequest, 1);
  g_weak_ref_init (&request->provider, self);
  request->version2 = SHELL_IS_REMOTE_SEARCH_PROVIDER2 (self);

  if (SHELL_IS_REMOTE_SEARCH_PROVIDER2 (self))
    shell_dbus_search_provider2_proxy_new_for_bus (G_BUS_TYPE_SESSION, flags,
                                                   priv->bus_name, priv->object_path,
                                                   priv->proxy_cancellable,
                                                   proxy_ready_cb, request);
  else
    shell_dbus_search_provider_proxy_new_for_bus (G_BUS_TYPE_SESSION, flags,
                                                  priv->bus_name, priv->object_path,
                                                  priv->proxy_cancellable,
                                                  proxy_ready_cb, request);
}

static void
shell_remote_search_provider_dispose (GObject *object)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (object);
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);
  Call *call;

  g_cancellable_cancel (priv->proxy_cancellable);

  while ((call = g_queue_pop_head (&priv->waiting_calls)) != NULL)
    call_free (call);

  g_clear_object (&priv->proxy);

  G_OBJECT_CLASS (shell_remote_search_provider_parent_class)->dispose (object);
}

static void
shell_remote_search_provider_finalize (GObject *object)
{
  ShellRemoteSearchProvider *self = SHELL_REMOTE_SEARCH_PROVIDER (object);
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  g_clear_object (&priv->app_info);
  g_clear_object (&priv->proxy_cancellable);
  g_clear_error (&priv->proxy_error);
  g_clear_pointer (&priv->bus_name, g_free);
  g_clear_pointer (&priv->object_path, g_free);

  G_OBJECT_CLASS (shell_remote_search_provider_parent_class)->finalize (object);
}

static void
shell_remote_search_provider_class_init (ShellRemoteSearchProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_remote_search_provider_set_property;
  object_class->get_property = shell_remote_search_provider_get_property;
  object_class->constructed = shell_remote_search_provider_constructed;
  object_class->dispose = shell_remote_search_provider_dispose;
  object_class->finalize = shell_remote_search_provider_finalize;

  /**
   * ShellRemoteSearchProvider:app-info:
   *
   * The application the provider searches in (`appInfo`). It is what the
   * #ShellSearchProvider:app-info property of the interface reads.
   */
  props[PROP_APP_INFO] =
    g_param_spec_object ("app-info", NULL, NULL,
                         G_TYPE_APP_INFO,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellRemoteSearchProvider:bus-name:
   *
   * The name on the session bus of the application.
   */
  props[PROP_BUS_NAME] =
    g_param_spec_string ("bus-name", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellRemoteSearchProvider:object-path:
   *
   * The path of the search provider object of the application.
   */
  props[PROP_OBJECT_PATH] =
    g_param_spec_string ("object-path", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellRemoteSearchProvider:auto-start:
   *
   * Whether the application is started when the provider is first used.
   */
  props[PROP_AUTO_START] =
    g_param_spec_boolean ("auto-start", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellRemoteSearchProvider:default-enabled:
   *
   * Whether the provider is used unless the user disabled it, rather than
   * only if the user enabled it (`defaultEnabled`).
   */
  props[PROP_DEFAULT_ENABLED] =
    g_param_spec_boolean ("default-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_ID, props);

  g_object_class_override_property (object_class, PROP_ID, "id");
  g_object_class_override_property (object_class, PROP_CAN_LAUNCH_SEARCH, "can-launch-search");
  g_object_class_override_property (object_class, PROP_IS_REMOTE_PROVIDER, "is-remote-provider");
  g_object_class_override_property (object_class, PROP_MAX_RESULTS, "max-results");
}

static void
shell_remote_search_provider_init (ShellRemoteSearchProvider *self)
{
  ShellRemoteSearchProviderPrivate *priv = shell_remote_search_provider_get_instance_private (self);

  priv->default_enabled = TRUE;
  priv->auto_start = TRUE;
  priv->proxy_cancellable = g_cancellable_new ();
  g_queue_init (&priv->waiting_calls);
}

static void
shell_remote_search_provider2_class_init (ShellRemoteSearchProvider2Class *klass)
{
}

static void
shell_remote_search_provider2_init (ShellRemoteSearchProvider2 *self)
{
}

/**
 * shell_remote_search_provider_new:
 * @app_info: the application the provider searches in
 * @bus_name: the name of the application on the session bus
 * @object_path: the path of the search provider object
 * @auto_start: whether the application is started when it is first used
 *
 * Creates a provider which talks `org.gnome.Shell.SearchProvider` to an
 * application (`new RemoteSearchProvider ()`).
 *
 * Returns: (transfer full): a new #ShellRemoteSearchProvider
 */
ShellRemoteSearchProvider *
shell_remote_search_provider_new (GAppInfo   *app_info,
                                  const char *bus_name,
                                  const char *object_path,
                                  gboolean    auto_start)
{
  return g_object_new (SHELL_TYPE_REMOTE_SEARCH_PROVIDER,
                       "app-info", app_info,
                       "bus-name", bus_name,
                       "object-path", object_path,
                       "auto-start", auto_start,
                       NULL);
}

/**
 * shell_remote_search_provider2_new:
 * @app_info: the application the provider searches in
 * @bus_name: the name of the application on the session bus
 * @object_path: the path of the search provider object
 * @auto_start: whether the application is started when it is first used
 *
 * Creates a provider which talks `org.gnome.Shell.SearchProvider2` to an
 * application (`new RemoteSearchProvider2 ()`).
 *
 * Returns: (transfer full): a new #ShellRemoteSearchProvider2
 */
ShellRemoteSearchProvider2 *
shell_remote_search_provider2_new (GAppInfo   *app_info,
                                   const char *bus_name,
                                   const char *object_path,
                                   gboolean    auto_start)
{
  return g_object_new (SHELL_TYPE_REMOTE_SEARCH_PROVIDER2,
                       "app-info", app_info,
                       "bus-name", bus_name,
                       "object-path", object_path,
                       "auto-start", auto_start,
                       NULL);
}

/**
 * shell_remote_search_provider_get_default_enabled:
 * @self: a #ShellRemoteSearchProvider
 *
 * Returns: whether the provider is used unless the user disabled it
 */
gboolean
shell_remote_search_provider_get_default_enabled (ShellRemoteSearchProvider *self)
{
  ShellRemoteSearchProviderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_REMOTE_SEARCH_PROVIDER (self), FALSE);

  priv = shell_remote_search_provider_get_instance_private (self);
  return priv->default_enabled;
}

/**
 * shell_remote_search_provider_set_default_enabled:
 * @self: a #ShellRemoteSearchProvider
 * @default_enabled: whether the provider is used unless the user disabled it
 */
void
shell_remote_search_provider_set_default_enabled (ShellRemoteSearchProvider *self,
                                                  gboolean                   default_enabled)
{
  ShellRemoteSearchProviderPrivate *priv;

  g_return_if_fail (SHELL_IS_REMOTE_SEARCH_PROVIDER (self));

  priv = shell_remote_search_provider_get_instance_private (self);
  if (priv->default_enabled == default_enabled)
    return;

  priv->default_enabled = default_enabled;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DEFAULT_ENABLED]);
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                   */
/* ------------------------------------------------------------------------ */

/* What a search provider file describes, before the provider is made */
typedef struct
{
  GAppInfo *app_info;
  char *bus_name;
  char *object_path;
  gboolean auto_start;
  gboolean version2;
  gboolean default_enabled;
} ProviderDescription;

static void
provider_description_free (ProviderDescription *description)
{
  g_clear_object (&description->app_info);
  g_free (description->bus_name);
  g_free (description->object_path);
  g_free (description);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ProviderDescription, provider_description_free)

/* JS compared the version string to 2 as a number */
static gboolean
version_is_at_least_2 (const char *version)
{
  char *end = NULL;
  double value;

  value = g_ascii_strtod (version, &end);
  if (end == version)
    return FALSE;

  while (g_ascii_isspace (*end))
    end++;

  return *end == '\0' && value >= 2;
}

static void
load_provider_description (GFile      *file,
                           GHashTable *object_paths,
                           GList     **descriptions)
{
  g_autofree char *path = g_file_get_path (file);
  g_autoptr (GKeyFile) keyfile = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (ProviderDescription) description = NULL;
  g_autofree char *bus_name = NULL;
  g_autofree char *object_path = NULL;
  g_autofree char *desktop_id = NULL;
  g_autofree char *version = NULL;
  g_autoptr (GDesktopAppInfo) app_info = NULL;
  gboolean auto_start = TRUE;
  gboolean default_disabled = FALSE;

  if (path == NULL || !g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, NULL))
    return;

  if (!g_key_file_has_group (keyfile, KEY_FILE_GROUP))
    return;

  bus_name = g_key_file_get_string (keyfile, KEY_FILE_GROUP, "BusName", &error);
  if (bus_name != NULL)
    object_path = g_key_file_get_string (keyfile, KEY_FILE_GROUP, "ObjectPath", &error);

  if (bus_name == NULL || object_path == NULL)
    {
      g_message ("Failed to add search provider %s: %s", path, error->message);
      return;
    }

  if (g_hash_table_contains (object_paths, object_path))
    return;

  desktop_id = g_key_file_get_string (keyfile, KEY_FILE_GROUP, "DesktopId", NULL);
  if (desktop_id != NULL)
    app_info = g_desktop_app_info_new (desktop_id);

  if (app_info == NULL)
    {
      g_message ("Ignoring search provider %s: missing DesktopId", path);
      return;
    }

  if (!g_app_info_should_show (G_APP_INFO (app_info)))
    return;

  auto_start = g_key_file_get_boolean (keyfile, KEY_FILE_GROUP, "AutoStart", &error);
  if (error != NULL)
    {
      auto_start = TRUE;
      g_clear_error (&error);
    }

  version = g_key_file_get_string (keyfile, KEY_FILE_GROUP, "Version", NULL);

  default_disabled = g_key_file_get_boolean (keyfile, KEY_FILE_GROUP, "DefaultDisabled", &error);
  if (error != NULL)
    {
      default_disabled = FALSE;
      g_clear_error (&error);
    }

  description = g_new0 (ProviderDescription, 1);
  description->app_info = G_APP_INFO (g_steal_pointer (&app_info));
  description->bus_name = g_steal_pointer (&bus_name);
  description->object_path = g_strdup (object_path);
  description->auto_start = auto_start;
  description->version2 = version != NULL && version_is_at_least_2 (version);
  description->default_enabled = !default_disabled;

  g_hash_table_add (object_paths, g_steal_pointer (&object_path));
  *descriptions = g_list_append (*descriptions, g_steal_pointer (&description));
}

static gboolean
strv_contains_string (char **strv,
                      const char *string)
{
  return strv != NULL && string != NULL && g_strv_contains ((const char * const *) strv, string);
}

static int
strv_index_of (char **strv,
               const char *string)
{
  for (int i = 0; strv != NULL && strv[i] != NULL; i++)
    {
      if (g_strcmp0 (strv[i], string) == 0)
        return i;
    }

  return -1;
}

typedef struct
{
  char **sort_order;
} SortData;

static int
compare_descriptions (gconstpointer a,
                      gconstpointer b,
                      gpointer      user_data)
{
  const ProviderDescription *description_a = a;
  const ProviderDescription *description_b = b;
  SortData *sort = user_data;
  int index_a = strv_index_of (sort->sort_order, g_app_info_get_id (description_a->app_info));
  int index_b = strv_index_of (sort->sort_order, g_app_info_get_id (description_b->app_info));

  /* if no provider is found in the order, use alphabetical order */
  if (index_a == -1 && index_b == -1)
    return g_utf8_collate (g_app_info_get_name (description_a->app_info),
                           g_app_info_get_name (description_b->app_info));

  /* if provider A isn't found, it's sorted after provider B */
  if (index_a == -1)
    return 1;

  /* if provider B isn't found, it's sorted after provider A */
  if (index_b == -1)
    return -1;

  /* finally, if both providers are found, return their order in the list */
  return index_a - index_b;
}

/**
 * shell_remote_search_load_providers:
 * @search_settings: the `org.gnome.desktop.search-providers` settings
 *
 * Reads the search provider files of the `gnome-shell/search-providers`
 * directories of the system data directories and makes a provider for each
 * application which provides one (`loadRemoteSearchProviders ()`). The
 * providers the user disabled are left out, and the others sorted by the
 * `sort-order` setting and then by the name of the application.
 *
 * Returns: (transfer full) (element-type ShellSearchProvider): the providers
 */
GList *
shell_remote_search_load_providers (GSettings *search_settings)
{
  g_autoptr (GHashTable) object_paths = NULL;
  g_autolist (ProviderDescription) descriptions = NULL;
  g_auto (GStrv) sort_order = NULL;
  g_auto (GStrv) disabled = NULL;
  g_auto (GStrv) enabled = NULL;
  const char * const *data_dirs;
  GList *providers = NULL;
  SortData sort;

  g_return_val_if_fail (G_IS_SETTINGS (search_settings), NULL);

  if (g_settings_get_boolean (search_settings, "disable-external"))
    return NULL;

  object_paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  data_dirs = g_get_system_data_dirs ();
  for (int i = 0; data_dirs[i] != NULL; i++)
    {
      g_autofree char *path = g_build_filename (data_dirs[i], "gnome-shell", "search-providers", NULL);
      g_autoptr (GFile) dir = g_file_new_for_path (path);
      g_autoptr (GFileEnumerator) enumerator =
        g_file_enumerate_children (dir, "standard::name,standard::type",
                                   G_FILE_QUERY_INFO_NONE, NULL, NULL);
      GFileInfo *info;

      if (enumerator == NULL)
        continue;

      while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
        {
          g_autoptr (GFileInfo) owned_info = info;
          g_autoptr (GFile) file = g_file_enumerator_get_child (enumerator, owned_info);

          load_provider_description (file, object_paths, &descriptions);
        }
    }

  sort_order = g_settings_get_strv (search_settings, "sort-order");
  disabled = g_settings_get_strv (search_settings, "disabled");
  enabled = g_settings_get_strv (search_settings, "enabled");

  for (GList *l = descriptions; l != NULL;)
    {
      ProviderDescription *description = l->data;
      GList *next = l->next;
      const char *app_id = g_app_info_get_id (description->app_info);
      gboolean keep;

      if (description->default_enabled)
        keep = !strv_contains_string (disabled, app_id);
      else
        keep = strv_contains_string (enabled, app_id);

      if (!keep)
        {
          descriptions = g_list_delete_link (descriptions, l);
          provider_description_free (description);
        }

      l = next;
    }

  sort.sort_order = sort_order;
  descriptions = g_list_sort_with_data (descriptions, compare_descriptions, &sort);

  for (GList *l = descriptions; l != NULL; l = l->next)
    {
      ProviderDescription *description = l->data;

      if (description->version2)
        providers = g_list_prepend (providers,
                                    shell_remote_search_provider2_new (description->app_info,
                                                                       description->bus_name,
                                                                       description->object_path,
                                                                       description->auto_start));
      else
        providers = g_list_prepend (providers,
                                    shell_remote_search_provider_new (description->app_info,
                                                                      description->bus_name,
                                                                      description->object_path,
                                                                      description->auto_start));

      shell_remote_search_provider_set_default_enabled (SHELL_REMOTE_SEARCH_PROVIDER (providers->data),
                                                        description->default_enabled);
    }

  return g_list_reverse (providers);
}
