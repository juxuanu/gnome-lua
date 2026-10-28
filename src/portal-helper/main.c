/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * gnome-shell-portal-helper: the captive portal login window.
 *
 * A separate process, started by D-Bus activation of
 * org.gnome.Shell.PortalHelper. The shell (ui/status/network.c) calls
 * Authenticate (), Close () and Refresh () with the object path of the
 * NetworkManager connection that sits behind the portal and listens to
 * StatusChanged () to know when the portal is gone or has to be checked again.
 * It shows a WebKit view per request, one at a time, and exits when it is
 * idle for 30 seconds.
 */

#include "config.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "portal-helper-dbus.h"

typedef enum
{
  PORTAL_HELPER_RESULT_CANCELLED = 0,
  PORTAL_HELPER_RESULT_COMPLETED = 1,
  PORTAL_HELPER_RESULT_RECHECK = 2,
} PortalHelperResult;

typedef enum
{
  PORTAL_HELPER_SECURITY_LEVEL_NOT_YET_DETERMINED = 0,
  PORTAL_HELPER_SECURITY_LEVEL_SECURE = 1,
  PORTAL_HELPER_SECURITY_LEVEL_INSECURE = 2,
} PortalHelperSecurityLevel;

typedef enum
{
  ZOOM_ACTION_NORMAL = 0,
  ZOOM_ACTION_IN = 1,
  ZOOM_ACTION_OUT = 2,
} ZoomAction;

#define HTTP_URI_FLAGS (G_URI_FLAGS_HAS_PASSWORD |      \
                        G_URI_FLAGS_ENCODED_PATH |      \
                        G_URI_FLAGS_ENCODED_QUERY |     \
                        G_URI_FLAGS_ENCODED_FRAGMENT |  \
                        G_URI_FLAGS_SCHEME_NORMALIZE |  \
                        G_URI_FLAGS_PARSE_RELAXED)

#define CONNECTIVITY_CHECK_HOST "nmcheck.gnome.org"
#define CONNECTIVITY_CHECK_URI "http://" CONNECTIVITY_CHECK_HOST
#define CONNECTIVITY_RECHECK_RATELIMIT_TIMEOUT (5 * G_USEC_PER_SEC)

/* Adapted from ephy-zoom.c */
static const double zoom_steps[] = {
  0.30, 0.50, 0.67, 0.80, 0.90, 1.00, 1.10,
  1.20, 1.33, 1.50, 1.70, 2.00, 2.40, 3.00,
};

/* The security indicator of the header bar. GtkMenuButton is a final type,
 * so this is a plain struct that owns the widgets of the popover. */

typedef struct
{
  GtkWidget *button;
  GtkWidget *secure_icon;
  GtkWidget *title_label;
  GtkWidget *description_label;
} PortalSecurityButton;

static void
portal_security_button_init (PortalSecurityButton *self)
{
  GtkWidget *popover;
  GtkWidget *vbox;
  GtkWidget *hbox;

  popover = gtk_popover_new ();
  self->button = g_object_new (GTK_TYPE_MENU_BUTTON,
                               "popover", popover,
                               "visible", FALSE,
                               NULL);

  vbox = g_object_new (GTK_TYPE_BOX,
                       "orientation", GTK_ORIENTATION_VERTICAL,
                       "margin-top", 6,
                       "margin-bottom", 6,
                       "margin-start", 6,
                       "margin-end", 6,
                       "spacing", 6,
                       NULL);
  gtk_popover_set_child (GTK_POPOVER (popover), vbox);

  hbox = g_object_new (GTK_TYPE_BOX,
                       "orientation", GTK_ORIENTATION_HORIZONTAL,
                       "halign", GTK_ALIGN_CENTER,
                       NULL);
  gtk_box_append (GTK_BOX (vbox), hbox);

  self->secure_icon = gtk_image_new ();
  gtk_box_append (GTK_BOX (hbox), self->secure_icon);

  g_object_bind_property (self->secure_icon, "icon-name",
                          self->button, "icon-name",
                          G_BINDING_DEFAULT);

  self->title_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->title_label, "title");
  gtk_box_append (GTK_BOX (hbox), self->title_label);

  self->description_label = g_object_new (GTK_TYPE_LABEL,
                                          "wrap", TRUE,
                                          "max-width-chars", 32,
                                          NULL);
  gtk_box_append (GTK_BOX (vbox), self->description_label);
}

