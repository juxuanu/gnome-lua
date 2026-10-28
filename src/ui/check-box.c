/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * check-box.c: A toggle button with a check mark and a wrapping label. Port
 * of js/ui/checkBox.js.
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>

#include "ui/check-box.h"

struct _ShellCheckBox
{
  StButton parent;

  StWidget *label; /* weak, owned by the box layout */
};

G_DEFINE_FINAL_TYPE (ShellCheckBox, shell_check_box, ST_TYPE_BUTTON)

static void
shell_check_box_dispose (GObject *object)
{
  ShellCheckBox *self = SHELL_CHECK_BOX (object);

  g_clear_weak_pointer (&self->label);

  G_OBJECT_CLASS (shell_check_box_parent_class)->dispose (object);
}

static void
shell_check_box_class_init (ShellCheckBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_check_box_dispose;
}

static void
shell_check_box_init (ShellCheckBox *self)
{
  StButton *button = ST_BUTTON (self);
  StWidget *container, *box, *check, *label;
  ClutterText *text;

  container = st_box_layout_new ();
  clutter_actor_set_x_expand (CLUTTER_ACTOR (container), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (container), TRUE);

  st_widget_set_style_class_name (ST_WIDGET (self), "check-box");
  st_bin_set_child (ST_BIN (self), CLUTTER_ACTOR (container));
  st_button_set_button_mask (button, ST_BUTTON_PRIMARY);
  st_button_set_toggle_mode (button, TRUE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_CHECK_BOX);

  box = st_bin_new ();
  clutter_actor_set_y_align (CLUTTER_ACTOR (box), CLUTTER_ACTOR_ALIGN_START);
  clutter_actor_add_child (CLUTTER_ACTOR (container), CLUTTER_ACTOR (box));

  check = ST_WIDGET (st_icon_new ());
  st_icon_set_icon_name (ST_ICON (check), "check-symbolic");
  st_bin_set_child (ST_BIN (box), CLUTTER_ACTOR (check));

  label = st_label_new (NULL);
  clutter_actor_set_y_align (CLUTTER_ACTOR (label), CLUTTER_ACTOR_ALIGN_CENTER);
  text = st_label_get_clutter_text (ST_LABEL (label));
  clutter_text_set_line_wrap (text, TRUE);
  clutter_text_set_ellipsize (text, PANGO_ELLIPSIZE_NONE);
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (label));
  clutter_actor_add_child (CLUTTER_ACTOR (container), CLUTTER_ACTOR (label));

  g_set_weak_pointer (&self->label, label);
}

/**
 * shell_check_box_new:
 * @label: (nullable): the text of the label
 *
 * Creates a new check box. There is no `label` construct property: that name
 * belongs to #StButton, whose setter would replace the box layout child.
 *
 * Returns: (transfer floating): a new #ShellCheckBox
 */
ShellCheckBox *
shell_check_box_new (const char *label)
{
  ShellCheckBox *self = g_object_new (SHELL_TYPE_CHECK_BOX, NULL);

  if (label != NULL && *label != '\0')
    shell_check_box_set_label (self, label);

  return self;
}

/**
 * shell_check_box_set_label:
 * @self: a #ShellCheckBox
 * @label: (nullable): the new text of the label
 *
 * Sets the text displayed next to the check mark. This hides
 * st_button_set_label(), which must not be used on a check box.
 */
void
shell_check_box_set_label (ShellCheckBox *self,
                           const char    *label)
{
  g_return_if_fail (SHELL_IS_CHECK_BOX (self));

  if (self->label != NULL)
    st_label_set_text (ST_LABEL (self->label), label);
}

/**
 * shell_check_box_get_label_actor:
 * @self: a #ShellCheckBox
 *
 * Returns: (transfer none) (nullable): the #StLabel showing the text
 */
ClutterActor *
shell_check_box_get_label_actor (ShellCheckBox *self)
{
  g_return_val_if_fail (SHELL_IS_CHECK_BOX (self), NULL);

  return CLUTTER_ACTOR (self->label);
}
