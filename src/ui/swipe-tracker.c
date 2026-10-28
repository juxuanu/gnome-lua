/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * swipe-tracker.c: Turns touchscreen/mouse drags, three finger touchpad
 * swipes and touchpad scrolling on an actor into one stream of progress
 * updates. Port of js/ui/swipeTracker.js.
 *
 * The private JS classes TouchpadSwipeGesture and ScrollGesture were GObjects
 * which only forwarded events to the tracker through signals. They are plain
 * state of the tracker here: their `enabled`, `orientation` and
 * `scroll-modifiers` properties were bound to the properties of the tracker,
 * which are read directly. `Main.actionMode` is read from #ShellMain.
 */

#include "config.h"

#include <math.h>

#include <gio/gio.h>
#include <meta/display.h>

#include "shell-enum-types.h"
#include "shell-global.h"
#include "ui/main.h"
#include "ui/swipe-tracker.h"

/* FIXME: ideally these values matches physical touchpad size. We can get the
 * correct values for gnome-shell specifically, since mutter uses libinput
 * directly, but GTK apps cannot get it, so use an arbitrary value so that
 * it's consistent with apps.
 */
#define TOUCHPAD_BASE_HEIGHT 300
#define TOUCHPAD_BASE_WIDTH 400

#define EVENT_HISTORY_THRESHOLD_MS 150

#define SCROLL_MULTIPLIER 10

#define MIN_ANIMATION_DURATION 100
#define MAX_ANIMATION_DURATION 400
#define VELOCITY_THRESHOLD_TOUCH 0.3
#define VELOCITY_THRESHOLD_TOUCHPAD 0.6
#define DECELERATION_TOUCH 0.998
#define DECELERATION_TOUCHPAD 0.997
#define VELOCITY_CURVE_THRESHOLD 2
#define DECELERATION_PARABOLA_MULTIPLIER 0.35
#define DRAG_THRESHOLD_DISTANCE 16

/* Derivative of easeOutCubic at t=0 */
#define DURATION_MULTIPLIER 3
#define ANIMATION_BASE_VELOCITY 0.002
#define EPSILON 0.005

#define GESTURE_FINGER_COUNT 3

#define TOUCHPAD_SCHEMA "org.gnome.desktop.peripherals.touchpad"

typedef enum
{
  SWIPE_STATE_NONE,
  SWIPE_STATE_SCROLLING,
} SwipeState;

typedef enum
{
  TOUCHPAD_STATE_NONE,
  TOUCHPAD_STATE_PENDING,
  TOUCHPAD_STATE_HANDLING,
  TOUCHPAD_STATE_IGNORED,
} TouchpadState;

typedef struct
{
  int64_t time;
  double delta;
} HistoryRecord;

enum
{
  PROP_0,