static void
portal_security_button_set_popover_title (PortalSecurityButton *self,
                                          const char           *label)
{
  gtk_label_set_text (GTK_LABEL (self->title_label), label);
}

static void
portal_security_button_set_security_icon (PortalSecurityButton   *self,
                                          PortalHelperSecurityLevel security_level)
{
  switch (security_level)
    {
    case PORTAL_HELPER_SECURITY_LEVEL_NOT_YET_DETERMINED:
      gtk_widget_set_visible (self->button, FALSE);
      break;

    case PORTAL_HELPER_SECURITY_LEVEL_SECURE:
      gtk_widget_set_visible (self->button, TRUE);
      gtk_image_set_from_icon_name (GTK_IMAGE (self->secure_icon),
                                    "channel-secure-symbolic");
      gtk_label_set_label (GTK_LABEL (self->description_label),
                           _("Your connection seems to be secure"));
      break;

    case PORTAL_HELPER_SECURITY_LEVEL_INSECURE:
      gtk_widget_set_visible (self->button, TRUE);
      gtk_image_set_from_icon_name (GTK_IMAGE (self->secure_icon),
                                    "channel-insecure-symbolic");
      gtk_label_set_label (GTK_LABEL (self->description_label),
                           _("Your connection to this hotspot login is not secure. Passwords or other information you enter on this page can be viewed by people nearby."));
      break;

    default:
      break;
    }
}

/* ShellPortalWindow */

#define SHELL_TYPE_PORTAL_WINDOW (shell_portal_window_get_type ())
G_DECLARE_FINAL_TYPE (ShellPortalWindow, shell_portal_window,
                      SHELL, PORTAL_WINDOW, GtkApplicationWindow)

struct _ShellPortalWindow
{
  GtkApplicationWindow parent;

  ShellPortalHelper *helper;
  char *connection;

  PortalSecurityButton secure_menu;
  WebKitNetworkSession *network_session;
  WebKitWebView *web_view;

  GUri *uri;
  char *original_url;
  gboolean original_url_was_gnome;
  gboolean ever_seen_redirect;
  gboolean destroyed;

  int64_t last_recheck;
  unsigned int recheck_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellPortalWindow, shell_portal_window,
                     GTK_TYPE_APPLICATION_WINDOW)

static void
portal_window_status_changed (ShellPortalWindow   *self,
                              PortalHelperResult   result)
{
  shell_portal_helper_emit_status_changed (self->helper,
                                           self->connection,
                                           result);
}

static double
portal_window_get_changed_zoom_level (ShellPortalWindow *self,
                                      ZoomAction         zoom_action)
{
  double zoom_level;
  int i = -1;
  int n_steps = G_N_ELEMENTS (zoom_steps);
  int j;

  if (zoom_action == ZOOM_ACTION_NORMAL)
    return 1.0;

  zoom_level = webkit_web_view_get_zoom_level (self->web_view);

  for (j = 0; j < n_steps; j++)
    {
      if (zoom_steps[j] == zoom_level)
        {
          i = j;
          break;
        }
    }

  if (i < 0)
    {
      /* No exact match found, try to find the nearest value */
      for (j = n_steps - 1; j >= 0; j--)
        {
          if (zoom_steps[j] < zoom_level)
            {
              i = j;
              break;
            }
        }
    }

  /* Still no match? Return default */
  if (i < 0)
    return 1.0;

  if (zoom_action == ZOOM_ACTION_OUT && i > 0)
    return zoom_steps[i - 1];
  else if (zoom_action == ZOOM_ACTION_IN && i < n_steps - 1)
    return zoom_steps[i + 1];

  /* ensure that we have a consistent value */
  return zoom_level;
}

static void
portal_window_set_zoom (ShellPortalWindow *self,
                        ZoomAction         zoom_action)
{
  webkit_web_view_set_zoom_level (self->web_view,
                                  portal_window_get_changed_zoom_level (self, zoom_action));
}

static void
zoom_in_activate_cb (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  portal_window_set_zoom (SHELL_PORTAL_WINDOW (user_data), ZOOM_ACTION_IN);
}

