/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * modem-manager.c: The operator name and signal quality of a mobile
 * broadband modem. Port of js/misc/modemManager.js.
 *
 * The JS had ModemBase with the subclasses ModemGsm and ModemCdma (the old
 * ModemManager < 0.7 D-Bus API) and BroadbandModem (the ModemManager1 API).
 * They are one class here, #ShellModem, whose #ShellModem:modem-type selects
 * the D-Bus API; the shell_modem_new_*() constructors are the former
 * constructors. The proxies are created asynchronously, so
 * #ShellModem:operator-name and #ShellModem:signal-quality are set a moment
 * after the modem was created (the JS created them with blocking calls).
 *
 * The only part of the old API the JS destructured wrongly was the struct
 * returned by GetRegistrationInfo() and GetServingSystem(); the struct is
 * unpacked as the interface XML describes it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <nma-mobile-providers.h>

#include "misc/modem-manager.h"
#include "shell-enum-types.h"
#include "shell-global.h"

#include "modem-manager-dbus.h"

#define MM_OLD_BUS_NAME "org.freedesktop.ModemManager"
#define MM1_BUS_NAME "org.freedesktop.ModemManager1"

enum
{
  PROP_0,

  PROP_OPERATOR_NAME,
  PROP_SIGNAL_QUALITY,
  PROP_MODEM_TYPE,
  PROP_PATH,
  PROP_CAPABILITIES,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellModem
{
  GObject parent;

  ShellModemType modem_type;
  char *path;
  NMDeviceModemCapabilities capabilities;

  char *operator_name;
  int signal_quality;

  GCancellable *cancellable;
  gboolean shutting_down;

  /* SHELL_MODEM_TYPE_GSM */
  ShellModemManagerModemGsmNetwork *gsm_proxy;

  /* SHELL_MODEM_TYPE_CDMA */
  ShellModemManagerModemCdma *cdma_proxy;

  /* SHELL_MODEM_TYPE_BROADBAND */
  ShellModemManager1Modem *broadband_proxy;
  ShellModemManager1ModemModem3gpp *proxy_3gpp;
  ShellModemManager1ModemModemCdma *proxy_cdma;
  char *operator_name_3gpp;
  char *operator_name_cdma;
};

G_DEFINE_FINAL_TYPE (ShellModem, shell_modem, G_TYPE_OBJECT)

/* ----------------------------------------------------------------------- */
/* The database of mobile providers                                        */
/* ----------------------------------------------------------------------- */

/*
 * Gets the database of mobile providers, with references between MCCMNC/SID
 * and operator name
 */
static NMAMobileProvidersDatabase *
get_mobile_providers_database (void)
{
  static NMAMobileProvidersDatabase *mpd = NULL;
  g_autoptr (GError) error = NULL;

  if (mpd == NULL)
    {
      mpd = nma_mobile_providers_database_new_sync (NULL, NULL, NULL, &error);
      if (mpd == NULL)
        g_message ("%s", error->message);
    }

  return mpd;
}

/* The length of @str in UTF-16 code units, like JavaScript's String.length */
static size_t
js_string_length (const char *str)
{
  const char *p;
  size_t length = 0;

  if (!g_utf8_validate (str, -1, NULL))
    return strlen (str);

  for (p = str; *p != '\0'; p = g_utf8_next_char (p))
    length += g_utf8_get_char (p) > 0xFFFF ? 2 : 1;

  return length;
}

/* Whether JavaScript's parseInt (@str) would not be NaN */
static gboolean
is_parseable_as_int (const char *str)
{
  while (g_ascii_isspace (*str))
    str++;

  if (*str == '+' || *str == '-')
    str++;

  return g_ascii_isdigit (*str);
}

/*
 * Given an operator name string (which may not be a real operator name) and
 * an operator code string, tries to find a proper operator name to display.
 */
static char *
find_provider_for_mcc_mnc (const char *operator_name,
                           const char *operator_code)
{
  NMAMobileProvidersDatabase *mpd;
  NMAMobileProvider *provider;
  const char *needle;
  size_t name_length = 0;

  if (operator_name != NULL && *operator_name != '\0')
    {
      name_length = js_string_length (operator_name);

      if (name_length > 6 || name_length < 5)
        {
          /* this looks like a valid name, i.e. not an MCCMNC (that some
           * devices return when not yet connected */
          return g_strdup (operator_name);
        }

      if (!is_parseable_as_int (operator_name))
        {
          /* name is definitely not a MCCMNC, so it may be a name
           * after all; return that */
          return g_strdup (operator_name);
        }
    }

  if ((operator_name == NULL || *operator_name == '\0') &&
      operator_code != NULL && *operator_code != '\0')
    needle = operator_code;
  else if (operator_name != NULL && (name_length == 6 || name_length == 5))
    needle = operator_name;
  else /* nothing to search */
    return NULL;

  mpd = get_mobile_providers_database ();
  if (mpd == NULL)
    return NULL;

  provider = nma_mobile_providers_database_lookup_3gpp_mcc_mnc (mpd, needle);
  if (provider == NULL)
    return NULL;

  return g_strdup (nma_mobile_provider_get_name (provider));
}

/*
 * Tries to find the operator name corresponding to the given System
 * Identifier of the serving CDMA network
 */
static char *
find_provider_for_sid (uint32_t sid)
{
  NMAMobileProvidersDatabase *mpd;
  NMAMobileProvider *provider;

  if (sid == 0)
    return NULL;

  mpd = get_mobile_providers_database ();
  if (mpd == NULL)
    return NULL;

  provider = nma_mobile_providers_database_lookup_cdma_sid (mpd, sid);
  if (provider == NULL)
    return NULL;

  return g_strdup (nma_mobile_provider_get_name (provider));
}

/* ----------------------------------------------------------------------- */
/* The properties                                                          */
/* ----------------------------------------------------------------------- */

static void
set_operator_name (ShellModem *self,
                   const char *operator_name)
{
  if (g_strcmp0 (self->operator_name, operator_name) == 0)
    return;

  g_free (self->operator_name);
  self->operator_name = g_strdup (operator_name);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OPERATOR_NAME]);
}

