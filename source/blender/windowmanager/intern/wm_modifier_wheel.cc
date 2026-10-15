/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Modifier wheel: a small floating control for touch screens.
 *
 * A finger cannot hold Ctrl while it taps, and half of Blender is a modifier and a click. The
 * wheel puts Ctrl, Shift and Alt within reach, latched, along with the Quick Favorites menu and,
 * while a modal operator is waiting for one, Enter and Esc.
 *
 * Ported from the official `android-demo` branch (Brainzman/blender, tracking issue #164191),
 * where it reads raw touch contacts from an SDL backend. This port has no such events: the
 * Android GHOST backend turns a finger into a pointer and buttons, so the wheel takes those, from
 * the same place and for the same reason as the on-screen keyboard (#wm_virtual_keyboard.cc):
 * #ghost_event_proc asks it before anything else, so a tap on the wheel never reaches a button or
 * an editor underneath.
 *
 * The modifiers it latches are the window's own (#wmEvent::modifier on the event state), pressed
 * and released with key events. That is the state the on-screen keyboard reads as well, so a Ctrl
 * turned on here shows lit there and the other way round, and a hardware keyboard's Ctrl simply
 * adds to it.
 *
 * The pen is ignored, as in the demo: a stylus is for drawing, and its tip landing on the wheel
 * should draw rather than press Ctrl.
 */

#include <algorithm>

#include "BLI_math_color_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "GHOST_Types.hh"
#include "GPU_state.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm_event_system.hh"
#include "wm_event_types.hh"
#include "wm_modifier_wheel.hh"
#include "wm_window.hh"

namespace blender {

namespace {

enum class WheelButton { None, Handle, Ctrl, Shift, Alt, Favorites, Confirm, Cancel };

struct ModifierWheel {
  /** Android hands out a single window; the wheel follows whichever one draws it. */
  wmWindow *win = nullptr;
  /** Shown by default the first time, then left as the user set it. */
  bool initialized = false;
  bool enabled = false;
  bool collapsed = false;
  /** Set while the status bar shows a plain Enter or Esc for the running modal operator. */
  bool show_confirm = false;
  bool show_cancel = false;
  /** Window coordinates where the user put it; negative until first placed. */
  float center[2] = {-1.0f, -1.0f};
  /** Where it is drawn and hit: #center, lifted above the on-screen keyboard while it is open. */
  float shown[2] = {-1.0f, -1.0f};

  /** Last pointer position from a finger or mouse, in window coordinates. */
  int pointer[2] = {0, 0};
  /** The button a press landed on, held until its release. */
  WheelButton pressed = WheelButton::None;
  /** Buttons pressed on the wheel: their releases belong to it too. */
  uint32_t captured_buttons = 0;
  int press_xy[2] = {0, 0};
  float press_center[2] = {0.0f, 0.0f};
  bool moved = false;
};

ModifierWheel g_wheel;

}  // namespace

static constexpr float WHEEL_SCALE = 0.9f;

static void wheel_redraw(wmWindow *win)
{
  if (bScreen *screen = WM_window_get_active_screen(win)) {
    screen->do_draw = true;
  }
}

static float wheel_radius(const ModifierWheel &wheel)
{
  return (wheel.collapsed ? 1.25f : 3.15f) * UI_UNIT_X * WHEEL_SCALE;
}

/**
 * Keep the wheel inside the window, and work out where it is shown: #ModifierWheel::center is
 * where the user put it, #ModifierWheel::shown is that lifted above the on-screen keyboard while
 * the keyboard is open, so it stands on the keyboard instead of covering its keys and goes back
 * down when it closes.
 */
static void wheel_clamp(ModifierWheel &wheel, const wmWindow *win)
{
  const int2 size = WM_window_native_pixel_size(win);
  const float radius = wheel_radius(wheel);
  if (wheel.center[0] < 0.0f) {
    /* Bottom left, above the timeline and status bar: out of the way of the tool bar and the
     * folded top bar, and where a left thumb rests on a phone held upright. */
    wheel.center[0] = radius + UI_UNIT_X;
    wheel.center[1] = radius + 4.0f * UI_UNIT_Y;
  }
  rcti keyboard;
  const bool keyboard_open = WM_virtual_keyboard_rect_get(win, &keyboard);
  for (int i = 0; i < 2; i++) {
    const float margin = std::min(radius + 0.25f * UI_UNIT_X, size[i] * 0.5f);
    const float upper = std::max(margin, float(size[i]) - margin);
    wheel.center[i] = std::clamp(wheel.center[i], margin, upper);
    float lower = margin;
    if (i == 1 && keyboard_open) {
      lower = std::min(float(keyboard.ymax) + margin, upper);
    }
    wheel.shown[i] = std::clamp(wheel.center[i], lower, upper);
  }
}

static rctf wheel_button_rect(const ModifierWheel &wheel, const WheelButton button)
{
  float x = wheel.shown[0], y = wheel.shown[1];
  const float offset = 2.15f * UI_UNIT_X * WHEEL_SCALE;
  switch (button) {
    case WheelButton::Shift:
      x -= offset;
      break;
    case WheelButton::Ctrl:
      y += offset;
      break;
    case WheelButton::Alt:
      y -= offset;
      break;
    case WheelButton::Favorites:
      x += offset;
      break;
    case WheelButton::Confirm:
      x += offset;
      y -= offset;
      break;
    case WheelButton::Cancel:
      x -= offset;
      y -= offset;
      break;
    default:
      break;
  }
  const float radius = (button == WheelButton::Handle && wheel.collapsed ? 1.25f : 1.0f) *
                       UI_UNIT_X * WHEEL_SCALE;
  return {x - radius, x + radius, y - radius, y + radius};
}

static WheelButton wheel_hit_test(const ModifierWheel &wheel, const int xy[2])
{
  if (!wheel.enabled || wheel.shown[0] < 0.0f) {
    return WheelButton::None;
  }
  for (const WheelButton button : {WheelButton::Handle,
                                   WheelButton::Ctrl,
                                   WheelButton::Shift,
                                   WheelButton::Alt,
                                   WheelButton::Favorites,
                                   WheelButton::Confirm,
                                   WheelButton::Cancel})
  {
    if ((button == WheelButton::Confirm && !wheel.show_confirm) ||
        (button == WheelButton::Cancel && !wheel.show_cancel))
    {
      continue;
    }
    const rctf rect = wheel_button_rect(wheel, button);
    const float dx = xy[0] - BLI_rctf_cent_x(&rect);
    const float dy = xy[1] - BLI_rctf_cent_y(&rect);
    const float radius = BLI_rctf_size_x(&rect) * 0.5f;
    if (dx * dx + dy * dy <= radius * radius) {
      return button;
    }
    if (wheel.collapsed) {
      break;
    }
  }
  return WheelButton::None;
}

static int wheel_button_modifier(const WheelButton button)
{
  switch (button) {
    case WheelButton::Ctrl:
      return KM_CTRL;
    case WheelButton::Shift:
      return KM_SHIFT;
    case WheelButton::Alt:
      return KM_ALT;
    default:
      return 0;
  }
}

static GHOST_TKey wheel_modifier_key(const int modifier)
{
  switch (modifier) {
    case KM_CTRL:
      return GHOST_kKeyLeftControl;
    case KM_SHIFT:
      return GHOST_kKeyLeftShift;
    case KM_ALT:
      return GHOST_kKeyLeftAlt;
    default:
      return GHOST_kKeyUnknown;
  }
}

static int wheel_modifiers_held(const wmWindow *win)
{
  return win->runtime->eventstate ? win->runtime->eventstate->modifier : 0;
}

/**
 * Press or release a modifier the way a keyboard would.
 *
 * Through #wm_event_add_ghostevent, like the on-screen keyboard's own modifiers, so the event
 * state carries it from here on: the next tap anywhere, and the next key from any keyboard.
 */
static void wheel_modifier_toggle(wmWindowManager *wm,
                                  wmWindow *win,
                                  const int modifier,
                                  const uint64_t time)
{
  const bool down = (wheel_modifiers_held(win) & modifier) == 0;
  GHOST_TEventKeyData kdata = {};
  kdata.key = wheel_modifier_key(modifier);
  kdata.is_repeat = false;
  kdata.utf8_buf[0] = '\0';
  wm_event_add_ghostevent(wm, win, down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp, &kdata, time);
}

static void wheel_button_activate(wmWindowManager *wm,
                                  wmWindow *win,
                                  const WheelButton button,
                                  const uint64_t time)
{
  ModifierWheel &wheel = g_wheel;
  if (const int modifier = wheel_button_modifier(button)) {
    wheel_modifier_toggle(wm, win, modifier, time);
  }
  else if (button == WheelButton::Handle) {
    wheel.collapsed = !wheel.collapsed;
    wheel_clamp(wheel, win);
  }
  else if (button == WheelButton::Favorites) {
    /* Opened while handling the event queue, where there is a context to open it in. */
    wmEvent event = *win->runtime->eventstate;
    event.type = EVT_MODIFIER_WHEEL_MENU;
    event.val = KM_PRESS;
    event.flag = eWM_EventFlag(0);
    event.utf8_buf[0] = '\0';
    WM_event_add(win, &event);
  }
  else if (ELEM(button, WheelButton::Confirm, WheelButton::Cancel)) {
    /* Plain Enter or Esc, whatever is latched: these are only shown for the unmodified keys. The
     * cursor is left where it was, so the operator sees no movement. */
    wmEvent event{};
    copy_v2_v2_int(event.xy, win->runtime->eventstate->xy);
    copy_v2_v2_int(event.prev_xy, event.xy);
    event.type = button == WheelButton::Confirm ? EVT_RETKEY : EVT_ESCKEY;
    event.val = KM_PRESS;
    WM_event_add(win, &event);
    event.val = KM_RELEASE;
    WM_event_add(win, &event);
  }
}

bool wm_modifier_wheel_ghost_event(wmWindowManager *wm,
                                   wmWindow *win,
                                   const int type,
                                   const void *customdata,
                                   const uint64_t time)
{
  ModifierWheel &wheel = g_wheel;
  if (!wheel.enabled || wheel.win != win) {
    return false;
  }
  /* The keyboard may have opened or closed since the last draw. */
  wheel_clamp(wheel, win);

  switch (type) {
    case GHOST_kEventCursorMove: {
      const GHOST_TEventCursorData *cd = static_cast<const GHOST_TEventCursorData *>(customdata);
      if (cd->tablet.Active != GHOST_kTabletModeNone) {
        return false;
      }
      int xy[2] = {cd->x, cd->y};
      wm_cursor_position_from_ghost_screen_coords(win, &xy[0], &xy[1]);
      copy_v2_v2_int(wheel.pointer, xy);
      if (wheel.pressed == WheelButton::None) {
        return false;
      }
      const float dx = float(xy[0] - wheel.press_xy[0]);
      const float dy = float(xy[1] - wheel.press_xy[1]);
      const float threshold = 5.0f * UI_SCALE_FAC;
      wheel.moved |= dx * dx + dy * dy > threshold * threshold;
      if (wheel.pressed == WheelButton::Handle && wheel.moved) {
        wheel.center[0] = wheel.press_center[0] + dx;
        wheel.center[1] = wheel.press_center[1] + dy;
        wheel_clamp(wheel, win);
        wheel_redraw(win);
      }
      return true;
    }
    case GHOST_kEventButtonDown:
    case GHOST_kEventButtonUp: {
      const GHOST_TEventButtonData *bd = static_cast<const GHOST_TEventButtonData *>(customdata);
      /* #GHOST_TButton is an index, not a mask: the left button is 0. Using it as a mask here
       * left the left button never recorded as captured, so its release never reached the wheel,
       * the press stayed live, and from then on the wheel followed every touch and swallowed every
       * tap in the window. */
      const uint32_t mask = 1u << uint32_t(bd->button);
      const bool down = type == GHOST_kEventButtonDown;
      if (down) {
        if (bd->tablet.Active != GHOST_kTabletModeNone) {
          return false;
        }
        if (bd->button == GHOST_kButtonMaskLeft) {
          /* A new press means any earlier one is over, whatever happened to its release. */
          wheel.pressed = WheelButton::None;
          wheel.captured_buttons &= ~mask;
        }
        const WheelButton button = wheel_hit_test(wheel, wheel.pointer);
        if (button == WheelButton::None) {
          return false;
        }
        /* Any button: a finger held still arrives as a right click (the backend's long press),
         * and that must not open a context menu under the wheel either. */
        wheel.captured_buttons |= mask;
        if (bd->button == GHOST_kButtonMaskLeft) {
          wheel.pressed = button;
          wheel.moved = false;
          copy_v2_v2_int(wheel.press_xy, wheel.pointer);
          copy_v2_v2(wheel.press_center, wheel.shown);
          /* Modifiers toggle on press, so the finger can slide off before lifting. */
          if (wheel_button_modifier(button)) {
            wheel_button_activate(wm, win, button, time);
          }
        }
        wheel_redraw(win);
        return true;
      }
      if (!(wheel.captured_buttons & mask)) {
        return false;
      }
      wheel.captured_buttons &= ~mask;
      if (bd->button == GHOST_kButtonMaskLeft && wheel.pressed != WheelButton::None) {
        const WheelButton button = wheel.pressed;
        wheel.pressed = WheelButton::None;
        if (!wheel_button_modifier(button) && !wheel.moved &&
            wheel_hit_test(wheel, wheel.pointer) == button)
        {
          wheel_button_activate(wm, win, button, time);
        }
        wheel_redraw(win);
      }
      return true;
    }
    default:
      return false;
  }
}

void wm_modifier_wheel_window_add(wmWindow *win)
{
  ModifierWheel &wheel = g_wheel;
  /* Also called after a file is read, which replaces the window struct. */
  wheel.win = win;
  if (!wheel.initialized) {
    wheel.initialized = true;
#ifdef __ANDROID__
    wheel.enabled = true;
#endif
  }
}

void wm_modifier_wheel_window_close(wmWindow *win)
{
  ModifierWheel &wheel = g_wheel;
  if (wheel.win == win) {
    wheel.win = nullptr;
    wheel.pressed = WheelButton::None;
    wheel.captured_buttons = 0;
  }
}

void WM_modifier_wheel_confirm_set(wmWindow *win, const bool confirm, const bool cancel)
{
  ModifierWheel &wheel = g_wheel;
  if (wheel.win != win || (wheel.show_confirm == confirm && wheel.show_cancel == cancel)) {
    return;
  }
  wheel.show_confirm = confirm;
  wheel.show_cancel = cancel;
  wheel_redraw(win);
}

void WM_modifier_wheel_confirm_add(wmWindow *win, const bool confirm, const bool cancel)
{
  ModifierWheel &wheel = g_wheel;
  WM_modifier_wheel_confirm_set(
      win, wheel.show_confirm || confirm, wheel.show_cancel || cancel);
}

void wm_modifier_wheel_popup(bContext *C, wmWindow *win)
{
  const ModifierWheel &wheel = g_wheel;
  bScreen *screen = WM_window_get_active_screen(win);
  if (!wheel.enabled || wheel.win != win || !screen) {
    return;
  }

  /* The menu runs in the editor under the cursor, which the wheel never moves: it is where the
   * last tap outside the wheel landed. */
  const int *xy = win->runtime->eventstate->xy;
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, xy);
  if (!area) {
    area = screen->areabase.first();
  }
  if (!area) {
    return;
  }
  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_ANY, xy);
  if (!region) {
    region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  }
  if (!region) {
    return;
  }

  wmWindow *previous_window = CTX_wm_window(C);
  ScrArea *previous_area = CTX_wm_area(C);
  ARegion *previous_region = CTX_wm_region(C);
  CTX_wm_window_set(C, win);
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);
  WM_tooltip_clear(C, win);
  /* Beside the wheel. A menu opens centered on the cursor, which here is the wheel itself, and
   * pushed back against the window edge it would cover it. */
  const int anchor[2] = {int(wheel.shown[0] + wheel_radius(wheel) + 0.5f * UI_UNIT_X),
                         int(wheel.shown[1])};
  ui::popup_menu_invoke(C, "SCREEN_MT_user_menu", nullptr, anchor);
  CTX_wm_window_set(C, previous_window);
  CTX_wm_area_set(C, previous_area);
  CTX_wm_region_set(C, previous_region);
}

