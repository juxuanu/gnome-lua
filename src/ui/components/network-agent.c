/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * network-agent.c: The user interface of the NetworkManager secret agent.
 * Port of js/ui/components/networkAgent.js.
 *
 * The secret agent itself is #ShellNetworkAgent (shell-network-agent.c), which
 * emits `new-request` and `cancel-request`. This file has what the JS module
 * had around it:
 *
 *   NetworkSecretDialog   ShellNetworkSecretDialog (final #ShellModalDialog)
 *   VPNRequestHandler     ShellNetworkVpnRequest (final GObject)
 *   NetworkAgent          ShellNetworkAgentComponent (public)
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The component works out which requests are alive on its own: the agent
 *    only accepts a response, or a secret, for a request it still has (it
 *    warns otherwise), and the JS responded to requests that NetworkManager
 *    had canceled (a notification which was dismissed after `cancel-request`)
 *    or answered twice (a dialog which was canceled by `disable ()` while it
 *    was closing). The response is skipped in these cases.
 *  - Of the dialogs and the notifications of a request id, only the
 *    registered ones remove themselves from the table of the component when
 *    they are destroyed (the JS removed whatever was registered under the id).
 *  - A VPN helper which cannot be started answers the request with an
 *    internal error and has no handler (the JS left a half initialized
 *    handler in the table).
 *  - The output of a VPN helper is not processed after the handler was
 *    destroyed, and the standard output is closed together with the standard
 *    input.
 *  - A missing VPN plugin or authentication binary, a keyfile of a VPN
 *    helper which cannot be parsed and the failures of the helper are logged
 *    at the level of the message they had; a failure to initialize the agent
 *    is only a debug message because a missing system bus is the normal
 *    case in a test session.
 *  - The initial value of the password of the mobile broadband and Bluetooth
 *    dialogs is empty, as it always was (the JS read the `value` property of
 *    a setting, which has none).
 *  - After ShellGlobal::shutdown nothing the asynchronous calls complete is
 *    handled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <NetworkManager.h>
#include <nm-vpn-plugin-info.h>
#include <pango/pango.h>

#include "shell-action-modes.h"
#include "shell-global.h"
#include "shell-network-agent.h"
#include "shell-util.h"
#include "st.h"
#include "ui/components/network-agent.h"
#include "ui/dialog.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/modal-dialog.h"
#include "ui/session-mode.h"
#include "ui/shell-entry.h"

#define VPN_UI_GROUP "VPN Plugin UI"

/* ------------------------------------------------------------------------ */
/* The requests the agent has                                               */
/* ------------------------------------------------------------------------ */

#define LIVE_REQUESTS_KEY "shell-network-agent-component-live-requests"

/* The agent warns about the requests it does not have, which the dialogs and
 * the VPN handlers can answer after NetworkManager canceled them. The ids of
 * the requests which it has are kept with the agent. */
static GHashTable *
agent_get_live_requests (ShellNetworkAgent *agent)
{
  GHashTable *requests = g_object_get_data (G_OBJECT (agent), LIVE_REQUESTS_KEY);

  if (requests == NULL)
    {
      requests = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_object_set_data_full (G_OBJECT (agent), LIVE_REQUESTS_KEY, requests,
                              (GDestroyNotify) g_hash_table_unref);
    }

  return requests;
}

static void
agent_track_request (ShellNetworkAgent *agent,
                     const char        *request_id)
{
  g_hash_table_add (agent_get_live_requests (agent), g_strdup (request_id));
}

static void
agent_forget_request (ShellNetworkAgent *agent,
                      const char        *request_id)
{
  g_hash_table_remove (agent_get_live_requests (agent), request_id);
}

static gboolean
agent_has_request (ShellNetworkAgent *agent,
                   const char        *request_id)
{
  return g_hash_table_contains (agent_get_live_requests (agent), request_id);
}

static void
agent_respond (ShellNetworkAgent         *agent,
               const char                *request_id,
               ShellNetworkAgentResponse  response)
{
  g_autofree char *id = g_strdup (request_id);

  if (!agent_has_request (agent, id))
    return;

  agent_forget_request (agent, id);
  shell_network_agent_respond (agent, id, response);
}

static void
agent_add_vpn_secret (ShellNetworkAgent *agent,
                      const char        *request_id,
                      const char        *key,
                      const char        *value)
{
  if (agent_has_request (agent, request_id))
    shell_network_agent_add_vpn_secret (agent, request_id, key, value);
}

static void
agent_set_password (ShellNetworkAgent *agent,
                    const char        *request_id,
                    const char        *key,
                    const char        *value)
{
  if (agent_has_request (agent, request_id))
    shell_network_agent_set_password (agent, request_id, key, value);
}

/* ------------------------------------------------------------------------ */
/* The content of a dialog                                                  */
/* ------------------------------------------------------------------------ */

typedef enum
{
  SECRET_VALIDATE_NONE,       /* the secret is valid if it is not empty */
  SECRET_VALIDATE_WPA_PSK,    /* _validateWpaPsk () */
  SECRET_VALIDATE_STATIC_WEP, /* _validateStaticWep () */
} SecretValidate;

/* What the dialog asks for */
typedef struct
{
  char *label;
  char *key;                  /* NULL: the value is shown, but not answered */
  char *value;
  gboolean password;
  NMWepKeyType wep_key_type;
  SecretValidate validate;
  gboolean valid;
  StEntry *entry;             /* weak */
} Secret;

typedef struct
{
  char *title;
  char *message;
  GPtrArray *secrets;         /* Secret */
} DialogContent;

static void
secret_free (Secret *secret)
{
  g_clear_weak_pointer (&secret->entry);
  g_free (secret->label);
  g_free (secret->key);
  g_free (secret->value);
  g_free (secret);
}

static Secret *
secret_new (const char     *label,
            const char     *key,
            const char     *value,
            gboolean        password,
            SecretValidate  validate)
{
  Secret *secret = g_new0 (Secret, 1);

  secret->label = g_strdup (label);
  secret->key = g_strdup (key);
  secret->value = g_strdup (value != NULL ? value : "");
  secret->password = password;
  secret->validate = validate;

  return secret;
}

static DialogContent *
dialog_content_new (void)
{
  DialogContent *content = g_new0 (DialogContent, 1);

  content->secrets = g_ptr_array_new_with_free_func ((GDestroyNotify) secret_free);

  return content;
}

static void
dialog_content_free (DialogContent *content)
{
  if (content == NULL)
    return;

  g_free (content->title);
  g_free (content->message);
  g_ptr_array_unref (content->secrets);
  g_free (content);
}

/* The length of @value in the units that the JS strings count (UTF-16) */
static gsize
utf16_length (const char *value)
{
  gsize length = 0;
  const char *p;

  for (p = value; *p != '\0'; p = g_utf8_next_char (p))
    length += g_utf8_get_char (p) > 0xFFFF ? 2 : 1;

  return length;
}

static gboolean
is_hex_digit (char c)
{
  return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9');
}

static gboolean
is_letter (char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* _validateWpaPsk () */
static gboolean
validate_wpa_psk (Secret *secret)
{
  const char *value = secret->value;
  gsize length = utf16_length (value);
  gsize i;

  if (length == 64)
    {
      /* must be composed of hexadecimal digits only */
      for (i = 0; value[i] != '\0'; i++)
        {
          if (!is_hex_digit (value[i]))
            return FALSE;
        }

      return TRUE;
    }

  return length >= 8 && length <= 63;
}

/* _validateStaticWep () */
static gboolean
validate_static_wep (Secret *secret)
{
  const char *value = secret->value;
  gsize length = utf16_length (value);
  gsize i;

  if (secret->wep_key_type == NM_WEP_KEY_TYPE_KEY)
    {
      if (length == 10 || length == 26)
        {
          for (i = 0; value[i] != '\0'; i++)
            {
              if (!is_hex_digit (value[i]))
                return FALSE;
            }
        }
      else if (length == 5 || length == 13)
        {
          for (i = 0; value[i] != '\0'; i++)
            {
              if (!is_letter (value[i]))
                return FALSE;
            }
        }
      else
        {
          return FALSE;
        }
    }
  else if (secret->wep_key_type == NM_WEP_KEY_TYPE_PASSPHRASE)
    {
      if (length > 64)
        return FALSE;
    }

  return TRUE;
}

static gboolean
secret_check_valid (Secret *secret)
{
  switch (secret->validate)
    {
    case SECRET_VALIDATE_WPA_PSK:
      return validate_wpa_psk (secret);

    case SECRET_VALIDATE_STATIC_WEP:
      return validate_static_wep (secret);

    case SECRET_VALIDATE_NONE:
    default:
      /* no special validation, just ensure it's not empty */
      return secret->value[0] != '\0';
    }
}

static gboolean
hints_contain (const char * const *hints,
               const char         *hint)
{
  return hints != NULL && g_strv_contains (hints, hint);
}

/* _get8021xSecrets () */
static void
get_8021x_secrets (NMConnection        *connection,
                   const char          *setting_name,
                   const char * const  *hints,
                   GPtrArray           *secrets)
{
  NMSetting8021x *setting = nm_connection_get_setting_802_1x (connection);
  const char *eap_method;

  if (setting == NULL)
    {
      g_message ("Invalid EAP/IEEE802.1x connection: no 802.1x setting");
      return;
    }

  /* If hints were given we know exactly what we need to ask */
  if (g_strcmp0 (setting_name, "802-1x") == 0 && hints != NULL && hints[0] != NULL)
    {
      if (hints_contain (hints, "identity"))
        g_ptr_array_add (secrets,
                         secret_new (_("Username"), "identity",
                                     nm_setting_802_1x_get_identity (setting),
                                     FALSE, SECRET_VALIDATE_NONE));

      if (hints_contain (hints, "password"))
        g_ptr_array_add (secrets,
                         secret_new (_("Password"), "password",
                                     nm_setting_802_1x_get_password (setting),
                                     TRUE, SECRET_VALIDATE_NONE));

      if (hints_contain (hints, "private-key-password"))
        g_ptr_array_add (secrets,
                         secret_new (_("Private key password"), "private-key-password",
                                     nm_setting_802_1x_get_private_key_password (setting),
                                     TRUE, SECRET_VALIDATE_NONE));

      return;
    }

  eap_method = nm_setting_802_1x_get_eap_method (setting, 0);
  if (g_strcmp0 (eap_method, "md5") == 0 ||
      g_strcmp0 (eap_method, "leap") == 0 ||
      g_strcmp0 (eap_method, "ttls") == 0 ||
      g_strcmp0 (eap_method, "peap") == 0 ||
      g_strcmp0 (eap_method, "fast") == 0)
    {
      /* TTLS and PEAP are actually much more complicated, but this complication
       * is not visible here since we only care about phase2 authentication
       * (and don't even care of which one) */
      g_ptr_array_add (secrets,
                       secret_new (_("Username"), NULL,
                                   nm_setting_802_1x_get_identity (setting),
                                   FALSE, SECRET_VALIDATE_NONE));
      g_ptr_array_add (secrets,
                       secret_new (_("Password"), "password",
                                   nm_setting_802_1x_get_password (setting),
                                   TRUE, SECRET_VALIDATE_NONE));
    }
  else if (g_strcmp0 (eap_method, "tls") == 0)
    {
      g_ptr_array_add (secrets,
                       secret_new (_("Identity"), NULL,
                                   nm_setting_802_1x_get_identity (setting),
                                   FALSE, SECRET_VALIDATE_NONE));
      g_ptr_array_add (secrets,
                       secret_new (_("Private key password"), "private-key-password",
                                   nm_setting_802_1x_get_private_key_password (setting),
                                   TRUE, SECRET_VALIDATE_NONE));
    }
  else
    {
      g_message ("Invalid EAP/IEEE802.1x method: %s", eap_method != NULL ? eap_method : "(null)");
    }
}

/* _getWirelessSecrets () */
static void
get_wireless_secrets (NMConnection        *connection,
                      const char          *setting_name,
                      const char * const  *hints,
                      GPtrArray           *secrets)
{
  NMSettingWirelessSecurity *setting = nm_connection_get_setting_wireless_security (connection);
  const char *key_mgmt;

  if (g_strcmp0 (setting_name, "802-1x") == 0)
    {
      get_8021x_secrets (connection, setting_name, hints, secrets);
      return;
    }

  if (setting == NULL)
    {
      g_message ("Invalid wireless connection: no wireless security setting");
      return;
    }

  key_mgmt = nm_setting_wireless_security_get_key_mgmt (setting);

  /* First the easy ones */
  if (g_strcmp0 (key_mgmt, "wpa-none") == 0 ||
      g_strcmp0 (key_mgmt, "wpa-psk") == 0 ||
      g_strcmp0 (key_mgmt, "sae") == 0)
    {
      g_ptr_array_add (secrets,
                       secret_new (_("Password"), "psk",
                                   nm_setting_wireless_security_get_psk (setting),
                                   TRUE, SECRET_VALIDATE_WPA_PSK));
    }
  else if (g_strcmp0 (key_mgmt, "none") == 0) /* static WEP */
    {
      guint32 index = nm_setting_wireless_security_get_wep_tx_keyidx (setting);
      g_autofree char *key = g_strdup_printf ("wep-key%u", index);
      Secret *secret;

      secret = secret_new (_("Key"), key,
                           nm_setting_wireless_security_get_wep_key (setting, index),
                           TRUE, SECRET_VALIDATE_STATIC_WEP);
      secret->wep_key_type = nm_setting_wireless_security_get_wep_key_type (setting);
      g_ptr_array_add (secrets, secret);
    }
  else if (g_strcmp0 (key_mgmt, "ieee8021x") == 0)
    {
      if (g_strcmp0 (nm_setting_wireless_security_get_auth_alg (setting), "leap") == 0) /* Cisco LEAP */
        g_ptr_array_add (secrets,
                         secret_new (_("Password"), "leap-password",
                                     nm_setting_wireless_security_get_leap_password (setting),
                                     TRUE, SECRET_VALIDATE_NONE));
      else /* Dynamic (IEEE 802.1x) WEP */
        get_8021x_secrets (connection, setting_name, hints, secrets);
    }
  else if (g_strcmp0 (key_mgmt, "wpa-eap") == 0)
    {
      get_8021x_secrets (connection, setting_name, hints, secrets);
    }
  else
    {
      g_message ("Invalid wireless key management: %s", key_mgmt != NULL ? key_mgmt : "(null)");
    }
}

/* _getPPPoESecrets () */
static void
get_pppoe_secrets (NMConnection *connection,
                   GPtrArray    *secrets)
{
  NMSettingPppoe *setting = nm_connection_get_setting_pppoe (connection);

  if (setting == NULL)
    {
      g_message ("Invalid DSL connection: no PPPoE setting");
      return;
    }

  g_ptr_array_add (secrets,
                   secret_new (_("Username"), "username",
                               nm_setting_pppoe_get_username (setting),
                               FALSE, SECRET_VALIDATE_NONE));
  g_ptr_array_add (secrets,
                   secret_new (_("Service"), "service",
                               nm_setting_pppoe_get_service (setting),
                               FALSE, SECRET_VALIDATE_NONE));
  g_ptr_array_add (secrets,
                   secret_new (_("Password"), "password",
                               nm_setting_pppoe_get_password (setting),
                               TRUE, SECRET_VALIDATE_NONE));
}

/* _getMobileSecrets (); the settings do not have a `value` */
static void
get_mobile_secrets (GPtrArray *secrets)
{
  g_ptr_array_add (secrets,
                   secret_new (_("Password"), "password", NULL,
                               TRUE, SECRET_VALIDATE_NONE));
}

/* The ssid of the wireless setting of @connection, to show to the user */
static char *
dup_wireless_ssid (NMConnection *connection)
{
  NMSettingWireless *setting = nm_connection_get_setting_wireless (connection);
  GBytes *ssid = setting != NULL ? nm_setting_wireless_get_ssid (setting) : NULL;
  gsize len = 0;
  const guint8 *data;

  if (ssid == NULL)
    return g_strdup ("");

  data = g_bytes_get_data (ssid, &len);
  return nm_utils_ssid_to_utf8 (data, len);
}

/* _getContent () */
static DialogContent *
get_content (NMConnection        *connection,
             const char          *setting_name,
             const char * const  *hints)
{
  NMSettingConnection *connection_setting = nm_connection_get_setting_connection (connection);
  const char *connection_type = nm_setting_connection_get_connection_type (connection_setting);
  DialogContent *content = dialog_content_new ();

  if (g_strcmp0 (connection_type, "802-11-wireless") == 0)
    {
      g_autofree char *ssid = dup_wireless_ssid (connection);

      content->title = g_strdup (_("Authentication required"));
      content->message = g_strdup_printf (_("Passwords or encryption keys are required to access the wireless network “%s”"), ssid);
      get_wireless_secrets (connection, setting_name, hints, content->secrets);
    }
  else if (g_strcmp0 (connection_type, "802-3-ethernet") == 0)
    {
      content->title = g_strdup (_("Wired 802.1X authentication"));
      content->message = NULL;
      g_ptr_array_add (content->secrets,
                       secret_new (_("Network name"), NULL,
                                   nm_setting_connection_get_id (connection_setting),
                                   FALSE, SECRET_VALIDATE_NONE));
      get_8021x_secrets (connection, setting_name, hints, content->secrets);
    }
  else if (g_strcmp0 (connection_type, "pppoe") == 0)
    {
      content->title = g_strdup (_("DSL authentication"));
      content->message = NULL;
      get_pppoe_secrets (connection, content->secrets);
    }
  else if (g_strcmp0 (connection_type, "gsm") == 0 && hints_contain (hints, "pin"))
    {
      NMSettingGsm *gsm_setting = nm_connection_get_setting_gsm (connection);

      content->title = g_strdup (_("PIN code required"));
      content->message = g_strdup (_("PIN code is needed for the mobile broadband device"));
      g_ptr_array_add (content->secrets,
                       secret_new (_("PIN"), "pin",
                                   gsm_setting != NULL ? nm_setting_gsm_get_pin (gsm_setting) : NULL,
                                   TRUE, SECRET_VALIDATE_NONE));
    }
  else if (g_strcmp0 (connection_type, "gsm") == 0 ||
           g_strcmp0 (connection_type, "cdma") == 0 ||
           g_strcmp0 (connection_type, "bluetooth") == 0)
    {
      content->title = g_strdup (_("Authentication required"));
      content->message = g_strdup_printf (_("A password is required to connect to “%s”"),
                                          nm_setting_connection_get_id (connection_setting));
      get_mobile_secrets (content->secrets);
    }
  else
    {
      g_message ("Invalid connection type: %s", connection_type != NULL ? connection_type : "(null)");
    }

  return content;
}

/* ------------------------------------------------------------------------ */
/* ShellNetworkSecretDialog                                                 */
/* ------------------------------------------------------------------------ */

/* The dialog which asks the user for the secrets of a connection */

#define SHELL_TYPE_NETWORK_SECRET_DIALOG (shell_network_secret_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellNetworkSecretDialog, shell_network_secret_dialog,
                      SHELL, NETWORK_SECRET_DIALOG, ShellModalDialog)

struct _ShellNetworkSecretDialog
{
  ShellModalDialog parent;

  ShellNetworkAgent *agent;
  char *request_id;
  NMConnection *connection;
  char *setting_name;
  char **hints;
  DialogContent *content;
  StButton *ok_button;          /* weak */
};

G_DEFINE_FINAL_TYPE (ShellNetworkSecretDialog, shell_network_secret_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

#define SECRET_INDEX_KEY "shell-network-secret-index"

/* _updateOkButton () */
static void
secret_dialog_update_ok_button (ShellNetworkSecretDialog *self)
{
  gboolean valid = TRUE;
  guint i;

  if (self->ok_button == NULL)
    return;

  for (i = 0; i < self->content->secrets->len; i++)
    {
      Secret *secret = g_ptr_array_index (self->content->secrets, i);

      valid = valid && secret->valid;
    }

  clutter_actor_set_reactive (CLUTTER_ACTOR (self->ok_button), valid);
  st_widget_set_can_focus (ST_WIDGET (self->ok_button), valid);
}

/* _onOk () */
static void
secret_dialog_on_ok (ShellNetworkSecretDialog *self)
{
  gboolean valid = TRUE;
  gboolean is_vpn = g_strcmp0 (self->setting_name, "vpn") == 0;
  guint i;

  for (i = 0; i < self->content->secrets->len; i++)
    {
      Secret *secret = g_ptr_array_index (self->content->secrets, i);

      valid = valid && secret->valid;
      if (secret->key != NULL)
        {
          if (is_vpn)
            agent_add_vpn_secret (self->agent, self->request_id, secret->key, secret->value);
          else
            agent_set_password (self->agent, self->request_id, secret->key, secret->value);
        }
    }

  if (valid)
    {
      agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_CONFIRMED);
      shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
    }
  /* do nothing if not valid */
}

static void
secret_dialog_ok_button_cb (gpointer user_data)
{
  secret_dialog_on_ok (SHELL_NETWORK_SECRET_DIALOG (user_data));
}

/* cancel () */
static void
shell_network_secret_dialog_cancel (ShellNetworkSecretDialog *self)
{
  agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_USER_CANCELED);
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

static void
secret_dialog_cancel_button_cb (gpointer user_data)
{
  shell_network_secret_dialog_cancel (SHELL_NETWORK_SECRET_DIALOG (user_data));
}

static void
secret_dialog_entry_activate_cb (ClutterText              *text,
                                 ShellNetworkSecretDialog *self)
{
  secret_dialog_on_ok (self);
}

static void
secret_dialog_entry_text_changed_cb (ClutterText              *text,
                                     ShellNetworkSecretDialog *self)
{
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (text), SECRET_INDEX_KEY));
  Secret *secret;

  if (index == 0 || index > self->content->secrets->len)
    return;

  secret = g_ptr_array_index (self->content->secrets, index - 1);

  g_free (secret->value);
  secret->value = g_strdup (clutter_text_get_text (text));
  secret->valid = secret_check_valid (secret);

  secret_dialog_update_ok_button (self);
}

