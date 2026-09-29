/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * A text frame set around the pictures laid over it, and the height it comes to.
 *
 * Every case here is a defect that was reported on one frame -- "the second paragraph follows
 * the picture down", "the text disappears", "the auto height makes the frame infinitely high",
 * "empty lines beside the picture" -- and each assertion below is the measurement that told its
 * fault from the others; the numbers in each comment are what the fault gave before it was
 * fixed, taken by putting it back. The engine they pin was then rewritten to fill its lines by
 * measure (`canvas/canvas_text_words.h`), and every case held.
 *
 * The frame is measured rather than read: `dt_canvas_paint_text_natural_height()` is what the
 * auto height solves against, and where a line actually landed is counted off a painted
 * surface, since a walk that stops early reports a plausible height and simply sets less text.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_props.h"
#include "caches/pixelpipe_cache.h"

#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner): cmocka requires it ahead of its own header
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#define FRAME_WIDTH 400.0
#define FRAME_TOP (-150.0)
#define STANDOFF 12.0

/** Three paragraphs, so the space between them is charged twice and no more. */
static const char *TEXT
    = "The first paragraph runs along for a few lines so that the column has something to set and the "
      "flow has something to push about when a picture is laid over it.\n\n"
      "The second paragraph is the one that follows the picture down the page, leaving a gap behind it "
      "that nothing closes again.\n\n"
      "The third paragraph should sit under the second one at the paragraph spacing and no more.";

/**
 * The same, with a comma at the end of most lines.
 *
 * Prose punctuated this way is what makes the optical margins hang something: a line ending on a
 * letter hangs nothing at all, and the case below needs the hang to exist.
 */
static const char *PUNCTUATED
    = "Alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, "
      "alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha.\n\n"
      "Bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo, "
      "bravo, bravo, bravo, bravo, bravo, bravo, bravo, bravo.\n\n"
      "Charlie, charlie, charlie, charlie, charlie, charlie, charlie, charlie, charlie, charlie.";

/** A flowing text frame, black on white, its top edge at FRAME_TOP. */
static dt_canvas_object_t *_flowing_text(dt_canvas_t *canvas, const double height, const double spacing)
{
  dt_canvas_object_t *text
      = dt_canvas_add_text(canvas, 0.0, FRAME_TOP + height * 0.5, FRAME_WIDTH, height, TEXT);
  g_strlcpy(text->text.font, "DejaVu Serif 14", DT_CANVAS_FONT_LEN);
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = STANDOFF;
  text->text.paragraph_spacing = (float)spacing;
  text->text.background.alpha = 0.0f;
  text->text.text_color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  text->border_width = 0.0f;
  text->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  return text;
}

/** A picture over the text: white, so only where it stands is read, never its own ink. */
static dt_canvas_object_t *_picture(dt_canvas_t *canvas, const double left, const double top,
                                    const double width, const double height)
{
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  style.border_override = TRUE;
  style.border_width = 0.0f;
  style.shadow_override = TRUE;
  memset(&style.shadow, 0, sizeof(style.shadow));
  const dt_canvas_rect_t box = { left, top, width, height };
  return dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
}

/** A white canvas with nothing on it but what a case adds. */
static dt_canvas_t *_white_canvas(void)
{
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  return canvas;
}

/** What one measuring pass says the text comes to, at the frame's current height. */
static double _natural(const dt_canvas_t *canvas, const dt_canvas_object_t *text)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(surface);
  const double natural = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return natural;
}

/** Place the frame's top edge at FRAME_TOP whatever height it is given, as the fit does. */
static void _set_height(dt_canvas_object_t *text, const double height)
{
  text->height = height;
  text->y = FRAME_TOP + height * 0.5;
}

/**
 * The lowest canvas y carrying any of the text's ink, or -G_MAXDOUBLE where none does.
 *
 * Painted at one unit per pixel over `span` units from FRAME_TOP, which is the only way to see
 * that a walk set fewer lines than it claimed: the height it returns is the height of what it
 * DID set.
 */
