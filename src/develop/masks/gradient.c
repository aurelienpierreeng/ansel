/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/undo.h"
#include "control/conf.h"

#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"

static void _gradient_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                   int num_points, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  (void)num_points; // unused arg, keep compiler from complaining
  if(!gui) return;

  *inside = *inside_border = *inside_source = 0;
  *near = -1;
  *dist = FLT_MAX;

  const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  const float as2 = as * as;

  float close_to_controls = FALSE;

  // compute distances with the three control points
  for(int k = 0; k<3; k++)
  {
    const float dx = x - gpt->points[k * 2];
    const float dy = y - gpt->points[k * 2 + 1];
    const float dd = sqf(dx) + sqf(dy);
    *dist = fminf(*dist, dd);

    close_to_controls = close_to_controls || (dd < as2);
  }

  // check if we are close to pivot or anchor
  if(close_to_controls)
  {
    *inside = 1;
    return;
  }

  // check if we are close to borders
  for(int i = 0; i < gpt->border_count; i++)
  {
    if((x - gpt->border[i * 2]) * (x - gpt->border[i * 2])
       + (y - gpt->border[i * 2 + 1]) * (y - gpt->border[i * 2 + 1]) < as2)
    {
      *inside_border = 1;
      return;
    }
  }

  // check if we are close to main line
  for(int i = 3; i < gpt->points_count; i++)
  {
    if((x - gpt->points[i * 2]) * (x - gpt->points[i * 2])
       + (y - gpt->points[i * 2 + 1]) * (y - gpt->points[i * 2 + 1]) < as2)
    {
      *inside = 1;
      return;
    }
  }
}


static int _gradient_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                           uint32_t state, dt_masks_form_t *form, int parentid,
                                           dt_masks_form_gui_t *gui, int index,
                                           dt_masks_interaction_t interaction)
{
  if(gui->creation)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      float compression = MIN(1.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/compression"));
      if(up)
        compression = fminf(fmaxf(compression, 0.001f) * 1.0f / 0.8f, 1.0f);
      else
        compression = fmaxf(compression, 0.001f) * 0.8f;
      dt_conf_set_float("plugins/darkroom/masks/gradient/compression", compression);
      dt_toast_log(_("compression: %3.2f%%"), compression*100.0f);
    }
    else if (dt_modifier_is(state, 0)) // simple scroll to adjust curvature, calling func adjusts opacity with Ctrl
    {
      float curvature = dt_conf_get_float("plugins/darkroom/masks/gradient/curvature");
      if(up)
        curvature = fminf(curvature + 0.01f, 2.0f);
      else
        curvature = fmaxf(curvature - 0.01f, -2.0f);
      dt_conf_set_float("plugins/darkroom/masks/gradient/curvature", curvature);
      dt_toast_log(_("curvature: %3.2f%%"), curvature * 50.0f);
    }
    return 1;
  }

  if(gui->form_selected)
  {
    // we register the current position
    if(gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up);
    }
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);
      if(up)
        gradient->compression = fminf(fmaxf(gradient->compression, 0.001f) * 1.0f / 0.8f, 1.0f);
      else
        gradient->compression = fmaxf(gradient->compression, 0.001f) * 0.8f;

      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index, module);
      dt_conf_set_float("plugins/darkroom/masks/gradient/compression", gradient->compression);
      dt_toast_log(_("compression: %3.2f%%"), gradient->compression*100.0f);

    }
    else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);
      if(up)
        gradient->curvature = fminf(gradient->curvature + 0.01f, 2.0f);
      else
        gradient->curvature = fmaxf(gradient->curvature - 0.01f, -2.0f);
      dt_toast_log(_("curvature: %3.2f%%"), gradient->curvature*50.0f);

      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index, module);

    }
    return 1;
  }
  return 0;
}

static int _gradient_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                           double pressure, int which, int type, uint32_t state,
                                           dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  if(which == 1 && type == GDK_2BUTTON_PRESS)
  {
    // double-click resets curvature
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

    gradient->curvature = 0.0f;


    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);



    return 1;
  }
  else if(!gui->creation && dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    gui->gradient_toggling = TRUE;

    return 1;
  }
  else if(!gui->creation && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    // we start the form rotating or dragging
    if(gui->pivot_selected)
      gui->form_rotating = TRUE;
    else
      gui->form_dragging = TRUE;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
    return 1;
  }
  else if(gui->creation && (which == 3))
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);

    return 1;
  }
  else if(gui->creation)
  {
    gui->posx_source = gui->posx;
    gui->posy_source = gui->posy;
    gui->form_dragging = TRUE;
  }
  return 0;
}