static void
set_signal_quality (ShellModem *self,
                    int         signal_quality)
{
  if (self->signal_quality == signal_quality)
    return;

  self->signal_quality = signal_quality;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SIGNAL_QUALITY]);
}

static gboolean
should_ignore (ShellModem *self,
               GError     *error)
{
  return self->shutting_down ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* ----------------------------------------------------------------------- */
/* Support for the old ModemManager interface (MM < 0.7)                   */
/* ----------------------------------------------------------------------- */

typedef struct
{
  ShellModem *modem;
  GVariant *registration_info;  /* (uss), nullable */
} GsmInitialState;

static void
gsm_initial_state_free (GsmInitialState *state)
{
  g_clear_pointer (&state->registration_info, g_variant_unref);
  g_object_unref (state->modem);
  g_free (state);
}

static void
gsm_set_registration (ShellModem *self,
                      const char *code,
                      const char *name)
{
  g_autofree char *operator_name = find_provider_for_mcc_mnc (name, code);

  set_operator_name (self, operator_name);
}

static void
gsm_signal_quality_ready (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  GsmInitialState *state = user_data;
  ShellModem *self = state->modem;
  g_autoptr (GError) error = NULL;
  unsigned int quality = 0;

  if (!shell_modem_manager_modem_gsm_network_call_get_signal_quality_finish (SHELL_MODEM_MANAGER_MODEM_GSM_NETWORK (source),
                                                                             &quality, res, &error))
    {
      if (!should_ignore (self, error))
        {
          /* it will return an error if the device is not connected */
          set_signal_quality (self, 0);
        }

      gsm_initial_state_free (state);
      return;
    }

  if (!self->shutting_down)
    {
      const char *code, *name;
      uint32_t status;

      g_variant_get (state->registration_info, "(u&s&s)", &status, &code, &name);
      gsm_set_registration (self, code, name);
      set_signal_quality (self, quality);
    }

  gsm_initial_state_free (state);
}

static void
gsm_registration_info_ready (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  GsmInitialState *state = user_data;
  ShellModem *self = state->modem;
  g_autoptr (GError) error = NULL;

  if (!shell_modem_manager_modem_gsm_network_call_get_registration_info_finish (SHELL_MODEM_MANAGER_MODEM_GSM_NETWORK (source),
                                                                                &state->registration_info,
                                                                                res, &error))
    {
      if (!should_ignore (self, error))
        {
          /* it will return an error if the device is not connected */
          set_signal_quality (self, 0);
        }

      gsm_initial_state_free (state);
      return;
    }

  if (self->shutting_down)
    {
      gsm_initial_state_free (state);
      return;
    }

  shell_modem_manager_modem_gsm_network_call_get_signal_quality (self->gsm_proxy,
                                                                 self->cancellable,
                                                                 gsm_signal_quality_ready,
                                                                 state);
}

static void
on_gsm_signal_quality (ShellModemManagerModemGsmNetwork *proxy,
                       unsigned int                      quality,
                       ShellModem                       *self)
{
  set_signal_quality (self, quality);
}

static void
on_gsm_registration_info (ShellModemManagerModemGsmNetwork *proxy,
                          unsigned int                      status,
                          const char                       *code,
                          const char                       *name,
                          ShellModem                       *self)
{
  gsm_set_registration (self, code, name);
}

static void
gsm_proxy_ready (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GError) error = NULL;
  GsmInitialState *state;

  self->gsm_proxy = shell_modem_manager_modem_gsm_network_proxy_new_for_bus_finish (res, &error);
  if (self->gsm_proxy == NULL)
    {
      if (!should_ignore (self, error))
        g_warning ("Could not create a ModemManager GSM proxy: %s", error->message);
      return;
    }

  if (self->shutting_down)
    return;

  /* Code is duplicated because the function have different signatures */
  g_signal_connect_object (self->gsm_proxy, "signal-quality",
                           G_CALLBACK (on_gsm_signal_quality), self, 0);
  g_signal_connect_object (self->gsm_proxy, "registration-info",
                           G_CALLBACK (on_gsm_registration_info), self, 0);

  /* Both are needed for the initial state */
  state = g_new0 (GsmInitialState, 1);
  state->modem = g_object_ref (self);
  shell_modem_manager_modem_gsm_network_call_get_registration_info (self->gsm_proxy,
                                                                    self->cancellable,
                                                                    gsm_registration_info_ready,
                                                                    state);
}

