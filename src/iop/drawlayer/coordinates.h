#ifndef DT_IOP_DRAWLAYER_COORDINATES_H
#define DT_IOP_DRAWLAYER_COORDINATES_H


/** @file
 *  @brief Shared coordinate transforms and geometry computations for drawlayer.
 */

typedef struct drawlayer_view_patch_t
{
  int x;
  int y;
  int width;
  int height;
} drawlayer_view_patch_t;

typedef struct drawlayer_view_patch_info_t
{
  drawlayer_view_patch_t patch;
  float layer_x0;
  float layer_y0;
  float layer_x1;
  float layer_y1;
} drawlayer_view_patch_info_t;

/**
 * @brief Where the raw-anchored layer canvas sits in this module's own frame.
 *
 * @details The layer has the RAW image's dimensions and sits at the origin of the raw frame. The
 * module's own frame is a window into it, at the position the size fold gives that window. That is
 * the whole placement: an offset, never a scale, never a rotation.
 *
 * It follows from what the layer is FOR. Paint is authored on the sensor's frame, so it must not
 * move when the frame around it changes -- crop it tighter, or move the crop without resizing it,
 * and the same paint stays on the same content, cropped along with it. Uncropped, the two frames
 * coincide and so do their centres.
 *
 * The layer is deliberately IMMUNE to perspective correction and to horizon rotation. Those change
 * what the frame contains, not where the sensor's centre is, and following them would mean warping
 * a four-channel raster through the upstream chain on every render.
 *
 * The one transform that does apply is the FLIP. A raw buffer is landscape, so a portrait image is
 * a raw rotated 90 degrees, and the layer is rotated with it -- carried here by the canvas being
 * stated in the module's own orientation (::dt_drawlayer_layer_canvas), which is what the user
 * paints on and what the sidecar holds.
 */
typedef struct dt_drawlayer_raw_placement_t
{
  int offset_x, offset_y;   /**< canvas pixel under the frame's origin, at full resolution */
  gboolean valid;
} dt_drawlayer_raw_placement_t;

/**
 * @brief The layer canvas: the raw image's dimensions, in this image's own orientation.
 *
 * @details Transposed when the axes are swapped, because a portrait is a landscape raw seen rotated
 * and the paint has to be rotated with it. WHETHER they are swapped is asked of iop/flip.c's own
 * size fold, not of the EXIF tag -- a raw's recorded orientation is not always right, and flip is
 * where the user corrects it. Everything else about the canvas is fixed: it does not move with a
 * crop, a zoom, a pipe type or a mipmap level, which is what makes it something to anchor to.
 *
 * @p pipe selects which side's copy of that fold answers -- NULL for the GUI's records, a pipe for
 * its own pieces. They are the same fold, so they agree.
 */
gboolean dt_drawlayer_layer_canvas_for_pipe(dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                                            int *width, int *height);

/** @brief ::dt_drawlayer_layer_canvas_for_pipe for a GUI caller. */
gboolean dt_drawlayer_layer_canvas(dt_iop_module_t *self, int *width, int *height);

/** @brief Anchor the canvas against @p frame, this module's input rect from the size fold. */
gboolean dt_drawlayer_raw_placement_for_frame(dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                                              const dt_iop_roi_t *frame,
                                              dt_drawlayer_raw_placement_t *placement);

/** @brief The placement against this module's frame, as the GUI sees it. */
gboolean dt_drawlayer_raw_placement_gui(dt_iop_module_t *self, dt_drawlayer_raw_placement_t *placement,
                                        int *frame_width, int *frame_height);

gboolean dt_drawlayer_widget_points_to_layer_coords(dt_iop_module_t *self, float *pts, int count);
gboolean dt_drawlayer_layer_points_to_widget_coords(dt_iop_module_t *self, float *pts, int count);
gboolean dt_drawlayer_widget_to_layer_coords(dt_iop_module_t *self, double wx, double wy, float *lx, float *ly);
gboolean dt_drawlayer_layer_to_widget_coords(dt_iop_module_t *self, float x, float y, float *wx, float *wy);
gboolean dt_drawlayer_layer_bounds_to_widget_bounds(dt_iop_module_t *self, float x0, float y0,
                                                    float x1, float y1,
                                                    float *left, float *top,
                                                    float *right, float *bottom);
float dt_drawlayer_widget_brush_radius(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab, float fallback);
float dt_drawlayer_current_live_padding(dt_iop_module_t *self);
gboolean dt_drawlayer_compute_view_patch(dt_iop_module_t *self, float padding, drawlayer_view_patch_info_t *view);
#endif // DT_IOP_DRAWLAYER_COORDINATES_H