static void
shell_network_secret_dialog_dispose (GObject *object)
{
  ShellNetworkSecretDialog *self = SHELL_NETWORK_SECRET_DIALOG (object);

  g_clear_weak_pointer (&self->ok_button);
  g_clear_object (&self->agent);

  G_OBJECT_CLASS (shell_network_secret_dialog_parent_class)->dispose (object);
}

static void
shell_network_secret_dialog_finalize (GObject *object)
{
  ShellNetworkSecretDialog *self = SHELL_NETWORK_SECRET_DIALOG (object);

  g_clear_pointer (&self->request_id, g_free);
  g_clear_object (&self->connection);
  g_clear_pointer (&self->setting_name, g_free);
  g_clear_pointer (&self->hints, g_strfreev);
  g_clear_pointer (&self->content, dialog_content_free);

  G_OBJECT_CLASS (shell_network_secret_dialog_parent_class)->finalize (object);
}

static void
shell_network_secret_dialog_class_init (ShellNetworkSecretDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_network_secret_dialog_dispose;
  object_class->finalize = shell_network_secret_dialog_finalize;
}

static void
shell_network_secret_dialog_init (ShellNetworkSecretDialog *self)
{
}

/*
 * shell_network_secret_dialog_new:
 * @content_override: (nullable) (transfer full): what to ask for instead of
 *   what the connection needs, the dialog takes it
 */