static void _gradient_init_values(float zoom_scale, dt_masks_form_gui_t *gui, float xpos, float ypos, float pzx,
                                  float pzy, float *anchorx, float *anchory, float *rotation, float *compression,
                                  float *curvature)
{
  const float diff = 3.0f * zoom_scale / 2.0;
  float x0 = 0.0f, y0 = 0.0f;
  float dx = 0.0f, dy = 0.0f;

  if(!gui->form_dragging
     || (gui->posx_source - xpos > -diff && gui->posx_source - xpos < diff && gui->posy_source - ypos > -diff
         && gui->posy_source - ypos < diff))
  {
    x0 = pzx;
    y0 = pzy;
    // rotation not updated and not yet dragged, in this case let's
    // pretend that we are using a neutral dx, dy (where the rotation will
    // still be unchanged). We do that as we don't know the actual rotation
    // because those points must go through the backtransform.
    dx = x0 + 100.0f;
    dy = y0;
  }
  else
  {
    x0 = gui->posx_source;
    y0 = gui->posy_source;
    dx = pzx;
    dy = pzy;
  }

  // we change the offset value
  float pts[8] = { x0, y0, dx, dy, x0 + 10.0f, y0, x0, y0 + 10.0f };
  dt_dev_distort_backtransform(darktable.develop, pts, 4);
  *anchorx = pts[0] / darktable.develop->preview_pipe->iwidth;
  *anchory = pts[1] / darktable.develop->preview_pipe->iheight;

  float rot = atan2f(pts[3] - pts[1], pts[2] - pts[0]);
  // If the transform has flipped the image about one axis, then the
  // 'handedness' of the coordinate system is changed. In this case the
  // rotation angle must be offset by 180 degrees so that the gradient points
  // in the correct direction as dragged. We test for this by checking the
  // angle between two vectors that should be 90 degrees apart. If the angle
  // is -90 degrees, then the image is flipped.
  float check_angle = atan2f(pts[7] - pts[1], pts[6] - pts[0]) - atan2f(pts[5] - pts[1], pts[4] - pts[0]);
  // Normalize to the range -180 to 180 degrees
  check_angle = atan2f(sinf(check_angle), cosf(check_angle));
  if(check_angle < 0.0f) rot -= M_PI;

  const float compr = MIN(1.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/compression"));

  *rotation = -rot / M_PI * 180.0f;
  *compression = MAX(0.0f, compr);
  *curvature = MAX(-2.0f, MIN(2.0f, dt_conf_get_float("plugins/darkroom/masks/gradient/curvature")));
}

static int _gradient_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                            uint32_t state, dt_masks_form_t *form, int parentid,
                                            dt_masks_form_gui_t *gui, int index)
{
  if(which == 3 && parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we hide the form
    if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
      dt_masks_change_form_gui(NULL);
    else if(g_list_shorter_than(darktable.develop->form_visible->points, 2))
      dt_masks_change_form_gui(NULL);
    else
    {
      dt_masks_clear_form_gui(darktable.develop);
      for(GList *forms = darktable.develop->form_visible->points; forms; forms = g_list_next(forms))
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if(gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, gpt);
          free(gpt);
          break;
        }
      }
      gui->edit_mode = DT_MASKS_EDIT_FULL;
    }

    // we remove the shape
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    return 1;
  }

  if(gui->form_dragging && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

    // we end the form dragging
    gui->form_dragging = FALSE;

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    gradient->anchor[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    gradient->anchor[1] = pts[1] / darktable.develop->preview_pipe->iheight;


    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the move


    return 1;
  }
  else if(gui->form_rotating && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

    // we end the form rotating
    gui->form_rotating = FALSE;

    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    const float x = pzx * wd;
    const float y = pzy * ht;

    // we need the reference point
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    const float xref = gpt->points[0];
    const float yref = gpt->points[1];

    float pts[8] = { xref, yref, x , y, 0, 0, gui->dx, gui->dy };

    const float dv = atan2f(pts[3] - pts[1], pts[2] - pts[0]) - atan2f(-(pts[7] - pts[5]), -(pts[6] - pts[4]));

    float pts2[8] = { xref, yref, x , y, xref+10.0f, yref, xref, yref+10.0f };

    dt_dev_distort_backtransform(darktable.develop, pts2, 4);

    float check_angle = atan2f(pts2[7] - pts2[1], pts2[6] - pts2[0]) - atan2f(pts2[5] - pts2[1], pts2[4] - pts2[0]);
    // Normalize to the range -180 to 180 degrees
    check_angle = atan2f(sinf(check_angle), cosf(check_angle));
    if (check_angle < 0)
      gradient->rotation += dv / M_PI * 180.0f;
    else
      gradient->rotation -= dv / M_PI * 180.0f;



    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the rotation


    return 1;
  }
  else if(gui->gradient_toggling)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

    // we end the gradient toggling
    gui->gradient_toggling = FALSE;

    // toggle transition type of gradient
    if(gradient->state == DT_MASKS_GRADIENT_STATE_LINEAR)
      gradient->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    else
      gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;



    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the new parameters


    return 1;
  }
  else if(gui->creation)
  {
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;

    // get the rotation angle only if we are not too close from starting point
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1 << closeup, 1);

    dt_iop_module_t *crea_module = gui->creation_module;
    // we create the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(malloc(sizeof(dt_masks_point_gradient_t)));

    _gradient_init_values(zoom_scale, gui, gui->posx, gui->posy, pzx * wd, pzy * ht, &gradient->anchor[0],
                          &gradient->anchor[1], &gradient->rotation, &gradient->compression, &gradient->curvature);

    gui->form_dragging = FALSE;

    gradient->steepness = 0.0f;
    gradient->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    // not used for masks
    form->source[0] = form->source[1] = 0.0f;

    form->points = g_list_append(form->points, gradient);
    dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

    if(crea_module)
    {
      // we save the move

      // and we switch in edit mode to show all the forms
      dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(crea_module);
      dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid, TRUE);
      gui->creation_module = NULL;
    }
    else
    {
      // we select the new form
      dt_dev_masks_selection_change(darktable.develop, NULL, form->formid, TRUE);
    }
    return 1;
  }

  return 0;
}

