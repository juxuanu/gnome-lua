/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * main.c: The startup and the singletons of the shell, and the modal stack
 * (`Main.pushModal ()` and `Main.popModal ()`) with the action mode derived
 * from it. Port of js/ui/main.js and of the startup part of js/ui/init.js.
 *
 * shell_main_start () is `start ()` and `_initializeUI ()` of main.js: it
 * creates the singletons of the shell (the session mode, the layout manager,
 * the window manager, the overview, the panel, the message tray, ...), wires
 * them to each other with the signals the C classes emit for what used to be
 * JS-only (`overview-hide-requested`, `window-activated`, ...) and exports
 * the D-Bus services. #ShellMain keeps the objects (the actors are owned by
 * the stage) and releases them in shell_main_shutdown ().
 *
 * What the exports of main.js became:
 *
 *   Main.pushModal (), popModal ()      shell_main_push_modal (), shell_main_pop_modal ()
 *   Main.modalCount, actionMode         the properties of the same names
 *   Main.panel, overview, layoutManager, wm, sessionMode, messageTray,
 *   osdWindowManager, inputMethod, keyboard, xdndHandler, screenshotUI,
 *   brightnessManager, screenShield, extensionManager, lookingGlass, runDialog,
 *   welcomeDialog                       the properties of #ShellMain
 *   Main.notify (), notifyError ()      shell_main_send_notification (), shell_main_notify_error ()
 *   Main.openRunDialog ()               shell_main_open_run_dialog ()
 *   Main.openWelcomeDialog ()           shell_main_open_welcome_dialog ()
 *   Main.createLookingGlass ()          shell_main_create_looking_glass ()
 *   Main.activateWindow ()              shell_main_activate_window ()
 *   Main.moveWindowToMonitorAndWorkspace ()
 *                                       shell_main_move_window_to_monitor_and_workspace ()
 *   Main.loadTheme (), reloadThemeResource (), getThemeStylesheet (),
 *   setThemeStylesheet (), getStyleVariant ()
 *                                       shell_main_load_theme (), ...
 *
 * Differences from main.js:
 *
 *  - `Main.createWorkspacesAdjustment ()` is
 *    shell_window_manager_create_workspaces_adjustment (); `Main.uiGroup` is
 *    shell_layout_manager_get_ui_group ().
 *  - `initializeDeferredWork ()` and `queueDeferredWork ()` are
 *    shell_deferred_work_new () (ui/deferred-work.h).
 *  - `Main.shellDBusService.eval-requested` evaluates the code as Lua in the
 *    privileged inspector state (unsafe mode only), where main.js evaluated
 *    JavaScript. The reply is the rendered result (one line per value) and
 *    not JSON.
 *  - The automation script is not imported here: the test runner of
 *    src/shell-test-runner.c (started from main ()) replaces
 *    js/ui/scripting.js and finds the singletons through #ShellMain; the
 *    startup-complete handler only skips the welcome dialog and the
 *    notification for a privileged user when it is enabled.
 *  - `globalThis.log` and `logError` are gone with the JS global scope.
 *  - The privileged user and screen lock notifications and the welcome dialog
 *    run from the startup-complete handler as before.
 *  - Errors that main.js reported by rejecting `start ()` (no stylesheet, a
 *    resource that does not load) are the #GError of shell_main_start (),
 *    which src/main.c turns into meta_context_terminate_with_error ().
 *
 * The modal stack was ported first; the rest of this file wires them.
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n-lib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/keybindings.h>
#include <meta/meta-backend.h>
#include <meta/meta-context.h>
#include <meta/meta-remote-access-controller.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-app-system.h"
#include "shell-app-usage.h"
#include "shell-global.h"
#include "shell-screenshot.h"
#include "shell-test-runner.h"
#include "shell-util.h"
#include "shell-window-tracker.h"
#include "misc/brightness-manager.h"
#include "misc/break-manager.h"
#include "misc/introspect.h"
#include "misc/login-manager.h"
#include "misc/parental-controls-manager.h"
#include "misc/system-actions.h"
#include "misc/time-limits-manager.h"
#include "shell-lua-inspector.h"
#include "ui/access-dialog.h"
#include "ui/app-favorites.h"
#include "ui/audio-device-selection.h"
#include "ui/background-menu.h"
#include "ui/components.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/dnd.h"
#include "ui/end-session-dialog.h"
#include "ui/extension-system.h"
#include "ui/kbd-a11y-dialog.h"
#include "ui/locate-pointer.h"
#include "ui/looking-glass.h"
#include "ui/magnifier.h"
#include "ui/main.h"
#include "ui/modal-dialog.h"
#include "ui/notification-daemon.h"
#include "ui/osd-monitor-labeler.h"
#include "ui/pad-osd.h"
#include "ui/panel-items.h"
#include "ui/pointer-a11y-timeout.h"
#include "ui/run-dialog.h"
#include "ui/screen-shield.h"
#include "ui/screenshot-ui.h"
#include "ui/shell-dbus.h"
#include "ui/shell-mount-operation-ui.h"
#include "ui/welcome-dialog.h"
#include "ui/window-attention-handler.h"
#include "shell-enum-types.h"

#define WELCOME_DIALOG_LAST_SHOWN_VERSION "welcome-dialog-last-shown-version"
/* Make sure to mention the point release, otherwise it will show every time
 * until this version is current */
#define WELCOME_DIALOG_LAST_TOUR_CHANGE "40.beta"
#define LOG_DOMAIN "GNOME Shell"
#define GNOMESHELL_STARTED_MESSAGE_ID "f3ea493c22934e26811cd62abe8e203a"

enum
{
  PROP_0,

  PROP_ACTION_MODE,
  PROP_MODAL_COUNT,
  PROP_SESSION_MODE,
  PROP_LAYOUT_MANAGER,
  PROP_OSD_WINDOW_MANAGER,
  PROP_PANEL,
  PROP_MESSAGE_TRAY,
  PROP_WINDOW_MANAGER,
  PROP_INPUT_METHOD,
  PROP_KEYBOARD,
  PROP_OVERVIEW,
  PROP_XDND_HANDLER,
  PROP_CTRL_ALT_TAB_MANAGER,
  PROP_OSD_MONITOR_LABELER,
  PROP_SCREENSHOT_UI,
  PROP_BRIGHTNESS_MANAGER,
  PROP_TIME_LIMITS_MANAGER,
  PROP_SCREEN_SHIELD,
  PROP_EXTENSION_MANAGER,
  PROP_LOOKING_GLASS,
  PROP_RUN_DIALOG,
  PROP_WELCOME_DIALOG,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  MODAL_ENDED,
  SYSTEM_MODAL_OPENED,
  SETTINGS_APP_ACTIVATING,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/*
 * One entry of the former `modalActorFocusStack`: what a pushModal ()
 * invocation recorded so that popModal () can undo it. The records are
 * never moved in memory, popping a record from the middle of the stack
 * shifts the focus chain between them instead.
 */
typedef struct
{
  ShellMain *main;

  ClutterActor *actor;            /* strong, as the JS record held it */
  ClutterGrab *grab;              /* strong */
  gulong destroy_id;              /* "destroy" handler on @actor */

  ClutterActor *prev_focus;       /* strong, nullable */
  gulong prev_focus_destroy_id;   /* "destroy" handler on @prev_focus */

  ShellActionMode action_mode;    /* the action mode before this push */
} ModalRecord;

struct _ShellMain
{
  GObject parent;

  GPtrArray *modal_stack;         /* of ModalRecord */
  guint modal_count;
  ShellActionMode action_mode;
  ShellSessionMode *session_mode;
  ShellLayoutManager *layout_manager;
  ShellOsdWindowManager *osd_window_manager;
  ShellPanel *panel;              /* weak */
  ShellMessageTray *message_tray; /* weak */
  ShellWindowManager *window_manager; /* weak */
  ShellInputMethod *input_method; /* weak */
  ShellOskManager *keyboard;      /* weak */
  ShellOverview *overview;        /* weak */
  MetaCompositor *compositor;     /* weak; same reason as @stage */
  ClutterStage *stage;            /* weak; set on the first push_modal so that
                                   * pops during shell teardown (after ShellGlobal
                                   * is gone) still work */

  /* Created by shell_main_start () */
  GPtrArray *owned;               /* of GObject, strong, in creation order;
                                   * released in reverse by shell_main_shutdown ().
                                   * Actors are not in it: the stage owns them */
  gboolean started;
  gboolean shutting_down;
  GCancellable *cancellable;      /* cancelled by shell_main_shutdown () */
  ShellXdndHandler *xdnd_handler;       /* weak */
  ShellCtrlAltTabManager *ctrl_alt_tab_manager; /* weak */
  ShellOsdMonitorLabeler *osd_monitor_labeler; /* weak */
  ShellScreenshotUI *screenshot_ui;     /* weak */
  ShellBrightnessManager *brightness_manager; /* weak */
  ShellTimeLimitsManager *time_limits_manager; /* weak */
  ShellScreenShield *screen_shield;     /* weak */
  ShellExtensionManager *extension_manager; /* weak */
  ShellLookingGlass *looking_glass;     /* weak */
  ShellRunDialog *run_dialog;           /* weak */
  ShellWelcomeDialog *welcome_dialog;   /* weak */
  ShellLocatePointer *locate_pointer;   /* weak */

  GSettings *keybindings_settings;      /* strong, for 'focus-active-notification' */

  /* The theme: _cssStylesheet, _defaultCssStylesheet and the resources */
  GFile *css_stylesheet;
  GFile *default_css_stylesheet;
  GResource *theme_resource;
  GResource *icon_resource;
  GResource *osk_resource;

  gboolean remote_access_inhibited;
  gboolean forwarding_system_modal_opened;
  gulong screenshot_overview_hidden_id;
  GDateTime *start_date;
  guint notifications_watch_id;

  /* AnimationsSettings */
  gboolean animations_enabled;
  GHashTable *remote_access_handles;    /* of MetaRemoteAccessHandle, strong */
};

G_DEFINE_FINAL_TYPE (ShellMain, shell_main, G_TYPE_OBJECT)

static void
modal_record_free (ModalRecord *record)
{
  if (record->destroy_id != 0 && record->actor != NULL)
    g_signal_handler_disconnect (record->actor, record->destroy_id);

  if (record->prev_focus != NULL && record->prev_focus_destroy_id != 0)
    g_signal_handler_disconnect (record->prev_focus, record->prev_focus_destroy_id);

  g_clear_object (&record->prev_focus);
  g_clear_object (&record->grab);
  g_clear_object (&record->actor);
  g_free (record);
}

/* _findModal () */
static int
find_modal (ShellMain   *self,
            ClutterGrab *grab)
{
  guint i;

  if (grab == NULL)
    return -1;

  for (i = 0; i < self->modal_stack->len; i++)
    {
      ModalRecord *record = g_ptr_array_index (self->modal_stack, i);

      if (record->grab == grab)
        return i;
    }

  return -1;
}

static void
set_modal_count (ShellMain *self,
                 guint      modal_count)
{
  if (self->modal_count == modal_count)
    return;

  self->modal_count = modal_count;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MODAL_COUNT]);
}

static void
prev_focus_destroyed_cb (ClutterActor *prev_focus,
                         ShellMain    *self)
{
  guint i;

  /* Only the first record is updated, like Array.findIndex (): every record
   * that recorded this actor has a handler of its own. */
  for (i = 0; i < self->modal_stack->len; i++)
    {
      ModalRecord *record = g_ptr_array_index (self->modal_stack, i);

      if (record->prev_focus == prev_focus)
        {
          /* The handler stays connected to the dying actor; it is not
           * disconnected later because prev_focus is now NULL. */
          record->prev_focus_destroy_id = 0;
          g_clear_object (&record->prev_focus);
          break;
        }
    }
}