static double _lowest_ink(const dt_canvas_t *canvas, const double span)
{
  const int size = (int)span;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_t *cr = cairo_create(surface);
  // One pixel per unit, with the surface's top-left corner at (-size / 2, FRAME_TOP).
  cairo_translate(cr, size * 0.5, -FRAME_TOP);
  const dt_canvas_rect_t whole = { -size * 0.5, FRAME_TOP, (double)size, (double)size };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  double lowest = -G_MAXDOUBLE;
  for(int row = 0; row < size; row++)
  {
    const uint32_t *line = (const uint32_t *)(pixels + (size_t)row * stride);
    int dark = 0;
    for(int column = 0; column < size; column++)
      if((line[column] & 0xFFu) < 80u) dark++;
    // A stray anti-aliased pixel is not a line of type.
    if(dark > 2) lowest = FRAME_TOP + row;
  }
  cairo_surface_destroy(surface);
  return lowest;
}

/**
 * The map's pitch is a length in canvas units, never the frame's height divided by a row count.
 *
 * Sized as `extent / rows` the pitch wobbled with the height by a fortieth of a unit, which over
 * a hundred rows moves a cell boundary near the bottom of the map by a whole cell -- and the
 * height is the one number the auto height is solving for, so the measurement fed on its own
 * answer. Measured on this frame and this picture, the natural height took two values three
 * units of frame height apart, 365.94 and 382.23, NEITHER of them a fixed point.
 */
static void _the_map_is_pitched_in_canvas_units_not_in_frame_heights(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _white_canvas();
  dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 8.0);
  _picture(canvas, -150.0, -20.0, 530.0, 120.0);

  _set_height(text, 340.0);
  const double reference = _natural(canvas, text);
  assert_true(reference > 0.0);
  // Every height across two full cells of the old wobble, which is where it flipped.
  for(double height = 340.0; height <= 400.0; height += 1.0)
  {
    _set_height(text, height);
    assert_float_equal(_natural(canvas, text), reference, 0.01);
  }
  dt_canvas_free(canvas);
}

/**
 * The auto height settles, and a settled frame reports no movement.
 *
 * The flip above had no fixed point at all, so `dt_canvas_props_settle_all()` answered "moved"
 * for as long as anything asked -- the frame growing and shrinking by a line on every repaint.
 * Swept down the column, because it flipped at some placements of the picture and not others.
 */
static void _the_auto_height_settles_wherever_the_picture_stands(void **state)
{
  (void)state;
  for(double top = -180.0; top <= 180.0; top += 13.0)
  {
    dt_canvas_t *canvas = _white_canvas();
    dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 8.0);
    text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT;
    _picture(canvas, -150.0, top, 530.0, 120.0);

    int rounds = 0;
    gboolean moved = TRUE;
    // Two is what every placement takes: one to grow, one to confirm.
    for(; rounds < 8 && moved; rounds++) moved = dt_canvas_props_settle_all(canvas);
    assert_false(moved);
    const double settled = text->height;
    // And the fixed point is a fixed point: asked again, the frame stays where it is.
    assert_float_equal(_natural(canvas, text), settled, 0.01);
    dt_canvas_free(canvas);
  }
}

/**
 * A band the map does not reach is clear, not a copy of the map's nearest row.
 *
 * The map is built over the frame's current height while the measuring pass flows without a
 * cut, so every line past that height falls outside it. Clamped onto the last row, a picture
 * covering the map's bottom blocked the whole page below it and the walk stepped down to its
 * line cap: measured on this frame, ONE measuring pass returned 66776 units where the text
 * wants about 270 -- the frame reported as infinitely high.
 */
