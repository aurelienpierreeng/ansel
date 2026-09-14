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
                                   DT_CANVAS_OBJECT_SVG, DT_CANVAS_OBJECT_CONNECTOR };

static gchar *_write_svg(const char *body)
{
  gchar *path = NULL;
  const int handle = g_file_open_tmp("canvas-props-XXXXXX.svg", &path, NULL);
  assert_true(handle >= 0);
  close(handle);
  assert_true(g_file_set_contents(path, body, -1, NULL));
  return path;
}

/** A canvas with one object of every kind: a text, a picture, a map, a drawing and a connector. */
typedef struct props_fixture_t
{
  dt_canvas_t *canvas;
  dt_canvas_object_t *objects[5]; ///< in `_kinds` order
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
  for(int idx = 0; idx < 5; idx++)
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
                           | (1u << DT_CANVAS_OBJECT_CONNECTOR);
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
  // The bar used to throw this edit away whenever the blur read "default": the offset was
  // written into a shadow the object did not own, so nothing drawn changed.
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
  // The property bar's spin has no reset to the canvas's value but its "default" sentinel, so
  // stepping off it onto the canvas's own number -- a square corner, the default everywhere --
  // owns the group first and writes second. The number must stick when the canvas changes later.
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

int main(void)
{
  const struct CMUnitTest tests[] = {
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
    cmocka_unit_test(_map_properties_ask_for_a_render_and_never_touch_conf),
    cmocka_unit_test(_owning_a_group_changes_nothing_on_screen),
    cmocka_unit_test(_giving_the_font_back_refits_the_frame),
    cmocka_unit_test(_a_size_on_an_inheriting_font_writes_the_family_out),
    cmocka_unit_test(_arrowheads_and_backgrounds_land_where_they_belong),
    cmocka_unit_test(_rows_follow_what_they_depend_on),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