static void
modal_actor_destroyed_cb (ClutterActor *actor,
                          ModalRecord  *record)
{
  ShellMain *self = record->main;
  g_autoptr (ClutterGrab) grab = g_object_ref (record->grab);
  g_autoptr (GError) error = NULL;

  if (find_modal (self, grab) >= 0 &&
      !shell_main_pop_modal (self, grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
set_action_mode (ShellMain       *self,
                 ShellActionMode  action_mode)
{
  if (self->action_mode == action_mode)
    return;

  self->action_mode = action_mode;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTION_MODE]);
}

static void
shell_main_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  ShellMain *self = SHELL_MAIN (object);

  switch (prop_id)
    {
    case PROP_ACTION_MODE:
      g_value_set_flags (value, self->action_mode);
      break;

    case PROP_MODAL_COUNT:
      g_value_set_uint (value, self->modal_count);
      break;

    case PROP_SESSION_MODE:
      g_value_set_object (value, self->session_mode);
      break;

    case PROP_LAYOUT_MANAGER:
      g_value_set_object (value, self->layout_manager);
      break;

    case PROP_OSD_WINDOW_MANAGER:
      g_value_set_object (value, self->osd_window_manager);
      break;

    case PROP_PANEL:
      g_value_set_object (value, self->panel);
      break;

    case PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    case PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case PROP_INPUT_METHOD:
      g_value_set_object (value, self->input_method);
      break;

    case PROP_KEYBOARD:
      g_value_set_object (value, self->keyboard);
      break;

    case PROP_OVERVIEW:
      g_value_set_object (value, self->overview);
      break;

    case PROP_XDND_HANDLER:
      g_value_set_object (value, self->xdnd_handler);
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      g_value_set_object (value, self->ctrl_alt_tab_manager);
      break;

    case PROP_OSD_MONITOR_LABELER:
      g_value_set_object (value, self->osd_monitor_labeler);
      break;

    case PROP_SCREENSHOT_UI:
      g_value_set_object (value, self->screenshot_ui);
      break;

    case PROP_BRIGHTNESS_MANAGER:
      g_value_set_object (value, self->brightness_manager);
      break;

    case PROP_TIME_LIMITS_MANAGER:
      g_value_set_object (value, self->time_limits_manager);
      break;

    case PROP_SCREEN_SHIELD:
      g_value_set_object (value, self->screen_shield);
      break;

    case PROP_EXTENSION_MANAGER:
      g_value_set_object (value, self->extension_manager);
      break;

    case PROP_LOOKING_GLASS:
      g_value_set_object (value, self->looking_glass);
      break;

    case PROP_RUN_DIALOG:
      g_value_set_object (value, self->run_dialog);
      break;

    case PROP_WELCOME_DIALOG:
      g_value_set_object (value, self->welcome_dialog);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_main_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  ShellMain *self = SHELL_MAIN (object);

  switch (prop_id)
    {
    case PROP_ACTION_MODE:
      shell_main_set_action_mode (self, g_value_get_flags (value));
      break;

    case PROP_SESSION_MODE:
      shell_main_set_session_mode (self, g_value_get_object (value));
      break;

    case PROP_LAYOUT_MANAGER:
      shell_main_set_layout_manager (self, g_value_get_object (value));
      break;

    case PROP_OSD_WINDOW_MANAGER:
      shell_main_set_osd_window_manager (self, g_value_get_object (value));
      break;

    case PROP_PANEL:
      shell_main_set_panel (self, g_value_get_object (value));
      break;

    case PROP_MESSAGE_TRAY:
      shell_main_set_message_tray (self, g_value_get_object (value));
      break;

    case PROP_WINDOW_MANAGER:
      shell_main_set_window_manager (self, g_value_get_object (value));
      break;

    case PROP_INPUT_METHOD:
      shell_main_set_input_method (self, g_value_get_object (value));
      break;

    case PROP_KEYBOARD:
      shell_main_set_keyboard (self, g_value_get_object (value));
      break;

    case PROP_OVERVIEW:
      shell_main_set_overview (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_main_dispose (GObject *object)
{
  ShellMain *self = SHELL_MAIN (object);

  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->compositor);
  g_clear_weak_pointer (&self->panel);
  g_clear_weak_pointer (&self->message_tray);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->input_method);
  g_clear_weak_pointer (&self->keyboard);
  g_clear_weak_pointer (&self->overview);
  g_clear_weak_pointer (&self->xdnd_handler);
  g_clear_weak_pointer (&self->ctrl_alt_tab_manager);
  g_clear_weak_pointer (&self->osd_monitor_labeler);
  g_clear_weak_pointer (&self->screenshot_ui);
  g_clear_weak_pointer (&self->brightness_manager);
  g_clear_weak_pointer (&self->time_limits_manager);
  g_clear_weak_pointer (&self->screen_shield);
  g_clear_weak_pointer (&self->extension_manager);
  g_clear_weak_pointer (&self->looking_glass);
  g_clear_weak_pointer (&self->run_dialog);
  g_clear_weak_pointer (&self->welcome_dialog);
  g_clear_weak_pointer (&self->locate_pointer);
  g_clear_pointer (&self->owned, g_ptr_array_unref);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->keybindings_settings);
  g_clear_object (&self->css_stylesheet);
  g_clear_object (&self->default_css_stylesheet);
  g_clear_pointer (&self->theme_resource, g_resource_unref);
  g_clear_pointer (&self->icon_resource, g_resource_unref);
  g_clear_pointer (&self->osk_resource, g_resource_unref);
  g_clear_pointer (&self->start_date, g_date_time_unref);
  g_clear_pointer (&self->remote_access_handles, g_hash_table_unref);
  g_clear_pointer (&self->modal_stack, g_ptr_array_unref);
  g_clear_object (&self->session_mode);
  g_clear_object (&self->layout_manager);
  g_clear_object (&self->osd_window_manager);

  G_OBJECT_CLASS (shell_main_parent_class)->dispose (object);
}

static void
shell_main_class_init (ShellMainClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_main_dispose;
  object_class->get_property = shell_main_get_property;
  object_class->set_property = shell_main_set_property;

  /**
   * ShellMain:action-mode:
   *
   * The current #ShellActionMode, used to filter global keybindings and
   * gestures (`Main.actionMode`). It is the mode of the topmost modal, and
   * %SHELL_ACTION_MODE_NORMAL once the modal stack is empty. It starts as
   * %SHELL_ACTION_MODE_NONE until the startup is complete, when the shell
   * sets it to %SHELL_ACTION_MODE_NORMAL. Notified whenever it changes.
   */
  props[PROP_ACTION_MODE] =
    g_param_spec_flags ("action-mode", NULL, NULL,
                        SHELL_TYPE_ACTION_MODE, SHELL_ACTION_MODE_NONE,
                        G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:modal-count:
   *
   * The number of modals currently pushed (`Main.modalCount`).
   */
  props[PROP_MODAL_COUNT] =
    g_param_spec_uint ("modal-count", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READABLE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:session-mode:
   *
   * The session mode of the shell (`Main.sessionMode`). The startup sets it
   * right after creating it, so that the ported UI code (the popup menus)
   * can follow it.
   */
  props[PROP_SESSION_MODE] =
    g_param_spec_object ("session-mode", NULL, NULL,
                         SHELL_TYPE_SESSION_MODE,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:layout-manager:
   *
   * The layout manager of the shell (`Main.layoutManager`). The startup sets it
   * right after creating it, so that the ported UI code can reach the
   * monitors and the chrome.
   */
  props[PROP_LAYOUT_MANAGER] =
    g_param_spec_object ("layout-manager", NULL, NULL,
                         SHELL_TYPE_LAYOUT_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:osd-window-manager:
   *
   * The OSD window manager of the shell (`Main.osdWindowManager`). The startup
   * sets it right after creating it, so that the switcher popups can hide
   * the OSDs when they appear.
   */
  props[PROP_OSD_WINDOW_MANAGER] =
    g_param_spec_object ("osd-window-manager", NULL, NULL,
                         SHELL_TYPE_OSD_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:panel:
   *
   * The top bar of the shell (`Main.panel`). The startup sets it right after
   * creating it, so that the window manager can toggle and close the
   * calendar and the quick settings and read the height of the bar. Held
   * weakly: the panel is owned by the stage.
   */
  props[PROP_PANEL] =
    g_param_spec_object ("panel", NULL, NULL,
                         SHELL_TYPE_PANEL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:message-tray:
   *
   * The message tray of the shell (`Main.messageTray`). The startup sets it right
   * after creating it, so that the date menu can show its notifications.
   * Held weakly: the tray is owned by the stage.
   */
  props[PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:window-manager:
   *
   * The window manager of the shell (`Main.wm`). The startup sets it right after
   * creating it, so that the input source manager can add its key bindings.
   * Held weakly.
   */
  props[PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:input-method:
   *
   * The input method of the compositor (`Main.inputMethod`). The startup sets it
   * right after creating it, so that the input source manager can tell it
   * whether there is an input source. Held weakly.
   */
  props[PROP_INPUT_METHOD] =
    g_param_spec_object ("input-method", NULL, NULL,
                         SHELL_TYPE_INPUT_METHOD,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:keyboard:
   *
   * The on-screen keyboard manager (`Main.keyboard`). The startup sets it right
   * after creating it, so that the IBus candidate popup can feed the
   * suggestions of the keyboard. Held weakly.
   */
  props[PROP_KEYBOARD] =
    g_param_spec_object ("keyboard", NULL, NULL,
                         SHELL_TYPE_OSK_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:overview:
   *
   * The overview of the shell (`Main.overview`). The startup sets it right after
   * creating it, so that the test runner (and Looking Glass) can reach it
   * through `main`. Held weakly.
   */
  props[PROP_OVERVIEW] =
    g_param_spec_object ("overview", NULL, NULL,
                         SHELL_TYPE_OVERVIEW,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:xdnd-handler:
   *
   * The handler of the drags that come from other applications
   * (`Main.xdndHandler`). Held weakly.
   */
  props[PROP_XDND_HANDLER] =
    g_param_spec_object ("xdnd-handler", NULL, NULL,
                         SHELL_TYPE_XDND_HANDLER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:ctrl-alt-tab-manager:
   *
   * The Ctrl+Alt+Tab focus groups (`Main.ctrlAltTabManager`). Held weakly.
   */
  props[PROP_CTRL_ALT_TAB_MANAGER] =
    g_param_spec_object ("ctrl-alt-tab-manager", NULL, NULL,
                         SHELL_TYPE_CTRL_ALT_TAB_MANAGER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:osd-monitor-labeler:
   *
   * The labels that identify the monitors (`Main.osdMonitorLabeler`). Held
   * weakly.
   */
  props[PROP_OSD_MONITOR_LABELER] =
    g_param_spec_object ("osd-monitor-labeler", NULL, NULL,
                         SHELL_TYPE_OSD_MONITOR_LABELER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:screenshot-ui:
   *
   * The screenshot and screen recording UI (`Main.screenshotUI`). Held
   * weakly.
   */
  props[PROP_SCREENSHOT_UI] =
    g_param_spec_object ("screenshot-ui", NULL, NULL,
                         SHELL_TYPE_SCREENSHOT_UI,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:brightness-manager:
   *
   * The brightness manager (`Main.brightnessManager`). Held weakly.
   */
  props[PROP_BRIGHTNESS_MANAGER] =
    g_param_spec_object ("brightness-manager", NULL, NULL,
                         SHELL_TYPE_BRIGHTNESS_MANAGER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:time-limits-manager:
   *
   * The manager of the screen time limits (`Main.timeLimitsManager`). Held
   * weakly.
   */
  props[PROP_TIME_LIMITS_MANAGER] =
    g_param_spec_object ("time-limits-manager", NULL, NULL,
                         SHELL_TYPE_TIME_LIMITS_MANAGER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:screen-shield:
   *
   * The screen shield (`Main.screenShield`), %NULL without a display manager
   * that can lock the session. Held weakly.
   */
  props[PROP_SCREEN_SHIELD] =
    g_param_spec_object ("screen-shield", NULL, NULL,
                         SHELL_TYPE_SCREEN_SHIELD,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:extension-manager:
   *
   * The manager of the Lua extensions (`Main.extensionManager`). Held weakly.
   */
  props[PROP_EXTENSION_MANAGER] =
    g_param_spec_object ("extension-manager", NULL, NULL,
                         SHELL_TYPE_EXTENSION_MANAGER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:looking-glass:
   *
   * Looking Glass (`Main.lookingGlass`), %NULL until it is first opened. Held
   * weakly.
   */
  props[PROP_LOOKING_GLASS] =
    g_param_spec_object ("looking-glass", NULL, NULL,
                         SHELL_TYPE_LOOKING_GLASS,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:run-dialog:
   *
   * The run dialog (`Main.runDialog`), %NULL until it is first opened. Held
   * weakly.
   */
  props[PROP_RUN_DIALOG] =
    g_param_spec_object ("run-dialog", NULL, NULL,
                         SHELL_TYPE_RUN_DIALOG,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:welcome-dialog:
   *
   * The welcome dialog (`Main.welcomeDialog`), %NULL until it is first
   * opened. Held weakly.
   */
  props[PROP_WELCOME_DIALOG] =
    g_param_spec_object ("welcome-dialog", NULL, NULL,
                         SHELL_TYPE_WELCOME_DIALOG,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellMain::modal-ended:
   * @self: the #ShellMain
   *
   * Emitted when the last modal was popped, after the modal count has
   * dropped to 0 and the grab has been dismissed, and before the action mode
   * goes back to %SHELL_ACTION_MODE_NORMAL (`layoutManager.modal_ended ()`).
   */
  signals[MODAL_ENDED] =
    g_signal_new ("modal-ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellMain::system-modal-opened:
   * @self: the #ShellMain
   *
   * Emitted when a system modal dialog opens, which dismisses the popup
   * menus. ShellMain forwards the signal of the same name of the layout
   * manager (`layoutManager.emit ('system-modal-opened')`) by calling
   * shell_main_system_modal_opened ().
   */
  signals[SYSTEM_MODAL_OPENED] =
    g_signal_new ("system-modal-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellMain::settings-app-activating:
   * @self: the #ShellMain
   *
   * Emitted by the settings items of the popup menus before they activate
   * the settings application. ShellMain hides the overview and closes the
   * quick settings in response (`Main.overview.hide ()` and
   * `Main.panel.closeQuickSettings ()`), which are not reachable from C.
   */
  signals[SETTINGS_APP_ACTIVATING] =
    g_signal_new ("settings-app-activating",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_main_init (ShellMain *self)
{
  self->modal_stack = g_ptr_array_new_with_free_func ((GDestroyNotify) modal_record_free);
  self->action_mode = SHELL_ACTION_MODE_NONE;
  self->owned = g_ptr_array_new_with_free_func (g_object_unref);
  self->cancellable = g_cancellable_new ();
  self->animations_enabled = TRUE;
  self->remote_access_handles = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                       g_object_unref, NULL);
}

/**
 * shell_main_get_default:
 *
 * Gets the modal stack of the shell.
 *
 * Returns: (transfer none): the #ShellMain singleton
 */
ShellMain *
shell_main_get_default (void)
{
  static ShellMain *instance = NULL;

  if (instance == NULL)
    instance = g_object_new (SHELL_TYPE_MAIN, NULL);

  return instance;
}

/**
 * shell_main_push_modal:
 * @self: a #ShellMain
 * @actor: actor which will be given keyboard focus
 * @action_mode: used to set the current #ShellActionMode to filter global
 *   keybindings; %SHELL_ACTION_MODE_NONE filters out all keybindings
 *
 * Ensure we are in a mode where all keyboard and mouse input goes to the
 * stage, and focus @actor. Multiple calls to this function act in a stacking
 * fashion; the effect will be undone when an equal number of
 * shell_main_pop_modal () invocations have been made.
 *
 * Next, record the current Clutter keyboard focus on a stack. If the modal
 * stack returns to this actor, reset the focus to the actor which was focused
 * at the time shell_main_push_modal () was invoked.
 *
 * The modal is popped automatically if @actor is destroyed.
 *
 * Returns: (transfer none): the grab handle created. It is owned by the modal
 *   stack until it is passed to shell_main_pop_modal (); keep a reference to
 *   it if it is needed afterwards. `Main.pushModal ()` returned the same
 *   grab.
 */
ClutterGrab *
shell_main_push_modal (ShellMain       *self,
                       ClutterActor    *actor,
                       ShellActionMode  action_mode)
{
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  ClutterActor *prev_focus;
  ModalRecord *record;
  ClutterGrab *grab;

  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  if (self->stage != stage)
    g_set_weak_pointer (&self->stage, stage);
  if (self->compositor != shell_global_get_compositor (global))
    g_set_weak_pointer (&self->compositor, shell_global_get_compositor (global));
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  grab = clutter_stage_grab (stage, actor);

  if (self->modal_count == 0)
    meta_compositor_disable_unredirect (shell_global_get_compositor (global));

  set_modal_count (self, self->modal_count + 1);

  record = g_new0 (ModalRecord, 1);
  record->main = self;
  record->actor = g_object_ref (actor);
  record->grab = grab;
  record->destroy_id = g_signal_connect (actor, "destroy",
                                         G_CALLBACK (modal_actor_destroyed_cb),
                                         record);

  prev_focus = clutter_stage_get_key_focus (stage);
  if (prev_focus != NULL)
    {
      record->prev_focus = g_object_ref (prev_focus);
      record->prev_focus_destroy_id =
        g_signal_connect (prev_focus, "destroy",
                          G_CALLBACK (prev_focus_destroyed_cb), self);
    }

  record->action_mode = self->action_mode;
  g_ptr_array_add (self->modal_stack, record);

  set_action_mode (self, action_mode);
  clutter_stage_set_key_focus (stage,
                               actor == CLUTTER_ACTOR (stage) ? NULL : actor);

  return grab;
}

/**
 * shell_main_pop_modal:
 * @self: a #ShellMain
 * @grab: (nullable): the grab given by shell_main_push_modal ()
 * @error: return location for a #GError
 *
 * Reverse the effect of shell_main_push_modal (). If this invocation is
 * undoing the topmost invocation, then the focus will be restored to the
 * previous focus at the time when shell_main_push_modal () was invoked.
 *
 * If @grab is not on the modal stack, the key focus is cleared, the action
 * mode goes back to %SHELL_ACTION_MODE_NORMAL and @error is set (`Main.popModal ()`
 * threw an "incorrect pop" error).
 *
 * Returns: %TRUE if the modal was popped
 */
gboolean
shell_main_pop_modal (ShellMain    *self,
                      ClutterGrab  *grab,
                      GError      **error)
{
  ClutterStage *stage = self->stage;
  g_autoptr (ClutterGrab) grab_ref = NULL;
  g_autoptr (ClutterActor) prev_focus = NULL;
  ModalRecord *record;
  ShellActionMode record_action_mode;
  gboolean is_top;
  guint index;
  int focus_index;

  g_return_val_if_fail (SHELL_IS_MAIN (self), FALSE);

  if (stage == NULL)
    {
      /* Nothing was ever pushed, or the stage is already gone (shell
       * teardown): there is nothing to restore. */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "incorrect pop");
      return FALSE;
    }

  /* The record can go away while the grab is being dismissed */
  if (grab != NULL)
    grab_ref = g_object_ref (grab);

  focus_index = find_modal (self, grab);
  if (focus_index < 0)
    {
      clutter_stage_set_key_focus (stage, NULL);
      set_action_mode (self, SHELL_ACTION_MODE_NORMAL);

      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "incorrect pop");
      return FALSE;
    }

  set_modal_count (self, self->modal_count - 1);

  record = g_ptr_array_index (self->modal_stack, focus_index);
  g_signal_handler_disconnect (record->actor, record->destroy_id);
  record->destroy_id = 0;

  clutter_grab_dismiss (record->grab);

  is_top = (guint) focus_index == self->modal_stack->len - 1;
  if (is_top)
    {
      if (record->prev_focus != NULL)
        {
          g_signal_handler_disconnect (record->prev_focus,
                                       record->prev_focus_destroy_id);
          record->prev_focus_destroy_id = 0;
        }

      /* record->prev_focus is kept (and still owned by the record) until the
       * record is removed below, like the JS record kept it */
      prev_focus = record->prev_focus != NULL ?
        g_object_ref (record->prev_focus) : NULL;
      record_action_mode = record->action_mode;

      set_action_mode (self, record_action_mode);
      clutter_stage_set_key_focus (stage, prev_focus);
    }
  else
    {
      ModalRecord *top;

      /* If we have:
       *     global.stage.set_focus (a);
       *     push_modal (b);
       *     push_modal (c);
       *     push_modal (d);
       *
       * then we have the stack:
       *     [{ prev_focus: a, actor: b },
       *      { prev_focus: b, actor: c },
       *      { prev_focus: c, actor: d }]
       *
       * When actor c is destroyed/popped, if we only simply remove the
       * record, then the focus stack will be [a, c], rather than the correct
       * [a, b]. Shift the focus stack up before removing the record to ensure
       * that we get the correct result.
       */
      top = g_ptr_array_index (self->modal_stack, self->modal_stack->len - 1);
      if (top->prev_focus != NULL)
        {
          g_signal_handler_disconnect (top->prev_focus,
                                       top->prev_focus_destroy_id);
          top->prev_focus_destroy_id = 0;
          g_clear_object (&top->prev_focus);
        }

      /* Remove from the middle, shift the focus chain up. The prev_focus of
       * the record being removed moves to the record above it. */
      for (index = self->modal_stack->len - 1; index > (guint) focus_index; index--)
        {
          ModalRecord *cur = g_ptr_array_index (self->modal_stack, index);
          ModalRecord *below = g_ptr_array_index (self->modal_stack, index - 1);

          cur->prev_focus = below->prev_focus;
          cur->prev_focus_destroy_id = below->prev_focus_destroy_id;
          cur->action_mode = below->action_mode;

          below->prev_focus = NULL;
          below->prev_focus_destroy_id = 0;
        }
    }

  /* The set_key_focus () above can run arbitrary handlers: look the record up
   * again instead of trusting the index. */
  if (g_ptr_array_find (self->modal_stack, record, &index))
    g_ptr_array_remove_index (self->modal_stack, index);

  if (self->modal_count > 0)
    return TRUE;

  g_signal_emit (self, signals[MODAL_ENDED], 0);
  if (self->compositor != NULL)
    meta_compositor_enable_unredirect (self->compositor);
  set_action_mode (self, SHELL_ACTION_MODE_NORMAL);

  return TRUE;
}

/**
 * shell_main_get_modal_count:
 * @self: a #ShellMain
 *
 * Returns: the number of modals currently pushed
 */
guint
shell_main_get_modal_count (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), 0);

  return self->modal_count;
}

/**
 * shell_main_get_action_mode:
 * @self: a #ShellMain
 *
 * Returns: the current action mode
 */
ShellActionMode
shell_main_get_action_mode (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), SHELL_ACTION_MODE_NONE);

  return self->action_mode;
}

/**
 * shell_main_set_action_mode:
 * @self: a #ShellMain
 * @action_mode: the new action mode
 *
 * Sets the action mode directly. The modal stack manages the action mode on
 * its own; this is only for the shell to leave %SHELL_ACTION_MODE_NONE once
 * the startup is complete.
 */
void
shell_main_set_action_mode (ShellMain       *self,
                            ShellActionMode  action_mode)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  set_action_mode (self, action_mode);
}

/**
 * shell_main_get_session_mode:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the session mode, once the startup has
 *   created it
 */
ShellSessionMode *
shell_main_get_session_mode (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->session_mode;
}

/**
 * shell_main_set_session_mode:
 * @self: a #ShellMain
 * @session_mode: (nullable): the session mode of the shell
 *
 * Sets #ShellMain:session-mode. Menus which already exist keep following the
 * session mode they were created with.
 */
void
shell_main_set_session_mode (ShellMain        *self,
                             ShellSessionMode *session_mode)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (session_mode == NULL || SHELL_IS_SESSION_MODE (session_mode));

  if (g_set_object (&self->session_mode, session_mode))
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SESSION_MODE]);
}

/**
 * shell_main_get_layout_manager:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the layout manager, once the startup has
 *   created it
 */
ShellLayoutManager *
shell_main_get_layout_manager (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->layout_manager;
}

/**
 * shell_main_set_layout_manager:
 * @self: a #ShellMain
 * @layout_manager: (nullable): the layout manager of the shell
 *
 * Sets #ShellMain:layout-manager.
 */
void
shell_main_set_layout_manager (ShellMain          *self,
                               ShellLayoutManager *layout_manager)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (layout_manager == NULL || SHELL_IS_LAYOUT_MANAGER (layout_manager));

  if (g_set_object (&self->layout_manager, layout_manager))
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAYOUT_MANAGER]);
}

/**
 * shell_main_get_osd_window_manager:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the OSD window manager, once the startup
 *   has created it
 */
ShellOsdWindowManager *
shell_main_get_osd_window_manager (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->osd_window_manager;
}

/**
 * shell_main_set_osd_window_manager:
 * @self: a #ShellMain
 * @osd_window_manager: (nullable): the OSD window manager of the shell
 *
 * Sets #ShellMain:osd-window-manager.
 */
void
shell_main_set_osd_window_manager (ShellMain             *self,
                                   ShellOsdWindowManager *osd_window_manager)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (osd_window_manager == NULL ||
                    SHELL_IS_OSD_WINDOW_MANAGER (osd_window_manager));

  if (g_set_object (&self->osd_window_manager, osd_window_manager))
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OSD_WINDOW_MANAGER]);
}

/**
 * shell_main_get_panel:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the top bar, once the startup has created
 *   it
 */
ShellPanel *
shell_main_get_panel (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->panel;
}

/**
 * shell_main_set_panel:
 * @self: a #ShellMain
 * @panel: (nullable): the top bar of the shell
 *
 * Sets #ShellMain:panel.
 */
void
shell_main_set_panel (ShellMain  *self,
                      ShellPanel *panel)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (panel == NULL || SHELL_IS_PANEL (panel));

  if (self->panel == panel)
    return;

  g_clear_weak_pointer (&self->panel);
  if (panel != NULL)
    g_set_weak_pointer (&self->panel, panel);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PANEL]);
}

/**
 * shell_main_get_message_tray:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the message tray, once the startup has
 *   created it
 */
ShellMessageTray *
shell_main_get_message_tray (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->message_tray;
}

/**
 * shell_main_set_message_tray:
 * @self: a #ShellMain
 * @message_tray: (nullable): the message tray of the shell
 *
 * Sets #ShellMain:message-tray.
 */
void
shell_main_set_message_tray (ShellMain        *self,
                             ShellMessageTray *message_tray)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (message_tray == NULL || SHELL_IS_MESSAGE_TRAY (message_tray));

  if (self->message_tray == message_tray)
    return;

  g_clear_weak_pointer (&self->message_tray);
  if (message_tray != NULL)
    g_set_weak_pointer (&self->message_tray, message_tray);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MESSAGE_TRAY]);
}

/**
 * shell_main_get_window_manager:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the window manager, once the startup has
 *   created it
 */
ShellWindowManager *
shell_main_get_window_manager (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->window_manager;
}

/**
 * shell_main_set_window_manager:
 * @self: a #ShellMain
 * @window_manager: (nullable): the window manager of the shell
 *
 * Sets #ShellMain:window-manager.
 */
void
shell_main_set_window_manager (ShellMain          *self,
                               ShellWindowManager *window_manager)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (window_manager == NULL || SHELL_IS_WINDOW_MANAGER (window_manager));

  if (self->window_manager == window_manager)
    return;

  g_clear_weak_pointer (&self->window_manager);
  if (window_manager != NULL)
    g_set_weak_pointer (&self->window_manager, window_manager);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_WINDOW_MANAGER]);
}

/**
 * shell_main_get_input_method:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the input method, once the startup has
 *   created it
 */
ShellInputMethod *
shell_main_get_input_method (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->input_method;
}

/**
 * shell_main_set_input_method:
 * @self: a #ShellMain
 * @input_method: (nullable): the input method of the compositor
 *
 * Sets #ShellMain:input-method.
 */
void
shell_main_set_input_method (ShellMain        *self,
                             ShellInputMethod *input_method)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (input_method == NULL || SHELL_IS_INPUT_METHOD (input_method));

  if (self->input_method == input_method)
    return;

  g_clear_weak_pointer (&self->input_method);
  if (input_method != NULL)
    g_set_weak_pointer (&self->input_method, input_method);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_INPUT_METHOD]);
}

