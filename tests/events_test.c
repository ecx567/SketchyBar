// tests/events_test.c
//
// S5 system-event routing suite (tasks 5.1-5.7). Links the Windows event core
// end-to-end and drives it through the real message pump:
//
//   platform/win_main.c    hidden window + GetMessage/DispatchMessage pump
//   platform/win_events.c  WM_* -> portable-event router (CGEvent mirror)
//   src/event.c            portable dispatch table (event_post, untested
//                          dispatch table content test: nothing here touches
//                          the event_handler[] initializer itself)
//   src/animation.c        16 ms SetTimer animator beat
//   src/hotload.c           ReadDirectoryChangesW watcher + 500 ms debounce
//   src/mouse.c            Windows no-op mouse_begin (compile check)
//
// The bar-manager surface (bar_manager_*, bar_item_*, handle_message_mach,
// windows_unfreeze) is stubbed below; the stubs' signatures mirror
// src/bar_manager.h / src/bar_item.h and are checked at compile time by
// including those headers. g_bar_manager / g_connection / g_space_management_mode
// live here; g_config_file comes from the linked src/hotload.c.
//
// All stimuli are posted Win32 messages; every assertion polls a stub counter
// with a deadline while the pump thread runs skbar_win_main(). The pump is
// stopped with WM_CLOSE (PostQuitMessage) before each test returns.

#ifdef SKBAR_CMOCKA_SHIM
#include "cmocka_shim.h"
#else
#include <setjmp.h>
#include <cmocka.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "event.h"
#include "bar_manager.h"
#include "bar_item.h"
#include "hotload.h"
#include "animation.h"
#include "mach.h"
#include "win_platform.h"

#include <windows.h>

// ---------------------------------------------------------------------------
// Portable-core globals this link defines (event.c externs them).
// ---------------------------------------------------------------------------
struct bar_manager g_bar_manager;
int g_space_management_mode = 0;
int g_connection = 0;
char g_name[256] = "events_test";
char g_config_file[4096];   /* hotload.c externs it; sketchybar.c owns it in prod */

// ---------------------------------------------------------------------------
// Stub state.
// ---------------------------------------------------------------------------
static struct bar_item s_item_a;
static struct bar_item s_item_b;
static struct bar_item* s_hit_item;      /* get_item_by_* return value */

static int s_entered_local;
static int s_entered_global;
static int s_exited_local;
static int s_exited_global;
static int s_scrolled_global;
static struct bar_item* s_last_enter_item;
static struct bar_item* s_last_exit_item;
static struct bar_item* s_scroll_item;
static int s_scroll_delta;

static int s_click_total;
static struct bar_item* s_click_item;
static CGEventType s_click_type;
static uint32_t s_click_button;
static uint32_t s_click_flags;

static int s_mach_messages;
static int s_animator_refresh;
static int s_display_change;
static int s_display_added;
static int s_will_sleep;
static int s_woke;
static int s_destroy;
static int s_init;
static int s_begin;
static int s_resize;
static int s_refresh;
static int s_update;
static int s_unfreeze;
static int s_volume;
static int s_wifi;
static int s_brightness;
static int s_power_source;
static int s_media;
static int s_cover;
static int s_space_windows;
static int s_notification;
static int s_front_app;
static int s_space_change;
static int s_removed;
static int s_moved;
static int s_resized;
static int s_drag;

static void reset_stubs(void) {
  s_entered_local = s_entered_global = s_exited_local = s_exited_global = 0;
  s_scrolled_global = s_click_total = s_mach_messages = 0;
  s_animator_refresh = s_display_change = s_display_added = 0;
  s_will_sleep = s_woke = s_destroy = s_init = s_begin = 0;
  s_resize = s_refresh = s_update = s_unfreeze = s_volume = 0;
  s_wifi = s_brightness = s_power_source = s_media = s_cover = 0;
  s_space_windows = s_notification = s_front_app = s_space_change = 0;
  s_removed = s_moved = s_resized = s_drag = 0;
  s_last_enter_item = s_last_exit_item = s_scroll_item = NULL;
  s_click_item = NULL;
  s_scroll_delta = 0;
  s_click_type = 0;
  s_click_button = s_click_flags = 0;
}