static void wheel_draw_icon(const int icon,
                            const float x,
                            const float y,
                            const float size,
                            const uchar color[4],
                            const bool active)
{
  /* Some key icons, such as Ctrl, are wider than a single key. */
  const float offset = ui::event_icon_offset(icon);
  const float width = size * (offset >= 2.0f ? 2.0f : offset >= 1.0f ? 1.5f : 1.0f);
  ui::icon_draw_ex(x - width * 0.5f,
                   y - size * 0.5f,
                   icon,
                   ICON_DEFAULT_HEIGHT / size,
                   color[3] / 255.0f,
                   0.0f,
                   color,
                   false,
                   UI_NO_ICON_OVERLAY_TEXT,
                   active);
}

static void wheel_draw_button(const ModifierWheel &wheel,
                              const int held,
                              const WheelButton button,
                              const int icon)
{
  const uiWidgetColors &colors = ui::theme::theme_get()->tui.wcol_toolbar_item;
  const bool active = (held & wheel_button_modifier(button)) != 0;
  float fill[4], outline[4];
  rgba_uchar_to_float(fill, active ? colors.inner_sel : colors.inner);
  rgba_uchar_to_float(outline, colors.outline);
  if (wheel.pressed == button && !wheel.moved) {
    for (int i = 0; i < 3; i++) {
      fill[i] = std::min(fill[i] + 0.08f, 1.0f);
    }
  }
  const rctf rect = wheel_button_rect(wheel, button);
  ui::draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_4fv_ex(
      &rect, fill, nullptr, 0.0f, outline, UI_SCALE_FAC, BLI_rctf_size_x(&rect) * 0.5f);
  const uchar *text = active ? colors.text_sel : colors.text;
  const float x = BLI_rctf_cent_x(&rect);
  const float y = BLI_rctf_cent_y(&rect);
  const float unit = UI_UNIT_X * WHEEL_SCALE;
  /* Collapsed, the handle shows what is latched, so a held Ctrl is never invisible. */
  const bool show_modifiers = button == WheelButton::Handle && wheel.collapsed &&
                              (held & (KM_CTRL | KM_SHIFT | KM_ALT));
  wheel_draw_icon(icon, x, y + (show_modifiers ? 0.25f * unit : 0.0f), unit, text, active);
  if (show_modifiers) {
    const int icons[] = {ICON_EVENT_CTRL, ICON_EVENT_SHIFT, ICON_EVENT_ALT};
    const int modifiers[] = {KM_CTRL, KM_SHIFT, KM_ALT};
    for (int i = 0; i < 3; i++) {
      if (held & modifiers[i]) {
        wheel_draw_icon(icons[i],
                        x + (i - 1) * 0.75f * unit,
                        y - 0.65f * unit,
                        0.4f * unit,
                        colors.text_sel,
                        true);
      }
    }
  }
}

