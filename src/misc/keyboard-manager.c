/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyboard-manager.c: Applies the XKB layouts of the input sources to the
 * compositor keymap. Port of js/misc/keyboardManager.js.
 *
 * The user layouts are split into groups of at most
 * %SHELL_KEYBOARD_MANAGER_MAX_LAYOUTS_PER_GROUP layouts (one slot is kept
 * for the layout of the locale, which is appended to every group that does
 * not contain it). One keymap is built per group and activated on the
 * backend; switching between layouts of the same group only changes the
 * layout index.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <meta/meta-backend.h>

#include "misc/keyboard-manager.h"
#include <libgnome-desktop/gnome-languages.h>
#include "shell-global.h"

enum
{
  PROP_0,

  PROP_SHORT_NAME,
  PROP_DISPLAY_NAME,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  KEYMAP_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct _LayoutSet LayoutSet;

/* The user layouts of one setUserLayouts () call */
typedef struct
{
  char *id;
  char *layout;
  char *variant;
  char *display_name;
  char *short_name;
  GPtrArray *group;     /* (element-type LayoutInfo) unowned, owned by the set */
  unsigned int group_index;
} LayoutInfo;

struct _LayoutSet
{
  grefcount ref_count;
  GHashTable *infos;    /* id -> LayoutInfo, owned */
  GPtrArray *groups;    /* (element-type GPtrArray) owned */
};

/* The layout of the locale, the only part of LayoutInfo that is needed */
typedef struct
{
  char *layout;
  char *variant;
} LocaleLayout;

typedef struct
{
  char *layouts;
  char *variants;
  char *options;
  char *model;
  GStrv display_names;
  GStrv short_names;
} CurrentKeymap;

struct _ShellKeyboardManager
{
  GObject parent;

  GnomeXkbInfo *xkb_info;

  /* The info that was applied last. It points into @current_set, which is
   * kept alive for as long as the info is current, even if the user layouts
   * were replaced meanwhile. */
  LayoutInfo *current;
  LayoutSet *current_set;

  LayoutSet *layout_set;
  LocaleLayout locale_layout;
  CurrentKeymap *current_keymap;

  char **xkb_options;
  char *xkb_model;

  /* The keymap that the backend reports, set on the first keymap-changed */
  MetaKeymapDescription *keymap_description;
  /* The keymap that was last built here */
  MetaKeymapDescription *our_keymap_description;
};

G_DEFINE_FINAL_TYPE (ShellKeyboardManager, shell_keyboard_manager, G_TYPE_OBJECT)

static void
layout_info_free (LayoutInfo *info)
{
  g_free (info->id);
  g_free (info->layout);
  g_free (info->variant);
  g_free (info->display_name);
  g_free (info->short_name);
  g_free (info);
}

static LayoutSet *
layout_set_new (void)
{
  LayoutSet *set = g_new0 (LayoutSet, 1);

  g_ref_count_init (&set->ref_count);
  set->infos = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                      (GDestroyNotify) layout_info_free);
  set->groups = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);

  return set;
}

static LayoutSet *
layout_set_ref (LayoutSet *set)
{
  g_ref_count_inc (&set->ref_count);

  return set;
}

static void
layout_set_unref (LayoutSet *set)
{
  if (!g_ref_count_dec (&set->ref_count))
    return;

  g_clear_pointer (&set->infos, g_hash_table_unref);
  g_clear_pointer (&set->groups, g_ptr_array_unref);
  g_free (set);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LayoutSet, layout_set_unref)

static void
current_keymap_free (CurrentKeymap *keymap)
{
  if (keymap == NULL)
    return;

  g_free (keymap->layouts);
  g_free (keymap->variants);
  g_free (keymap->options);
  g_free (keymap->model);
  g_strfreev (keymap->display_names);
  g_strfreev (keymap->short_names);
  g_free (keymap);
}