  PROP_ACTOR,
  PROP_ALLOWED_MODES,
  PROP_ALLOW_DRAG,
  PROP_ALLOW_SCROLL,
  PROP_PHASE,
  PROP_NAME,
  PROP_ENABLED,
  PROP_ORIENTATION,
  PROP_DISTANCE,
  PROP_ALLOW_LONG_SWIPES,
  PROP_SCROLL_MODIFIERS,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  BEGIN,
  UPDATE,
  END,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellSwipeTracker
{
  GObject parent;

  /* Construct-only properties. The actor is a weak pointer. */
  ClutterActor *actor;
  ShellActionMode allowed_modes;
  gboolean allow_drag;
  gboolean allow_scroll;
  ClutterEventPhase phase;
  char *name;

  gboolean enabled;
  ClutterOrientation orientation;
  double distance;
  gboolean allow_long_swipes;
  ClutterModifierType scroll_modifiers;

  /* Progress of the current swipe */
  SwipeState state;
  GArray *snap_points; /* double */
  double initial_progress;
  double cancel_progress;
  double progress;
  gboolean cancelled;
  GArray *history; /* HistoryRecord */

  ClutterPanGesture *pan_gesture;

  /* TouchpadSwipeGesture */
  GSettings *touchpad_settings;
  gulong touchpad_handler_id;
  TouchpadState touchpad_state;
  double cumulative_x;
  double cumulative_y;

  /* ScrollGesture */
  gulong scroll_handler_id;
  gboolean scroll_began;
};

G_DEFINE_FINAL_TYPE (ShellSwipeTracker, shell_swipe_tracker, G_TYPE_OBJECT)

static double
clamp_js (double x,
          double lower,
          double upper)
{
  return MIN (MAX (x, lower), upper);
}

static gboolean
mode_allowed (ShellSwipeTracker *self)
{
  return (self->allowed_modes &
          shell_main_get_action_mode (shell_main_get_default ())) != 0;
}

/* ------------------------------------------------------------------------ */
/* EventHistory                                                             */
/* ------------------------------------------------------------------------ */

static void
history_reset (ShellSwipeTracker *self)
{
  g_array_set_size (self->history, 0);
}

static void
history_trim (ShellSwipeTracker *self,
              int64_t            time)
{
  int64_t threshold_time = time - EVENT_HISTORY_THRESHOLD_MS;
  unsigned int i;

  /* Like Array.splice (0, findIndex ()): nothing is dropped if every record
   * is older than the threshold (findIndex () is -1 then).
   */
  for (i = 0; i < self->history->len; i++)
    {
      if (g_array_index (self->history, HistoryRecord, i).time >= threshold_time)
        {
          g_array_remove_range (self->history, 0, i);
          return;
        }
    }
}

static void
history_append (ShellSwipeTracker *self,
                int64_t            time,
                double             delta)
{
  HistoryRecord record = { time, delta };

  history_trim (self, time);
  g_array_append_val (self->history, record);
}

static double
history_calculate_velocity (ShellSwipeTracker *self)
{
  int64_t first_time, last_time;
  double total_delta = 0;
  unsigned int i;

  if (self->history->len < 2)
    return 0;

  first_time = g_array_index (self->history, HistoryRecord, 0).time;
  last_time = g_array_index (self->history, HistoryRecord,
                             self->history->len - 1).time;

  if (first_time == last_time)
    return 0;

  for (i = 1; i < self->history->len; i++)
    total_delta += g_array_index (self->history, HistoryRecord, i).delta;

  return total_delta / (double) (last_time - first_time);
}

/* ------------------------------------------------------------------------ */
/* Snap points                                                              */
/* ------------------------------------------------------------------------ */

static unsigned int
get_n_snap_points (ShellSwipeTracker *self)
{
  return self->snap_points->len;
}

/* The JS indexes past the array and gets undefined; stay inside it. */
static double
snap_point_at (ShellSwipeTracker *self,
               int                index)
{
  index = CLAMP (index, 0, (int) get_n_snap_points (self) - 1);
  return g_array_index (self->snap_points, double, index);
}

static int
find_closest_point (ShellSwipeTracker *self,
                    double             pos)
{
  double min = INFINITY;
  int closest = -1;
  unsigned int i;

  for (i = 0; i < get_n_snap_points (self); i++)
    {
      double distance = fabs (g_array_index (self->snap_points, double, i) - pos);

      if (distance < min)
        {
          min = distance;
          closest = i;
        }
    }

  return closest;
}

static int
find_next_point (ShellSwipeTracker *self,
                 double             pos)
{
  unsigned int i;

  for (i = 0; i < get_n_snap_points (self); i++)
    {
      if (g_array_index (self->snap_points, double, i) >= pos)
        return i;
    }

  return -1;
}

static int
find_previous_point (ShellSwipeTracker *self,
                     double             pos)
{
  int i;

  for (i = (int) get_n_snap_points (self) - 1; i >= 0; i--)
    {
      if (g_array_index (self->snap_points, double, i) <= pos)
        return i;
    }

  g_message ("%s: Invalid position %f", G_STRFUNC, pos);

  /* also out-of-bounds, but somehow callers fail less hard than with -1 */
  return get_n_snap_points (self);
}

static int
find_point_for_projection (ShellSwipeTracker *self,
                           double             pos,
                           double             velocity)
{
  int initial = find_closest_point (self, self->initial_progress);
  int prev = find_previous_point (self, pos);
  int next = find_next_point (self, pos);

  if ((velocity > 0 ? prev : next) == initial)
    return velocity > 0 ? next : prev;

  return find_closest_point (self, pos);
}

static void
get_bounds (ShellSwipeTracker *self,
            double             pos,
            double            *lower,
            double            *upper)
{
  int closest, prev, next, lower_index, upper_index;

  if (self->allow_long_swipes)
    {
      *lower = snap_point_at (self, 0);
      *upper = snap_point_at (self, (int) get_n_snap_points (self) - 1);
      return;
    }

  closest = find_closest_point (self, pos);

  if (fabs (snap_point_at (self, closest) - pos) < EPSILON)
    {
      prev = next = closest;
    }
  else
    {
      prev = find_previous_point (self, pos);
      next = find_next_point (self, pos);
    }

  lower_index = MAX (prev - 1, 0);
  upper_index = MIN (next + 1, (int) get_n_snap_points (self) - 1);

  *lower = snap_point_at (self, lower_index);
  *upper = snap_point_at (self, upper_index);
}

/* ------------------------------------------------------------------------ */
/* The swipe                                                                */
/* ------------------------------------------------------------------------ */

static void
reset (ShellSwipeTracker *self)
{
  self->state = SWIPE_STATE_NONE;

  g_array_set_size (self->snap_points, 0);
  self->initial_progress = 0;
  self->cancel_progress = 0;

  self->progress = 0;

  self->cancelled = FALSE;

  history_reset (self);
}

static void
emit_begin (ShellSwipeTracker *self,
            unsigned int       monitor)
{
  g_autoptr (ShellSwipeTracker) keep_alive = g_object_ref (self);

  g_signal_emit (self, signals[BEGIN], 0, monitor);
}

static void
emit_update (ShellSwipeTracker *self,
             double             progress)
{
  g_autoptr (ShellSwipeTracker) keep_alive = g_object_ref (self);

  g_signal_emit (self, signals[UPDATE], 0, progress);
}

static void
emit_end (ShellSwipeTracker *self,
          double             duration,
          double             end_progress)
{
  g_autoptr (ShellSwipeTracker) keep_alive = g_object_ref (self);

  g_signal_emit (self, signals[END], 0, (guint64) duration, end_progress);
}

static void
interrupt (ShellSwipeTracker *self)
{
  g_autoptr (ShellSwipeTracker) keep_alive = g_object_ref (self);

  emit_end (self, 0, self->cancel_progress);
  if (self->pan_gesture != NULL)
    clutter_gesture_cancel (CLUTTER_GESTURE (self->pan_gesture));
  reset (self);
}

static void
begin_gesture (ShellSwipeTracker *self,
               double             x,
               double             y)
{
  MtkRectangle rect;
  ShellGlobal *global;
  int monitor;

  if (self->state == SWIPE_STATE_SCROLLING)
    return;

  global = shell_global_get ();
  if (global == NULL)
    return;

  rect.x = (int) x;
  rect.y = (int) y;
  rect.width = 1;
  rect.height = 1;
  monitor = meta_display_get_monitor_index_for_rect (shell_global_get_display (global),
                                                     &rect);

  emit_begin (self, (unsigned int) monitor);
}

static void
begin_touchpad_gesture (ShellSwipeTracker *self,
                        int64_t            time,
                        double             x,
                        double             y)
{
  history_append (self, time, 0);

  begin_gesture (self, x, y);
}

static void
update_gesture (ShellSwipeTracker *self,
                double             delta,
                double             distance)
{
  double lower, upper;

  if (self->state != SWIPE_STATE_SCROLLING)
    return;

  self->progress += delta / distance;

  get_bounds (self, self->initial_progress, &lower, &upper);
  self->progress = clamp_js (self->progress, lower, upper);

  emit_update (self, self->progress);
}

static double
get_gesture_dir_factor (ShellSwipeTracker *self)
{
  if (self->orientation == CLUTTER_ORIENTATION_HORIZONTAL &&
      clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
    return 1;

  return -1;
}

static void
update_touchpad_gesture (ShellSwipeTracker *self,
                         int64_t            time,
                         double             delta,
                         double             distance)
{
  if (!mode_allowed (self) || !self->enabled)
    {
      interrupt (self);
      return;
    }

  if (self->orientation == CLUTTER_ORIENTATION_HORIZONTAL &&
      clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
    delta = -delta;

  history_append (self, time, delta);
  update_gesture (self, delta, distance);
}

static double
get_end_progress (ShellSwipeTracker *self,
                  double             velocity,
                  double             distance,
                  gboolean           is_touchpad)
{
  double threshold, decel, slope, pos, lower, upper;
  int index;

  if (self->cancelled)
    return self->cancel_progress;

  threshold = is_touchpad ? VELOCITY_THRESHOLD_TOUCHPAD : VELOCITY_THRESHOLD_TOUCH;

  if (fabs (velocity) < threshold)
    return snap_point_at (self, find_closest_point (self, self->progress));

  decel = is_touchpad ? DECELERATION_TOUCHPAD : DECELERATION_TOUCH;
  slope = decel / (1.0 - decel) / 1000.0;

  if (fabs (velocity) > VELOCITY_CURVE_THRESHOLD)
    {
      double c = slope / 2 / DECELERATION_PARABOLA_MULTIPLIER;
      double x = fabs (velocity) - VELOCITY_CURVE_THRESHOLD + c;

      pos = slope * VELOCITY_CURVE_THRESHOLD +
            DECELERATION_PARABOLA_MULTIPLIER * x * x -
            DECELERATION_PARABOLA_MULTIPLIER * c * c;
    }
  else
    {
      pos = fabs (velocity) * slope;
    }

  pos = pos * (velocity > 0 ? 1 : (velocity < 0 ? -1 : 0)) + self->progress;
  get_bounds (self, self->initial_progress, &lower, &upper);
  pos = clamp_js (pos, lower, upper);

  index = find_point_for_projection (self, pos, velocity);

  return snap_point_at (self, index);
}

static void
end_gesture (ShellSwipeTracker *self,
             double             velocity,
             double             distance,
             gboolean           is_touchpad)
{
  double end_progress, n_points, max_duration, duration;

  if (self->state != SWIPE_STATE_SCROLLING)
    return;

  if (!mode_allowed (self) || !self->enabled)
    {
      interrupt (self);
      return;
    }

  end_progress = get_end_progress (self, velocity, distance, is_touchpad);

  velocity /= distance;

  if ((end_progress - self->progress) * velocity <= 0)
    velocity = ANIMATION_BASE_VELOCITY;

  n_points = MAX (1, ceil (fabs (self->progress - end_progress)));
  max_duration = MAX_ANIMATION_DURATION * log2 (1 + n_points);

  duration = fabs ((self->progress - end_progress) / velocity * DURATION_MULTIPLIER);
  if (duration > 0)
    duration = clamp_js (duration, MIN_ANIMATION_DURATION, max_duration);

  reset (self);
  emit_end (self, duration, end_progress);
}

static void
end_touchpad_gesture (ShellSwipeTracker *self,
                      int64_t            time,
                      double             distance)
{
  double velocity;

  if (!mode_allowed (self) || !self->enabled)
    {
      interrupt (self);
      return;
    }

  history_trim (self, time);
  velocity = history_calculate_velocity (self);

  end_gesture (self, velocity, distance, TRUE);
}

/* ------------------------------------------------------------------------ */
/* Pan gesture                                                              */
/* ------------------------------------------------------------------------ */

static gboolean
pan_may_recognize_cb (ClutterGesture    *gesture,
                      ShellSwipeTracker *self)
{
  return mode_allowed (self);
}

static void
pan_recognize_cb (ClutterGesture    *gesture,
                  ShellSwipeTracker *self)
{
  graphene_point_t coords;

  clutter_pan_gesture_get_begin_centroid (CLUTTER_PAN_GESTURE (gesture), &coords);

  begin_gesture (self, coords.x, coords.y);
}

static void
pan_update_cb (ClutterPanGesture  *gesture,
               ShellSwipeTracker  *self)
{
  graphene_vec2_t delta_vec;
  double delta;

  clutter_pan_gesture_get_delta_abs (gesture, &delta_vec);
  delta = get_gesture_dir_factor (self) *
          (self->orientation == CLUTTER_ORIENTATION_HORIZONTAL
           ? graphene_vec2_get_x (&delta_vec)
           : graphene_vec2_get_y (&delta_vec));

  update_gesture (self, delta, self->distance);
}

static void
pan_end_cb (ClutterGesture    *gesture,
            ShellSwipeTracker *self)
{
  graphene_vec2_t velocity;
  double v;

  clutter_pan_gesture_get_velocity (CLUTTER_PAN_GESTURE (gesture), &velocity);
  v = get_gesture_dir_factor (self) *
      (self->orientation == CLUTTER_ORIENTATION_HORIZONTAL
       ? graphene_vec2_get_x (&velocity)
       : graphene_vec2_get_y (&velocity));

  end_gesture (self, v, self->distance, FALSE);
}

static void
pan_cancel_cb (ClutterGesture    *gesture,
               ShellSwipeTracker *self)
{
  if (self->state != SWIPE_STATE_SCROLLING)
    return;

  self->cancelled = TRUE;
  end_gesture (self, 0, self->distance, FALSE);
}

static void
sync_pan_axis (ShellSwipeTracker *self)
{
  if (self->pan_gesture == NULL)
    return;

  clutter_pan_gesture_set_pan_axis (self->pan_gesture,
                                    self->orientation == CLUTTER_ORIENTATION_HORIZONTAL
                                    ? CLUTTER_PAN_AXIS_X : CLUTTER_PAN_AXIS_Y);
}

/* ------------------------------------------------------------------------ */
/* Touchpad swipe gesture (TouchpadSwipeGesture)                            */
/* ------------------------------------------------------------------------ */

static gboolean
touchpad_event_cb (ClutterActor       *actor,
                   const ClutterEvent *event,
                   ShellSwipeTracker  *self)
{
  ClutterTouchpadGesturePhase phase;
  gboolean vertical;
  float x, y;
  double dx, dy, delta, distance;
  int64_t time;

  if (clutter_event_type (event) != CLUTTER_TOUCHPAD_SWIPE)
    return CLUTTER_EVENT_PROPAGATE;

  phase = clutter_event_get_gesture_phase (event);

  if (phase == CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN)
    self->touchpad_state = TOUCHPAD_STATE_NONE;

  if (clutter_event_get_touchpad_gesture_finger_count (event) < GESTURE_FINGER_COUNT)
    return CLUTTER_EVENT_PROPAGATE;

  if (!mode_allowed (self))
    return CLUTTER_EVENT_PROPAGATE;

  if (!self->enabled)
    return CLUTTER_EVENT_PROPAGATE;

  if (self->touchpad_state == TOUCHPAD_STATE_IGNORED)
    return CLUTTER_EVENT_PROPAGATE;

  time = clutter_event_get_time (event);

  clutter_event_get_coords (event, &x, &y);
  clutter_event_get_gesture_motion_delta_unaccelerated (event, &dx, &dy);

  if (self->touchpad_state == TOUCHPAD_STATE_NONE)
    {
      if (dx == 0 && dy == 0)
        return CLUTTER_EVENT_PROPAGATE;

      self->cumulative_x = 0;
      self->cumulative_y = 0;
      self->touchpad_state = TOUCHPAD_STATE_PENDING;
    }

  if (self->touchpad_state == TOUCHPAD_STATE_PENDING)
    {
      double cdx, cdy, cumulative_distance;

      self->cumulative_x += dx;
      self->cumulative_y += dy;

      cdx = self->cumulative_x;
      cdy = self->cumulative_y;
      cumulative_distance = sqrt (cdx * cdx + cdy * cdy);

      if (cumulative_distance >= DRAG_THRESHOLD_DISTANCE)
        {
          ClutterOrientation gesture_orientation =
            fabs (cdx) > fabs (cdy) ? CLUTTER_ORIENTATION_HORIZONTAL
                                    : CLUTTER_ORIENTATION_VERTICAL;

          self->cumulative_x = 0;
          self->cumulative_y = 0;

          if (gesture_orientation == self->orientation)
            {
              self->touchpad_state = TOUCHPAD_STATE_HANDLING;
              begin_touchpad_gesture (self, time, x, y);
            }
          else
            {
              self->touchpad_state = TOUCHPAD_STATE_IGNORED;
              return CLUTTER_EVENT_PROPAGATE;
            }
        }
      else
        {
          return CLUTTER_EVENT_PROPAGATE;
        }
    }

  vertical = self->orientation == CLUTTER_ORIENTATION_VERTICAL;
  delta = vertical ? dy : dx;
  distance = vertical ? TOUCHPAD_BASE_HEIGHT : TOUCHPAD_BASE_WIDTH;

  switch (phase)
    {
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN:
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_UPDATE:
      if (self->touchpad_settings != NULL &&
          g_settings_get_boolean (self->touchpad_settings, "natural-scroll"))
        delta = -delta;

      update_touchpad_gesture (self, time, delta, distance);
      break;

    case CLUTTER_TOUCHPAD_GESTURE_PHASE_END:
    case CLUTTER_TOUCHPAD_GESTURE_PHASE_CANCEL:
      end_touchpad_gesture (self, time, distance);
      self->touchpad_state = TOUCHPAD_STATE_NONE;
      break;

    default:
      break;
    }

  return self->touchpad_state == TOUCHPAD_STATE_HANDLING
         ? CLUTTER_EVENT_STOP
         : CLUTTER_EVENT_PROPAGATE;
}

/* ------------------------------------------------------------------------ */
/* Scroll gesture (ScrollGesture)                                           */
/* ------------------------------------------------------------------------ */

static gboolean
can_handle_scroll_event (ShellSwipeTracker  *self,
                         const ClutterEvent *event)
{
  ClutterInputDevice *device;

  if (clutter_event_type (event) != CLUTTER_SCROLL)
    return FALSE;

  device = clutter_event_get_source_device (event);
  if (clutter_event_get_scroll_source (event) != CLUTTER_SCROLL_SOURCE_FINGER &&
      (device == NULL ||
       clutter_input_device_get_device_type (device) != CLUTTER_TOUCHPAD_DEVICE))
    return FALSE;

  if (!self->enabled)
    return FALSE;

  if (!mode_allowed (self))
    return FALSE;

  if (!self->scroll_began && self->scroll_modifiers != 0 &&
      (clutter_event_get_state (event) & self->scroll_modifiers) !=
      self->scroll_modifiers)
    return FALSE;

  return TRUE;
}

static gboolean
scroll_event_cb (ClutterActor       *actor,
                 const ClutterEvent *event,
                 ShellSwipeTracker  *self)
{
  gboolean vertical;
  double distance, dx, dy, delta;
  int64_t time;

  if (!can_handle_scroll_event (self, event))
    return CLUTTER_EVENT_PROPAGATE;

  if (clutter_event_get_scroll_direction (event) != CLUTTER_SCROLL_SMOOTH)
    return CLUTTER_EVENT_PROPAGATE;

  vertical = self->orientation == CLUTTER_ORIENTATION_VERTICAL;
  distance = vertical ? TOUCHPAD_BASE_HEIGHT : TOUCHPAD_BASE_WIDTH;

  time = clutter_event_get_time (event);
  clutter_event_get_scroll_delta (event, &dx, &dy);
  if (dx == 0 && dy == 0)
    {
      end_touchpad_gesture (self, time, distance);
      self->scroll_began = FALSE;
      return CLUTTER_EVENT_STOP;
    }

  if (!self->scroll_began)
    {
      float x, y;

      clutter_event_get_coords (event, &x, &y);
      begin_touchpad_gesture (self, time, x, y);
      self->scroll_began = TRUE;
    }

  delta = (vertical ? dy : dx) * SCROLL_MULTIPLIER;

  update_touchpad_gesture (self, time, delta, distance);

  return CLUTTER_EVENT_STOP;
}

/* ------------------------------------------------------------------------ */
/* Object                                                                   */
/* ------------------------------------------------------------------------ */

/* The inverse of what constructed () connected. The actor can be gone. */
static void
release_actor (ShellSwipeTracker *self)
{
  if (self->actor != NULL)
    {
      g_clear_signal_handler (&self->touchpad_handler_id, self->actor);
      g_clear_signal_handler (&self->scroll_handler_id, self->actor);
    }
  else
    {
      self->touchpad_handler_id = 0;
      self->scroll_handler_id = 0;
    }

  if (self->pan_gesture != NULL)
    {
      ClutterActor *pan_actor =
        clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->pan_gesture));

      g_signal_handlers_disconnect_by_data (self->pan_gesture, self);

      if (pan_actor != NULL)
        clutter_actor_remove_action (pan_actor,
                                     CLUTTER_ACTION (self->pan_gesture));
    }
}

static void
shell_swipe_tracker_constructed (GObject *object)
{
  ShellSwipeTracker *self = SHELL_SWIPE_TRACKER (object);
  GSettingsSchemaSource *source;
  g_autoptr (GSettingsSchema) schema = NULL;
  ShellGlobal *global;

  G_OBJECT_CLASS (shell_swipe_tracker_parent_class)->constructed (object);

  global = shell_global_get ();
  if (global != NULL)
    self->distance = shell_global_get_screen_height (global);

  g_return_if_fail (CLUTTER_IS_ACTOR (self->actor));

  self->pan_gesture = g_object_ref_sink (CLUTTER_PAN_GESTURE (clutter_pan_gesture_new ()));
  clutter_pan_gesture_set_min_n_points (self->pan_gesture,
                                        self->allow_drag ? 1 : GESTURE_FINGER_COUNT);
  clutter_pan_gesture_set_max_n_points (self->pan_gesture,
                                        self->allow_drag ? 0 : GESTURE_FINGER_COUNT);
  g_signal_connect (self->pan_gesture, "may-recognize",
                    G_CALLBACK (pan_may_recognize_cb), self);
  g_signal_connect (self->pan_gesture, "recognize",
                    G_CALLBACK (pan_recognize_cb), self);
  g_signal_connect (self->pan_gesture, "pan-update",
                    G_CALLBACK (pan_update_cb), self);
  g_signal_connect (self->pan_gesture, "end",
                    G_CALLBACK (pan_end_cb), self);
  g_signal_connect (self->pan_gesture, "cancel",
                    G_CALLBACK (pan_cancel_cb), self);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->pan_gesture),
                                  self->enabled);
  sync_pan_axis (self);
  clutter_actor_add_action_full (self->actor, self->name, self->phase,
                                 CLUTTER_ACTION (self->pan_gesture));

  source = g_settings_schema_source_get_default ();
  schema = source != NULL ? g_settings_schema_source_lookup (source, TOUCHPAD_SCHEMA, TRUE)
                          : NULL;
  if (schema != NULL)
    self->touchpad_settings = g_settings_new_full (schema, NULL, NULL);
  else
    g_warning ("Schema %s is not installed, natural scrolling is not followed",
               TOUCHPAD_SCHEMA);

  self->touchpad_handler_id =
    g_signal_connect (self->actor, "event",
                      G_CALLBACK (touchpad_event_cb), self);

  if (self->allow_scroll)
    self->scroll_handler_id =
      g_signal_connect (self->actor, "scroll-event",
                        G_CALLBACK (scroll_event_cb), self);
}

