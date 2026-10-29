/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-network.c: `shell.network`, HTTP and HTTPS requests with
 * libsoup 3 (docs/api/network.md, API 1.1, capability "network").
 *
 * Handle type:
 *
 *   shell.Request   ShellLuaNetRequest (private, below)
 *
 * Every extension has one #SoupSession, made at its first request and kept
 * as qdata of its #ShellLuaState (no cookie jar, no cache, the user agent
 * "gnome-lua/<uuid>"). It goes with the state; the requests in flight are
 * destroyed (and cancelled) by the owned-object registry before.
 *
 * A request is an object that the extension context owns
 * (shell_lua_own_object ()): destroyed in reverse creation order at disable,
 * by `request:destroy ()`, and by the request itself when it has completed
 * (so a finished request stops counting against the cap of owned objects and
 * its handle turns dead, which makes a late `cancel ()` harmless). The two
 * callbacks are callbacks of that owned object: they are killed with it, so
 * nothing runs after disable or `destroy ()`.
 *
 * The flow is soup_session_send_async () for the headers and then
 * g_input_stream_read_bytes_async () in chunks, so that `max_bytes` bounds
 * the memory a response can take. The time limit is a timeout source that
 * cancels the GCancellable; the reason it was cancelled for (timeout, size,
 * redirects) decides the error code that the cancelled operation reports.
 * Every completion handler first checks `finished`: a destroyed request may
 * still get a (cancelled) completion, and g_cancellable_cancel () may call a
 * completion synchronously.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <libsoup/soup.h>

#include "shell-lua-api-private.h"

#define REQUEST_TYPE "shell.Request"
#define SESSION_KEY "shell-lua-network-session"

#define DEFAULT_TIMEOUT_MS 30000
#define MAX_TIMEOUT_MS 3600000
#define DEFAULT_MAX_BYTES (8 * 1024 * 1024)
#define MAX_MAX_BYTES (64 * 1024 * 1024)
#define MAX_URL_LENGTH 8192
#define MAX_HEADERS 64
#define MAX_REDIRECTS 10
#define READ_CHUNK_SIZE 65536

typedef enum {
  STOP_NONE,
  STOP_TIMEOUT,
  STOP_TOO_LARGE,
  STOP_REDIRECTS
} StopReason;

/* ------------------------------------------------------------------------ */
/* ShellLuaNetRequest                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_NET_REQUEST (shell_lua_net_request_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaNetRequest, shell_lua_net_request,
                      SHELL, LUA_NET_REQUEST, GObject)

struct _ShellLuaNetRequest
{
  GObject parent;

  ShellLuaState *state;        /* unowned; NULL once the request is over */
  SoupSession *session;
  SoupMessage *message;
  GCancellable *cancellable;
  GInputStream *stream;        /* while the body is read */
  GByteArray *body;
  gsize max_bytes;

  guint timeout_id;
  gulong restarted_id;
  guint redirects;
  StopReason stop;
  gboolean cancelling;         /* cancel () was called, the idle is queued */
  gboolean finished;           /* a result was delivered, or it was destroyed */

  ShellLuaCallback *on_done;   /* strong, may be NULL */
  ShellLuaCallback *on_error;
};

G_DEFINE_FINAL_TYPE (ShellLuaNetRequest, shell_lua_net_request, G_TYPE_OBJECT)

static void
request_close_stream_done (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  g_input_stream_close_finish (G_INPUT_STREAM (source), result, &error);
}

static void
request_drop_stream (ShellLuaNetRequest *self)
{
  g_autoptr (GInputStream) stream = g_steal_pointer (&self->stream);

  if (stream != NULL)
    g_input_stream_close_async (stream, G_PRIORITY_DEFAULT, NULL,
                                request_close_stream_done, NULL);
}

/* Stops everything the request has in the main loop. Idempotent. */
static void
request_quiesce (ShellLuaNetRequest *self)
{
  g_clear_handle_id (&self->timeout_id, g_source_remove);
  if (self->restarted_id != 0)
    {
      if (self->message != NULL)
        g_signal_handler_disconnect (self->message, self->restarted_id);
      self->restarted_id = 0;
    }
}

/* ShellLuaOwnOps.destroy: `request:destroy ()` and disable. No callback runs
 * (the registry killed them before calling this). */