static void
zoom_out_activate_cb (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
  portal_window_set_zoom (SHELL_PORTAL_WINDOW (user_data), ZOOM_ACTION_OUT);
}

static void
zoom_normal_activate_cb (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  portal_window_set_zoom (SHELL_PORTAL_WINDOW (user_data), ZOOM_ACTION_NORMAL);
}

static void
portal_window_sync_uri (ShellPortalWindow *self)
{
  const char *uri = webkit_web_view_get_uri (self->web_view);
  g_autoptr (GError) error = NULL;
  g_autofree char *host = NULL;

  if (uri != NULL &&
      g_uri_split_network (uri, HTTP_URI_FLAGS, NULL, &host, NULL, &error))
    {
      portal_security_button_set_popover_title (&self->secure_menu, host);
      return;
    }

  if (uri != NULL)
    g_warning ("Failed to parse Uri %s: %s", uri, error->message);
  portal_security_button_set_popover_title (&self->secure_menu, "");
}

static void
web_view_uri_changed_cb (WebKitWebView     *web_view,
                         GParamSpec        *pspec,
                         ShellPortalWindow *self)
{
  portal_window_sync_uri (self);
}

static void
web_view_load_changed_cb (WebKitWebView     *web_view,
                          WebKitLoadEvent    load_event,
                          ShellPortalWindow *self)
{
  if (load_event == WEBKIT_LOAD_STARTED)
    {
      portal_security_button_set_security_icon (&self->secure_menu,
                                                PORTAL_HELPER_SECURITY_LEVEL_NOT_YET_DETERMINED);
    }
  else if (load_event == WEBKIT_LOAD_COMMITTED)
    {
      g_autoptr (GTlsCertificate) certificate = NULL;
      GTlsCertificateFlags flags = 0;
      gboolean ret;

      ret = webkit_web_view_get_tls_info (self->web_view, &certificate, &flags);
      if (ret && flags == 0)
        {
          portal_security_button_set_security_icon (&self->secure_menu,
                                                    PORTAL_HELPER_SECURITY_LEVEL_SECURE);
        }
      else
        {
          portal_security_button_set_security_icon (&self->secure_menu,
                                                    PORTAL_HELPER_SECURITY_LEVEL_INSECURE);
        }
    }
}

static void
web_view_insecure_content_detected_cb (WebKitWebView            *web_view,
                                       WebKitInsecureContentEvent event,
                                       ShellPortalWindow         *self)
{
  portal_security_button_set_security_icon (&self->secure_menu,
                                            PORTAL_HELPER_SECURITY_LEVEL_INSECURE);
}

static gboolean
web_view_load_failed_with_tls_errors_cb (WebKitWebView       *web_view,
                                         const char          *failing_uri,
                                         GTlsCertificate     *certificate,
                                         GTlsCertificateFlags errors,
                                         ShellPortalWindow   *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GUri) uri = NULL;

  portal_security_button_set_security_icon (&self->secure_menu,
                                            PORTAL_HELPER_SECURITY_LEVEL_INSECURE);

  uri = g_uri_parse (failing_uri, HTTP_URI_FLAGS, &error);
  if (uri == NULL)
    {
      g_warning ("Failed to parse Uri %s: %s", failing_uri, error->message);
      return FALSE;
    }

  webkit_network_session_allow_tls_certificate_for_host (self->network_session,
                                                         certificate,
                                                         g_uri_get_host (uri));
  webkit_web_view_load_uri (self->web_view, failing_uri);
  return TRUE;
}

static void
recheck_timeout_destroy_cb (gpointer user_data)
{
  g_object_unref (user_data);
}

static gboolean
recheck_timeout_cb (gpointer user_data)
{
  ShellPortalWindow *self = user_data;

  portal_window_status_changed (self, PORTAL_HELPER_RESULT_RECHECK);
  self->recheck_timeout_id = 0;

  return G_SOURCE_REMOVE;
}

