/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * looking-glass.c: Looking Glass, the integrated debugger and inspector of
 * the shell. Port of js/ui/lookingGlass.js.
 *
 * Looking Glass is Lua only. The "Evaluator" page is the prompt of the
 * privileged state of #ShellLuaInspector (the page that was called "Lua" next
 * to the JavaScript evaluator, which is gone together with js/misc/jsParse.js
 * and the JavaScript auto completion). What the JS module kept in the evaluator
 * itself is done by the inspector state:
 *
 *  - the results of the commands are Lua values in the inspector state, `r (n)`
 *    gets the n-th (counting from 1, the JS counted from 0) and `it` is the
 *    last one; a command can return several values, each is a result;
 *  - what the command prints with `print ()` is shown above its results;
 *  - the object inspector shows the entries of a table, or the properties and
 *    methods of an object, and the links in it open the next value;
 *  - Tab completes global variables, keywords, the fields of tables and the
 *    properties and methods of objects (shell_lua_inspector_complete ()).
 *
 * Objects (actors, windows, applications) reach Lua as generic handles, see
 * shell-lua-inspector.h. The globals `global`, `stage`, `st_settings` and
 * `main` and the functions `inspect (x, y)` and `window_actors ()` are
 * defined here, and actors have a few navigation methods.
 *
 * The Extensions page lists the extensions of #ShellLuaExtensionManager.
 *
 * Other differences from the JS module, besides the renames of
 * docs/PORT_STATUS.md:
 *
 *  - There is no "Lua" page any more, and no JS (`System.gc ()`): the button
 *    with the trash can collects the garbage of the Lua states.
 *  - `Main.initializeDeferredWork ()` of the window list did nothing (the list
 *    is only updated when Looking Glass is open) and is not ported.
 *  - Closing Looking Glass while the object inspector is open pops the modal
 *    of the inspector too (the JS left it on the modal stack), and reopening
 *    it while it is still sliding out no longer loses the new modal.
 *  - The key bindings Ctrl+Page_Down and Ctrl+Page_Up select the previous and
 *    the next page, as in the JS module, where the names are swapped.
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <cogl/cogl.h>
#include <glib/gi18n-lib.h>
#include <graphene.h>
#include <json-glib/json-glib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-context.h>
#include <meta/meta-debug.h>
#include <meta/meta-enum-types.h>
#include <meta/meta-later.h>
#include <meta/meta-window-actor.h>
#include <meta/util.h>
#include <meta/window.h>

#include "misc/history.h"
#include "shell-app.h"
#include "shell-global.h"
#include "shell-lua-extension.h"
#include "shell-lua-handle.h"
#include "shell-lua-inspector.h"
#include "shell-window-tracker.h"
#include "st.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/looking-glass.h"
#include "ui/main.h"
#include "ui/popup-menu.h"
#include "ui/shell-entry.h"
#include "ui/slider.h"

#define CHEVRON ">>> "

#define HISTORY_KEY "looking-glass-history"

/* Time between tabs for them to count as a double-tab event */
#define AUTO_COMPLETE_DOUBLE_TAB_DELAY 500
#define AUTO_COMPLETE_SHOW_COMPLETION_ANIMATION_DURATION 200

/* Sort of magic, but...eh. */
#define MAX_RESULT_ROWS 150

#define OBJ_INSPECTOR_ANIMATION_TIME 200

#define MAX_SLOWDOWN 8 /* matches GTK inspector */
#define TOGGLE_SLOWDOWN 5

/* Looking Glass, for the pages that report back to it */
static void     lg_inspect_value             (ShellLookingGlass *self,
                                              guint64            id,
                                              ClutterActor      *source_actor);
static void     lg_inspect_object            (ShellLookingGlass *self,
                                              GObject           *object,
                                              ClutterActor      *source_actor);
static void     lg_insert_value              (ShellLookingGlass *self,
                                              guint64            id);
static void     lg_set_border_paint_target   (ShellLookingGlass *self,
                                              ClutterActor      *target);
static ShellLuaInspector *lg_get_inspector   (ShellLookingGlass *self);

static ShellLayoutManager *
get_layout_manager (void)
{
  return shell_main_get_layout_manager (shell_main_get_default ());
}

static ClutterActor *
get_ui_group (void)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();

  if (layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
}

/* The geometry of the primary monitor; zeros if there is no layout manager. */
static void
get_primary_monitor (int *x,
                     int *y,
                     int *width,
                     int *height)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ShellMonitor *monitor = NULL;

  if (layout_manager != NULL)
    monitor = shell_layout_manager_get_primary_monitor (layout_manager);

  *x = monitor != NULL ? monitor->x : 0;
  *y = monitor != NULL ? monitor->y : 0;
  *width = monitor != NULL ? monitor->width : 0;
  *height = monitor != NULL ? monitor->height : 0;

  g_clear_pointer (&monitor, shell_monitor_free);
}

/* What an actor is called in the lists: its type and its name */
static char *
describe_object (gpointer object)
{
  const char *name = NULL;

  if (CLUTTER_IS_ACTOR (object))
    name = clutter_actor_get_name (CLUTTER_ACTOR (object));

  if (name != NULL && *name != '\0')
    return g_strdup_printf ("%s \"%s\"", G_OBJECT_TYPE_NAME (object), name);

  return g_strdup (G_OBJECT_TYPE_NAME (object));
}

static ClutterText *
label_get_text (ClutterActor *label)
{
  return st_label_get_clutter_text (ST_LABEL (label));
}

/* A label that is cut with an ellipsis at the end */
static ClutterActor *
ellipsized_label_new (const char *text)
{
  ClutterActor *label = CLUTTER_ACTOR (st_label_new (text));

  clutter_text_set_ellipsize (label_get_text (label), PANGO_ELLIPSIZE_END);
  return label;
}

/* A label that wraps over several lines */
static ClutterActor *
wrapping_label_new (const char *text)
{
  ClutterActor *label = CLUTTER_ACTOR (st_label_new (text));

  clutter_text_set_ellipsize (label_get_text (label), PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (label_get_text (label), TRUE);
  return label;
}

static ClutterActor *
box_layout_new (const char       *name,
                const char       *style_class,
                ClutterOrientation orientation)
{
  return g_object_new (ST_TYPE_BOX_LAYOUT,
                       "name", name,
                       "style-class", style_class,
                       "orientation", orientation,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* LgNotebook: the pages, one visible at a time, and the labels to switch     */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_NOTEBOOK (lg_notebook_get_type ())
G_DECLARE_FINAL_TYPE (LgNotebook, lg_notebook, LG, NOTEBOOK, StBoxLayout)

typedef struct
{
  LgNotebook   *notebook; /* not owned: the tab dies with it */
  ClutterActor *child;
  ClutterActor *label_box;
  ClutterActor *label;        /* weak */
  ClutterActor *scroll_view;
  StAdjustment *adjustment;   /* weak */
  gboolean      scroll_to_bottom;
} LgTab;

struct _LgNotebook
{
  StBoxLayout parent;

  ClutterActor *tab_controls; /* the labels, put in the toolbar by the owner */

  int selected_index;
  GPtrArray *tabs; /* LgTab */
};

G_DEFINE_FINAL_TYPE (LgNotebook, lg_notebook, ST_TYPE_BOX_LAYOUT)

static void
lg_tab_free (LgTab *tab)
{
  g_clear_weak_pointer (&tab->label);
  g_clear_weak_pointer (&tab->adjustment);
  g_free (tab);
}

static void
lg_notebook_constructed (GObject *object)
{
  LgNotebook *self = LG_NOTEBOOK (object);

  G_OBJECT_CLASS (lg_notebook_parent_class)->constructed (object);

  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);
  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);

  self->tab_controls = box_layout_new (NULL, "labels", CLUTTER_ORIENTATION_HORIZONTAL);
  g_object_ref_sink (self->tab_controls);
}

static void
lg_notebook_dispose (GObject *object)
{
  LgNotebook *self = LG_NOTEBOOK (object);
  guint i;

  /* The tabs are freed with the notebook, the widgets they were connected
   * to can outlive it for a moment */
  for (i = 0; self->tabs != NULL && i < self->tabs->len; i++)
    {
      LgTab *tab = g_ptr_array_index (self->tabs, i);

      if (tab->adjustment != NULL)
        g_signal_handlers_disconnect_by_data (tab->adjustment, tab);
      if (tab->label != NULL)
        g_signal_handlers_disconnect_by_data (tab->label, tab);
    }

  if (self->tab_controls != NULL)
    {
      /* the owner added the labels to its toolbar, which destroys them; if it
       * did not, this is the only reference */
      g_clear_object (&self->tab_controls);
    }

  G_OBJECT_CLASS (lg_notebook_parent_class)->dispose (object);
}

static void
lg_notebook_finalize (GObject *object)
{
  LgNotebook *self = LG_NOTEBOOK (object);

  g_clear_pointer (&self->tabs, g_ptr_array_unref);

  G_OBJECT_CLASS (lg_notebook_parent_class)->finalize (object);
}

static void
lg_notebook_class_init (LgNotebookClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = lg_notebook_constructed;
  object_class->dispose = lg_notebook_dispose;
  object_class->finalize = lg_notebook_finalize;
}

static void
lg_notebook_init (LgNotebook *self)
{
  self->selected_index = -1;
  self->tabs = g_ptr_array_new_with_free_func ((GDestroyNotify) lg_tab_free);
}

static ClutterActor *
lg_notebook_new (void)
{
  return g_object_new (LG_TYPE_NOTEBOOK, NULL);
}

static void
lg_notebook_unselect (LgNotebook *self)
{
  LgTab *tab;

  if (self->selected_index < 0)
    return;

  tab = g_ptr_array_index (self->tabs, self->selected_index);
  st_widget_remove_style_pseudo_class (ST_WIDGET (tab->label_box), "selected");
  clutter_actor_hide (tab->scroll_view);
  self->selected_index = -1;
}

static void
lg_notebook_select_index (LgNotebook *self,
                          int         index)
{
  LgTab *tab;

  if (index == self->selected_index)
    return;

  if (index < 0)
    {
      lg_notebook_unselect (self);
      return;
    }

  g_return_if_fail ((guint) index < self->tabs->len);

  /* Focus the new tab before unmapping the old one */
  tab = g_ptr_array_index (self->tabs, index);
  if (!st_widget_navigate_focus (ST_WIDGET (tab->scroll_view), NULL,
                                 ST_DIR_TAB_FORWARD, FALSE))
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));

  lg_notebook_unselect (self);

  st_widget_add_style_pseudo_class (ST_WIDGET (tab->label_box), "selected");
  clutter_actor_show (tab->scroll_view);
  self->selected_index = index;
}

static void
lg_notebook_select_child (LgNotebook   *self,
                          ClutterActor *child)
{
  guint i;

  if (child == NULL)
    {
      lg_notebook_select_index (self, -1);
      return;
    }

  for (i = 0; i < self->tabs->len; i++)
    {
      LgTab *tab = g_ptr_array_index (self->tabs, i);

      if (tab->child == child)
        {
          lg_notebook_select_index (self, i);
          return;
        }
    }
}

static void
tab_label_clicked_cb (StButton *button,
                      int       clicked_button,
                      LgTab    *tab)
{
  lg_notebook_select_child (tab->notebook, tab->child);
}

static void
tab_adjustment_changed_cb (StAdjustment *adjustment,
                           LgTab        *tab)
{
  double upper, page_size;

  if (!tab->scroll_to_bottom)
    return;

  g_object_get (adjustment, "upper", &upper, "page-size", &page_size, NULL);
  st_adjustment_set_value (adjustment, upper - page_size);
}

static void
lg_notebook_append_page (LgNotebook   *self,
                         const char   *name,
                         ClutterActor *child)
{
  LgTab *tab = g_new0 (LgTab, 1);
  ClutterActor *label_box;
  ClutterActor *label;
  ClutterActor *scroll_view;
  StAdjustment *adjustment;

  label_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "style-class", "notebook-tab",
                            "reactive", TRUE,
                            "track-hover", TRUE,
                            NULL);
  label = CLUTTER_ACTOR (st_button_new_with_label (name));
  clutter_actor_add_child (label_box, label);
  clutter_actor_add_child (self->tab_controls, label_box);

  scroll_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                              "y-expand", TRUE,
                              NULL);
  st_scroll_view_set_child (ST_SCROLL_VIEW (scroll_view), ST_SCROLLABLE (child));

  tab->notebook = self;
  tab->child = child;
  tab->label_box = label_box;
  g_set_weak_pointer (&tab->label, label);
  tab->scroll_view = scroll_view;
  tab->scroll_to_bottom = FALSE;
  g_ptr_array_add (self->tabs, tab);

  g_signal_connect (label, "clicked", G_CALLBACK (tab_label_clicked_cb), tab);

  clutter_actor_hide (scroll_view);
  clutter_actor_add_child (CLUTTER_ACTOR (self), scroll_view);

  adjustment = st_scroll_view_get_vadjustment (ST_SCROLL_VIEW (scroll_view));
  g_set_weak_pointer (&tab->adjustment, adjustment);
  g_signal_connect (adjustment, "changed",
                    G_CALLBACK (tab_adjustment_changed_cb), tab);

  if (self->selected_index == -1)
    lg_notebook_select_index (self, 0);
}

/* Keeps the page scrolled to the bottom from now on, like the JS module: the
 * flag is never cleared again. */
static void
lg_notebook_scroll_to_bottom (LgNotebook *self,
                              int         index)
{
  LgTab *tab;

  g_return_if_fail (index >= 0 && (guint) index < self->tabs->len);

  tab = g_ptr_array_index (self->tabs, index);
  tab->scroll_to_bottom = TRUE;
}

static void
lg_notebook_next_tab (LgNotebook *self)
{
  int next = self->selected_index;

  if (next < (int) self->tabs->len - 1)
    next++;

  lg_notebook_select_index (self, next);
}

static void
lg_notebook_prev_tab (LgNotebook *self)
{
  int prev = self->selected_index;

  if (prev > 0)
    prev--;

  lg_notebook_select_index (self, prev);
}

/* ------------------------------------------------------------------------ */
/* LgRedBorderEffect: the red frame around the actor that is being looked at  */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_RED_BORDER_EFFECT (lg_red_border_effect_get_type ())
G_DECLARE_FINAL_TYPE (LgRedBorderEffect, lg_red_border_effect,
                      LG, RED_BORDER_EFFECT, ClutterEffect)

struct _LgRedBorderEffect
{
  ClutterEffect parent;

  CoglPipeline *pipeline;
};

G_DEFINE_FINAL_TYPE (LgRedBorderEffect, lg_red_border_effect, CLUTTER_TYPE_EFFECT)