static ShellNetworkSecretDialog *
shell_network_secret_dialog_new (ShellNetworkAgent             *agent,
                                 const char                    *request_id,
                                 NMConnection                  *connection,
                                 const char                    *setting_name,
                                 const char * const            *hints,
                                 NMSecretAgentGetSecretsFlags   flags,
                                 DialogContent                 *content_override)
{
  ShellNetworkSecretDialog *self =
    g_object_new (SHELL_TYPE_NETWORK_SECRET_DIALOG,
                  "dialog-style-class", "prompt-dialog",
                  NULL);
  ShellModalDialog *modal_dialog = SHELL_MODAL_DIALOG (self);
  ClutterActor *content_box;
  gboolean initial_focus_set = FALSE;
  gboolean any_password = FALSE;
  StButton *ok_button;
  guint i;

  self->agent = g_object_ref (agent);
  self->request_id = g_strdup (request_id);
  self->connection = g_object_ref (connection);
  self->setting_name = g_strdup (setting_name);
  self->hints = g_strdupv ((char **) hints);

  if (content_override != NULL)
    self->content = content_override;
  else
    self->content = get_content (connection, setting_name, hints);

  content_box = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                              "title", self->content->title,
                              "description", self->content->message,
                              NULL);

  for (i = 0; i < self->content->secrets->len; i++)
    {
      Secret *secret = g_ptr_array_index (self->content->secrets, i);
      gboolean reactive = secret->key != NULL;
      ClutterActor *entry;
      ClutterText *text;

      entry = g_object_new (secret->password ? ST_TYPE_PASSWORD_ENTRY : ST_TYPE_ENTRY,
                            "style-class", "prompt-dialog-password-entry",
                            "hint-text", secret->label,
                            "text", secret->value,
                            "can-focus", reactive,
                            "reactive", reactive,
                            "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            NULL);
      g_set_weak_pointer (&secret->entry, ST_ENTRY (entry));
      shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);
      clutter_actor_add_child (content_box, entry);

      secret->valid = secret_check_valid (secret);
      if (secret->password)
        any_password = TRUE;

      if (reactive)
        {
          if (!initial_focus_set)
            {
              shell_modal_dialog_set_initial_key_focus (modal_dialog, entry);
              initial_focus_set = TRUE;
            }

          text = st_entry_get_clutter_text (ST_ENTRY (entry));
          g_object_set_data (G_OBJECT (text), SECRET_INDEX_KEY, GUINT_TO_POINTER (i + 1));
          g_signal_connect_object (text, "activate",
                                   G_CALLBACK (secret_dialog_entry_activate_cb), self, 0);
          g_signal_connect_object (text, "text-changed",
                                   G_CALLBACK (secret_dialog_entry_text_changed_cb), self, 0);
        }
      else
        {
          secret->valid = TRUE;
        }
    }

  if (any_password)
    clutter_actor_add_child (content_box, CLUTTER_ACTOR (shell_caps_lock_warning_new ()));

  if (flags & NM_SECRET_AGENT_GET_SECRETS_FLAG_WPS_PBC_ACTIVE)
    {
      ClutterActor *description_label =
        g_object_new (ST_TYPE_LABEL,
                      "text", _("Alternatively you can connect by pushing the “WPS” button on your router"),
                      "style-class", "message-dialog-description",
                      NULL);
      ClutterText *label_text = st_label_get_clutter_text (ST_LABEL (description_label));

      clutter_text_set_line_wrap (label_text, TRUE);
      clutter_text_set_ellipsize (label_text, PANGO_ELLIPSIZE_NONE);

      clutter_actor_add_child (content_box, description_label);
    }

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (modal_dialog)),
                           content_box);

  shell_modal_dialog_add_button (modal_dialog, _("Cancel"), CLUTTER_KEY_Escape, FALSE,
                                 secret_dialog_cancel_button_cb, self, NULL);
  ok_button = shell_modal_dialog_add_button (modal_dialog, _("Connect"), 0, TRUE,
                                             secret_dialog_ok_button_cb, self, NULL);
  g_set_weak_pointer (&self->ok_button, ok_button);

  secret_dialog_update_ok_button (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNetworkVpnRequest                                                   */
/* ------------------------------------------------------------------------ */

/* The authentication of a VPN connection, which is done by a helper
 * program of the VPN plugin: the helper is given the connection on its
 * standard input and answers on its standard output. Either it asks the user
 * itself, or (the new style plugins) it tells what to ask and the shell shows
 * the dialog. */

/* How to run the authentication helper of a VPN plugin */
typedef struct
{
  char *file_name;
  gboolean supports_hints;
  gboolean external_ui_mode;
} AuthHelper;

static void
auth_helper_free (AuthHelper *helper)
{
  g_free (helper->file_name);
  g_free (helper);
}

#define SHELL_TYPE_NETWORK_VPN_REQUEST (shell_network_vpn_request_get_type ())
G_DECLARE_FINAL_TYPE (ShellNetworkVpnRequest, shell_network_vpn_request,
                      SHELL, NETWORK_VPN_REQUEST, GObject)

struct _ShellNetworkVpnRequest
{
  GObject parent;

  ShellNetworkAgent *agent;
  char *request_id;
  NMConnection *connection;
  NMSecretAgentGetSecretsFlags flags;

  GPid child_pid;
  GOutputStream *stdin_stream;
  GInputStream *stdout_stream;
  GDataInputStream *data_stdout;
  gboolean new_style_plugin;
  char *previous_line;
  gboolean has_previous_line;       /* `_previousLine !== undefined` */

  ShellNetworkSecretDialog *shell_dialog;   /* weak */
  GCancellable *cancellable;
  gboolean destroyed;
};

enum
{
  VPN_REQUEST_DESTROY,

  N_VPN_REQUEST_SIGNALS
};

static guint vpn_request_signals[N_VPN_REQUEST_SIGNALS] = { 0, };

G_DEFINE_FINAL_TYPE (ShellNetworkVpnRequest, shell_network_vpn_request, G_TYPE_OBJECT)

static void shell_network_vpn_request_destroy (ShellNetworkVpnRequest *self);

/* this._stdin.write ('QUIT\n\n', null), ignoring the broken pipes */
static void
vpn_request_write_quit (ShellNetworkVpnRequest *self)
{
  if (self->stdin_stream == NULL || g_output_stream_is_closed (self->stdin_stream))
    return;

  g_output_stream_write_all (self->stdin_stream, "QUIT\n\n", 6, NULL, NULL, NULL);
}

/* cancel () */
static void
shell_network_vpn_request_cancel (ShellNetworkVpnRequest *self,
                                  gboolean                respond)
{
  g_autoptr (ShellNetworkVpnRequest) keep = g_object_ref (self);

  if (respond)
    agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_USER_CANCELED);

  if (self->new_style_plugin && self->shell_dialog != NULL)
    {
      ShellNetworkSecretDialog *dialog = self->shell_dialog;

      shell_modal_dialog_close (SHELL_MODAL_DIALOG (dialog));
      clutter_actor_destroy (CLUTTER_ACTOR (dialog));
    }
  else
    {
      vpn_request_write_quit (self);
    }

  shell_network_vpn_request_destroy (self);
}