static void
request_destroy (GObject *object)
{
  ShellLuaNetRequest *self = SHELL_LUA_NET_REQUEST (object);

  g_object_ref (self);
  self->finished = TRUE;
  self->state = NULL;
  request_quiesce (self);
  g_cancellable_cancel (self->cancellable);
  request_drop_stream (self);
  g_object_unref (self);
}

static const ShellLuaOwnOps request_ops = {
  .destroy = request_destroy,
};

static void
shell_lua_net_request_dispose (GObject *object)
{
  ShellLuaNetRequest *self = SHELL_LUA_NET_REQUEST (object);

  self->finished = TRUE;
  self->state = NULL;
  request_quiesce (self);
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  request_drop_stream (self);
  g_clear_pointer (&self->on_done, shell_lua_callback_unref);
  g_clear_pointer (&self->on_error, shell_lua_callback_unref);
  g_clear_object (&self->message);
  g_clear_object (&self->session);

  G_OBJECT_CLASS (shell_lua_net_request_parent_class)->dispose (object);
}

static void
shell_lua_net_request_finalize (GObject *object)
{
  ShellLuaNetRequest *self = SHELL_LUA_NET_REQUEST (object);

  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->body, g_byte_array_unref);

  G_OBJECT_CLASS (shell_lua_net_request_parent_class)->finalize (object);
}

static void
shell_lua_net_request_class_init (ShellLuaNetRequestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_net_request_dispose;
  object_class->finalize = shell_lua_net_request_finalize;
}

static void
shell_lua_net_request_init (ShellLuaNetRequest *self)
{
  self->cancellable = g_cancellable_new ();
  self->body = g_byte_array_new ();
}

/* ------------------------------------------------------------------------ */
/* The session of an extension                                               */
/* ------------------------------------------------------------------------ */

static void
session_free (gpointer data)
{
  soup_session_abort (SOUP_SESSION (data));
  g_object_unref (data);
}

static SoupSession *
get_session (lua_State     *L,
             ShellLuaState *state)
{
  SoupSession *session = g_object_get_data (G_OBJECT (state), SESSION_KEY);
  ShellLuaExtension *extension;
  const char *id;
  g_autofree char *user_agent = NULL;

  if (session != NULL)
    return session;

  extension = shell_lua_extension_from_L (L);
  id = extension != NULL ? shell_lua_extension_get_uuid (extension) : shell_lua_state_get_id (state);
  user_agent = g_strdup_printf ("gnome-lua/%s", id);

  /* No cookie jar and no cache: SoupSession has neither unless a feature
   * is added. The user agent has no trailing space, so libsoup adds nothing
   * to it. */
  session = soup_session_new_with_options ("user-agent", user_agent, NULL);
  /* Whatever is in flight is a request of the context, which destroys (and
   * so cancels) them at disable; the session goes with the state */
  g_object_set_data_full (G_OBJECT (state), SESSION_KEY, session, session_free);
  return session;
}

/* ------------------------------------------------------------------------ */
/* Results                                                                   */
/* ------------------------------------------------------------------------ */

G_GNUC_PRINTF (3, 4)
static void
ext_log (ShellLuaState  *state,
         GLogLevelFlags  level,
         const char     *format,
         ...)
{
  g_autofree char *domain = NULL;
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  domain = g_strdup_printf ("shell-lua/%s", state != NULL ? shell_lua_state_get_id (state) : "network");
  g_log (domain, level, "%s", message);
}

typedef struct
{
  lua_State *L;
  int        table;
} HeaderContext;

static void
push_header_cb (const char *name,
                const char *value,
                gpointer    user_data)
{
  HeaderContext *ctx = user_data;
  lua_State *L = ctx->L;
  g_autofree char *lower = g_ascii_strdown (name, -1);

  if (lua_getfield (L, ctx->table, lower) == LUA_TSTRING)
    lua_pushfstring (L, "%s, %s", lua_tostring (L, -1), value);
  else
    lua_pushstring (L, value);
  lua_setfield (L, ctx->table, lower);
  lua_pop (L, 1);   /* the previous value, or nil */
}