static int _gradient_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy,
                                        double pressure, int which, dt_masks_form_t *form, int parentid,
                                        dt_masks_form_gui_t *gui, int index)
{
  if(gui->creation && gui->form_dragging)
  {

    return 1;
  }
  else if(gui->form_dragging)
  {
    // we get the gradient
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

    // we change the center value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    gradient->anchor[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    gradient->anchor[1] = pts[1] / darktable.develop->preview_pipe->iheight;

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  if(gui->form_rotating)
  {
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    const float x = pzx * wd;
    const float y = pzy * ht;

    // we need the reference point
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;
    const float xref = gpt->points[0];
    const float yref = gpt->points[1];

    float pts[8] = { xref, yref, x, y, 0, 0, gui->dx, gui->dy };

    // we remap dx, dy to the right values, as it will be used in next movements
    gui->dx = xref - gui->posx;
    gui->dy = yref - gui->posy;

    const float dv = atan2f(pts[3] - pts[1], pts[2] - pts[0]) - atan2f(-(pts[7] - pts[5]), -(pts[6] - pts[4]));

    float pts2[8] = { xref, yref, x, y, xref + 10.0f, yref, xref, yref + 10.0f };
    dt_dev_distort_backtransform(darktable.develop, pts2, 4);

    float check_angle = atan2f(pts2[7] - pts2[1], pts2[6] - pts2[0]) - atan2f(pts2[5] - pts2[1], pts2[4] - pts2[0]);
    // Normalize to the range -180 to 180 degrees
    check_angle = atan2f(sinf(check_angle), cosf(check_angle));
    if(check_angle < 0.0f)
      gradient->rotation += dv / M_PI * 180.0f;
    else
      gradient->rotation -= dv / M_PI * 180.0f;

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  else if(!gui->creation)
  {
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
    const float as = DT_PIXEL_APPLY_DPI(20) / zoom_scale;  // transformed to backbuf dimensions
    const float x = pzx * darktable.develop->preview_pipe->backbuf_width;
    const float y = pzy * darktable.develop->preview_pipe->backbuf_height;
    int in, inb, near, ins;
    float dist;
    _gradient_get_distance(x, y, as, gui, index, 0, &in, &inb, &near, &ins, &dist);

    const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

    if(gpt
       && (x - gpt->points[2]) * (x - gpt->points[2]) + (y - gpt->points[3]) * (y - gpt->points[3]) < as)
    {
      gui->pivot_selected = TRUE;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(gpt
            && (x - gpt->points[4]) * (x - gpt->points[4]) + (y - gpt->points[5]) * (y - gpt->points[5])
               < as)
    {
      gui->pivot_selected = TRUE;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(in)
    {
      gui->pivot_selected = FALSE;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if(inb)
    {
      gui->pivot_selected = FALSE;
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
    }
    else
    {
      gui->pivot_selected = FALSE;
      gui->form_selected = FALSE;
      gui->border_selected = FALSE;
    }


    if(!gui->form_selected && !gui->border_selected) return 0;
    if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
    return 1;
  }
  // add a preview when creating a gradient
  else if(gui->creation)
  {

    return 1;
  }

  return 0;
}

// check if (x,y) lies within reasonable limits relative to image frame
static inline int _gradient_is_canonical(const float x, const float y, const float wd, const float ht)
{
  return (isnormal(x) && isnormal(y) && x >= -wd && x <= 2 * wd && y >= -ht && y <= 2 * ht) ? TRUE : FALSE;
}

static int _gradient_get_points(dt_develop_t *dev, float x, float y, float rotation, float curvature,
                                float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);
  const float distance = 0.1f * fminf(wd, ht);

  const float v = (-rotation / 180.0f) * M_PI;
  const float cosv = cosf(v);
  const float sinv = sinf(v);

  const int count = sqrtf(wd * wd + ht * ht) + 3;
  *points = dt_alloc_align_float((size_t)2 * count);
  if(*points == NULL) return 0;

  // we set the anchor point
  (*points)[0] = x * wd;
  (*points)[1] = y * ht;

  // we set the pivot points
  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;
  const float x1 = x * wd + distance * cosf(v1);
  const float y1 = y * ht + distance * sinf(v1);
  (*points)[2] = x1;
  (*points)[3] = y1;
  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;
  const float x2 = x * wd + distance * cosf(v2);
  const float y2 = y * ht + distance * sinf(v2);
  (*points)[4] = x2;
  (*points)[5] = y2;

  const int nthreads = omp_get_max_threads();
  size_t c_padded_size;
  uint32_t *pts_count = dt_calloc_perthread(nthreads, sizeof(uint32_t), &c_padded_size);
  float *const restrict pts = dt_alloc_align_float((size_t)2 * count * nthreads);

  // we set the line point
  const float xstart = fabsf(curvature) > 1.0f ? -sqrtf(1.0f / fabsf(curvature)) : -1.0f;
  const float xdelta = -2.0f * xstart / (count - 3);

//  gboolean in_frame = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                       \
    dt_omp_firstprivate(nthreads, pts, pts_count, count, cosv, sinv, xstart, xdelta, curvature, scale, x, y, wd,  \
                        ht, c_padded_size, points) schedule(static) if(count > 100)
#endif
  for(int i = 3; i < count; i++)
  {
    const float xi = xstart + (i - 3) * xdelta;
    const float yi = curvature * xi * xi;
    const float xii = (cosv * xi + sinv * yi) * scale;
    const float yii = (sinv * xi - cosv * yi) * scale;
    const float xiii = xii + x * wd;
    const float yiii = yii + y * ht;

    // don't generate guide points if they extend too far beyond the image frame;
    // this is to avoid that modules like lens correction fail on out of range coordinates
    if(!(xiii < -wd || xiii > 2 * wd || yiii < -ht || yiii > 2 * ht))
    {
      const int thread = omp_get_thread_num();
      uint32_t *tcount = dt_get_perthread(pts_count, c_padded_size);
      pts[(thread * count) + *tcount * 2]     = xiii;
      pts[(thread * count) + *tcount * 2 + 1] = yiii;
      (*tcount)++;
    }
  }

  *points_count = 3;
  for(int thread = 0; thread < nthreads; thread++)
  {
    const uint32_t tcount = *(uint32_t *)dt_get_bythread(pts_count, c_padded_size, thread);
    for(int k = 0; k < tcount; k++)
    {
      (*points)[(*points_count) * 2]     = pts[(thread * count) + k * 2];
      (*points)[(*points_count) * 2 + 1] = pts[(thread * count) + k * 2 + 1];
      (*points_count)++;
    }
  }

  dt_free_align(pts_count);
  dt_free_align(pts);

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, *points_count)) return 1;

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int _gradient_get_pts_border(dt_develop_t *dev, float x, float y, float rotation, float distance,
                                    float curvature, float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  float *points1 = NULL, *points2 = NULL;
  int points_count1 = 0, points_count2 = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);

  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;

  const float x1 = (x * wd + distance * scale * cosf(v1)) / wd;
  const float y1 = (y * ht + distance * scale * sinf(v1)) / ht;

  const int r1 = _gradient_get_points(dev, x1, y1, rotation, curvature, &points1, &points_count1);

  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;

  const float x2 = (x * wd + distance * scale * cosf(v2)) / wd;
  const float y2 = (y * ht + distance * scale * sinf(v2)) / ht;

  const int r2 = _gradient_get_points(dev, x2, y2, rotation, curvature, &points2, &points_count2);

  int res = 0;

  if(r1 && r2 && points_count1 > 4 && points_count2 > 4)
  {
    int k = 0;
    *points = dt_alloc_align_float((size_t)2 * ((points_count1 - 3) + (points_count2 - 3) + 1));
    if(*points == NULL) goto end;
    *points_count = (points_count1 - 3) + (points_count2 - 3) + 1;
    for(int i = 3; i < points_count1; i++)
    {
      (*points)[k * 2] = points1[i * 2];
      (*points)[k * 2 + 1] = points1[i * 2 + 1];
      k++;
    }
    (*points)[k * 2] = (*points)[k * 2 + 1] = INFINITY;
    k++;
    for(int i = 3; i < points_count2; i++)
    {
      (*points)[k * 2] = points2[i * 2];
      (*points)[k * 2 + 1] = points2[i * 2 + 1];
      k++;
    }
    res = 1;
    goto end;
  }
  else if(r1 && points_count1 > 4)
  {
    int k = 0;
    *points = dt_alloc_align_float((size_t)2 * ((points_count1 - 3)));
    if(*points == NULL) goto end;
    *points_count = points_count1 - 3;
    for(int i = 3; i < points_count1; i++)
    {
      (*points)[k * 2] = points1[i * 2];
      (*points)[k * 2 + 1] = points1[i * 2 + 1];
      k++;
    }
    res = 1;
    goto end;
  }
  else if(r2 && points_count2 > 4)
  {
    int k = 0;
    *points = dt_alloc_align_float((size_t)2 * ((points_count2 - 3)));
    if(*points == NULL) goto end;
    *points_count = points_count2 - 3;

    for(int i = 3; i < points_count2; i++)
    {
      (*points)[k * 2] = points2[i * 2];
      (*points)[k * 2 + 1] = points2[i * 2 + 1];
      k++;
    }
    res = 1;
    goto end;
  }

end:
  dt_free_align(points1);
  dt_free_align(points2);

  return res;
}

static void _gradient_draw_lines(gboolean borders, cairo_t *cr, double *dashed, const float len,
                                 const gboolean selected, const float zoom_scale, float *pts_line,
                                 int pts_line_count, const float xref, const float yref)
{
  // safeguard in case of malformed arrays of points
  if(borders && pts_line_count <= 3) return;
  if(!borders && pts_line_count <= 4) return;

  const float *points = (borders) ? pts_line : pts_line + 6;
  const int points_count = (borders) ? pts_line_count : pts_line_count - 3;
  const float wd = darktable.develop->preview_pipe->iwidth;
  const float ht = darktable.develop->preview_pipe->iheight;

  int count = 0;
  float x = 0.0f, y = 0.0f;

  while(count < points_count)
  {
    if(!isnormal(points[count * 2]))
    {
      count++;
      continue;
    }

    x = points[count * 2];
    y = points[count * 2 + 1];

    if(!_gradient_is_canonical(x, y, wd, ht))
    {
      count++;
      continue;
    }

    if(borders)
      cairo_set_dash(cr, dashed, len, 0);
    else
      cairo_set_dash(cr, dashed, 0, 0);
    if(selected)
    {
      if(borders)
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 5.0 / zoom_scale);
    }
    else
    {
      if(borders)
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 3.0 / zoom_scale);
    }
    dt_draw_set_color_overlay(cr, FALSE, 0.8);

    cairo_move_to(cr, x, y);

    count++;
    for(; count < points_count && isnormal(points[count * 2]); count++)
    {
      if(!_gradient_is_canonical(points[count * 2], points[count * 2 + 1], wd, ht)) break;

      cairo_line_to(cr, points[count * 2], points[count * 2 + 1]);
    }
    cairo_stroke_preserve(cr);
    if(selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_stroke(cr);
  }
}

static void _gradient_draw_arrow(cairo_t *cr, double *dashed, const float len, const gboolean selected,
                                 const gboolean border_selected, const float zoom_scale, float *pts, int pts_count)
{
  if(pts_count < 3) return;

  const float anchor_x = pts[0];
  const float anchor_y = pts[1];
  const float pivot_end_x = pts[2];
  const float pivot_end_y = pts[3];
  const float pivot_start_x = pts[4];
  const float pivot_start_y = pts[5];

  // draw anchor point
  {
    cairo_set_dash(cr, dashed, 0, 0);
    const float anchor_size = (selected) ? 7.0f / zoom_scale : 5.0f / zoom_scale;
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_rectangle(cr, anchor_x - (anchor_size * 0.5), anchor_y - (anchor_size * 0.5), anchor_size, anchor_size);
    cairo_fill_preserve(cr);

    if(selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_stroke(cr);
  }


  // draw pivot points
  {
    cairo_set_dash(cr, dashed, 0, 0);
    if(border_selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);

    // from start to end
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_move_to(cr, pivot_start_x, pivot_start_y);
    cairo_line_to(cr, pivot_end_x, pivot_end_y);
    cairo_stroke(cr);

    // start side of the gradient
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_arc(cr, pivot_start_x, pivot_start_y, 3.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);

    // end side of the gradient
    cairo_arc(cr, pivot_end_x, pivot_end_y, 1.0f / zoom_scale, 0, 2.0f * M_PI);
    cairo_fill_preserve(cr);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_stroke(cr);

    // draw arrow on the end of the gradient to clearly display the direction

    // size & width of the arrow
    const float arrow_angle = 0.25f;
    const float arrow_length = 15.0f / zoom_scale;

    const float a_dx = anchor_x - pivot_end_x;
    const float a_dy = pivot_end_y - anchor_y;
    const float angle = atan2f(a_dx, a_dy) - M_PI / 2.0f;

    const float arrow_x1 = pivot_end_x + (arrow_length * cosf(angle + arrow_angle));
    const float arrow_x2 = pivot_end_x + (arrow_length * cosf(angle - arrow_angle));
    const float arrow_y1 = pivot_end_y + (arrow_length * sinf(angle + arrow_angle));
    const float arrow_y2 = pivot_end_y + (arrow_length * sinf(angle - arrow_angle));

    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_move_to(cr, pivot_end_x, pivot_end_y);
    cairo_line_to(cr, arrow_x1, arrow_y1);
    cairo_line_to(cr, arrow_x2, arrow_y2);
    cairo_line_to(cr, pivot_end_x, pivot_end_y);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);
  }
}

static void _gradient_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int nb)
{
  (void)nb; // unused arg, keep compiler from complaining
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  const int len = sizeof(dashed) / sizeof(dashed[0]);

  // preview gradient creation
  if(gui->creation)
  {
    const float zoom_x = dt_control_get_dev_zoom_x();
    const float zoom_y = dt_control_get_dev_zoom_y();

    float xpos = 0.0f, ypos = 0.0f;
    if((gui->posx == -1.0f && gui->posy == -1.0f) || gui->mouse_leaved_center)
    {
      xpos = (.5f + zoom_x) * darktable.develop->preview_pipe->backbuf_width;
      ypos = (.5f + zoom_y) * darktable.develop->preview_pipe->backbuf_height;
    }
    else
    {
      xpos = gui->posx;
      ypos = gui->posy;
    }

    float xx = 0.0f, yy = 0.0f, rotation = 0.0f, compression = 0.0f, curvature = 0.0f;
    _gradient_init_values(zoom_scale, gui, xpos, ypos, xpos, ypos, &xx, &yy, &rotation, &compression, &curvature);

    float *points = NULL;
    int points_count = 0;
    float *border = NULL;
    int border_count = 0;
    int draw = _gradient_get_points(darktable.develop, xx, yy, rotation, curvature, &points, &points_count);
    if(draw && compression > 0.0)
    {
      draw = _gradient_get_pts_border(darktable.develop, xx, yy, rotation, compression, curvature, &border,
                                      &border_count);
    }

    cairo_save(cr);
    // draw main line
    _gradient_draw_lines(FALSE, cr, dashed, len, FALSE, zoom_scale, points, points_count, points[0], points[1]);
    // draw borders
    _gradient_draw_lines(TRUE, cr, dashed, len, FALSE, zoom_scale, border, border_count, points[0], points[1]);
    // draw arrow
    _gradient_draw_arrow(cr, dashed, len, FALSE, FALSE, zoom_scale, points, points_count);
    cairo_restore(cr);

    if(points) dt_free_align(points);
    if(border) dt_free_align(border);
    return;
  }
  const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;
  const float xref = gpt->points[0];
  const float yref = gpt->points[1];

  const gboolean selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging);
  // draw main line
  _gradient_draw_lines(FALSE, cr, dashed, len, selected, zoom_scale, gpt->points, gpt->points_count, xref, yref);
  // draw borders
  if(gui->group_selected == index)
    _gradient_draw_lines(TRUE, cr, dashed, len, gui->border_selected, zoom_scale, gpt->border, gpt->border_count,
                         xref, yref);

  _gradient_draw_arrow(cr, dashed, len, selected, ((gui->group_selected == index) && (gui->border_selected)),
                       zoom_scale, gpt->points, gpt->points_count);
}