// ---------------------------------------------------------------------------
// Bar-manager stubs (signatures match src/bar_manager.h / src/bar_item.h).
// ---------------------------------------------------------------------------
struct bar_item* bar_manager_get_item_by_wid(struct bar_manager* bar_manager,
                                             uint32_t wid,
                                             struct window** result) {
  (void)bar_manager;
  if (result) *result = NULL;
  // wid 0 is the S5 "no display window" key; route to the test's hit item so
  // the wid-first lookup paths see the same item the point-based router found.
  return (wid == 0) ? s_hit_item : NULL;
}

struct bar_item* bar_manager_get_item_by_point(struct bar_manager* bar_manager,
                                               CGPoint point,
                                               struct window** result) {
  (void)bar_manager;
  (void)point;
  if (result) *result = NULL;
  return s_hit_item;
}

struct bar* bar_manager_get_bar_by_wid(struct bar_manager* bar_manager,
                                       uint32_t wid) {
  (void)bar_manager;
  (void)wid;
  return NULL;
}

struct bar* bar_manager_get_bar_by_point(struct bar_manager* bar_manager,
                                         CGPoint point) {
  (void)bar_manager;
  (void)point;
  return NULL;
}

struct popup* bar_manager_get_popup_by_wid(struct bar_manager* bar_manager,
                                           uint32_t wid) {
  (void)bar_manager;
  (void)wid;
  return NULL;
}

struct popup* bar_manager_get_popup_by_point(struct bar_manager* bar_manager,
                                             CGPoint point) {
  (void)bar_manager;
  (void)point;
  return NULL;
}

bool bar_manager_mouse_over_any_bar(struct bar_manager* bar_manager) {
  (void)bar_manager;
  return false;
}

bool bar_manager_mouse_over_any_popup(struct bar_manager* bar_manager) {
  (void)bar_manager;
  return false;
}

void bar_manager_handle_mouse_entered(struct bar_manager* bar_manager,
                                      struct bar_item* item) {
  (void)bar_manager;
  s_entered_local++;
  s_last_enter_item = item;
}

void bar_manager_handle_mouse_entered_global(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_entered_global++;
}

void bar_manager_handle_mouse_exited(struct bar_manager* bar_manager,
                                     struct bar_item* item) {
  (void)bar_manager;
  s_exited_local++;
  s_last_exit_item = item;
}

void bar_manager_handle_mouse_exited_global(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_exited_global++;
}

void bar_manager_handle_mouse_scrolled_global(struct bar_manager* bar_manager,
                                              int scroll_delta,
                                              uint32_t did,
                                              uint32_t modifier) {
  (void)bar_manager;
  (void)did;
  (void)modifier;
  s_scrolled_global++;
  s_scroll_delta = scroll_delta;
}

void bar_item_on_click(struct bar_item* item, uint32_t type,
                       uint32_t mouse_button_code, uint32_t modifier,
                       CGPoint point_in_window_coords) {
  (void)point_in_window_coords;
  s_click_total++;
  s_click_item = item;
  s_click_type = (CGEventType)type;
  s_click_button = mouse_button_code;
  s_click_flags = modifier;
}

void bar_item_on_drag(struct bar_item* item, CGPoint point_in_window_coords) {
  (void)item;
  (void)point_in_window_coords;
  s_drag++;
}

void bar_item_on_scroll(struct bar_item* item, int scroll_delta,
                        uint32_t modifier) {
  (void)modifier;
  s_scroll_item = item;
  s_scroll_delta = scroll_delta;
}

void bar_manager_handle_notification(struct bar_manager* bar_manager,
                                     struct notification* notification) {
  (void)bar_manager;
  (void)notification;
  s_notification++;
}

void bar_manager_handle_front_app_switch(struct bar_manager* bar_manager,
                                         char* info) {
  (void)bar_manager;
  (void)info;
  s_front_app++;
}

void bar_manager_handle_space_change(struct bar_manager* bar_manager,
                                     bool forced) {
  (void)bar_manager;
  (void)forced;
  s_space_change++;
}

void bar_manager_handle_display_change(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_display_change++;
}

void bar_manager_display_added(struct bar_manager* bar_manager, uint32_t did) {
  (void)bar_manager;
  (void)did;
  s_display_added++;
}

void bar_manager_display_removed(struct bar_manager* bar_manager, uint32_t did) {
  (void)bar_manager;
  (void)did;
  s_removed++;
}

void bar_manager_display_moved(struct bar_manager* bar_manager, uint32_t did) {
  (void)bar_manager;
  (void)did;
  s_moved++;
}