static void
push_response (lua_State *L,
               gpointer   data)
{
  ShellLuaNetRequest *self = data;
  SoupMessage *message = self->message;
  const char *reason = soup_message_get_reason_phrase (message);
  GUri *uri = soup_message_get_uri (message);
  HeaderContext ctx;

  luaL_checkstack (L, 6, "response");
  lua_createtable (L, 0, 5);

  lua_pushinteger (L, (lua_Integer) soup_message_get_status (message));
  lua_setfield (L, -2, "status");

  lua_pushstring (L, reason != NULL ? reason : "");
  lua_setfield (L, -2, "reason");

  lua_newtable (L);
  ctx.L = L;
  ctx.table = lua_gettop (L);
  soup_message_headers_foreach (soup_message_get_response_headers (message), push_header_cb, &ctx);
  lua_setfield (L, -2, "headers");

  if (self->body->len > 0)
    lua_pushlstring (L, (const char *) self->body->data, self->body->len);
  else
    lua_pushliteral (L, "");
  lua_setfield (L, -2, "body");

  if (uri != NULL)
    {
      g_autofree char *url = g_uri_to_string_partial (uri, G_URI_HIDE_PASSWORD);

      lua_pushstring (L, url);
      lua_setfield (L, -2, "url");
    }
}

typedef struct
{
  const char *code;
  const char *message;
} ErrorInfo;

static void
push_error (lua_State *L,
            gpointer   data)
{
  const ErrorInfo *info = data;

  luaL_checkstack (L, 4, "error");
  lua_createtable (L, 0, 2);
  lua_pushstring (L, info->message);
  lua_setfield (L, -2, "message");
  lua_pushstring (L, info->code);
  lua_setfield (L, -2, "code");
}

/* After a callback: the request is over, give it back to its context. The
 * callback may have destroyed it itself (then the destruction was deferred
 * until the callback returned and `state` is NULL by now). */
static void
request_release (ShellLuaNetRequest *self)
{
  ShellLuaState *state = self->state;

  if (state != NULL && shell_lua_state_is_alive (state))
    shell_lua_destroy_object (shell_lua_state_get_L (state), G_OBJECT (self));
}

static void
request_succeed (ShellLuaNetRequest *self)
{
  ShellLuaState *state = self->state;

  if (self->finished)
    return;

  g_object_ref (self);
  self->finished = TRUE;
  request_quiesce (self);
  request_drop_stream (self);

  if (state != NULL && shell_lua_state_is_alive (state))
    {
      if (self->on_done != NULL)
        shell_lua_callback_emit_data (self->on_done, 1, push_response, self);
      request_release (self);
    }
  g_object_unref (self);
}

static void
request_fail (ShellLuaNetRequest *self,
              const char         *code,
              const char         *message)
{
  ShellLuaState *state = self->state;
  ErrorInfo info = { code, message };

  if (self->finished)
    return;

  g_object_ref (self);
  self->finished = TRUE;
  request_quiesce (self);
  request_drop_stream (self);
  g_cancellable_cancel (self->cancellable);

  if (state != NULL && shell_lua_state_is_alive (state))
    {
      if (self->on_error != NULL)
        shell_lua_callback_emit_data (self->on_error, 1, push_error, &info);
      else
        ext_log (state, G_LOG_LEVEL_MESSAGE, "network request failed (%s): %s", code, message);
      request_release (self);
    }
  g_object_unref (self);
}

/* The error code (docs/api/network.md "Errors") of a GError */
static const char *
classify_error (const GError *error,
                StopReason    stop)
{
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      switch (stop)
        {
        case STOP_TIMEOUT:
          return "timeout";
        case STOP_TOO_LARGE:
          return "too-large";
        case STOP_REDIRECTS:
          return "other";
        case STOP_NONE:
        default:
          return "cancelled";
        }
    }

  if (error->domain == G_TLS_ERROR)
    return "tls";
  if (error->domain == G_RESOLVER_ERROR)
    return "resolve";

  if (error->domain == G_IO_ERROR)
    {
      switch ((GIOErrorEnum) error->code)
        {
        case G_IO_ERROR_TIMED_OUT:
          return "timeout";
        case G_IO_ERROR_HOST_NOT_FOUND:
          return "resolve";
        case G_IO_ERROR_CONNECTION_REFUSED:
        case G_IO_ERROR_CONNECTION_CLOSED:
        case G_IO_ERROR_HOST_UNREACHABLE:
        case G_IO_ERROR_NETWORK_UNREACHABLE:
        case G_IO_ERROR_NOT_CONNECTED:
        case G_IO_ERROR_PARTIAL_INPUT:
        case G_IO_ERROR_PROXY_FAILED:
        case G_IO_ERROR_PROXY_AUTH_FAILED:
        case G_IO_ERROR_PROXY_NEED_AUTH:
        case G_IO_ERROR_PROXY_NOT_ALLOWED:
          return "connection";
        default:
          break;
        }
    }

  return "other";
}