static gboolean
web_view_decide_policy_cb (WebKitWebView            *web_view,
                           WebKitPolicyDecision     *decision,
                           WebKitPolicyDecisionType  type,
                           ShellPortalWindow        *self)
{
  WebKitNavigationPolicyDecision *navigation_decision;
  WebKitNavigationAction *navigation_action;
  WebKitURIRequest *request;
  g_autoptr (GError) error = NULL;
  g_autoptr (GUri) uri = NULL;

  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;

  navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION (decision);
  navigation_action =
    webkit_navigation_policy_decision_get_navigation_action (navigation_decision);
  request = webkit_navigation_action_get_request (navigation_action);

  if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    {
      if (webkit_navigation_action_is_user_gesture (navigation_action))
        {
          /* Even though the portal asks for a new window,
           * perform the navigation in the current one. Some
           * portals open a window as their last login step and
           * ignoring that window causes them to not let the
           * user go through. We don't risk popups taking over
           * the page because we check that the navigation is
           * user initiated.
           */
          webkit_web_view_load_request (self->web_view, request);
        }

      webkit_policy_decision_ignore (decision);
      return TRUE;
    }

  uri = g_uri_parse (webkit_uri_request_get_uri (request),
                     HTTP_URI_FLAGS, &error);
  if (uri == NULL)
    {
      g_warning ("Failed to parse Uri %s: %s",
                 webkit_uri_request_get_uri (request), error->message);
      return FALSE;
    }

  if (g_strcmp0 (g_uri_get_host (uri), g_uri_get_host (self->uri)) != 0 &&
      self->original_url_was_gnome)
    {
      if (g_strcmp0 (g_uri_get_host (uri), CONNECTIVITY_CHECK_HOST) == 0 &&
          self->ever_seen_redirect)
        {
          /* Yay, we got to gnome! */
          webkit_policy_decision_ignore (decision);
          portal_window_status_changed (self, PORTAL_HELPER_RESULT_COMPLETED);
          return TRUE;
        }
      else if (g_strcmp0 (g_uri_get_host (uri), CONNECTIVITY_CHECK_HOST) != 0)
        {
          self->ever_seen_redirect = TRUE;
        }
    }

  /* We *may* have finished here, but we don't know for
   * sure. Tell gnome-shell to run another connectivity check
   * (but ratelimit the checks, we don't want to spam
   * nmcheck.gnome.org for portals that have 10 or more internal
   * redirects - and unfortunately they exist)
   */
  if (self->recheck_timeout_id == 0)
    {
      int64_t now = g_get_monotonic_time ();
      int64_t time_since_last_recheck = self->last_recheck ? now - self->last_recheck : 0;

      if (time_since_last_recheck > CONNECTIVITY_RECHECK_RATELIMIT_TIMEOUT ||
          self->last_recheck == 0)
        {
          self->last_recheck = now;
          portal_window_status_changed (self, PORTAL_HELPER_RESULT_RECHECK);
        }
      else
        {
          unsigned int seconds =
            (CONNECTIVITY_RECHECK_RATELIMIT_TIMEOUT - time_since_last_recheck) /
            G_USEC_PER_SEC;

          self->recheck_timeout_id =
            g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, seconds,
                                        recheck_timeout_cb,
                                        g_object_ref (self),
                                        recheck_timeout_destroy_cb);
        }
    }

  /* Update the URI, in case of chained redirects, so we still
   * think we're doing the login until gnome-shell kills us
   */
  g_clear_pointer (&self->uri, g_uri_unref);
  self->uri = g_steal_pointer (&uri);

  webkit_policy_decision_use (decision);
  return TRUE;
}

static gboolean
shell_portal_window_close_request (GtkWindow *window)
{
  ShellPortalWindow *self = SHELL_PORTAL_WINDOW (window);

  if (self->recheck_timeout_id != 0)
    {
      portal_window_status_changed (self, PORTAL_HELPER_RESULT_RECHECK);
      g_clear_handle_id (&self->recheck_timeout_id, g_source_remove);
    }
  portal_window_status_changed (self, PORTAL_HELPER_RESULT_CANCELLED);

  /* The window is destroyed by the caller */
  self->destroyed = TRUE;

  return FALSE;
}

static void
shell_portal_window_finalize (GObject *object)
{
  ShellPortalWindow *self = SHELL_PORTAL_WINDOW (object);

  /* The helper stays until the end: a recheck timeout set before the window
   * was destroyed still reports through it. */
  g_clear_object (&self->network_session);
  g_clear_object (&self->helper);
  g_clear_pointer (&self->uri, g_uri_unref);
  g_clear_pointer (&self->original_url, g_free);
  g_clear_pointer (&self->connection, g_free);

  G_OBJECT_CLASS (shell_portal_window_parent_class)->finalize (object);
}

