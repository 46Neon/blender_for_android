/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Modifier wheel: floating Ctrl, Shift, Alt, Quick Favorites, Enter and Esc for touch screens.
 * See `wm_modifier_wheel.cc`.
 */

#pragma once

#include <cstdint>

namespace blender {

struct bContext;
struct wmOperatorType;
struct wmWindow;
struct wmWindowManager;

/** Attach the wheel to a new window; it is shown by default on Android. */
void wm_modifier_wheel_window_add(wmWindow *win);
void wm_modifier_wheel_window_close(wmWindow *win);

/**
 * Take a GHOST event that lands on the wheel, before it reaches anything else in the window.
 * \return true when the event was consumed.
 */
bool wm_modifier_wheel_ghost_event(
    wmWindowManager *wm, wmWindow *win, int type, const void *customdata, uint64_t time);

/** Drawn over everything else in the window, the on-screen keyboard included. */
void wm_modifier_wheel_draw(wmWindow *win);

/** Open Quick Favorites, in reply to the #EVT_MODIFIER_WHEEL_MENU event the wheel queued. */
void wm_modifier_wheel_popup(bContext *C, wmWindow *win);

void WM_OT_modifier_wheel_toggle(wmOperatorType *ot);

}  // namespace blender