void wm_modifier_wheel_draw(wmWindow *win)
{
  ModifierWheel &wheel = g_wheel;
  if (!wheel.enabled || wheel.win != win) {
    return;
  }
  wheel_clamp(wheel, win);
  const int held = wheel_modifiers_held(win);
  GPU_blend(GPU_BLEND_ALPHA);
  wheel_draw_button(
      wheel, held, WheelButton::Handle, wheel.collapsed ? ICON_ADD : ICON_COLLAPSEMENU);
  if (!wheel.collapsed) {
    wheel_draw_button(wheel, held, WheelButton::Ctrl, ICON_EVENT_CTRL);
    wheel_draw_button(wheel, held, WheelButton::Shift, ICON_EVENT_SHIFT);
    wheel_draw_button(wheel, held, WheelButton::Alt, ICON_EVENT_ALT);
    wheel_draw_button(wheel, held, WheelButton::Favorites, ICON_SOLO_ON);
    if (wheel.show_confirm) {
      wheel_draw_button(wheel, held, WheelButton::Confirm, ICON_EVENT_RETURN);
    }
    if (wheel.show_cancel) {
      wheel_draw_button(wheel, held, WheelButton::Cancel, ICON_EVENT_ESC);
    }
  }
  GPU_blend(GPU_BLEND_NONE);
}

static wmOperatorStatus modifier_wheel_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  wmWindow *win = CTX_wm_window(C);
  ModifierWheel &wheel = g_wheel;
  wheel.win = win;
  wheel.enabled = !wheel.enabled;
  wheel.pressed = WheelButton::None;
  wheel.captured_buttons = 0;
  wheel_redraw(win);
  return OPERATOR_FINISHED;
}

void WM_OT_modifier_wheel_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Modifier Wheel";
  ot->idname = "WM_OT_modifier_wheel_toggle";
  ot->description = "Show or hide the floating Ctrl, Shift and Alt controls for touch screens";
  ot->exec = modifier_wheel_toggle_exec;
  ot->poll = WM_operator_winactive;
}

}  // namespace blender