static void
shell_portal_window_class_init (ShellPortalWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  object_class->finalize = shell_portal_window_finalize;

  window_class->close_request = shell_portal_window_close_request;
}

static void
shell_portal_window_init (ShellPortalWindow *self)
{
  static const GActionEntry entries[] = {
    { "zoom-in", zoom_in_activate_cb },
    { "zoom-out", zoom_out_activate_cb },
    { "zoom-normal", zoom_normal_activate_cb },
  };

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   entries, G_N_ELEMENTS (entries),
                                   self);
}

/* @url has been validated by the caller (NULL or empty for the default) */
static ShellPortalWindow *
shell_portal_window_new (GtkApplication    *application,
                         ShellPortalHelper *helper,
                         const char        *connection,
                         const char        *url,
                         uint32_t           timestamp)
{
  ShellPortalWindow *self;
  g_autoptr (WebKitWebContext) web_context = NULL;
  g_autoptr (GError) error = NULL;
  GtkWidget *headerbar;

  self = g_object_new (SHELL_TYPE_PORTAL_WINDOW,
                       "application", application,
                       "title", _("Hotspot Login"),
                       "default-width", 600,
                       "default-height", 450,
                       NULL);

  self->helper = g_object_ref (helper);
  self->connection = g_strdup (connection);

  headerbar = gtk_header_bar_new ();
  portal_security_button_init (&self->secure_menu);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (headerbar), self->secure_menu.button);

  gtk_window_set_titlebar (GTK_WINDOW (self), headerbar);

  if (url == NULL || *url == '\0')
    {
      url = CONNECTIVITY_CHECK_URI;
      self->original_url_was_gnome = TRUE;
    }
  else
    {
      self->original_url_was_gnome = FALSE;
    }

  self->uri = g_uri_parse (url, HTTP_URI_FLAGS, &error);
  if (self->uri == NULL)
    {
      /* Authenticate () checks the URL, so this is not reached */
      g_warning ("Failed to parse Uri %s: %s", url, error->message);
      self->uri = g_uri_parse (CONNECTIVITY_CHECK_URI, HTTP_URI_FLAGS, NULL);
    }
  self->ever_seen_redirect = FALSE;
  self->original_url = g_strdup (url);
  self->last_recheck = 0;
  self->recheck_timeout_id = 0;

  self->network_session = webkit_network_session_new_ephemeral ();
  webkit_network_session_set_proxy_settings (self->network_session,
                                             WEBKIT_NETWORK_PROXY_MODE_NO_PROXY,
                                             NULL);

  web_context = webkit_web_context_new ();
  webkit_web_context_set_cache_model (web_context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

  self->web_view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                 "network-session", self->network_session,
                                 "web-context", web_context,
                                 NULL);
  g_signal_connect_object (self->web_view, "decide-policy",
                           G_CALLBACK (web_view_decide_policy_cb), self, 0);
  g_signal_connect_object (self->web_view, "load-changed",
                           G_CALLBACK (web_view_load_changed_cb), self, 0);
  g_signal_connect_object (self->web_view, "insecure-content-detected",
                           G_CALLBACK (web_view_insecure_content_detected_cb), self, 0);
  g_signal_connect_object (self->web_view, "load-failed-with-tls-errors",
                           G_CALLBACK (web_view_load_failed_with_tls_errors_cb), self, 0);
  webkit_web_view_load_uri (self->web_view, url);
  g_signal_connect_object (self->web_view, "notify::uri",
                           G_CALLBACK (web_view_uri_changed_cb), self, 0);
  portal_window_sync_uri (self);

  gtk_window_set_child (GTK_WINDOW (self), GTK_WIDGET (self->web_view));
  gtk_window_maximize (GTK_WINDOW (self));
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_window_present_with_time (GTK_WINDOW (self), timestamp);
  G_GNUC_END_IGNORE_DEPRECATIONS

  return self;
}

static void
portal_window_refresh (ShellPortalWindow *self)
{
  self->ever_seen_redirect = FALSE;
  webkit_web_view_load_uri (self->web_view, self->original_url);
}

static void
portal_window_destroy (ShellPortalWindow *self)
{
  if (self->destroyed)
    return;

  self->destroyed = TRUE;
  gtk_window_destroy (GTK_WINDOW (self));
}