static int _gradient_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                                       float **border, int *border_count, int source,
                                       const dt_iop_module_t *module)
{
  (void)source;  // unused arg, keep compiler from complaining
  dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)form->points->data;
  if(_gradient_get_points(dev, gradient->anchor[0], gradient->anchor[1], gradient->rotation, gradient->curvature,
                          points, points_count))
  {
    if(border)
      return _gradient_get_pts_border(dev, gradient->anchor[0], gradient->anchor[1],
                                      gradient->rotation, gradient->compression, gradient->curvature,
                                      border, border_count);
    else
      return 1;
  }
  return 0;
}

static int _gradient_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                              dt_masks_form_t *const form,
                              int *width, int *height, int *posx, int *posy)
{
  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  float points[8] = { 0.0f, 0.0f, wd, 0.0f, wd, ht, 0.0f, ht };

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, 4)) return 0;

  // now we search min and max
  float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 0; i < 4; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }

  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 1;
}

// caller needs to make sure that input remains within bounds
static inline float dt_gradient_lookup(const float *lut, const float i)
{
  const int bin0 = i;
  const int bin1 = i + 1;
  const float f = i - bin0;
  return lut[bin1] * f + lut[bin0] * (1.0f - f);
}

static int _gradient_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                              dt_masks_form_t *const form,
                              float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();
  // we get the area
  if(!_gradient_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient area took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the gradient values
  dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)((form->points)->data);

  // we create a buffer of grid points for later interpolation. mainly in order to reduce memory footprint
  const int w = *width;
  const int h = *height;
  const int px = *posx;
  const int py = *posy;
  const int grid = 8;
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_alloc_align_float((size_t)2 * gw * gh);
  if(points == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(grid, gh, gw, px, py) \
  shared(points) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {
      points[(j * gw + i) * 2] = (grid * i + px);
      points[(j * gw + i) * 2 + 1] = (grid * j + py);
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, (size_t)gw * gh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask at grid points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float ihwscale = 1.0f / hwscale;
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float xoffset = cosv * gradient->anchor[0] * wd + sinv * gradient->anchor[1] * ht;
  const float yoffset = sinv * gradient->anchor[0] * wd - cosv * gradient->anchor[1] * ht;
  const float compression = fmaxf(gradient->compression, 0.001f);
  const float normf = 1.0f / compression;
  const float curvature = gradient->curvature;
  const dt_masks_gradient_states_t state = gradient->state;

  const int lutmax = ceilf(4 * compression * ihwscale);
  const int lutsize = 2 * lutmax + 2;
  float *lut = dt_alloc_align_float((size_t)lutsize);
  if(lut == NULL)
  {
    dt_free_align(points);
    return 0;
  }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(lutsize, lutmax, hwscale, state, normf, compression) \
  shared(lut) schedule(static)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < lutsize; n++)
  {
    const float distance = (n - lutmax) * hwscale;
    const float value = 0.5f + 0.5f * ((state == DT_MASKS_GRADIENT_STATE_LINEAR) ? normf * distance: erff(distance / compression));
    lut[n] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
  }

  // center lut around zero
  float *clut = lut + lutmax;


#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gh, gw, sinv, cosv, xoffset, yoffset, hwscale, ihwscale, curvature, compression) \
  shared(points, clut) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
  {
    for(int i = 0; i < gw; i++)
    {
      const float x = points[(j * gw + i) * 2];
      const float y = points[(j * gw + i) * 2 + 1];

      const float x0 = (cosv * x + sinv * y - xoffset) * hwscale;
      const float y0 = (sinv * x - cosv * y - yoffset) * hwscale;

      const float distance = y0 - curvature * x0 * x0;

      points[(j * gw + i) * 2] = (distance <= -4.0f * compression) ? 0.0f :
                                    ((distance >= 4.0f * compression) ? 1.0f : dt_gradient_lookup(clut, distance * ihwscale));
    }
  }

  dt_free_align(lut);

  // we allocate the buffer
  float *const bufptr = *buffer = dt_alloc_align_float((size_t)w * h);
  if(*buffer == NULL)
  {
    dt_free_align(points);
    return 0;
  }

// we fill the mask buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w, gw, grid, bufptr) \
  shared(points) schedule(simd:static)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    const int grid_jj = grid - jj;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      const int grid_ii = grid - ii;
      const size_t pt_index = mj * gw + mi;
      bufptr[j * w + i] = (points[2 * pt_index] * grid_ii * grid_jj
                           + points[2 * (pt_index + 1)] * ii * grid_jj
                           + points[2 * (pt_index + gw)] * grid_ii * jj
                           + points[2 * (pt_index + gw + 1)] * ii * jj) / (grid * grid);
    }
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  return 1;
}