static void
get_locale_layout (ShellKeyboardManager *self)
{
  const char * const *names = g_get_language_names ();
  const char *locale = names[0];
  const char *type = NULL;
  const char *id = NULL;
  const char *layout = NULL;
  const char *variant = NULL;
  gboolean found;

  if (strchr (locale, '_') == NULL)
    locale = SHELL_KEYBOARD_MANAGER_DEFAULT_LOCALE;

  found = gnome_get_input_source_from_locale (locale, &type, &id);
  if (!found)
    found = gnome_get_input_source_from_locale (SHELL_KEYBOARD_MANAGER_DEFAULT_LOCALE,
                                                &type, &id);

  if (found && id != NULL &&
      gnome_xkb_info_get_layout_info (self->xkb_info, id, NULL, NULL,
                                      &layout, &variant))
    {
      self->locale_layout.layout = g_strdup (layout);
      self->locale_layout.variant = g_strdup (variant);
    }
  else
    {
      self->locale_layout.layout = g_strdup (SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT);
      self->locale_layout.variant = g_strdup (SHELL_KEYBOARD_MANAGER_DEFAULT_VARIANT);
    }
}

static gboolean
is_locale_layout (ShellKeyboardManager *self,
                  const LayoutInfo     *info)
{
  return g_strcmp0 (info->layout, self->locale_layout.layout) == 0 &&
         g_strcmp0 (info->variant, self->locale_layout.variant) == 0;
}

static gboolean
group_has_locale_layout (ShellKeyboardManager *self,
                         GPtrArray            *group)
{
  unsigned int i;

  for (i = 0; i < group->len; i++)
    {
      if (is_locale_layout (self, g_ptr_array_index (group, i)))
        return TRUE;
    }

  return FALSE;
}

/*
 * Builds the comma-separated layout and variant strings of @group, adding
 * the layout of the locale if the group does not contain it.
 */
static void
build_group_strings (ShellKeyboardManager  *self,
                     GPtrArray             *group,
                     char                 **layouts,
                     char                 **variants)
{
  g_autoptr (GString) layouts_str = g_string_new (NULL);
  g_autoptr (GString) variants_str = g_string_new (NULL);
  unsigned int i;

  for (i = 0; i < group->len; i++)
    {
      LayoutInfo *info = g_ptr_array_index (group, i);

      if (i > 0)
        {
          g_string_append_c (layouts_str, ',');
          g_string_append_c (variants_str, ',');
        }

      g_string_append (layouts_str, info->layout);
      g_string_append (variants_str, info->variant);
    }

  if (!group_has_locale_layout (self, group))
    {
      if (group->len > 0)
        {
          g_string_append_c (layouts_str, ',');
          g_string_append_c (variants_str, ',');
        }

      g_string_append (layouts_str, self->locale_layout.layout);
      g_string_append (variants_str, self->locale_layout.variant);
    }

  *layouts = g_string_free (g_steal_pointer (&layouts_str), FALSE);
  *variants = g_string_free (g_steal_pointer (&variants_str), FALSE);
}

static char *
build_options_string (ShellKeyboardManager *self)
{
  if (self->xkb_options == NULL)
    return g_strdup ("");

  return g_strjoinv (",", self->xkb_options);
}

/* Returns whether the keymap that has to be applied for @info changed */
static gboolean
update_current_keymap (ShellKeyboardManager *self,
                       LayoutInfo           *info)
{
  g_autofree char *options = build_options_string (self);
  g_autofree char *layouts = NULL;
  g_autofree char *variants = NULL;
  CurrentKeymap *keymap;
  unsigned int i;

  build_group_strings (self, info->group, &layouts, &variants);

  if (self->current_keymap != NULL &&
      g_strcmp0 (self->current_keymap->layouts, layouts) == 0 &&
      g_strcmp0 (self->current_keymap->variants, variants) == 0 &&
      g_strcmp0 (self->current_keymap->options, options) == 0 &&
      g_strcmp0 (self->current_keymap->model, self->xkb_model) == 0)
    return FALSE;

  keymap = g_new0 (CurrentKeymap, 1);
  keymap->layouts = g_steal_pointer (&layouts);
  keymap->variants = g_steal_pointer (&variants);
  keymap->options = g_steal_pointer (&options);
  keymap->model = g_strdup (self->xkb_model);

  /* The names are those of the group only, the appended locale layout has
   * none. */
  keymap->display_names = g_new0 (char *, info->group->len + 1);
  keymap->short_names = g_new0 (char *, info->group->len + 1);
  for (i = 0; i < info->group->len; i++)
    {
      LayoutInfo *member = g_ptr_array_index (info->group, i);

      keymap->display_names[i] = g_strdup (member->display_name);
      keymap->short_names[i] = g_strdup (member->short_name);
    }

  current_keymap_free (self->current_keymap);
  self->current_keymap = keymap;

  return TRUE;
}