/**
 * shell_main_get_keyboard:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the on-screen keyboard manager, once
 *   the startup has created it
 */
ShellOskManager *
shell_main_get_keyboard (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->keyboard;
}

/**
 * shell_main_set_keyboard:
 * @self: a #ShellMain
 * @keyboard: (nullable): the on-screen keyboard manager of the shell
 *
 * Sets #ShellMain:keyboard.
 */
void
shell_main_set_keyboard (ShellMain       *self,
                         ShellOskManager *keyboard)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (keyboard == NULL || SHELL_IS_OSK_MANAGER (keyboard));

  if (self->keyboard == keyboard)
    return;

  g_clear_weak_pointer (&self->keyboard);
  if (keyboard != NULL)
    g_set_weak_pointer (&self->keyboard, keyboard);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_KEYBOARD]);
}

/**
 * shell_main_get_overview:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the overview, once the startup has
 *   created it
 */
ShellOverview *
shell_main_get_overview (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->overview;
}

/**
 * shell_main_set_overview:
 * @self: a #ShellMain
 * @overview: (nullable): the overview of the shell
 *
 * Sets #ShellMain:overview.
 */
void
shell_main_set_overview (ShellMain     *self,
                         ShellOverview *overview)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (overview == NULL || SHELL_IS_OVERVIEW (overview));

  if (self->overview == overview)
    return;

  g_clear_weak_pointer (&self->overview);
  if (overview != NULL)
    g_set_weak_pointer (&self->overview, overview);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERVIEW]);
}