void bar_manager_display_resized(struct bar_manager* bar_manager, uint32_t did) {
  (void)bar_manager;
  (void)did;
  s_resized++;
}

void bar_manager_resize(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_resize++;
}

void bar_manager_refresh(struct bar_manager* bar_manager, bool force) {
  (void)bar_manager;
  (void)force;
  s_refresh++;
}

void bar_manager_handle_system_woke(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_woke++;
}

void bar_manager_handle_system_will_sleep(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_will_sleep++;
}

void bar_manager_update(struct bar_manager* bar_manager, bool force) {
  (void)bar_manager;
  (void)force;
  s_update++;
}

void bar_manager_animator_refresh(struct bar_manager* bar_manager,
                                  uint64_t timestamp) {
  (void)bar_manager;
  (void)timestamp;
  s_animator_refresh++;
}

void bar_manager_handle_volume_change(struct bar_manager* bar_manager,
                                      float diff) {
  (void)bar_manager;
  (void)diff;
  s_volume++;
}

void bar_manager_handle_wifi_change(struct bar_manager* bar_manager,
                                    char* ssid) {
  (void)bar_manager;
  (void)ssid;
  s_wifi++;
}

void bar_manager_handle_brightness_change(struct bar_manager* bar_manager,
                                          float brightness) {
  (void)bar_manager;
  (void)brightness;
  s_brightness++;
}

void bar_manager_handle_power_source_change(struct bar_manager* bar_manager,
                                            char* source) {
  (void)bar_manager;
  (void)source;
  s_power_source++;
}

void bar_manager_handle_media_change(struct bar_manager* bar_manager,
                                     char* title) {
  (void)bar_manager;
  (void)title;
  s_media++;
}

void bar_manager_handle_media_cover_change(struct bar_manager* bar_manager,
                                           CGImageRef image) {
  (void)bar_manager;
  (void)image;
  s_cover++;
}

void bar_manager_handle_space_windows_change(struct bar_manager* bar_manager,
                                             char* title) {
  (void)bar_manager;
  (void)title;
  s_space_windows++;
}

void bar_manager_destroy(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_destroy++;
}

void bar_manager_init(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_init++;
}

void bar_manager_begin(struct bar_manager* bar_manager) {
  (void)bar_manager;
  s_begin++;
}

void bar_manager_poll_active_display(struct bar_manager* bar_manager) {
  (void)bar_manager;
}

void windows_unfreeze(void) {
  s_unfreeze++;
}

// animation.c's animation_update marks the target item dirty through this;
// the fake items are static so a no-op is exactly right for this link.
void bar_item_needs_update(struct bar_item* bar_item) {
  (void)bar_item;
}

void handle_message_mach(struct mach_buffer* buffer) {
  (void)buffer;
  s_mach_messages++;
}

// ---------------------------------------------------------------------------
// Pump harness: the suite runs skbar_win_main() on a dedicated thread so the
// pump is real (GetMessage/DispatchMessage on the hidden event window) and all
// stimuli go through PostMessageW like they would in production.
// ---------------------------------------------------------------------------
static HANDLE s_pump_thread;

static DWORD WINAPI pump_thread_main(LPVOID arg) {
  (void)arg;
  return skbar_win_main(0, NULL);
}

static void pump_start(void) {
  reset_stubs();
  s_pump_thread = CreateThread(NULL, 0, pump_thread_main, NULL, 0, NULL);
  assert_non_null(s_pump_thread);
  ULONGLONG deadline = GetTickCount64() + 3000;
  while (skbar_win_event_hwnd() == 0 && GetTickCount64() < deadline) Sleep(10);
  assert_true(skbar_win_event_hwnd() != 0);
}

static void pump_stop(void) {
  HWND hwnd = (HWND)skbar_win_event_hwnd();
  if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
  assert_int_equal(WaitForSingleObject(s_pump_thread, 3000), WAIT_OBJECT_0);
  CloseHandle(s_pump_thread);
  s_pump_thread = NULL;
  // Give the window a beat to be gone before the next test recreates it.
  while (skbar_win_event_hwnd() != 0) Sleep(10);
}

static int wait_for_counter(int* counter, int target, DWORD deadline_ms) {
  ULONGLONG deadline = GetTickCount64() + deadline_ms;
  while (*counter < target && GetTickCount64() < deadline) Sleep(10);
  return *counter >= target;
}

