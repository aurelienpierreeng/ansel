#pragma once

#include <glib.h>

struct dt_dev_pixelpipe_t;

typedef void (*dt_gui_throttle_callback_t)(gpointer user_data);

void dt_gui_throttle_init(void);
void dt_gui_throttle_cleanup(void);

void dt_gui_throttle_record_runtime(const struct dt_dev_pixelpipe_t *pipe, gint64 runtime_us);
int dt_gui_throttle_get_runtime_us(void);
guint dt_gui_throttle_get_timeout_ms(void);
gint64 dt_gui_throttle_get_timeout_us(void);

void dt_gui_throttle_queue(gpointer source, dt_gui_throttle_callback_t callback, gpointer user_data);
void dt_gui_throttle_cancel(gpointer source);