static MetaKeymapDescription *
create_keymap_description (ShellKeyboardManager *self)
{
  CurrentKeymap *keymap = self->current_keymap;

  return meta_keymap_description_new_from_rules (keymap->model,
                                                 keymap->layouts,
                                                 keymap->variants,
                                                 keymap->options,
                                                 keymap->display_names,
                                                 keymap->short_names);
}

static MetaBackend *
get_backend (void)
{
  return shell_global_get_backend (shell_global_get ());
}

static ClutterKeymap *
get_seat_keymap (void)
{
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));
  ClutterBackend *backend =
    clutter_context_get_backend (clutter_actor_get_context (stage));
  ClutterSeat *seat = clutter_backend_get_default_seat (backend);

  return clutter_seat_get_keymap (seat);
}

static void
on_keymap_changed (MetaBackend          *backend,
                   ShellKeyboardManager *self)
{
  g_clear_pointer (&self->keymap_description, meta_keymap_description_unref);

  self->keymap_description = meta_backend_get_keymap_description (backend);
  if (self->keymap_description != NULL)
    meta_keymap_description_ref (self->keymap_description);

  g_signal_emit (self, signals[KEYMAP_CHANGED], 0);
}

static void
on_keymap_layout_group_changed (MetaBackend          *backend,
                                unsigned int          layout_index,
                                ShellKeyboardManager *self)
{
  g_signal_emit (self, signals[KEYMAP_CHANGED], 0);
}

/*
 * The signal is declared to return a reference (the GValue handed to the
 * emitter owns it), so the handler returns a new one.
 */
static MetaKeymapDescription *
on_reset_keymap_description (MetaBackend          *backend,
                             ShellKeyboardManager *self)
{
  if (self->our_keymap_description == NULL)
    return NULL;

  return meta_keymap_description_ref (self->our_keymap_description);
}

static unsigned int
on_reset_keymap_layout_index (MetaBackend          *backend,
                              ShellKeyboardManager *self)
{
  if (self->current == NULL)
    return 0;

  return self->current->group_index;
}

typedef struct
{
  ShellKeyboardManager *self;
  LayoutSet *set;
  LayoutInfo *info;
} ApplyData;

static void
apply_data_free (ApplyData *data)
{
  layout_set_unref (data->set);
  g_object_unref (data->self);
  g_free (data);
}

static void
set_current (ShellKeyboardManager *self,
             LayoutSet            *set,
             LayoutInfo           *info)
{
  LayoutSet *old_set = self->current_set;

  self->current_set = layout_set_ref (set);
  self->current = info;

  g_clear_pointer (&old_set, layout_set_unref);
}

static void
on_set_keymap_done (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  ApplyData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (meta_backend_set_keymap_finish (META_BACKEND (source), res, &error))
    set_current (data->self, data->set, data->info);
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Failed to apply the keymap: %s", error->message);

  apply_data_free (data);
}

static void
do_apply (ShellKeyboardManager *self,
          const char           *id)
{
  LayoutInfo *info;
  gboolean recreate;

  if (self->layout_set == NULL)
    return;

  info = g_hash_table_lookup (self->layout_set->infos, id);
  if (info == NULL)
    return;

  if (update_current_keymap (self, info))
    recreate = TRUE;
  else if (shell_keyboard_manager_is_external (self))
    recreate = TRUE;
  else
    recreate = FALSE;

  if (recreate)
    {
      g_clear_pointer (&self->our_keymap_description, meta_keymap_description_unref);
      self->our_keymap_description = create_keymap_description (self);
    }

  if (recreate || self->current == NULL ||
      self->current->group_index != info->group_index)
    {
      ApplyData *data = g_new0 (ApplyData, 1);

      data->self = g_object_ref (self);
      data->set = layout_set_ref (self->layout_set);
      data->info = info;

      meta_backend_set_keymap_async (get_backend (),
                                     self->our_keymap_description,
                                     info->group_index,
                                     NULL,
                                     on_set_keymap_done,
                                     data);
      return;
    }

  set_current (self, self->layout_set, info);
}

