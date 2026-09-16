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

/* The per-object property table and its writer: the shape of the table every frontend builds
 * its rows from, and the edit rules the property bar's handlers used to carry one by one. */

#include "caches/pixelpipe_cache.h"
#include "canvas/canvas.h"
#include "canvas/canvas_props.h"
#include "canvas/canvas_render.h"
#include "common/conf.h"
#include "darktable.h"
#include "system/macros.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <pango/pango.h>
#include <unistd.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

static char *_rcfile = NULL;

static const uint32_t _kinds[] = { DT_CANVAS_OBJECT_TEXT, DT_CANVAS_OBJECT_IMAGE, DT_CANVAS_OBJECT_MAP,
                                   DT_CANVAS_OBJECT_SVG, DT_CANVAS_OBJECT_CONNECTOR, DT_CANVAS_OBJECT_SHAPE };

static gchar *_write_svg(const char *body)
{
  gchar *path = NULL;
  const int handle = g_file_open_tmp("canvas-props-XXXXXX.svg", &path, NULL);
  assert_true(handle >= 0);
  close(handle);
  assert_true(g_file_set_contents(path, body, -1, NULL));
  return path;
}

/** A canvas with one object of every kind: a text, a picture, a map, a drawing, a connector and a shape. */
typedef struct props_fixture_t
{
  dt_canvas_t *canvas;
  dt_canvas_object_t *objects[6]; ///< in `_kinds` order
  gchar *svg_path;
} props_fixture_t;

/** The canvas's border once the objects exist: not the one every frame was born with a copy of. */
#define CANVAS_BORDER_WIDTH 4.0f
#define CANVAS_BORDER_RED 0.1f
#define CANVAS_BORDER_GREEN 0.5f
#define CANVAS_BORDER_BLUE 0.9f

static void _fixture_build(props_fixture_t *fixture)
{
  memset(fixture, 0, sizeof(*fixture));
  fixture->canvas = dt_canvas_new();
  fixture->canvas->border_width = 3.0f;
  fixture->canvas->border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  fixture->canvas->corner_radius = 6.0f;
  fixture->canvas->shadow.blur = 10.0f;
  fixture->objects[0]
      = dt_canvas_add_text(fixture->canvas, 0.0, 0.0, 300.0, 120.0, "A caption, set in the canvas's font.");
  fixture->objects[1] = dt_canvas_add_image(fixture->canvas, 600.0, 0.0, 6000, 4000);
  fixture->objects[2] = dt_canvas_add_map(fixture->canvas, 0.0, 600.0, 38.7, -9.1, 12, 0);
  fixture->svg_path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 144 72'>"
                                 "<rect width='144' height='72' fill='#ff0000'/></svg>");
  GError *error = NULL;
  fixture->objects[3] = dt_canvas_add_svg(fixture->canvas, 600.0, 600.0, fixture->svg_path, &error);
  assert_null(error);
  fixture->objects[4] = dt_canvas_add_connector(fixture->canvas, fixture->objects[1]->id, fixture->objects[2]->id);
  const dt_canvas_rect_t shape_box = { 1000.0, 0.0, 200.0, 120.0 };
  fixture->objects[5] = dt_canvas_add_shape(fixture->canvas, DT_CANVAS_SHAPE_RECTANGLE, &shape_box, NULL);
  for(int idx = 0; idx < 6; idx++)
    assert_non_null(fixture->objects[idx]);
  // Every frame is born holding a copy of the canvas's border in its own fields. Moved on from
  // that copy, the canvas's border and a frame's own differ, so a read that takes the wrong one --
  // or a seed that forgets a field -- shows.
  fixture->canvas->border_width = CANVAS_BORDER_WIDTH;
  fixture->canvas->border_color
      = dt_canvas_color(CANVAS_BORDER_RED, CANVAS_BORDER_GREEN, CANVAS_BORDER_BLUE, 1.0f);
}

static void _fixture_free(props_fixture_t *fixture)
{
  dt_canvas_free(fixture->canvas);
  g_unlink(fixture->svg_path);
  g_free(fixture->svg_path);
}

static dt_canvas_prop_value_t _number(const double number)
{
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  value.number = number;
  return value;
}

static dt_canvas_prop_value_t _flag(const gboolean flag)
{
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  value.flag = flag;
  return value;
}

static dt_canvas_prop_value_t _choice(const int choice)
{
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  value.choice = choice;
  return value;
}

static dt_canvas_prop_value_t _color(const float red_value, const float green_value, const float blue_value,
                                     const float alpha_value)
{
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  value.color = dt_canvas_color(red_value, green_value, blue_value, alpha_value);
  return value;
}

static dt_canvas_prop_value_t _text(const char *text)
{
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  g_strlcpy(value.text, text, sizeof(value.text));
  return value;
}

static double _read_number(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                           const dt_canvas_prop_id_t id)
{
  dt_canvas_prop_value_t value;
  dt_canvas_prop_read(canvas, object, id, &value);
  return value.number;
}

/* --- the table ------------------------------------------------------------------------------ */

static void _every_property_is_described_once(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  assert_int_equal(count, DT_CANVAS_PROP_COUNT - 1);
  GHashTable *keys = g_hash_table_new(g_str_hash, g_str_equal);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    // Dense: the id is the index, so a lookup is an index and no id is missing or repeated.
    assert_int_equal(prop->id, (int)idx + 1);
    assert_ptr_equal(dt_canvas_prop_get(prop->id), prop);
    assert_non_null(prop->key);
    assert_non_null(prop->label);
    assert_non_null(prop->tooltip);
    assert_false(g_hash_table_contains(keys, prop->key));
    g_hash_table_add(keys, (gpointer)prop->key);
    assert_true(prop->kinds != 0);
    const uint32_t known = (1u << DT_CANVAS_OBJECT_TEXT) | (1u << DT_CANVAS_OBJECT_IMAGE)
                           | (1u << DT_CANVAS_OBJECT_MAP) | (1u << DT_CANVAS_OBJECT_SVG)
                           | (1u << DT_CANVAS_OBJECT_CONNECTOR) | (1u << DT_CANVAS_OBJECT_SHAPE);
    assert_int_equal(prop->kinds & ~known, 0);
    assert_true(prop->section < DT_CANVAS_SECTION_COUNT);
    assert_true(prop->factor != 0.0);
  }
  g_hash_table_destroy(keys);
  assert_null(dt_canvas_prop_get(DT_CANVAS_PROP_NONE));
  assert_null(dt_canvas_prop_get(DT_CANVAS_PROP_COUNT));
  // Every kind has something to show.
  for(size_t kind_index = 0; kind_index < G_N_ELEMENTS(_kinds); kind_index++)
  {
    int rows = 0;
    for(size_t idx = 0; idx < count; idx++)
      rows += dt_canvas_prop_for_kind(&table[idx], _kinds[kind_index]) ? 1 : 0;
    assert_true(rows > 0);
  }
}

static void _a_numbers_soft_range_and_neutral_lie_inside_its_hard_range(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->widget != DT_CANVAS_WIDGET_TUNE && prop->widget != DT_CANVAS_WIDGET_MEASURE) continue;
    assert_true(prop->min < prop->max);
    assert_true(prop->min <= prop->soft_min);
    assert_true(prop->soft_min < prop->soft_max);
    assert_true(prop->soft_max <= prop->max);
    assert_true(prop->step > 0.0);
    assert_true(prop->digits >= 0);
    if(!isnan(prop->neutral))
    {
      assert_true(prop->neutral >= prop->min);
      assert_true(prop->neutral <= prop->max);
    }
  }
}

static void _pairs_point_at_each_other(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->pair_with == DT_CANVAS_PROP_NONE) continue;
    const dt_canvas_prop_t *other = dt_canvas_prop_get(prop->pair_with);
    assert_non_null(other);
    assert_int_equal(other->pair_with, prop->id);
    assert_int_equal(other->section, prop->section);
    assert_int_equal(other->tier, prop->tier);
    assert_int_equal(other->kinds, prop->kinds);
  }
}

static void _every_kind_reads_its_sections_in_screen_order(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t kind_index = 0; kind_index < G_N_ELEMENTS(_kinds); kind_index++)
  {
    int last_section = -1;
    int last_tier = -1;
    int strip_rows = 0;
    for(size_t idx = 0; idx < count; idx++)
    {
      const dt_canvas_prop_t *prop = &table[idx];
      if(!dt_canvas_prop_for_kind(prop, _kinds[kind_index])) continue;
      // Sections never go back, and within one the strip comes first and the extras last.
      assert_true((int)prop->section >= last_section);
      if((int)prop->section != last_section) last_tier = -1;
      assert_true((int)prop->tier >= last_tier);
      last_section = (int)prop->section;
      last_tier = (int)prop->tier;
      if(prop->tier != DT_CANVAS_TIER_STRIP) continue;
      // The strip carries only the kind's own everyday controls, one button row tall.
      strip_rows++;
      assert_true(prop->section < DT_CANVAS_SECTION_ARRANGE);
      assert_true(prop->widget == DT_CANVAS_WIDGET_MEASURE || prop->widget == DT_CANVAS_WIDGET_ICONS
                  || prop->widget == DT_CANVAS_WIDGET_ICON_FLAG || prop->widget == DT_CANVAS_WIDGET_COLOR
                  || prop->widget == DT_CANVAS_WIDGET_FONT || prop->widget == DT_CANVAS_WIDGET_INFO
                  || prop->widget == DT_CANVAS_WIDGET_ACTION);
    }
    assert_true(strip_rows <= 5);
  }
}

static void _groups_belong_to_their_sections(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    switch(prop->group)
    {
      case DT_CANVAS_GROUP_NONE:
        break;
      case DT_CANVAS_GROUP_BORDER:
        assert_int_equal(prop->section, DT_CANVAS_SECTION_STROKE);
        assert_false(dt_canvas_prop_for_kind(prop, DT_CANVAS_OBJECT_CONNECTOR));
        break;
      case DT_CANVAS_GROUP_CORNER:
        assert_int_equal(prop->section, DT_CANVAS_SECTION_CORNERS);
        break;
      case DT_CANVAS_GROUP_SHADOW:
        assert_int_equal(prop->section, DT_CANVAS_SECTION_SHADOW);
        break;
      case DT_CANVAS_GROUP_FONT:
        assert_int_equal(prop->section, DT_CANVAS_SECTION_CHARACTER);
        assert_int_equal(prop->kinds, 1u << DT_CANVAS_OBJECT_TEXT);
        break;
      default:
        fail();
    }
    // A section's group is the group of the rows it holds.
    for(size_t kind_index = 0; kind_index < G_N_ELEMENTS(_kinds); kind_index++)
    {
      if(!dt_canvas_prop_for_kind(prop, _kinds[kind_index]) || prop->group == DT_CANVAS_GROUP_NONE) continue;
      assert_int_equal(dt_canvas_prop_section_group(prop->section, _kinds[kind_index]), prop->group);
    }
  }
}

static void _conditions_depend_on_the_objects_own_switches(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    const dt_canvas_prop_id_t conditions[2] = { prop->visible_if, prop->sensitive_if };
    const uint32_t allowed[2] = { prop->visible_values, prop->sensitive_values };
    for(int which = 0; which < 2; which++)
    {
      if(conditions[which] == DT_CANVAS_PROP_NONE) continue;
      const dt_canvas_prop_t *controller = dt_canvas_prop_get(conditions[which]);
      assert_non_null(controller);
      // Read with no canvas by dt_canvas_prop_applies(), so never an inherited value.
      assert_int_equal(controller->group, DT_CANVAS_GROUP_NONE);
      assert_true(controller->widget == DT_CANVAS_WIDGET_FLAG || controller->widget == DT_CANVAS_WIDGET_ICON_FLAG
                  || controller->widget == DT_CANVAS_WIDGET_ICONS
                  || controller->widget == DT_CANVAS_WIDGET_CHOICE);
      assert_true((controller->kinds & prop->kinds) != 0);
      assert_true(allowed[which] != 0);
    }
    if(prop->widget == DT_CANVAS_WIDGET_ICONS)
    {
      const int choices = dt_canvas_prop_choice_count(prop);
      assert_int_equal(choices, (int)lround(prop->max) + 1);
      int icons = 0;
      while(!IS_NULL_PTR(prop->icons) && !IS_NULL_PTR(prop->icons[icons])) icons++;
      assert_int_equal(icons, choices);
    }
  }
}

/* --- reading and writing -------------------------------------------------------------------- */

/** Put the property a row depends on at the first value that shows the row. */
static void _make_visible(dt_canvas_t *canvas, dt_canvas_object_t *object, const dt_canvas_prop_t *prop)
{
  if(prop->visible_if == DT_CANVAS_PROP_NONE) return;
  const dt_canvas_prop_t *controller = dt_canvas_prop_get(prop->visible_if);
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  for(int bit_index = 0; bit_index < 32; bit_index++)
  {
    if(!(prop->visible_values & (1u << bit_index))) continue;
    value.choice = bit_index;
    value.flag = bit_index == 1;
    break;
  }
  dt_canvas_prop_write(canvas, object, controller->id, &value);
  assert_true(dt_canvas_prop_applies(prop, object));
}

static void _note_pair(GString *list, const dt_canvas_prop_t *prop, const dt_canvas_object_t *object)
{
  g_string_append_printf(list, "%s@%u;", prop->key, object->kind);
}