/* ShellPortalHelperApp */

#define SHELL_TYPE_PORTAL_HELPER_APP (shell_portal_helper_app_get_type ())
G_DECLARE_FINAL_TYPE (ShellPortalHelperApp, shell_portal_helper_app,
                      SHELL, PORTAL_HELPER_APP, AdwApplication)

typedef struct
{
  char *connection;
  char *url;
  uint32_t timestamp;
  ShellPortalWindow *window;
  gboolean window_created;
} PortalRequest;

struct _ShellPortalHelperApp
{
  AdwApplication parent;

  ShellPortalHelper *dbus_impl;
  GPtrArray *queue;
};

G_DEFINE_FINAL_TYPE (ShellPortalHelperApp, shell_portal_helper_app,
                     ADW_TYPE_APPLICATION)

static void
portal_request_free (PortalRequest *request)
{
  g_clear_weak_pointer (&request->window);
  g_free (request->connection);
  g_free (request->url);
  g_free (request);
}

static void
portal_helper_app_process_queue (ShellPortalHelperApp *self)
{
  PortalRequest *top;

  if (self->queue->len == 0)
    return;

  top = g_ptr_array_index (self->queue, 0);
  if (top->window_created)
    return;

  top->window_created = TRUE;
  g_set_weak_pointer (&top->window,
                      shell_portal_window_new (GTK_APPLICATION (self),
                                               self->dbus_impl,
                                               top->connection,
                                               top->url,
                                               top->timestamp));
}

static gboolean
portal_helper_app_validate_url (const char  *url,
                                GError     **error)
{
  g_autoptr (GUri) uri = NULL;

  if (url == NULL || *url == '\0')
    return TRUE;

  uri = g_uri_parse (url, HTTP_URI_FLAGS, error);
  return uri != NULL;
}

static void
portal_helper_app_authenticate (ShellPortalHelperApp *self,
                                const char           *connection,
                                const char           *url,
                                uint32_t              timestamp)
{
  PortalRequest *request = g_new0 (PortalRequest, 1);

  request->connection = g_strdup (connection);
  request->url = g_strdup (url);
  request->timestamp = timestamp;

  g_ptr_array_add (self->queue, request);

  portal_helper_app_process_queue (self);
}

static gboolean
handle_authenticate_cb (ShellPortalHelper     *helper,
                        GDBusMethodInvocation *invocation,
                        const char            *connection,
                        const char            *url,
                        uint32_t               timestamp,
                        gpointer               user_data)
{
  ShellPortalHelperApp *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!portal_helper_app_validate_url (url, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid URL %s: %s",
                                             url, error->message);
      return TRUE;
    }

  portal_helper_app_authenticate (self, connection, url, timestamp);

  shell_portal_helper_complete_authenticate (helper, invocation);
  return TRUE;
}

static gboolean
handle_close_cb (ShellPortalHelper     *helper,
                 GDBusMethodInvocation *invocation,
                 const char            *connection,
                 gpointer               user_data)
{
  ShellPortalHelperApp *self = user_data;
  unsigned int i;

  for (i = 0; i < self->queue->len; i++)
    {
      PortalRequest *request = g_ptr_array_index (self->queue, i);

      if (g_strcmp0 (request->connection, connection) == 0)
        {
          if (request->window != NULL)
            portal_window_destroy (request->window);
          g_ptr_array_remove_index (self->queue, i);
          break;
        }
    }

  portal_helper_app_process_queue (self);

  shell_portal_helper_complete_close (helper, invocation);
  return TRUE;
}

static gboolean
handle_refresh_cb (ShellPortalHelper     *helper,
                   GDBusMethodInvocation *invocation,
                   const char            *connection,
                   gpointer               user_data)
{
  ShellPortalHelperApp *self = user_data;
  unsigned int i;

  for (i = 0; i < self->queue->len; i++)
    {
      PortalRequest *request = g_ptr_array_index (self->queue, i);

      if (g_strcmp0 (request->connection, connection) == 0)
        {
          if (request->window != NULL)
            portal_window_refresh (request->window);
          break;
        }
    }

  shell_portal_helper_complete_refresh (helper, invocation);
  return TRUE;
}