static int _gradient_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                                  dt_masks_form_t *const form, const dt_iop_roi_t *roi, float *buffer)
{
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();
  // we get the gradient values
  const dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(form->points->data);

  // we create a buffer of grid points for later interpolation. mainly in order to reduce memory footprint
  const int w = roi->width;
  const int h = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int grid = CLAMP((10.0f*roi->scale + 2.0f) / 3.0f, 1, 4);
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_alloc_align_float((size_t)2 * gw * gh);
  if(points == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(iscale, gh, gw, py, px, grid) \
  shared(points) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {

      const size_t index = (size_t)j * gw + i;
      points[index * 2] = (grid * i + px) * iscale;
      points[index * 2 + 1] = (grid * j + py) * iscale;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)gw * gh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask at grid points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float ihwscale = 1.0f / hwscale;
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float xoffset = cosv * gradient->anchor[0] * wd + sinv * gradient->anchor[1] * ht;
  const float yoffset = sinv * gradient->anchor[0] * wd - cosv * gradient->anchor[1] * ht;
  const float compression = fmaxf(gradient->compression, 0.001f);
  const float normf = 1.0f / compression;
  const float curvature = gradient->curvature;
  const dt_masks_gradient_states_t state = gradient->state;

  const int lutmax = ceilf(4 * compression * ihwscale);
  const int lutsize = 2 * lutmax + 2;
  float *lut = dt_alloc_align_float((size_t)lutsize);
  if(lut == NULL)
  {
    dt_free_align(points);
    return 0;
  }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(lutsize, lutmax, hwscale, state, normf, compression) \
  shared(lut) schedule(static)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < lutsize; n++)
  {
    const float distance = (n - lutmax) * hwscale;
    const float value = 0.5f + 0.5f * ((state == DT_MASKS_GRADIENT_STATE_LINEAR) ? normf * distance: erff(distance / compression));
    lut[n] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
  }

  // center lut around zero
  float *clut = lut + lutmax;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gh, gw, sinv, cosv, xoffset, yoffset, hwscale, ihwscale, curvature, compression) \
  shared(points, clut) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
  {
    for(int i = 0; i < gw; i++)
    {
      const size_t index = (size_t)j * gw + i;
      const float x = points[index * 2];
      const float y = points[index * 2 + 1];

      const float x0 = (cosv * x + sinv * y - xoffset) * hwscale;
      const float y0 = (sinv * x - cosv * y - yoffset) * hwscale;

      const float distance = y0 - curvature * x0 * x0;

      points[index * 2] = (distance <= -4.0f * compression) ? 0.0f : ((distance >= 4.0f * compression) ? 1.0f : dt_gradient_lookup(clut, distance * ihwscale));
    }
  }

  dt_free_align(lut);