static void _every_property_round_trips_on_every_kind(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  int written = 0;
  // Every row this test passes over, and every row whose read-back it excuses, by name: the
  // matrix must not thin out silently.
  GString *skipped = g_string_new(NULL);
  GString *excused = g_string_new(NULL);
  for(size_t kind_index = 0; kind_index < G_N_ELEMENTS(_kinds); kind_index++)
  {
    dt_canvas_object_t *object = fixture.objects[kind_index];
    for(size_t idx = 0; idx < count; idx++)
    {
      const dt_canvas_prop_t *prop = &table[idx];
      if(!dt_canvas_prop_for_kind(prop, object->kind)) continue;
      // Descriptions, requests and the view's own state carry nothing to round-trip.
      if(prop->widget == DT_CANVAS_WIDGET_INFO || prop->widget == DT_CANVAS_WIDGET_ACTION) continue;
      if(prop->id == DT_CANVAS_PROP_CUTOUT_EDIT) continue;
      _make_visible(fixture.canvas, object, prop);
      // A height that follows the text is refitted over whatever is typed, which is the point of it.
      if(!dt_canvas_prop_sensitive(prop, fixture.canvas, object))
      {
        _note_pair(skipped, prop, object);
        continue;
      }
      dt_canvas_prop_value_t current;
      dt_canvas_prop_read(fixture.canvas, object, prop->id, &current);
      dt_canvas_prop_value_t wanted = current;
      switch(prop->widget)
      {
        case DT_CANVAS_WIDGET_TUNE:
        case DT_CANVAS_WIDGET_MEASURE:
        {
          const double quantum = pow(10.0, -(double)prop->digits);
          const double span = prop->soft_max - prop->soft_min;
          if(span > 10000.0)
          {
            // A position or a size has no soft range to speak of: move it by a few steps, so the
            // frame stays where the other frames are and the auto-height refit stays small.
            wanted.number = round((current.number + 37.0 * prop->step) / quantum) * quantum;
          }
          else
          {
            wanted.number = round((prop->soft_min + 0.37 * span) / quantum) * quantum;
            if(fabs(wanted.number - current.number) < quantum)
              wanted.number = round((prop->soft_min + 0.61 * span) / quantum) * quantum;
          }
          break;
        }
        case DT_CANVAS_WIDGET_FLAG:
        case DT_CANVAS_WIDGET_ICON_FLAG:
          wanted.flag = !current.flag;
          break;
        case DT_CANVAS_WIDGET_ICONS:
        case DT_CANVAS_WIDGET_CHOICE:
        {
          const int choices = dt_canvas_prop_choice_count(prop);
          if(choices < 2)
          {
            _note_pair(skipped, prop, object);
            continue;
          }
          wanted.choice = (current.choice + 1) % choices;
          break;
        }
        case DT_CANVAS_WIDGET_COLOR:
          wanted.color = dt_canvas_color(0.2f, 0.4f, 0.6f, 0.8f);
          break;
        case DT_CANVAS_WIDGET_FONT:
          g_strlcpy(wanted.text, "Serif Bold 17", sizeof(wanted.text));
          break;
        case DT_CANVAS_WIDGET_FEATURES:
          g_strlcpy(wanted.text, "liga 0, onum 1", sizeof(wanted.text));
          break;
        default:
          continue;
      }
      const double height_before = object->height;
      const uint32_t effects = dt_canvas_prop_write(fixture.canvas, object, prop->id, &wanted);
      if(!(effects & DT_CANVAS_EFFECT_CHANGED))
        fprintf(stderr, "no change: %s on kind %u\n", prop->key, object->kind);
      assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
      // Whatever moves, sizes, cuts or shadows a frame changes what other frames' lines avoid, and
      // the writer must say so. Keeping a ratio moves nothing.
      const gboolean moves_what_text_avoids
          = (prop->section == DT_CANVAS_SECTION_ARRANGE && prop->id != DT_CANVAS_PROP_KEEP_RATIO)
            || prop->section == DT_CANVAS_SECTION_SHADOW || prop->section == DT_CANVAS_SECTION_CUTOUT
            || prop->id == DT_CANVAS_PROP_BORDER_WIDTH || prop->id == DT_CANVAS_PROP_CORNER_RADIUS
            || prop->id == DT_CANVAS_PROP_TEXT_WRAP || prop->id == DT_CANVAS_PROP_TEXT_WRAP_GAP;
      if(moves_what_text_avoids) assert_true(effects & DT_CANVAS_EFFECT_SETTLE_ALL);
      written++;
      dt_canvas_prop_value_t back;
      dt_canvas_prop_read(fixture.canvas, object, prop->id, &back);
      // A text frame whose height follows its content is refitted after every edit, growing from
      // its top edge: moved into a frame its text flows around, its centre moves with half its
      // growth. That is the refit, not a lost value -- and a centre is the only value it moves.
      const gboolean refitted = prop->id == DT_CANVAS_PROP_Y && object->height != height_before;
      if(refitted) _note_pair(excused, prop, object);
      switch(prop->widget)
      {
        case DT_CANVAS_WIDGET_TUNE:
        case DT_CANVAS_WIDGET_MEASURE:
          if(refitted) break;
          if(fabs(back.number - wanted.number) >= 0.5 * pow(10.0, -(double)prop->digits))
            fprintf(stderr, "%s on kind %u read back %f\n", prop->key, object->kind, back.number);
          assert_float_equal(back.number, wanted.number, 0.5 * pow(10.0, -(double)prop->digits));
          break;
        case DT_CANVAS_WIDGET_FLAG:
        case DT_CANVAS_WIDGET_ICON_FLAG:
          assert_int_equal(back.flag != 0, wanted.flag != 0);
          break;
        case DT_CANVAS_WIDGET_ICONS:
        case DT_CANVAS_WIDGET_CHOICE:
          assert_int_equal(back.choice, wanted.choice);
          break;
        case DT_CANVAS_WIDGET_COLOR:
          assert_float_equal(back.color.red, wanted.color.red, 0.5 / 255.0);
          assert_float_equal(back.color.green, wanted.color.green, 0.5 / 255.0);
          assert_float_equal(back.color.blue, wanted.color.blue, 0.5 / 255.0);
          assert_float_equal(back.color.alpha, wanted.color.alpha, 0.5 / 255.0);
          break;
        default:
          assert_string_equal(back.text, wanted.text);
          break;
      }
      // A group member written owns its group from then on.
      if(prop->group != DT_CANVAS_GROUP_NONE)
        assert_int_equal(dt_canvas_group_state(fixture.canvas, object, prop->group), DT_CANVAS_OWN_CUSTOM);
      // The same value again is no edit.
      if(!refitted) assert_int_equal(dt_canvas_prop_write(fixture.canvas, object, prop->id, &wanted), 0);
    }
  }
  // Only the height a text follows is passed over, and the map styles when this build has one.
  GString *expected = g_string_new(NULL);
  g_string_append_printf(expected, "arrange.height@%u;", (unsigned)DT_CANVAS_OBJECT_TEXT);
  if(dt_canvas_map_source_count() < 2)
    g_string_append_printf(expected, "map.style@%u;", (unsigned)DT_CANVAS_OBJECT_MAP);
  // The rectangle is the only geometry this build offers, so its row has nothing to round-trip
  // through; the line goes when the polygon and the star join it. It comes last because the kinds
  // are walked in `_kinds` order and the shape is the last of them.
  g_string_append_printf(expected, "shape.geometry@%u;", (unsigned)DT_CANVAS_OBJECT_SHAPE);
  fprintf(stderr, "round trip: %d writes, skipped %s, excused %s\n", written, skipped->str, excused->str);
  assert_string_equal(skipped->str, expected->str);
  // Only a text frame's centre is ever excused, and only when its refit moved it.
  gchar **excuses = g_strsplit(excused->str, ";", -1);
  for(int idx = 0; !IS_NULL_PTR(excuses[idx]); idx++)
  {
    if(excuses[idx][0] == '\0') continue;
    gchar *text_centre = g_strdup_printf("arrange.y@%u", (unsigned)DT_CANVAS_OBJECT_TEXT);
    assert_string_equal(excuses[idx], text_centre);
    g_free(text_centre);
  }
  g_strfreev(excuses);
  g_string_free(expected, TRUE);
  g_string_free(excused, TRUE);
  g_string_free(skipped, TRUE);
  _fixture_free(&fixture);
}

static void _a_shadow_offset_edited_while_inheriting_is_kept_and_owned(void **state)
{
  (void)state;
  // The floating bar these properties replaced threw this edit away whenever the blur read
  // "default": the offset was written into a shadow the object did not own, so nothing drawn changed.
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *image = fixture.objects[1];
  assert_int_equal(dt_canvas_group_state(fixture.canvas, image, DT_CANVAS_GROUP_SHADOW), DT_CANVAS_OWN_INHERIT);
  const dt_canvas_prop_value_t offset = _number(20.0);
  const uint32_t effects = dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_SHADOW_OFFSET_X, &offset);
  assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
  assert_true(image->flags & DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE);
  dt_canvas_shadow_t drawn;
  dt_canvas_object_effective_shadow(fixture.canvas, image, &drawn);
  assert_float_equal(drawn.offset_x, 20.0, 1e-6);
  // The rest of the shadow is what the canvas gave it, not zeros.
  assert_float_equal(drawn.offset_y, fixture.canvas->shadow.offset_y, 1e-6);
  assert_float_equal(drawn.blur, fixture.canvas->shadow.blur, 1e-6);
  assert_float_equal(drawn.color.alpha, fixture.canvas->shadow.color.alpha, 1e-6);
  _fixture_free(&fixture);
}

static void _a_border_colour_keeps_the_effective_width(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *image = fixture.objects[1];
  // The frame's own fields still hold the white, 3-point border it was born with a copy of; the
  // colour it is DRAWN with is the canvas's.
  assert_float_equal(image->border_width, 3.0, 1e-6);
  dt_canvas_prop_value_t drawn;
  dt_canvas_prop_read(fixture.canvas, image, DT_CANVAS_PROP_BORDER_COLOR, &drawn);
  assert_float_equal(drawn.color.red, CANVAS_BORDER_RED, 1e-6);
  assert_float_equal(drawn.color.green, CANVAS_BORDER_GREEN, 1e-6);
  // Reselecting the colour the canvas gives is not an edit, and must not make the object own its
  // border behind the user's back.
  const dt_canvas_prop_value_t canvas_blue
      = _color(CANVAS_BORDER_RED, CANVAS_BORDER_GREEN, CANVAS_BORDER_BLUE, 1.0f);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_BORDER_COLOR, &canvas_blue), 0);
  assert_false(image->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE);
  // The white in its own field is not what is drawn, so choosing it is a real colour: it owns the
  // border, and keeps the width that was drawn -- the canvas's, not the copy.
  const dt_canvas_prop_value_t white = _color(1.0f, 1.0f, 1.0f, 1.0f);
  assert_true(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_BORDER_COLOR, &white)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(image->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE);
  assert_float_equal(image->border_width, CANVAS_BORDER_WIDTH, 1e-6);
  assert_float_equal(image->border_color.green, 1.0, 1e-6);

  // A width edited while inheriting keeps the colour that was drawn, and is owned with it.
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_value_t thick = _number(9.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_BORDER_WIDTH, &thick)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(text->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE);
  assert_float_equal(text->border_width, 9.0, 1e-6);
  assert_float_equal(text->border_color.red, CANVAS_BORDER_RED, 1e-6);
  assert_float_equal(text->border_color.green, CANVAS_BORDER_GREEN, 1e-6);
  assert_float_equal(text->border_color.blue, CANVAS_BORDER_BLUE, 1e-6);
  _fixture_free(&fixture);
}

static void _a_blur_of_minus_one_round_trips(void **state)
{
  (void)state;
  // It used to be the sentinel for "the canvas's shadow", so a small inner shadow could not be typed.
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_value_t blur = _number(-1.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_SHADOW_BLUR, &blur)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(_read_number(fixture.canvas, text, DT_CANVAS_PROP_SHADOW_BLUR), -1.0, 1e-6);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, text, DT_CANVAS_GROUP_SHADOW), DT_CANVAS_OWN_CUSTOM);
  _fixture_free(&fixture);
}

static void _a_fresh_drawing_owns_only_what_its_kind_is_born_with(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *drawing = fixture.objects[3];
  assert_int_equal(dt_canvas_group_state(fixture.canvas, drawing, DT_CANVAS_GROUP_BORDER),
                   DT_CANVAS_OWN_KIND_DEFAULT);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, drawing, DT_CANVAS_GROUP_SHADOW),
                   DT_CANVAS_OWN_KIND_DEFAULT);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, drawing, DT_CANVAS_GROUP_CORNER), DT_CANVAS_OWN_INHERIT);
  char summary[128];
  dt_canvas_group_summary(fixture.canvas, drawing, DT_CANVAS_GROUP_BORDER, summary, sizeof(summary));
  assert_string_equal(summary, "none (drawing)");
  const dt_canvas_prop_value_t width = _number(2.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, drawing, DT_CANVAS_PROP_BORDER_WIDTH, &width)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, drawing, DT_CANVAS_GROUP_BORDER), DT_CANVAS_OWN_CUSTOM);
  // A picture inherits, and says so.
  dt_canvas_group_summary(fixture.canvas, fixture.objects[1], DT_CANVAS_GROUP_BORDER, summary, sizeof(summary));
  assert_non_null(strstr(summary, "canvas default"));
  _fixture_free(&fixture);
}

static void _an_all_zero_inset_stays_zero(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_id_t sides[4] = { DT_CANVAS_PROP_TEXT_INSET_TOP, DT_CANVAS_PROP_TEXT_INSET_RIGHT,
                                         DT_CANVAS_PROP_TEXT_INSET_BOTTOM, DT_CANVAS_PROP_TEXT_INSET_LEFT };
  const dt_canvas_prop_value_t zero = _number(0.0);
  // Side by side, the way a person takes four margins to nothing.
  for(int side = 0; side < 4; side++)
    assert_true(dt_canvas_prop_write(fixture.canvas, text, sides[side], &zero) & DT_CANVAS_EFFECT_CHANGED);
  double margins[4];
  dt_canvas_text_margins(text, margins);
  // Four zeros are what "unset" looks like, and unset used to bring the birth inset back.
  for(int side = 0; side < 4; side++)
    assert_float_equal(margins[side], 0.0, 1e-9);

  // The uniform inset reaches the same zero in one go.
  dt_canvas_object_t *other = dt_canvas_add_text(fixture.canvas, 0.0, -400.0, 200.0, 80.0, "x");
  assert_true(dt_canvas_prop_write(fixture.canvas, other, DT_CANVAS_PROP_TEXT_INSET, &zero)
              & DT_CANVAS_EFFECT_CHANGED);
  dt_canvas_text_margins(other, margins);
  for(int side = 0; side < 4; side++)
    assert_float_equal(margins[side], 0.0, 1e-9);
  _fixture_free(&fixture);
}