static void
shell_swipe_tracker_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellSwipeTracker *self = SHELL_SWIPE_TRACKER (object);

  switch (prop_id)
    {
    case PROP_ACTOR:
      self->actor = g_value_get_object (value);
      if (self->actor != NULL)
        g_object_add_weak_pointer (G_OBJECT (self->actor),
                                   (gpointer *) &self->actor);
      break;

    case PROP_ALLOWED_MODES:
      self->allowed_modes = g_value_get_flags (value);
      break;

    case PROP_ALLOW_DRAG:
      self->allow_drag = g_value_get_boolean (value);
      break;

    case PROP_ALLOW_SCROLL:
      self->allow_scroll = g_value_get_boolean (value);
      break;

    case PROP_PHASE:
      self->phase = g_value_get_enum (value);
      break;

    case PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;

    case PROP_ENABLED:
      shell_swipe_tracker_set_enabled (self, g_value_get_boolean (value));
      break;

    case PROP_ORIENTATION:
      shell_swipe_tracker_set_orientation (self, g_value_get_enum (value));
      break;

    case PROP_DISTANCE:
      shell_swipe_tracker_set_distance (self, g_value_get_double (value));
      break;

    case PROP_ALLOW_LONG_SWIPES:
      shell_swipe_tracker_set_allow_long_swipes (self, g_value_get_boolean (value));
      break;

    case PROP_SCROLL_MODIFIERS:
      shell_swipe_tracker_set_scroll_modifiers (self, g_value_get_flags (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_swipe_tracker_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellSwipeTracker *self = SHELL_SWIPE_TRACKER (object);

  switch (prop_id)
    {
    case PROP_ACTOR:
      g_value_set_object (value, self->actor);
      break;

    case PROP_ALLOWED_MODES:
      g_value_set_flags (value, self->allowed_modes);
      break;

    case PROP_ALLOW_DRAG:
      g_value_set_boolean (value, self->allow_drag);
      break;

    case PROP_ALLOW_SCROLL:
      g_value_set_boolean (value, self->allow_scroll);
      break;

    case PROP_PHASE:
      g_value_set_enum (value, self->phase);
      break;

    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case PROP_ENABLED:
      g_value_set_boolean (value, self->enabled);
      break;

    case PROP_ORIENTATION:
      g_value_set_enum (value, self->orientation);
      break;

    case PROP_DISTANCE:
      g_value_set_double (value, self->distance);
      break;

    case PROP_ALLOW_LONG_SWIPES:
      g_value_set_boolean (value, self->allow_long_swipes);
      break;

    case PROP_SCROLL_MODIFIERS:
      g_value_set_flags (value, self->scroll_modifiers);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_swipe_tracker_dispose (GObject *object)
{
  ShellSwipeTracker *self = SHELL_SWIPE_TRACKER (object);

  release_actor (self);

  if (self->actor != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->actor),
                                    (gpointer *) &self->actor);
      self->actor = NULL;
    }

  g_clear_object (&self->pan_gesture);
  g_clear_object (&self->touchpad_settings);

  G_OBJECT_CLASS (shell_swipe_tracker_parent_class)->dispose (object);
}

static void
shell_swipe_tracker_finalize (GObject *object)
{
  ShellSwipeTracker *self = SHELL_SWIPE_TRACKER (object);

  g_clear_pointer (&self->snap_points, g_array_unref);
  g_clear_pointer (&self->history, g_array_unref);
  g_clear_pointer (&self->name, g_free);

  G_OBJECT_CLASS (shell_swipe_tracker_parent_class)->finalize (object);
}

static void
shell_swipe_tracker_class_init (ShellSwipeTrackerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_swipe_tracker_constructed;
  object_class->set_property = shell_swipe_tracker_set_property;
  object_class->get_property = shell_swipe_tracker_get_property;
  object_class->dispose = shell_swipe_tracker_dispose;
  object_class->finalize = shell_swipe_tracker_finalize;

  /**
   * ShellSwipeTracker:actor:
   *
   * The actor the gestures are recognized on. Only a weak reference is kept.
   */
  props[PROP_ACTOR] =
    g_param_spec_object ("actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:allowed-modes:
   *
   * The #ShellActionMode flags in which the gestures are handled; they are
   * ignored when the current action mode is none of them.
   */
  props[PROP_ALLOWED_MODES] =
    g_param_spec_flags ("allowed-modes", NULL, NULL,
                        SHELL_TYPE_ACTION_MODE, SHELL_ACTION_MODE_NORMAL,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:allow-drag:
   *
   * Whether a one finger drag (or a mouse drag) on the actor is a swipe. If
   * not, only three finger drags on a touchscreen are (`params.allowDrag`).
   */
  props[PROP_ALLOW_DRAG] =
    g_param_spec_boolean ("allow-drag", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:allow-scroll:
   *
   * Whether touchpad scrolling on the actor is a swipe (`params.allowScroll`).
   */
  props[PROP_ALLOW_SCROLL] =
    g_param_spec_boolean ("allow-scroll", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:phase:
   *
   * The event phase in which the drag gesture sees the events
   * (`params.phase`).
   */
  props[PROP_PHASE] =
    g_param_spec_enum ("phase", NULL, NULL,
                       CLUTTER_TYPE_EVENT_PHASE, CLUTTER_PHASE_BUBBLE,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:name:
   *
   * The name of the drag gesture, for debugging (`params.name`).
   */
  props[PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL,
                         "SwipeTracker",
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:enabled:
   *
   * Whether swipes are tracked. Disabling the tracker while a swipe is in
   * progress interrupts it.
   */
  props[PROP_ENABLED] =
    g_param_spec_boolean ("enabled", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:orientation:
   *
   * The axis of the swipes. A #ShellSwipeTracker::begin handler may change it.
   */
  props[PROP_ORIENTATION] =
    g_param_spec_enum ("orientation", NULL, NULL,
                       CLUTTER_TYPE_ORIENTATION, CLUTTER_ORIENTATION_HORIZONTAL,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:distance:
   *
   * The size in pixels of one unit of progress for drags. It starts as the
   * height of the screen and is set by shell_swipe_tracker_confirm_swipe ().
   */
  props[PROP_DISTANCE] =
    g_param_spec_double ("distance", NULL, NULL,
                         0, G_MAXDOUBLE, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:allow-long-swipes:
   *
   * Whether a swipe can go past the neighbors of the point it started at, up
   * to the first and the last snap point.
   */
  props[PROP_ALLOW_LONG_SWIPES] =
    g_param_spec_boolean ("allow-long-swipes", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwipeTracker:scroll-modifiers:
   *
   * The modifiers which have to be held to start a swipe by scrolling, none
   * by default.
   */
  props[PROP_SCROLL_MODIFIERS] =
    g_param_spec_flags ("scroll-modifiers", NULL, NULL,
                        CLUTTER_TYPE_MODIFIER_TYPE, 0,
                        G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellSwipeTracker::begin:
   * @self: the #ShellSwipeTracker
   * @monitor: the index of the monitor the swipe started on
   *
   * Emitted when a swipe may start. The handler has to stop a deceleration
   * animation which is running (without resetting the progress), and call
   * shell_swipe_tracker_confirm_swipe (). If it does not, the swipe is ignored.
   * It may set #ShellSwipeTracker:orientation.
   */
  signals[BEGIN] =
    g_signal_new ("begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);

  /**
   * ShellSwipeTracker::update:
   * @self: the #ShellSwipeTracker
   * @progress: the new progress
   *
   * Emitted when the progress of a confirmed swipe changes. The handler
   * should set the progress to the given value.
   */
  signals[UPDATE] =
    g_signal_new ("update",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_DOUBLE);

  /**
   * ShellSwipeTracker::end:
   * @self: the #ShellSwipeTracker
   * @duration: the duration in milliseconds of the animation to @end_progress
   * @end_progress: the progress to animate to
   *
   * Emitted when a confirmed swipe ends. The handler should animate the
   * progress to @end_progress. If it is the cancel progress, nothing else
   * should happen afterwards, otherwise it should change the state, e.g. the
   * current page or workspace. @duration can be 0, in which case it should
   * finish instantly.
   */
  signals[END] =
    g_signal_new ("end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_UINT64, G_TYPE_DOUBLE);
}

static void
shell_swipe_tracker_init (ShellSwipeTracker *self)
{
  self->enabled = TRUE;
  self->orientation = CLUTTER_ORIENTATION_HORIZONTAL;
  self->snap_points = g_array_new (FALSE, FALSE, sizeof (double));
  self->history = g_array_new (FALSE, FALSE, sizeof (HistoryRecord));
  self->touchpad_state = TOUCHPAD_STATE_NONE;

  reset (self);
}

/**
 * shell_swipe_tracker_new:
 * @actor: the actor to track swipes on
 * @orientation: the axis of the swipes
 * @allowed_modes: the action modes in which swipes are handled
 *
 * Creates a #ShellSwipeTracker with the default #ShellSwipeTracker:allow-drag,
 * #ShellSwipeTracker:allow-scroll, #ShellSwipeTracker:phase and
 * #ShellSwipeTracker:name. Use g_object_new () to change these.
 *
 * Returns: (transfer full): a new #ShellSwipeTracker
 */
ShellSwipeTracker *
shell_swipe_tracker_new (ClutterActor       *actor,
                         ClutterOrientation  orientation,
                         ShellActionMode     allowed_modes)
{
  return g_object_new (SHELL_TYPE_SWIPE_TRACKER,
                       "actor", actor,
                       "orientation", orientation,
                       "allowed-modes", allowed_modes,
                       NULL);
}

/**
 * shell_swipe_tracker_get_enabled:
 * @self: a #ShellSwipeTracker
 *
 * Returns: the value of #ShellSwipeTracker:enabled
 */
gboolean
shell_swipe_tracker_get_enabled (ShellSwipeTracker *self)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self), FALSE);

  return self->enabled;
}

/**
 * shell_swipe_tracker_set_enabled:
 * @self: a #ShellSwipeTracker
 * @enabled: whether to track swipes
 *
 * Sets #ShellSwipeTracker:enabled.
 */
void
shell_swipe_tracker_set_enabled (ShellSwipeTracker *self,
                                 gboolean           enabled)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));

  enabled = !!enabled;
  if (self->enabled == enabled)
    return;

  self->enabled = enabled;
  if (!enabled && self->state == SWIPE_STATE_SCROLLING)
    interrupt (self);

  if (self->pan_gesture != NULL)
    clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->pan_gesture),
                                    enabled);
  self->scroll_began = FALSE;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ENABLED]);
}

/**
 * shell_swipe_tracker_get_orientation:
 * @self: a #ShellSwipeTracker
 *
 * Returns: the value of #ShellSwipeTracker:orientation
 */
ClutterOrientation
shell_swipe_tracker_get_orientation (ShellSwipeTracker *self)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self),
                        CLUTTER_ORIENTATION_HORIZONTAL);

  return self->orientation;
}

/**
 * shell_swipe_tracker_set_orientation:
 * @self: a #ShellSwipeTracker
 * @orientation: the axis of the swipes
 *
 * Sets #ShellSwipeTracker:orientation.
 */
void
shell_swipe_tracker_set_orientation (ShellSwipeTracker  *self,
                                     ClutterOrientation  orientation)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));

  if (self->orientation == orientation)
    return;

  self->orientation = orientation;
  sync_pan_axis (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ORIENTATION]);
}

