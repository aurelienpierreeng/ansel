#ifndef DT_DEVELOP_GUI_THROTTLE_H
#define DT_DEVELOP_GUI_THROTTLE_H

#include <glib.h>

G_BEGIN_DECLS


/* Which render this timing belongs to. Deliberately NOT the pipeline's own enum: the throttle
 * only ever distinguishes the main view from the preview, and taking develop/pixelpipe.h for
 * that would make a widget depend on the pixel pipeline. The caller maps its pipe to a slot. */
typedef enum dt_throttle_slot_t
{
  DT_THROTTLE_SLOT_MAIN = 0,
  DT_THROTTLE_SLOT_PREVIEW,
  DT_THROTTLE_SLOT_OTHER
} dt_throttle_slot_t;

typedef void (*dt_gui_throttle_callback_t)(gpointer user_data);

/* The throttle adapts to how long redraws actually take, and remembers that across sessions.
 * Both the user's timeout preference and the remembered runtime are configuration, so the
 * host supplies one and collects the other rather than this module reading conf itself. */

/** @param saved_runtime_us the average redraw time persisted from the last session, or 0. */
void dt_gui_throttle_init(int saved_runtime_us);

/** Maximum time a queued task may wait, in ms. Set by the host from its preferences; 0
 *  disables the timeout. */
void dt_gui_throttle_set_timeout_ms(guint timeout_ms);


void dt_gui_throttle_cleanup(void);

void dt_gui_throttle_record_runtime(dt_throttle_slot_t slot, gint64 runtime_us);
int dt_gui_throttle_get_runtime_us(void);
int dt_gui_throttle_get_pipe_runtime_us(dt_throttle_slot_t slot);
guint dt_gui_throttle_get_timeout_ms(void);
gint64 dt_gui_throttle_get_timeout_us(void);

void dt_gui_throttle_queue(gpointer source, dt_gui_throttle_callback_t callback, gpointer user_data);
void dt_gui_throttle_cancel(gpointer source);
G_END_DECLS

#endif // DT_DEVELOP_GUI_THROTTLE_H