static void _the_uniform_inset_writes_all_four_even_when_the_top_agrees(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_value_t wide = _number(30.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_INSET_RIGHT, &wide)
              & DT_CANVAS_EFFECT_CHANGED);
  // Each side's property is that side, the one the record stores under its own index.
  const dt_canvas_prop_id_t side_props[4] = { DT_CANVAS_PROP_TEXT_INSET_TOP, DT_CANVAS_PROP_TEXT_INSET_RIGHT,
                                              DT_CANVAS_PROP_TEXT_INSET_BOTTOM, DT_CANVAS_PROP_TEXT_INSET_LEFT };
  const int side_indices[4]
      = { DT_CANVAS_TEXT_MARGIN_TOP, DT_CANVAS_TEXT_MARGIN_RIGHT, DT_CANVAS_TEXT_MARGIN_BOTTOM,
          DT_CANVAS_TEXT_MARGIN_LEFT };
  double margins[4];
  dt_canvas_text_margins(text, margins);
  assert_float_equal(margins[DT_CANVAS_TEXT_MARGIN_RIGHT], 30.0, 1e-6);
  assert_float_equal(margins[DT_CANVAS_TEXT_MARGIN_LEFT], 12.0, 1e-6);
  for(int side = 0; side < 4; side++)
    assert_float_equal(_read_number(fixture.canvas, text, side_props[side]), margins[side_indices[side]], 1e-6);
  // The top still reads 12, so a test on the one number would call this no edit.
  const dt_canvas_prop_value_t twelve = _number(12.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_INSET, &twelve)
              & DT_CANVAS_EFFECT_CHANGED);
  dt_canvas_text_margins(text, margins);
  for(int side = 0; side < 4; side++)
    assert_float_equal(margins[side], 12.0, 1e-6);
  assert_true(dt_canvas_prop_is_neutral(fixture.canvas, text, DT_CANVAS_PROP_TEXT_INSET));
  // Now all four agree, the same number again is no edit: the uniform inset skips the writer's
  // general test and answers for itself.
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_INSET, &twelve), 0);
  _fixture_free(&fixture);
}

static void _an_unset_line_height_reads_one_and_is_never_written_back(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  assert_float_equal(text->text.line_height, 0.0, 1e-9);
  assert_float_equal(_read_number(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LINE_HEIGHT), 1.0, 1e-9);
  const dt_canvas_prop_value_t unit_leading = _number(1.0);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LINE_HEIGHT, &unit_leading), 0);
  assert_float_equal(text->text.line_height, 0.0, 1e-9);
  const dt_canvas_prop_value_t loose = _number(1.5);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LINE_HEIGHT, &loose)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(text->text.line_height, 1.5, 1e-6);
  _fixture_free(&fixture);
}

static void _an_inherited_value_written_while_inheriting_changes_nothing(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *image = fixture.objects[1];
  const dt_canvas_prop_value_t border = _number(CANVAS_BORDER_WIDTH);
  const dt_canvas_prop_value_t radius = _number(6.0);
  const dt_canvas_prop_value_t blur = _number(10.0);
  // A frontend resetting a control to the canvas's value must leave the object inheriting.
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_BORDER_WIDTH, &border), 0);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CORNER_RADIUS, &radius), 0);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_SHADOW_BLUR, &blur), 0);
  assert_int_equal(image->flags & (DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE
                                   | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE),
                   0);
  // So a later change of the canvas's default still reaches it.
  fixture.canvas->border_width = 5.0f;
  assert_float_equal(_read_number(fixture.canvas, image, DT_CANVAS_PROP_BORDER_WIDTH), 5.0, 1e-6);
  // A different value is an edit, and owns the group.
  const dt_canvas_prop_value_t thicker = _number(7.0);
  const uint32_t effects = dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_BORDER_WIDTH, &thicker);
  assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
  assert_true(effects & DT_CANVAS_EFFECT_SETTLE_ALL);
  assert_true(image->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE);

  // An inherited value is compared at the precision its row shows, the one a reset rounds to:
  // a border a hair off the canvas's, while inheriting, is still the canvas's border.
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_value_t rounded = _number(5.04);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_BORDER_WIDTH, &rounded), 0);
  assert_false(text->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE);
  // Once the value is the frame's own, the same hair is an edit.
  assert_true(dt_canvas_group_set_own(fixture.canvas, text, DT_CANVAS_GROUP_BORDER, TRUE)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_BORDER_WIDTH, &rounded)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(text->border_width, 5.04, 1e-6);
  _fixture_free(&fixture);
}

static void _owning_first_keeps_a_value_equal_to_the_canvas(void **state)
{
  (void)state;
  // A section's own switch turned on, then the canvas's own number typed -- a square corner, the
  // default everywhere -- owns the group first and writes second. The number must stick when the
  // canvas changes later, or the switch meant nothing.
  props_fixture_t fixture;
  _fixture_build(&fixture);
  fixture.canvas->corner_radius = 0.0f;
  dt_canvas_object_t *image = fixture.objects[1];
  const dt_canvas_prop_value_t square = _number(0.0);
  // Written alone it is the inherited value, and changes nothing.
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CORNER_RADIUS, &square), 0);
  assert_false(image->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE);
  // Owned first, the write that follows changes nothing more, and leaves the group owned.
  uint32_t effects = dt_canvas_group_set_own(fixture.canvas, image, DT_CANVAS_GROUP_CORNER, TRUE);
  effects |= dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CORNER_RADIUS, &square);
  assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
  assert_true(image->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE);
  fixture.canvas->corner_radius = 20.0f;
  assert_float_equal(_read_number(fixture.canvas, image, DT_CANVAS_PROP_CORNER_RADIUS), 0.0, 1e-9);
  _fixture_free(&fixture);
}

static void _a_typed_number_snaps_a_dragged_one(void **state)
{
  (void)state;
  // A spin shows 100 for a frame dragged to 100.4; typing 100 there is how two frames are set
  // level, and must not be refused for showing the same digits.
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *image = fixture.objects[1];
  image->x = 100.4;
  const dt_canvas_prop_value_t level = _number(100.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_X, &level) & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(image->x, 100.0, 1e-9);
  // An opacity the wheel left between two whole percents is snapped the same way.
  image->transparency = 0.276f;
  const dt_canvas_prop_value_t whole = _number(72.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_OPACITY, &whole)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(image->transparency, 0.28, 1e-6);
  // A float's own rounding is not an edit: the value stored reads back and writes as nothing.
  const dt_canvas_prop_value_t stored = _number(_read_number(fixture.canvas, image, DT_CANVAS_PROP_OPACITY));
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_OPACITY, &stored), 0);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_OPACITY, &whole), 0);
  _fixture_free(&fixture);
}

static void _a_kept_ratio_answers_a_width_with_a_height(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *image = fixture.objects[1];
  image->width = 300.0;
  image->height = 200.0;
  const dt_canvas_prop_value_t wider = _number(600.0);
  const uint32_t effects = dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_WIDTH, &wider);
  assert_true(effects & DT_CANVAS_EFFECT_COUPLED);
  assert_float_equal(image->width, 600.0, 1e-9);
  assert_float_equal(image->height, 400.0, 1e-9);
  // Set free, the other side stays where it is.
  const dt_canvas_prop_value_t free_ratio = _flag(FALSE);
  dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_KEEP_RATIO, &free_ratio);
  assert_true(image->flags & DT_CANVAS_OBJECT_FLAG_FREE_RATIO);
  const dt_canvas_prop_value_t taller = _number(500.0);
  assert_false(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_HEIGHT, &taller)
               & DT_CANVAS_EFFECT_COUPLED);
  assert_float_equal(image->width, 600.0, 1e-9);
  assert_float_equal(image->height, 500.0, 1e-9);
  _fixture_free(&fixture);
}

/**
 * An ellipse's Size scales the whole ellipse and Size Y alone changes its proportions. The context
 * menu's Size slider did that, and when the cutout sliders left the menu nothing else could: a handle
 * drags one radius and the wheel sets the feather.
 */
static void _an_ellipse_size_keeps_its_proportions(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *image = fixture.objects[1];
  dt_canvas_prop_value_t shape;
  memset(&shape, 0, sizeof(shape));
  shape.choice = DT_CANVAS_MASK_ELLIPSE;
  dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CUTOUT_SHAPE, &shape);
  image->mask.radius_x = 0.2f;
  image->mask.radius_y = 0.3f;
  // Twice as large: both radii double, and the vertical one is a coupled row to refill.
  const dt_canvas_prop_value_t larger = _number(40.0);
  const uint32_t effects = dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CUTOUT_SIZE_X, &larger);
  assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
  assert_true(effects & DT_CANVAS_EFFECT_COUPLED);
  assert_float_equal(image->mask.radius_x, 0.4, 1e-6);
  assert_float_equal(image->mask.radius_y, 0.6, 1e-6);
  // A drag of the slider is a run of LIVE writes: the proportions hold across the steps, not only for one.
  const double steps[3] = { 33.0, 71.5, 25.0 };
  for(int idx = 0; idx < 3; idx++)
  {
    const dt_canvas_prop_value_t step = _number(steps[idx]);
    dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CUTOUT_SIZE_X, &step);
  }
  assert_float_equal(image->mask.radius_x, 0.25, 1e-6);
  assert_float_equal(image->mask.radius_y, 0.375, 1e-5);
  // Size Y moves the vertical radius alone.
  const dt_canvas_prop_value_t taller = _number(50.0);
  assert_false(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CUTOUT_SIZE_Y, &taller)
               & DT_CANVAS_EFFECT_COUPLED);
  assert_float_equal(image->mask.radius_x, 0.25, 1e-6);
  assert_float_equal(image->mask.radius_y, 0.5, 1e-6);
  // A circle has no second radius to scale, and a gradient's size is its extent, which leaves the
  // curvature kept in the same field alone.
  const uint32_t others[2] = { DT_CANVAS_MASK_CIRCLE, DT_CANVAS_MASK_GRADIENT };
  for(int idx = 0; idx < 2; idx++)
  {
    shape.choice = (int)others[idx];
    dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CUTOUT_SHAPE, &shape);
    image->mask.radius_x = 0.2f;
    image->mask.radius_y = 0.3f;
    const dt_canvas_prop_value_t size = _number(10.0);
    assert_false(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_CUTOUT_SIZE_X, &size)
                 & DT_CANVAS_EFFECT_COUPLED);
    assert_float_equal(image->mask.radius_x, 0.1, 1e-6);
    assert_float_equal(image->mask.radius_y, 0.3, 1e-6);
  }
  _fixture_free(&fixture);
}

#define MAP_CONF_SENTINEL "7"

static void _map_properties_ask_for_a_render_and_never_touch_conf(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *map = fixture.objects[2];
  const char *const conf_keys[4]
      = { "canvas/map_zoom", "canvas/map_latitude", "canvas/map_longitude", "canvas/map_source" };
  for(int idx = 0; idx < 4; idx++)
    dt_conf_set_string(conf_keys[idx], MAP_CONF_SENTINEL);
  const dt_canvas_prop_value_t zoom = _number(15.0);
  const dt_canvas_prop_value_t latitude = _number(48.8);
  const dt_canvas_prop_value_t longitude = _number(2.35);
  const uint32_t wanted = DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_COMMIT_RENDER | DT_CANVAS_EFFECT_COMMIT_CONF;
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_MAP_ZOOM, &zoom) & wanted, wanted);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_MAP_LATITUDE, &latitude) & wanted,
                   wanted);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_MAP_LONGITUDE, &longitude) & wanted,
                   wanted);
  assert_int_equal(map->map.zoom, 15);
  // Another style is new tiles, and the next map's default, like any of the map's own settings.
  const int styles = dt_canvas_map_source_count();
  fprintf(stderr, "map styles in this build: %d\n", styles);
  if(styles >= 2)
  {
    dt_canvas_prop_value_t style;
    dt_canvas_prop_read(fixture.canvas, map, DT_CANVAS_PROP_MAP_STYLE, &style);
    style.choice = (style.choice + 1) % styles;
    assert_int_equal(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_MAP_STYLE, &style) & wanted, wanted);
  }
  // The configuration is the caller's: the writer only says it is owed.
  for(int idx = 0; idx < 4; idx++)
    assert_string_equal(dt_conf_get_string_const(conf_keys[idx]), MAP_CONF_SENTINEL);
  // A new size wants new tiles, whichever side is typed; a move does not.
  const dt_canvas_prop_value_t width = _number(400.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_WIDTH, &width)
              & DT_CANVAS_EFFECT_COMMIT_RENDER);
  const dt_canvas_prop_value_t height = _number(250.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_HEIGHT, &height)
              & DT_CANVAS_EFFECT_COMMIT_RENDER);
  const dt_canvas_prop_value_t moved = _number(123.0);
  assert_false(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_X, &moved)
               & DT_CANVAS_EFFECT_COMMIT_RENDER);
  // Fetching again changes nothing to undo.
  const dt_canvas_prop_value_t nothing = _number(0.0);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, map, DT_CANVAS_PROP_MAP_FETCH, &nothing),
                   DT_CANVAS_EFFECT_COMMIT_RENDER);
  _fixture_free(&fixture);
}