/**
 * shell_main_system_modal_opened:
 * @self: a #ShellMain
 *
 * Emits #ShellMain::system-modal-opened.
 */
void
shell_main_system_modal_opened (ShellMain *self)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  g_signal_emit (self, signals[SYSTEM_MODAL_OPENED], 0);
}

/**
 * shell_main_emit_settings_app_activating:
 * @self: a #ShellMain
 *
 * Emits #ShellMain::settings-app-activating.
 */
void
shell_main_emit_settings_app_activating (ShellMain *self)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  g_signal_emit (self, signals[SETTINGS_APP_ACTIVATING], 0);
}

/* ======================================================================== */
/* What `Main.overview.hide ()` and `Main.panel.closeCalendar ()` were       */
/* ======================================================================== */

/* The handlers of the signals that the C classes emit to ask the shell to
 * hide the overview. They are connected with G_CONNECT_SWAPPED: @self is the
 * first argument and the arguments of the signal are ignored. */
static void
main_hide_overview (ShellMain *self)
{
  if (self->overview != NULL)
    shell_overview_hide (self->overview);
}

/* Keeps @object (a new reference) until shell_main_shutdown () */
static gpointer
main_own (ShellMain *self,
          gpointer   object)
{
  g_ptr_array_add (self->owned, object);

  return object;
}

/* What `Main.activateWindow ()` does on top of activating the window, and
 * what the notification sources did on top of opening an application */
static void
main_hide_overview_and_close_calendar (ShellMain *self)
{
  main_hide_overview (self);

  if (self->panel != NULL)
    shell_panel_close_calendar (self->panel);
}

/* ======================================================================== */
/* The singletons                                                           */
/* ======================================================================== */

/**
 * shell_main_get_xdnd_handler:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the handler of the drags that come
 *   from other applications (`Main.xdndHandler`), once the shell started
 */
ShellXdndHandler *
shell_main_get_xdnd_handler (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->xdnd_handler;
}

/**
 * shell_main_get_ctrl_alt_tab_manager:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the Ctrl+Alt+Tab focus groups
 *   (`Main.ctrlAltTabManager`), once the shell started
 */
ShellCtrlAltTabManager *
shell_main_get_ctrl_alt_tab_manager (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->ctrl_alt_tab_manager;
}

/**
 * shell_main_get_screenshot_ui:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the screenshot and screen recording UI
 *   (`Main.screenshotUI`), once the shell started
 */
ShellScreenshotUI *
shell_main_get_screenshot_ui (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->screenshot_ui;
}

/**
 * shell_main_get_brightness_manager:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the brightness manager
 *   (`Main.brightnessManager`), once the shell started
 */
ShellBrightnessManager *
shell_main_get_brightness_manager (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->brightness_manager;
}

/**
 * shell_main_get_screen_shield:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the screen shield (`Main.screenShield`),
 *   %NULL when the display manager cannot lock the session
 */
ShellScreenShield *
shell_main_get_screen_shield (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->screen_shield;
}

/**
 * shell_main_get_extension_manager:
 * @self: a #ShellMain
 *
 * Returns: (transfer none) (nullable): the manager of the Lua extensions
 *   (`Main.extensionManager`), once the shell started
 */
ShellExtensionManager *
shell_main_get_extension_manager (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->extension_manager;
}

/* ======================================================================== */
/* Notifications                                                            */
/* ======================================================================== */

/**
 * shell_main_send_notification:
 * @self: a #ShellMain
 * @msg: a message
 * @details: (nullable): additional information
 *
 * Shows a transient notification from the system source (`Main.notify ()`).
 */
void
shell_main_send_notification (ShellMain  *self,
                   const char *msg,
                   const char *details)
{
  g_autoptr (ShellNotification) notification = NULL;
  ShellSource *source;

  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (msg != NULL);

  if (self->message_tray == NULL)
    {
      g_message ("%s%s%s", msg, details != NULL ? ": " : "", details != NULL ? details : "");
      return;
    }

  source = shell_message_tray_get_system_source (self->message_tray);
  notification = shell_notification_new (source, msg, details);
  shell_notification_set_is_transient (notification, TRUE);
  shell_source_add_notification (source, notification);
}

/**
 * shell_main_notify_error:
 * @self: a #ShellMain
 * @msg: an error message
 * @details: (nullable): additional information
 *
 * Shows an error notification, and logs it (`Main.notifyError ()`). See
 * shell_global_notify_error (), which emits the signal that calls this.
 */
void
shell_main_notify_error (ShellMain  *self,
                         const char *msg,
                         const char *details)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (msg != NULL);

  /* Also print to stderr so it's logged somewhere */
  if (details != NULL && *details != '\0')
    g_message ("error: %s: %s", msg, details);
  else
    g_message ("error: %s", msg);

  shell_main_send_notification (self, msg, details);
}

static void
global_notify_error_cb (ShellGlobal *global,
                        const char  *msg,
                        const char  *details,
                        ShellMain   *self)
{
  shell_main_notify_error (self, msg, details);
}

typedef struct
{
  ShellAppFavorites *favorites;   /* strong */
  char *app_id;
  int position;
  gboolean added;
} FavoriteUndo;

static void
favorite_undo_free (FavoriteUndo *undo)
{
  g_clear_object (&undo->favorites);
  g_free (undo->app_id);
  g_free (undo);
}

static void
favorite_undo_cb (ShellNotification *notification,
                  gpointer           user_data)
{
  FavoriteUndo *undo = user_data;

  if (undo->added)
    shell_app_favorites_remove_favorite_silently (undo->favorites, undo->app_id);
  else
    shell_app_favorites_add_favorite_silently (undo->favorites, undo->app_id,
                                               undo->position);
}

/*
 * Shows the "Pinned to Dash" / "Unpinned From Dash" notifications of
 * #ShellAppFavorites, each with an Undo button (_connectAppFavorites ()).
 */
static void
show_favorite_notification (ShellMain         *self,
                            ShellAppFavorites *favorites,
                            ShellApp          *app,
                            int                position,
                            gboolean           added)
{
  g_autoptr (ShellNotification) notification = NULL;
  g_autofree char *title = NULL;
  FavoriteUndo *undo;
  ShellSource *source;

  if (self->message_tray == NULL)
    return;

  if (added)
    title = g_strdup_printf (_("%s Pinned to Dash"), shell_app_get_name (app));
  else
    title = g_strdup_printf (_("%s Unpinned From Dash"), shell_app_get_name (app));

  undo = g_new0 (FavoriteUndo, 1);
  undo->favorites = g_object_ref (favorites);
  undo->app_id = g_strdup (shell_app_get_id (app));
  undo->position = position;
  undo->added = added;

  source = shell_message_tray_get_system_source (self->message_tray);
  notification = shell_notification_new (source, title, NULL);
  shell_notification_set_is_transient (notification, TRUE);
  shell_notification_set_for_feedback (notification, TRUE);
  shell_notification_add_action (notification, _("Undo"),
                                 favorite_undo_cb, undo,
                                 (GDestroyNotify) favorite_undo_free);
  shell_source_add_notification (source, notification);
}

static void
favorite_added_cb (ShellAppFavorites *favorites,
                   ShellApp          *app,
                   ShellMain         *self)
{
  show_favorite_notification (self, favorites, app, -1, TRUE);
}

static void
favorite_removed_cb (ShellAppFavorites *favorites,
                     ShellApp          *app,
                     int                position,
                     ShellMain         *self)
{
  show_favorite_notification (self, favorites, app, position, FALSE);
}

static void
connect_app_favorites (ShellMain *self)
{
  ShellAppFavorites *favorites = shell_app_favorites_get_default ();

  g_signal_connect_object (favorites, "favorite-added",
                           G_CALLBACK (favorite_added_cb), self, 0);
  g_signal_connect_object (favorites, "favorite-removed",
                           G_CALLBACK (favorite_removed_cb), self, 0);
}

static void
unsafe_mode_undo_cb (ShellNotification *notification,
                     gpointer           user_data)
{
  g_object_set (user_data, "unsafe-mode", FALSE, NULL);
}

static void
context_unsafe_mode_notify_cb (GObject    *context,
                               GParamSpec *pspec,
                               ShellMain  *self)
{
  g_autoptr (ShellNotification) notification = NULL;
  gboolean unsafe_mode = FALSE;
  ShellSource *source;

  g_object_get (context, "unsafe-mode", &unsafe_mode, NULL);
  if (!unsafe_mode)
    return; /* we're safe */

  if (self->looking_glass != NULL &&
      shell_looking_glass_get_is_open (self->looking_glass))
    return; /* assume user action */

  if (self->message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (self->message_tray);
  notification = shell_notification_new (source,
                                         _("System was put in unsafe mode"),
                                         _("Apps now have unrestricted access"));
  shell_notification_set_is_transient (notification, TRUE);
  shell_notification_add_action (notification, _("Undo"),
                                 unsafe_mode_undo_cb, g_object_ref (context),
                                 g_object_unref);
  shell_source_add_notification (source, notification);
}

/* ======================================================================== */
/* The theme                                                                */
/* ======================================================================== */

/* _getStylesheet (): the resource, the system data directories, then the
 * data directory of the shell */
static GFile *
get_stylesheet (const char *name)
{
  g_autofree char *uri = g_strdup_printf ("resource:///org/gnome/shell/theme/%s", name);
  g_autofree char *path = NULL;
  const char * const *data_dirs;
  GFile *stylesheet;
  int i;

  stylesheet = g_file_new_for_uri (uri);
  if (g_file_query_exists (stylesheet, NULL))
    return stylesheet;
  g_object_unref (stylesheet);

  data_dirs = g_get_system_data_dirs ();
  for (i = 0; data_dirs[i] != NULL; i++)
    {
      g_autofree char *dir_path = g_build_filename (data_dirs[i], "gnome-shell", "theme", name, NULL);

      stylesheet = g_file_new_for_path (dir_path);
      if (g_file_query_exists (stylesheet, NULL))
        return stylesheet;
      g_object_unref (stylesheet);
    }

  path = g_build_filename (shell_global_get_datadir (shell_global_get ()), "theme", name, NULL);
  stylesheet = g_file_new_for_path (path);
  if (g_file_query_exists (stylesheet, NULL))
    return stylesheet;
  g_object_unref (stylesheet);

  return NULL;
}

/* name.replace ('.css', suffix) */
static char *
replace_css_extension (const char *name,
                       const char *suffix)
{
  const char *match = strstr (name, ".css");

  if (match == NULL)
    return g_strdup (name);

  return g_strdup_printf ("%.*s%s%s", (int) (match - name), name, suffix, match + 4);
}

/**
 * shell_main_get_style_variant:
 * @self: a #ShellMain
 *
 * Returns: the style variant of the current color scheme: "dark", "light" or
 *   the empty string if the session mode does not follow the color scheme
 *   (`Main.getStyleVariant ()`)
 */
const char *
shell_main_get_style_variant (ShellMain *self)
{
  g_autofree char *scheme = NULL;
  StSystemColorScheme color_scheme;

  g_return_val_if_fail (SHELL_IS_MAIN (self), "");
  g_return_val_if_fail (self->session_mode != NULL, "");

  color_scheme = st_settings_get_color_scheme (st_settings_get ());
  g_object_get (self->session_mode, "color-scheme", &scheme, NULL);

  if (g_strcmp0 (scheme, "force-dark") == 0)
    return "dark";
  else if (g_strcmp0 (scheme, "force-light") == 0)
    return "light";
  else if (g_strcmp0 (scheme, "prefer-dark") == 0)
    return color_scheme == ST_SYSTEM_COLOR_SCHEME_PREFER_LIGHT ? "light" : "dark";
  else if (g_strcmp0 (scheme, "prefer-light") == 0)
    return color_scheme == ST_SYSTEM_COLOR_SCHEME_PREFER_DARK ? "dark" : "light";

  return "";
}

/* _getDefaultStylesheet () */
static GFile *
get_default_stylesheet (ShellMain *self)
{
  g_autofree char *name = NULL;
  GFile *stylesheet = NULL;

  g_object_get (self->session_mode, "stylesheet-name", &name, NULL);
  if (name == NULL)
    return NULL;

  /* Look for a high-contrast variant first */
  if (st_settings_get_high_contrast (st_settings_get ()))
    {
      g_autofree char *variant = replace_css_extension (name, "-high-contrast.css");

      stylesheet = get_stylesheet (variant);
    }

  if (stylesheet == NULL)
    {
      g_autofree char *suffix = g_strdup_printf ("-%s.css", shell_main_get_style_variant (self));
      g_autofree char *variant = replace_css_extension (name, suffix);

      stylesheet = get_stylesheet (variant);
    }

  if (stylesheet == NULL)
    stylesheet = get_stylesheet (name);

  return stylesheet;
}

/**
 * shell_main_load_theme:
 * @self: a #ShellMain
 * @error: return location for a #GError
 *
 * Reloads the theme CSS file, keeping the custom stylesheets that extensions
 * loaded (`Main.loadTheme ()`).
 *
 * Returns: %FALSE with @error set if there is no valid stylesheet for the
 *   session mode
 */
gboolean
shell_main_load_theme (ShellMain  *self,
                       GError    **error)
{
  StThemeContext *theme_context;
  StTheme *previous_theme;
  g_autoptr (StTheme) theme = NULL;
  g_autofree char *name = NULL;
  GSList *custom_stylesheets, *l;

  g_return_val_if_fail (SHELL_IS_MAIN (self), FALSE);
  g_return_val_if_fail (self->session_mode != NULL, FALSE);

  theme_context = st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ()));
  previous_theme = st_theme_context_get_theme (theme_context);

  theme = st_theme_new (self->css_stylesheet, NULL, self->default_css_stylesheet);

  if (st_theme_get_default_stylesheet (theme) == NULL)
    {
      g_object_get (self->session_mode, "stylesheet-name", &name, NULL);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No valid stylesheet found for '%s'", name);
      return FALSE;
    }

  if (previous_theme != NULL)
    {
      custom_stylesheets = st_theme_get_custom_stylesheets (previous_theme);
      for (l = custom_stylesheets; l != NULL; l = l->next)
        {
          g_autoptr (GError) local_error = NULL;

          if (!st_theme_load_stylesheet (theme, l->data, &local_error))
            g_message ("Failed to load the custom stylesheet: %s", local_error->message);
        }
      g_slist_free_full (custom_stylesheets, g_object_unref);
    }

  st_theme_context_set_theme (theme_context, theme);

  return TRUE;
}