static void _a_band_the_map_does_not_reach_is_clear(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _white_canvas();
  dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 8.0);
  // Across the whole column, from above the frame's top down past anything the map can cover.
  _picture(canvas, -260.0, -400.0, 520.0, 5000.0);

  for(double height = 100.0; height <= 500.0; height += 100.0)
  {
    _set_height(text, height);
    const double natural = _natural(canvas, text);
    // It may exceed the frame -- the text is being pushed below a picture it cannot clear --
    // but by what the map can see, not by three orders of magnitude.
    assert_true(natural > 0.0);
    assert_true(natural < height + 1000.0);
  }
  dt_canvas_free(canvas);
}

/**
 * The space between paragraphs is charged once per paragraph, not once per attempt.
 *
 * A line the obstacles refuse sets nothing and consumes nothing, so the walk moves down and asks
 * again at the same offset -- which is a paragraph start again. Charged there, a paragraph
 * waiting for the foot of a picture was pushed one further gap down for every line it waited.
 * Two breaks in this text, so a 200-unit spacing costs exactly 400 units of frame: measured over
 * this grid before the fix, 600.
 */
static void _the_space_between_paragraphs_is_paid_once_per_paragraph(void **state)
{
  (void)state;
  const double spacing = 200.0;
  // One position cannot see it: the picture has to refuse the line a paragraph opens on.
  for(double left = -190.0; left <= 60.0; left += 25.0)
    for(double top = -160.0; top <= 40.0; top += 20.0)
    {
      double heights[2];
      for(int spaced = 0; spaced < 2; spaced++)
      {
        dt_canvas_t *canvas = _white_canvas();
        dt_canvas_object_t *text = _flowing_text(canvas, 300.0, spaced ? spacing : 0.0);
        text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT;
        _picture(canvas, left, top, FRAME_WIDTH - left, 120.0);
        for(int round = 0; round < 8 && dt_canvas_props_settle_all(canvas); round++) continue;
        heights[spaced] = text->height;
        dt_canvas_free(canvas);
      }
      // Never more than the two breaks owe. Less is legitimate: a gap can absorb a line the
      // picture was refusing anyway, and then the frame does not grow by the whole of it.
      assert_true(heights[1] - heights[0] <= 2.0 * spacing + 0.01);
    }
}

/**
 * A band with no room in it is a line the walk steps OVER, never the end of the text.
 *
 * Ending the walk there deleted every word below a picture as wide as the column: measured on
 * this frame, 19 rows of type against 77, and nothing at all below the picture -- reported as
 * the text disappearing. The lines above it were set correctly, which is what makes it read as
 * a flow problem rather than a truncation.
 */
static void _a_band_with_no_room_is_a_line_stepped_over(void **state)
{
  (void)state;
  const double picture_top = -80.0;
  const double picture_height = 120.0;
  dt_canvas_t *canvas = _white_canvas();
  dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 8.0);
  text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT;
  // Wider than the column, so no stretch of any band beside it can hold a word.
  _picture(canvas, -260.0, picture_top, 520.0, picture_height);
  for(int round = 0; round < 8 && dt_canvas_props_settle_all(canvas); round++) continue;

  // The text carries on below the picture rather than stopping at it.
  const double lowest = _lowest_ink(canvas, 900.0);
  assert_true(lowest > picture_top + picture_height);
  dt_canvas_free(canvas);
}

/**
 * An indented first line is judged against ITS OWN measure, which is the frame's less the indent.
 *
 * The old walk set each stretch with Pango and asked whether the result would have fitted a
 * wider stretch, answering against the frame's whole measure while the piece had been set on the
 * measure less the paragraph indent -- so a first line whose set width landed between the two (an
 * optical margin hangs its comma past the measure, and a justified line is stretched to it) was
 * refused for being one indent too wide. A refused line consumed nothing, so the next attempt was
 * the same first line, indented again, refused again, one line lower each time.
 *
 * Measured with nothing laid over the column at all: at any indent the frame came to 66776 units
 * -- the line cap -- with NO ink anywhere, the text gone and the frame bottomless. Over a picture
 * it read differently and was the same fault: the cascade stopped at the first stretch narrow
 * enough for a short piece to fit, which is beside the picture, so the paragraph's opening line
 * landed against it and moved down with it whenever it was dragged. That was the report. Filled
 * by measure there is no refusal to get wrong: the indent narrows the stretch the words are
 * measured against, and that is the whole of it.
 */