static void _owning_a_group_changes_nothing_on_screen(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  const dt_canvas_prop_group_t groups[] = { DT_CANVAS_GROUP_BORDER, DT_CANVAS_GROUP_CORNER, DT_CANVAS_GROUP_SHADOW,
                                            DT_CANVAS_GROUP_FONT };
  dt_canvas_object_t *text = fixture.objects[0];
  for(size_t group_index = 0; group_index < G_N_ELEMENTS(groups); group_index++)
  {
    const dt_canvas_prop_group_t group = groups[group_index];
    assert_int_equal(dt_canvas_group_state(fixture.canvas, text, group), DT_CANVAS_OWN_INHERIT);
    dt_canvas_prop_value_t before[DT_CANVAS_PROP_COUNT];
    for(size_t idx = 0; idx < count; idx++)
      if(table[idx].group == group) dt_canvas_prop_read(fixture.canvas, text, table[idx].id, &before[idx]);
    assert_true(dt_canvas_group_set_own(fixture.canvas, text, group, TRUE) & DT_CANVAS_EFFECT_CHANGED);
    assert_int_equal(dt_canvas_group_set_own(fixture.canvas, text, group, TRUE), 0);
    assert_int_equal(dt_canvas_group_state(fixture.canvas, text, group), DT_CANVAS_OWN_CUSTOM);
    if(group == DT_CANVAS_GROUP_BORDER)
    {
      // Seeded from the canvas, both fields: not left at the copy the frame was born with.
      assert_float_equal(text->border_width, CANVAS_BORDER_WIDTH, 1e-6);
      assert_float_equal(text->border_color.red, CANVAS_BORDER_RED, 1e-6);
      assert_float_equal(text->border_color.green, CANVAS_BORDER_GREEN, 1e-6);
      assert_float_equal(text->border_color.blue, CANVAS_BORDER_BLUE, 1e-6);
    }
    for(size_t idx = 0; idx < count; idx++)
    {
      if(table[idx].group != group) continue;
      dt_canvas_prop_value_t after;
      dt_canvas_prop_read(fixture.canvas, text, table[idx].id, &after);
      assert_memory_equal(&before[idx], &after, sizeof(after));
    }
    assert_true(dt_canvas_group_set_own(fixture.canvas, text, group, FALSE) & DT_CANVAS_EFFECT_CHANGED);
    assert_int_equal(dt_canvas_group_state(fixture.canvas, text, group), DT_CANVAS_OWN_INHERIT);
  }
  // Giving the border back keeps its fields: they are what owning it again would find.
  dt_canvas_group_set_own(fixture.canvas, text, DT_CANVAS_GROUP_BORDER, TRUE);
  text->border_width = 42.0f;
  assert_true(dt_canvas_group_set_own(fixture.canvas, text, DT_CANVAS_GROUP_BORDER, FALSE)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(text->border_width, 42.0, 1e-6);
  assert_float_equal(_read_number(fixture.canvas, text, DT_CANVAS_PROP_BORDER_WIDTH), CANVAS_BORDER_WIDTH, 1e-6);
  _fixture_free(&fixture);
}

/**
 * What a control inside an override group resets to: the value the object shows once the group is
 * given back. Checked on an object that owns every group with values of its own, so a reader that
 * answered with the object's fields instead of the canvas's shows.
 */
static void _the_inherited_value_is_what_giving_the_group_back_shows(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  fixture.canvas->shadow.offset_x = 3.0f;
  fixture.canvas->shadow.offset_y = -2.0f;
  fixture.canvas->shadow.color = dt_canvas_color(0.2f, 0.3f, 0.4f, 0.5f);
  g_strlcpy(fixture.canvas->default_font, "Serif Bold 17", sizeof(fixture.canvas->default_font));
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(int object_index = 0; object_index < 6; object_index++)
  {
    dt_canvas_object_t *object = fixture.objects[object_index];
    const dt_canvas_prop_group_t groups[] = { DT_CANVAS_GROUP_BORDER, DT_CANVAS_GROUP_CORNER,
                                              DT_CANVAS_GROUP_SHADOW, DT_CANVAS_GROUP_FONT };
    for(size_t group_index = 0; group_index < G_N_ELEMENTS(groups); group_index++)
    {
      const dt_canvas_prop_group_t group = groups[group_index];
      dt_canvas_group_set_own(fixture.canvas, object, group, TRUE);
      // Values of the object's own, unlike the canvas's in every field.
      object->border_width = 21.0f;
      object->border_color = dt_canvas_color(0.9f, 0.1f, 0.1f, 1.0f);
      object->corner_radius = 33.0f;
      object->shadow.offset_x = 11.0f;
      object->shadow.offset_y = 12.0f;
      object->shadow.blur = -13.0f;
      object->shadow.color = dt_canvas_color(0.7f, 0.7f, 0.1f, 0.9f);
      if(object->kind == DT_CANVAS_OBJECT_TEXT)
        g_strlcpy(object->text.font, "Monospace Italic 41", sizeof(object->text.font));
      for(size_t idx = 0; idx < count; idx++)
      {
        const dt_canvas_prop_t *prop = &table[idx];
        if(prop->group != group || !dt_canvas_prop_for_kind(prop, object->kind)) continue;
        dt_canvas_prop_value_t inherited;
        dt_canvas_prop_read_inherited(fixture.canvas, object, prop->id, &inherited);
        dt_canvas_prop_value_t owned;
        dt_canvas_prop_read(fixture.canvas, object, prop->id, &owned);
        assert_true(memcmp(&inherited, &owned, sizeof(owned)) != 0);
        dt_canvas_object_t given_back = *object;
        dt_canvas_group_set_own(fixture.canvas, &given_back, group, FALSE);
        dt_canvas_prop_value_t shown;
        dt_canvas_prop_read(fixture.canvas, &given_back, prop->id, &shown);
        assert_memory_equal(&inherited, &shown, sizeof(shown));
      }
    }
  }
  // Outside any group there is nothing to fall back on: the value read is the value.
  dt_canvas_prop_value_t opacity_inherited;
  dt_canvas_prop_read_inherited(fixture.canvas, fixture.objects[1], DT_CANVAS_PROP_OPACITY, &opacity_inherited);
  dt_canvas_prop_value_t opacity;
  dt_canvas_prop_read(fixture.canvas, fixture.objects[1], DT_CANVAS_PROP_OPACITY, &opacity);
  assert_memory_equal(&opacity_inherited, &opacity, sizeof(opacity));
  _fixture_free(&fixture);
}

static void _giving_the_font_back_refits_the_frame(void **state)
{
  (void)state;
  // Another face is another height for a frame that follows its text, and other features to
  // offer: giving the font back owes both, exactly as writing the canvas's font would.
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_value_t large = _text("Serif 48");
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_FONT, &large)
              & DT_CANVAS_EFFECT_CHANGED);
  const dt_canvas_prop_value_t switched_on = _flag(TRUE);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_AUTO_HEIGHT, &switched_on)
              & DT_CANVAS_EFFECT_CHANGED);
  const double tall = text->height;
  const uint32_t effects = dt_canvas_group_set_own(fixture.canvas, text, DT_CANVAS_GROUP_FONT, FALSE);
  fprintf(stderr, "font given back: height %f -> %f, effects 0x%x\n", tall, text->height, effects);
  assert_string_equal(text->text.font, "");
  assert_true(text->height < tall);
  const uint32_t owed = DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_RESTRUCTURE | DT_CANVAS_EFFECT_COUPLED
                        | DT_CANVAS_EFFECT_SETTLE_ALL;
  assert_int_equal(effects & owed, owed);
  _fixture_free(&fixture);
}

static void _a_size_on_an_inheriting_font_writes_the_family_out(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  g_strlcpy(fixture.canvas->default_font, "Serif Italic 12", sizeof(fixture.canvas->default_font));
  assert_string_equal(text->text.font, "");
  assert_float_equal(_read_number(fixture.canvas, text, DT_CANVAS_PROP_TEXT_SIZE), 12.0, 1e-6);
  const dt_canvas_prop_value_t twenty = _number(20.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_SIZE, &twenty)
              & DT_CANVAS_EFFECT_CHANGED);
  PangoFontDescription *stored = pango_font_description_from_string(text->text.font);
  assert_string_equal(pango_font_description_get_family(stored), "Serif");
  assert_int_equal(pango_font_description_get_style(stored), PANGO_STYLE_ITALIC);
  assert_int_equal(pango_font_description_get_size(stored), 20 * PANGO_SCALE);
  pango_font_description_free(stored);
  // Back at the canvas's size it IS the canvas's font again, and is stored as none.
  const dt_canvas_prop_value_t twelve = _number(12.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_SIZE, &twelve)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_string_equal(text->text.font, "");

  // A chooser that offers only a family and a style keeps the size the text is set at.
  g_strlcpy(text->text.font, "Serif 20", sizeof(text->text.font));
  const dt_canvas_prop_value_t face = _text("Sans Bold");
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_FONT, &face)
              & DT_CANVAS_EFFECT_RESTRUCTURE);
  assert_string_equal(text->text.font, "Sans Bold 20");
  // And the canvas's own font, reselected, is none of the frame's own.
  const dt_canvas_prop_value_t canvas_font = _text("Serif Italic 12");
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_FONT, &canvas_font)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_string_equal(text->text.font, "");
  _fixture_free(&fixture);
}

/**
 * cmocka's float and double comparisons both pass anything within FLT_EPSILON of the larger
 * value, whatever tolerance they are handed -- 3.6e-5 at 300 units, measured -- so a check that
 * means a tighter tolerance than that has to say so itself.
 */
