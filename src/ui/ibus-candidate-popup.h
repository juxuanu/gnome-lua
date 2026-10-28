/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ibus-candidate-popup.h: The popup which shows the preedit text, the
 * auxiliary text and the candidates of the current IBus engine next to the
 * text cursor. Port of js/ui/ibusCandidatePopup.js.
 *
 * #ShellIBusCandidatePopup is created by the input source manager
 * (ui/status/keyboard.h), which gives it the panel service of IBus with
 * shell_ibus_candidate_popup_set_panel_service () whenever that changes. The
 * list of candidates (`CandidateArea`) is private to the C file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <ibus.h>

#include "ui/box-pointer.h"

G_BEGIN_DECLS

#define SHELL_TYPE_IBUS_CANDIDATE_POPUP (shell_ibus_candidate_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellIBusCandidatePopup, shell_ibus_candidate_popup,
                      SHELL, IBUS_CANDIDATE_POPUP, ShellBoxPointer)

ShellIBusCandidatePopup *shell_ibus_candidate_popup_new               (void);

void                     shell_ibus_candidate_popup_set_panel_service (ShellIBusCandidatePopup *self,
                                                                       IBusPanelService        *panel_service);

G_END_DECLS