static void
request_fail_gerror (ShellLuaNetRequest *self,
                     const GError       *error)
{
  const char *code = classify_error (error, self->stop);

  if (g_str_equal (code, "other") && self->stop == STOP_REDIRECTS)
    request_fail (self, code, "too many redirects");
  else if (g_str_equal (code, "too-large"))
    request_fail (self, code, "the response is bigger than max_bytes");
  else if (g_str_equal (code, "timeout") && self->stop == STOP_TIMEOUT)
    request_fail (self, code, "the request timed out (timeout_ms)");
  else if (g_str_equal (code, "cancelled"))
    request_fail (self, code, "the request was cancelled");
  else
    request_fail (self, code, error->message);
}

/* ------------------------------------------------------------------------ */
/* The transfer                                                              */
/* ------------------------------------------------------------------------ */

static void request_read (ShellLuaNetRequest *self);

static gboolean
request_timeout_cb (gpointer user_data)
{
  ShellLuaNetRequest *self = user_data;

  self->timeout_id = 0;
  if (!self->finished)
    {
      g_object_ref (self);
      self->stop = STOP_TIMEOUT;
      g_cancellable_cancel (self->cancellable);
      g_object_unref (self);
    }
  return G_SOURCE_REMOVE;
}

/* Cancels from the main loop, never from inside a call of Lua or of libsoup
 * (g_cancellable_cancel () can run the completion at once). */