static void
lg_red_border_effect_paint_node (ClutterEffect          *effect,
                                 ClutterPaintNode       *node,
                                 ClutterPaintContext    *paint_context,
                                 ClutterEffectPaintFlags flags)
{
  LgRedBorderEffect *self = LG_RED_BORDER_EFFECT (effect);
  ClutterActor *actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (effect));
  g_autoptr (ClutterPaintNode) actor_node = NULL;
  g_autoptr (ClutterPaintNode) pipeline_node = NULL;
  ClutterActorBox alloc;
  float width, height;
  const float border = 2;

  if (actor == NULL)
    return;

  actor_node = clutter_actor_node_new (actor, -1);
  clutter_paint_node_add_child (node, actor_node);

  if (self->pipeline == NULL)
    {
      CoglFramebuffer *framebuffer = clutter_paint_context_get_framebuffer (paint_context);
      CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
      CoglColor color = { .red = 255, .green = 0, .blue = 0, .alpha = 196 };

      self->pipeline = cogl_pipeline_new (cogl_context);
      cogl_pipeline_set_color (self->pipeline, &color);
    }

  clutter_actor_get_allocation_box (actor, &alloc);
  width = clutter_actor_box_get_width (&alloc);
  height = clutter_actor_box_get_height (&alloc);

  pipeline_node = clutter_pipeline_node_new (self->pipeline);
  clutter_paint_node_set_static_name (pipeline_node, "Red Border");
  clutter_paint_node_add_child (node, pipeline_node);

  /* clockwise order */
  clutter_paint_node_add_rectangle (pipeline_node,
                                    &(ClutterActorBox) {
                                      0, 0, width, border,
                                    });
  clutter_paint_node_add_rectangle (pipeline_node,
                                    &(ClutterActorBox) {
                                      width - border, border, width, height,
                                    });
  clutter_paint_node_add_rectangle (pipeline_node,
                                    &(ClutterActorBox) {
                                      0, height - border, width - border, height,
                                    });
  clutter_paint_node_add_rectangle (pipeline_node,
                                    &(ClutterActorBox) {
                                      0, border, border, height - border,
                                    });
}

static void
lg_red_border_effect_finalize (GObject *object)
{
  LgRedBorderEffect *self = LG_RED_BORDER_EFFECT (object);

  g_clear_object (&self->pipeline);

  G_OBJECT_CLASS (lg_red_border_effect_parent_class)->finalize (object);
}

static void
lg_red_border_effect_class_init (LgRedBorderEffectClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterEffectClass *effect_class = CLUTTER_EFFECT_CLASS (klass);

  object_class->finalize = lg_red_border_effect_finalize;
  effect_class->paint_node = lg_red_border_effect_paint_node;
}

static void
lg_red_border_effect_init (LgRedBorderEffect *self)
{
}

/* ------------------------------------------------------------------------ */
/* LgObjLink: a link to a value, opens the object inspector                   */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_OBJ_LINK (lg_obj_link_get_type ())
G_DECLARE_FINAL_TYPE (LgObjLink, lg_obj_link, LG, OBJ_LINK, StButton)

struct _LgObjLink
{
  StButton parent;

  ShellLookingGlass *looking_glass; /* weak */
  guint64 id;                       /* a result or pin of the inspector, or 0 */
  GObject *object;                  /* weak, put in the inspector when clicked */
};

G_DEFINE_FINAL_TYPE (LgObjLink, lg_obj_link, ST_TYPE_BUTTON)

static void
lg_obj_link_clicked (StButton *button,
                     int       clicked_button)
{
  LgObjLink *self = LG_OBJ_LINK (button);

  if (self->looking_glass == NULL)
    return;

  if (self->id != 0)
    lg_inspect_value (self->looking_glass, self->id, CLUTTER_ACTOR (self));
  else if (self->object != NULL)
    lg_inspect_object (self->looking_glass, self->object, CLUTTER_ACTOR (self));
}

static void
lg_obj_link_dispose (GObject *object)
{
  LgObjLink *self = LG_OBJ_LINK (object);

  g_clear_weak_pointer (&self->looking_glass);
  g_clear_weak_pointer (&self->object);

  G_OBJECT_CLASS (lg_obj_link_parent_class)->dispose (object);
}

static void
lg_obj_link_class_init (LgObjLinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = lg_obj_link_dispose;
  button_class->clicked = lg_obj_link_clicked;
}

static void
lg_obj_link_init (LgObjLink *self)
{
}

/* A link which inspects the value with the id (result or pin) if @id is not
 * 0, otherwise @object. */
static ClutterActor *
lg_obj_link_new (ShellLookingGlass *looking_glass,
                 guint64            id,
                 GObject           *object,
                 const char        *text)
{
  LgObjLink *self;
  ClutterActor *child;

  self = g_object_new (LG_TYPE_OBJ_LINK,
                       "reactive", TRUE,
                       "track-hover", TRUE,
                       "style-class", "shell-link",
                       "label", text,
                       "x-align", CLUTTER_ACTOR_ALIGN_START,
                       NULL);

  child = st_bin_get_child (ST_BIN (self));
  if (child != NULL && ST_IS_LABEL (child))
    clutter_text_set_single_line_mode (label_get_text (child), TRUE);

  g_set_weak_pointer (&self->looking_glass, looking_glass);
  self->id = id;
  if (object != NULL)
    g_set_weak_pointer (&self->object, object);

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* LgResult: one command of the Evaluator with what it printed and returned   */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_RESULT (lg_result_get_type ())
G_DECLARE_FINAL_TYPE (LgResult, lg_result, LG, RESULT, StBoxLayout)

struct _LgResult
{
  StBoxLayout parent;

  ShellLuaInspector *inspector; /* strong */
  GArray *ids;                  /* the results of the inspector it holds */
};

G_DEFINE_FINAL_TYPE (LgResult, lg_result, ST_TYPE_BOX_LAYOUT)

static void
lg_result_dispose (GObject *object)
{
  LgResult *self = LG_RESULT (object);

  if (self->inspector != NULL && self->ids != NULL)
    {
      guint i;

      /* `r (n)` of a row that is gone is an unknown result, as in the JS */
      for (i = 0; i < self->ids->len; i++)
        shell_lua_inspector_release (self->inspector,
                                     g_array_index (self->ids, guint64, i));
      g_array_set_size (self->ids, 0);
    }
  g_clear_object (&self->inspector);

  G_OBJECT_CLASS (lg_result_parent_class)->dispose (object);
}

static void
lg_result_finalize (GObject *object)
{
  LgResult *self = LG_RESULT (object);

  g_clear_pointer (&self->ids, g_array_unref);

  G_OBJECT_CLASS (lg_result_parent_class)->finalize (object);
}

static void
lg_result_class_init (LgResultClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = lg_result_dispose;
  object_class->finalize = lg_result_finalize;
}

static void
lg_result_init (LgResult *self)
{
  self->ids = g_array_new (FALSE, FALSE, sizeof (guint64));
}

static LgResult *
lg_result_new (ShellLuaInspector *inspector)
{
  LgResult *self = g_object_new (LG_TYPE_RESULT,
                                 "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                 NULL);

  self->inspector = g_object_ref (inspector);
  return self;
}

static void
lg_result_add_id (LgResult *self,
                  guint64   id)
{
  g_array_append_val (self->ids, id);
}

/* ------------------------------------------------------------------------ */
/* LgObjInspector: the properties of a value, in a window on Looking Glass    */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_OBJ_INSPECTOR (lg_obj_inspector_get_type ())
G_DECLARE_FINAL_TYPE (LgObjInspector, lg_obj_inspector,
                      LG, OBJ_INSPECTOR, StScrollView)

struct _LgObjInspector
{
  StScrollView parent;

  ShellLookingGlass *looking_glass; /* weak */
  ClutterActor *container;          /* weak, the child of the scroll view */

  guint64 id;          /* what is shown, or 0 */
  guint64 previous_id; /* what the Back button goes to, or 0 */
  GArray *pins;        /* the ids of the inspector the view made, released when closed */

  gboolean open;
  ClutterGrab *grab;
};

G_DEFINE_FINAL_TYPE (LgObjInspector, lg_obj_inspector, ST_TYPE_SCROLL_VIEW)

static void lg_obj_inspector_close (LgObjInspector *self);

static void
lg_obj_inspector_track_pin (LgObjInspector *self,
                            guint64         id)
{
  g_array_append_val (self->pins, id);
}

static void
lg_obj_inspector_release_pins (LgObjInspector *self)
{
  ShellLuaInspector *inspector;
  guint i;

  inspector = self->looking_glass != NULL ? lg_get_inspector (self->looking_glass) : NULL;
  if (inspector != NULL)
    {
      for (i = 0; i < self->pins->len; i++)
        shell_lua_inspector_release (inspector,
                                     g_array_index (self->pins, guint64, i));
    }
  g_array_set_size (self->pins, 0);
}

static void
obj_inspector_insert_cb (StButton       *button,
                         int             clicked_button,
                         LgObjInspector *self)
{
  guint64 id = self->id;

  if (self->looking_glass == NULL || id == 0)
    return;

  /* Insert while the value is still there, closing releases its pins */
  lg_insert_value (self->looking_glass, id);
  lg_obj_inspector_close (self);
}

static void
lg_obj_inspector_select_object (LgObjInspector *self,
                                guint64         id,
                                gboolean        skip_previous);

static void
obj_inspector_back_cb (StButton       *button,
                       int             clicked_button,
                       LgObjInspector *self)
{
  lg_obj_inspector_select_object (self, self->previous_id, TRUE);
}

static void
obj_inspector_close_cb (StButton       *button,
                        int             clicked_button,
                        LgObjInspector *self)
{
  lg_obj_inspector_close (self);
}

static void
lg_obj_inspector_select_object (LgObjInspector *self,
                                guint64         id,
                                gboolean        skip_previous)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) info = NULL;
  g_autoptr (GVariant) members = NULL;
  g_autofree char *title = NULL;
  const char *type_name = NULL, *summary = NULL;
  ClutterActor *hbox, *label, *button;
  GVariantIter iter;
  const char *key, *value;
  guint64 pin;

  if (self->looking_glass == NULL)
    return;

  self->previous_id = skip_previous ? 0 : self->id;
  self->id = id;

  clutter_actor_destroy_all_children (self->container);

  info = shell_lua_inspector_inspect (lg_get_inspector (self->looking_glass),
                                      id, &error);
  if (info != NULL)
    {
      g_variant_get (info, "(&s&s@a(sst))", &type_name, &summary, &members);
      title = g_strdup_printf ("Inspecting: %s: %s", type_name, summary);
    }
  else
    {
      title = g_strdup_printf ("Inspecting: <error> %s",
                               error != NULL ? error->message : "unknown error");
    }

  hbox = box_layout_new (NULL, "lg-obj-inspector-title", CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_actor_add_child (self->container, hbox);

  label = CLUTTER_ACTOR (st_label_new (title));
  clutter_actor_set_x_expand (label, TRUE);
  clutter_text_set_single_line_mode (label_get_text (label), TRUE);
  clutter_actor_add_child (hbox, label);

  button = g_object_new (ST_TYPE_BUTTON,
                         "label", "Insert",
                         "style-class", "lg-obj-inspector-button",
                         NULL);
  g_signal_connect (button, "clicked", G_CALLBACK (obj_inspector_insert_cb), self);
  clutter_actor_add_child (hbox, button);

  if (self->previous_id != 0)
    {
      button = g_object_new (ST_TYPE_BUTTON,
                             "label", "Back",
                             "style-class", "lg-obj-inspector-button",
                             NULL);
      g_signal_connect (button, "clicked", G_CALLBACK (obj_inspector_back_cb), self);
      clutter_actor_add_child (hbox, button);
    }

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "lg-obj-inspector-close-button",
                         "icon-name", "window-close-symbolic",
                         NULL);
  g_signal_connect (button, "clicked", G_CALLBACK (obj_inspector_close_cb), self);
  clutter_actor_add_child (hbox, button);

  if (members == NULL)
    return;

  g_variant_iter_init (&iter, members);
  while (g_variant_iter_next (&iter, "(&s&st)", &key, &value, &pin))
    {
      g_autofree char *key_text = g_strdup_printf ("%s: ", key);
      ClutterActor *box = g_object_new (ST_TYPE_BOX_LAYOUT, NULL);
      ClutterActor *link;

      clutter_actor_add_child (box, CLUTTER_ACTOR (st_label_new (key_text)));

      if (pin != 0)
        {
          lg_obj_inspector_track_pin (self, pin);
          link = lg_obj_link_new (self->looking_glass, pin, NULL, value);
        }
      else
        {
          link = CLUTTER_ACTOR (st_label_new (value));
        }
      clutter_actor_add_child (box, link);
      clutter_actor_add_child (self->container, box);
    }
}

static void
lg_obj_inspector_open (LgObjInspector *self,
                       gboolean        animate)
{
  if (self->open)
    return;

  self->grab = shell_main_push_modal (shell_main_get_default (),
                                      CLUTTER_ACTOR (self),
                                      SHELL_ACTION_MODE_LOOKING_GLASS);
  if (self->grab != NULL)
    g_object_ref (self->grab);

  self->previous_id = 0;
  self->open = TRUE;
  clutter_actor_show (CLUTTER_ACTOR (self));

  if (animate)
    {
      ShellEaseParams params = {
        .duration = OBJ_INSPECTOR_ANIMATION_TIME,
        .delay = -1,
        .mode = CLUTTER_EASE_OUT_QUAD,
        .repeat_count = 0,
        .auto_reverse = FALSE,
        .animation_required = FALSE,
      };

      clutter_actor_set_scale (CLUTTER_ACTOR (self), 0, 0);
      shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                        "scale-x", 1.0,
                        "scale-y", 1.0,
                        NULL);
    }
  else
    {
      clutter_actor_set_scale (CLUTTER_ACTOR (self), 1, 1);
    }
}

static void
lg_obj_inspector_close (LgObjInspector *self)
{
  g_autoptr (GError) error = NULL;

  if (!self->open)
    return;

  if (self->grab != NULL &&
      !shell_main_pop_modal (shell_main_get_default (), self->grab, &error))
    g_debug ("Could not pop the modal of the object inspector: %s", error->message);
  g_clear_object (&self->grab);

  self->open = FALSE;
  clutter_actor_hide (CLUTTER_ACTOR (self));
  self->previous_id = 0;
  self->id = 0;
  lg_obj_inspector_release_pins (self);
}

static gboolean
obj_inspector_close_binding_cb (GObject             *object,
                                const char          *action_name,
                                guint                key_val,
                                ClutterModifierType  modifiers,
                                gpointer             user_data)
{
  lg_obj_inspector_close (LG_OBJ_INSPECTOR (object));
  return CLUTTER_EVENT_STOP;
}

static void
lg_obj_inspector_constructed (GObject *object)
{
  LgObjInspector *self = LG_OBJ_INSPECTOR (object);
  ClutterActor *container;

  G_OBJECT_CLASS (lg_obj_inspector_parent_class)->constructed (object);

  clutter_actor_set_name (CLUTTER_ACTOR (self), "LookingGlassPropertyInspector");
  st_widget_set_style_class_name (ST_WIDGET (self), "lg-dialog");
  clutter_actor_set_pivot_point (CLUTTER_ACTOR (self), 0.5, 0.5);

  container = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            "x-expand", TRUE,
                            "y-expand", TRUE,
                            NULL);
  g_set_weak_pointer (&self->container, container);
  st_scroll_view_set_child (ST_SCROLL_VIEW (self), ST_SCROLLABLE (container));
}

static void
lg_obj_inspector_dispose (GObject *object)
{
  LgObjInspector *self = LG_OBJ_INSPECTOR (object);

  if (self->open)
    lg_obj_inspector_close (self);
  lg_obj_inspector_release_pins (self);

  g_clear_weak_pointer (&self->looking_glass);
  g_clear_weak_pointer (&self->container);
  g_clear_object (&self->grab);

  G_OBJECT_CLASS (lg_obj_inspector_parent_class)->dispose (object);
}