static void _an_indented_first_line_is_judged_against_its_own_measure(void **state)
{
  (void)state;
  double plain = 0.0;
  for(double indent = 0.0; indent <= 60.0; indent += 20.0)
  {
    dt_canvas_t *canvas = _white_canvas();
    dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 0.0);
    dt_canvas_text_set_markdown(canvas, text, PUNCTUATED);
    // The combination the report came on: justified, hanging its punctuation, fitting its height.
    text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT | DT_CANVAS_TEXT_OPTICAL_MARGINS;
    text->text.align_h = DT_CANVAS_ALIGN_JUSTIFY;
    text->text.first_line_indent = (float)indent;
    text->text.wrap_standoff = 1.0f;
    _set_height(text, 300.0);
    for(int round = 0; round < 8 && dt_canvas_props_settle_all(canvas); round++) continue;

    if(indent == 0.0) plain = text->height;
    assert_true(plain > 0.0);
    // An indent moves a word to the next line at most: it can cost a line, never the whole text.
    assert_true(text->height <= plain + 2.0 * 20.0);
    // And the text is still on the page. Refused for ever, it was nowhere at all.
    assert_true(_lowest_ink(canvas, 900.0) > FRAME_TOP);
    dt_canvas_free(canvas);
  }
}

/**
 * The line bands inside a picture's vertical span, and how many of them carry ink LEFT of it.
 *
 * Painted like `_lowest_ink()`; a band is a run of rows with ink anywhere in the frame's width.
 */
static int _bands_beside(const dt_canvas_t *canvas, const double picture_left, const double picture_top,
                         const double picture_bottom, int *inked_left)
{
  const int size = 900;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, size * 0.5, -FRAME_TOP);
  const dt_canvas_rect_t whole = { -size * 0.5, FRAME_TOP, (double)size, (double)size };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const int left_edge = (int)(picture_left + size * 0.5);
  int bands = 0;
  *inked_left = 0;
  gboolean in_band = FALSE;
  gboolean band_left = FALSE;
  for(int row = (int)(picture_top - FRAME_TOP); row <= (int)(picture_bottom - FRAME_TOP) && row < size; row++)
  {
    const uint32_t *line = (const uint32_t *)(pixels + (size_t)row * stride);
    int dark = 0;
    int dark_left = 0;
    for(int column = 0; column < size; column++)
      if((line[column] & 0xFFu) < 80u)
      {
        dark++;
        if(column < left_edge) dark_left++;
      }
    if(dark > 2)
    {
      in_band = TRUE;
      if(dark_left > 2) band_left = TRUE;
    }
    else if(in_band)
    {
      bands++;
      if(band_left) (*inked_left)++;
      in_band = FALSE;
      band_left = FALSE;
    }
  }
  cairo_surface_destroy(surface);
  return bands;
}

/**
 * A stretch beside a picture holds every word that fits it.
 *
 * The old walk set each stretch with Pango and refused the result when its width exceeded the
 * stretch -- which an optical margin's hang and a justified line's stretching both make it do
 * on purpose -- so beside a picture every line ending on a comma was refused and its stretch
 * left EMPTY for the picture's whole height (measured on the reported document: `run 1 w 167.00
 * REFUSED (took 22 bytes)` on a stretch that plainly held it). Filled by measure, a stretch wide
 * enough for the words takes them: every band beside this picture carries ink on the open side.
 */