static void
cdma_serving_system_ready (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GVariant) serving_system = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *operator_name = NULL;
  const char *band;
  uint32_t band_class, sid;

  if (!shell_modem_manager_modem_cdma_call_get_serving_system_finish (SHELL_MODEM_MANAGER_MODEM_CDMA (source),
                                                                      &serving_system, res, &error))
    {
      if (!should_ignore (self, error))
        {
          /* it will return an error if the device is not connected */
          set_operator_name (self, NULL);
        }
      return;
    }

  if (self->shutting_down)
    return;

  g_variant_get (serving_system, "(u&su)", &band_class, &band, &sid);
  operator_name = find_provider_for_sid (sid);
  set_operator_name (self, operator_name);
}

static void
cdma_refresh_serving_system (ShellModem *self)
{
  shell_modem_manager_modem_cdma_call_get_serving_system (self->cdma_proxy,
                                                          self->cancellable,
                                                          cdma_serving_system_ready,
                                                          g_object_ref (self));
}

static void
on_cdma_signal_quality (ShellModemManagerModemCdma *proxy,
                        unsigned int                quality,
                        ShellModem                 *self)
{
  set_signal_quality (self, quality);

  /* receiving this signal means the device got activated
   * and we can finally call GetServingSystem */
  if (self->operator_name == NULL)
    cdma_refresh_serving_system (self);
}

