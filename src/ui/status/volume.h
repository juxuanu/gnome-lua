/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * volume.h: The sound output and input indicators of the top bar and their
 * quick settings sliders. Port of js/ui/status/volume.js.
 *
 * #ShellStreamSlider and #ShellVolumeIndicator are derivable and abstract; the
 * output and the input flavours are final. The top bar creates the two
 * indicators (`new Shell.VolumeOutputIndicator ()` and
 * `new Shell.VolumeInputIndicator ()`) and adds them to the quick settings.
 */

#pragma once

#include <gio/gio.h>

#include "gvc-mixer-control.h"
#include "gvc-mixer-stream.h"
#include "gvc-mixer-ui-device.h"

#include "ui/panel-menu.h"
#include "ui/quick-settings.h"

G_BEGIN_DECLS

GvcMixerControl *shell_volume_get_mixer_control (void);

/* ------------------------------------------------------------------------ */
/* ShellStreamSlider                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_STREAM_SLIDER (shell_stream_slider_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellStreamSlider, shell_stream_slider,
                          SHELL, STREAM_SLIDER, ShellQuickSlider)

/**
 * ShellStreamSliderClass:
 * @parent_class: the parent class
 * @lookup_device: finds the device of the menu with the given id (the former
 *   `_lookupDevice ()`); must be implemented
 * @activate_device: makes the device the active one (`_activateDevice ()`);
 *   must be implemented
 * @connect_stream: connects to a stream which became the stream of the slider
 *   (`_connectStream ()`); the default watches the volume and the mute state
 * @update_icon: updates the icon of the slider (`_updateIcon ()`)
 * @should_be_visible: whether the slider is shown (`_shouldBeVisible ()`);
 *   the default is whether it has a stream
 */
struct _ShellStreamSliderClass
{
  ShellQuickSliderClass parent_class;

  GvcMixerUIDevice * (* lookup_device)     (ShellStreamSlider *self,
                                            guint              id);
  void               (* activate_device)   (ShellStreamSlider *self,
                                            GvcMixerUIDevice  *device);
  void               (* connect_stream)    (ShellStreamSlider *self,
                                            GvcMixerStream    *stream);
  void               (* update_icon)       (ShellStreamSlider *self);
  gboolean           (* should_be_visible) (ShellStreamSlider *self);

  /*< private >*/
  gpointer padding[8];
};

GvcMixerControl *shell_stream_slider_get_control      (ShellStreamSlider  *self);
GvcMixerStream  *shell_stream_slider_get_stream       (ShellStreamSlider  *self);
void             shell_stream_slider_set_stream       (ShellStreamSlider  *self,
                                                       GvcMixerStream     *stream);
void             shell_stream_slider_set_icons        (ShellStreamSlider  *self,
                                                       const char * const *icons);
const char      *shell_stream_slider_get_icon         (ShellStreamSlider  *self);
double           shell_stream_slider_get_level        (ShellStreamSlider  *self);
double           shell_stream_slider_get_max_level    (ShellStreamSlider  *self);
void             shell_stream_slider_show_osd         (ShellStreamSlider  *self);

/* ------------------------------------------------------------------------ */
/* ShellOutputStreamSlider and ShellInputStreamSlider                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OUTPUT_STREAM_SLIDER (shell_output_stream_slider_get_type ())
G_DECLARE_FINAL_TYPE (ShellOutputStreamSlider, shell_output_stream_slider,
                      SHELL, OUTPUT_STREAM_SLIDER, ShellStreamSlider)

#define SHELL_TYPE_INPUT_STREAM_SLIDER (shell_input_stream_slider_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputStreamSlider, shell_input_stream_slider,
                      SHELL, INPUT_STREAM_SLIDER, ShellStreamSlider)

ShellOutputStreamSlider *shell_output_stream_slider_new (GvcMixerControl *control);
ShellInputStreamSlider  *shell_input_stream_slider_new  (GvcMixerControl *control);

/* ------------------------------------------------------------------------ */
/* ShellVolumeIndicator                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_VOLUME_INDICATOR (shell_volume_indicator_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellVolumeIndicator, shell_volume_indicator,
                          SHELL, VOLUME_INDICATOR, ShellSystemIndicator)

/**
 * ShellVolumeIndicatorClass:
 * @parent_class: the parent class
 * @scroll: called when the status icon is scrolled (the former `_onScroll ()`);
 *   @dy is the vertical scroll delta
 */
struct _ShellVolumeIndicatorClass
{
  ShellSystemIndicatorClass parent_class;

  void (* scroll) (ShellVolumeIndicator *self,
                   ClutterScrollSource   source,
                   double                dx,
                   double                dy);

  /*< private >*/
  gpointer padding[8];
};

StIcon *shell_volume_indicator_get_indicator (ShellVolumeIndicator *self);
void    shell_volume_indicator_handle_scroll (ShellVolumeIndicator *self,
                                              ShellStreamSlider    *item,
                                              double                delta);

/* ------------------------------------------------------------------------ */
/* ShellVolumeOutputIndicator and ShellVolumeInputIndicator                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_VOLUME_OUTPUT_INDICATOR (shell_volume_output_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellVolumeOutputIndicator, shell_volume_output_indicator,
                      SHELL, VOLUME_OUTPUT_INDICATOR, ShellVolumeIndicator)

#define SHELL_TYPE_VOLUME_INPUT_INDICATOR (shell_volume_input_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellVolumeInputIndicator, shell_volume_input_indicator,
                      SHELL, VOLUME_INPUT_INDICATOR, ShellVolumeIndicator)

ShellVolumeOutputIndicator *shell_volume_output_indicator_new (void);
ShellVolumeInputIndicator  *shell_volume_input_indicator_new  (void);

G_END_DECLS