/**
 * shell_swipe_tracker_get_distance:
 * @self: a #ShellSwipeTracker
 *
 * Returns: the value of #ShellSwipeTracker:distance
 */
double
shell_swipe_tracker_get_distance (ShellSwipeTracker *self)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self), 0);

  return self->distance;
}

/**
 * shell_swipe_tracker_set_distance:
 * @self: a #ShellSwipeTracker
 * @distance: the size in pixels of one unit of progress
 *
 * Sets #ShellSwipeTracker:distance.
 */
void
shell_swipe_tracker_set_distance (ShellSwipeTracker *self,
                                  double             distance)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));

  if (self->distance == distance)
    return;

  self->distance = distance;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DISTANCE]);
}

/**
 * shell_swipe_tracker_get_allow_long_swipes:
 * @self: a #ShellSwipeTracker
 *
 * Returns: the value of #ShellSwipeTracker:allow-long-swipes
 */
gboolean
shell_swipe_tracker_get_allow_long_swipes (ShellSwipeTracker *self)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self), FALSE);

  return self->allow_long_swipes;
}

/**
 * shell_swipe_tracker_set_allow_long_swipes:
 * @self: a #ShellSwipeTracker
 * @allow_long_swipes: whether a swipe can reach past the neighbors
 *
 * Sets #ShellSwipeTracker:allow-long-swipes.
 */