static void _a_stretch_beside_a_picture_holds_the_words_that_fit_it(void **state)
{
  (void)state;
  const double picture_top = -60.0;
  const double picture_height = 120.0;
  dt_canvas_t *canvas = _white_canvas();
  dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 0.0);
  dt_canvas_text_set_markdown(canvas, text, PUNCTUATED);
  text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  text->text.align_h = DT_CANVAS_ALIGN_JUSTIFY;
  text->text.wrap_standoff = 1.0f;
  // Over the right half of the column: the left stretch is 250 units wide, room for several words.
  _picture(canvas, 60.0, picture_top, 150.0, picture_height);
  for(int round = 0; round < 8 && dt_canvas_props_settle_all(canvas); round++) continue;

  int inked_left = 0;
  const int bands = _bands_beside(canvas, 60.0, picture_top, picture_top + picture_height, &inked_left);
  assert_true(bands >= 4);
  assert_int_equal(inked_left, bands);
  dt_canvas_free(canvas);
}

/**
 * A word wider than every stretch of its line waits for a wider one below -- the line box moves
 * down, as CSS 2.1 §9.5 has it -- and a word wider than the frame's own measure is set where it
 * stands, overflowing, rather than waited on for ever.
 */
static void _a_word_wider_than_a_stretch_waits_and_one_wider_than_the_frame_overflows(void **state)
{
  (void)state;
  const char *long_word = "Short words here and then Pneumonoultramicroscopicsilicovolcanoconiosis follows, and "
                          "the column carries on below with short words again.";
  // A picture leaving a stretch too narrow for the long word but wide enough for the others.
  dt_canvas_t *canvas = _white_canvas();
  dt_canvas_object_t *text = _flowing_text(canvas, 300.0, 0.0);
  dt_canvas_text_set_markdown(canvas, text, long_word);
  text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT;
  const double bare = _natural(canvas, text);
  _picture(canvas, -80.0, -140.0, 280.0, 60.0);
  const double waited = _natural(canvas, text);
  assert_true(bare > 0.0);
  // The long word waited below the picture, costing the column lines, and no more than the
  // picture's own height plus a line or two: never a runaway.
  assert_true(waited > bare);
  assert_true(waited < bare + 60.0 + 3.0 * 20.0);
  dt_canvas_free(canvas);

  // A frame narrower than the word: it is set anyway, overflowing, and the text stays finite.
  canvas = _white_canvas();
  text = _flowing_text(canvas, 300.0, 0.0);
  dt_canvas_text_set_markdown(canvas, text, long_word);
  text->text.text_flags |= DT_CANVAS_TEXT_AUTO_HEIGHT | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  text->width = 160.0;
  for(int round = 0; round < 8 && dt_canvas_props_settle_all(canvas); round++) continue;
  assert_true(text->height < 600.0);
  assert_true(_lowest_ink(canvas, 900.0) > FRAME_TOP + 100.0);
  dt_canvas_free(canvas);
}

static int _group_setup(void **state)
{
  (void)state;
  // The shapes take their scratch from the pixelpipe cache's arena.
  return dt_dev_pixelpipe_cache_init(64u * 1024u * 1024u, FALSE, FALSE) ? 0 : 1;
}

static int _group_teardown(void **state)
{
  (void)state;
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_the_map_is_pitched_in_canvas_units_not_in_frame_heights),
    cmocka_unit_test(_the_auto_height_settles_wherever_the_picture_stands),
    cmocka_unit_test(_a_band_the_map_does_not_reach_is_clear),
    cmocka_unit_test(_the_space_between_paragraphs_is_paid_once_per_paragraph),
    cmocka_unit_test(_a_band_with_no_room_is_a_line_stepped_over),
    cmocka_unit_test(_an_indented_first_line_is_judged_against_its_own_measure),
    cmocka_unit_test(_a_stretch_beside_a_picture_holds_the_words_that_fit_it),
    cmocka_unit_test(_a_word_wider_than_a_stretch_waits_and_one_wider_than_the_frame_overflows),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