/* destroy () */
static void
shell_network_vpn_request_destroy (ShellNetworkVpnRequest *self)
{
  g_autoptr (ShellNetworkVpnRequest) keep = g_object_ref (self);

  if (self->destroyed)
    return;

  /* The agent is not told when the handler is gone: the table of the
   * component forgets it with this signal */
  g_signal_emit (self, vpn_request_signals[VPN_REQUEST_DESTROY], 0);

  self->destroyed = TRUE;
  g_cancellable_cancel (self->cancellable);

  if (self->stdin_stream != NULL)
    g_output_stream_close (self->stdin_stream, NULL, NULL);
  if (self->stdout_stream != NULL)
    g_input_stream_close (self->stdout_stream, NULL, NULL);
}

static void
vpn_request_child_finished_cb (GPid     pid,
                               int      status,
                               gpointer user_data)
{
  g_autoptr (ShellNetworkVpnRequest) self = user_data;
  int exit_status;

  g_spawn_close_pid (pid);

  if (self->destroyed || g_cancellable_is_cancelled (self->cancellable))
    return;

  /* For new style plugin, all work is done in the async reading functions.
   * Just reap the process here */
  if (self->new_style_plugin)
    return;

  if (shell_util_wifexited (status, &exit_status))
    {
      if (exit_status != 0)
        agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_USER_CANCELED);
      else
        agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_CONFIRMED);
    }
  else
    {
      agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
    }

  shell_network_vpn_request_destroy (self);
}

/* _vpnChildProcessLineOldStyle () */
static void
vpn_request_process_line_old_style (ShellNetworkVpnRequest *self,
                                    const char             *line)
{
  if (self->has_previous_line)
    {
      /* Two consecutive newlines mean that the child should be closed
       * (the actual newlines are eaten by GDataInputStream).
       * Send a termination message */
      if (*line == '\0' && self->previous_line != NULL && *self->previous_line == '\0')
        {
          vpn_request_write_quit (self);
        }
      else
        {
          agent_add_vpn_secret (self->agent, self->request_id, self->previous_line, line);
          self->has_previous_line = FALSE;
          g_clear_pointer (&self->previous_line, g_free);
        }
    }
  else
    {
      self->has_previous_line = TRUE;
      g_free (self->previous_line);
      self->previous_line = g_strdup (line);
    }
}

static void vpn_request_read_stdout_old_style (ShellNetworkVpnRequest *self);