void
shell_swipe_tracker_set_allow_long_swipes (ShellSwipeTracker *self,
                                           gboolean           allow_long_swipes)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));

  allow_long_swipes = !!allow_long_swipes;
  if (self->allow_long_swipes == allow_long_swipes)
    return;

  self->allow_long_swipes = allow_long_swipes;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ALLOW_LONG_SWIPES]);
}

/**
 * shell_swipe_tracker_get_scroll_modifiers:
 * @self: a #ShellSwipeTracker
 *
 * Returns: the value of #ShellSwipeTracker:scroll-modifiers
 */
ClutterModifierType
shell_swipe_tracker_get_scroll_modifiers (ShellSwipeTracker *self)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self), 0);

  return self->scroll_modifiers;
}

/**
 * shell_swipe_tracker_set_scroll_modifiers:
 * @self: a #ShellSwipeTracker
 * @scroll_modifiers: the modifiers to hold to start a swipe by scrolling
 *
 * Sets #ShellSwipeTracker:scroll-modifiers.
 */
void
shell_swipe_tracker_set_scroll_modifiers (ShellSwipeTracker  *self,
                                          ClutterModifierType scroll_modifiers)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));

  if (self->scroll_modifiers == scroll_modifiers)
    return;

  self->scroll_modifiers = scroll_modifiers;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCROLL_MODIFIERS]);
}

