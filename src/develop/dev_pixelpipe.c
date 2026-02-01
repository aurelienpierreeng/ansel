

#include "common/debug.h"
#include "common/darktable.h"
#include "common/dtpthread.h"
//#include "develop/develop.h"
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

void dt_dev_pixelpipe_resync_main(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_all(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_resync_preview(dev);
  dt_dev_pixelpipe_resync_main(dev);
}

void dt_dev_pixelpipe_update_main_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_preview_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_all_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_update_preview(dev);
  dt_dev_pixelpipe_update_main(dev);
}

void dt_dev_pixelpipe_change_zoom_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_change_zoom_main_real(dt_develop_t *dev)
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
    dt_dev_pixelpipe_resync_main(dev);
  else
    dt_dev_pixelpipe_update_main(dev);

  dt_dev_process_image(dev);
}

void dt_dev_pixel_pipe_refresh_preview(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  if(full)
    dt_dev_pixelpipe_resync_preview(dev);
  else
    dt_dev_pixelpipe_update_preview(dev);

  dt_dev_process_preview(dev);
}