static void
vpn_request_read_line_cb (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (ShellNetworkVpnRequest) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *line = NULL;
  g_autofree char *valid_line = NULL;
  gsize length = 0;

  line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (source_object),
                                               res, &length, &error);
  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to read the output of the VPN helper: %s", error->message);
      return;
    }

  if (self->destroyed)
    return;

  if (line == NULL)
    {
      /* end of file */
      g_input_stream_close (self->stdout_stream, NULL, NULL);
      return;
    }

  valid_line = g_utf8_make_valid (line, length);
  vpn_request_process_line_old_style (self, valid_line);

  /* try to read more! */
  if (!self->destroyed)
    vpn_request_read_stdout_old_style (self);
}

/* _readStdoutOldStyle () */
static void
vpn_request_read_stdout_old_style (ShellNetworkVpnRequest *self)
{
  g_data_input_stream_read_line_async (self->data_stdout, G_PRIORITY_DEFAULT,
                                       self->cancellable,
                                       vpn_request_read_line_cb,
                                       g_object_ref (self));
}

/* Fills @content from the keyfile that a new style plugin prints:
 * the secrets that it wants answered are asked, the others are answered */
static gboolean
vpn_request_parse_keyfile (ShellNetworkVpnRequest  *self,
                           GKeyFile                *keyfile,
                           DialogContent           *content,
                           GError                 **error)
{
  g_auto (GStrv) groups = NULL;
  int version;
  gsize n_groups = 0;
  gsize i;

  version = g_key_file_get_integer (keyfile, VPN_UI_GROUP, "Version", error);
  if (version == 0 && *error != NULL)
    return FALSE;

  if (version != 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid plugin keyfile version, is %d", version);
      return FALSE;
    }

  content->title = g_key_file_get_string (keyfile, VPN_UI_GROUP, "Title", error);
  if (content->title == NULL)
    return FALSE;

  content->message = g_key_file_get_string (keyfile, VPN_UI_GROUP, "Description", error);
  if (content->message == NULL)
    return FALSE;

  groups = g_key_file_get_groups (keyfile, &n_groups);
  for (i = 0; i < n_groups; i++)
    {
      g_autofree char *value = NULL;
      gboolean should_ask;

      if (g_strcmp0 (groups[i], VPN_UI_GROUP) == 0)
        continue;

      value = g_key_file_get_string (keyfile, groups[i], "Value", error);
      if (value == NULL)
        return FALSE;

      should_ask = g_key_file_get_boolean (keyfile, groups[i], "ShouldAsk", error);
      if (should_ask == FALSE && *error != NULL)
        return FALSE;

      if (should_ask)
        {
          g_autofree char *label = g_key_file_get_string (keyfile, groups[i], "Label", error);
          gboolean is_secret;

          if (label == NULL)
            return FALSE;

          is_secret = g_key_file_get_boolean (keyfile, groups[i], "IsSecret", error);
          if (is_secret == FALSE && *error != NULL)
            return FALSE;

          g_ptr_array_add (content->secrets,
                           secret_new (label, groups[i], value, is_secret, SECRET_VALIDATE_NONE));
        }
      else
        {
          if (*value == '\0') /* Ignore empty secrets */
            continue;

          agent_add_vpn_secret (self->agent, self->request_id, groups[i], value);
        }
    }

  return TRUE;
}

/* _showNewStyleDialog () */
static void
vpn_request_show_new_style_dialog (ShellNetworkVpnRequest *self)
{
  g_autoptr (GKeyFile) keyfile = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) data = NULL;
  DialogContent *content_override = NULL;
  const guint8 *buffer;
  gsize count = 0;

  buffer = g_buffered_input_stream_peek_buffer (G_BUFFERED_INPUT_STREAM (self->data_stdout), &count);
  data = g_bytes_new (buffer, count);

  /* No output is a valid case: it means "both secrets are stored" */
  if (count > 0)
    {
      content_override = dialog_content_new ();

      if (!g_key_file_load_from_bytes (keyfile, data, G_KEY_FILE_NONE, &error) ||
          !vpn_request_parse_keyfile (self, keyfile, content_override, &error))
        {
          g_warning ("error while reading VPN plugin output keyfile: %s", error->message);

          dialog_content_free (content_override);
          agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
          shell_network_vpn_request_destroy (self);
          return;
        }
    }

  if (content_override != NULL && content_override->secrets->len > 0)
    {
      ShellNetworkSecretDialog *dialog;

      /* Only show the dialog if we actually have something to ask */
      dialog = shell_network_secret_dialog_new (self->agent, self->request_id, self->connection,
                                                "vpn", NULL, self->flags,
                                                g_steal_pointer (&content_override));
      g_set_weak_pointer (&self->shell_dialog, dialog);
      shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
    }
  else
    {
      dialog_content_free (content_override);
      agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_CONFIRMED);
      shell_network_vpn_request_destroy (self);
    }
}

static void vpn_request_read_stdout_new_style (ShellNetworkVpnRequest *self);

static void
vpn_request_fill_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellNetworkVpnRequest) self = user_data;
  GBufferedInputStream *buffered = G_BUFFERED_INPUT_STREAM (source_object);
  g_autoptr (GError) error = NULL;
  gssize count;

  count = g_buffered_input_stream_fill_finish (buffered, res, &error);
  if (count < 0)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to read the output of the VPN helper: %s", error->message);
      return;
    }

  if (self->destroyed)
    return;

  if (count == 0)
    {
      /* end of file */
      vpn_request_show_new_style_dialog (self);

      g_input_stream_close (self->stdout_stream, NULL, NULL);
      return;
    }

  /* Try to read more */
  g_buffered_input_stream_set_buffer_size (buffered,
                                           2 * g_buffered_input_stream_get_buffer_size (buffered));
  vpn_request_read_stdout_new_style (self);
}

/* _readStdoutNewStyle () */
static void
vpn_request_read_stdout_new_style (ShellNetworkVpnRequest *self)
{
  g_buffered_input_stream_fill_async (G_BUFFERED_INPUT_STREAM (self->data_stdout), -1,
                                      G_PRIORITY_DEFAULT, self->cancellable,
                                      vpn_request_fill_cb, g_object_ref (self));
}

typedef struct
{
  GOutputStream *stream;
  gboolean failed;
} WriteConnectionData;

static void
write_connection_string (WriteConnectionData *data,
                         const char          *key_name,
                         const char          *key,
                         const char          *value_name,
                         const char          *value)
{
  g_autofree char *text = NULL;

  if (data->failed)
    return;

  text = g_strdup_printf ("%s=%s\n%s=%s\n\n", key_name, key, value_name, value != NULL ? value : "");
  data->failed = !g_output_stream_write_all (data->stream, text, strlen (text), NULL, NULL, NULL);
}

static void
write_connection_data_item_cb (const char *key,
                               const char *value,
                               gpointer    user_data)
{
  write_connection_string (user_data, "DATA_KEY", key, "DATA_VAL", value);
}

static void
write_connection_secret_cb (const char *key,
                            const char *value,
                            gpointer    user_data)
{
  write_connection_string (user_data, "SECRET_KEY", key, "SECRET_VAL", value);
}