static void
cdma_signal_quality_ready (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GError) error = NULL;
  unsigned int quality = 0;

  if (!shell_modem_manager_modem_cdma_call_get_signal_quality_finish (SHELL_MODEM_MANAGER_MODEM_CDMA (source),
                                                                      &quality, res, &error))
    {
      if (!should_ignore (self, error))
        {
          /* it will return an error if the device is not connected */
          set_signal_quality (self, 0);
        }
      return;
    }

  if (!self->shutting_down)
    set_signal_quality (self, quality);
}

static void
cdma_proxy_ready (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GError) error = NULL;

  self->cdma_proxy = shell_modem_manager_modem_cdma_proxy_new_for_bus_finish (res, &error);
  if (self->cdma_proxy == NULL)
    {
      if (!should_ignore (self, error))
        g_warning ("Could not create a ModemManager CDMA proxy: %s", error->message);
      return;
    }

  if (self->shutting_down)
    return;

  g_signal_connect_object (self->cdma_proxy, "signal-quality",
                           G_CALLBACK (on_cdma_signal_quality), self, 0);

  shell_modem_manager_modem_cdma_call_get_signal_quality (self->cdma_proxy,
                                                          self->cancellable,
                                                          cdma_signal_quality_ready,
                                                          g_object_ref (self));
}

/* ------------------------------------------------------- */
/* Support for the new ModemManager1 interface (MM >= 0.7) */
/* ------------------------------------------------------- */

static void
broadband_reload_signal_quality (ShellModem *self)
{
  GVariant *signal_quality;
  uint32_t quality;
  gboolean recent;

  signal_quality = shell_modem_manager1_modem_get_signal_quality (self->broadband_proxy);
  if (signal_quality == NULL)
    return;

  g_variant_get (signal_quality, "(ub)", &quality, &recent);
  set_signal_quality (self, quality);
}

static void
broadband_reload_operator_name (ShellModem *self)
{
  g_autoptr (GString) name = g_string_new ("");

  if (self->operator_name_3gpp != NULL && *self->operator_name_3gpp != '\0')
    g_string_append (name, self->operator_name_3gpp);

  if (self->operator_name_cdma != NULL && *self->operator_name_cdma != '\0')
    {
      if (name->len > 0)
        g_string_append (name, ", ");
      g_string_append (name, self->operator_name_cdma);
    }

  set_operator_name (self, name->str);
}

static void
broadband_reload_3gpp_operator_name (ShellModem *self)
{
  const char *name = shell_modem_manager1_modem_modem3gpp_get_operator_name (self->proxy_3gpp);
  const char *code = shell_modem_manager1_modem_modem3gpp_get_operator_code (self->proxy_3gpp);

  g_free (self->operator_name_3gpp);
  self->operator_name_3gpp = find_provider_for_mcc_mnc (name, code);
  broadband_reload_operator_name (self);
}

static void
broadband_reload_cdma_operator_name (ShellModem *self)
{
  uint32_t sid = shell_modem_manager1_modem_modem_cdma_get_sid (self->proxy_cdma);

  g_free (self->operator_name_cdma);
  self->operator_name_cdma = find_provider_for_sid (sid);
  broadband_reload_operator_name (self);
}

static gboolean
has_property (GVariant   *changed_properties,
              const char *name)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (changed_properties, name, NULL);

  return value != NULL;
}

static void
on_broadband_properties_changed (GDBusProxy         *proxy,
                                 GVariant           *changed_properties,
                                 const char * const *invalidated_properties,
                                 ShellModem         *self)
{
  if (has_property (changed_properties, "SignalQuality"))
    broadband_reload_signal_quality (self);
}