#define assert_near(actual, expected, tolerance)                                                                 \
  do                                                                                                            \
  {                                                                                                             \
    const double near_actual = (double)(actual);                                                                \
    const double near_expected = (double)(expected);                                                            \
    const double near_tolerance = (double)(tolerance);                                                          \
    if(!(fabs(near_actual - near_expected) <= near_tolerance))                                                  \
      fail_msg("%s is %.17g, expected %.17g within %g", #actual, near_actual, near_expected, near_tolerance);    \
  } while(0)

/**
 * Reverses a connector and measures how far its new route strays from the one it had, walked from
 * the other end: each point against its mirror. The point counts must agree.
 * @return the largest gap along either axis, in canvas units.
 */
static double _reverse_largest_mirror_gap(dt_canvas_t *canvas, dt_canvas_object_t *connector)
{
  dt_canvas_route_t before;
  assert_true(dt_canvas_connector_route(canvas, connector, &before));
  const dt_canvas_prop_value_t nothing = _number(0.0);
  assert_true(dt_canvas_prop_write(canvas, connector, DT_CANVAS_PROP_CONNECTOR_REVERSE, &nothing)
              & DT_CANVAS_EFFECT_CHANGED);
  dt_canvas_route_t after;
  assert_true(dt_canvas_connector_route(canvas, connector, &after));
  assert_int_equal(after.point_count, before.point_count);
  double largest_gap = 0.0;
  for(int idx = 0; idx < before.point_count; idx++)
  {
    const int mirrored = before.point_count - 1 - idx;
    largest_gap = fmax(largest_gap, fabs(after.points[2 * idx] - before.points[2 * mirrored]));
    largest_gap = fmax(largest_gap, fabs(after.points[2 * idx + 1] - before.points[2 * mirrored + 1]));
  }
  return largest_gap;
}

/** Reverses a connector and fails, naming the gap, unless its route is the same one walked back. */
static void _assert_reverse_walks_back(dt_canvas_t *canvas, dt_canvas_object_t *connector)
{
  const double largest_gap = _reverse_largest_mirror_gap(canvas, connector);
  if(largest_gap > 1e-9) fail_msg("the reversed route strays %g units from the mirror of the old one", largest_gap);
}

/**
 * Reversing a connector swaps everything its ends are made of, so the curve is the same curve
 * walked the other way. Each case below is steered by one thing an id swap alone leaves behind:
 * the reaches of the handles at an anchored end, a free end's point and tangent, and the
 * waypoint's tangent, which points toward the end and so has to turn round with it.
 */
static void _reversing_a_connector_walks_the_same_curve_backwards(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);

  // A free cubic, bent by one end's tangent and by the seed at the other.
  dt_canvas_object_t *line
      = dt_canvas_add_line(fixture.canvas, -80.0, 30.0, 220.0, -45.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  line->connector.to_tangent_x = -15.0f;
  line->connector.to_tangent_y = 90.0f;
  const float from_tangent_x = line->connector.from_tangent_x;
  _assert_reverse_walks_back(fixture.canvas, line);
  assert_true(line->connector.from_x == 220.0);
  assert_true(line->connector.to_y == 30.0);
  assert_true(line->connector.from_tangent_x == -15.0f);
  assert_true(line->connector.to_tangent_x == from_tangent_x);

  // The same free cubic through a waypoint whose tangent was dragged off the chord.
  line->connector.via_count = 1;
  line->connector.via_x = 60.0;
  line->connector.via_y = 140.0;
  line->connector.via_tangent_x = 70.0;
  line->connector.via_tangent_y = -25.0;
  _assert_reverse_walks_back(fixture.canvas, line);
  assert_true(line->connector.via_tangent_x == -70.0);
  assert_true(line->connector.via_tangent_y == 25.0);

  // A steered cubic between two frames: each end's handle dragged to a length of its own, far
  // enough apart that a reach left at its old end bends the curve visibly.
  dt_canvas_object_t *steered = fixture.objects[4];
  steered->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  steered->connector.from_anchor = DT_CANVAS_ANCHOR_EAST;
  steered->connector.to_anchor = DT_CANVAS_ANCHOR_NORTH;
  steered->connector.from_reach = 25.0f;
  steered->connector.to_reach = 180.0f;
  _assert_reverse_walks_back(fixture.canvas, steered);
  assert_true(steered->connector.from_reach == 180.0f);
  assert_true(steered->connector.to_reach == 25.0f);

  // An anchored cubic through a waypoint with a dragged tangent. The reaches are left automatic,
  // so only the tangent's turn is on trial here; the automatic tangent is the next case's.
  dt_canvas_object_t *through
      = dt_canvas_add_connector(fixture.canvas, fixture.objects[0]->id, fixture.objects[3]->id);
  assert_non_null(through);
  through->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  through->connector.via_count = 1;
  through->connector.via_x = 150.0;
  through->connector.via_y = 900.0;
  through->connector.via_tangent_x = 120.0;
  through->connector.via_tangent_y = 45.0;
  _assert_reverse_walks_back(fixture.canvas, through);
  assert_true(through->connector.via_tangent_x == -120.0);

  // The same connector with the waypoint's tangent left automatic. Between two frames its length is
  // 0.4 of the leg that leaves the START, floored at 40 units, so the curve is the same walked back
  // only where both legs are as long. Which leg leaves the start is exactly what Reverse changes,
  // and nothing it writes can make that length not care: the routing would have to, and that moves
  // every anchored cubic through an automatic waypoint in every existing document, which the golden
  // routes in test_canvas_document pin. It is left for a decision, and both halves are pinned here.
  // A waypoint on the perpendicular bisector of the two ends, far enough out that neither leg sits
  // on the floor, walks back exactly...
  through->connector.via_tangent_x = 0.0;
  through->connector.via_tangent_y = 0.0;
  dt_canvas_route_t ends;
  assert_true(dt_canvas_connector_route(fixture.canvas, through, &ends));
  const double chord_x = ends.to_x - ends.from_x;
  const double chord_y = ends.to_y - ends.from_y;
  const double chord = hypot(chord_x, chord_y);
  assert_true(chord > 100.0);
  through->connector.via_x = 0.5 * (ends.from_x + ends.to_x) - 300.0 * chord_y / chord;
  through->connector.via_y = 0.5 * (ends.from_y + ends.to_y) + 300.0 * chord_x / chord;
  _assert_reverse_walks_back(fixture.canvas, through);
  // ...and the same waypoint slid toward one end does not. Should this start failing, the routing
  // has been made symmetric on purpose: drop this half and let the one above take any waypoint.
  through->connector.via_x += 0.3 * chord_x;
  through->connector.via_y += 0.3 * chord_y;
  assert_true(_reverse_largest_mirror_gap(fixture.canvas, through) > 10.0);

  // Half free: the frame's end and the free end change places, and the straight chord with them.
  dt_canvas_object_t *frame = fixture.objects[0];
  dt_canvas_object_t *half
      = dt_canvas_add_line(fixture.canvas, 0.0, 0.0, 900.0, 900.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  half->connector.from_id = frame->id;
  dt_canvas_route_t before;
  assert_true(dt_canvas_connector_route(fixture.canvas, half, &before));
  _assert_reverse_walks_back(fixture.canvas, half);
  assert_int_equal(half->connector.from_id, 0);
  assert_int_equal(half->connector.to_id, frame->id);
  assert_true(half->connector.from_x == 900.0);
  dt_canvas_route_t after;
  assert_true(dt_canvas_connector_route(fixture.canvas, half, &after));
  assert_near(after.from_x, before.to_x, 1e-9);
  assert_near(after.from_y, before.to_y, 1e-9);
  assert_near(after.to_x, before.from_x, 1e-9);
  assert_near(after.to_y, before.from_y, 1e-9);
  _fixture_free(&fixture);
}

/**
 * A free line made cubic in its properties is bent into the seeded arc: left with automatic tangents
 * its ends would aim at each other and it would stay as straight as it was. The arc is worked out on
 * paper -- 30 degrees off a 300-unit chord, at 0.4 of it -- and a bend the user already gave the
 * line survives being made straight and cubic again. A connector between frames, and a line
 * through a waypoint, are left as they are.
 */
static void _making_a_line_cubic_bends_it_into_an_arc(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  const dt_canvas_prop_value_t cubic = _choice(DT_CANVAS_ROUTING_CUBIC);
  const dt_canvas_prop_value_t straight = _choice(DT_CANVAS_ROUTING_STRAIGHT);
  dt_canvas_object_t *line
      = dt_canvas_add_line(fixture.canvas, 100.0, 50.0, 400.0, 50.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  assert_true(line->connector.from_tangent_x == 0.0f && line->connector.to_tangent_y == 0.0f);
  assert_true(dt_canvas_prop_write(fixture.canvas, line, DT_CANVAS_PROP_CONNECTOR_ROUTING, &cubic)
              & DT_CANVAS_EFFECT_CHANGED);
  const double reach = 0.4 * 300.0;
  assert_near(line->connector.from_tangent_x, reach * cos(M_PI / 6.0), 1e-4);
  assert_near(line->connector.from_tangent_y, -reach * sin(M_PI / 6.0), 1e-4);
  assert_near(line->connector.to_tangent_x, -reach * cos(M_PI / 6.0), 1e-4);
  assert_near(line->connector.to_tangent_y, -reach * sin(M_PI / 6.0), 1e-4);
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(fixture.canvas, line, &route));
  double middle_x = 0.0;
  double middle_y = 0.0;
  dt_canvas_route_midpoint(&route, &middle_x, &middle_y);
  assert_true(middle_y < 50.0 - 10.0);

  // A bend of the user's own is not re-seeded over.
  line->connector.from_tangent_x = 17.0f;
  line->connector.from_tangent_y = 90.0f;
  assert_true(dt_canvas_prop_write(fixture.canvas, line, DT_CANVAS_PROP_CONNECTOR_ROUTING, &straight)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(dt_canvas_prop_write(fixture.canvas, line, DT_CANVAS_PROP_CONNECTOR_ROUTING, &cubic)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(line->connector.from_tangent_x == 17.0f && line->connector.from_tangent_y == 90.0f);

  // Through a waypoint the waypoint bends it, and nothing is seeded.
  dt_canvas_object_t *through
      = dt_canvas_add_line(fixture.canvas, 0.0, 0.0, 300.0, 0.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  dt_canvas_connector_add_via(fixture.canvas, through);
  assert_true(dt_canvas_prop_write(fixture.canvas, through, DT_CANVAS_PROP_CONNECTOR_ROUTING, &cubic)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(through->connector.from_tangent_x == 0.0f && through->connector.to_tangent_x == 0.0f);

  // Between two frames, the normals of its anchors already bend it.
  dt_canvas_object_t *anchored = fixture.objects[4];
  anchored->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  assert_true(dt_canvas_prop_write(fixture.canvas, anchored, DT_CANVAS_PROP_CONNECTOR_ROUTING, &cubic)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(anchored->connector.from_tangent_x == 0.0f && anchored->connector.to_tangent_x == 0.0f);
  _fixture_free(&fixture);
}

/**
 * How a LINE is drawn is what the next line drawn is styled with, so the writer asks the caller to
 * remember it -- the same COMMIT_CONF a map's settings ask for. A connector holding a frame is always
 * born with the defaults, so styling one asks for nothing: both ends free is the whole test, and a
 * connector left HALF free by a hand-edited file would otherwise style every line drawn after it,
 * since that state is modelled, loads verbatim and routes. Where a line GOES is nobody's default
 * either: only its style is remembered.
 */
static void _a_line_style_is_the_next_line_s_and_a_connector_s_is_not(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *line
      = dt_canvas_add_line(fixture.canvas, 0.0, 900.0, 200.0, 900.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  assert_non_null(line);
  // One end on a frame, the other at its own point: it owns something, so it has a free end, but it
  // is not a line and its style is its frame's business.
  dt_canvas_object_t *half
      = dt_canvas_add_line(fixture.canvas, 0.0, 1200.0, 200.0, 1200.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  assert_non_null(half);
  half->connector.from_id = fixture.objects[1]->id;
  half->connector.from_anchor = DT_CANVAS_ANCHOR_CENTRE;
  assert_true(dt_canvas_connector_has_free_end(half));
  assert_false(dt_canvas_connector_is_line(half));
  dt_canvas_object_t *const styled[3] = { line, fixture.objects[4], half };
  const dt_canvas_prop_id_t style_props[5]
      = { DT_CANVAS_PROP_LINE_WIDTH, DT_CANVAS_PROP_LINE_COLOR, DT_CANVAS_PROP_LINE_DASHED,
          DT_CANVAS_PROP_CONNECTOR_ARROW_START, DT_CANVAS_PROP_CONNECTOR_ARROW_END };
  for(int idx = 0; idx < 5; idx++)
  {
    const dt_canvas_prop_id_t prop_id = style_props[idx];
    dt_canvas_prop_value_t current;
    dt_canvas_prop_value_t wanted;
    for(int object_index = 0; object_index < 3; object_index++)
    {
      dt_canvas_object_t *object = styled[object_index];
      dt_canvas_prop_read(fixture.canvas, object, prop_id, &current);
      if(prop_id == DT_CANVAS_PROP_LINE_WIDTH)
        wanted = _number(current.number + 3.0);
      else if(prop_id == DT_CANVAS_PROP_LINE_COLOR)
        wanted = _color(0.2f, 0.4f, 0.6f, 0.8f);
      else
        wanted = _flag(!current.flag);
      const uint32_t effects = dt_canvas_prop_write(fixture.canvas, object, prop_id, &wanted);
      assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
      if(object_index == 0)
        assert_true(effects & DT_CANVAS_EFFECT_COMMIT_CONF);
      else
        assert_false(effects & DT_CANVAS_EFFECT_COMMIT_CONF);
    }
  }
  // Where a line goes is its own: the route is not asked to be remembered for the next one.
  const dt_canvas_prop_value_t cubic = _choice(DT_CANVAS_ROUTING_CUBIC);
  const uint32_t routed = dt_canvas_prop_write(fixture.canvas, line, DT_CANVAS_PROP_CONNECTOR_ROUTING, &cubic);
  assert_true(routed & DT_CANVAS_EFFECT_CHANGED);
  assert_false(routed & DT_CANVAS_EFFECT_COMMIT_CONF);
  const dt_canvas_prop_value_t waypoint = _flag(TRUE);
  const uint32_t bent = dt_canvas_prop_write(fixture.canvas, line, DT_CANVAS_PROP_CONNECTOR_WAYPOINT, &waypoint);
  assert_true(bent & DT_CANVAS_EFFECT_CHANGED);
  assert_false(bent & DT_CANVAS_EFFECT_COMMIT_CONF);
  _fixture_free(&fixture);
}

static void _arrowheads_and_backgrounds_land_where_they_belong(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *connector = fixture.objects[4];
  connector->connector.style = 0;
  const dt_canvas_prop_value_t switched_on = _flag(TRUE);
  const dt_canvas_prop_value_t switched_off = _flag(FALSE);
  dt_canvas_prop_write(fixture.canvas, connector, DT_CANVAS_PROP_CONNECTOR_ARROW_END, &switched_on);
  dt_canvas_prop_write(fixture.canvas, connector, DT_CANVAS_PROP_CONNECTOR_ARROW_START, &switched_on);
  dt_canvas_prop_write(fixture.canvas, connector, DT_CANVAS_PROP_LINE_DASHED, &switched_on);
  assert_int_equal(connector->connector.style, DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_ARROW_START
                                                   | DT_CANVAS_CONNECTOR_DASHED);
  dt_canvas_prop_write(fixture.canvas, connector, DT_CANVAS_PROP_CONNECTOR_ARROW_START, &switched_off);
  assert_int_equal(connector->connector.style, DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_DASHED);
  // Reversing swaps the ends -- which frame, and where on it each end is fastened -- and keeps
  // the style. The anchors differ, so a swap that forgot them shows.
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_NORTH;
  connector->connector.to_anchor = DT_CANVAS_ANCHOR_SOUTH_WEST;
  const uint32_t from_id = connector->connector.from_id;
  const uint32_t to_id = connector->connector.to_id;
  const dt_canvas_prop_value_t nothing = _number(0.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, connector, DT_CANVAS_PROP_CONNECTOR_REVERSE, &nothing)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_int_equal(connector->connector.from_id, to_id);
  assert_int_equal(connector->connector.to_id, from_id);
  assert_int_equal(connector->connector.from_anchor, DT_CANVAS_ANCHOR_SOUTH_WEST);
  assert_int_equal(connector->connector.to_anchor, DT_CANVAS_ANCHOR_NORTH);
  assert_int_equal(connector->connector.style, DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_DASHED);

  // A text frame keeps its background in its own field; a picture in the shared one.
  const dt_canvas_prop_value_t tint = _color(0.9f, 0.8f, 0.7f, 0.5f);
  dt_canvas_prop_write(fixture.canvas, fixture.objects[0], DT_CANVAS_PROP_BACKGROUND, &tint);
  assert_float_equal(fixture.objects[0]->text.background.red, 0.9, 1e-6);
  assert_float_equal(fixture.objects[0]->background.alpha, 0.0, 1e-9);
  dt_canvas_prop_write(fixture.canvas, fixture.objects[1], DT_CANVAS_PROP_BACKGROUND, &tint);
  assert_float_equal(fixture.objects[1]->background.red, 0.9, 1e-6);
  // Opacity is stored the other way round.
  const dt_canvas_prop_value_t quarter = _number(25.0);
  dt_canvas_prop_write(fixture.canvas, fixture.objects[1], DT_CANVAS_PROP_OPACITY, &quarter);
  assert_float_equal(fixture.objects[1]->transparency, 0.75, 1e-6);
  // A kind without the property refuses it.
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, connector, DT_CANVAS_PROP_BACKGROUND, &tint), 0);
  _fixture_free(&fixture);
}

static void _rows_follow_what_they_depend_on(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  const dt_canvas_prop_t *feather = dt_canvas_prop_get(DT_CANVAS_PROP_CUTOUT_FEATHER);
  const dt_canvas_prop_t *size_y = dt_canvas_prop_get(DT_CANVAS_PROP_CUTOUT_SIZE_Y);
  const dt_canvas_prop_t *gap_row = dt_canvas_prop_get(DT_CANVAS_PROP_TEXT_WRAP_GAP);
  const dt_canvas_prop_t *height = dt_canvas_prop_get(DT_CANVAS_PROP_HEIGHT);
  assert_false(dt_canvas_prop_applies(feather, text));
  dt_canvas_prop_value_t shape;
  memset(&shape, 0, sizeof(shape));
  shape.choice = DT_CANVAS_MASK_CIRCLE;
  const uint32_t effects = dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_CUTOUT_SHAPE, &shape);
  assert_true(effects & DT_CANVAS_EFFECT_RESTRUCTURE);
  assert_true(effects & DT_CANVAS_EFFECT_VIEW);
  assert_true(dt_canvas_prop_applies(feather, text));
  assert_false(dt_canvas_prop_applies(size_y, text));
  shape.choice = DT_CANVAS_MASK_GRADIENT;
  dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_CUTOUT_SHAPE, &shape);
  assert_false(dt_canvas_prop_applies(feather, text));
  // The gap means something only while the text flows.
  assert_false(dt_canvas_prop_applies(gap_row, text));
  const dt_canvas_prop_value_t switched_on = _flag(TRUE);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_WRAP, &switched_on)
              & DT_CANVAS_EFFECT_RESTRUCTURE);
  assert_true(dt_canvas_prop_applies(gap_row, text));
  // A height that follows the text cannot be typed; a picture has no such switch.
  assert_true(dt_canvas_prop_sensitive(height, fixture.canvas, text));
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_AUTO_HEIGHT, &switched_on)
              & DT_CANVAS_EFFECT_COUPLED);
  assert_false(dt_canvas_prop_sensitive(height, fixture.canvas, text));
  assert_true(dt_canvas_prop_sensitive(height, fixture.canvas, fixture.objects[1]));
  // Sticky extras: a letter spacing somebody set is not neutral.
  assert_true(dt_canvas_prop_is_neutral(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LETTER_SPACING));
  const dt_canvas_prop_value_t tracking = _number(40.0);
  dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LETTER_SPACING, &tracking);
  assert_false(dt_canvas_prop_is_neutral(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LETTER_SPACING));
  _fixture_free(&fixture);
}

/**
 * The card's sections, as the properties binder decides them: a section is there when one of its
 * own rows applies to this object. The strip is not a section -- its rows carry no heading and
 * cannot bring one back -- so only the rows the card would hold are counted.
 */
static uint32_t _sections_present(const dt_canvas_object_t *object)
{
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  uint32_t sections = 0;
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->tier == DT_CANVAS_TIER_STRIP) continue;
    if(dt_canvas_prop_applies(prop, object)) sections |= 1u << prop->section;
  }
  return sections;
}

/** The rule the binder followed before: a section was there when the KIND owned one of its rows. */
static uint32_t _sections_for_kind(const uint32_t kind)
{
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  uint32_t sections = 0;
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->tier == DT_CANVAS_TIER_STRIP) continue;
    if(dt_canvas_prop_for_kind(prop, kind)) sections |= 1u << prop->section;
  }
  return sections;
}