static void
lg_obj_inspector_finalize (GObject *object)
{
  LgObjInspector *self = LG_OBJ_INSPECTOR (object);

  g_clear_pointer (&self->pins, g_array_unref);

  G_OBJECT_CLASS (lg_obj_inspector_parent_class)->finalize (object);
}

static void
lg_obj_inspector_class_init (LgObjInspectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructed = lg_obj_inspector_constructed;
  object_class->dispose = lg_obj_inspector_dispose;
  object_class->finalize = lg_obj_inspector_finalize;

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool, "close",
                                       CLUTTER_KEY_Escape, 0,
                                       G_CALLBACK (obj_inspector_close_binding_cb),
                                       NULL, NULL);
}

static void
lg_obj_inspector_init (LgObjInspector *self)
{
  self->pins = g_array_new (FALSE, FALSE, sizeof (guint64));
}

static LgObjInspector *
lg_obj_inspector_new (ShellLookingGlass *looking_glass)
{
  LgObjInspector *self = g_object_new (LG_TYPE_OBJ_INSPECTOR, NULL);

  g_set_weak_pointer (&self->looking_glass, looking_glass);
  return self;
}

/* ------------------------------------------------------------------------ */
/* LgPicker: pick an actor of the scene (the "Inspect" button)                */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_PICKER (lg_picker_get_type ())
G_DECLARE_FINAL_TYPE (LgPicker, lg_picker, LG, PICKER, ClutterActor)

struct _LgPicker
{
  ClutterActor parent;

  ShellLookingGlass *looking_glass; /* weak */

  ClutterActor *event_handler; /* weak, NULL once closed */
  ClutterActor *display_text;  /* weak */
  ClutterGrab *grab;

  /* target is the actor currently shown by the picker, pointer_target is the
   * actor directly under the pointer. Normally these are the same, but if you
   * use the scroll wheel to drill down, they'll diverge until you either
   * scroll back out, or move the pointer outside of pointer_target. */
  ClutterActor *target;         /* weak */
  ClutterActor *pointer_target; /* weak */
};

G_DEFINE_FINAL_TYPE (LgPicker, lg_picker, CLUTTER_TYPE_ACTOR)

enum
{
  PICKER_CLOSED,
  PICKER_TARGET,

  PICKER_LAST_SIGNAL
};

static guint picker_signals[PICKER_LAST_SIGNAL] = { 0, };

static void
lg_picker_close (LgPicker *self)
{
  if (self->grab != NULL)
    {
      clutter_grab_dismiss (self->grab);
      g_clear_object (&self->grab);
    }

  if (self->event_handler != NULL)
    {
      ClutterActor *event_handler = self->event_handler;

      g_clear_weak_pointer (&self->event_handler);
      clutter_actor_destroy (event_handler);
    }

  g_signal_emit (self, picker_signals[PICKER_CLOSED], 0);
}

static void
lg_picker_update (LgPicker *self,
                  float     stage_x,
                  float     stage_y)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterActor *target;
  g_autofree char *description = NULL;
  g_autofree char *text = NULL;

  target = clutter_stage_get_actor_at_pos (stage, CLUTTER_PICK_ALL,
                                           stage_x, stage_y);

  if (target != self->pointer_target)
    g_set_weak_pointer (&self->target, target);
  g_set_weak_pointer (&self->pointer_target, target);

  if (self->display_text != NULL)
    {
      description = self->target != NULL ? describe_object (self->target)
                                          : g_strdup ("null");
      text = g_strdup_printf ("[inspect x: %g y: %g] %s",
                              stage_x, stage_y, description);
      st_label_set_text (ST_LABEL (self->display_text), "");
      st_label_set_text (ST_LABEL (self->display_text), text);
    }

  if (self->looking_glass != NULL)
    lg_set_border_paint_target (self->looking_glass, self->target);
}

static void
picker_motion_cb (ClutterMotionController *controller,
                  ClutterSprite           *sprite,
                  float                    x,
                  float                    y,
                  LgPicker                *self)
{
  graphene_point_t coords;

  clutter_sprite_get_coords (sprite, &coords);
  lg_picker_update (self, coords.x, coords.y);
}

static void
picker_scroll_cb (ClutterScrollController *controller,
                  ClutterSprite           *sprite,
                  ClutterScrollSource      source,
                  double                   dx,
                  double                   dy,
                  LgPicker                *self)
{
  graphene_point_t coords;

  clutter_sprite_get_coords (sprite, &coords);

  if (self->target == NULL)
    return;

  if (dy < 0)
    {
      /* select parent */
      ClutterActor *parent = clutter_actor_get_parent (self->target);

      if (parent != NULL)
        {
          g_set_weak_pointer (&self->target, parent);
          lg_picker_update (self, coords.x, coords.y);
        }
    }
  else if (dy > 0)
    {
      /* select child */
      if (self->target != self->pointer_target)
        {
          ClutterActor *child = self->pointer_target;

          while (child != NULL)
            {
              ClutterActor *parent = clutter_actor_get_parent (child);

              if (parent == self->target)
                break;
              child = parent;
            }

          if (child != NULL)
            {
              g_set_weak_pointer (&self->target, child);
              lg_picker_update (self, coords.x, coords.y);
            }
        }
    }
}

static void
picker_click_cb (ClutterClickGesture *gesture,
                 LgPicker            *self)
{
  graphene_point_t coords;

  if (self->target != NULL)
    {
      clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
      g_signal_emit (self, picker_signals[PICKER_TARGET], 0,
                     self->target, (double) coords.x, (double) coords.y);
    }

  lg_picker_close (self);
}

static gboolean
picker_key_press_cb (ClutterKeyController *controller,
                     LgPicker             *self)
{
  uint32_t symbol = 0;

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);
  if (symbol == CLUTTER_KEY_Escape)
    lg_picker_close (self);

  return CLUTTER_EVENT_PROPAGATE;
}

static void
lg_picker_allocate (ClutterActor          *actor,
                    const ClutterActorBox *box)
{
  LgPicker *self = LG_PICKER (actor);
  ClutterActorBox child_box;
  float nat_width, nat_height;
  int x, y, width, height;

  clutter_actor_set_allocation (actor, box);

  if (self->event_handler == NULL)
    return;

  get_primary_monitor (&x, &y, &width, &height);

  clutter_actor_get_preferred_size (self->event_handler, NULL, NULL,
                                    &nat_width, &nat_height);

  child_box.x1 = x + floorf ((width - nat_width) / 2);
  child_box.x2 = child_box.x1 + nat_width;
  child_box.y1 = y + floorf ((height - nat_height) / 2);
  child_box.y2 = child_box.y1 + nat_height;
  clutter_actor_allocate (self->event_handler, &child_box);
}

/* Called once the picker is in the ui group: the grab needs a mapped actor */
static void
lg_picker_populate (LgPicker *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterActor *event_handler, *display_text;
  ClutterAction *action;

  event_handler = box_layout_new ("LookingGlassDialog", NULL,
                                  CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_actor_set_reactive (event_handler, TRUE);
  g_set_weak_pointer (&self->event_handler, event_handler);
  clutter_actor_add_child (actor, event_handler);

  display_text = CLUTTER_ACTOR (st_label_new (""));
  clutter_actor_set_x_expand (display_text, TRUE);
  g_set_weak_pointer (&self->display_text, display_text);
  clutter_actor_add_child (event_handler, display_text);

  action = clutter_motion_controller_new ();
  g_signal_connect_object (action, "motion", G_CALLBACK (picker_motion_cb), self, 0);
  clutter_actor_add_action (event_handler, action);

  action = clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                          CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect_object (action, "scroll", G_CALLBACK (picker_scroll_cb), self, 0);
  clutter_actor_add_action (event_handler, action);

  action = clutter_click_gesture_new ();
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (action), TRUE);
  g_signal_connect_object (action, "recognize", G_CALLBACK (picker_click_cb), self, 0);
  clutter_actor_add_action (event_handler, action);

  action = CLUTTER_ACTION (clutter_key_controller_new (NULL));
  g_signal_connect_object (action, "key-press", G_CALLBACK (picker_key_press_cb), self, 0);
  clutter_actor_add_action (event_handler, action);

  self->grab = g_object_ref (clutter_stage_grab (stage, event_handler));
  clutter_actor_grab_key_focus (event_handler);
}

static void
lg_picker_dispose (GObject *object)
{
  LgPicker *self = LG_PICKER (object);

  if (self->grab != NULL)
    {
      clutter_grab_dismiss (self->grab);
      g_clear_object (&self->grab);
    }

  g_clear_weak_pointer (&self->looking_glass);
  g_clear_weak_pointer (&self->event_handler);
  g_clear_weak_pointer (&self->display_text);
  g_clear_weak_pointer (&self->target);
  g_clear_weak_pointer (&self->pointer_target);

  G_OBJECT_CLASS (lg_picker_parent_class)->dispose (object);
}