/**
 * shell_swipe_tracker_get_allowed_modes:
 * @self: a #ShellSwipeTracker
 *
 * Returns: the value of #ShellSwipeTracker:allowed-modes
 */
ShellActionMode
shell_swipe_tracker_get_allowed_modes (ShellSwipeTracker *self)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self), SHELL_ACTION_MODE_NONE);

  return self->allowed_modes;
}

/**
 * shell_swipe_tracker_can_handle_scroll_event:
 * @self: a #ShellSwipeTracker
 * @scroll_event: an event to check
 *
 * This function can be used to combine swipe gesture and mouse scrolling.
 *
 * Returns: whether the event can be handled by the tracker
 */
gboolean
shell_swipe_tracker_can_handle_scroll_event (ShellSwipeTracker  *self,
                                             const ClutterEvent *scroll_event)
{
  g_return_val_if_fail (SHELL_IS_SWIPE_TRACKER (self), FALSE);
  g_return_val_if_fail (scroll_event != NULL, FALSE);

  if (!self->enabled || !self->allow_scroll)
    return FALSE;

  return can_handle_scroll_event (self, scroll_event);
}

/**
 * shell_swipe_tracker_confirm_swipe:
 * @self: a #ShellSwipeTracker
 * @distance: swipe distance in pixels
 * @snap_points: (array length=n_snap_points): the snap points, sorted in
 *   ascending order
 * @n_snap_points: the number of snap points, at least one
 * @current_progress: initial progress value
 * @cancel_progress: the value to be used on cancelling
 *
 * Confirms a swipe. The #ShellSwipeTracker::begin handler has to call this,
 * otherwise the swipe would not start. If there is an animation running, it
 * should be stopped first.
 *
 * @cancel_progress must always be a snap point, or a value matching some
 * other non-transient state. If no animation was running, @current_progress
 * and @cancel_progress should be the same.
 */
void
shell_swipe_tracker_confirm_swipe (ShellSwipeTracker *self,
                                   double             distance,
                                   const double      *snap_points,
                                   gsize              n_snap_points,
                                   double             current_progress,
                                   double             cancel_progress)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));
  g_return_if_fail (snap_points != NULL && n_snap_points > 0);

  shell_swipe_tracker_set_distance (self, distance);

  g_array_set_size (self->snap_points, 0);
  g_array_append_vals (self->snap_points, snap_points, n_snap_points);
  self->initial_progress = current_progress;
  self->progress = current_progress;
  self->cancel_progress = cancel_progress;

  self->state = SWIPE_STATE_SCROLLING;
}

/**
 * shell_swipe_tracker_destroy:
 * @self: a #ShellSwipeTracker
 *
 * Stops tracking: removes the gestures from the actor and disconnects from
 * its events. The tracker does not emit anything afterwards.
 */
void
shell_swipe_tracker_destroy (ShellSwipeTracker *self)
{
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (self));

  release_actor (self);
}