static void
on_3gpp_properties_changed (GDBusProxy         *proxy,
                            GVariant           *changed_properties,
                            const char * const *invalidated_properties,
                            ShellModem         *self)
{
  if (has_property (changed_properties, "OperatorName") ||
      has_property (changed_properties, "OperatorCode"))
    broadband_reload_3gpp_operator_name (self);
}

static void
on_cdma_properties_changed (GDBusProxy         *proxy,
                            GVariant           *changed_properties,
                            const char * const *invalidated_properties,
                            ShellModem         *self)
{
  if (has_property (changed_properties, "Nid") ||
      has_property (changed_properties, "Sid"))
    broadband_reload_cdma_operator_name (self);
}

static void
broadband_proxy_ready (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GError) error = NULL;

  self->broadband_proxy = shell_modem_manager1_modem_proxy_new_for_bus_finish (res, &error);
  if (self->broadband_proxy == NULL)
    {
      if (!should_ignore (self, error))
        g_warning ("Could not create a ModemManager proxy: %s", error->message);
      return;
    }

  if (self->shutting_down)
    return;

  g_signal_connect_object (self->broadband_proxy, "g-properties-changed",
                           G_CALLBACK (on_broadband_properties_changed), self, 0);
  broadband_reload_signal_quality (self);
}

static void
proxy_3gpp_ready (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GError) error = NULL;

  self->proxy_3gpp = shell_modem_manager1_modem_modem3gpp_proxy_new_for_bus_finish (res, &error);
  if (self->proxy_3gpp == NULL)
    {
      if (!should_ignore (self, error))
        g_warning ("Could not create a ModemManager 3GPP proxy: %s", error->message);
      return;
    }

  if (self->shutting_down)
    return;

  g_signal_connect_object (self->proxy_3gpp, "g-properties-changed",
                           G_CALLBACK (on_3gpp_properties_changed), self, 0);
  broadband_reload_3gpp_operator_name (self);
}

static void
proxy_cdma_ready (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (ShellModem) self = user_data;
  g_autoptr (GError) error = NULL;

  self->proxy_cdma = shell_modem_manager1_modem_modem_cdma_proxy_new_for_bus_finish (res, &error);
  if (self->proxy_cdma == NULL)
    {
      if (!should_ignore (self, error))
        g_warning ("Could not create a ModemManager CDMA proxy: %s", error->message);
      return;
    }

  if (self->shutting_down)
    return;

  g_signal_connect_object (self->proxy_cdma, "g-properties-changed",
                           G_CALLBACK (on_cdma_properties_changed), self, 0);
  broadband_reload_cdma_operator_name (self);
}

/* ----------------------------------------------------------------------- */
/* The object                                                              */
/* ----------------------------------------------------------------------- */

/**
 * shell_modem_new_broadband:
 * @path: the D-Bus object path of the modem, as NetworkManager reports it as
 *   the UDI of the device
 * @capabilities: the current capabilities of the NetworkManager device
 *
 * Creates a #ShellModem for a modem of the ModemManager1 D-Bus API
 * (ModemManager 0.7 and newer), the former `BroadbandModem`.
 *
 * Returns: (transfer full): a new #ShellModem
 */
ShellModem *
shell_modem_new_broadband (const char                *path,
                           NMDeviceModemCapabilities  capabilities)
{
  g_return_val_if_fail (path != NULL, NULL);

  return g_object_new (SHELL_TYPE_MODEM,
                       "modem-type", SHELL_MODEM_TYPE_BROADBAND,
                       "path", path,
                       "capabilities", capabilities,
                       NULL);
}