static void
quit_activate_cb (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  GtkWindow *window =
    gtk_application_get_active_window (GTK_APPLICATION (user_data));

  if (window != NULL)
    {
      if (SHELL_IS_PORTAL_WINDOW (window))
        portal_window_destroy (SHELL_PORTAL_WINDOW (window));
      else
        gtk_window_destroy (window);
    }
}

static gboolean
shell_portal_helper_app_dbus_register (GApplication     *application,
                                       GDBusConnection  *connection,
                                       const char       *object_path,
                                       GError          **error)
{
  ShellPortalHelperApp *self = SHELL_PORTAL_HELPER_APP (application);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->dbus_impl),
                                         connection, object_path, error))
    return FALSE;

  return G_APPLICATION_CLASS (shell_portal_helper_app_parent_class)->dbus_register (application,
                                                                                     connection,
                                                                                     object_path,
                                                                                     error);
}

static void
shell_portal_helper_app_dbus_unregister (GApplication    *application,
                                         GDBusConnection *connection,
                                         const char      *object_path)
{
  ShellPortalHelperApp *self = SHELL_PORTAL_HELPER_APP (application);

  g_dbus_interface_skeleton_unexport_from_connection (G_DBUS_INTERFACE_SKELETON (self->dbus_impl),
                                                      connection);

  G_APPLICATION_CLASS (shell_portal_helper_app_parent_class)->dbus_unregister (application,
                                                                                connection,
                                                                                object_path);
}

static void
shell_portal_helper_app_activate (GApplication *application)
{
  ShellPortalHelperApp *self = SHELL_PORTAL_HELPER_APP (application);

  /* If launched manually (for example for testing), force a dummy authentication
   * session with the default url
   */
  portal_helper_app_authenticate (self, "/org/gnome/dummy", "", 0);
}

static void
shell_portal_helper_app_dispose (GObject *object)
{
  ShellPortalHelperApp *self = SHELL_PORTAL_HELPER_APP (object);

  g_clear_object (&self->dbus_impl);
  g_clear_pointer (&self->queue, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_portal_helper_app_parent_class)->dispose (object);
}

static void
shell_portal_helper_app_class_init (ShellPortalHelperAppClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = shell_portal_helper_app_dispose;

  application_class->dbus_register = shell_portal_helper_app_dbus_register;
  application_class->dbus_unregister = shell_portal_helper_app_dbus_unregister;
  application_class->activate = shell_portal_helper_app_activate;
}

static void
shell_portal_helper_app_init (ShellPortalHelperApp *self)
{
  g_autoptr (GSimpleAction) action = NULL;

  self->queue = g_ptr_array_new_with_free_func ((GDestroyNotify) portal_request_free);

  self->dbus_impl = shell_portal_helper_skeleton_new ();
  g_signal_connect (self->dbus_impl, "handle-authenticate",
                    G_CALLBACK (handle_authenticate_cb), self);
  g_signal_connect (self->dbus_impl, "handle-close",
                    G_CALLBACK (handle_close_cb), self);
  g_signal_connect (self->dbus_impl, "handle-refresh",
                    G_CALLBACK (handle_refresh_cb), self);

  action = g_simple_action_new ("quit", NULL);
  g_signal_connect (action, "activate", G_CALLBACK (quit_activate_cb), self);
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));

  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "win.zoom-in",
                                         (const char *[]) {
                                           "<Primary>plus",
                                           "<Primary>KP_Add",
                                           "<Primary>equal",
                                           "ZoomIn",
                                           NULL,
                                         });
  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "win.zoom-out",
                                         (const char *[]) {
                                           "<Primary>minus",
                                           "<Primary>KP_Subtract",
                                           "ZoomOut",
                                           NULL,
                                         });
  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "win.zoom-normal",
                                         (const char *[]) {
                                           "<Primary>0",
                                           "<Primary>KP_0",
                                           NULL,
                                         });
  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "app.quit",
                                         (const char *[]) {
                                           "<Primary>q",
                                           "<Primary>w",
                                           NULL,
                                         });
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr (ShellPortalHelperApp) app = NULL;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = g_object_new (SHELL_TYPE_PORTAL_HELPER_APP,
                      "application-id", "org.gnome.Shell.PortalHelper",
                      "flags", G_APPLICATION_IS_SERVICE,
                      "inactivity-timeout", 30000,
                      NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