// ---------------------------------------------------------------------------
// Watch helper: worker-thread producer used by the marshalling test.
// ---------------------------------------------------------------------------
static struct event s_worker_event;

static DWORD WINAPI worker_post(LPVOID arg) {
  (void)arg;
  event_post(&s_worker_event);
  return 0;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

// 5.7 (a): a struct event produced on a worker thread reaches the dispatch
// table through the WM_APP_EVENT marshal (skbar_win_post_event copy + pump).
static void test_event_dispatch_from_worker_thread(void** state) {
  (void)state;
  pump_start();

  s_worker_event = (struct event){ NULL, MACH_MESSAGE };
  HANDLE worker = CreateThread(NULL, 0, worker_post, NULL, 0, NULL);
  assert_non_null(worker);
  assert_int_equal(WaitForSingleObject(worker, 2000), WAIT_OBJECT_0);
  CloseHandle(worker);

  assert_true(wait_for_counter(&s_mach_messages, 1, 3000));
  assert_int_equal(s_mach_messages, 1);
  assert_true(s_unfreeze >= 1);   /* event_execute() teardown ran on the pump */

  pump_stop();
}

// 5.7 (b): WM_MOUSEMOVE / WM_LBUTTONUP / WM_MOUSEWHEEL / WM_MOUSELEAVE route
// into MOUSE_* with correct hit-testing and hover transition bookkeeping.
static void test_mouse_routing_and_hover(void** state) {
  (void)state;
  pump_start();

  HWND hwnd = (HWND)skbar_win_event_hwnd();
  s_hit_item = &s_item_a;

  // First move: enter item A.
  PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(100, 80));
  assert_true(wait_for_counter(&s_entered_local, 1, 2000));
  assert_int_equal(s_entered_local + s_entered_global, 1);
  assert_ptr_equal(s_last_enter_item, &s_item_a);

  // Same item move: no spurious enter/exit pair.
  PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(120, 90));
  Sleep(100);
  assert_int_equal(s_entered_local + s_entered_global, 1);
  assert_int_equal(s_exited_local + s_exited_global, 0);

  // Item change under the cursor: exit A, enter B.
  s_hit_item = &s_item_b;
  PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(140, 100));
  assert_true(wait_for_counter(&s_exited_local, 1, 2000));
  assert_ptr_equal(s_last_exit_item, &s_item_a);
  assert_true(wait_for_counter(&s_entered_local, 2, 2000));
  assert_ptr_equal(s_last_enter_item, &s_item_b);

  // Left-button release: click on the hit item with the mirror's CG types.
  PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(150, 110));
  assert_true(wait_for_counter(&s_click_total, 1, 2000));
  assert_ptr_equal(s_click_item, &s_item_b);
  assert_int_equal(s_click_type, kCGEventLeftMouseUp);
  assert_int_equal(s_click_button, 0);

  // Wheel: MOUSE_SCROLLED with the WHEEL_DELTA-normalized delta (120 -> 1).
  // event.c routes to bar_item_on_scroll when an item is hit, or to the
  // scrolled-global handler otherwise - accept either sink.
  PostMessageW(hwnd, WM_MOUSEWHEEL, MAKELPARAM(0, WHEEL_DELTA),
               MAKELPARAM(400, 300));
  {
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (s_scrolled_global < 1 && s_scroll_item == NULL
           && GetTickCount64() < deadline) Sleep(10);
    assert_true(s_scrolled_global >= 1 || s_scroll_item == &s_item_b);
    assert_int_equal(s_scroll_delta, 1);
  }

  // Mouse leave: hover chain ends with an exit of the current item.
  PostMessageW(hwnd, WM_MOUSELEAVE, 0, 0);
  assert_true(wait_for_counter(&s_exited_local, 2, 2000));
  assert_ptr_equal(s_last_exit_item, &s_item_b);

  pump_stop();
}

