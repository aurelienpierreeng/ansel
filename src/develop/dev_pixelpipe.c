
#include "control/control.h"
#include "common/debug.h"
#include "common/darktable.h"
#include "common/dtpthread.h"
#include "develop/pixelpipe.h"

static void _change_pipe(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_change_t flag)
{
  if(!pipe) return;
  pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  pipe->changed |= flag;
  dt_atomic_set_int(&pipe->shutdown, TRUE);
}

void dt_dev_pixelpipe_rebuild_all(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_REMOVE);
  _change_pipe(dev->pipe, DT_DEV_PIPE_REMOVE);
}

void dt_dev_pixelpipe_resync_history_main(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_history_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_history_all(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_resync_history_preview(dev);
  dt_dev_pixelpipe_resync_history_main(dev);
}

void dt_dev_pixelpipe_update_history_main_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_history_preview_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_history_all_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_update_preview(dev);
  dt_dev_pixelpipe_update_history_main(dev);
}

void dt_dev_pixelpipe_update_zoom_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_update_zoom_main_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_reset_all(dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&darktable.pipeline_threadsafe);
  dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, -1);
  dt_pthread_mutex_unlock(&darktable.pipeline_threadsafe);

  if(darktable.gui->reset || !dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_rebuild_all(dev);
}

void dt_dev_pixelpipe_refresh_main(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  if(full)
    dt_dev_pixelpipe_resync_history_main(dev);
  else
    dt_dev_pixelpipe_update_history_main(dev);

  dt_dev_process_main(dev);

  dt_control_queue_redraw_center();
}

void dt_dev_pixelpipe_refresh_preview(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  if(full)
    dt_dev_pixelpipe_resync_history_preview(dev);
  else
    dt_dev_pixelpipe_update_preview(dev);

  dt_dev_process_preview(dev);

  // Note: since preview pipe provides data to many GUI overlays
  // on the main image, we need to redraw everything
  dt_control_queue_redraw();
}

void dt_dev_pixelpipe_refresh_all(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  // Always start reprocessing thumbnail first,
  // because it's needed for final GUI sizes,
  // histograms, color pickers, etc. and is used
  // as placeholder pending a main image recompute.
  if(full)
  {
    dt_dev_pixelpipe_resync_history_preview(dev);
    dt_dev_pixelpipe_resync_history_main(dev);
  }
  else
  {
    dt_dev_pixelpipe_update_preview(dev);
    dt_dev_pixelpipe_update_history_main(dev);
  }

  dt_dev_process_all(dev);

  dt_control_queue_redraw();
}

void dt_dev_pixelpipe_change_zoom_main(dt_develop_t *dev)
{
  if (!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_update_zoom_main(dev);
  dt_dev_process_main(dev);
  dt_control_queue_redraw();
}