/**
 * shell_keyboard_manager_get_default:
 *
 * Gets the #ShellKeyboardManager singleton. It connects to the keymap
 * signals of the backend when it is first created.
 *
 * Returns: (transfer none): the #ShellKeyboardManager
 */
ShellKeyboardManager *
shell_keyboard_manager_get_default (void)
{
  static ShellKeyboardManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_KEYBOARD_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_keyboard_manager_get_xkb_info:
 * @self: a #ShellKeyboardManager
 *
 * Gets the #GnomeXkbInfo shared by everything that needs to look up layouts.
 * This replaces the former module function `getXkbInfo ()`.
 *
 * Returns: (transfer none): the #GnomeXkbInfo
 */
GnomeXkbInfo *
shell_keyboard_manager_get_xkb_info (ShellKeyboardManager *self)
{
  g_return_val_if_fail (SHELL_IS_KEYBOARD_MANAGER (self), NULL);

  return self->xkb_info;
}

/**
 * shell_keyboard_manager_apply:
 * @self: a #ShellKeyboardManager
 * @id: the input source id of one of the user layouts
 *
 * Activates the layout @id, rebuilding and applying the keymap of its group
 * if it differs from the current one. Does nothing if @id is not one of the
 * layouts set with shell_keyboard_manager_set_user_layouts ().
 */
void
shell_keyboard_manager_apply (ShellKeyboardManager *self,
                              const char           *id)
{
  g_return_if_fail (SHELL_IS_KEYBOARD_MANAGER (self));
  g_return_if_fail (id != NULL);

  do_apply (self, id);
}

/**
 * shell_keyboard_manager_reapply:
 * @self: a #ShellKeyboardManager
 *
 * Applies the current layout again, e.g. after the keyboard options or model
 * changed. Does nothing if no layout was applied since the user layouts were
 * set.
 */
void
shell_keyboard_manager_reapply (ShellKeyboardManager *self)
{
  g_autofree char *id = NULL;

  g_return_if_fail (SHELL_IS_KEYBOARD_MANAGER (self));

  if (self->current == NULL)
    return;

  /* The id is copied because applying can replace the current layout */
  id = g_strdup (self->current->id);
  do_apply (self, id);
}

/**
 * shell_keyboard_manager_set_user_layouts:
 * @self: a #ShellKeyboardManager
 * @ids: (array zero-terminated=1) (element-type utf8): the input source ids
 *   of the XKB layouts, in the order of the user's input sources
 *
 * Replaces the layouts that can be activated, and forgets the current one.
 * Ids that are unknown to #GnomeXkbInfo are ignored.
 */
void
shell_keyboard_manager_set_user_layouts (ShellKeyboardManager *self,
                                         const char * const   *ids)
{
  g_autoptr (LayoutSet) set = NULL;
  GPtrArray *group = NULL;
  int i;

  g_return_if_fail (SHELL_IS_KEYBOARD_MANAGER (self));

  /* Whatever is still being applied keeps its own reference to the old set */
  self->current = NULL;
  g_clear_pointer (&self->current_set, layout_set_unref);
  g_clear_pointer (&self->layout_set, layout_set_unref);

  set = layout_set_new ();

  for (i = 0; ids != NULL && ids[i] != NULL; i++)
    {
      const char *display_name = NULL;
      const char *short_name = NULL;
      const char *layout = NULL;
      const char *variant = NULL;
      LayoutInfo *info;

      if (!gnome_xkb_info_get_layout_info (self->xkb_info, ids[i],
                                           &display_name, &short_name,
                                           &layout, &variant))
        continue;

      info = g_new0 (LayoutInfo, 1);
      info->id = g_strdup (ids[i]);
      info->layout = g_strdup (layout);
      info->variant = g_strdup (variant);
      info->display_name = g_strdup (display_name);
      info->short_name = g_strdup (short_name);

      /* An id that is listed twice replaces its earlier info */
      g_hash_table_replace (set->infos, info->id, info);
    }

  for (i = 0; ids != NULL && ids[i] != NULL; i++)
    {
      LayoutInfo *info = g_hash_table_lookup (set->infos, ids[i]);
      gboolean is_locale;
      gboolean has_locale;

      if (info == NULL)
        continue;

      is_locale = is_locale_layout (self, info);
      has_locale = group != NULL && group_has_locale_layout (self, group);

      /* We need to leave one slot on the group free if we haven't included
       * the locale layout yet. This ensures we can add a layout containing
       * the symbols for the language used in UI strings. */
      if (group != NULL &&
          (group->len == SHELL_KEYBOARD_MANAGER_MAX_LAYOUTS_PER_GROUP ||
           (group->len == SHELL_KEYBOARD_MANAGER_MAX_LAYOUTS_PER_GROUP - 1 &&
            !has_locale && !is_locale)))
        group = NULL;

      if (group == NULL)
        {
          group = g_ptr_array_new ();
          g_ptr_array_add (set->groups, group);
        }

      info->group_index = group->len;
      g_ptr_array_add (group, info);
      info->group = group;
    }

  self->layout_set = g_steal_pointer (&set);
}

/**
 * shell_keyboard_manager_set_keyboard_options:
 * @self: a #ShellKeyboardManager
 * @options: (array zero-terminated=1) (element-type utf8) (nullable): the XKB
 *   options, e.g. `caps:escape`
 *
 * Sets the XKB options used for the next keymap. Call
 * shell_keyboard_manager_reapply () to apply them.
 */
void
shell_keyboard_manager_set_keyboard_options (ShellKeyboardManager *self,
                                             const char * const   *options)
{
  g_return_if_fail (SHELL_IS_KEYBOARD_MANAGER (self));

  g_strfreev (self->xkb_options);
  self->xkb_options = g_strdupv ((char **) options);
}

/**
 * shell_keyboard_manager_set_keyboard_model:
 * @self: a #ShellKeyboardManager
 * @model: the XKB keyboard model
 *
 * Sets the XKB model used for the next keymap. Call
 * shell_keyboard_manager_reapply () to apply it.
 */
void
shell_keyboard_manager_set_keyboard_model (ShellKeyboardManager *self,
                                           const char           *model)
{
  g_return_if_fail (SHELL_IS_KEYBOARD_MANAGER (self));

  g_free (self->xkb_model);
  self->xkb_model = g_strdup (model != NULL ? model : "");
}

/**
 * shell_keyboard_manager_get_current_layout_id:
 * @self: a #ShellKeyboardManager
 *
 * Gets the id of the layout that was applied last (the former
 * `currentLayout.id`).
 *
 * Returns: (nullable): the input source id, or %NULL if no layout was applied
 *   since the user layouts were set
 */
const char *
shell_keyboard_manager_get_current_layout_id (ShellKeyboardManager *self)
{
  g_return_val_if_fail (SHELL_IS_KEYBOARD_MANAGER (self), NULL);

  if (self->current == NULL)
    return NULL;

  return self->current->id;
}

/**
 * shell_keyboard_manager_get_short_name:
 * @self: a #ShellKeyboardManager
 *
 * Gets the short name of the layout that is active in the seat keymap, which
 * can be set by something other than the shell.
 *
 * Returns: (nullable): the short name
 */
const char *
shell_keyboard_manager_get_short_name (ShellKeyboardManager *self)
{
  g_return_val_if_fail (SHELL_IS_KEYBOARD_MANAGER (self), NULL);

  return clutter_keymap_get_current_short_name (get_seat_keymap ());
}

/**
 * shell_keyboard_manager_get_display_name:
 * @self: a #ShellKeyboardManager
 *
 * Gets the display name of the layout that is active in the seat keymap,
 * which can be set by something other than the shell.
 *
 * Returns: (nullable): the display name
 */
const char *
shell_keyboard_manager_get_display_name (ShellKeyboardManager *self)
{
  g_return_val_if_fail (SHELL_IS_KEYBOARD_MANAGER (self), NULL);

  return clutter_keymap_get_current_display_name (get_seat_keymap ());
}

/**
 * shell_keyboard_manager_is_locked:
 * @self: a #ShellKeyboardManager
 *
 * Returns: whether the keymap was set by an external client that locked it,
 *   so that the shell must not change it. %FALSE until the backend reported
 *   its first keymap.
 */
gboolean
shell_keyboard_manager_is_locked (ShellKeyboardManager *self)
{
  g_return_val_if_fail (SHELL_IS_KEYBOARD_MANAGER (self), FALSE);

  if (self->keymap_description == NULL)
    return FALSE;

  return meta_keymap_description_is_locked (self->keymap_description);
}

/**
 * shell_keyboard_manager_is_external:
 * @self: a #ShellKeyboardManager
 *
 * Returns: whether the keymap of the backend is not the one that was built
 *   here, i.e. it was set by an external client. %FALSE until the backend
 *   reported its first keymap.
 */
gboolean
shell_keyboard_manager_is_external (ShellKeyboardManager *self)
{
  g_return_val_if_fail (SHELL_IS_KEYBOARD_MANAGER (self), FALSE);

  if (self->keymap_description == NULL)
    return FALSE;

  if (self->our_keymap_description == NULL)
    return TRUE;

  return !meta_keymap_description_direct_equal (self->keymap_description,
                                                self->our_keymap_description);
}

static void
shell_keyboard_manager_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellKeyboardManager *self = SHELL_KEYBOARD_MANAGER (object);

  switch (prop_id)
    {
    case PROP_SHORT_NAME:
      g_value_set_string (value, shell_keyboard_manager_get_short_name (self));
      break;

    case PROP_DISPLAY_NAME:
      g_value_set_string (value, shell_keyboard_manager_get_display_name (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_keyboard_manager_constructed (GObject *object)
{
  ShellKeyboardManager *self = SHELL_KEYBOARD_MANAGER (object);
  MetaBackend *backend = get_backend ();

  G_OBJECT_CLASS (shell_keyboard_manager_parent_class)->constructed (object);

  get_locale_layout (self);

  g_signal_connect_object (backend, "keymap-changed",
                           G_CALLBACK (on_keymap_changed), self, 0);
  g_signal_connect_object (backend, "keymap-layout-group-changed",
                           G_CALLBACK (on_keymap_layout_group_changed), self, 0);
  g_signal_connect_object (backend, "reset-keymap-description",
                           G_CALLBACK (on_reset_keymap_description), self, 0);
  g_signal_connect_object (backend, "reset-keymap-layout-index",
                           G_CALLBACK (on_reset_keymap_layout_index), self, 0);
}

static void
shell_keyboard_manager_dispose (GObject *object)
{
  ShellKeyboardManager *self = SHELL_KEYBOARD_MANAGER (object);

  self->current = NULL;
  g_clear_pointer (&self->current_set, layout_set_unref);
  g_clear_pointer (&self->layout_set, layout_set_unref);
  g_clear_pointer (&self->keymap_description, meta_keymap_description_unref);
  g_clear_pointer (&self->our_keymap_description, meta_keymap_description_unref);
  g_clear_object (&self->xkb_info);

  G_OBJECT_CLASS (shell_keyboard_manager_parent_class)->dispose (object);
}

static void
shell_keyboard_manager_finalize (GObject *object)
{
  ShellKeyboardManager *self = SHELL_KEYBOARD_MANAGER (object);

  g_clear_pointer (&self->current_keymap, current_keymap_free);
  g_clear_pointer (&self->locale_layout.layout, g_free);
  g_clear_pointer (&self->locale_layout.variant, g_free);
  g_clear_pointer (&self->xkb_options, g_strfreev);
  g_clear_pointer (&self->xkb_model, g_free);

  G_OBJECT_CLASS (shell_keyboard_manager_parent_class)->finalize (object);
}

static void
shell_keyboard_manager_class_init (ShellKeyboardManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_keyboard_manager_constructed;
  object_class->get_property = shell_keyboard_manager_get_property;
  object_class->dispose = shell_keyboard_manager_dispose;
  object_class->finalize = shell_keyboard_manager_finalize;

  /**
   * ShellKeyboardManager:short-name:
   *
   * The short name of the layout that is active in the seat keymap.
   */
  props[PROP_SHORT_NAME] =
    g_param_spec_string ("short-name", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellKeyboardManager:display-name:
   *
   * The display name of the layout that is active in the seat keymap.
   */
  props[PROP_DISPLAY_NAME] =
    g_param_spec_string ("display-name", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellKeyboardManager::keymap-changed:
   * @self: the #ShellKeyboardManager
   *
   * Emitted when the keymap of the backend or its active layout changed,
   * whoever caused it.
   */
  signals[KEYMAP_CHANGED] =
    g_signal_new ("keymap-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_keyboard_manager_init (ShellKeyboardManager *self)
{
  self->xkb_info = gnome_xkb_info_new ();
  self->xkb_model = g_strdup ("");
}
