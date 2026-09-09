#include "animation.h"
#include "event.h"

#ifdef _WIN32
#include <windows.h>
#include "win_platform.h"

// Windows (S5): the animator ticks through a 16 ms SetTimer on the hidden
// event window; WM_TIMER routing in win_events.c consults this id and posts
// ANIMATOR_REFRESH stamped with skbar_animator_timestamp() (QPC). The clock
// animator->clock holds the QPC frequency, so animation_update()'s ratio math
// is platform-identical.
static uintptr_t g_animator_timer_id = 0;

uintptr_t skbar_animator_timer_id(void) {
  return g_animator_timer_id;
}

uint64_t skbar_animator_timestamp(void) {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return (uint64_t)counter.QuadPart;
}
#endif

#ifndef _WIN32
static CVReturn animation_frame_callback(CVDisplayLinkRef display_link, const CVTimeStamp* now, const CVTimeStamp* output_time, CVOptionFlags flags, CVOptionFlags* flags_out, void* context) {
  uint64_t hostTime = output_time->hostTime;
  dispatch_async(dispatch_get_main_queue(), ^{
    struct event event = { (void*)hostTime, ANIMATOR_REFRESH };
    event_post(&event);
  });
  return kCVReturnSuccess;
}
#endif

struct animation* animation_create() {
  struct animation* animation = malloc(sizeof(struct animation));
  memset(animation, 0, sizeof(struct animation));

  return animation;
}

static void animation_destroy(struct animation* animation) {
  if (animation) free(animation);
}

static void animation_lock(struct animation* animation) {
  animation->locked = true;
}

void animation_setup(struct animation* animation, void* target, animator_function* update_function, int initial_value, int final_value, uint32_t duration, char interp_function) {
  // The animation duration is represented as a frame count equivalent on a
  // 60Hz display. E.g. 120frames = 2 seconds
  animation->duration = (double)duration / 60.0;
  animation->initial_value = initial_value;
  animation->final_value = final_value;
  animation->update_function = update_function;
  animation->target = target;
  animation->separate_bytes = false;
  animation->as_float = false;

  if (interp_function == INTERP_FUNCTION_TANH) {
    animation->interp_function = &function_tanh;
  } else if (interp_function == INTERP_FUNCTION_SIN) {
    animation->interp_function = &function_sin;
  } else if (interp_function == INTERP_FUNCTION_QUADRATIC) {
    animation->interp_function = &function_square;
  } else if (interp_function == INTERP_FUNCTION_EXP) {
    animation->interp_function = &function_exp;
  } else if (interp_function == INTERP_FUNCTION_CIRC) {
    animation->interp_function = &function_circ;
  } else {
    animation->interp_function = &function_linear;
  }
}

static bool animation_update(struct animation* animation, uint64_t time, uint64_t clock) {
  if (!animation->target
      || !animation->update_function
      || animation->waiting         ) {
    return false;
  }

  if (!animation->initial_time) animation->initial_time = time;
  double t = animation->duration > 0
             ? ((double)(time - animation->initial_time)
               / (double)(animation->duration * clock))
             : 1.0;

  bool final_frame = t >= 1.0;
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;

  double slider = final_frame ? 1.0 : animation->interp_function(t);

  int value;
  if (animation->separate_bytes) {
    for (int i = 0; i < 4; i++) {
      unsigned char byte_i = *((unsigned char*)&animation->initial_value + i);
      unsigned char byte_f = *((unsigned char*)&animation->final_value + i);

      unsigned char byte_val = (1. - slider) * byte_i + slider * byte_f;
      *((unsigned char*)&value + i) = byte_val;
    }
  } else if (animation->as_float) {
    *((float*)&value) = (1. - slider) * *(float*)&animation->initial_value
             + slider * *(float*)&animation->final_value;

  } else {
    value = (1. - slider) * animation->initial_value
            + slider * animation->final_value
            + 0.5;
    if (final_frame) value = animation->final_value;
  }

  bool needs_update;
  if (animation->as_float) {
    needs_update =
      ((bool (*)(void*, float))animation->update_function)(animation->target,
                                                           *((float*)&value) );
  } else {
    needs_update = animation->update_function(animation->target, value);
  }

  bool found_item = false;
  for (int i = 0; i < g_bar_manager.bar_item_count; i++) {
    if (needs_update
        && (animation->target >= (void*)g_bar_manager.bar_items[i])
        && (animation->target < ((void*)g_bar_manager.bar_items[i]
                                 + sizeof(struct bar_item)         ))) {

      bar_item_needs_update(g_bar_manager.bar_items[i]);
      found_item = true;
    }
  }

  if (!found_item && needs_update) g_bar_manager.bar_needs_update = true;

  animation->finished = final_frame;
  if (animation->finished && animation->next) {
    animation->next->previous = NULL;
    animation->next->waiting = false;
    animation->next = NULL;
  }
  return needs_update;
}

void animator_init(struct animator* animator) {
  animator->animations = NULL;
  animator->animation_count = 0;
  animator->interp_function = 0;
  animator->duration = 0;
  animator->display_link = NULL;
#ifdef _WIN32
  animator->timer_id = 0;
#endif

  animator_renew_display_link(animator);
}