/* _loadDefaultStylesheet () */
static gboolean
load_default_stylesheet (ShellMain  *self,
                         GError    **error)
{
  g_autoptr (GFile) stylesheet = get_default_stylesheet (self);

  if (self->default_css_stylesheet != NULL && stylesheet != NULL &&
      g_file_equal (self->default_css_stylesheet, stylesheet))
    return TRUE;

  g_set_object (&self->default_css_stylesheet, stylesheet);

  return shell_main_load_theme (self, error);
}

static void
load_default_stylesheet_cb (ShellMain *self)
{
  g_autoptr (GError) error = NULL;

  if (!load_default_stylesheet (self, &error))
    g_warning ("%s", error->message);
}

/**
 * shell_main_get_theme_stylesheet:
 * @self: a #ShellMain
 *
 * Gets the theme CSS file that the shell will load (`Main.getThemeStylesheet
 * ()`).
 *
 * Returns: (transfer none) (nullable): the file, %NULL if using the default
 */
GFile *
shell_main_get_theme_stylesheet (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  return self->css_stylesheet;
}

/**
 * shell_main_set_theme_stylesheet:
 * @self: a #ShellMain
 * @css_stylesheet: (nullable) (type filename): a file path that contains the
 *   theme CSS, %NULL to use the default
 *
 * Sets the theme CSS file that the shell will load on the next
 * shell_main_load_theme () (`Main.setThemeStylesheet ()`).
 */
void
shell_main_set_theme_stylesheet (ShellMain  *self,
                                 const char *css_stylesheet)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  g_clear_object (&self->css_stylesheet);
  if (css_stylesheet != NULL)
    self->css_stylesheet = g_file_new_for_path (css_stylesheet);
}

static gboolean
load_resource (const char  *path,
               GResource  **resource,
               GError     **error)
{
  GResource *loaded = g_resource_load (path, error);

  if (loaded == NULL)
    return FALSE;

  if (*resource != NULL)
    {
      g_resources_unregister (*resource);
      g_resource_unref (*resource);
    }

  g_resources_register (loaded);
  *resource = loaded;

  return TRUE;
}

/**
 * shell_main_reload_theme_resource:
 * @self: a #ShellMain
 * @error: return location for a #GError
 *
 * (Re)loads the resource of the theme of the session mode
 * (`Main.reloadThemeResource ()`), replacing the one registered before.
 *
 * Returns: %FALSE with @error set if the resource cannot be loaded
 */
gboolean
shell_main_reload_theme_resource (ShellMain  *self,
                                  GError    **error)
{
  g_autofree char *resource_name = NULL;
  g_autofree char *path = NULL;

  g_return_val_if_fail (SHELL_IS_MAIN (self), FALSE);
  g_return_val_if_fail (self->session_mode != NULL, FALSE);

  g_object_get (self->session_mode, "theme-resource-name", &resource_name, NULL);
  path = g_build_filename (shell_global_get_datadir (shell_global_get ()), resource_name, NULL);

  return load_resource (path, &self->theme_resource, error);
}

static gboolean
load_icons (ShellMain  *self,
            GError    **error)
{
  g_autofree char *path =
    g_build_filename (shell_global_get_datadir (shell_global_get ()),
                      "gnome-shell-icons.gresource", NULL);

  return load_resource (path, &self->icon_resource, error);
}

static gboolean
load_osk_layouts (ShellMain  *self,
                  GError    **error)
{
  g_autofree char *path =
    g_build_filename (shell_global_get_datadir (shell_global_get ()),
                      "gnome-shell-osk-layouts.gresource", NULL);

  return load_resource (path, &self->osk_resource, error);
}

/* ======================================================================== */
/* The animations                                                           */
/* ======================================================================== */

/* _shouldEnableAnimations () */
static gboolean
should_enable_animations (ShellMain *self)
{
  ShellGlobal *global = shell_global_get ();

  if (shell_global_get_force_animations (global))
    return TRUE;

  if (g_hash_table_size (self->remote_access_handles) > 0)
    return FALSE;

  if (!meta_backend_is_rendering_hardware_accelerated (shell_global_get_backend (global)))
    return FALSE;

  if (shell_util_has_x11_display_extension (shell_global_get_display (global),
                                            "VNC-EXTENSION"))
    return FALSE;

  return TRUE;
}

/* _syncAnimationsEnabled () */
static void
sync_animations_enabled (ShellMain *self)
{
  gboolean should_enable = should_enable_animations (self);
  StSettings *settings;

  if (self->animations_enabled == should_enable)
    return;
  self->animations_enabled = should_enable;

  settings = st_settings_get ();
  if (should_enable)
    st_settings_uninhibit_animations (settings);
  else
    st_settings_inhibit_animations (settings);
}

static void
force_animations_notify_cb (ShellGlobal *global,
                            GParamSpec  *pspec,
                            ShellMain   *self)
{
  sync_animations_enabled (self);
}

/* _onRemoteAccessHandleStopped () */
static void
remote_access_handle_stopped_cb (MetaRemoteAccessHandle *handle,
                                 ShellMain              *self)
{
  g_hash_table_remove (self->remote_access_handles, handle);
  sync_animations_enabled (self);
}

/* _onNewRemoteAccessHandle () */
static void
remote_access_new_handle_cb (MetaRemoteAccessController *controller,
                             MetaRemoteAccessHandle     *handle,
                             ShellMain                  *self)
{
  if (!meta_remote_access_handle_get_disable_animations (handle))
    return;

  g_hash_table_add (self->remote_access_handles, g_object_ref (handle));
  sync_animations_enabled (self);
  g_signal_connect_object (handle, "stopped",
                           G_CALLBACK (remote_access_handle_stopped_cb), self, 0);
}

/* new AnimationsSettings () */
static void
animations_settings_init (ShellMain *self)
{
  ShellGlobal *global = shell_global_get ();
  MetaRemoteAccessController *controller;

  g_signal_connect_object (global, "notify::force-animations",
                           G_CALLBACK (force_animations_notify_cb), self, 0);
  sync_animations_enabled (self);

  controller = meta_backend_get_remote_access_controller (shell_global_get_backend (global));
  if (controller != NULL)
    g_signal_connect_object (controller, "new-handle",
                             G_CALLBACK (remote_access_new_handle_cb), self, 0);
}

/* ======================================================================== */
/* The dialogs                                                              */
/* ======================================================================== */

/**
 * shell_main_create_looking_glass:
 * @self: a #ShellMain
 *
 * Creates Looking Glass if it does not exist yet (`Main.createLookingGlass
 * ()`).
 *
 * Returns: (transfer none): Looking Glass
 */
ShellLookingGlass *
shell_main_create_looking_glass (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);

  if (self->looking_glass == NULL)
    {
      ShellLookingGlass *looking_glass = shell_looking_glass_new ();

      /* It adds itself to the ui group, which owns it */
      g_set_weak_pointer (&self->looking_glass, looking_glass);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOOKING_GLASS]);
    }

  return self->looking_glass;
}

static gboolean
run_dialog_internal_command_cb (ShellRunDialog *dialog,
                                const char     *name,
                                ShellMain      *self)
{
  g_autoptr (GError) error = NULL;

  /* The internal commands of the development tools which need more than the
   * dialog has */
  if (g_strcmp0 (name, "lg") == 0)
    {
      shell_looking_glass_open (shell_main_create_looking_glass (self));
      return TRUE;
    }
  else if (g_strcmp0 (name, "rt") == 0)
    {
      /* rt is short for "reload theme" */
      if (!shell_main_reload_theme_resource (self, &error) ||
          !shell_main_load_theme (self, &error))
        g_message ("Failed to reload the theme: %s", error->message);
      return TRUE;
    }

  return FALSE;
}

/**
 * shell_main_open_run_dialog:
 * @self: a #ShellMain
 *
 * Opens the run dialog, creating it first if needed (`Main.openRunDialog
 * ()`).
 */
void
shell_main_open_run_dialog (ShellMain *self)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  if (self->run_dialog == NULL)
    {
      ShellRunDialog *run_dialog = shell_run_dialog_new ();

      g_set_weak_pointer (&self->run_dialog, run_dialog);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_RUN_DIALOG]);
      g_signal_connect_object (run_dialog, "internal-command",
                               G_CALLBACK (run_dialog_internal_command_cb), self, 0);
    }

  shell_modal_dialog_open (SHELL_MODAL_DIALOG (self->run_dialog));
}

static void
open_run_dialog_key_handler (MetaDisplay         *display,
                             MetaWindow          *window,
                             const ClutterEvent  *event,
                             MetaKeyBinding      *binding,
                             gpointer             user_data)
{
  shell_main_open_run_dialog (SHELL_MAIN (user_data));
}

/**
 * shell_main_open_welcome_dialog:
 * @self: a #ShellMain
 *
 * Opens the welcome dialog, creating it first if needed
 * (`Main.openWelcomeDialog ()`).
 */
void
shell_main_open_welcome_dialog (ShellMain *self)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  if (self->welcome_dialog == NULL)
    {
      ShellWelcomeDialog *welcome_dialog = shell_welcome_dialog_new ();

      g_set_weak_pointer (&self->welcome_dialog, welcome_dialog);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_WELCOME_DIALOG]);
      g_signal_connect_object (welcome_dialog, "overview-hide-requested",
                               G_CALLBACK (main_hide_overview), self,
                               G_CONNECT_SWAPPED);
    }

  shell_modal_dialog_open (SHELL_MODAL_DIALOG (self->welcome_dialog));
}

/* ======================================================================== */
/* Windows                                                                  */
/* ======================================================================== */

/**
 * shell_main_activate_window:
 * @self: a #ShellMain
 * @window: the window to activate
 * @time: the current event time, 0 for the current time
 * @workspace_index: the index of the workspace of the window, -1 to ask the
 *   window
 *
 * Activates @window, switching to its workspace first if necessary, and
 * switching out of the overview if it's currently active
 * (`Main.activateWindow ()`).
 */
void
shell_main_activate_window (ShellMain *self,
                            MetaWindow *window,
                            guint32     time,
                            int         workspace_index)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  int active_workspace_index;
  int window_workspace_index;

  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (META_IS_WINDOW (window));

  active_workspace_index = meta_workspace_manager_get_active_workspace_index (workspace_manager);
  window_workspace_index = workspace_index >= 0 ?
    workspace_index : meta_workspace_index (meta_window_get_workspace (window));

  if (time == 0)
    time = shell_global_get_current_time (global);

  if (window_workspace_index != active_workspace_index)
    {
      MetaWorkspace *workspace =
        meta_workspace_manager_get_workspace_by_index (workspace_manager, window_workspace_index);

      meta_workspace_activate_with_focus (workspace, window, time);
    }
  else
    {
      meta_window_activate (window, time);
    }

  main_hide_overview_and_close_calendar (self);
}

typedef struct
{
  MetaWindow *window;     /* strong, as the closure of the JS held it */
  int workspace_index;
  gboolean append;
  gulong handler_id;
} MoveWindowData;