/** How many of the card's rows apply right now: what tells one state of an object from another. */
static int _card_rows_applying(const dt_canvas_object_t *object)
{
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  int rows = 0;
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->tier == DT_CANVAS_TIER_STRIP) continue;
    rows += dt_canvas_prop_applies(prop, object) ? 1 : 0;
  }
  return rows;
}

/**
 * Deciding a section by what applies rather than by what the kind owns moves no section of any
 * kind there is today: every gated row sits beside an ungated one, so a section closed by a shape
 * or a switch has never been a section closed altogether. The day a kind arrives whose section is
 * gated throughout -- a rectangle has nothing to say about a polygon's sides -- this is where the
 * two rules part company, and it should be that kind that parts them, not one of these.
 */
static void _a_section_is_there_when_one_of_its_rows_applies(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  for(size_t kind_index = 0; kind_index < G_N_ELEMENTS(_kinds); kind_index++)
  {
    dt_canvas_object_t *object = fixture.objects[kind_index];
    const uint32_t owned = _sections_for_kind(_kinds[kind_index]);
    assert_true(owned != 0);
    assert_int_equal(_sections_present(object), owned);
    if(_kinds[kind_index] == DT_CANVAS_OBJECT_CONNECTOR) continue;
    // Every state the gated rows can put a frame in: the cutout's five shapes, and under each of
    // them the text's flow switch, which the other kinds refuse and stay as they were.
    int rows_uncut[2] = { 0, 0 };
    int rows_cut[2] = { 0, 0 };
    for(int flowing = 0; flowing < 2; flowing++)
    {
      const dt_canvas_prop_value_t wrap = _flag(flowing != 0);
      dt_canvas_prop_write(fixture.canvas, object, DT_CANVAS_PROP_TEXT_WRAP, &wrap);
      for(int choice = DT_CANVAS_MASK_NONE; choice <= DT_CANVAS_MASK_GRADIENT; choice++)
      {
        const dt_canvas_prop_value_t shape = _choice(choice);
        dt_canvas_prop_write(fixture.canvas, object, DT_CANVAS_PROP_CUTOUT_SHAPE, &shape);
        assert_int_equal(_sections_present(object), owned);
        if(choice == DT_CANVAS_MASK_NONE) rows_uncut[flowing] = _card_rows_applying(object);
        if(choice == DT_CANVAS_MASK_ELLIPSE) rows_cut[flowing] = _card_rows_applying(object);
      }
    }
    // BOTH axes are real states, or the agreement above is a state agreeing with itself. An ellipse
    // opens rows an uncut frame has none of, whichever way the flow switch is set; and the switch
    // itself opens one more row on the text, which is the only kind that has it -- the others own no
    // such row, so their two passes must come out at exactly the same count.
    for(int flowing = 0; flowing < 2; flowing++) assert_true(rows_cut[flowing] > rows_uncut[flowing]);
    if(_kinds[kind_index] == DT_CANVAS_OBJECT_TEXT)
      assert_true(rows_uncut[1] > rows_uncut[0]);
    else
      assert_int_equal(rows_uncut[1], rows_uncut[0]);
  }
  // A line whose ends need no frame is a connector as far as the card is concerned: no connector row
  // is gated on anything, so having free ends closes none of them. The day one is -- a route a free
  // line cannot take -- this is the line that parts the two rules for the kind that already exists.
  dt_canvas_object_t *free_line
      = dt_canvas_add_line(fixture.canvas, 0.0, 1500.0, 300.0, 1500.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  assert_non_null(free_line);
  assert_int_equal(_sections_present(free_line), _sections_present(fixture.objects[4]));
  assert_int_equal(_card_rows_applying(free_line), _card_rows_applying(fixture.objects[4]));
  _fixture_free(&fixture);
}

/**
 * The section names are what a configuration file holds, so they are pinned twice over: each name
 * against the section it belongs to, and each section against its place in the enum. Reordering the
 * enum -- which the screen order is free to ask for -- must not quietly rename anybody's stored
 * section.
 */
static void _section_names_are_pinned_to_the_enum(void **state)
{
  (void)state;
  static const struct
  {
    dt_canvas_prop_section_t section;
    const char *name;
  } pinned[] = {
    { DT_CANVAS_SECTION_CHARACTER, "character" }, { DT_CANVAS_SECTION_PARAGRAPH, "paragraph" },
    { DT_CANVAS_SECTION_TEXT_BOX, "text_box" },   { DT_CANVAS_SECTION_PICTURE, "picture" },
    { DT_CANVAS_SECTION_DRAWING, "drawing" },     { DT_CANVAS_SECTION_MAP, "map" },
    { DT_CANVAS_SECTION_ROUTE, "route" },         { DT_CANVAS_SECTION_SHAPE, "shape" },
    { DT_CANVAS_SECTION_ARRANGE, "arrange" },     { DT_CANVAS_SECTION_FILL, "fill" },
    { DT_CANVAS_SECTION_STROKE, "stroke" },       { DT_CANVAS_SECTION_CORNERS, "corners" },
    { DT_CANVAS_SECTION_SHADOW, "shadow" },       { DT_CANVAS_SECTION_CUTOUT, "cutout" },
  };
  assert_int_equal(G_N_ELEMENTS(pinned), DT_CANVAS_SECTION_COUNT);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  for(size_t idx = 0; idx < G_N_ELEMENTS(pinned); idx++)
  {
    assert_int_equal((int)pinned[idx].section, (int)idx);
    const char *name = dt_canvas_prop_section_name(pinned[idx].section);
    assert_non_null(name);
    assert_string_equal(name, pinned[idx].name);
    // One name per section, and none of them the sentinel a folded card is stored as.
    assert_false(g_hash_table_contains(seen, name));
    assert_string_not_equal(name, "none");
    g_hash_table_add(seen, (gpointer)name);
  }
  g_hash_table_destroy(seen);
  assert_null(dt_canvas_prop_section_name(DT_CANVAS_SECTION_COUNT));
  assert_null(dt_canvas_prop_section_name((dt_canvas_prop_section_t)-1));
}

static int _group_setup(void **state)
{
  (void)state;
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel_test_canvas_props.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  // The auto-height refit lays text out around cutouts, whose scratch comes from this arena.
  return dt_dev_pixelpipe_cache_init(64u * 1024u * 1024u, FALSE, FALSE) ? 0 : 1;
}

static int _group_teardown(void **state)
{
  (void)state;
  dt_dev_pixelpipe_cache_cleanup();
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(_rcfile);
  g_free(_rcfile);
  _rcfile = NULL;
  return 0;
}

/* --- the double-click drill rule ------------------------------------------------------------ */

/** GTK's default double-click delay and distance. */
#define DELAY_MS 400u
#define DISTANCE_PX 5u
/** A press this long after the previous one pairs with it. */
#define FAST_MS 120u

/** A place on the screen, and one far enough from it that GDK never pairs presses across the two. */
static const double HERE_X = 300.0;
static const double HERE_Y = 200.0;
static const double THERE_X = 520.0;
static const double THERE_Y = 90.0;

/** The clock and the place a sequence of presses is played at. */
typedef struct click_player_t
{
  dt_canvas_click_sequence_t sequence;
  guint32 clock_ms;
} click_player_t;

static void _player_init(click_player_t *player, const guint32 clock_ms)
{
  memset(player, 0, sizeof(*player));
  player->clock_ms = clock_ms;
}

/** A left press `gap_ms` after the previous one, at (x, y), with `shown_id`'s properties on screen. */
static gboolean _press_at(click_player_t *player, const guint32 gap_ms, const double x, const double y,
                          const uint32_t shown_id)
{
  player->clock_ms += gap_ms;
  const dt_canvas_click_t click = { .time_ms = player->clock_ms, .x = x, .y = y, .button = 1 };
  return dt_canvas_click_sequence_press(&player->sequence, &click, DELAY_MS, DISTANCE_PX, shown_id);
}

/**
 * A press at the usual place and, when GDK reports it as completing a double click, the answer
 * to that double click on `object_id`. `shown_id` is whose properties are on screen when the
 * press happens -- which for a press after an opening is the object that opening showed.
 */
static dt_canvas_double_click_t _play(click_player_t *player, const guint32 gap_ms, const uint32_t shown_id,
                                      const gboolean reported_double, const uint32_t object_id)
{
  _press_at(player, gap_ms, HERE_X, HERE_Y, shown_id);
  if(!reported_double) return DT_CANVAS_DOUBLE_CLICK_NOTHING;
  return dt_canvas_click_sequence_double(&player->sequence, object_id);
}

/** A double click on an object whose properties are closed opens them; a second one drills in. */
static void _a_double_click_opens_and_a_second_one_drills(void **state)
{
  (void)state;
  const uint32_t object_id = 7;
  click_player_t player;
  _player_init(&player, 1000);
  assert_int_equal(_play(&player, 0, 0, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, 0, TRUE, object_id), DT_CANVAS_DOUBLE_CLICK_OPEN);
  // A pause, then another double click: its first press finds the properties showing.
  assert_int_equal(_play(&player, 2 * DELAY_MS, object_id, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, object_id, TRUE, object_id), DT_CANVAS_DOUBLE_CLICK_DRILL);
}

/**
 * Five fast clicks: GDK reports a double click on the second press, a triple on the third, and
 * another double on the fifth. From closed properties the burst opens them and goes no further,
 * although by the fourth press the properties it opened are on screen.
 */
static void _a_burst_of_clicks_never_drills(void **state)
{
  (void)state;
  const uint32_t object_id = 7;
  click_player_t player;
  _player_init(&player, 5000);
  assert_int_equal(_play(&player, 0, 0, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, 0, TRUE, object_id), DT_CANVAS_DOUBLE_CLICK_OPEN);
  assert_int_equal(_play(&player, FAST_MS, object_id, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, object_id, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, object_id, TRUE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);

  // A burst that began with the properties showing drills once, on its first double click.
  player.clock_ms += 2 * DELAY_MS;
  assert_int_equal(_play(&player, 0, object_id, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, object_id, TRUE, object_id), DT_CANVAS_DOUBLE_CLICK_DRILL);
  assert_int_equal(_play(&player, FAST_MS, object_id, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, object_id, FALSE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(_play(&player, FAST_MS, object_id, TRUE, object_id), DT_CANVAS_DOUBLE_CLICK_NOTHING);
}

/**
 * A run ends where GDK stops pairing presses: at the delay itself (GDK pairs strictly sooner),
 * past the distance along either axis, and on another button. Timestamps wrap, and a pair across
 * the wrap is still a pair.
 */
static void _a_run_ends_where_gdk_stops_pairing_presses(void **state)
{
  (void)state;
  click_player_t player;
  _player_init(&player, 1000);
  assert_true(_press_at(&player, 0, HERE_X, HERE_Y, 0));
  assert_false(_press_at(&player, DELAY_MS - 1, HERE_X, HERE_Y, 0));
  assert_true(_press_at(&player, DELAY_MS, HERE_X, HERE_Y, 0));

  // The distance is inclusive, from the previous press, and measured on each axis on its own
  // rather than as a length: five pixels across and five down pair, though they are seven apart.
  assert_false(_press_at(&player, FAST_MS, HERE_X + DISTANCE_PX, HERE_Y + DISTANCE_PX, 0));
  assert_true(_press_at(&player, FAST_MS, HERE_X + DISTANCE_PX, HERE_Y + 2 * DISTANCE_PX + 1.0, 0));
  assert_true(_press_at(&player, FAST_MS, HERE_X + 2 * DISTANCE_PX + 1.0, HERE_Y + 2 * DISTANCE_PX + 1.0, 0));

  const dt_canvas_click_t right = { .time_ms = player.clock_ms + FAST_MS, .x = player.sequence.last.x,
                                    .y = player.sequence.last.y, .button = 3 };
  assert_true(dt_canvas_click_sequence_press(&player.sequence, &right, DELAY_MS, DISTANCE_PX, 0));
  player.clock_ms = right.time_ms;
  assert_true(_press_at(&player, FAST_MS, player.sequence.last.x, player.sequence.last.y, 0));

  _player_init(&player, G_MAXUINT32 - 50u);
  assert_true(_press_at(&player, 0, HERE_X, HERE_Y, 0));
  assert_false(_press_at(&player, FAST_MS, HERE_X, HERE_Y, 0));
  assert_true(player.clock_ms < FAST_MS);

  // Every press is counted, paired or not: that is what a deferred action checks.
  assert_int_equal(player.sequence.press_count, 2);
}

/**
 * The double click GDK reports is read against what showed before ITS OWN first press, the press
 * before the latest -- neither before the run's first press, nor before the latest press itself.
 */
static void _a_double_click_reads_its_own_first_press(void **state)
{
  (void)state;
  const uint32_t object_id = 4;
  click_player_t player;
  _player_init(&player, 1000);
  // The properties were open when the run began and are gone by the double click's first press.
  _press_at(&player, 0, HERE_X, HERE_Y, object_id);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, 0);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, 0);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, object_id), DT_CANVAS_DOUBLE_CLICK_OPEN);

  // Closed at the first press, and open by the second: the double click still only opens them.
  _player_init(&player, 1000);
  _press_at(&player, 0, HERE_X, HERE_Y, 0);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, object_id);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, object_id), DT_CANVAS_DOUBLE_CLICK_OPEN);
}

/**
 * A double click on 4, then at once one on 9, somewhere else: GDK pairs the second pair of
 * presses on its own, and so does the run, whose one answer 4 had spent -- 9's properties open.
 */
static void _a_double_click_elsewhere_is_answered_on_its_own(void **state)
{
  (void)state;
  click_player_t player;
  _player_init(&player, 1000);
  _press_at(&player, 0, HERE_X, HERE_Y, 0);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, 0);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, 4), DT_CANVAS_DOUBLE_CLICK_OPEN);
  _press_at(&player, FAST_MS, THERE_X, THERE_Y, 4);
  _press_at(&player, FAST_MS, THERE_X, THERE_Y, 0);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, 9), DT_CANVAS_DOUBLE_CLICK_OPEN);
}

/**
 * Properties that close after the double click's first press lead nowhere: a click on the
 * background right beside the object closes them and pairs with the next press, on the object.
 */
static void _properties_closed_since_the_first_press_do_not_drill(void **state)
{
  (void)state;
  const uint32_t object_id = 4;
  click_player_t player;
  _player_init(&player, 1000);
  _press_at(&player, 0, HERE_X, HERE_Y, object_id);
  dt_canvas_click_sequence_closed(&player.sequence);
  _press_at(&player, FAST_MS, HERE_X + 2.0, HERE_Y, 0);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, object_id), DT_CANVAS_DOUBLE_CLICK_OPEN);
}