/**
 * shell_modem_new_gsm:
 * @path: the D-Bus object path of the modem, as NetworkManager reports it as
 *   the UDI of the device
 *
 * Creates a #ShellModem for a GSM modem of the old ModemManager D-Bus API
 * (before ModemManager 0.7), the former `ModemGsm`.
 *
 * Returns: (transfer full): a new #ShellModem
 */
ShellModem *
shell_modem_new_gsm (const char *path)
{
  g_return_val_if_fail (path != NULL, NULL);

  return g_object_new (SHELL_TYPE_MODEM,
                       "modem-type", SHELL_MODEM_TYPE_GSM,
                       "path", path,
                       NULL);
}

/**
 * shell_modem_new_cdma:
 * @path: the D-Bus object path of the modem, as NetworkManager reports it as
 *   the UDI of the device
 *
 * Creates a #ShellModem for a CDMA modem of the old ModemManager D-Bus API
 * (before ModemManager 0.7), the former `ModemCdma`.
 *
 * Returns: (transfer full): a new #ShellModem
 */
ShellModem *
shell_modem_new_cdma (const char *path)
{
  g_return_val_if_fail (path != NULL, NULL);

  return g_object_new (SHELL_TYPE_MODEM,
                       "modem-type", SHELL_MODEM_TYPE_CDMA,
                       "path", path,
                       NULL);
}

/**
 * shell_modem_get_operator_name:
 * @modem: a #ShellModem
 *
 * Returns: (nullable): the name of the operator the modem is connected to
 */
const char *
shell_modem_get_operator_name (ShellModem *modem)
{
  g_return_val_if_fail (SHELL_IS_MODEM (modem), NULL);

  return modem->operator_name;
}

/**
 * shell_modem_get_signal_quality:
 * @modem: a #ShellModem
 *
 * Returns: the signal quality in percent, 0 if unknown
 */
int
shell_modem_get_signal_quality (ShellModem *modem)
{
  g_return_val_if_fail (SHELL_IS_MODEM (modem), 0);

  return modem->signal_quality;
}

static void
on_shutdown (ShellGlobal *global,
             ShellModem  *self)
{
  /* The pending calls are cancelled and the proxies stop reporting */
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);

  if (self->gsm_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->gsm_proxy, self);
  if (self->cdma_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->cdma_proxy, self);
  if (self->broadband_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->broadband_proxy, self);
  if (self->proxy_3gpp != NULL)
    g_signal_handlers_disconnect_by_data (self->proxy_3gpp, self);
  if (self->proxy_cdma != NULL)
    g_signal_handlers_disconnect_by_data (self->proxy_cdma, self);
}