static void
move_window_data_free (MoveWindowData *data,
                       GClosure       *closure)
{
  g_object_unref (data->window);
  g_free (data);
}

static void
window_entered_monitor_cb (MetaDisplay    *display,
                           int             monitor,
                           MetaWindow     *window,
                           MoveWindowData *data)
{
  if (window != data->window)
    return;

  meta_window_change_workspace_by_index (window, data->workspace_index, data->append);
  g_signal_handler_disconnect (display, data->handler_id);
}

/**
 * shell_main_move_window_to_monitor_and_workspace:
 * @self: a #ShellMain
 * @window: the window to move
 * @monitor_index: the requested monitor
 * @workspace_index: the requested workspace
 * @append: whether to create the workspace if it doesn't exist
 *
 * Moves @window to the specified monitor and workspace
 * (`Main.moveWindowToMonitorAndWorkspace ()`).
 */
void
shell_main_move_window_to_monitor_and_workspace (ShellMain  *self,
                                                 MetaWindow *window,
                                                 int         monitor_index,
                                                 int         workspace_index,
                                                 gboolean    append)
{
  g_return_if_fail (SHELL_IS_MAIN (self));
  g_return_if_fail (META_IS_WINDOW (window));

  /* We need to move the window before changing the workspace, because the
   * move itself could cause a workspace change if the window enters the
   * primary monitor */
  if (meta_window_get_monitor (window) != monitor_index)
    {
      MetaDisplay *display = shell_global_get_display (shell_global_get ());
      MoveWindowData *data = g_new0 (MoveWindowData, 1);

      data->window = g_object_ref (window);
      data->workspace_index = workspace_index;
      data->append = append;

      /* Wait for the monitor change to take effect */
      data->handler_id = g_signal_connect_data (display, "window-entered-monitor",
                                                G_CALLBACK (window_entered_monitor_cb),
                                                data,
                                                (GClosureNotify) move_window_data_free, 0);
      meta_window_move_to_monitor (window, monitor_index);
    }
  else
    {
      meta_window_change_workspace_by_index (window, workspace_index, append);
    }
}

static void
capture_screenshot_ready_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = shell_screenshot_capture_screenshot_finish (res, &error);

  if (file == NULL && error != NULL &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Error capturing screenshot: %s", error->message);
}

/* The window menu asks for a screenshot of a window (_connectWindowManager ()) */
static void
window_manager_screenshot_requested_cb (ShellWindowManager *wm,
                                        CoglTexture        *texture,
                                        ShellMain          *self)
{
  shell_screenshot_capture_screenshot_async (texture, NULL, 1.0f, NULL, 0, 0, 1.0f,
                                             SHELL_SCREENSHOT_KIND_WINDOW,
                                             self->cancellable,
                                             capture_screenshot_ready_cb, NULL);
}

/* ======================================================================== */
/* The session mode                                                         */
/* ======================================================================== */

/* _sessionUpdated () */
static void
session_updated (ShellMain *self)
{
  ShellSessionMode *session_mode = self->session_mode;
  ShellGlobal *global = shell_global_get ();
  MetaBackend *backend = shell_global_get_backend (global);
  MetaRemoteAccessController *controller;
  gboolean has_run_dialog = FALSE;
  gboolean allow_screencast = FALSE;

  if (session_mode == NULL || self->window_manager == NULL)
    return;

  g_object_get (session_mode,
                "has-run-dialog", &has_run_dialog,
                "allow-screencast", &allow_screencast,
                NULL);

  if (shell_session_mode_is_primary (session_mode))
    load_default_stylesheet_cb (self);

  shell_window_manager_allow_keybinding (self->window_manager, "overlay-key",
                                         SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW);

  shell_window_manager_allow_keybinding (self->window_manager, "locate-pointer-key",
                                         SHELL_ACTION_MODE_ALL);

  shell_window_manager_set_custom_keybinding_handler (self->window_manager,
                                                      "panel-run-dialog",
                                                      SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW,
                                                      has_run_dialog ? open_run_dialog_key_handler : NULL,
                                                      has_run_dialog ? self : NULL,
                                                      NULL);

  if (!has_run_dialog)
    {
      if (self->run_dialog != NULL)
        shell_modal_dialog_close (SHELL_MODAL_DIALOG (self->run_dialog));
      if (self->looking_glass != NULL)
        shell_looking_glass_close (self->looking_glass);
      if (self->welcome_dialog != NULL)
        shell_modal_dialog_close (SHELL_MODAL_DIALOG (self->welcome_dialog));
    }

  controller = meta_backend_get_remote_access_controller (backend);
  if (controller != NULL && !meta_backend_is_headless (backend))
    {
      if (allow_screencast && self->remote_access_inhibited)
        {
          meta_remote_access_controller_uninhibit_remote_access (controller);
          self->remote_access_inhibited = FALSE;
        }
      else if (!allow_screencast && !self->remote_access_inhibited)
        {
          meta_remote_access_controller_inhibit_remote_access (controller);
          self->remote_access_inhibited = TRUE;
        }
    }
}

static void
xdnd_session_updated_cb (ShellSessionMode *session_mode,
                         ShellMain        *self)
{
  if (self->xdnd_handler != NULL)
    shell_xdnd_handler_set_session_locked (self->xdnd_handler,
                                           shell_session_mode_is_locked (session_mode));
}

static void
session_updated_cb (ShellSessionMode *session_mode,
                    ShellMain        *self)
{
  session_updated (self);
}

static void
settings_app_activating_cb (ShellMain *self)
{
  main_hide_overview (self);
  if (self->panel != NULL)
    shell_panel_close_quick_settings (self->panel);
}

/* ---- the system actions ------------------------------------------------- */

static void
system_actions_lock_screen_requested_cb (ShellSystemActions *actions,
                                         gboolean            animate,
                                         ShellMain          *self)
{
  if (self->screen_shield != NULL)
    shell_screen_shield_lock (self->screen_shield, animate);
}

static void
screenshot_overview_hidden_cb (ShellOverview *overview,
                               ShellMain     *self)
{
  g_clear_signal_handler (&self->screenshot_overview_hidden_id, overview);

  if (self->screenshot_ui != NULL)
    shell_screenshot_ui_open (self->screenshot_ui, SHELL_SCREENSHOT_UI_MODE_SCREENSHOT);
}

/* Screenshot.showScreenshotUI (): the screenshot UI opens once the overview is
 * hidden */
static void
system_actions_screenshot_ui_requested_cb (ShellSystemActions *actions,
                                           ShellMain          *self)
{
  if (self->screenshot_overview_hidden_id != 0)
    return;

  if (self->overview != NULL && shell_overview_get_visible (self->overview))
    {
      self->screenshot_overview_hidden_id =
        g_signal_connect (self->overview, "hidden",
                          G_CALLBACK (screenshot_overview_hidden_cb), self);
      shell_overview_hide (self->overview);
    }
  else if (self->screenshot_ui != NULL)
    {
      shell_screenshot_ui_open (self->screenshot_ui, SHELL_SCREENSHOT_UI_MODE_SCREENSHOT);
    }
}

/* ======================================================================== */
/* Shutdown                                                                 */
/* ======================================================================== */

typedef struct
{
  GMainLoop *loop;
  ShellTimeLimitsManager *manager;  /* strong */
} TimeLimitsShutdown;

static void
time_limits_shutdown_ready_cb (GObject      *source,
                               GAsyncResult *res,
                               gpointer      user_data)
{
  TimeLimitsShutdown *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_time_limits_manager_shutdown_finish (SHELL_TIME_LIMITS_MANAGER (source),
                                                  res, &error))
    g_message ("Failed to stop time limits manager: %s", error->message);

  g_main_loop_quit (data->loop);
}

static gboolean
time_limits_shutdown_idle_cb (gpointer user_data)
{
  TimeLimitsShutdown *data = user_data;

  shell_time_limits_manager_shutdown_async (data->manager, NULL,
                                            time_limits_shutdown_ready_cb, data);

  return G_SOURCE_REMOVE;
}

/* The ::shutdown handler that blocks shutdown until the session history file
 * has been written */
static void
global_shutdown_time_limits_cb (ShellGlobal *global,
                                ShellMain   *self)
{
  TimeLimitsShutdown data;
  GSource *source;

  if (self->time_limits_manager == NULL)
    return;

  data.loop = g_main_loop_new (NULL, FALSE);
  data.manager = g_object_ref (self->time_limits_manager);

  source = g_idle_source_new ();
  g_source_set_callback (source, time_limits_shutdown_idle_cb, &data, NULL);
  g_source_set_name (source, "[gnome-shell] time limits manager shutdown");
  g_source_attach (source, g_main_loop_get_context (data.loop));
  g_source_unref (source);

  g_main_loop_run (data.loop);

  g_main_loop_unref (data.loop);
  g_object_unref (data.manager);
}

static void
global_shutdown_input_method_cb (ShellGlobal *global,
                                 ShellMain   *self)
{
  ClutterStage *stage = shell_global_get_stage (global);
  ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (stage));

  clutter_backend_set_input_method (clutter_context_get_backend (context), NULL);
}

/**
 * shell_main_shutdown:
 * @self: a #ShellMain
 *
 * Releases the objects that shell_main_start () created, in the reverse order
 * of their creation, and cancels what was still pending. It runs after
 * ShellGlobal::shutdown, where the JS context used to be destroyed; the
 * actors are destroyed with the stage afterwards. Safe to call more than
 * once.
 */
void
shell_main_shutdown (ShellMain *self)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  if (self->shutting_down)
    return;
  self->shutting_down = TRUE;

  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->notifications_watch_id, g_bus_unwatch_name);

  while (self->owned->len > 0)
    g_ptr_array_remove_index (self->owned, self->owned->len - 1);
}

/* ======================================================================== */
/* The D-Bus services                                                       */
/* ======================================================================== */

static void
dbus_eval_requested_cb (ShellDBusService       *service,
                        const char             *code,
                        GDBusMethodInvocation  *invocation,
                        ShellMain              *self)
{
  ShellLuaInspector *inspector = shell_global_get_lua_inspector (shell_global_get ());
  g_autoptr (GError) error = NULL;
  g_autofree char *result = shell_lua_inspector_eval (inspector, code, &error);

  shell_dbus_service_complete_eval (service, invocation, result != NULL,
                                    result != NULL ? result : error->message);
}

static void
dbus_focus_search_requested_cb (ShellDBusService *service,
                                ShellMain        *self)
{
  if (self->overview != NULL)
    shell_overview_focus_search (self->overview);
}

static void
dbus_focus_app_requested_cb (ShellDBusService *service,
                             const char       *id,
                             ShellMain        *self)
{
  if (self->overview != NULL)
    shell_overview_select_app (self->overview, id);
}

static void
dbus_show_applications_requested_cb (ShellDBusService *service,
                                     ShellMain        *self)
{
  if (self->overview != NULL)
    shell_overview_show_for_reason (self->overview, SHELL_CONTROLS_STATE_APP_GRID, "dbus");
}

static void
dbus_allow_keybinding_requested_cb (ShellDBusService *service,
                                    const char       *name,
                                    guint             mode_flags,
                                    ShellMain        *self)
{
  if (self->window_manager != NULL)
    shell_window_manager_allow_keybinding (self->window_manager, name, mode_flags);
}

static void
dbus_overview_active_requested_cb (ShellDBusService *service,
                                   gboolean          active,
                                   ShellMain        *self)
{
  if (self->overview == NULL)
    return;

  if (active)
    shell_overview_show_for_reason (self->overview, SHELL_CONTROLS_STATE_WINDOW_PICKER, "dbus");
  else
    shell_overview_hide_for_reason (self->overview, "dbus");
}

static void
dbus_sync_overview_active_cb (ShellOverview     *overview,
                              ShellDBusService  *service)
{
  shell_dbus_service_set_overview_active (service, shell_overview_get_visible (overview));
}

/*
 * Exports org.gnome.Shell and org.gnome.Shell.Extensions, and the Screenshot
 * service (_createShellDBusServices ()). The first two are in
 * src/ui/shell-dbus.c; what they used to reach through the singletons that
 * were JS is a signal that is connected here.
 */
static void
create_shell_dbus_services (ShellMain *self)
{
  ShellDBusService *service;

  service = main_own (self, shell_dbus_service_new ());
  shell_dbus_service_set_osd_window_manager (service, self->osd_window_manager);
  shell_dbus_service_set_osd_monitor_labeler (service, self->osd_monitor_labeler);

  g_signal_connect_object (service, "eval-requested",
                           G_CALLBACK (dbus_eval_requested_cb), self, 0);
  g_signal_connect_object (service, "focus-search-requested",
                           G_CALLBACK (dbus_focus_search_requested_cb), self, 0);
  g_signal_connect_object (service, "focus-app-requested",
                           G_CALLBACK (dbus_focus_app_requested_cb), self, 0);
  g_signal_connect_object (service, "show-applications-requested",
                           G_CALLBACK (dbus_show_applications_requested_cb), self, 0);
  g_signal_connect_object (service, "allow-keybinding-requested",
                           G_CALLBACK (dbus_allow_keybinding_requested_cb), self, 0);
  g_signal_connect_object (service, "overview-active-requested",
                           G_CALLBACK (dbus_overview_active_requested_cb), self, 0);

  if (self->overview != NULL)
    {
      g_signal_connect_object (self->overview, "showing",
                               G_CALLBACK (dbus_sync_overview_active_cb), service, 0);
      g_signal_connect_object (self->overview, "hidden",
                               G_CALLBACK (dbus_sync_overview_active_cb), service, 0);
    }

  /* The extension manager is in C: the service answers through it */
  main_own (self, shell_dbus_extensions_service_new (self->extension_manager));

  main_own (self, shell_screenshot_service_new (self->screenshot_ui));
}

