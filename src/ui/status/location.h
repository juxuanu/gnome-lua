/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * location.h: The location agent of the shell for GeoClue, the dialog which
 * asks whether an application may know the location and the indicator of the
 * top bar that shows while the location is in use. Port of
 * js/ui/status/location.js.
 */

#pragma once

#include "ui/modal-dialog.h"
#include "ui/panel-menu.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellGeoclueAgent                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_GEOCLUE_AGENT (shell_geoclue_agent_get_type ())
G_DECLARE_FINAL_TYPE (ShellGeoclueAgent, shell_geoclue_agent,
                      SHELL, GEOCLUE_AGENT, GObject)

ShellGeoclueAgent *shell_geoclue_agent_get_default            (void);

gboolean           shell_geoclue_agent_get_enabled            (ShellGeoclueAgent *self);
void               shell_geoclue_agent_set_enabled            (ShellGeoclueAgent *self,
                                                               gboolean           enabled);
gboolean           shell_geoclue_agent_get_in_use             (ShellGeoclueAgent *self);
int                shell_geoclue_agent_get_max_accuracy_level (ShellGeoclueAgent *self);

/* ------------------------------------------------------------------------ */
/* ShellGeolocationDialog                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_GEOLOCATION_DIALOG (shell_geolocation_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellGeolocationDialog, shell_geolocation_dialog,
                      SHELL, GEOLOCATION_DIALOG, ShellModalDialog)

ShellGeolocationDialog *shell_geolocation_dialog_new (const char *name,
                                                      const char *reason,
                                                      guint       req_accuracy_level);

/* ------------------------------------------------------------------------ */
/* ShellLocationIndicator                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LOCATION_INDICATOR (shell_location_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellLocationIndicator, shell_location_indicator,
                      SHELL, LOCATION_INDICATOR, ShellSystemIndicator)

ShellLocationIndicator *shell_location_indicator_new (void);

G_END_DECLS