/* _writeConnection () */
static void
vpn_request_write_connection (ShellNetworkVpnRequest *self)
{
  NMSettingVpn *vpn_setting = nm_connection_get_setting_vpn (self->connection);
  WriteConnectionData data = { self->stdin_stream, FALSE };

  if (vpn_setting != NULL)
    {
      nm_setting_vpn_foreach_data_item (vpn_setting, write_connection_data_item_cb, &data);
      nm_setting_vpn_foreach_secret (vpn_setting, write_connection_secret_cb, &data);
    }

  if (!data.failed)
    data.failed = !g_output_stream_write_all (self->stdin_stream, "DONE\n\n", 6, NULL, NULL, NULL);

  if (data.failed)
    {
      g_warning ("internal error while writing connection to helper");

      agent_respond (self->agent, self->request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
      shell_network_vpn_request_destroy (self);
    }
}

static void
shell_network_vpn_request_dispose (GObject *object)
{
  ShellNetworkVpnRequest *self = SHELL_NETWORK_VPN_REQUEST (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_weak_pointer (&self->shell_dialog);
  g_clear_object (&self->data_stdout);
  g_clear_object (&self->stdout_stream);
  g_clear_object (&self->stdin_stream);
  g_clear_object (&self->agent);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (shell_network_vpn_request_parent_class)->dispose (object);
}

static void
shell_network_vpn_request_finalize (GObject *object)
{
  ShellNetworkVpnRequest *self = SHELL_NETWORK_VPN_REQUEST (object);

  g_clear_pointer (&self->request_id, g_free);
  g_clear_pointer (&self->previous_line, g_free);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_network_vpn_request_parent_class)->finalize (object);
}

static void
shell_network_vpn_request_class_init (ShellNetworkVpnRequestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_network_vpn_request_dispose;
  object_class->finalize = shell_network_vpn_request_finalize;

  vpn_request_signals[VPN_REQUEST_DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_network_vpn_request_init (ShellNetworkVpnRequest *self)
{
  self->cancellable = g_cancellable_new ();
}

/*
 * shell_network_vpn_request_new:
 *
 * Starts the authentication helper of a VPN plugin for @connection.
 * Returns: (nullable): the handler, %NULL if the helper cannot be started (the
 *   request is answered with an internal error then)
 */
static ShellNetworkVpnRequest *
shell_network_vpn_request_new (ShellNetworkAgent             *agent,
                               const char                    *request_id,
                               AuthHelper                    *auth_helper,
                               const char                    *service_type,
                               NMConnection                  *connection,
                               const char * const            *hints,
                               NMSecretAgentGetSecretsFlags   flags)
{
  g_autoptr (ShellNetworkVpnRequest) self = g_object_new (SHELL_TYPE_NETWORK_VPN_REQUEST, NULL);
  NMSettingConnection *connection_setting = nm_connection_get_setting_connection (connection);
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_autoptr (GAppLaunchContext) launch_context = NULL;
  g_auto (GStrv) envp = NULL;
  g_autoptr (GError) error = NULL;
  int stdin_fd = -1, stdout_fd = -1, stderr_fd = -1;
  GPid pid;
  guint i;

  self->agent = g_object_ref (agent);
  self->request_id = g_strdup (request_id);
  self->connection = g_object_ref (connection);
  self->flags = flags;
  self->new_style_plugin = auth_helper->external_ui_mode;

  g_ptr_array_add (argv, auth_helper->file_name);
  g_ptr_array_add (argv, (char *) "-u");
  g_ptr_array_add (argv, (char *) nm_setting_connection_get_uuid (connection_setting));
  g_ptr_array_add (argv, (char *) "-n");
  g_ptr_array_add (argv, (char *) nm_setting_connection_get_id (connection_setting));
  g_ptr_array_add (argv, (char *) "-s");
  g_ptr_array_add (argv, (char *) service_type);
  if (auth_helper->external_ui_mode)
    g_ptr_array_add (argv, (char *) "--external-ui-mode");
  if (flags & NM_SECRET_AGENT_GET_SECRETS_FLAG_ALLOW_INTERACTION)
    g_ptr_array_add (argv, (char *) "-i");
  if (flags & NM_SECRET_AGENT_GET_SECRETS_FLAG_REQUEST_NEW)
    g_ptr_array_add (argv, (char *) "-r");
  if (auth_helper->supports_hints)
    {
      for (i = 0; hints != NULL && hints[i] != NULL; i++)
        {
          g_ptr_array_add (argv, (char *) "-t");
          g_ptr_array_add (argv, (char *) hints[i]);
        }
    }
  g_ptr_array_add (argv, NULL);

  launch_context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  envp = g_app_launch_context_get_environment (launch_context);

  pid = shell_util_spawn_async_with_pipes (NULL, /* pwd */
                                           (const char * const *) argv->pdata,
                                           (const char * const *) envp,
                                           G_SPAWN_DO_NOT_REAP_CHILD,
                                           &stdin_fd, &stdout_fd, &stderr_fd,
                                           &error);
  if (pid == 0)
    {
      g_warning ("error while spawning VPN auth helper: %s", error->message);

      agent_respond (agent, request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
      return NULL;
    }

  self->child_pid = pid;
  self->stdin_stream = g_unix_output_stream_new (stdin_fd, TRUE);
  self->stdout_stream = g_unix_input_stream_new (stdout_fd, TRUE);
  g_close (stderr_fd, NULL);
  self->data_stdout = g_data_input_stream_new (self->stdout_stream);

  if (self->new_style_plugin)
    vpn_request_read_stdout_new_style (self);
  else
    vpn_request_read_stdout_old_style (self);

  g_child_watch_add (pid, vpn_request_child_finished_cb, g_object_ref (self));

  vpn_request_write_connection (self);

  return g_steal_pointer (&self);
}

/* ------------------------------------------------------------------------ */
/* ShellNetworkAgentComponent                                               */
/* ------------------------------------------------------------------------ */

/* A request which the user is told about with a notification, until they
 * choose to answer it */
typedef struct
{
  ShellNetworkAgentComponent *component;   /* not owned: the table is the component's */
  char *request_id;
  NMConnection *connection;
  char *setting_name;
  char **hints;
  NMSecretAgentGetSecretsFlags flags;
  ShellNotification *notification;
  gboolean answered;
} NotificationRequest;

struct _ShellNetworkAgentComponent
{
  GObject parent;

  ShellNetworkAgent *native;                /* NULL if it could not be created */
  gboolean initialized;
  GCancellable *cancellable;
  gboolean shutting_down;

  GHashTable *dialogs;                      /* request id -> ShellNetworkSecretDialog */
  GHashTable *vpn_requests;                 /* request id -> ShellNetworkVpnRequest */
  GHashTable *notifications;                /* request id -> NotificationRequest */
};

G_DEFINE_FINAL_TYPE (ShellNetworkAgentComponent, shell_network_agent_component, G_TYPE_OBJECT)

static void
notification_request_free (NotificationRequest *request)
{
  if (request->notification != NULL)
    {
      g_signal_handlers_disconnect_by_data (request->notification, request);
      g_clear_object (&request->notification);
    }

  g_free (request->request_id);
  g_clear_object (&request->connection);
  g_free (request->setting_name);
  g_strfreev (request->hints);
  g_free (request);
}

/* _handleRequest () for the VPN connections: _vpnRequest () */
typedef struct
{
  ShellNetworkAgentComponent *self;
  char *request_id;
  NMConnection *connection;
  char **hints;
  NMSecretAgentGetSecretsFlags flags;
  char *service_type;
} VpnRequestData;

static void
vpn_request_data_free (VpnRequestData *data)
{
  g_clear_object (&data->self);
  g_free (data->request_id);
  g_clear_object (&data->connection);
  g_strfreev (data->hints);
  g_free (data->service_type);
  g_free (data);
}

static void
component_vpn_request_destroy_cb (ShellNetworkVpnRequest      *request,
                                  ShellNetworkAgentComponent  *self)
{
  /* Only forget the handler which is registered */
  if (g_hash_table_lookup (self->vpn_requests, request->request_id) == request)
    g_hash_table_remove (self->vpn_requests, request->request_id);
}

/* _findAuthBinary () */
static AuthHelper *
auth_helper_from_plugin (NMVpnPluginInfo *plugin)
{
  const char *file_name = nm_vpn_plugin_info_get_auth_dialog (plugin);
  const char *prop;
  g_autofree char *trimmed = NULL;
  AuthHelper *helper;

  if (file_name == NULL || !g_file_test (file_name, G_FILE_TEST_IS_EXECUTABLE))
    {
      g_message ("VPN plugin at %s is not executable", file_name != NULL ? file_name : "(null)");
      return NULL;
    }

  prop = nm_vpn_plugin_info_lookup_property (plugin, "GNOME", "supports-external-ui-mode");
  if (prop != NULL)
    trimmed = g_ascii_strdown (g_strstrip (g_strdup (prop)), -1);

  helper = g_new0 (AuthHelper, 1);
  helper->file_name = g_strdup (file_name);
  helper->supports_hints = nm_vpn_plugin_info_supports_hints (plugin);
  helper->external_ui_mode = trimmed != NULL &&
                             (g_strcmp0 (trimmed, "true") == 0 ||
                              g_strcmp0 (trimmed, "yes") == 0 ||
                              g_strcmp0 (trimmed, "on") == 0 ||
                              g_strcmp0 (trimmed, "1") == 0);

  return helper;
}

static void
component_search_vpn_plugin_cb (GObject      *source_object,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  VpnRequestData *data = user_data;
  ShellNetworkAgentComponent *self = data->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (NMVpnPluginInfo) plugin = NULL;
  AuthHelper *binary = NULL;
  ShellNetworkVpnRequest *vpn_request;

  /* The agent can be gone, or the component disabled in the meantime */
  if (self->shutting_down || self->native == NULL ||
      !agent_has_request (self->native, data->request_id))
    {
      vpn_request_data_free (data);
      return;
    }

  plugin = shell_network_agent_search_vpn_plugin_finish (SHELL_NETWORK_AGENT (source_object),
                                                         res, &error);
  if (plugin == NULL)
    g_warning ("%s", error->message);
  else
    binary = auth_helper_from_plugin (plugin);

  if (binary == NULL)
    {
      g_message ("Invalid VPN service type (cannot find authentication binary)");

      /* cancel the auth process */
      agent_respond (self->native, data->request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
      vpn_request_data_free (data);
      return;
    }

  vpn_request = shell_network_vpn_request_new (self->native, data->request_id, binary,
                                               data->service_type, data->connection,
                                               (const char * const *) data->hints, data->flags);
  auth_helper_free (binary);

  if (vpn_request != NULL)
    {
      g_signal_connect (vpn_request, "destroy",
                        G_CALLBACK (component_vpn_request_destroy_cb), self);
      g_hash_table_replace (self->vpn_requests, g_strdup (data->request_id), vpn_request);
    }

  vpn_request_data_free (data);
}

/* _vpnRequest () */
static void
component_vpn_request (ShellNetworkAgentComponent *self,
                       const char                 *request_id,
                       NMConnection               *connection,
                       const char * const         *hints,
                       NMSecretAgentGetSecretsFlags flags)
{
  NMSettingVpn *vpn_setting = nm_connection_get_setting_vpn (connection);
  VpnRequestData *data;

  if (vpn_setting == NULL || nm_setting_vpn_get_service_type (vpn_setting) == NULL)
    {
      g_message ("Invalid VPN service type (cannot find authentication binary)");
      agent_respond (self->native, request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
      return;
    }

  data = g_new0 (VpnRequestData, 1);
  data->self = g_object_ref (self);
  data->request_id = g_strdup (request_id);
  data->connection = g_object_ref (connection);
  data->hints = g_strdupv ((char **) hints);
  data->flags = flags;
  data->service_type = g_strdup (nm_setting_vpn_get_service_type (vpn_setting));

  shell_network_agent_search_vpn_plugin (self->native, data->service_type,
                                         component_search_vpn_plugin_cb, data);
}

static void
component_dialog_destroy_cb (ClutterActor               *dialog,
                             ShellNetworkAgentComponent *self)
{
  ShellNetworkSecretDialog *secret_dialog = SHELL_NETWORK_SECRET_DIALOG (dialog);

  /* Only forget the dialog which is registered */
  if (g_hash_table_lookup (self->dialogs, secret_dialog->request_id) == secret_dialog)
    g_hash_table_remove (self->dialogs, secret_dialog->request_id);
}

/* _handleRequest () */
static void
component_handle_request (ShellNetworkAgentComponent *self,
                          const char                 *request_id,
                          NMConnection               *connection,
                          const char                 *setting_name,
                          const char * const         *hints,
                          NMSecretAgentGetSecretsFlags flags)
{
  ShellNetworkSecretDialog *dialog;

  if (g_strcmp0 (setting_name, "vpn") == 0)
    {
      component_vpn_request (self, request_id, connection, hints, flags);
      return;
    }

  dialog = shell_network_secret_dialog_new (self->native, request_id, connection,
                                            setting_name, hints, flags, NULL);
  g_signal_connect (dialog, "destroy",
                    G_CALLBACK (component_dialog_destroy_cb), self);
  g_hash_table_replace (self->dialogs, g_strdup (request_id), g_object_ref (dialog));
  shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
}

static void
notification_request_activated_cb (ShellNotification *notification,
                                   NotificationRequest *request)
{
  ShellNetworkAgentComponent *self = request->component;
  g_autofree char *request_id = g_strdup (request->request_id);
  g_autoptr (NMConnection) connection = g_object_ref (request->connection);
  g_autofree char *setting_name = g_strdup (request->setting_name);
  g_auto (GStrv) hints = g_strdupv (request->hints);
  NMSecretAgentGetSecretsFlags flags = request->flags;

  request->answered = TRUE;

  if (self->native != NULL)
    component_handle_request (self, request_id, connection, setting_name,
                              (const char * const *) hints, flags);
}

static void
notification_request_destroy_cb (ShellNotification              *notification,
                                 ShellNotificationDestroyedReason reason,
                                 NotificationRequest            *request)
{
  ShellNetworkAgentComponent *self = request->component;
  g_autofree char *request_id = g_strdup (request->request_id);

  if (!request->answered && self->native != NULL)
    agent_respond (self->native, request_id, SHELL_NETWORK_AGENT_USER_CANCELED);

  /* Only forget the notification which is registered */
  if (g_hash_table_lookup (self->notifications, request_id) == request)
    g_hash_table_remove (self->notifications, request_id);
}

/* _showNotification () */
static void
component_show_notification (ShellNetworkAgentComponent *self,
                             const char                 *request_id,
                             NMConnection               *connection,
                             const char                 *setting_name,
                             const char * const         *hints,
                             NMSecretAgentGetSecretsFlags flags)
{
  NMSettingConnection *connection_setting = nm_connection_get_setting_connection (connection);
  const char *connection_type = nm_setting_connection_get_connection_type (connection_setting);
  g_autofree char *title = NULL;
  g_autofree char *body = NULL;
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main_get_default ());
  ShellSource *source;
  g_autoptr (ShellNotification) notification = NULL;
  NotificationRequest *request;

  if (g_strcmp0 (connection_type, "802-11-wireless") == 0)
    {
      g_autofree char *ssid = dup_wireless_ssid (connection);

      title = g_strdup (_("Authentication required"));
      body = g_strdup_printf (_("Passwords or encryption keys are required to access the wireless network “%s”"), ssid);
    }
  else if (g_strcmp0 (connection_type, "802-3-ethernet") == 0)
    {
      title = g_strdup (_("Wired 802.1X authentication"));
      body = g_strdup_printf (_("A password is required to connect to “%s”"),
                              nm_connection_get_id (connection));
    }
  else if (g_strcmp0 (connection_type, "pppoe") == 0)
    {
      title = g_strdup (_("DSL authentication"));
      body = g_strdup_printf (_("A password is required to connect to “%s”"),
                              nm_connection_get_id (connection));
    }
  else if (g_strcmp0 (connection_type, "gsm") == 0 && hints_contain (hints, "pin"))
    {
      title = g_strdup (_("PIN code required"));
      body = g_strdup (_("PIN code is needed for the mobile broadband device"));
    }
  else if (g_strcmp0 (connection_type, "gsm") == 0 ||
           g_strcmp0 (connection_type, "cdma") == 0 ||
           g_strcmp0 (connection_type, "bluetooth") == 0)
    {
      title = g_strdup (_("Authentication required"));
      body = g_strdup_printf (_("A password is required to connect to “%s”"),
                              nm_setting_connection_get_id (connection_setting));
    }
  else if (g_strcmp0 (connection_type, "vpn") == 0)
    {
      title = g_strdup (_("VPN password"));
      body = g_strdup_printf (_("A password is required to connect to “%s”"),
                              nm_setting_connection_get_id (connection_setting));
    }
  else
    {
      g_message ("Invalid connection type: %s", connection_type != NULL ? connection_type : "(null)");
      agent_respond (self->native, request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
      return;
    }

  source = message_tray != NULL ? shell_message_tray_get_system_source (message_tray) : NULL;
  if (source == NULL)
    {
      agent_respond (self->native, request_id, SHELL_NETWORK_AGENT_INTERNAL_ERROR);
      return;
    }

  notification = shell_notification_new (source, title, body);
  shell_notification_set_icon_name (notification, "dialog-password-symbolic");

  request = g_new0 (NotificationRequest, 1);
  request->component = self;
  request->request_id = g_strdup (request_id);
  request->connection = g_object_ref (connection);
  request->setting_name = g_strdup (setting_name);
  request->hints = g_strdupv ((char **) hints);
  request->flags = flags;
  request->notification = g_object_ref (notification);

  g_signal_connect (notification, "activated",
                    G_CALLBACK (notification_request_activated_cb), request);
  g_signal_connect (notification, "destroy",
                    G_CALLBACK (notification_request_destroy_cb), request);

  g_hash_table_replace (self->notifications, g_strdup (request_id), request);

  shell_source_add_notification (source, notification);
}

/* _newRequest () */
static void
component_new_request_cb (ShellNetworkAgent          *agent,
                          const char                 *request_id,
                          NMConnection               *connection,
                          const char                 *setting_name,
                          char                      **hints,
                          int                         flags,
                          ShellNetworkAgentComponent *self)
{
  agent_track_request (agent, request_id);

  if (!(flags & NM_SECRET_AGENT_GET_SECRETS_FLAG_USER_REQUESTED))
    component_show_notification (self, request_id, connection, setting_name,
                                 (const char * const *) hints, flags);
  else
    component_handle_request (self, request_id, connection, setting_name,
                              (const char * const *) hints, flags);
}

/* _cancelRequest () */
static void
component_cancel_request_cb (ShellNetworkAgent          *agent,
                             const char                 *request_id,
                             ShellNetworkAgentComponent *self)
{
  g_autofree char *id = g_strdup (request_id);
  ShellNetworkSecretDialog *dialog;
  ShellNetworkVpnRequest *vpn_request;

  agent_forget_request (agent, id);

  dialog = g_hash_table_lookup (self->dialogs, id);
  vpn_request = g_hash_table_lookup (self->vpn_requests, id);

  if (dialog != NULL)
    {
      g_autoptr (ShellNetworkSecretDialog) keep = g_object_ref (dialog);

      g_hash_table_remove (self->dialogs, id);
      shell_modal_dialog_close (SHELL_MODAL_DIALOG (keep));
      clutter_actor_destroy (CLUTTER_ACTOR (keep));
    }
  else if (vpn_request != NULL)
    {
      g_autoptr (ShellNetworkVpnRequest) keep = g_object_ref (vpn_request);

      g_hash_table_remove (self->vpn_requests, id);
      shell_network_vpn_request_cancel (keep, FALSE);
    }
}

static void
component_init_native_cb (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (ShellNetworkAgentComponent) self = user_data;
  g_autoptr (GError) error = NULL;

  if (g_async_initable_init_finish (G_ASYNC_INITABLE (source_object), res, &error))
    {
      if (!self->shutting_down)
        self->initialized = TRUE;
      return;
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->shutting_down)
    return;

  /* A system bus is not required either */
  g_debug ("error initializing the NetworkManager Agent: %s", error->message);
  if (self->native != NULL)
    g_signal_handlers_disconnect_by_data (self->native, self);
  g_clear_object (&self->native);
}

/* enable () */
void
shell_network_agent_component_enable (ShellNetworkAgentComponent *self)
{
  gboolean registered = FALSE;

  g_return_if_fail (SHELL_IS_NETWORK_AGENT_COMPONENT (self));

  if (self->native == NULL)
    return;

  g_object_set (self->native, "auto-register", TRUE, NULL);
  g_object_get (self->native, "registered", &registered, NULL);
  if (self->initialized && !registered)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      nm_secret_agent_old_register_async (NM_SECRET_AGENT_OLD (self->native), NULL, NULL, NULL);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
}

/* Dialogs, VPN handlers and notifications, in a way that survives the removal
 * of the entries from @table. The values are referenced. */
static GPtrArray *
table_dup_values (GHashTable *table)
{
  GPtrArray *values = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_ptr_array_add (values, g_object_ref (value));

  return values;
}

/* disable () */
void
shell_network_agent_component_disable (ShellNetworkAgentComponent *self)
{
  g_autoptr (GPtrArray) dialogs = NULL;
  g_autoptr (GPtrArray) vpn_requests = NULL;
  g_autoptr (GPtrArray) notifications = NULL;
  GHashTableIter iter;
  gpointer value;
  gboolean registered = FALSE;
  guint i;

  g_return_if_fail (SHELL_IS_NETWORK_AGENT_COMPONENT (self));

  dialogs = table_dup_values (self->dialogs);
  for (i = 0; i < dialogs->len; i++)
    shell_network_secret_dialog_cancel (g_ptr_array_index (dialogs, i));
  g_hash_table_remove_all (self->dialogs);

  vpn_requests = table_dup_values (self->vpn_requests);
  for (i = 0; i < vpn_requests->len; i++)
    shell_network_vpn_request_cancel (g_ptr_array_index (vpn_requests, i), TRUE);
  g_hash_table_remove_all (self->vpn_requests);

  notifications = g_ptr_array_new_with_free_func (g_object_unref);
  g_hash_table_iter_init (&iter, self->notifications);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      NotificationRequest *request = value;

      if (request->notification != NULL)
        g_ptr_array_add (notifications, g_object_ref (request->notification));
    }
  for (i = 0; i < notifications->len; i++)
    shell_notification_destroy (g_ptr_array_index (notifications, i),
                                SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
  g_hash_table_remove_all (self->notifications);

  if (self->native == NULL)
    return;

  g_object_set (self->native, "auto-register", FALSE, NULL);
  g_object_get (self->native, "registered", &registered, NULL);
  if (self->initialized && registered)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      nm_secret_agent_old_unregister_async (NM_SECRET_AGENT_OLD (self->native), NULL, NULL, NULL);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
}

static void
component_shutdown_cb (ShellGlobal               *global,
                       ShellNetworkAgentComponent *self)
{
  GHashTableIter iter;
  gpointer value;

  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);

  /* The helpers are not asked anything anymore */
  g_hash_table_iter_init (&iter, self->vpn_requests);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_cancellable_cancel (SHELL_NETWORK_VPN_REQUEST (value)->cancellable);
}

static void
shell_network_agent_component_dispose (GObject *object)
{
  ShellNetworkAgentComponent *self = SHELL_NETWORK_AGENT_COMPONENT (object);

  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);

  if (self->native != NULL)
    g_signal_handlers_disconnect_by_data (self->native, self);

  if (self->vpn_requests != NULL)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->vpn_requests);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        g_signal_handlers_disconnect_by_data (value, self);
      g_hash_table_remove_all (self->vpn_requests);
    }

  if (self->dialogs != NULL)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->dialogs);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        g_signal_handlers_disconnect_by_data (value, self);
      g_hash_table_remove_all (self->dialogs);
    }

  /* Frees the requests, which disconnect their notifications */
  if (self->notifications != NULL)
    g_hash_table_remove_all (self->notifications);

  g_clear_object (&self->native);

  G_OBJECT_CLASS (shell_network_agent_component_parent_class)->dispose (object);
}