static void
notifications_name_appeared_cb (GDBusConnection *connection,
                                const char      *name,
                                const char      *name_owner,
                                gpointer         user_data)
{
  ShellMain *self = user_data;

  g_clear_handle_id (&self->notifications_watch_id, g_bus_unwatch_name);
}

static void
notifications_name_vanished_cb (GDBusConnection *connection,
                                const char      *name,
                                gpointer         user_data)
{
  ShellMain *self = user_data;

  g_clear_handle_id (&self->notifications_watch_id, g_bus_unwatch_name);
}

/* ======================================================================== */
/* The startup                                                              */
/* ======================================================================== */

static void
lock_warning_delete_ready_cb (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to remove the lock warning file: %s", error->message);
}

static void
lock_warning_touch_ready_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  ShellMain *self = user_data;
  g_autoptr (GError) error = NULL;

  if (self->shutting_down)
    return;

  if (!shell_util_touch_file_finish (G_FILE (source), res, &error))
    {
      if (error == NULL)
        return;

      g_message ("Failed to create the lock warning file: %s", error->message);
    }

  /* A test session has no GDM either: the banner would stay on screen (the
   * session is idle, nothing expires it) and hold back the banners the test
   * waits for, like the dialogs and notifications skipped below */
  if (shell_test_runner_is_enabled ())
    return;

  shell_main_send_notification (self,
                     _("Screen Lock disabled"),
                     _("Screen Locking requires the GNOME display manager"));
}

/* _handleLockScreenWarning () */
static void
handle_lock_screen_warning (ShellMain *self)
{
  g_autofree char *path =
    g_build_filename (shell_global_get_userdatadir (shell_global_get ()),
                      "lock-warning-shown", NULL);
  g_autoptr (GFile) file = g_file_new_for_path (path);

  if (self->screen_shield != NULL)
    g_file_delete_async (file, G_PRIORITY_DEFAULT, self->cancellable,
                         lock_warning_delete_ready_cb, NULL);
  else
    shell_util_touch_file_async (file, lock_warning_touch_ready_cb, self);
}

/* _handleShowWelcomeScreen () */
static void
handle_show_welcome_screen (ShellMain *self)
{
  GSettings *settings = shell_global_get_settings (shell_global_get ());
  g_autofree char *last_shown_version =
    g_settings_get_string (settings, WELCOME_DIALOG_LAST_SHOWN_VERSION);

  if (shell_util_gnome_version_compare (WELCOME_DIALOG_LAST_TOUR_CHANGE,
                                        last_shown_version) > 0)
    {
      shell_main_open_welcome_dialog (self);
      g_settings_set_string (settings, WELCOME_DIALOG_LAST_SHOWN_VERSION, PACKAGE_VERSION);
    }
}

static void
layout_startup_prepared_cb (ShellLayoutManager *layout_manager,
                            ShellMain          *self)
{
  if (self->screen_shield != NULL)
    shell_screen_shield_show_dialog (self->screen_shield);
}

static void
layout_startup_complete_cb (ShellLayoutManager *layout_manager,
                            ShellMain          *self)
{
  ShellGlobal *global = shell_global_get ();
  const char *current_mode;

  if (self->action_mode == SHELL_ACTION_MODE_NONE)
    shell_main_set_action_mode (self, SHELL_ACTION_MODE_NORMAL);

  /* Lua extensions (gnome-lua): emits the "shell.started" hook */
  shell_global_notify_startup_complete (global);

  if (self->screen_shield != NULL)
    shell_screen_shield_lock_if_was_locked (self->screen_shield);

  current_mode = shell_session_mode_get_current_mode (self->session_mode);
  if (g_strcmp0 (current_mode, "gdm") != 0 &&
      g_strcmp0 (current_mode, "initial-setup") != 0)
    {
      g_autofree char *date = g_date_time_format (self->start_date,
                                                  "%a %b %d %Y %H:%M:%S GMT%z (%Z)");
      g_autofree char *message = g_strdup_printf ("GNOME Shell started at %s", date);

      g_log_structured (LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
                        "MESSAGE", message,
                        "MESSAGE_ID", GNOMESHELL_STARTED_MESSAGE_ID);
    }

  /* The dialogs and notifications that would get in the way of a test are
   * skipped when the test runner runs a script */
  if (!shell_test_runner_is_enabled ())
    {
      g_autoptr (GCredentials) credentials = g_credentials_new ();

      if (g_credentials_get_unix_user (credentials, NULL) == 0)
        {
          shell_main_send_notification (self,
                             _("Logged in as a privileged user"),
                             _("Running a session as a privileged user should be avoided for security reasons. If possible, you should log in as a normal user."));
        }
      else
        {
          gboolean show_welcome_dialog = FALSE;

          g_object_get (self->session_mode, "show-welcome-dialog", &show_welcome_dialog, NULL);
          if (show_welcome_dialog)
            handle_show_welcome_screen (self);
        }
    }

  if (g_strcmp0 (current_mode, "gdm") != 0 &&
      g_strcmp0 (current_mode, "initial-setup") != 0)
    handle_lock_screen_warning (self);

  shell_login_manager_register_display_with_gdm ();
}

static void
global_locate_pointer_cb (ShellGlobal *global,
                          ShellMain   *self)
{
  if (self->locate_pointer != NULL)
    shell_locate_pointer_show (self->locate_pointer);
}

static void
ctrl_alt_tab_window_activated_cb (ShellCtrlAltTabManager *manager,
                                  MetaWindow             *window,
                                  guint                   timestamp,
                                  ShellMain              *self)
{
  main_hide_overview_and_close_calendar (self);
}

static void
brightness_osd_requested_cb (ShellBrightnessManager *manager,
                             GIcon                  *icon,
                             GVariant               *levels,
                             ShellMain              *self)
{
  if (self->osd_window_manager != NULL)
    shell_osd_window_manager_show (self->osd_window_manager, icon, NULL, levels);
}

static void
layout_background_added_cb (ShellLayoutManager *layout_manager,
                            ClutterActor       *bg_actor,
                            int                 monitor_index,
                            ShellMain          *self)
{
  shell_background_menu_add_to (bg_actor, layout_manager);
}

/* The signal is emitted from C (modal dialogs, the screenshot UI) and from
 * elsewhere: forward it both ways, once */
static void
layout_system_modal_opened_cb (ShellLayoutManager *layout_manager,
                               ShellMain          *self)
{
  if (self->forwarding_system_modal_opened)
    return;

  self->forwarding_system_modal_opened = TRUE;
  shell_main_system_modal_opened (self);
  self->forwarding_system_modal_opened = FALSE;
}

static void
main_system_modal_opened_cb (ShellMain          *self,
                             ShellLayoutManager *layout_manager)
{
  if (self->forwarding_system_modal_opened)
    return;

  self->forwarding_system_modal_opened = TRUE;
  shell_layout_manager_system_modal_opened (layout_manager);
  self->forwarding_system_modal_opened = FALSE;
}

static void
main_modal_ended_cb (ShellMain *self,
                     gpointer   user_data)
{
  if (self->layout_manager != NULL)
    shell_layout_manager_modal_ended (self->layout_manager);
}

/* ---- the drags ---------------------------------------------------------- */

static void
main_drag_begin (ShellMain *self)
{
  if (self->message_tray != NULL)
    shell_message_tray_set_dragging (self->message_tray, TRUE);
}

static void
main_drag_end (ShellMain *self)
{
  if (self->message_tray != NULL)
    shell_message_tray_set_dragging (self->message_tray, FALSE);
}

static void
expand_active_notification_key_handler (MetaDisplay        *display,
                                        MetaWindow         *window,
                                        const ClutterEvent *event,
                                        MetaKeyBinding     *binding,
                                        gpointer            user_data)
{
  ShellMain *self = user_data;

  if (self->message_tray != NULL)
    shell_message_tray_expand_active_notification (self->message_tray);
}

/*
 * Connects the message tray to the parts of the shell it cannot reach: the
 * tray is hidden from the pick while something is dragged in the overview or
 * from another application, and the keybinding that moves the focus to the
 * banner (_connectMessageTray ()).
 */
static void
connect_message_tray (ShellMain *self)
{
  static const char * const drag_begin_signals[] = {
    "window-drag-begin", "item-drag-begin", NULL
  };
  static const char * const drag_end_signals[] = {
    "window-drag-cancelled", "window-drag-end",
    "item-drag-cancelled", "item-drag-end", NULL
  };
  int i;

  for (i = 0; drag_begin_signals[i] != NULL; i++)
    g_signal_connect_object (self->overview, drag_begin_signals[i],
                             G_CALLBACK (main_drag_begin), self, G_CONNECT_SWAPPED);
  for (i = 0; drag_end_signals[i] != NULL; i++)
    g_signal_connect_object (self->overview, drag_end_signals[i],
                             G_CALLBACK (main_drag_end), self, G_CONNECT_SWAPPED);

  g_signal_connect_object (self->xdnd_handler, "drag-begin",
                           G_CALLBACK (main_drag_begin), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->xdnd_handler, "drag-end",
                           G_CALLBACK (main_drag_end), self, G_CONNECT_SWAPPED);

  self->keybindings_settings = g_settings_new ("org.gnome.shell.keybindings");
  shell_window_manager_add_keybinding (self->window_manager,
                                       "focus-active-notification",
                                       self->keybindings_settings,
                                       META_KEY_BINDING_NONE,
                                       SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW,
                                       expand_active_notification_key_handler,
                                       self, NULL);
}

/* ---- the panel ---------------------------------------------------------- */

static ShellPanelButtonBox *
panel_create_indicator_cb (ShellPanel *panel,
                           const char *role,
                           ShellMain  *self)
{
  return shell_panel_item_create (role);
}

static void
panel_banner_alignment_notify_cb (ShellPanel *panel,
                                  GParamSpec *pspec,
                                  ShellMain  *self)
{
  if (self->message_tray != NULL)
    shell_message_tray_set_banner_alignment (self->message_tray,
                                             shell_panel_get_banner_alignment (panel));
}

static void
panel_banner_blocked_notify_cb (ShellPanel *panel,
                                GParamSpec *pspec,
                                ShellMain  *self)
{
  if (self->message_tray != NULL)
    shell_message_tray_set_banner_blocked (self->message_tray,
                                           shell_panel_get_banner_blocked (panel));
}

/*
 * Creates the top bar. The indicators of the shell are created by
 * shell_panel_item_create (), which the panel asks for through its
 * create-indicator signal, and the parts of the shell it needs but cannot
 * reach (the message tray, the Ctrl+Alt+Tab popup) are wired up here
 * (_createPanel ()).
 */
static ShellPanel *
create_panel (ShellMain *self)
{
  ShellPanel *panel = g_object_new (SHELL_TYPE_PANEL, NULL);

  g_signal_connect_object (panel, "create-indicator",
                           G_CALLBACK (panel_create_indicator_cb), self, 0);

  shell_ctrl_alt_tab_manager_add_group_full (self->ctrl_alt_tab_manager,
                                             CLUTTER_ACTOR (panel),
                                             _("Top Bar"), "shell-focus-top-bar-symbolic",
                                             SHELL_CTRL_ALT_TAB_SORT_GROUP_TOP,
                                             NULL, NULL, NULL, NULL);

  /* Banners are aligned with the date menu, and hidden while a menu is open
   * in the same box of the top bar */
  g_signal_connect_object (panel, "notify::banner-alignment",
                           G_CALLBACK (panel_banner_alignment_notify_cb), self, 0);
  g_signal_connect_object (panel, "notify::banner-blocked",
                           G_CALLBACK (panel_banner_blocked_notify_cb), self, 0);

  shell_panel_begin_startup (panel);
  shell_message_tray_set_banner_alignment (self->message_tray,
                                           shell_panel_get_banner_alignment (panel));

  return panel;
}

static gboolean
ready_idle_cb (gpointer user_data)
{
  /* We're ready for the session manager to move to the next phase */
  shell_util_sd_notify ();
  meta_context_notify_ready (shell_global_get_context (shell_global_get ()));

  return G_SOURCE_REMOVE;
}