/**
 * Properties showing for another object do not drill into this one: the double click opens this
 * one's. And a double click on nothing answers nothing and leaves the run's one answer unspent.
 */
static void _only_the_objects_own_properties_lead_into_it(void **state)
{
  (void)state;
  click_player_t player;
  _player_init(&player, 1000);
  _play(&player, 0, 4, FALSE, 0);
  assert_int_equal(_play(&player, FAST_MS, 4, TRUE, 0), DT_CANVAS_DOUBLE_CLICK_NOTHING);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, 5), DT_CANVAS_DOUBLE_CLICK_OPEN);
  player.clock_ms += 2 * DELAY_MS;
  _play(&player, 0, 4, FALSE, 0);
  _play(&player, FAST_MS, 4, FALSE, 0);
  assert_int_equal(dt_canvas_click_sequence_double(&player.sequence, 4), DT_CANVAS_DOUBLE_CLICK_DRILL);
}

/**
 * A handle takes a double click only when the double click's first press took one: a first press
 * that selected the frame, bringing its handles out under the second, leaves the double click to
 * the frame.
 */
static void _a_double_click_takes_a_handle_only_when_its_first_press_did(void **state)
{
  (void)state;
  click_player_t player;
  _player_init(&player, 1000);
  _press_at(&player, 0, HERE_X, HERE_Y, 0);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, 0);
  dt_canvas_click_sequence_took_handle(&player.sequence);
  assert_false(dt_canvas_click_sequence_began_on_handle(&player.sequence));

  _press_at(&player, 2 * DELAY_MS, HERE_X, HERE_Y, 0);
  dt_canvas_click_sequence_took_handle(&player.sequence);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, 0);
  dt_canvas_click_sequence_took_handle(&player.sequence);
  assert_true(dt_canvas_click_sequence_began_on_handle(&player.sequence));

  // And a handle the first press took does not stay taken for the presses after it.
  _press_at(&player, 2 * DELAY_MS, HERE_X, HERE_Y, 0);
  _press_at(&player, FAST_MS, HERE_X, HERE_Y, 0);
  assert_false(dt_canvas_click_sequence_began_on_handle(&player.sequence));
}

/* --- what a folded card says ----------------------------------------------------------------- */

/** One inset or four is decided the way the uniform inset's writer decides it: a float's rounding is not a side apart. */
static void _the_inset_is_one_number_as_its_writer_counts_it(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  assert_true(dt_canvas_props_inset_uniform(text));
  // Sides a thousandth of a point apart: what a drag left, not what anybody set.
  text->text.margins[DT_CANVAS_TEXT_MARGIN_TOP] = 3.0f;
  text->text.margins[DT_CANVAS_TEXT_MARGIN_RIGHT] = 3.001f;
  text->text.margins[DT_CANVAS_TEXT_MARGIN_BOTTOM] = 3.0f;
  text->text.margins[DT_CANVAS_TEXT_MARGIN_LEFT] = 2.999f;
  assert_true(dt_canvas_props_inset_uniform(text));
  // ...and the writer agrees: the same number written over them is no edit.
  const dt_canvas_prop_value_t three = _number(3.0);
  assert_int_equal(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_INSET, &three), 0);
  text->text.margins[DT_CANVAS_TEXT_MARGIN_RIGHT] = 9.0f;
  assert_false(dt_canvas_props_inset_uniform(text));
  assert_true(dt_canvas_props_inset_uniform(fixture.objects[1]));
  _fixture_free(&fixture);
}

/** A folded section names what it holds, and every extra that is not left as it was. */
static void _a_folded_section_names_the_extras_it_hides(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  char line[DT_CANVAS_PROP_TEXT_LEN];

  dt_canvas_prop_section_summary(fixture.canvas, text, DT_CANVAS_SECTION_PARAGRAPH, line, sizeof(line));
  fprintf(stderr, "paragraph, untouched: \"%s\"\n", line);
  assert_non_null(strstr(line, "1.00"));
  assert_null(strstr(line, "indent"));
  const dt_canvas_prop_value_t indent = _number(12.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_FIRST_LINE_INDENT, &indent)
              & DT_CANVAS_EFFECT_CHANGED);
  dt_canvas_prop_section_summary(fixture.canvas, text, DT_CANVAS_SECTION_PARAGRAPH, line, sizeof(line));
  fprintf(stderr, "paragraph, indented: \"%s\"\n", line);
  assert_non_null(strstr(line, "first-line indent 12 pt"));

  // An override section speaks for its group first, and still names its extras.
  dt_canvas_prop_section_summary(fixture.canvas, text, DT_CANVAS_SECTION_CHARACTER, line, sizeof(line));
  assert_non_null(strstr(line, "canvas default"));
  assert_null(strstr(line, "letter spacing"));
  const dt_canvas_prop_value_t tracking = _number(20.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_LETTER_SPACING, &tracking)
              & DT_CANVAS_EFFECT_CHANGED);
  dt_canvas_prop_section_summary(fixture.canvas, text, DT_CANVAS_SECTION_CHARACTER, line, sizeof(line));
  fprintf(stderr, "character, tracked: \"%s\"\n", line);
  assert_non_null(strstr(line, "canvas default"));
  assert_non_null(strstr(line, "letter spacing 20"));

  // Four insets apart are one entry, not the inset and its four sides.
  const dt_canvas_prop_value_t wide = _number(30.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_INSET_RIGHT, &wide)
              & DT_CANVAS_EFFECT_CHANGED);
  dt_canvas_prop_section_summary(fixture.canvas, text, DT_CANVAS_SECTION_TEXT_BOX, line, sizeof(line));
  fprintf(stderr, "text box, inset apart: \"%s\"\n", line);
  assert_non_null(strstr(line, "inset 12/30/12/12 pt"));
  assert_null(strstr(line, "right"));

  // A kind without the section has nothing to say in it.
  dt_canvas_prop_section_summary(fixture.canvas, fixture.objects[4], DT_CANVAS_SECTION_PARAGRAPH, line, sizeof(line));
  assert_string_equal(line, "");
  _fixture_free(&fixture);
}

/** The card is altered by a group the object chose values for, even one whose rows are on the strip. */
static void _a_card_is_altered_by_a_font_of_its_own(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *text = fixture.objects[0];
  assert_false(dt_canvas_props_card_altered(fixture.canvas, text));
  const dt_canvas_prop_value_t face = _text("Monospace 12");
  assert_true(dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_FONT, &face)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, text, DT_CANVAS_GROUP_FONT), DT_CANVAS_OWN_CUSTOM);
  assert_true(dt_canvas_props_card_altered(fixture.canvas, text));
  dt_canvas_group_set_own(fixture.canvas, text, DT_CANVAS_GROUP_FONT, FALSE);
  assert_false(dt_canvas_props_card_altered(fixture.canvas, text));
  // A row moved off its neutral value alters the card as well.
  const dt_canvas_prop_value_t indent = _number(12.0);
  dt_canvas_prop_write(fixture.canvas, text, DT_CANVAS_PROP_TEXT_FIRST_LINE_INDENT, &indent);
  assert_true(dt_canvas_props_card_altered(fixture.canvas, text));
  _fixture_free(&fixture);
}

/** An override section and the cutout never open by themselves, and an override section stays open only for values of the object's own. */
static void _override_sections_open_only_when_asked(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  assert_true(dt_canvas_prop_section_opens_by_itself(DT_CANVAS_SECTION_PARAGRAPH, DT_CANVAS_OBJECT_TEXT));
  assert_true(dt_canvas_prop_section_opens_by_itself(DT_CANVAS_SECTION_ARRANGE, DT_CANVAS_OBJECT_IMAGE));
  assert_false(dt_canvas_prop_section_opens_by_itself(DT_CANVAS_SECTION_STROKE, DT_CANVAS_OBJECT_IMAGE));
  assert_false(dt_canvas_prop_section_opens_by_itself(DT_CANVAS_SECTION_SHADOW, DT_CANVAS_OBJECT_CONNECTOR));
  assert_false(dt_canvas_prop_section_opens_by_itself(DT_CANVAS_SECTION_CUTOUT, DT_CANVAS_OBJECT_IMAGE));
  // A connector's line is no override group: it is its own.
  assert_true(dt_canvas_prop_section_opens_by_itself(DT_CANVAS_SECTION_STROKE, DT_CANVAS_OBJECT_CONNECTOR));

  dt_canvas_object_t *image = fixture.objects[1];
  assert_true(dt_canvas_prop_section_stays_open(fixture.canvas, image, DT_CANVAS_SECTION_ARRANGE));
  assert_false(dt_canvas_prop_section_stays_open(fixture.canvas, image, DT_CANVAS_SECTION_STROKE));
  const dt_canvas_prop_value_t width = _number(9.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, image, DT_CANVAS_PROP_BORDER_WIDTH, &width)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_true(dt_canvas_prop_section_stays_open(fixture.canvas, image, DT_CANVAS_SECTION_STROKE));
  // A drawing owns no border by choice, only by birth: that is not a reason to stay open either.
  assert_false(dt_canvas_prop_section_stays_open(fixture.canvas, fixture.objects[3], DT_CANVAS_SECTION_STROKE));
  _fixture_free(&fixture);
}

/* --- drawn shapes -------------------------------------------------------------------------- */

/**
 * Filled is the fill's own opacity and nothing else, so the switch and the colour well can never
 * disagree; the colour is KEPT when it is switched off, so switching it back on brings the colour
 * the user chose rather than a grey they never asked for.
 */
static void _the_filled_switch_is_the_fills_own_opacity(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *shape = fixture.objects[5];
  const dt_canvas_prop_value_t chosen = _color(0.1f, 0.7f, 0.3f, 1.0f);
  assert_true(dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_BACKGROUND, &chosen)
              & DT_CANVAS_EFFECT_CHANGED);

  dt_canvas_prop_value_t filled;
  dt_canvas_prop_read(fixture.canvas, shape, DT_CANVAS_PROP_SHAPE_FILLED, &filled);
  assert_true(filled.flag);

  // The shape owns a border already, so nothing is rescued: only the opacity goes.
  const dt_canvas_prop_value_t six = _number(6.0);
  const dt_canvas_prop_value_t unfilled = _flag(FALSE);
  const dt_canvas_prop_value_t refilled = _flag(TRUE);
  assert_true(dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_BORDER_WIDTH, &six)
              & DT_CANVAS_EFFECT_CHANGED);
  const uint32_t off = dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_SHAPE_FILLED, &unfilled);
  assert_true(off & DT_CANVAS_EFFECT_CHANGED);
  assert_true(off & DT_CANVAS_EFFECT_COUPLED);
  // What a shape's style is worth remembering by: the view keeps it for the next shape drawn.
  assert_true(off & DT_CANVAS_EFFECT_COMMIT_CONF);
  assert_float_equal(shape->background.alpha, 0.0f, 1e-6);
  assert_float_equal(shape->background.red, 0.1f, 1e-6);
  assert_float_equal(shape->background.green, 0.7f, 1e-6);
  assert_float_equal(shape->background.blue, 0.3f, 1e-6);
  assert_float_equal(shape->border_width, 6.0f, 1e-6);
  dt_canvas_prop_read(fixture.canvas, shape, DT_CANVAS_PROP_SHAPE_FILLED, &filled);
  assert_false(filled.flag);

  // Back on: the opacity alone comes back, and with it the colour that was kept.
  assert_true(dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_SHAPE_FILLED, &refilled)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(shape->background.alpha, 1.0f, 1e-6);
  assert_float_equal(shape->background.green, 0.7f, 1e-6);
  _fixture_free(&fixture);
}

/** A shape with neither a fill nor a border is nothing at all, so switching the fill off gives it one. */
static void _switching_the_fill_off_rescues_a_shape_that_would_vanish(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *shape = fixture.objects[5];
  // The canvas's own border is what the shape inherits, and it is nothing.
  fixture.canvas->border_width = 0.0f;
  const dt_canvas_prop_value_t red = _color(0.9f, 0.2f, 0.1f, 1.0f);
  const dt_canvas_prop_value_t unfilled = _flag(FALSE);
  assert_true(dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_BACKGROUND, &red)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, shape, DT_CANVAS_GROUP_BORDER), DT_CANVAS_OWN_INHERIT);

  assert_true(dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_SHAPE_FILLED, &unfilled)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_float_equal(shape->background.alpha, 0.0f, 1e-6);
  assert_true((shape->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE) != 0);
  dt_canvas_color_t border_color;
  float border_width = 0.0f;
  dt_canvas_object_effective_border(fixture.canvas, shape, &border_color, &border_width);
  assert_true(border_width > 0.0f);
  // In the colour the fill had, so what was a filled shape becomes the outline of the same shape.
  assert_float_equal(border_color.red, 0.9f, 1e-6);
  assert_float_equal(border_color.green, 0.2f, 1e-6);
  assert_float_equal(border_color.alpha, 1.0f, 1e-6);
  _fixture_free(&fixture);
}

/**
 * A shape is born inheriting, as decided: the canvas's border, radius and shadow apply until it is
 * given its own. A zero one it was GIVEN reads as the kind's default rather than as a choice, so a
 * card is never marked altered for a border nobody chose.
 */