static void
shell_modem_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  ShellModem *self = SHELL_MODEM (object);

  switch (prop_id)
    {
    case PROP_MODEM_TYPE:
      self->modem_type = g_value_get_enum (value);
      break;

    case PROP_PATH:
      g_free (self->path);
      self->path = g_value_dup_string (value);
      break;

    case PROP_CAPABILITIES:
      self->capabilities = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_modem_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  ShellModem *self = SHELL_MODEM (object);

  switch (prop_id)
    {
    case PROP_OPERATOR_NAME:
      g_value_set_string (value, self->operator_name);
      break;

    case PROP_SIGNAL_QUALITY:
      g_value_set_int (value, self->signal_quality);
      break;

    case PROP_MODEM_TYPE:
      g_value_set_enum (value, self->modem_type);
      break;

    case PROP_PATH:
      g_value_set_string (value, self->path);
      break;

    case PROP_CAPABILITIES:
      g_value_set_flags (value, self->capabilities);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_modem_constructed (GObject *object)
{
  ShellModem *self = SHELL_MODEM (object);

  G_OBJECT_CLASS (shell_modem_parent_class)->constructed (object);

  g_return_if_fail (self->path != NULL);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  switch (self->modem_type)
    {
    case SHELL_MODEM_TYPE_GSM:
      shell_modem_manager_modem_gsm_network_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                                               G_DBUS_PROXY_FLAGS_NONE,
                                                               MM_OLD_BUS_NAME,
                                                               self->path,
                                                               self->cancellable,
                                                               gsm_proxy_ready,
                                                               g_object_ref (self));
      break;

    case SHELL_MODEM_TYPE_CDMA:
      shell_modem_manager_modem_cdma_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        MM_OLD_BUS_NAME,
                                                        self->path,
                                                        self->cancellable,
                                                        cdma_proxy_ready,
                                                        g_object_ref (self));
      break;

    case SHELL_MODEM_TYPE_BROADBAND:
      shell_modem_manager1_modem_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    MM1_BUS_NAME,
                                                    self->path,
                                                    self->cancellable,
                                                    broadband_proxy_ready,
                                                    g_object_ref (self));
      shell_modem_manager1_modem_modem3gpp_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                                              G_DBUS_PROXY_FLAGS_NONE,
                                                              MM1_BUS_NAME,
                                                              self->path,
                                                              self->cancellable,
                                                              proxy_3gpp_ready,
                                                              g_object_ref (self));
      shell_modem_manager1_modem_modem_cdma_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                                               G_DBUS_PROXY_FLAGS_NONE,
                                                               MM1_BUS_NAME,
                                                               self->path,
                                                               self->cancellable,
                                                               proxy_cdma_ready,
                                                               g_object_ref (self));
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
shell_modem_dispose (GObject *object)
{
  ShellModem *self = SHELL_MODEM (object);

  self->shutting_down = TRUE;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_object (&self->gsm_proxy);
  g_clear_object (&self->cdma_proxy);
  g_clear_object (&self->broadband_proxy);
  g_clear_object (&self->proxy_3gpp);
  g_clear_object (&self->proxy_cdma);

  G_OBJECT_CLASS (shell_modem_parent_class)->dispose (object);
}

static void
shell_modem_finalize (GObject *object)
{
  ShellModem *self = SHELL_MODEM (object);

  g_free (self->path);
  g_free (self->operator_name);
  g_free (self->operator_name_3gpp);
  g_free (self->operator_name_cdma);

  G_OBJECT_CLASS (shell_modem_parent_class)->finalize (object);
}

static void
shell_modem_class_init (ShellModemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_modem_set_property;
  object_class->get_property = shell_modem_get_property;
  object_class->constructed = shell_modem_constructed;
  object_class->dispose = shell_modem_dispose;
  object_class->finalize = shell_modem_finalize;

  /**
   * ShellModem:operator-name:
   *
   * The name of the operator the modem is connected to, or %NULL if it is
   * not known (yet).
   */
  props[PROP_OPERATOR_NAME] =
    g_param_spec_string ("operator-name", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModem:signal-quality:
   *
   * The signal quality in percent, 0 if unknown.
   */
  props[PROP_SIGNAL_QUALITY] =
    g_param_spec_int ("signal-quality", NULL, NULL,
                      0, 100, 0,
                      G_PARAM_READABLE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellModem:modem-type:
   *
   * Which ModemManager D-Bus API the modem uses.
   */
  props[PROP_MODEM_TYPE] =
    g_param_spec_enum ("modem-type", NULL, NULL,
                       SHELL_TYPE_MODEM_TYPE,
                       SHELL_MODEM_TYPE_BROADBAND,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellModem:path:
   *
   * The D-Bus object path of the modem.
   */
  props[PROP_PATH] =
    g_param_spec_string ("path", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModem:capabilities:
   *
   * The current capabilities of the NetworkManager device of the modem.
   */
  props[PROP_CAPABILITIES] =
    g_param_spec_flags ("capabilities", NULL, NULL,
                        NM_TYPE_DEVICE_MODEM_CAPABILITIES,
                        NM_DEVICE_MODEM_CAPABILITY_NONE,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_modem_init (ShellModem *self)
{
  self->cancellable = g_cancellable_new ();
}