/* _initializeUI () */
static gboolean
initialize_ui (ShellMain  *self,
               GError    **error)
{
  ShellGlobal *global = shell_global_get ();
  ShellMessageTray *message_tray;
  ClutterActor *ui_group;
  ShellBreakManager *break_manager;
  ShellInputMethod *input_method;
  ShellOskManager *keyboard;
  ShellWindowManager *window_manager;
  ShellOverview *overview;
  ShellLocatePointer *locate_pointer;
  ShellBrightnessManager *brightness_manager;
  ShellTimeLimitsManager *time_limits_manager;
  const char * const *keybinding_names;
  ShellXdndHandler *xdnd_handler;
  ShellCtrlAltTabManager *ctrl_alt_tab_manager;
  ShellOsdWindowManager *osd_window_manager;
  ShellOsdMonitorLabeler *osd_monitor_labeler;
  ShellSessionMode *session_mode = self->session_mode;
  ShellPanel *panel;
  ShellLayoutManager *layout_manager;
  ClutterContext *clutter_context;
  ShellScreenShield *screen_shield = NULL;
  ShellScreenshotUI *screenshot_ui;
  ShellExtensionManager *extension_manager;
  MetaContext *context = shell_global_get_context (global);
  int i;

  /* Ensure ShellWindowTracker and ShellAppUsage are initialized; this will
   * also initialize ShellAppSystem first. ShellAppSystem needs to load all
   * the .desktop files, and ShellWindowTracker will use those to associate
   * with windows. Right now the Monitor doesn't listen for installed app
   * changes and recalculate application associations, so to avoid races for
   * now we initialize it here. It's better to be predictable anyways. */
  shell_window_tracker_get_default ();
  shell_app_usage_get_default ();

  if (!shell_main_reload_theme_resource (self, error) ||
      !load_icons (self, error) ||
      !load_osk_layouts (self, error) ||
      !load_default_stylesheet (self, error))
    return FALSE;

  animations_settings_init (self);

  /* Setup the stage hierarchy early */
  layout_manager = shell_layout_manager_new ();
  shell_main_set_layout_manager (self, layout_manager);
  g_object_unref (layout_manager);

  /* The layout manager asks for the parts of the shell it cannot reach */
  g_signal_connect_object (layout_manager, "background-added",
                           G_CALLBACK (layout_background_added_cb), self, 0);
  g_signal_connect_object (layout_manager, "system-modal-opened",
                           G_CALLBACK (layout_system_modal_opened_cb), self, 0);
  g_signal_connect_object (self, "system-modal-opened",
                           G_CALLBACK (main_system_modal_opened_cb), layout_manager, 0);

  /* The modal dialogs are added to the modal dialog group of the layout
   * manager */
  shell_modal_dialog_set_default_container (CLUTTER_ACTOR (shell_layout_manager_get_modal_dialog_group (layout_manager)));

  ui_group = CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));

  main_own (self, shell_pad_osd_service_new ());

  xdnd_handler = main_own (self, shell_xdnd_handler_new (ui_group));
  shell_xdnd_handler_set_session_locked (xdnd_handler, shell_session_mode_is_locked (session_mode));
  g_set_weak_pointer (&self->xdnd_handler, xdnd_handler);
  shell_dnd_set_xdnd_handler (xdnd_handler);
  g_signal_connect_object (session_mode, "updated",
                           G_CALLBACK (xdnd_session_updated_cb), self, 0);

  ctrl_alt_tab_manager = main_own (self, shell_ctrl_alt_tab_manager_new ());
  g_set_weak_pointer (&self->ctrl_alt_tab_manager, ctrl_alt_tab_manager);
  /* What Main.activateWindow () does on top of activating the window */
  g_signal_connect_object (ctrl_alt_tab_manager, "window-activated",
                           G_CALLBACK (ctrl_alt_tab_window_activated_cb), self, 0);

  osd_window_manager = shell_osd_window_manager_new (ui_group);
  shell_main_set_osd_window_manager (self, osd_window_manager);
  g_object_unref (osd_window_manager);

  osd_monitor_labeler = main_own (self, shell_osd_monitor_labeler_new (ui_group));
  g_set_weak_pointer (&self->osd_monitor_labeler, osd_monitor_labeler);

  main_own (self, shell_kbd_a11y_dialog_new ());

  window_manager = main_own (self, g_object_new (SHELL_TYPE_WINDOW_MANAGER,
                                                 "ctrl-alt-tab-manager", ctrl_alt_tab_manager,
                                                 NULL));
  shell_main_set_window_manager (self, window_manager);
  g_signal_connect_object (window_manager, "screenshot-requested",
                           G_CALLBACK (window_manager_screenshot_requested_cb), self, 0);

  /* The overview connects itself to the window manager, the layout manager,
   * the XDND handler and the session mode, and keeps the state of the window
   * manager and the panel in sync */
  overview = main_own (self, shell_overview_new (window_manager, layout_manager,
                                                 xdnd_handler, ctrl_alt_tab_manager,
                                                 session_mode));
  shell_main_set_overview (self, overview);

  main_own (self, shell_magnifier_new ());
  locate_pointer = main_own (self, shell_locate_pointer_new (ui_group));
  g_set_weak_pointer (&self->locate_pointer, locate_pointer);

  if (shell_login_manager_can_lock ())
    {
      screen_shield = main_own (self, shell_screen_shield_new ());
      g_set_weak_pointer (&self->screen_shield, screen_shield);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCREEN_SHIELD]);
    }

  input_method = main_own (self, shell_input_method_new ());
  shell_main_set_input_method (self, input_method);
  clutter_context = clutter_actor_get_context (CLUTTER_ACTOR (shell_global_get_stage (global)));
  clutter_backend_set_input_method (clutter_context_get_backend (clutter_context),
                                    CLUTTER_INPUT_METHOD (input_method));
  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (global_shutdown_input_method_cb), self, 0);

  screenshot_ui = shell_screenshot_ui_new (window_manager);
  g_set_weak_pointer (&self->screenshot_ui, screenshot_ui);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCREENSHOT_UI]);

  brightness_manager = main_own (self, shell_brightness_manager_new ());
  g_set_weak_pointer (&self->brightness_manager, brightness_manager);
  keybinding_names = shell_brightness_manager_get_keybinding_names ();
  for (i = 0; keybinding_names[i] != NULL; i++)
    shell_window_manager_allow_keybinding (window_manager, keybinding_names[i],
                                           SHELL_ACTION_MODE_ALL);
  g_signal_connect_object (brightness_manager, "osd-requested",
                           G_CALLBACK (brightness_osd_requested_cb), self, 0);
  main_own (self, shell_brightness_service_new (brightness_manager));

  message_tray = shell_message_tray_new ();
  shell_main_set_message_tray (self, message_tray);
  connect_message_tray (self);

  panel = create_panel (self);
  shell_main_set_panel (self, panel);

  keyboard = main_own (self, shell_osk_manager_new (input_method));
  shell_main_set_keyboard (self, keyboard);

  /* What the notification sources did on top of activating an application */
  {
    ShellNotificationDaemon *notification_daemon =
      main_own (self, shell_notification_daemon_new (message_tray));
    ShellWindowAttentionHandler *attention_handler =
      main_own (self, shell_window_attention_handler_new (message_tray));

    g_signal_connect_object (notification_daemon, "app-opened",
                             G_CALLBACK (main_hide_overview_and_close_calendar), self,
                             G_CONNECT_SWAPPED);
    /* What Main.activateWindow () does on top of activating the window */
    g_signal_connect_object (attention_handler, "window-activated",
                             G_CALLBACK (main_hide_overview_and_close_calendar), self,
                             G_CONNECT_SWAPPED);
  }

  main_own (self, shell_component_manager_new (session_mode));

  main_own (self, shell_introspect_service_new ());

  /* Set up the global default break reminder manager and its D-Bus interface */
  break_manager = main_own (self, shell_break_manager_new ());
  time_limits_manager = main_own (self, shell_time_limits_manager_new ());
  g_set_weak_pointer (&self->time_limits_manager, time_limits_manager);
  /* The lock screen blocks the unlocking when the time limits lock the session */
  if (screen_shield != NULL)
    shell_screen_shield_set_time_limits_manager (screen_shield, time_limits_manager);
  main_own (self, shell_screen_time_service_new (G_OBJECT (break_manager)));
  main_own (self, shell_break_dispatcher_new (break_manager, layout_manager, message_tray));
  main_own (self, shell_time_limits_dispatcher_new (time_limits_manager, layout_manager,
                                                    message_tray));

  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (global_shutdown_time_limits_cb), self, 0);

  shell_layout_manager_begin_startup (layout_manager);
  connect_app_favorites (self);

  main_own (self, shell_pointer_a11y_timeout_new (ui_group));

  g_signal_connect_object (global, "locate-pointer",
                           G_CALLBACK (global_locate_pointer_cb), self, 0);

  g_signal_connect_object (context, "notify::unsafe-mode",
                           G_CALLBACK (context_unsafe_mode_notify_cb), self, 0);

  /* Provide the bus object for gnome-session to initiate logouts. The dialog
   * is an actor of the modal dialog group */
  shell_end_session_dialog_new ();

  g_idle_add_full (G_PRIORITY_DEFAULT, ready_idle_cb, NULL, NULL);

  self->start_date = g_date_time_new_now_local ();

  /* The extension manager is in C; the notification that updates are ready
   * asks to close the overview and the calendar when it opens the application
   * that installs them */
  extension_manager = main_own (self, shell_extension_manager_new (session_mode, message_tray));
  g_set_weak_pointer (&self->extension_manager, extension_manager);
  g_signal_connect_object (extension_manager, "overview-hide-requested",
                           G_CALLBACK (main_hide_overview_and_close_calendar), self,
                           G_CONNECT_SWAPPED);
  shell_extension_manager_start (extension_manager);

  shell_login_manager_register_session_with_gdm ();

  if (shell_session_mode_is_greeter (session_mode) && screen_shield != NULL)
    g_signal_connect_object (layout_manager, "startup-prepared",
                             G_CALLBACK (layout_startup_prepared_cb), self, 0);

  g_signal_connect_object (layout_manager, "startup-complete",
                           G_CALLBACK (layout_startup_complete_cb), self, 0);

  return TRUE;
}

/**
 * shell_main_start:
 * @self: a #ShellMain
 * @error: return location for a #GError
 *
 * Starts the shell: creates the session mode, the singletons and the UI and
 * exports the D-Bus services (`start ()` of main.js). It runs once, from the
 * main loop, after the compositor started.
 *
 * Returns: %FALSE with @error set if the shell cannot run (no stylesheet for
 *   the session mode, a resource that does not load)
 */
gboolean
shell_main_start (ShellMain  *self,
                  GError    **error)
{
  ShellGlobal *global = shell_global_get ();
  ShellSystemActions *system_actions;
  ShellAudioDeviceSelectionDBus *audio_selection;
  const char *current_desktop;
  const char *slowdown_env;
  g_auto (GStrv) desktops = NULL;

  g_return_val_if_fail (SHELL_IS_MAIN (self), FALSE);
  g_return_val_if_fail (!self->started, FALSE);

  self->started = TRUE;

  /* GNOME_SHELL_SLOWDOWN_FACTOR (environment.js) */
  slowdown_env = g_getenv ("GNOME_SHELL_SLOWDOWN_FACTOR");
  if (slowdown_env != NULL)
    {
      char *end = NULL;
      double factor = g_ascii_strtod (slowdown_env, &end);

      if (end != slowdown_env && factor > 0.0)
        st_settings_set_slow_down_factor (st_settings_get (), factor);
    }

  /* Chain up async errors reported from C */
  g_signal_connect_object (global, "notify-error",
                           G_CALLBACK (global_notify_error_cb), self, 0);

  current_desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  if (current_desktop != NULL)
    desktops = g_strsplit (current_desktop, ":", -1);
  if (desktops == NULL || !g_strv_contains ((const char * const *) desktops, "GNOME"))
    {
      /* Deprecated since 2.42 as it is not thread safe: the shell does it
       * once, before anything else uses the desktop files */
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      g_desktop_app_info_set_desktop_env ("GNOME");
      G_GNUC_END_IGNORE_DEPRECATIONS
    }

  {
    g_autoptr (ShellSessionMode) session_mode = shell_session_mode_new ();

    shell_main_set_session_mode (self, session_mode);
  }
  g_signal_connect_object (self->session_mode, "updated",
                           G_CALLBACK (session_updated_cb), self, 0);

  /* The popup menus live in C: they follow the session mode, and ask for the
   * parts of the shell they cannot reach */
  g_signal_connect_object (self, "settings-app-activating",
                           G_CALLBACK (settings_app_activating_cb), self,
                           G_CONNECT_SWAPPED);
  g_signal_connect (self, "modal-ended",
                    G_CALLBACK (main_modal_ended_cb), NULL);

  /* The system actions live in C and ask for the parts of the shell they
   * cannot reach */
  system_actions = shell_system_actions_get_default ();
  shell_system_actions_set_session_mode (system_actions, self->session_mode);
  g_signal_connect_object (system_actions, "lock-screen-requested",
                           G_CALLBACK (system_actions_lock_screen_requested_cb), self, 0);
  g_signal_connect_object (system_actions, "hide-overview-requested",
                           G_CALLBACK (main_hide_overview), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (system_actions, "screenshot-ui-requested",
                           G_CALLBACK (system_actions_screenshot_ui_requested_cb), self, 0);

  g_signal_connect_object (st_settings_get (), "notify::high-contrast",
                           G_CALLBACK (load_default_stylesheet_cb), self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (st_settings_get (), "notify::color-scheme",
                           G_CALLBACK (load_default_stylesheet_cb), self,
                           G_CONNECT_SWAPPED);

  /* Initialize ParentalControlsManager before the UI */
  shell_parental_controls_manager_get_default ();

  if (!initialize_ui (self, error))
    return FALSE;

  main_own (self, shell_access_dialog_dbus_new ());
  audio_selection = main_own (self, shell_audio_device_selection_dbus_new ());
  g_signal_connect_object (audio_selection, "overview-hide-requested",
                           G_CALLBACK (main_hide_overview), self, G_CONNECT_SWAPPED);
  create_shell_dbus_services (self);
  main_own (self, shell_mount_op_handler_new ());

  self->notifications_watch_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION, "org.gnome.Shell.Notifications",
                      G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                      notifications_name_appeared_cb,
                      notifications_name_vanished_cb,
                      self, NULL);

  session_updated (self);

  return TRUE;
}