static void _a_shape_inherits_at_birth_and_a_zero_it_owns_is_no_choice(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *shape = fixture.objects[5];
  assert_int_equal(dt_canvas_group_state(fixture.canvas, shape, DT_CANVAS_GROUP_BORDER), DT_CANVAS_OWN_INHERIT);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, shape, DT_CANVAS_GROUP_CORNER), DT_CANVAS_OWN_INHERIT);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, shape, DT_CANVAS_GROUP_SHADOW), DT_CANVAS_OWN_INHERIT);
  assert_false(dt_canvas_props_card_altered(fixture.canvas, shape));

  // A shape born from a remembered style that owns nothing at all: the flags are its own, the
  // values are the kind's.
  dt_canvas_shape_style_t bare = dt_canvas_shape_style_default();
  bare.border_override = TRUE;
  bare.border_width = 0.0f;
  bare.corner_override = TRUE;
  bare.corner_radius = 0.0f;
  bare.shadow_override = TRUE;
  const dt_canvas_rect_t box = { 1400.0, 0.0, 100.0, 100.0 };
  dt_canvas_object_t *plain = dt_canvas_add_shape(fixture.canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &bare);
  assert_non_null(plain);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, plain, DT_CANVAS_GROUP_BORDER),
                   DT_CANVAS_OWN_KIND_DEFAULT);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, plain, DT_CANVAS_GROUP_CORNER),
                   DT_CANVAS_OWN_KIND_DEFAULT);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, plain, DT_CANVAS_GROUP_SHADOW),
                   DT_CANVAS_OWN_KIND_DEFAULT);
  assert_false(dt_canvas_props_card_altered(fixture.canvas, plain));
  // And a value somebody chose is a choice again.
  const dt_canvas_prop_value_t five = _number(5.0);
  assert_true(dt_canvas_prop_write(fixture.canvas, plain, DT_CANVAS_PROP_BORDER_WIDTH, &five)
              & DT_CANVAS_EFFECT_CHANGED);
  assert_int_equal(dt_canvas_group_state(fixture.canvas, plain, DT_CANVAS_GROUP_BORDER), DT_CANVAS_OWN_CUSTOM);
  assert_true(dt_canvas_props_card_altered(fixture.canvas, plain));
  _fixture_free(&fixture);
}

/**
 * A shape's style is the next shape's; where it sits and how big it is are not. The writer says
 * which is which through COMMIT_CONF, and the view -- which owns the configuration -- keeps it.
 */
static void _only_a_shapes_style_is_worth_remembering(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *shape = fixture.objects[5];
  const struct
  {
    dt_canvas_prop_id_t prop;
    dt_canvas_prop_value_t value;
    gboolean remembered;
  } edits[] = {
    { DT_CANVAS_PROP_BACKGROUND, _color(0.3f, 0.6f, 0.9f, 1.0f), TRUE },
    { DT_CANVAS_PROP_BORDER_WIDTH, _number(13.0), TRUE },
    { DT_CANVAS_PROP_BORDER_COLOR, _color(1.0f, 1.0f, 0.0f, 1.0f), TRUE },
    { DT_CANVAS_PROP_CORNER_RADIUS, _number(9.0), TRUE },
    { DT_CANVAS_PROP_SHADOW_BLUR, _number(7.0), TRUE },
    { DT_CANVAS_PROP_X, _number(1234.0), FALSE },
    { DT_CANVAS_PROP_WIDTH, _number(321.0), FALSE },
    { DT_CANVAS_PROP_ROTATION, _number(15.0), FALSE },
    { DT_CANVAS_PROP_OPACITY, _number(50.0), FALSE },
  };
  for(size_t idx = 0; idx < G_N_ELEMENTS(edits); idx++)
  {
    const uint32_t effects = dt_canvas_prop_write(fixture.canvas, shape, edits[idx].prop, &edits[idx].value);
    assert_true(effects & DT_CANVAS_EFFECT_CHANGED);
    assert_int_equal((effects & DT_CANVAS_EFFECT_COMMIT_CONF) != 0, edits[idx].remembered);
  }
  // The same rows on a picture teach nothing: only a shape's style is a shape's style.
  const dt_canvas_prop_value_t eleven = _number(11.0);
  const uint32_t picture
      = dt_canvas_prop_write(fixture.canvas, fixture.objects[1], DT_CANVAS_PROP_BORDER_WIDTH, &eleven);
  assert_true(picture & DT_CANVAS_EFFECT_CHANGED);
  assert_int_equal(picture & DT_CANVAS_EFFECT_COMMIT_CONF, 0);

  // And what the object reads back is what the style says, field for field.
  dt_canvas_shape_style_t style;
  assert_true(dt_canvas_shape_style_get(shape, &style));
  assert_float_equal(style.fill.blue, 0.9f, 1e-6);
  assert_true(style.border_override);
  assert_float_equal(style.border_width, 13.0f, 1e-6);
  assert_float_equal(style.border_color.red, 1.0f, 1e-6);
  assert_true(style.corner_override);
  assert_float_equal(style.corner_radius, 9.0f, 1e-6);
  assert_true(style.shadow_override);
  assert_float_equal(style.shadow.blur, 7.0f, 1e-6);
  assert_false(dt_canvas_shape_style_get(fixture.objects[1], &style));
  _fixture_free(&fixture);
}

/**
 * Whether a shape is there at all, for the text flowing around it, is decided by two alphas: its
 * fill's and its border's. Writing either is therefore the same change to the plane the FILLED
 * switch makes, and owes the frames flowing around it the refit an obstacle's move owes them --
 * which of the two ways a fill was emptied must not decide whether the column beside it settles.
 */
static void _emptying_a_shapes_colours_refits_the_text_beside_it(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 400.0, 400.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and "
      "the measure a line is set to belongs to the page rather than to the window looking at it.");
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_AUTO_HEIGHT;
  text->text.wrap_standoff = 0.0f;
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.border_override = TRUE;
  style.border_width = 4.0f;
  style.border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  // Over the text from its very first line: an obstacle the column has not reached yet says
  // nothing about whether it would have had to go round it.
  const dt_canvas_rect_t box = { -110.0, -190.0, 220.0, 220.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
  assert_non_null(shape);
  dt_canvas_props_settle_all(canvas);
  const double walled = text->height;

  // The view settles the whole plane when the writer asks it to, and only then.
  const dt_canvas_prop_value_t emptied = _color(0.5f, 0.5f, 0.5f, 0.0f);
  const uint32_t fill = dt_canvas_prop_write(canvas, shape, DT_CANVAS_PROP_BACKGROUND, &emptied);
  assert_true(fill & DT_CANVAS_EFFECT_SETTLE_ALL);
  if(fill & DT_CANVAS_EFFECT_SETTLE_ALL) dt_canvas_props_settle_all(canvas);
  const double outlined = text->height;
  fprintf(stderr, "text under a shape: walled %f -> outlined %f\n", walled, outlined);
  // The hole in the middle is the column's again, so the same paragraph takes less height.
  assert_true(outlined < walled);

  // And the band itself goes with the border's own strength, which is the other alpha.
  const dt_canvas_prop_value_t invisible = _color(1.0f, 1.0f, 1.0f, 0.0f);
  const uint32_t border = dt_canvas_prop_write(canvas, shape, DT_CANVAS_PROP_BORDER_COLOR, &invisible);
  assert_true(border & DT_CANVAS_EFFECT_SETTLE_ALL);
  if(border & DT_CANVAS_EFFECT_SETTLE_ALL) dt_canvas_props_settle_all(canvas);
  const double gone = text->height;
  fprintf(stderr, "text under a shape: outlined %f -> gone %f\n", outlined, gone);
  assert_true(gone < outlined);
  dt_canvas_free(canvas);
}

/**
 * Handing a group back is as much a shape's style as taking it: what the next shape is drawn with
 * is the border, the corners and the shadow the last one was left with, override flags included.
 * Taking a group is always followed by a value write that asks to be remembered; handing it back
 * stands alone, and was the one edit the memory never heard about.
 */
static void _handing_a_group_back_is_a_shapes_style_too(void **state)
{
  (void)state;
  props_fixture_t fixture;
  _fixture_build(&fixture);
  dt_canvas_object_t *shape = fixture.objects[5];
  const dt_canvas_prop_value_t six = _number(6.0);
  const uint32_t taken = dt_canvas_prop_write(fixture.canvas, shape, DT_CANVAS_PROP_BORDER_WIDTH, &six);
  assert_true(taken & DT_CANVAS_EFFECT_COMMIT_CONF);
  dt_canvas_shape_style_t style;
  assert_true(dt_canvas_shape_style_get(shape, &style));
  assert_true(style.border_override);

  const uint32_t given_back = dt_canvas_group_set_own(fixture.canvas, shape, DT_CANVAS_GROUP_BORDER, FALSE);
  assert_true(given_back & DT_CANVAS_EFFECT_CHANGED);
  assert_true(given_back & DT_CANVAS_EFFECT_COMMIT_CONF);
  assert_true(dt_canvas_shape_style_get(shape, &style));
  assert_false(style.border_override);
  // Taking one is worth remembering for the same reason, whichever way round the switch went.
  assert_true(dt_canvas_group_set_own(fixture.canvas, shape, DT_CANVAS_GROUP_SHADOW, TRUE)
              & DT_CANVAS_EFFECT_COMMIT_CONF);
  assert_true(dt_canvas_group_set_own(fixture.canvas, shape, DT_CANVAS_GROUP_CORNER, TRUE)
              & DT_CANVAS_EFFECT_COMMIT_CONF);

  // A picture teaches the next shape nothing: only a shape's style is a shape's style.
  dt_canvas_object_t *picture = fixture.objects[1];
  assert_int_equal(dt_canvas_group_set_own(fixture.canvas, picture, DT_CANVAS_GROUP_BORDER, TRUE)
                       & DT_CANVAS_EFFECT_COMMIT_CONF,
                   0);
  assert_int_equal(dt_canvas_group_set_own(fixture.canvas, picture, DT_CANVAS_GROUP_BORDER, FALSE)
                       & DT_CANVAS_EFFECT_COMMIT_CONF,
                   0);
  _fixture_free(&fixture);
}

/** Only what has something inside it is drilled into. */
static void _only_texts_pictures_and_drawings_have_a_content_action(void **state)
{
  (void)state;
  assert_true(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_TEXT));
  assert_true(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_IMAGE));
  assert_true(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_SVG));
  assert_false(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_MAP));
  assert_false(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_CONNECTOR));
  // A shape holds nothing to go into: it IS its own outline, and every row it has is on the card.
  assert_false(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_SHAPE));
  assert_false(dt_canvas_props_has_content_action(DT_CANVAS_OBJECT_NONE));
  // A button that exists says what it does; one that does not has nothing to say.
  const uint32_t kinds[] = { DT_CANVAS_OBJECT_NONE, DT_CANVAS_OBJECT_TEXT,      DT_CANVAS_OBJECT_IMAGE,
                             DT_CANVAS_OBJECT_MAP,  DT_CANVAS_OBJECT_CONNECTOR, DT_CANVAS_OBJECT_SVG,
                             DT_CANVAS_OBJECT_SHAPE };
  for(size_t idx = 0; idx < G_N_ELEMENTS(kinds); idx++)
    assert_int_equal(dt_canvas_props_content_action_tooltip(kinds[idx]) != NULL,
                     dt_canvas_props_has_content_action(kinds[idx]));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_double_click_opens_and_a_second_one_drills),
    cmocka_unit_test(_a_burst_of_clicks_never_drills),
    cmocka_unit_test(_a_run_ends_where_gdk_stops_pairing_presses),
    cmocka_unit_test(_a_double_click_reads_its_own_first_press),
    cmocka_unit_test(_a_double_click_elsewhere_is_answered_on_its_own),
    cmocka_unit_test(_properties_closed_since_the_first_press_do_not_drill),
    cmocka_unit_test(_only_the_objects_own_properties_lead_into_it),
    cmocka_unit_test(_a_double_click_takes_a_handle_only_when_its_first_press_did),
    cmocka_unit_test(_only_texts_pictures_and_drawings_have_a_content_action),
    cmocka_unit_test(_the_filled_switch_is_the_fills_own_opacity),
    cmocka_unit_test(_switching_the_fill_off_rescues_a_shape_that_would_vanish),
    cmocka_unit_test(_a_shape_inherits_at_birth_and_a_zero_it_owns_is_no_choice),
    cmocka_unit_test(_only_a_shapes_style_is_worth_remembering),
    cmocka_unit_test(_emptying_a_shapes_colours_refits_the_text_beside_it),
    cmocka_unit_test(_handing_a_group_back_is_a_shapes_style_too),
    cmocka_unit_test(_every_property_is_described_once),
    cmocka_unit_test(_a_numbers_soft_range_and_neutral_lie_inside_its_hard_range),
    cmocka_unit_test(_pairs_point_at_each_other),
    cmocka_unit_test(_every_kind_reads_its_sections_in_screen_order),
    cmocka_unit_test(_groups_belong_to_their_sections),
    cmocka_unit_test(_conditions_depend_on_the_objects_own_switches),
    cmocka_unit_test(_every_property_round_trips_on_every_kind),
    cmocka_unit_test(_a_shadow_offset_edited_while_inheriting_is_kept_and_owned),
    cmocka_unit_test(_a_border_colour_keeps_the_effective_width),
    cmocka_unit_test(_a_blur_of_minus_one_round_trips),
    cmocka_unit_test(_a_fresh_drawing_owns_only_what_its_kind_is_born_with),
    cmocka_unit_test(_an_all_zero_inset_stays_zero),
    cmocka_unit_test(_the_uniform_inset_writes_all_four_even_when_the_top_agrees),
    cmocka_unit_test(_an_unset_line_height_reads_one_and_is_never_written_back),
    cmocka_unit_test(_an_inherited_value_written_while_inheriting_changes_nothing),
    cmocka_unit_test(_owning_first_keeps_a_value_equal_to_the_canvas),
    cmocka_unit_test(_a_typed_number_snaps_a_dragged_one),
    cmocka_unit_test(_a_kept_ratio_answers_a_width_with_a_height),
    cmocka_unit_test(_an_ellipse_size_keeps_its_proportions),
    cmocka_unit_test(_map_properties_ask_for_a_render_and_never_touch_conf),
    cmocka_unit_test(_owning_a_group_changes_nothing_on_screen),
    cmocka_unit_test(_the_inherited_value_is_what_giving_the_group_back_shows),
    cmocka_unit_test(_giving_the_font_back_refits_the_frame),
    cmocka_unit_test(_a_size_on_an_inheriting_font_writes_the_family_out),
    cmocka_unit_test(_making_a_line_cubic_bends_it_into_an_arc),
    cmocka_unit_test(_a_line_style_is_the_next_line_s_and_a_connector_s_is_not),
    cmocka_unit_test(_arrowheads_and_backgrounds_land_where_they_belong),
    cmocka_unit_test(_reversing_a_connector_walks_the_same_curve_backwards),
    cmocka_unit_test(_rows_follow_what_they_depend_on),
    cmocka_unit_test(_a_section_is_there_when_one_of_its_rows_applies),
    cmocka_unit_test(_section_names_are_pinned_to_the_enum),
    cmocka_unit_test(_the_inset_is_one_number_as_its_writer_counts_it),
    cmocka_unit_test(_a_folded_section_names_the_extras_it_hides),
    cmocka_unit_test(_a_card_is_altered_by_a_font_of_its_own),
    cmocka_unit_test(_override_sections_open_only_when_asked),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