static gboolean
request_cancel_idle (gpointer user_data)
{
  ShellLuaNetRequest *self = user_data;

  if (!self->finished)
    g_cancellable_cancel (self->cancellable);
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

static void
request_schedule_cancel (ShellLuaNetRequest *self)
{
  if (self->finished || self->cancelling)
    return;

  self->cancelling = TRUE;
  /* before the I/O of libsoup (a redirect loop is quick) */
  g_idle_add_full (G_PRIORITY_HIGH, request_cancel_idle, g_object_ref (self), NULL);
}

static void
message_restarted_cb (SoupMessage *message,
                      gpointer     user_data)
{
  ShellLuaNetRequest *self = user_data;

  if (++self->redirects > MAX_REDIRECTS && self->stop == STOP_NONE)
    {
      self->stop = STOP_REDIRECTS;
      request_schedule_cancel (self);
    }
}

static void
read_cb (GObject      *source,
         GAsyncResult *result,
         gpointer      user_data)
{
  ShellLuaNetRequest *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (!self->finished)
    {
      if (bytes == NULL)
        {
          request_fail_gerror (self, error);
        }
      else if (g_bytes_get_size (bytes) == 0)
        {
          request_succeed (self);
        }
      else if (g_bytes_get_size (bytes) > self->max_bytes - self->body->len)
        {
          self->stop = STOP_TOO_LARGE;
          request_fail (self, "too-large", "the response is bigger than max_bytes");
        }
      else
        {
          gsize size;
          gconstpointer data = g_bytes_get_data (bytes, &size);

          g_byte_array_append (self->body, data, (guint) size);
          request_read (self);
        }
    }

  g_object_unref (self);
}

static void
request_read (ShellLuaNetRequest *self)
{
  g_input_stream_read_bytes_async (self->stream, READ_CHUNK_SIZE, G_PRIORITY_DEFAULT,
                                   self->cancellable, read_cb, g_object_ref (self));
}

static void
send_cb (GObject      *source,
         GAsyncResult *result,
         gpointer      user_data)
{
  ShellLuaNetRequest *self = user_data;
  g_autoptr (GError) error = NULL;
  GInputStream *stream = soup_session_send_finish (SOUP_SESSION (source), result, &error);

  if (stream == NULL)
    {
      if (!self->finished)
        request_fail_gerror (self, error);
    }
  else if (self->finished)
    {
      g_input_stream_close_async (stream, G_PRIORITY_DEFAULT, NULL, request_close_stream_done, NULL);
      g_object_unref (stream);
    }
  else
    {
      SoupMessageHeaders *headers = soup_message_get_response_headers (self->message);

      self->stream = stream;

      /* A body that says it is too big is refused before it is read. With a
       * Content-Encoding the length is the encoded one: the bytes are counted
       * as they arrive. */
      if (strcmp (soup_message_get_method (self->message), "HEAD") != 0 &&
          soup_message_headers_get_encoding (headers) == SOUP_ENCODING_CONTENT_LENGTH &&
          soup_message_headers_get_one (headers, "Content-Encoding") == NULL &&
          soup_message_headers_get_content_length (headers) > (goffset) self->max_bytes)
        {
          self->stop = STOP_TOO_LARGE;
          request_fail (self, "too-large", "the response is bigger than max_bytes");
        }
      else
        {
          request_read (self);
        }
    }

  g_object_unref (self);
}

/* ------------------------------------------------------------------------ */
/* Arguments                                                                 */
/* ------------------------------------------------------------------------ */

static const char * const request_fields[] = {
  "url", "method", "headers", "body", "content_type", "timeout_ms", "max_bytes",
  "on_done", "on_error", NULL
};

static const char * const methods[] = {
  "GET", "POST", "PUT", "DELETE", "HEAD", "PATCH", NULL
};

/* Header names the HTTP layer owns (docs/api/network.md "Request headers") */
static const char * const forbidden_headers[] = {
  "host", "content-length", "transfer-encoding", "connection", "keep-alive",
  "proxy-connection", "proxy-authorization", "upgrade", "te", "trailer",
  "expect", "accept-encoding", NULL
};

static gboolean
is_token_char (guchar c)
{
  return g_ascii_isalnum (c) || (c != '\0' && strchr ("!#$%&'*+-.^_`|~", c) != NULL);
}

static gboolean
is_token (const char *s,
          size_t      len)
{
  size_t i;

  if (len == 0)
    return FALSE;
  for (i = 0; i < len; i++)
    if (!is_token_char ((guchar) s[i]))
      return FALSE;
  return TRUE;
}

static gboolean
is_header_value (const char *s,
                 size_t      len)
{
  size_t i;

  for (i = 0; i < len; i++)
    {
      guchar c = (guchar) s[i];

      if ((c < 0x20 && c != '\t') || c == 0x7f)
        return FALSE;
    }
  return TRUE;
}

static void
check_callable_field (lua_State  *L,
                      const char *field)
{
  if (lua_getfield (L, 1, field) != LUA_TNIL && lua_type (L, -1) != LUA_TFUNCTION)
    {
      if (luaL_getmetafield (L, -1, "__call") == LUA_TNIL)
        luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
}

/* An integer field in [min, max], or @def when absent */
static lua_Integer
range_field (lua_State  *L,
             const char *field,
             lua_Integer def,
             lua_Integer min,
             lua_Integer max)
{
  lua_Integer value = shell_lua_opt_integer (L, 1, field, def);

  if (value < min || value > max)
    luaL_error (L, "field '%s' must be between %I and %I", field, min, max);
  return value;
}

/* Validates the table of headers at the top of the stack (raises) */
static void
check_headers (lua_State *L,
               int        headers,
               gboolean  *has_content_type)
{
  const char *seen[MAX_HEADERS];
  int n = 0;

  *has_content_type = FALSE;
  luaL_checkstack (L, 4, "headers");

  lua_pushnil (L);
  while (lua_next (L, headers) != 0)
    {
      const char *name, *value;
      size_t name_len, value_len;
      int i;

      if (lua_type (L, -2) != LUA_TSTRING)
        luaL_error (L, "field 'headers' must map header names to strings, got a key of type %s",
                    luaL_typename (L, -2));
      if (lua_type (L, -1) != LUA_TSTRING)
        luaL_error (L, "header '%s' must be a string, got %s", lua_tostring (L, -2), luaL_typename (L, -1));

      name = lua_tolstring (L, -2, &name_len);
      value = lua_tolstring (L, -1, &value_len);

      if (n >= MAX_HEADERS)
        luaL_error (L, "field 'headers' has more than %d entries", MAX_HEADERS);
      if (!is_token (name, name_len) || strlen (name) != name_len)
        luaL_error (L, "'%s' is not a valid header name", name);
      if (!is_header_value (value, value_len))
        luaL_error (L, "the value of header '%s' contains a control character", name);

      for (i = 0; forbidden_headers[i] != NULL; i++)
        if (g_ascii_strcasecmp (name, forbidden_headers[i]) == 0)
          luaL_error (L, "header '%s' cannot be set", name);
      for (i = 0; i < n; i++)
        if (g_ascii_strcasecmp (name, seen[i]) == 0)
          luaL_error (L, "header '%s' is given twice", name);

      if (g_ascii_strcasecmp (name, "Content-Type") == 0)
        *has_content_type = TRUE;

      seen[n++] = name;
      lua_pop (L, 1);
    }
}

static int
l_request (lua_State *L)
{
  ShellLuaState *state;
  ShellLuaNetRequest *self;
  SoupSession *session;
  SoupMessage *message;
  SoupMessageHeaders *request_headers;
  const char *url, *method, *content_type, *scheme;
  const char *body = NULL;
  size_t body_len = 0;
  lua_Integer timeout_ms, max_bytes;
  const char *content_type_header = NULL;
  gboolean has_headers = FALSE, has_content_type = FALSE;
  GUri *uri;
  int headers_idx = 0, done_ref, error_ref, i;

  shell_lua_require_capability (L, "network", "shell.network.request()");
  luaL_checktype (L, 1, LUA_TTABLE);
  shell_lua_check_fields (L, 1, request_fields);
  shell_lua_check_owned_capacity (L);

  url = shell_lua_opt_string (L, 1, "url", NULL);
  if (url == NULL)
    return luaL_error (L, "missing required field 'url' (a string)");
  if (strlen (url) > MAX_URL_LENGTH)
    return luaL_error (L, "field 'url' is longer than %d bytes", MAX_URL_LENGTH);

  method = shell_lua_opt_string (L, 1, "method", "GET");
  for (i = 0; methods[i] != NULL; i++)
    if (strcmp (method, methods[i]) == 0)
      break;
  if (methods[i] == NULL)
    return luaL_error (L, "field 'method' must be one of GET, POST, PUT, DELETE, HEAD, PATCH, got '%s'", method);

  timeout_ms = range_field (L, "timeout_ms", DEFAULT_TIMEOUT_MS, 1, MAX_TIMEOUT_MS);
  max_bytes = range_field (L, "max_bytes", DEFAULT_MAX_BYTES, 0, MAX_MAX_BYTES);

  if (lua_getfield (L, 1, "headers") != LUA_TNIL)
    {
      if (lua_type (L, -1) != LUA_TTABLE)
        return luaL_error (L, "field 'headers' must be a table, got %s", luaL_typename (L, -1));
      headers_idx = lua_gettop (L);
      has_headers = TRUE;
      check_headers (L, headers_idx, &has_content_type);
    }
  else
    {
      lua_pop (L, 1);
    }

  if (lua_getfield (L, 1, "body") != LUA_TNIL)
    {
      if (lua_type (L, -1) != LUA_TSTRING)
        return luaL_error (L, "field 'body' must be a string, got %s", luaL_typename (L, -1));
      body = lua_tolstring (L, -1, &body_len);
      if (strcmp (method, "GET") == 0 || strcmp (method, "HEAD") == 0)
        return luaL_error (L, "a %s request cannot have a 'body'", method);
    }
  else
    {
      lua_pop (L, 1);
    }

  content_type = shell_lua_opt_string (L, 1, "content_type", NULL);
  if (content_type != NULL)
    {
      if (body == NULL)
        return luaL_error (L, "field 'content_type' needs a 'body'");
      if (has_content_type)
        return luaL_error (L, "field 'content_type' and a 'Content-Type' header are both given");
      if (!is_header_value (content_type, strlen (content_type)) || content_type[0] == '\0')
        return luaL_error (L, "field 'content_type' is not a valid media type");
    }
  else if (body != NULL && !has_content_type)
    {
      content_type = "application/octet-stream";
    }

  check_callable_field (L, "on_done");
  check_callable_field (L, "on_error");

  /* The last thing that can fail: the URL. Nothing is allocated for the
   * request before it has passed. */
  scheme = g_uri_peek_scheme (url);
  message = NULL;
  if (scheme != NULL && (strcmp (scheme, "http") == 0 || strcmp (scheme, "https") == 0))
    message = soup_message_new (method, url);
  if (message == NULL)
    return luaL_error (L, "field 'url' must be an absolute http or https URL with a host, got '%s'", url);

  uri = soup_message_get_uri (message);
  if (g_uri_get_host (uri) == NULL || g_uri_get_host (uri)[0] == '\0' ||
      g_uri_get_user (uri) != NULL || g_uri_get_password (uri) != NULL)
    {
      g_object_unref (message);
      return luaL_error (L, "field 'url' must be an http or https URL with a host and without a user name or password (use an Authorization header)");
    }

  /* From here on nothing raises */
  request_headers = soup_message_get_request_headers (message);
  if (has_headers)
    {
      lua_pushnil (L);
      while (lua_next (L, headers_idx) != 0)
        {
          const char *name = lua_tostring (L, -2);

          /* the body setter below owns the header (it needs the value) */
          if (body != NULL && g_ascii_strcasecmp (name, "Content-Type") == 0)
            content_type_header = lua_tostring (L, -1);
          else
            soup_message_headers_append (request_headers, name, lua_tostring (L, -1));
          lua_pop (L, 1);
        }
    }
  if (soup_message_headers_get_one (request_headers, "Accept") == NULL)
    soup_message_headers_append (request_headers, "Accept", "*/*");
  if (body != NULL)
    {
      g_autoptr (GBytes) bytes = g_bytes_new (body, body_len);

      soup_message_set_request_body_from_bytes (message,
                                                content_type_header != NULL ? content_type_header : content_type,
                                                bytes);
    }

  state = shell_lua_state_from_L (L);
  session = get_session (L, state);

  self = g_object_new (SHELL_TYPE_LUA_NET_REQUEST, NULL);
  shell_lua_own_object (L, G_OBJECT (self), &request_ops);   /* a reference of the context */
  self->state = state;
  self->session = g_object_ref (session);
  self->message = message;
  self->max_bytes = (gsize) max_bytes;

  done_ref = shell_lua_opt_callback (L, 1, "on_done", FALSE);
  error_ref = shell_lua_opt_callback (L, 1, "on_error", FALSE);
  if (done_ref != LUA_NOREF)
    self->on_done = shell_lua_callback_ref (shell_lua_callback_new (L, G_OBJECT (self), done_ref));
  if (error_ref != LUA_NOREF)
    self->on_error = shell_lua_callback_ref (shell_lua_callback_new (L, G_OBJECT (self), error_ref));

  self->restarted_id = g_signal_connect (message, "restarted", G_CALLBACK (message_restarted_cb), self);
  self->timeout_id = g_timeout_add ((guint) timeout_ms, request_timeout_cb, self);
  g_source_set_name_by_id (self->timeout_id, "[gnome-shell] shell.network timeout");

  soup_session_send_async (session, message, G_PRIORITY_DEFAULT, self->cancellable,
                           send_cb, g_object_ref (self));

  shell_lua_handle_push (L, REQUEST_TYPE, G_OBJECT (self));
  g_object_unref (self);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* shell.Request                                                             */
/* ------------------------------------------------------------------------ */

static int
l_request_cancel (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, REQUEST_TYPE);

  if (object != NULL)
    request_schedule_cancel (SHELL_LUA_NET_REQUEST (object));
  return 0;
}

static const ShellLuaHandleMethod request_methods[] = {
  { "cancel",  l_request_cancel,                SHELL_LUA_METHOD_DEAD_OK },
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty request_properties[] = {
  { NULL }
};

static ShellLuaHandleType request_type = {
  .name = REQUEST_TYPE,
  .properties = request_properties,
  .methods_ex = request_methods,
};

static const luaL_Reg network_funcs[] = {
  { "request", l_request },
  { NULL, NULL }
};

static void
shell_lua_api_network_register (ShellLuaState *state,
                                lua_State     *L)
{
  request_type.gtype = SHELL_TYPE_LUA_NET_REQUEST;
  shell_lua_handle_register_type (L, &request_type);

  lua_newtable (L);
  luaL_setfuncs (L, network_funcs, 0);
  lua_setfield (L, -2, "network");
}

static void __attribute__ ((constructor))
shell_lua_api_network_init (void)
{
  shell_lua_api_register ("network", shell_lua_api_network_register);
}