// 5.7 (c): the animator's 60 Hz beat arrives as ANIMATOR_REFRESH through the
// 16 ms SetTimer (WM_TIMER routing), timestamped with the QPC clock.
static void test_animator_timer_fires(void** state) {
  (void)state;
  pump_start();

  struct animator animator;
  memset(&animator, 0, sizeof(animator));
  animator_init(&animator);               /* SetTimer on the hidden window */
  assert_true(skbar_animator_timer_id() != 0);

  struct animation* animation = animation_create();
  animation_setup(animation, NULL, NULL, 0, 100, 60, INTERP_FUNCTION_LINEAR);
  animator_add(&animator, animation);

  assert_true(wait_for_counter(&s_animator_refresh, 1, 3000));
  assert_true(animator.clock > 0.0);      /* QPC frequency, not CVDisplayLink */

  animator_destroy(&animator);
  assert_int_equal(skbar_animator_timer_id(), 0);

  pump_stop();
}

// 5.7 (d): ReadDirectoryChangesW watcher: file changes fire HOTLOAD (marshalled
// through the pump) with the 500 ms debounce, and hotload_stop_watching() tears
// the thread down.
static void test_hotload_watcher_and_debounce(void** state) {
  (void)state;
  pump_start();

  char dir[MAX_PATH];
  char path[MAX_PATH];
  GetTempPathA(sizeof(dir), dir);
  strcat_s(dir, sizeof(dir), "skbar_evt_test");
  CreateDirectoryA(dir, NULL);
  snprintf(path, sizeof(path), "%s\\rc", dir);

  FILE* f = fopen(path, "w");
  assert_non_null(f);
  fprintf(f, "echo hotload\n");
  fclose(f);

  assert_true(set_config_file_path(path));
  hotload_set_state(HOTLOAD_STATE_ENABLED);
  // Note: hotload.h declares begin_receiving_config_change_events() as void
  // (its macOS definition returns int without ever including that header);
  // the watcher thread's health is verified through the s_destroy counter.
  begin_receiving_config_change_events();

  // First change: fires (debounce window is armed on the first fire).
  //
  // The watcher thread must be INSIDE ReadDirectoryChangesW before any change
  // is observable, and RDCW does not replay notifications that happen before
  // the first read is armed. Rather than racing that arming moment with a
  // single touch, touch repeatedly until the first fire is observed; the
  // debounce then suppresses the trailing touches of this loop, so exactly one
  // destroy/init/begin cycle is counted.
  for (int attempt = 0; attempt < 120 && s_destroy == 0; ++attempt) {
    f = fopen(path, "a");
    assert_non_null(f);
    fputs("x\n", f);
    fclose(f);
    Sleep(25);
  }
  assert_true(wait_for_counter(&s_destroy, 1, 3000));   /* event_hotload ran */
  assert_int_equal(s_destroy + s_init + s_begin, 3);    /* destroy+init+begin */

  // Second change within the 500 ms window: suppressed.
  f = fopen(path, "a");
  assert_non_null(f);
  fputs("y\n", f);
  fclose(f);
  Sleep(100);
  assert_int_equal(s_destroy, 1);

  // Change after the debounce window expires: fires again. The watcher reads
  // the directory, so wait comfortably past 500 ms before touching the file.
  Sleep(700);
  f = fopen(path, "a");
  assert_non_null(f);
  fputs("z\n", f);
  fclose(f);
  assert_true(wait_for_counter(&s_destroy, 2, 3000));
  assert_int_equal(s_destroy, 2);

  hotload_stop_watching();
  DeleteFileA(path);
  RemoveDirectoryA(dir);

  pump_stop();
}

// 5.7 (e): WM_DISPLAYCHANGE and WM_POWERBROADCAST map onto the portable
// display/power events (the win_events router feeds SYSTEM_* / DISPLAY_CHANGED).
static void test_display_and_power_events(void** state) {
  (void)state;
  pump_start();

  HWND hwnd = (HWND)skbar_win_event_hwnd();

  PostMessageW(hwnd, WM_DISPLAYCHANGE, 0, 0);
  assert_true(wait_for_counter(&s_display_change, 1, 2000));

  PostMessageW(hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
  assert_true(wait_for_counter(&s_will_sleep, 1, 2000));

  PostMessageW(hwnd, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
  assert_true(wait_for_counter(&s_woke, 1, 2000));

  assert_int_equal(s_display_added, 0);   /* unrelated dispatches stay quiet */

  pump_stop();
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_event_dispatch_from_worker_thread),
    cmocka_unit_test(test_mouse_routing_and_hover),
    cmocka_unit_test(test_animator_timer_fires),
    cmocka_unit_test(test_hotload_watcher_and_debounce),
    cmocka_unit_test(test_display_and_power_events),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}