// we fill the mask buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w, grid, gw) \
  shared(buffer, points) schedule(simd:static)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    const int grid_jj = grid - jj;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      const int grid_ii = grid - ii;
      const size_t mindex = (size_t)mj * gw + mi;
      buffer[(size_t)j * w + i]
          = (points[mindex * 2] * grid_ii * grid_jj
             + points[(mindex + 1) * 2] * ii * grid_jj
             + points[(mindex + gw) * 2] * grid_ii * jj
             + points[(mindex + gw + 1) * 2] * ii * jj)
            / (grid * grid);
    }
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  return 1;
}

static void _gradient_sanitize_config(dt_masks_type_t type)
{
  // we always want to start with no curvature
  dt_conf_set_float("plugins/darkroom/masks/gradient/curvature", 0.0f);
}

static void _gradient_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("gradient #%d"), (int)nb);
}

static void _gradient_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  if(gui->creation)
    g_snprintf(msgbuf, msgbuf_len,
               _("<b>curvature</b>: scroll, <b>compression</b>: shift+scroll\n"
                 "<b>rotation</b>: click+drag, <b>opacity</b>: ctrl+scroll (%d%%)"),
               opacity);
  else if(gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len, _("<b>curvature</b>: scroll, <b>compression</b>: shift+scroll\n"
                                     "<b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
  else if(gui->pivot_selected)
    g_strlcat(msgbuf, _("<b>rotate</b>: drag"), msgbuf_len);
}

static void _gradient_duplicate_points(dt_develop_t *dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_gradient_t *pt = (dt_masks_point_gradient_t *)pts->data;
    dt_masks_point_gradient_t *npt = (dt_masks_point_gradient_t *)malloc(sizeof(dt_masks_point_gradient_t));
    memcpy(npt, pt, sizeof(dt_masks_point_gradient_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

// The function table for gradients.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_gradient = {
  .point_struct_size = sizeof(struct dt_masks_point_gradient_t),
  .sanitize_config = _gradient_sanitize_config,
  .set_form_name = _gradient_set_form_name,
  .set_hint_message = _gradient_set_hint_message,
  .duplicate_points = _gradient_duplicate_points,
  .get_distance = _gradient_get_distance,
  .get_points_border = _gradient_get_points_border,
  .get_mask = _gradient_get_mask,
  .get_mask_roi = _gradient_get_mask_roi,
  .get_area = _gradient_get_area,
  .mouse_moved = _gradient_events_mouse_moved,
  .mouse_scrolled = _gradient_events_mouse_scrolled,
  .button_pressed = _gradient_events_button_pressed,
  .button_released = _gradient_events_button_released,
  .post_expose = _gradient_events_post_expose
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