static void
lg_picker_class_init (LgPickerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = lg_picker_dispose;

  actor_class->allocate = lg_picker_allocate;

  picker_signals[PICKER_CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  picker_signals[PICKER_TARGET] =
    g_signal_new ("target",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  CLUTTER_TYPE_ACTOR, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
}

static void
lg_picker_init (LgPicker *self)
{
  clutter_actor_set_size (CLUTTER_ACTOR (self), 0, 0);
}

/* The picker is put in the ui group, above Looking Glass */
static LgPicker *
lg_picker_new (ShellLookingGlass *looking_glass)
{
  LgPicker *self = g_object_new (LG_TYPE_PICKER, NULL);
  ClutterActor *ui_group = get_ui_group ();

  g_set_weak_pointer (&self->looking_glass, looking_glass);
  if (ui_group != NULL)
    clutter_actor_add_child (ui_group, CLUTTER_ACTOR (self));
  lg_picker_populate (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* LgWindowList: the "Windows" page                                           */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_WINDOW_LIST (lg_window_list_get_type ())
G_DECLARE_FINAL_TYPE (LgWindowList, lg_window_list, LG, WINDOW_LIST, StBoxLayout)

struct _LgWindowList
{
  StBoxLayout parent;

  ShellLookingGlass *looking_glass; /* weak */
};

G_DEFINE_FINAL_TYPE (LgWindowList, lg_window_list, ST_TYPE_BOX_LAYOUT)

#define WINDOW_MANAGED_KEY "looking-glass-managed"

static void lg_window_list_update (LgWindowList *self);

static void
window_unmanaged_cb (MetaWindow   *window,
                     LgWindowList *self)
{
  lg_window_list_update (self);
}

static void
tracked_windows_changed_cb (ShellWindowTracker *tracker,
                            LgWindowList       *self)
{
  lg_window_list_update (self);
}

static void
display_window_created_cb (MetaDisplay *display,
                           MetaWindow  *window,
                           LgWindowList *self)
{
  lg_window_list_update (self);
}

static void
lg_window_list_update (LgWindowList *self)
{
  ShellGlobal *global = shell_global_get ();
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  g_autoptr (GList) windows = NULL;
  GList *l;

  if (self->looking_glass == NULL ||
      !shell_looking_glass_get_is_open (self->looking_glass))
    return;

  clutter_actor_destroy_all_children (CLUTTER_ACTOR (self));

  windows = shell_global_get_window_actors (global);
  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *window = meta_window_actor_get_meta_window (META_WINDOW_ACTOR (l->data));
      const char *title = meta_window_get_title (window);
      const char *wm_class = meta_window_get_wm_class (window);
      g_autofree char *wmclass_text = NULL;
      g_autofree char *fallback = NULL;
      ShellApp *app;
      ClutterActor *box, *header, *window_link, *props_box;

      /* Avoid multiple connections */
      if (g_object_get_data (G_OBJECT (window), WINDOW_MANAGED_KEY) == NULL)
        {
          g_signal_connect_object (window, "unmanaged",
                                   G_CALLBACK (window_unmanaged_cb), self, 0);
          g_object_set_data (G_OBJECT (window), WINDOW_MANAGED_KEY,
                             GINT_TO_POINTER (TRUE));
        }

      box = box_layout_new (NULL, "lg-window", CLUTTER_ORIENTATION_VERTICAL);
      clutter_actor_add_child (CLUTTER_ACTOR (self), box);

      header = box_layout_new (NULL, "lg-window-name", CLUTTER_ORIENTATION_VERTICAL);
      clutter_actor_add_child (box, header);

      if (title == NULL || *title == '\0')
        title = fallback = describe_object (window);
      window_link = lg_obj_link_new (self->looking_glass, 0, G_OBJECT (window), title);
      clutter_actor_add_child (header, window_link);

      props_box = box_layout_new (NULL, "lg-window-props-box", CLUTTER_ORIENTATION_VERTICAL);
      clutter_actor_add_child (box, props_box);

      wmclass_text = g_strdup_printf ("wmclass: %s", wm_class != NULL ? wm_class : "null");
      clutter_actor_add_child (props_box, CLUTTER_ACTOR (st_label_new (wmclass_text)));

      app = shell_window_tracker_get_window_app (tracker, window);
      if (app != NULL && !shell_app_is_window_backed (app))
        {
          ClutterActor *icon = shell_app_create_icon_texture (app, 22);
          ClutterActor *prop_box = box_layout_new (NULL, "lg-window-props",
                                                   CLUTTER_ORIENTATION_HORIZONTAL);
          ClutterActor *app_link;

          clutter_actor_add_child (props_box, prop_box);
          clutter_actor_add_child (prop_box, CLUTTER_ACTOR (st_label_new ("app: ")));
          app_link = lg_obj_link_new (self->looking_glass, 0, G_OBJECT (app),
                                      shell_app_get_id (app));
          clutter_actor_add_child (prop_box, app_link);
          clutter_actor_add_child (prop_box, icon);
        }
      else
        {
          clutter_actor_add_child (props_box, CLUTTER_ACTOR (st_label_new ("<untracked>")));
        }
    }
}

static void
lg_window_list_dispose (GObject *object)
{
  LgWindowList *self = LG_WINDOW_LIST (object);

  g_clear_weak_pointer (&self->looking_glass);

  G_OBJECT_CLASS (lg_window_list_parent_class)->dispose (object);
}

static void
lg_window_list_class_init (LgWindowListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = lg_window_list_dispose;
}

static void
lg_window_list_init (LgWindowList *self)
{
}

static LgWindowList *
lg_window_list_new (ShellLookingGlass *looking_glass)
{
  LgWindowList *self = g_object_new (LG_TYPE_WINDOW_LIST,
                                     "name", "LookingGlassWindows",
                                     "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                     NULL);

  g_set_weak_pointer (&self->looking_glass, looking_glass);

  g_signal_connect_object (shell_global_get_display (shell_global_get ()),
                           "window-created",
                           G_CALLBACK (display_window_created_cb), self, 0);
  g_signal_connect_object (shell_window_tracker_get_default (),
                           "tracked-windows-changed",
                           G_CALLBACK (tracked_windows_changed_cb), self, 0);

  return self;
}

/* ------------------------------------------------------------------------ */
/* LgExtensions: the "Extensions" page, the Lua extensions                    */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_EXTENSIONS (lg_extensions_get_type ())
G_DECLARE_FINAL_TYPE (LgExtensions, lg_extensions, LG, EXTENSIONS, StBoxLayout)

struct _LgExtensions
{
  StBoxLayout parent;

  ShellLookingGlass *looking_glass; /* weak */
  ClutterActor *list;               /* weak */
};

G_DEFINE_FINAL_TYPE (LgExtensions, lg_extensions, ST_TYPE_BOX_LAYOUT)

static const char *
extension_state_to_string (ShellLuaExtensionState state)
{
  switch (state)
    {
    case SHELL_LUA_EXTENSION_STATE_ACTIVE:
      return _("Active");
    case SHELL_LUA_EXTENSION_STATE_INACTIVE:
    case SHELL_LUA_EXTENSION_STATE_INITIALIZED:
      return _("Inactive");
    case SHELL_LUA_EXTENSION_STATE_ERROR:
      return _("Error");
    case SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE:
      return _("Out of date");
    case SHELL_LUA_EXTENSION_STATE_DOWNLOADING:
      return _("Downloading");
    case SHELL_LUA_EXTENSION_STATE_DEACTIVATING:
      return _("Deactivating");
    case SHELL_LUA_EXTENSION_STATE_ACTIVATING:
      return _("Activating");
    default:
      break;
    }

  return "Unknown"; /* Not translated, shouldn't appear */
}

static const char *
extension_get_string (ShellLuaExtension *extension,
                      const char        *member)
{
  JsonObject *metadata = shell_lua_extension_get_metadata (extension);

  if (metadata == NULL || !json_object_has_member (metadata, member))
    return NULL;

  return json_object_get_string_member_with_default (metadata, member, NULL);
}

static const char *
extension_get_name (ShellLuaExtension *extension)
{
  const char *name = extension_get_string (extension, "name");

  return name != NULL ? name : shell_lua_extension_get_uuid (extension);
}

static void
extension_view_source_cb (StButton     *button,
                          int           clicked_button,
                          LgExtensions *self)
{
  ShellLuaExtension *extension = g_object_get_data (G_OBJECT (button), "extension");
  g_autofree char *uri = g_file_get_uri (shell_lua_extension_get_dir (extension));
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;

  context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  if (!g_app_info_launch_default_for_uri (uri, context, &error))
    g_message ("Could not open %s: %s", uri, error->message);

  if (self->looking_glass != NULL)
    shell_looking_glass_close (self->looking_glass);
}

static void
extension_web_page_cb (StButton     *button,
                       int           clicked_button,
                       LgExtensions *self)
{
  ShellLuaExtension *extension = g_object_get_data (G_OBJECT (button), "extension");
  const char *url = extension_get_string (extension, "url");
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;

  if (url == NULL)
    return;

  context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  if (!g_app_info_launch_default_for_uri (url, context, &error))
    g_message ("Could not open %s: %s", url, error->message);

  if (self->looking_glass != NULL)
    shell_looking_glass_close (self->looking_glass);
}

static void
extension_view_errors_cb (StButton     *button,
                          int           clicked_button,
                          LgExtensions *self)
{
  ShellLuaExtension *extension = g_object_get_data (G_OBJECT (button), "extension");
  ClutterActor *row = g_object_get_data (G_OBJECT (button), "row");
  ClutterActor *error_display = g_object_get_data (G_OBJECT (button), "error-display");

  if (error_display == NULL)
    {
      const char *error = shell_lua_extension_get_error (extension);

      error_display = g_object_new (ST_TYPE_BOX_LAYOUT,
                                    "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                    NULL);
      if (error != NULL && *error != '\0')
        {
          g_autofree char *valid = g_utf8_make_valid (error, -1);

          clutter_actor_add_child (error_display, CLUTTER_ACTOR (st_label_new (valid)));
        }
      else
        {
          /* Translators: argument is an extension UUID. */
          g_autofree char *message = g_strdup_printf (_("%s has not emitted any errors"),
                                                      shell_lua_extension_get_uuid (extension));

          clutter_actor_add_child (error_display, CLUTTER_ACTOR (st_label_new (message)));
        }

      g_object_set_data (G_OBJECT (button), "error-display", error_display);
      clutter_actor_add_child (row, error_display);
      st_button_set_label (button, _("Hide Errors"));
    }
  else
    {
      clutter_actor_destroy (error_display);
      g_object_set_data (G_OBJECT (button), "error-display", NULL);
      st_button_set_label (button, _("Show Errors"));
    }
}

static ClutterActor *
link_button_new (const char *label)
{
  return g_object_new (ST_TYPE_BUTTON,
                       "reactive", TRUE,
                       "track-hover", TRUE,
                       "style-class", "shell-link",
                       "label", label,
                       NULL);
}

static ClutterActor *
extension_display_new (LgExtensions      *self,
                       ShellLuaExtension *extension)
{
  ClutterActor *box, *meta_box, *state, *view_source, *view_errors;
  ShellLuaState *lua_state = shell_lua_extension_get_lua_state (extension);
  const char *description = extension_get_string (extension, "description");

  box = box_layout_new (NULL, "lg-extension", CLUTTER_ORIENTATION_VERTICAL);

  clutter_actor_add_child (box, g_object_new (ST_TYPE_LABEL,
                                              "style-class", "lg-extension-name",
                                              "text", extension_get_name (extension),
                                              "x-expand", TRUE,
                                              NULL));
  clutter_actor_add_child (box, g_object_new (ST_TYPE_LABEL,
                                              "style-class", "lg-extension-description",
                                              "text", description != NULL && *description != '\0' ?
                                                      description : "No description",
                                              "x-expand", TRUE,
                                              NULL));

  meta_box = box_layout_new (NULL, "lg-extension-meta", CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_actor_add_child (box, meta_box);

  state = g_object_new (ST_TYPE_LABEL,
                        "style-class", "lg-extension-state",
                        "text", extension_state_to_string (shell_lua_extension_get_state (extension)),
                        NULL);
  clutter_actor_add_child (meta_box, state);

  if (lua_state != NULL && shell_lua_state_is_alive (lua_state))
    {
      guint n_errors = shell_lua_state_get_error_count (lua_state);
      g_autofree char *heap = g_format_size (shell_lua_state_get_memory_usage (lua_state));
      g_autofree char *heap_text = g_strdup_printf (_("Lua heap: %s"), heap);
      g_autofree char *errors_text = g_strdup_printf (ngettext ("%u error", "%u errors", n_errors),
                                                      n_errors);
      g_autofree char *usage = g_strdup_printf ("%s, %s", heap_text, errors_text);

      clutter_actor_add_child (meta_box, CLUTTER_ACTOR (st_label_new (usage)));
    }

  /* What its callbacks cost since it was enabled: the line to look at when
   * the desktop stutters */
  if (lua_state != NULL && shell_lua_state_is_alive (lua_state))
    {
      ShellLuaStats stats;
      g_autofree char *calls = NULL;
      g_autofree char *slow = NULL;
      g_autofree char *text = NULL;

      shell_lua_state_get_stats (lua_state, &stats);
      calls = g_strdup_printf (ngettext ("%lu callback, %.1f ms in total",
                                         "%lu callbacks, %.1f ms in total",
                                         (unsigned long) stats.calls),
                               (unsigned long) stats.calls, stats.total_us / 1000.0);
      slow = g_strdup_printf (ngettext ("%lu longer than a frame",
                                        "%lu longer than a frame",
                                        (unsigned long) stats.slow_calls),
                              (unsigned long) stats.slow_calls);
      if (stats.longest_where != NULL)
        text = g_strdup_printf (_("%s, longest %.1f ms (%s), %s"), calls,
                                stats.longest_us / 1000.0, stats.longest_where, slow);
      else
        text = g_strdup (calls);

      clutter_actor_add_child (box, g_object_new (ST_TYPE_LABEL,
                                                  "style-class", "lg-extension-stats",
                                                  "text", text,
                                                  "x-expand", TRUE,
                                                  NULL));
    }

  view_source = link_button_new (_("View Source"));
  g_object_set_data_full (G_OBJECT (view_source), "extension",
                          g_object_ref (extension), g_object_unref);
  g_signal_connect (view_source, "clicked", G_CALLBACK (extension_view_source_cb), self);
  clutter_actor_add_child (meta_box, view_source);

  if (extension_get_string (extension, "url") != NULL)
    {
      ClutterActor *web_page = link_button_new (_("Web Page"));

      g_object_set_data_full (G_OBJECT (web_page), "extension",
                              g_object_ref (extension), g_object_unref);
      g_signal_connect (web_page, "clicked", G_CALLBACK (extension_web_page_cb), self);
      clutter_actor_add_child (meta_box, web_page);
    }

  view_errors = link_button_new (_("Show Errors"));
  g_object_set_data_full (G_OBJECT (view_errors), "extension",
                          g_object_ref (extension), g_object_unref);
  g_object_set_data (G_OBJECT (view_errors), "row", box);
  g_signal_connect (view_errors, "clicked", G_CALLBACK (extension_view_errors_cb), self);
  clutter_actor_add_child (meta_box, view_errors);

  return box;
}

static int
compare_extensions (gconstpointer a,
                    gconstpointer b)
{
  return g_utf8_collate (extension_get_name ((ShellLuaExtension *) a),
                         extension_get_name ((ShellLuaExtension *) b));
}

/* The state labels are not live: all the rows are made again when an
 * extension changes. */
static void
lg_extensions_sync (LgExtensions *self)
{
  ShellLuaExtensionManager *manager =
    shell_global_get_lua_extension_manager (shell_global_get ());
  GList *extensions, *l;

  clutter_actor_destroy_all_children (self->list);

  extensions = shell_lua_extension_manager_list (manager);
  extensions = g_list_sort (extensions, compare_extensions);

  if (extensions == NULL)
    {
      clutter_actor_add_child (self->list,
                               g_object_new (ST_TYPE_LABEL,
                                             "style-class", "lg-extensions-none",
                                             "text", _("No extensions installed"),
                                             NULL));
      return;
    }

  for (l = extensions; l != NULL; l = l->next)
    clutter_actor_add_child (self->list, extension_display_new (self, l->data));

  g_list_free (extensions);
}

static void
extension_state_changed_cb (ShellLuaExtensionManager *manager,
                            ShellLuaExtension        *extension,
                            LgExtensions             *self)
{
  lg_extensions_sync (self);
}

static void
extensions_loaded_cb (ShellLuaExtensionManager *manager,
                      LgExtensions             *self)
{
  lg_extensions_sync (self);
}

/* The figures of the rows are not live either: refresh them whenever the
 * tab is shown */
static void
extensions_mapped_cb (LgExtensions *self,
                      GParamSpec   *pspec,
                      gpointer      user_data)
{
  if (clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
    lg_extensions_sync (self);
}

static void
lg_extensions_dispose (GObject *object)
{
  LgExtensions *self = LG_EXTENSIONS (object);

  g_clear_weak_pointer (&self->looking_glass);
  g_clear_weak_pointer (&self->list);

  G_OBJECT_CLASS (lg_extensions_parent_class)->dispose (object);
}

static void
lg_extensions_class_init (LgExtensionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = lg_extensions_dispose;
}

static void
lg_extensions_init (LgExtensions *self)
{
}

static LgExtensions *
lg_extensions_new (ShellLookingGlass *looking_glass)
{
  LgExtensions *self = g_object_new (LG_TYPE_EXTENSIONS,
                                     "name", "LookingGlassExtensions",
                                     "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                     NULL);
  ShellLuaExtensionManager *manager =
    shell_global_get_lua_extension_manager (shell_global_get ());
  ClutterActor *list;

  g_set_weak_pointer (&self->looking_glass, looking_glass);

  list = box_layout_new (NULL, "lg-extensions-list", CLUTTER_ORIENTATION_VERTICAL);
  g_set_weak_pointer (&self->list, list);
  clutter_actor_add_child (CLUTTER_ACTOR (self), list);

  lg_extensions_sync (self);

  g_signal_connect_object (manager, "extension-state-changed",
                           G_CALLBACK (extension_state_changed_cb), self, 0);
  g_signal_connect_object (manager, "extensions-loaded",
                           G_CALLBACK (extensions_loaded_cb), self, 0);
  g_signal_connect (self, "notify::mapped", G_CALLBACK (extensions_mapped_cb), NULL);

  return self;
}

/* ------------------------------------------------------------------------ */
/* LgActorLink and LgActorTreeViewer: the "Actors" page                       */
/* ------------------------------------------------------------------------ */

#define LG_TYPE_ACTOR_LINK (lg_actor_link_get_type ())
G_DECLARE_FINAL_TYPE (LgActorLink, lg_actor_link, LG, ACTOR_LINK, StButton)

struct _LgActorLink
{
  StButton parent;

  ClutterActor *arrow; /* weak */
};

G_DEFINE_FINAL_TYPE (LgActorLink, lg_actor_link, ST_TYPE_BUTTON)

enum
{
  ACTOR_LINK_INSPECT_ACTOR,

  ACTOR_LINK_LAST_SIGNAL
};

static guint actor_link_signals[ACTOR_LINK_LAST_SIGNAL] = { 0, };

static void
lg_actor_link_clicked (StButton *button,
                       int       clicked_button)
{
  LgActorLink *self = LG_ACTOR_LINK (button);
  ShellEaseParams params = {
    .duration = 250,
    .delay = -1,
    .mode = 0,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (self->arrow == NULL)
    return;

  shell_actor_ease (self->arrow, &params, NULL, NULL, NULL,
                    "rotation-angle-z", st_button_get_checked (button) ? 90.0 : 0.0,
                    NULL);
}

static void
lg_actor_link_dispose (GObject *object)
{
  LgActorLink *self = LG_ACTOR_LINK (object);

  g_clear_weak_pointer (&self->arrow);

  G_OBJECT_CLASS (lg_actor_link_parent_class)->dispose (object);
}

static void
lg_actor_link_class_init (LgActorLinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = lg_actor_link_dispose;
  button_class->clicked = lg_actor_link_clicked;

  actor_link_signals[ACTOR_LINK_INSPECT_ACTOR] =
    g_signal_new ("inspect-actor",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
lg_actor_link_init (LgActorLink *self)
{
}

static void
actor_link_inspect_button_clicked_cb (StButton    *button,
                                      int          clicked_button,
                                      LgActorLink *self)
{
  g_signal_emit (self, actor_link_signals[ACTOR_LINK_INSPECT_ACTOR], 0);
}

static ClutterActor *
lg_actor_link_new (ClutterActor *actor)
{
  g_autofree char *description = describe_object (actor);
  ClutterActor *arrow, *label, *inspect_button, *box;
  LgActorLink *self;

  arrow = g_object_new (ST_TYPE_ICON,
                        "icon-name", "pan-end-symbolic",
                        "icon-size", 12,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_set_pivot_point (arrow, 0.5, 0.5);

  label = g_object_new (ST_TYPE_LABEL,
                        "text", description,
                        "x-align", CLUTTER_ACTOR_ALIGN_START,
                        NULL);

  inspect_button = g_object_new (ST_TYPE_BUTTON,
                                 "icon-name", "insert-object-symbolic",
                                 "reactive", TRUE,
                                 "x-expand", TRUE,
                                 "x-align", CLUTTER_ACTOR_ALIGN_START,
                                 "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 NULL);

  box = g_object_new (ST_TYPE_BOX_LAYOUT, NULL);
  clutter_actor_add_child (box, arrow);
  clutter_actor_add_child (box, label);
  clutter_actor_add_child (box, inspect_button);

  self = g_object_new (LG_TYPE_ACTOR_LINK,
                       "reactive", TRUE,
                       "track-hover", TRUE,
                       "toggle-mode", TRUE,
                       "style-class", "actor-link",
                       "child", box,
                       "x-align", CLUTTER_ACTOR_ALIGN_START,
                       NULL);
  g_set_weak_pointer (&self->arrow, arrow);
  g_signal_connect (inspect_button, "clicked",
                    G_CALLBACK (actor_link_inspect_button_clicked_cb), self);

  return CLUTTER_ACTOR (self);
}

#define LG_TYPE_ACTOR_TREE_VIEWER (lg_actor_tree_viewer_get_type ())
G_DECLARE_FINAL_TYPE (LgActorTreeViewer, lg_actor_tree_viewer,
                      LG, ACTOR_TREE_VIEWER, StBoxLayout)

struct _LgActorTreeViewer
{
  StBoxLayout parent;

  ShellLookingGlass *looking_glass; /* weak */
  GHashTable *actor_data;           /* ClutterActor* -> ActorData */
};

G_DEFINE_FINAL_TYPE (LgActorTreeViewer, lg_actor_tree_viewer, ST_TYPE_BOX_LAYOUT)

typedef struct
{
  LgActorTreeViewer *viewer;
  ClutterActor *actor;
  ClutterActor *button;
  ClutterActor *container;
  ClutterActor *children;
  gboolean visible;
  gulong actor_added_id;
  gulong actor_removed_id;
  gulong actor_destroyed_id;
} ActorData;

static void actor_tree_add_actor    (LgActorTreeViewer *self,
                                     ClutterActor      *container,
                                     ClutterActor      *actor);
static void actor_tree_remove_actor (LgActorTreeViewer *self,
                                     ClutterActor      *actor);

static void
actor_data_disconnect (ActorData *data)
{
  g_clear_signal_handler (&data->actor_added_id, data->actor);
  g_clear_signal_handler (&data->actor_removed_id, data->actor);
  g_clear_signal_handler (&data->actor_destroyed_id, data->actor);
}

static void
actor_data_free (ActorData *data)
{
  g_free (data);
}

static void
actor_child_added_cb (ClutterActor *actor,
                      ClutterActor *child,
                      ActorData    *data)
{
  actor_tree_add_actor (data->viewer, data->children, child);
}

static void
actor_child_removed_cb (ClutterActor *actor,
                        ClutterActor *child,
                        ActorData    *data)
{
  actor_tree_remove_actor (data->viewer, child);
}

static void
actor_destroyed_cb (ClutterActor *actor,
                    ActorData    *data)
{
  actor_tree_remove_actor (data->viewer, actor);
}

static void
actor_tree_show_children (LgActorTreeViewer *self,
                          ClutterActor      *actor)
{
  ActorData *data = g_hash_table_lookup (self->actor_data, actor);
  ClutterActor *child;

  if (data == NULL || data->visible)
    return;

  data->visible = TRUE;
  data->actor_added_id = g_signal_connect (actor, "child-added",
                                           G_CALLBACK (actor_child_added_cb), data);
  data->actor_removed_id = g_signal_connect (actor, "child-removed",
                                             G_CALLBACK (actor_child_removed_cb), data);

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    actor_tree_add_actor (self, data->children, child);
}

static void
actor_tree_hide_children (LgActorTreeViewer *self,
                          ClutterActor      *actor)
{
  ActorData *data = g_hash_table_lookup (self->actor_data, actor);
  ClutterActor *child;

  if (data == NULL || !data->visible)
    return;

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    actor_tree_remove_actor (self, child);

  data->visible = FALSE;
  g_clear_signal_handler (&data->actor_added_id, actor);
  g_clear_signal_handler (&data->actor_removed_id, actor);
  clutter_actor_remove_all_children (data->children);
}

static void
actor_link_checked_cb (GObject     *object,
                       GParamSpec  *pspec,
                       ActorData   *data)
{
  LgActorTreeViewer *self = data->viewer;

  if (self->looking_glass != NULL)
    lg_set_border_paint_target (self->looking_glass, data->actor);

  if (st_button_get_checked (ST_BUTTON (object)))
    actor_tree_show_children (self, data->actor);
  else
    actor_tree_hide_children (self, data->actor);
}

static void
actor_link_inspect_actor_cb (LgActorLink *button,
                             ActorData   *data)
{
  LgActorTreeViewer *self = data->viewer;

  if (self->looking_glass != NULL)
    lg_inspect_object (self->looking_glass, G_OBJECT (data->actor),
                       CLUTTER_ACTOR (button));
}

static void
actor_tree_add_actor (LgActorTreeViewer *self,
                      ClutterActor      *container,
                      ClutterActor      *actor)
{
  ClutterActor *button, *main_container, *children_container;
  ClutterActor *below_child = NULL;
  ClutterActor *next_sibling;
  ActorData *data;

  if (g_hash_table_contains (self->actor_data, actor))
    return;

  if (self->looking_glass != NULL && actor == CLUTTER_ACTOR (self->looking_glass))
    return;

  data = g_new0 (ActorData, 1);
  data->viewer = self;
  data->actor = actor;

  button = lg_actor_link_new (actor);
  g_signal_connect (button, "notify::checked", G_CALLBACK (actor_link_checked_cb), data);
  g_signal_connect (button, "inspect-actor", G_CALLBACK (actor_link_inspect_actor_cb), data);

  main_container = g_object_new (ST_TYPE_BOX_LAYOUT,
                                 "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                 NULL);
  children_container = g_object_new (ST_TYPE_BOX_LAYOUT,
                                     "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                     "style", "padding: 0 0 0 18px",
                                     NULL);

  clutter_actor_add_child (main_container, button);
  clutter_actor_add_child (main_container, children_container);

  data->button = button;
  data->container = main_container;
  data->children = children_container;
  data->visible = FALSE;
  data->actor_destroyed_id = g_signal_connect (actor, "destroy",
                                               G_CALLBACK (actor_destroyed_cb), data);
  g_hash_table_insert (self->actor_data, actor, data);

  next_sibling = clutter_actor_get_next_sibling (actor);
  if (next_sibling != NULL)
    {
      ActorData *next_data = g_hash_table_lookup (self->actor_data, next_sibling);

      if (next_data != NULL)
        below_child = next_data->container;
    }

  clutter_actor_insert_child_above (container, main_container, below_child);
}

static void
actor_tree_remove_actor (LgActorTreeViewer *self,
                         ClutterActor      *actor)
{
  ActorData *data = g_hash_table_lookup (self->actor_data, actor);
  ClutterActor *child;

  if (data == NULL)
    return;

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    actor_tree_remove_actor (self, child);

  actor_data_disconnect (data);
  clutter_actor_destroy (data->container);
  g_hash_table_remove (self->actor_data, actor);
}

static void
lg_actor_tree_viewer_map (ClutterActor *actor)
{
  LgActorTreeViewer *self = LG_ACTOR_TREE_VIEWER (actor);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  CLUTTER_ACTOR_CLASS (lg_actor_tree_viewer_parent_class)->map (actor);

  actor_tree_add_actor (self, actor, CLUTTER_ACTOR (stage));
}

static void
lg_actor_tree_viewer_unmap (ClutterActor *actor)
{
  LgActorTreeViewer *self = LG_ACTOR_TREE_VIEWER (actor);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  CLUTTER_ACTOR_CLASS (lg_actor_tree_viewer_parent_class)->unmap (actor);

  actor_tree_remove_actor (self, CLUTTER_ACTOR (stage));
}

static void
lg_actor_tree_viewer_dispose (GObject *object)
{
  LgActorTreeViewer *self = LG_ACTOR_TREE_VIEWER (object);

  /* The rows are children of the viewer and go away with it; what has to
   * be undone are the handlers on the actors of the scene. */
  if (self->actor_data != NULL)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->actor_data);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        actor_data_disconnect (value);
      g_hash_table_remove_all (self->actor_data);
    }

  g_clear_weak_pointer (&self->looking_glass);

  G_OBJECT_CLASS (lg_actor_tree_viewer_parent_class)->dispose (object);
}

static void
lg_actor_tree_viewer_finalize (GObject *object)
{
  LgActorTreeViewer *self = LG_ACTOR_TREE_VIEWER (object);

  g_clear_pointer (&self->actor_data, g_hash_table_unref);

  G_OBJECT_CLASS (lg_actor_tree_viewer_parent_class)->finalize (object);
}

static void
lg_actor_tree_viewer_class_init (LgActorTreeViewerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = lg_actor_tree_viewer_dispose;
  object_class->finalize = lg_actor_tree_viewer_finalize;

  actor_class->map = lg_actor_tree_viewer_map;
  actor_class->unmap = lg_actor_tree_viewer_unmap;
}

static void
lg_actor_tree_viewer_init (LgActorTreeViewer *self)
{
  self->actor_data = g_hash_table_new_full (NULL, NULL, NULL,
                                            (GDestroyNotify) actor_data_free);
}

static LgActorTreeViewer *
lg_actor_tree_viewer_new (ShellLookingGlass *looking_glass)
{
  LgActorTreeViewer *self = g_object_new (LG_TYPE_ACTOR_TREE_VIEWER,
                                          "name", "LookingGlassActors",
                                          NULL);

  g_set_weak_pointer (&self->looking_glass, looking_glass);
  return self;
}

/* ------------------------------------------------------------------------ */
/* LgDebugFlag: the "Flags" page                                              */
/* ------------------------------------------------------------------------ */

typedef enum
{
  LG_FLAG_CLUTTER_DEBUG,
  LG_FLAG_CLUTTER_DRAW,
  LG_FLAG_META_PAINT,
  LG_FLAG_META_TOPIC,
  LG_FLAG_UNSAFE_MODE,
  LG_FLAG_DEBUG_CONTROL,
} LgFlagKind;

#define LG_TYPE_DEBUG_FLAG (lg_debug_flag_get_type ())
G_DECLARE_FINAL_TYPE (LgDebugFlag, lg_debug_flag, LG, DEBUG_FLAG, StButton)

struct _LgDebugFlag
{
  StButton parent;

  LgFlagKind kind;
  int value; /* the flag, the topic */

  ShellSwitch *flag_switch; /* weak */
  gulong state_handler_id;
};

G_DEFINE_FINAL_TYPE (LgDebugFlag, lg_debug_flag, ST_TYPE_BUTTON)

static MetaContext *
get_context (void)
{
  return shell_global_get_context (shell_global_get ());
}

static gboolean
lg_debug_flag_is_enabled (LgDebugFlag *self)
{
  gboolean enabled = FALSE;

  switch (self->kind)
    {
    case LG_FLAG_CLUTTER_DEBUG:
      {
        ClutterDebugFlag flags = 0;
        ClutterDrawDebugFlag draw_flags = 0;
        ClutterPickDebugFlag pick_flags = 0;

        clutter_get_debug_flags (&flags, &draw_flags, &pick_flags);
        return (flags & self->value) != 0;
      }
    case LG_FLAG_CLUTTER_DRAW:
      {
        ClutterDebugFlag debug_flags = 0;
        ClutterDrawDebugFlag flags = 0;
        ClutterPickDebugFlag pick_flags = 0;

        clutter_get_debug_flags (&debug_flags, &flags, &pick_flags);
        return (flags & self->value) != 0;
      }
    case LG_FLAG_META_PAINT:
      return (meta_get_debug_paint_flags () & self->value) != 0;
    case LG_FLAG_META_TOPIC:
      return meta_is_topic_enabled (self->value);
    case LG_FLAG_UNSAFE_MODE:
      g_object_get (get_context (), "unsafe-mode", &enabled, NULL);
      return enabled;
    case LG_FLAG_DEBUG_CONTROL:
      g_object_get (meta_context_get_debug_control (get_context ()),
                    "exported", &enabled, NULL);
      return enabled;
    default:
      break;
    }

  return FALSE;
}

static void
lg_debug_flag_set_enabled (LgDebugFlag *self,
                           gboolean     enabled)
{
  switch (self->kind)
    {
    case LG_FLAG_CLUTTER_DEBUG:
      if (enabled)
        clutter_add_debug_flags (self->value, 0, 0);
      else
        clutter_remove_debug_flags (self->value, 0, 0);
      break;
    case LG_FLAG_CLUTTER_DRAW:
      if (enabled)
        clutter_add_debug_flags (0, self->value, 0);
      else
        clutter_remove_debug_flags (0, self->value, 0);
      break;
    case LG_FLAG_META_PAINT:
      if (enabled)
        meta_add_debug_paint_flag (self->value);
      else
        meta_remove_debug_paint_flag (self->value);
      break;
    case LG_FLAG_META_TOPIC:
      if (enabled)
        meta_add_verbose_topic (self->value);
      else
        meta_remove_verbose_topic (self->value);
      break;
    case LG_FLAG_UNSAFE_MODE:
      g_object_set (get_context (), "unsafe-mode", enabled, NULL);
      break;
    case LG_FLAG_DEBUG_CONTROL:
      g_object_set (meta_context_get_debug_control (get_context ()),
                    "exported", enabled, NULL);
      break;
    default:
      break;
    }
}

static void
flag_switch_state_cb (GObject     *object,
                      GParamSpec  *pspec,
                      LgDebugFlag *self)
{
  lg_debug_flag_set_enabled (self, shell_switch_get_state (SHELL_SWITCH (object)));
}

/* Update state whenever the switch is mapped, because most debug flags
 * don't have a way of notifying us of changes. */
static void
flag_switch_mapped_cb (GObject     *object,
                       GParamSpec  *pspec,
                       LgDebugFlag *self)
{
  ShellSwitch *flag_switch = SHELL_SWITCH (object);
  gboolean state;

  if (!clutter_actor_is_mapped (CLUTTER_ACTOR (flag_switch)))
    return;

  state = lg_debug_flag_is_enabled (self);
  if (state == shell_switch_get_state (flag_switch))
    return;

  g_signal_handler_block (flag_switch, self->state_handler_id);
  shell_switch_set_state (flag_switch, state);
  g_signal_handler_unblock (flag_switch, self->state_handler_id);
}

static void
lg_debug_flag_clicked (StButton *button,
                       int       clicked_button)
{
  LgDebugFlag *self = LG_DEBUG_FLAG (button);

  if (self->flag_switch != NULL)
    shell_switch_toggle (self->flag_switch);
}

static void
lg_debug_flag_dispose (GObject *object)
{
  LgDebugFlag *self = LG_DEBUG_FLAG (object);

  g_clear_weak_pointer (&self->flag_switch);

  G_OBJECT_CLASS (lg_debug_flag_parent_class)->dispose (object);
}

static void
lg_debug_flag_class_init (LgDebugFlagClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = lg_debug_flag_dispose;
  button_class->clicked = lg_debug_flag_clicked;
}

static void
lg_debug_flag_init (LgDebugFlag *self)
{
}

static ClutterActor *
lg_debug_flag_new (const char *label,
                   LgFlagKind  kind,
                   int         value)
{
  ClutterActor *box, *flag_label;
  LgDebugFlag *self;
  ShellSwitch *flag_switch;

  box = g_object_new (ST_TYPE_BOX_LAYOUT, "x-expand", TRUE, NULL);

  flag_label = g_object_new (ST_TYPE_LABEL,
                             "text", label,
                             "x-expand", TRUE,
                             "x-align", CLUTTER_ACTOR_ALIGN_START,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  clutter_actor_add_child (box, flag_label);

  flag_switch = shell_switch_new (FALSE);
  clutter_actor_add_child (box, CLUTTER_ACTOR (flag_switch));

  self = g_object_new (LG_TYPE_DEBUG_FLAG,
                       "style-class", "lg-debug-flag-button",
                       "can-focus", TRUE,
                       "toggle-mode", TRUE,
                       "child", box,
                       "label-actor", flag_label,
                       "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
  self->kind = kind;
  self->value = value;
  g_set_weak_pointer (&self->flag_switch, flag_switch);

  self->state_handler_id = g_signal_connect (flag_switch, "notify::state",
                                             G_CALLBACK (flag_switch_state_cb), self);
  g_signal_connect (flag_switch, "notify::mapped",
                    G_CALLBACK (flag_switch_mapped_cb), self);

  return CLUTTER_ACTOR (self);
}

static gboolean
slow_down_to_slider_cb (GBinding     *binding,
                        const GValue *from_value,
                        GValue       *to_value,
                        gpointer      user_data)
{
  g_value_set_double (to_value,
                      CLAMP (g_value_get_double (from_value) / MAX_SLOWDOWN, 0, 1));
  return TRUE;
}

static gboolean
slider_to_slow_down_cb (GBinding     *binding,
                        const GValue *from_value,
                        GValue       *to_value,
                        gpointer      user_data)
{
  g_value_set_double (to_value, g_value_get_double (from_value) * MAX_SLOWDOWN);
  return TRUE;
}

static gboolean
slow_down_to_checked_cb (GBinding     *binding,
                         const GValue *from_value,
                         GValue       *to_value,
                         gpointer      user_data)
{
  g_value_set_boolean (to_value, g_value_get_double (from_value) != 1);
  return TRUE;
}

static void
slow_down_flag_clicked_cb (StButton *button,
                           int       clicked_button,
                           gpointer  user_data)
{
  StSettings *settings = st_settings_get ();

  st_settings_set_slow_down_factor (settings,
                                    st_button_get_checked (button) ? 1 : TOGGLE_SLOWDOWN);
}

static ClutterActor *
slow_down_flag_new (void)
{
  StSettings *settings = st_settings_get ();
  ClutterActor *box, *flag_label, *button;
  ShellSlider *slider;

  box = g_object_new (ST_TYPE_BOX_LAYOUT, "x-expand", TRUE, NULL);

  flag_label = g_object_new (ST_TYPE_LABEL,
                             "text", "slow-down-factor",
                             "x-expand", TRUE,
                             "x-align", CLUTTER_ACTOR_ALIGN_START,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  clutter_actor_add_child (box, flag_label);

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "lg-debug-flag-button",
                         "can-focus", TRUE,
                         "child", box,
                         "label-actor", flag_label,
                         "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                         NULL);

  slider = shell_slider_new (0);
  shell_slider_add_mark (slider, 1.0 / MAX_SLOWDOWN);
  clutter_actor_add_child (box, CLUTTER_ACTOR (slider));

  g_object_bind_property_full (settings, "slow-down-factor",
                               slider, "value",
                               G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                               slow_down_to_slider_cb,
                               slider_to_slow_down_cb,
                               NULL, NULL);

  g_object_bind_property_full (settings, "slow-down-factor",
                               button, "checked",
                               G_BINDING_SYNC_CREATE,
                               slow_down_to_checked_cb,
                               NULL, NULL, NULL);

  g_signal_connect (button, "clicked", G_CALLBACK (slow_down_flag_clicked_cb), NULL);

  return button;
}

static ClutterActor *
debug_flags_header_new (const char *title)
{
  return g_object_new (ST_TYPE_LABEL,
                       "text", title,
                       "style-class", "lg-debug-flags-header",
                       "x-align", CLUTTER_ACTOR_ALIGN_START,
                       NULL);
}

/* The upper case names of the values of a flags or enum type that are
 * greater than 0, as GI names them, except @exclude. */
static void
debug_flags_add_flags (ClutterActor *container,
                       GType         type,
                       LgFlagKind    kind,
                       const char   *exclude)
{
  if (G_TYPE_IS_FLAGS (type))
    {
      GFlagsClass *klass = g_type_class_ref (type);
      guint i;

      for (i = 0; i < klass->n_values; i++)
        {
          const GFlagsValue *flag = &klass->values[i];
          g_autofree char *name = NULL;

          if ((int) flag->value <= 0)
            continue;

          name = g_ascii_strup (flag->value_nick, -1);
          g_strdelimit (name, "-", '_');
          if (exclude != NULL && strcmp (name, exclude) == 0)
            continue;

          clutter_actor_add_child (container, lg_debug_flag_new (name, kind, flag->value));
        }
      g_type_class_unref (klass);
    }
  else if (G_TYPE_IS_ENUM (type))
    {
      GEnumClass *klass = g_type_class_ref (type);
      guint i;

      for (i = 0; i < klass->n_values; i++)
        {
          const GEnumValue *flag = &klass->values[i];
          g_autofree char *name = NULL;

          if (flag->value <= 0)
            continue;

          name = g_ascii_strup (flag->value_nick, -1);
          g_strdelimit (name, "-", '_');
          if (exclude != NULL && strcmp (name, exclude) == 0)
            continue;

          clutter_actor_add_child (container, lg_debug_flag_new (name, kind, flag->value));
        }
      g_type_class_unref (klass);
    }
}

static ClutterActor *
debug_flags_new (void)
{
  ClutterActor *flags = g_object_new (ST_TYPE_BOX_LAYOUT,
                                      "name", "LookingGlassDebugFlags",
                                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                      "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                      NULL);

  /* Clutter debug flags. Paint debugging can easily result in a
   * non-responsive session. The pick flags are left out: the only current
   * option is likely to result in shooting ones foot. */
  clutter_actor_add_child (flags, debug_flags_header_new ("ClutterDebugFlag"));
  debug_flags_add_flags (flags, CLUTTER_TYPE_DEBUG_FLAG, LG_FLAG_CLUTTER_DEBUG, "PAINT");
  clutter_actor_add_child (flags, debug_flags_header_new ("ClutterDrawDebugFlag"));
  debug_flags_add_flags (flags, CLUTTER_TYPE_DRAW_DEBUG_FLAG, LG_FLAG_CLUTTER_DRAW, NULL);

  /* Meta paint flags */
  clutter_actor_add_child (flags, debug_flags_header_new ("MetaDebugPaintFlag"));
  debug_flags_add_flags (flags, META_TYPE_DEBUG_PAINT_FLAG, LG_FLAG_META_PAINT, NULL);

  /* Meta debug topics */
  clutter_actor_add_child (flags, debug_flags_header_new ("MetaDebugTopic"));
  debug_flags_add_flags (flags, META_TYPE_DEBUG_TOPIC, LG_FLAG_META_TOPIC, NULL);

  /* General / Context */
  clutter_actor_add_child (flags, debug_flags_header_new ("General"));
  /* MetaContext::unsafe-mode */
  clutter_actor_add_child (flags, lg_debug_flag_new ("unsafe-mode", LG_FLAG_UNSAFE_MODE, 0));
  /* DebugControl::exported */
  clutter_actor_add_child (flags, lg_debug_flag_new ("debug-control", LG_FLAG_DEBUG_CONTROL, 0));
  /* StSettings::slow-down-factor */
  clutter_actor_add_child (flags, slow_down_flag_new ());

  return flags;
}

/* ------------------------------------------------------------------------ */
/* What the Evaluator offers on top of the standard library                  */
/* ------------------------------------------------------------------------ */

static void
push_actor_or_nil (lua_State    *L,
                   ClutterActor *actor)
{
  if (actor != NULL)
    shell_lua_inspector_push_object (L, G_OBJECT (actor));
  else
    lua_pushnil (L);
}

/* inspect (x, y): the reactive actor at a position of the stage */
static int
l_inspect (lua_State *L)
{
  lua_Number x = luaL_checknumber (L, 1);
  lua_Number y = luaL_checknumber (L, 2);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  luaL_checkstack (L, 4, "inspect");
  push_actor_or_nil (L, clutter_stage_get_actor_at_pos (stage, CLUTTER_PICK_REACTIVE, x, y));
  return 1;
}

/* window_actors (): the MetaWindowActors of the shell, bottom to top */
static int
l_window_actors (lua_State *L)
{
  GList *actors = shell_global_get_window_actors (shell_global_get ());
  GObject **objects;
  guint n = g_list_length (actors);
  guint i = 0;
  GList *l;

  objects = g_newa (GObject *, n + 1);
  for (l = actors; l != NULL; l = l->next)
    objects[i++] = l->data;
  g_list_free (actors);

  luaL_checkstack (L, 6, "window_actors");
  lua_createtable (L, n, 0);
  for (i = 0; i < n; i++)
    {
      shell_lua_inspector_push_object (L, objects[i]);
      lua_rawseti (L, -2, i + 1);
    }

  return 1;
}

static ClutterActor *
check_actor (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, NULL);

  if (!CLUTTER_IS_ACTOR (object))
    luaL_error (L, "an actor is expected, got a %s", G_OBJECT_TYPE_NAME (object));

  return CLUTTER_ACTOR (object);
}

static int
l_actor_get_parent (lua_State *L)
{
  luaL_checkstack (L, 4, "get_parent");
  push_actor_or_nil (L, clutter_actor_get_parent (check_actor (L)));
  return 1;
}

static int
l_actor_get_next_sibling (lua_State *L)
{
  luaL_checkstack (L, 4, "get_next_sibling");
  push_actor_or_nil (L, clutter_actor_get_next_sibling (check_actor (L)));
  return 1;
}

static int
l_actor_get_previous_sibling (lua_State *L)
{
  luaL_checkstack (L, 4, "get_previous_sibling");
  push_actor_or_nil (L, clutter_actor_get_previous_sibling (check_actor (L)));
  return 1;
}

static int
l_actor_get_stage (lua_State *L)
{
  luaL_checkstack (L, 4, "get_stage");
  push_actor_or_nil (L, CLUTTER_ACTOR (clutter_actor_get_stage (check_actor (L))));
  return 1;
}

static int
l_actor_get_children (lua_State *L)
{
  ClutterActor *actor = check_actor (L);
  ClutterActor *child;
  lua_Integer i = 1;

  luaL_checkstack (L, 6, "get_children");
  lua_newtable (L);
  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      shell_lua_inspector_push_object (L, G_OBJECT (child));
      lua_rawseti (L, -2, i++);
    }

  return 1;
}

static int
l_actor_show (lua_State *L)
{
  clutter_actor_show (check_actor (L));
  return 0;
}

static int
l_actor_hide (lua_State *L)
{
  clutter_actor_hide (check_actor (L));
  return 0;
}

static int
l_actor_destroy (lua_State *L)
{
  clutter_actor_destroy (check_actor (L));
  return 0;
}

static const luaL_Reg actor_methods[] = {
  { "get_parent", l_actor_get_parent },
  { "get_children", l_actor_get_children },
  { "get_next_sibling", l_actor_get_next_sibling },
  { "get_previous_sibling", l_actor_get_previous_sibling },
  { "get_stage", l_actor_get_stage },
  { "show", l_actor_show },
  { "hide", l_actor_hide },
  { "destroy", l_actor_destroy },
  { NULL, NULL }
};

static void
setup_inspector (ShellLuaInspector *inspector)
{
  static gboolean methods_registered = FALSE;
  ShellGlobal *global = shell_global_get ();

  if (!methods_registered)
    {
      shell_lua_inspector_add_object_methods (CLUTTER_TYPE_ACTOR, actor_methods);
      methods_registered = TRUE;
    }

  shell_lua_inspector_add_function (inspector, "inspect", l_inspect);
  shell_lua_inspector_add_function (inspector, "window_actors", l_window_actors);

  shell_lua_inspector_set_global_object (inspector, "global", G_OBJECT (global));
  shell_lua_inspector_set_global_object (inspector, "stage",
                                         G_OBJECT (shell_global_get_stage (global)));
  shell_lua_inspector_set_global_object (inspector, "st_settings",
                                         G_OBJECT (st_settings_get ()));
  shell_lua_inspector_set_global_object (inspector, "main",
                                         G_OBJECT (shell_main_get_default ()));
}


/* ------------------------------------------------------------------------ */
/* ShellLookingGlass                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_IS_OPEN,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellLookingGlass
{
  StBoxLayout parent;

  ShellLuaInspector *inspector;

  ClutterActor *border_paint_target; /* weak */
  ClutterEffect *red_border_effect;  /* strong */

  gboolean open;
  ClutterGrab *grab;
  float hidden_y;
  float target_y;

  MetaLaters *laters; /* weak */
  unsigned int resize_later_id;
  guint gc_timeout_id;

  LgNotebook *notebook;            /* weak */
  LgObjInspector *obj_inspector;   /* weak, a sibling in the ui group */
  LgWindowList *window_list;       /* weak */
  ClutterActor *gc_button;         /* weak */

  /* The Evaluator page */
  ClutterActor *eval_box;          /* weak */
  ClutterActor *results_area;      /* weak */
  ClutterActor *entry_area;        /* weak */
  ClutterActor *entry;             /* weak */
  ClutterActor *completion_actor;  /* weak */
  ShellHistoryManager *history;
  guint32 last_tab_time;
};

G_DEFINE_FINAL_TYPE (ShellLookingGlass, shell_looking_glass, ST_TYPE_BOX_LAYOUT)

static ShellLuaInspector *
lg_get_inspector (ShellLookingGlass *self)
{
  return self->inspector;
}

static void
lg_set_border_paint_target (ShellLookingGlass *self,
                            ClutterActor      *target)
{
  ClutterActor *current;

  current = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->red_border_effect));
  if (current != NULL)
    clutter_actor_remove_effect (current, self->red_border_effect);

  if (target != NULL)
    g_set_weak_pointer (&self->border_paint_target, target);
  else
    g_clear_weak_pointer (&self->border_paint_target);

  if (self->border_paint_target != NULL)
    clutter_actor_add_effect (self->border_paint_target, self->red_border_effect);
}

static void
lg_scroll_evaluator_to_bottom (ShellLookingGlass *self)
{
  lg_notebook_scroll_to_bottom (self->notebook, 0);
}

/* Adds the row of a command to the Evaluator. @entries is an array of
 * (result number, type name, summary), or %NULL. */
static void
lg_push_result (ShellLookingGlass *self,
                const char        *command,
                const char        *output,
                GVariant          *entries,
                const char        *error_message)
{
  g_autofree char *command_text = g_strconcat (CHEVRON, command, NULL);
  LgResult *result = lg_result_new (self->inspector);
  ClutterActor *result_actor = CLUTTER_ACTOR (result);

  clutter_actor_add_child (result_actor, ellipsized_label_new (command_text));

  if (output != NULL && *output != '\0')
    {
      g_autofree char *text = g_strchomp (g_strdup (output));

      clutter_actor_add_child (result_actor, wrapping_label_new (text));
    }

  if (error_message != NULL)
    {
      /* a Lua error message can hold any bytes, Pango warns about invalid UTF-8 */
      g_autofree char *valid = g_utf8_make_valid (error_message, -1);
      g_autofree char *text = g_strdup_printf ("<error> %s", valid);

      clutter_actor_add_child (result_actor, wrapping_label_new (text));
    }

  if (entries != NULL)
    {
      GVariantIter iter;
      const char *type_name, *summary;
      guint64 id;

      g_variant_iter_init (&iter, entries);
      while (g_variant_iter_next (&iter, "(t&s&s)", &id, &type_name, &summary))
        {
          g_autofree char *index_text = g_strdup_printf ("r(%" G_GUINT64_FORMAT ") = ", id);
          g_autoptr (GObject) object = NULL;
          ClutterActor *box = g_object_new (ST_TYPE_BOX_LAYOUT, NULL);

          clutter_actor_add_child (result_actor, box);
          clutter_actor_add_child (box, ellipsized_label_new (index_text));
          clutter_actor_add_child (box, lg_obj_link_new (self, id, NULL, summary));
          lg_result_add_id (result, id);

          object = shell_lua_inspector_get_object (self->inspector, id);
          if (CLUTTER_IS_ACTOR (object))
            lg_set_border_paint_target (self, CLUTTER_ACTOR (object));
        }
    }

  clutter_actor_add_child (self->results_area, result_actor);

  if (clutter_actor_get_n_children (self->results_area) > MAX_RESULT_ROWS)
    clutter_actor_destroy (clutter_actor_get_first_child (self->results_area));

  /* Scroll to bottom */
  lg_scroll_evaluator_to_bottom (self);
}

/* A row for the one entry (result number, type name, summary) */
static void
lg_push_single_result (ShellLookingGlass *self,
                       const char        *command,
                       GVariant          *entry,
                       GError            *error)
{
  if (entry != NULL)
    {
      GVariant *children[1] = { entry };
      g_autoptr (GVariant) entries =
        g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(tss)"), children, 1));

      lg_push_result (self, command, NULL, entries, NULL);
    }
  else
    {
      lg_push_result (self, command, NULL, NULL,
                      error != NULL ? error->message : "unknown error");
    }
}

static void
lg_insert_value (ShellLookingGlass *self,
                 guint64            id)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) entry = NULL;

  entry = shell_lua_inspector_insert (self->inspector, id, &error);
  lg_push_single_result (self, "<insert>", entry, error);
}

static void
lg_inspect_value (ShellLookingGlass *self,
                  guint64            id,
                  ClutterActor      *source_actor)
{
  lg_obj_inspector_open (self->obj_inspector, source_actor != NULL);
  lg_obj_inspector_select_object (self->obj_inspector, id, FALSE);
}

static void
lg_inspect_object (ShellLookingGlass *self,
                   GObject           *object,
                   ClutterActor      *source_actor)
{
  g_autoptr (GError) error = NULL;
  guint64 id;

  id = shell_lua_inspector_pin_object (self->inspector, object, &error);
  if (id == 0)
    {
      g_message ("Looking Glass cannot inspect the %s: %s",
                 G_OBJECT_TYPE_NAME (object), error != NULL ? error->message : "?");
      return;
    }

  lg_obj_inspector_open (self->obj_inspector, source_actor != NULL);
  lg_obj_inspector_track_pin (self->obj_inspector, id);
  lg_obj_inspector_select_object (self->obj_inspector, id, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Evaluator: completion                                                      */
/* ------------------------------------------------------------------------ */

static void
hide_completions_done_cb (gboolean finished,
                          gpointer user_data)
{
  ClutterActor *completion_actor = user_data;

  clutter_actor_hide (completion_actor);
}

static void
lg_hide_completions (ShellLookingGlass *self)
{
  ShellEaseParams params = {
    .duration = 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (self->completion_actor == NULL)
    return;

  params.duration = (int) (AUTO_COMPLETE_SHOW_COMPLETION_ANIMATION_DURATION /
                           st_settings_get_slow_down_factor (st_settings_get ()));
  clutter_actor_remove_all_transitions (self->completion_actor);
  shell_actor_ease (self->completion_actor, &params,
                    hide_completions_done_cb,
                    g_object_ref (self->completion_actor), g_object_unref,
                    "height", 0.0,
                    "opacity", 0u,
                    NULL);
}

static void
lg_show_completions (ShellLookingGlass *self,
                     char             **completions)
{
  g_autofree char *text = g_strjoinv (", ", completions);
  float natural_height;

  if (self->completion_actor == NULL)
    {
      ClutterActor *label = g_object_new (ST_TYPE_LABEL,
                                          "name", "LookingGlassAutoCompletionText",
                                          "style-class", "lg-completions-text",
                                          NULL);

      clutter_text_set_ellipsize (label_get_text (label), PANGO_ELLIPSIZE_NONE);
      clutter_text_set_line_wrap (label_get_text (label), TRUE);
      g_set_weak_pointer (&self->completion_actor, label);
      clutter_actor_insert_child_below (self->eval_box, label, self->entry_area);
    }

  st_label_set_text (ST_LABEL (self->completion_actor), text);

  /* Setting the height to -1 allows us to get its actual preferred height
   * rather than whatever was last set when animating */
  clutter_actor_set_height (self->completion_actor, -1);
  clutter_actor_get_preferred_height (self->completion_actor,
                                      clutter_actor_get_width (self->results_area),
                                      NULL, &natural_height);

  /* Don't reanimate if we are already visible */
  if (clutter_actor_is_visible (self->completion_actor))
    {
      clutter_actor_set_height (self->completion_actor, natural_height);
    }
  else
    {
      ShellEaseParams params = {
        .duration = 0,
        .delay = -1,
        .mode = CLUTTER_EASE_OUT_QUAD,
        .repeat_count = 0,
        .auto_reverse = FALSE,
        .animation_required = FALSE,
      };

      params.duration = (int) (AUTO_COMPLETE_SHOW_COMPLETION_ANIMATION_DURATION /
                               st_settings_get_slow_down_factor (st_settings_get ()));
      clutter_actor_show (self->completion_actor);
      clutter_actor_remove_all_transitions (self->completion_actor);
      shell_actor_ease (self->completion_actor, &params, NULL, NULL, NULL,
                        "height", (double) natural_height,
                        "opacity", 255u,
                        NULL);
    }
}

/* The longest prefix all the words have in common */
static char *
common_prefix (char **words)
{
  size_t length;
  guint i;

  if (words[0] == NULL)
    return g_strdup ("");

  length = strlen (words[0]);
  for (i = 1; words[i] != NULL; i++)
    {
      size_t j = 0;

      while (j < length && words[i][j] == words[0][j])
        j++;
      length = j;
    }

  return g_strndup (words[0], length);
}

/* Insert the characters of @text which are not part of @head yet at the
 * cursor: if text="abc" and head="a", "bc" is inserted */
static void
insert_additional_completion_text (ShellLookingGlass *self,
                                   const char        *text,
                                   const char        *head)
{
  ClutterText *clutter_text = st_entry_get_clutter_text (ST_ENTRY (self->entry));
  const char *additional = text + MIN (strlen (head), strlen (text));

  clutter_text_insert_text (clutter_text, additional,
                            clutter_text_get_cursor_position (clutter_text));
}

static void
lg_handle_completions (ShellLookingGlass *self,
                       const char        *text,
                       guint32            time)
{
  g_auto (GStrv) completions = NULL;
  g_autofree char *head = NULL;
  gboolean double_tab;
  guint n_completions;

  completions = shell_lua_inspector_complete (self->inspector, text, &head);
  n_completions = g_strv_length (completions);
  double_tab = (time - self->last_tab_time) < AUTO_COMPLETE_DOUBLE_TAB_DELAY;

  /* Unique match = go ahead and complete; multiple matches + single tab =
   * complete the common starting string; multiple matches + double tab =
   * suggest all possible options */
  if (n_completions == 1)
    {
      insert_additional_completion_text (self, completions[0], head);
      /* the completion is complete: the suggestions are irrelevant */
      lg_hide_completions (self);
    }
  else if (n_completions > 1 && !double_tab)
    {
      g_autofree char *prefix = common_prefix (completions);

      if (prefix[0] != '\0')
        {
          insert_additional_completion_text (self, prefix, head);
          lg_show_completions (self, completions);
        }
    }
  else if (n_completions > 1 && double_tab)
    {
      lg_show_completions (self, completions);
    }

  self->last_tab_time = time;
}

static gboolean
entry_key_press_cb (ClutterKeyController *controller,
                    ShellLookingGlass    *self)
{
  ClutterText *clutter_text = st_entry_get_clutter_text (ST_ENTRY (self->entry));
  int cursor_position = clutter_text_get_cursor_position (clutter_text);
  const char *entry_text = clutter_text_get_text (clutter_text);
  g_autofree char *text = NULL;
  uint32_t symbol = 0;

  if (cursor_position != -1)
    text = g_utf8_substring (entry_text, 0, cursor_position);
  else
    text = g_strdup (entry_text);

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);
  if (symbol == CLUTTER_KEY_Tab)
    lg_handle_completions (self, text, clutter_get_current_event_time ());

  return CLUTTER_EVENT_PROPAGATE;
}

/* ------------------------------------------------------------------------ */
/* Evaluator: running commands                                                */
/* ------------------------------------------------------------------------ */

static void
lg_evaluate (ShellLookingGlass *self,
             const char        *input)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) entries = NULL;
  g_autofree char *command = NULL;
  g_autofree char *output = NULL;

  command = shell_history_manager_add_item (self->history, input); /* trims command */
  if (command[0] == '\0')
    return;

  entries = shell_lua_inspector_evaluate (self->inspector, command, &output, &error);
  lg_push_result (self, command, output, entries,
                  error != NULL ? error->message : NULL);

  st_entry_set_text (ST_ENTRY (self->entry), "");
}

static void
entry_activate_cb (ClutterText       *text,
                   ShellLookingGlass *self)
{
  g_autofree char *command = g_strdup (clutter_text_get_text (text));

  /* Hide any completions we are currently showing */
  lg_hide_completions (self);

  /* Ensure we don't get newlines in the command; the history file is
   * newline-separated. */
  g_strdelimit (command, "\n", ' ');
  lg_evaluate (self, command);
}

/* ------------------------------------------------------------------------ */
/* Toolbar                                                                    */
/* ------------------------------------------------------------------------ */

static void
picker_target_cb (LgPicker          *picker,
                  ClutterActor      *target,
                  double             stage_x,
                  double             stage_y,
                  ShellLookingGlass *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) entry = NULL;
  g_autofree char *command = NULL;

  command = g_strdup_printf ("inspect(%d, %d)",
                             (int) floor (stage_x + 0.5), (int) floor (stage_y + 0.5));
  entry = shell_lua_inspector_add_object_result (self->inspector, G_OBJECT (target),
                                                 &error);
  lg_push_single_result (self, command, entry, error);
}

static void
picker_closed_cb (LgPicker          *picker,
                  ShellLookingGlass *self)
{
  clutter_actor_show (CLUTTER_ACTOR (self));
  clutter_stage_set_key_focus (shell_global_get_stage (shell_global_get ()),
                               self->entry);
  clutter_actor_destroy (CLUTTER_ACTOR (picker));
}

static void
inspect_button_clicked_cb (StButton          *button,
                           int                clicked_button,
                           ShellLookingGlass *self)
{
  LgPicker *picker = lg_picker_new (self);

  g_signal_connect_object (picker, "target", G_CALLBACK (picker_target_cb), self, 0);
  g_signal_connect_object (picker, "closed", G_CALLBACK (picker_closed_cb), self, 0);
  clutter_actor_hide (CLUTTER_ACTOR (self));
}

static void
gc_timeout_cb (gpointer user_data)
{
  ShellLookingGlass *self = user_data;

  self->gc_timeout_id = 0;
  if (self->gc_button != NULL)
    st_button_set_icon_name (ST_BUTTON (self->gc_button), "user-trash-full-symbolic");
}

static void
gc_button_clicked_cb (StButton          *button,
                      int                clicked_button,
                      ShellLookingGlass *self)
{
  st_button_set_icon_name (button, "user-trash-symbolic");
  shell_lua_inspector_gc (self->inspector);

  g_clear_handle_id (&self->gc_timeout_id, g_source_remove);
  self->gc_timeout_id = g_timeout_add_once (500, gc_timeout_cb, self);
  g_source_set_name_by_id (self->gc_timeout_id,
                           "[gnome-shell] gc_button icon_name = 'user-trash-full-symbolic'");
}

/* ------------------------------------------------------------------------ */
/* Size and position                                                          */
/* ------------------------------------------------------------------------ */

static void
lg_resize (ShellLookingGlass *self)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *actor = CLUTTER_ACTOR (self);
  float panel_height = 0, keyboard_height = 0;
  int monitor_x, monitor_y, monitor_width, monitor_height;
  float my_width, my_height, available_height;

  if (layout_manager != NULL)
    {
      panel_height = clutter_actor_get_height (CLUTTER_ACTOR (shell_layout_manager_get_panel_box (layout_manager)));
      keyboard_height = clutter_actor_get_height (CLUTTER_ACTOR (shell_layout_manager_get_keyboard_box (layout_manager)));
    }

  get_primary_monitor (&monitor_x, &monitor_y, &monitor_width, &monitor_height);

  my_width = monitor_width * 0.7;
  available_height = monitor_height - keyboard_height;
  my_height = MIN (monitor_height * 0.7, available_height * 0.9);

  clutter_actor_set_x (actor, monitor_x + (monitor_width - my_width) / 2);
  self->hidden_y = monitor_y + panel_height - my_height;
  self->target_y = self->hidden_y + my_height;
  clutter_actor_set_y (actor, self->hidden_y);
  clutter_actor_set_size (actor, my_width, my_height);

  if (self->obj_inspector != NULL)
    {
      ClutterActor *inspector_actor = CLUTTER_ACTOR (self->obj_inspector);

      clutter_actor_set_size (inspector_actor,
                              floorf (my_width * 0.8), floorf (my_height * 0.8));
      clutter_actor_set_position (inspector_actor,
                                  clutter_actor_get_x (actor) + floorf (my_width * 0.1),
                                  self->target_y + floorf (my_height * 0.1));
    }
}

static gboolean
resize_later_cb (gpointer user_data)
{
  ShellLookingGlass *self = user_data;

  self->resize_later_id = 0;
  lg_resize (self);

  return G_SOURCE_REMOVE;
}

static void
lg_queue_resize (ShellLookingGlass *self)
{
  MetaCompositor *compositor = shell_global_get_compositor (shell_global_get ());

  if (self->resize_later_id != 0 || compositor == NULL)
    return;

  g_set_weak_pointer (&self->laters, meta_compositor_get_laters (compositor));
  self->resize_later_id = meta_laters_add (self->laters, META_LATER_BEFORE_REDRAW,
                                           resize_later_cb, self, NULL);
}

static void
allocation_changed_cb (GObject           *object,
                       GParamSpec        *pspec,
                       ShellLookingGlass *self)
{
  lg_queue_resize (self);
}

/* ------------------------------------------------------------------------ */
/* Opening and closing                                                        */
/* ------------------------------------------------------------------------ */

static void
lg_set_open (ShellLookingGlass *self,
             gboolean           open)
{
  if (self->open == open)
    return;

  self->open = open;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_IS_OPEN]);
}

/**
 * shell_looking_glass_open:
 * @self: a #ShellLookingGlass
 *
 * Slides Looking Glass out from under the panel and gives it the keyboard
 * focus, on the Evaluator.
 */
void
shell_looking_glass_open (ShellLookingGlass *self)
{
  ShellEaseParams params = {
    .duration = 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_LOOKING_GLASS (self));

  if (self->open)
    return;

  self->grab = shell_main_push_modal (shell_main_get_default (),
                                      CLUTTER_ACTOR (self),
                                      SHELL_ACTION_MODE_LOOKING_GLASS);
  if (self->grab != NULL)
    g_object_ref (self->grab);

  clutter_actor_show (CLUTTER_ACTOR (self));
  lg_set_open (self, TRUE);
  shell_history_manager_last_item (self->history);

  clutter_actor_remove_all_transitions (CLUTTER_ACTOR (self));

  /* We inverse compensate for the slow-down so you can change the factor
   * through LookingGlass without long waits. */
  params.duration = (int) (SHELL_LOOKING_GLASS_ANIMATION_TIME /
                           st_settings_get_slow_down_factor (st_settings_get ()));
  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "y", (double) self->target_y,
                    NULL);

  lg_window_list_update (self->window_list);
  clutter_actor_grab_key_focus (self->entry);
}

typedef struct
{
  ShellLookingGlass *self;
  ClutterGrab *grab;
} CloseData;

static void
close_data_free (CloseData *data)
{
  g_clear_object (&data->grab);
  g_clear_object (&data->self);
  g_free (data);
}

static void
close_done_cb (gboolean  finished,
               gpointer  user_data)
{
  CloseData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (data->grab != NULL &&
      !shell_main_pop_modal (shell_main_get_default (), data->grab, &error))
    g_debug ("Could not pop the modal of Looking Glass: %s", error->message);

  /* Opened again in the meantime */
  if (!data->self->open)
    clutter_actor_hide (CLUTTER_ACTOR (data->self));
}

/**
 * shell_looking_glass_close:
 * @self: a #ShellLookingGlass
 *
 * Slides Looking Glass back under the panel.
 */
void
shell_looking_glass_close (ShellLookingGlass *self)
{
  ShellEaseParams params = {
    .duration = 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  CloseData *data;
  double slow_down_factor;

  g_return_if_fail (SHELL_IS_LOOKING_GLASS (self));

  if (!self->open)
    return;

  if (self->obj_inspector != NULL)
    lg_obj_inspector_close (self->obj_inspector);

  lg_set_open (self, FALSE);
  clutter_actor_remove_all_transitions (CLUTTER_ACTOR (self));

  lg_set_border_paint_target (self, NULL);

  slow_down_factor = st_settings_get_slow_down_factor (st_settings_get ());
  params.duration = (int) MIN (SHELL_LOOKING_GLASS_ANIMATION_TIME / slow_down_factor,
                               SHELL_LOOKING_GLASS_ANIMATION_TIME);

  data = g_new0 (CloseData, 1);
  data->self = g_object_ref (self);
  data->grab = g_steal_pointer (&self->grab);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    close_done_cb, data, (GDestroyNotify) close_data_free,
                    "y", (double) self->hidden_y,
                    NULL);
}

/**
 * shell_looking_glass_toggle:
 * @self: a #ShellLookingGlass
 *
 * Opens Looking Glass if it is closed, closes it otherwise.
 */
void
shell_looking_glass_toggle (ShellLookingGlass *self)
{
  g_return_if_fail (SHELL_IS_LOOKING_GLASS (self));

  if (self->open)
    shell_looking_glass_close (self);
  else
    shell_looking_glass_open (self);
}

/**
 * shell_looking_glass_get_is_open:
 * @self: a #ShellLookingGlass
 *
 * Returns: whether Looking Glass is open, #ShellLookingGlass:is-open
 */
gboolean
shell_looking_glass_get_is_open (ShellLookingGlass *self)
{
  g_return_val_if_fail (SHELL_IS_LOOKING_GLASS (self), FALSE);

  return self->open;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                    */
/* ------------------------------------------------------------------------ */

static gboolean
close_binding_cb (GObject             *object,
                  const char          *action_name,
                  guint                key_val,
                  ClutterModifierType  modifiers,
                  gpointer             user_data)
{
  shell_looking_glass_close (SHELL_LOOKING_GLASS (object));
  return CLUTTER_EVENT_STOP;
}

/* The names are swapped, as in the JS module */
static gboolean
next_tab_binding_cb (GObject             *object,
                     const char          *action_name,
                     guint                key_val,
                     ClutterModifierType  modifiers,
                     gpointer             user_data)
{
  ShellLookingGlass *self = SHELL_LOOKING_GLASS (object);

  lg_notebook_prev_tab (self->notebook);
  return CLUTTER_EVENT_STOP;
}

static gboolean
prev_tab_binding_cb (GObject             *object,
                     const char          *action_name,
                     guint                key_val,
                     ClutterModifierType  modifiers,
                     gpointer             user_data)
{
  ShellLookingGlass *self = SHELL_LOOKING_GLASS (object);

  lg_notebook_next_tab (self->notebook);
  return CLUTTER_EVENT_STOP;
}

static void
shell_looking_glass_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellLookingGlass *self = SHELL_LOOKING_GLASS (object);

  switch (prop_id)
    {
    case PROP_IS_OPEN:
      g_value_set_boolean (value, self->open);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_looking_glass_constructed (GObject *object)
{
  ShellLookingGlass *self = SHELL_LOOKING_GLASS (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *ui_group = get_ui_group ();
  ClutterActor *toolbar, *inspect_button, *gc_button, *notebook, *empty_box;
  ClutterActor *eval_box, *results_area, *entry_area;
  ClutterActor *label, *entry;
  ClutterAction *key_controller;
  LgWindowList *window_list;
  LgExtensions *extensions;
  LgActorTreeViewer *actor_tree_viewer;
  LgObjInspector *obj_inspector;
  ShellGlobal *global = shell_global_get ();

  G_OBJECT_CLASS (shell_looking_glass_parent_class)->constructed (object);

  clutter_actor_set_name (actor, "LookingGlassDialog");
  st_widget_set_style_class_name (ST_WIDGET (self), "lg-dialog");
  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_hide (actor);
  clutter_actor_set_reactive (actor, TRUE);

  self->inspector = g_object_ref (shell_global_get_lua_inspector (global));
  setup_inspector (self->inspector);

  self->red_border_effect = g_object_ref_sink (g_object_new (LG_TYPE_RED_BORDER_EFFECT, NULL));

  /* We want it to appear to slide out from underneath the panel */
  if (ui_group != NULL && layout_manager != NULL)
    {
      ClutterActor *panel_box = CLUTTER_ACTOR (shell_layout_manager_get_panel_box (layout_manager));
      ClutterActor *keyboard_box = CLUTTER_ACTOR (shell_layout_manager_get_keyboard_box (layout_manager));

      clutter_actor_add_child (ui_group, actor);
      clutter_actor_set_child_below_sibling (ui_group, actor, panel_box);
      g_signal_connect_object (panel_box, "notify::allocation",
                               G_CALLBACK (allocation_changed_cb), self, 0);
      g_signal_connect_object (keyboard_box, "notify::allocation",
                               G_CALLBACK (allocation_changed_cb), self, 0);
    }

  obj_inspector = lg_obj_inspector_new (self);
  g_set_weak_pointer (&self->obj_inspector, obj_inspector);
  if (ui_group != NULL)
    clutter_actor_add_child (ui_group, CLUTTER_ACTOR (obj_inspector));
  clutter_actor_hide (CLUTTER_ACTOR (obj_inspector));

  toolbar = box_layout_new ("Toolbar", NULL, CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_actor_add_child (actor, toolbar);

  inspect_button = g_object_new (ST_TYPE_BUTTON,
                                 "style-class", "lg-toolbar-button",
                                 "icon-name", "find-location-symbolic",
                                 NULL);
  clutter_actor_add_child (toolbar, inspect_button);
  g_signal_connect (inspect_button, "clicked",
                    G_CALLBACK (inspect_button_clicked_cb), self);

  gc_button = g_object_new (ST_TYPE_BUTTON,
                            "style-class", "lg-toolbar-button",
                            "icon-name", "user-trash-full-symbolic",
                            NULL);
  g_set_weak_pointer (&self->gc_button, gc_button);
  clutter_actor_add_child (toolbar, gc_button);
  g_signal_connect (gc_button, "clicked", G_CALLBACK (gc_button_clicked_cb), self);

  notebook = lg_notebook_new ();
  g_set_weak_pointer (&self->notebook, LG_NOTEBOOK (notebook));
  clutter_actor_add_child (actor, notebook);

  empty_box = g_object_new (ST_TYPE_BIN, "x-expand", TRUE, NULL);
  clutter_actor_add_child (toolbar, empty_box);
  clutter_actor_add_child (toolbar, LG_NOTEBOOK (notebook)->tab_controls);

  /* The Evaluator */
  eval_box = box_layout_new ("LookingGlassEvaluator", NULL,
                             CLUTTER_ORIENTATION_VERTICAL);
  g_set_weak_pointer (&self->eval_box, eval_box);
  lg_notebook_append_page (LG_NOTEBOOK (notebook), "Evaluator", eval_box);

  results_area = g_object_new (ST_TYPE_BOX_LAYOUT,
                               "style-class", "evaluator-results",
                               "orientation", CLUTTER_ORIENTATION_VERTICAL,
                               "y-expand", TRUE,
                               NULL);
  g_set_weak_pointer (&self->results_area, results_area);
  clutter_actor_add_child (eval_box, results_area);

  entry_area = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "name", "EntryArea",
                             "y-align", CLUTTER_ACTOR_ALIGN_END,
                             NULL);
  g_set_weak_pointer (&self->entry_area, entry_area);
  clutter_actor_add_child (eval_box, entry_area);

  label = CLUTTER_ACTOR (st_label_new (CHEVRON));
  clutter_actor_add_child (entry_area, label);

  entry = g_object_new (ST_TYPE_ENTRY,
                        "input-purpose", CLUTTER_INPUT_CONTENT_PURPOSE_TERMINAL,
                        "can-focus", TRUE,
                        "x-expand", TRUE,
                        NULL);
  g_set_weak_pointer (&self->entry, entry);
  shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);
  clutter_actor_add_child (entry_area, entry);

  window_list = lg_window_list_new (self);
  g_set_weak_pointer (&self->window_list, window_list);
  lg_notebook_append_page (LG_NOTEBOOK (notebook), "Windows", CLUTTER_ACTOR (window_list));

  extensions = lg_extensions_new (self);
  lg_notebook_append_page (LG_NOTEBOOK (notebook), "Extensions", CLUTTER_ACTOR (extensions));

  actor_tree_viewer = lg_actor_tree_viewer_new (self);
  lg_notebook_append_page (LG_NOTEBOOK (notebook), "Actors", CLUTTER_ACTOR (actor_tree_viewer));

  lg_notebook_append_page (LG_NOTEBOOK (notebook), "Flags", debug_flags_new ());

  g_signal_connect_object (st_entry_get_clutter_text (ST_ENTRY (entry)), "activate",
                           G_CALLBACK (entry_activate_cb), self, 0);

  self->history = shell_history_manager_new (HISTORY_KEY,
                                             st_entry_get_clutter_text (ST_ENTRY (entry)));

  self->last_tab_time = shell_global_get_current_time (global);
  key_controller = clutter_key_controller_new (NULL);
  g_signal_connect_object (key_controller, "key-press",
                           G_CALLBACK (entry_key_press_cb), self, 0);
  clutter_actor_add_action (entry, key_controller);

  lg_resize (self);
}

static void
shell_looking_glass_dispose (GObject *object)
{
  ShellLookingGlass *self = SHELL_LOOKING_GLASS (object);

  g_clear_handle_id (&self->gc_timeout_id, g_source_remove);

  if (self->resize_later_id != 0)
    {
      if (self->laters != NULL)
        meta_laters_remove (self->laters, self->resize_later_id);
      self->resize_later_id = 0;
    }
  g_clear_weak_pointer (&self->laters);

  if (self->grab != NULL)
    {
      g_autoptr (GError) error = NULL;

      if (!shell_main_pop_modal (shell_main_get_default (), self->grab, &error))
        g_debug ("Could not pop the modal of Looking Glass: %s", error->message);
      g_clear_object (&self->grab);
    }
  self->open = FALSE;

  if (self->red_border_effect != NULL)
    {
      ClutterActor *current =
        clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->red_border_effect));

      if (current != NULL)
        clutter_actor_remove_effect (current, self->red_border_effect);
    }
  g_clear_weak_pointer (&self->border_paint_target);
  g_clear_object (&self->red_border_effect);

  /* a sibling of Looking Glass in the ui group */
  if (self->obj_inspector != NULL)
    {
      ClutterActor *obj_inspector = CLUTTER_ACTOR (self->obj_inspector);

      g_clear_weak_pointer (&self->obj_inspector);
      clutter_actor_destroy (obj_inspector);
    }

  g_clear_weak_pointer (&self->notebook);
  g_clear_weak_pointer (&self->window_list);
  g_clear_weak_pointer (&self->gc_button);
  g_clear_weak_pointer (&self->eval_box);
  g_clear_weak_pointer (&self->results_area);
  g_clear_weak_pointer (&self->entry_area);
  g_clear_weak_pointer (&self->entry);
  g_clear_weak_pointer (&self->completion_actor);
  g_clear_object (&self->history);
  g_clear_object (&self->inspector);

  G_OBJECT_CLASS (shell_looking_glass_parent_class)->dispose (object);
}

static void
shell_looking_glass_class_init (ShellLookingGlassClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructed = shell_looking_glass_constructed;
  object_class->dispose = shell_looking_glass_dispose;
  object_class->get_property = shell_looking_glass_get_property;

  /**
   * ShellLookingGlass:is-open:
   *
   * Whether Looking Glass is open (`isOpen`).
   */
  props[PROP_IS_OPEN] =
    g_param_spec_boolean ("is-open", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool, "close",
                                       CLUTTER_KEY_Escape, 0,
                                       G_CALLBACK (close_binding_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "next-tab",
                                       CLUTTER_KEY_Page_Down, CLUTTER_CONTROL_MASK,
                                       G_CALLBACK (next_tab_binding_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "prev-tab",
                                       CLUTTER_KEY_Page_Up, CLUTTER_CONTROL_MASK,
                                       G_CALLBACK (prev_tab_binding_cb), NULL, NULL);
}

static void
shell_looking_glass_init (ShellLookingGlass *self)
{
}

/**
 * shell_looking_glass_new:
 *
 * Creates Looking Glass (`new LookingGlass ()`), and adds it to the ui group
 * of the layout manager of #ShellMain, below the panel.
 *
 * Returns: (transfer floating): a new #ShellLookingGlass
 */
ShellLookingGlass *
shell_looking_glass_new (void)
{
  return g_object_new (SHELL_TYPE_LOOKING_GLASS, NULL);
}