void animator_renew_display_link(struct animator* animator) {
#ifdef _WIN32
  // Windows (S5): a 16 ms SetTimer replaces the CVDisplayLink. The timer id is
  // derived from the animator pointer, so multiple animators coexist. The
  // hidden event window must exist first (skbar_win_event_hwnd() != 0); before
  // that we stay idle and animator_add() retries on the next add.
  animator_destroy_display_link(animator);
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  animator->clock = (double)frequency.QuadPart;
  uintptr_t hwnd = skbar_win_event_hwnd();
  if (hwnd != 0) {
    animator->timer_id = (uintptr_t)SetTimer((HWND)hwnd, (UINT_PTR)animator,
                                             16, NULL);
    g_animator_timer_id = animator->timer_id;
  }
#else
  animator_destroy_display_link(animator);
  CVDisplayLinkCreateWithActiveCGDisplays(&animator->display_link);

  CVDisplayLinkSetOutputCallback(animator->display_link,
                                 animation_frame_callback,
                                 animator                 );

  animator->clock = CVGetHostClockFrequency();
  CVDisplayLinkStart(animator->display_link);
#endif
}

void animator_destroy_display_link(struct animator* animator) {
#ifdef _WIN32
  // KillTimer only while the hidden window still exists (teardown order: the
  // pump destroys the window after the animator is stopped).
  uintptr_t hwnd = skbar_win_event_hwnd();
  if (hwnd != 0 && animator->timer_id) {
    KillTimer((HWND)hwnd, (UINT_PTR)animator);
  }
  g_animator_timer_id = 0;
  animator->timer_id = 0;
#else
  if (animator->display_link) {
    CVDisplayLinkStop(animator->display_link);
    CVDisplayLinkRelease(animator->display_link);
    animator->display_link = NULL;
  }
#endif
}

void animator_lock(struct animator* animator) {
  for (int i = 0; i < animator->animation_count; i++) {
     animation_lock(animator->animations[i]);
  }
}

static void animator_calculate_offset_for_animation(struct animator* animator, struct animation* animation) {
  if (animator->animation_count < 1) return;

  struct animation* previous = NULL;
  for (int i = animator->animation_count - 1; i >= 0; i--) {
    struct animation* current = animator->animations[i];
    if (current->target == animation->target
        && current->update_function == animation->update_function) {
      previous = current;
      break;
    }
  }

  if (previous) {
    animation->initial_value = previous->final_value;
    previous->next = animation;
    animation->previous = previous;
    animation->waiting = true;
  }
}

void animator_add(struct animator* animator, struct animation* animation) {
  animator_calculate_offset_for_animation(animator, animation);
  animator->animations = realloc(animator->animations,
                                 sizeof(struct animation*)
                                        * ++animator->animation_count);
  animator->animations[animator->animation_count - 1] = animation;

  #ifdef _WIN32
  if (!animator->timer_id) animator_renew_display_link(animator);
#else
  if (!animator->display_link) animator_renew_display_link(animator);
#endif
}

static void animator_remove(struct animator* animator, struct animation* animation) {
  if (animator->animation_count == 1) {
    free(animator->animations);
    animator->animations = NULL;
    animator->animation_count = 0;
  } else {
    struct animation* tmp[animator->animation_count - 1];
    int count = 0;
    for (int i = 0; i < animator->animation_count; i++) {
      if (animator->animations[i] == animation) continue;
      tmp[count++] = animator->animations[i];
    }
    animator->animation_count--;
    animator->animations = realloc(animator->animations,
                                   sizeof(struct animation*)
                                          *animator->animation_count);

    memcpy(animator->animations,
           tmp,
           sizeof(struct animation*)*animator->animation_count);
  }

  if (animation->previous) animation->previous->next = NULL;
  if (animation->next) animation->next->previous = NULL;

  animation_destroy(animation);
}

void animator_cancel_locked(struct animator* animator, void* target, animator_function* function) {
  struct animation* remove[animator->animation_count];
  memset(remove, 0, animator->animation_count);
  uint32_t remove_count = 0;

  for (int i = 0; i < animator->animation_count; i++) {
    struct animation* animation = animator->animations[i];
    if (animation->locked
        && animation->target == target
        && animation->update_function == function) {
      remove[remove_count++] = animation;
    }
  }

  for (uint32_t i = 0; i < remove_count; i++) {
    animator_remove(animator, remove[i]);
  }
}

bool animator_cancel(struct animator* animator, void* target, animator_function* function) {
  bool needs_update = false;

  struct animation* remove[animator->animation_count];
  memset(remove, 0, animator->animation_count);
  uint32_t remove_count = 0;

  for (int i = 0; i < animator->animation_count; i++) {
    struct animation* animation = animator->animations[i];
    if (animation->target == target
        && animation->update_function == function) {
      needs_update |= function(animation->target, animation->final_value);
      remove[remove_count++] = animation;
    }
  }

  for (uint32_t i = 0; i < remove_count; i++) {
    animator_remove(animator, remove[i]);
  }

  return needs_update;
}

bool animator_update(struct animator* animator, uint64_t time) {
  bool needs_refresh = false;
  struct animation* remove[animator->animation_count];
  memset(remove, 0, animator->animation_count);
  uint32_t remove_count = 0;

  for (uint32_t i = 0; i < animator->animation_count; i++) {
    needs_refresh |= animation_update(animator->animations[i],
                                      time,
                                      animator->clock         );

    if (animator->animations[i]->finished) {
      remove[remove_count++] = animator->animations[i];
    }
  }

  for (uint32_t i = 0; i < remove_count; i++) {
    animator_remove(animator, remove[i]);
  }

  if (animator->animation_count == 0) animator_destroy_display_link(animator);
  return needs_refresh;
}

void animator_destroy(struct animator* animator) {
  if (animator->animation_count > 0) {
#ifdef _WIN32
    animator_destroy_display_link(animator);
#else
    if (animator->display_link)
      CVDisplayLinkStop(animator->display_link);
    CVDisplayLinkRelease(animator->display_link);
    animator->display_link = NULL;
#endif

    for (int i = 0; i < animator->animation_count; i++) {
      animation_destroy(animator->animations[i]);
    }
  }

  if (animator->animations) free(animator->animations);
}