static void
shell_network_agent_component_finalize (GObject *object)
{
  ShellNetworkAgentComponent *self = SHELL_NETWORK_AGENT_COMPONENT (object);

  g_clear_pointer (&self->dialogs, g_hash_table_unref);
  g_clear_pointer (&self->vpn_requests, g_hash_table_unref);
  g_clear_pointer (&self->notifications, g_hash_table_unref);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_network_agent_component_parent_class)->finalize (object);
}

static void
shell_network_agent_component_class_init (ShellNetworkAgentComponentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_network_agent_component_dispose;
  object_class->finalize = shell_network_agent_component_finalize;
}

static void
shell_network_agent_component_init (ShellNetworkAgentComponent *self)
{
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  gboolean is_greeter = session_mode != NULL && shell_session_mode_is_greeter (session_mode);

  self->cancellable = g_cancellable_new ();
  self->dialogs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->vpn_requests = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->notifications = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify) notification_request_free);

  self->native = g_object_new (SHELL_TYPE_NETWORK_AGENT,
                               "identifier", "org.gnome.Shell.NetworkAgent",
                               "capabilities", NM_SECRET_AGENT_CAPABILITY_VPN_HINTS,
                               "auto-register", FALSE,
                               "force-always-ask", is_greeter,
                               NULL);

  g_signal_connect (self->native, "new-request",
                    G_CALLBACK (component_new_request_cb), self);
  g_signal_connect (self->native, "cancel-request",
                    G_CALLBACK (component_cancel_request_cb), self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (component_shutdown_cb), self, 0);

  /* _initNative () */
  g_async_initable_init_async (G_ASYNC_INITABLE (self->native), G_PRIORITY_DEFAULT,
                               self->cancellable, component_init_native_cb,
                               g_object_ref (self));
}

/**
 * shell_network_agent_component_new:
 *
 * Creates the component which shows the dialogs and the notifications of the
 * NetworkManager secret agent (`new NetworkAgent ()`). It registers the agent
 * with NetworkManager once it is enabled.
 *
 * Returns: (transfer full): a new #ShellNetworkAgentComponent
 */
ShellNetworkAgentComponent *
shell_network_agent_component_new (void)
{
  return g_object_new (SHELL_TYPE_NETWORK_AGENT_COMPONENT, NULL);
}
