/*
    This file is part of darktable,
    Copyright (C) 2020-2021 Philippe Weyland.
    Copyright (C) 2021 HansBull.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Pascal Obry.
    Copyright (C) 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    
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

#include "common/geo.h"
#include "common/map_locations.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "database/location_repository.h"
#include "database/tag_repository.h"
#include "common/tags.h"

// root for location geotagging
const char *location_tag = "darktable|locations";
const char *location_tag_prefix = "darktable|locations|";

// create a new location
guint dt_map_location_new(const char *const name)
{
  char *loc_name = g_strconcat(location_tag_prefix, name, NULL);
  guint locid = -1;
  dt_tag_new(loc_name, &locid);
  dt_free(loc_name);
  return locid;
}

// remove a location
void dt_map_location_delete(const guint locid)
{
  if(locid == -1) return;
  char *name = dt_tag_get_name(locid);
  if(name)
  {
    if(g_str_has_prefix(name, location_tag_prefix))
    {
      dt_location_repository_delete(locid);
      dt_tag_remove(locid, TRUE);
    }
  dt_free(name);
  }
}

// rename a location
void dt_map_location_rename(const guint locid, const char *const name)
{
  if(locid == -1 || IS_NULL_PTR(name) || !name[0]) return;
  char *old_name = dt_tag_get_name(locid);
  if(old_name)
  {
    if(g_str_has_prefix(old_name, location_tag_prefix))
    {
      char *new_name = g_strconcat(location_tag_prefix, name, NULL);
      dt_tag_rename(locid, new_name);
      dt_free(new_name);
    }
    dt_free(old_name);
  }
}

// does the location name already exist
gboolean dt_map_location_name_exists(const char *const name)
{
  char *new_name = g_strconcat(location_tag_prefix, name, NULL);
  const gboolean exists = dt_tag_exists(new_name, NULL);
  dt_free(new_name);
  return exists;
}

// gets location's images number
int dt_map_location_get_images_count(const guint locid)
{
  /* The repository answers -1 when the count cannot be read (dt_tag_remove()'s original
   * default); this function's original answered 0, and its callers size things by it. */
  const int count = dt_tag_repository_count_attachments(locid);
  return count < 0 ? 0 : count;
}

// retrieve list of tags which are on that path
GList *dt_map_location_get_locations_by_path(const gchar *path,
                                             const gboolean remove_root)
{
  if(IS_NULL_PTR(path)) return NULL;

  gchar *path1, *path2;
  if(!path[0])
  {
    path1 = g_strdup(location_tag);
    path2 = g_strdup_printf("%s|", path1);
  }
  else
  {
    path1 = g_strconcat(location_tag_prefix, path, NULL);
    path2 = g_strdup_printf("%s|", path1);
  }

  GList *tags = dt_tag_repository_get_by_path_with_counts(path1, path2);

  /* The repository hands back full tag names; what a location is CALLED is this module's
   * definition, so the trimming stays here. */
  GList *locs = NULL;
  for(GList *l = tags; l; l = g_list_next(l))
  {
    const dt_tag_count_t *tc = (const dt_tag_count_t *)l->data;
    const int lgth = remove_root ? strlen(path1) + 1 : strlen(location_tag_prefix);
    if(tc->name && strlen(tc->name) > lgth)
    {
      dt_map_location_t *t = g_malloc0(sizeof(dt_map_location_t));
      if(t)
      {
        t->tag = g_strdup(tc->name + lgth);
        t->id = tc->id;
        t->count = tc->count;
        locs = g_list_prepend(locs, t);
      }
    }
  }
  g_list_free_full(tags, dt_tag_count_free);

  dt_free(path1);
  dt_free(path2);
  return locs;
}

GList *dt_map_location_get_locations_on_map(const dt_map_box_t *const bbox)
{
  return dt_location_repository_get_in_bbox(bbox);
}

void dt_map_location_get_polygons(dt_location_draw_t *ld)
{
  if(ld->data.shape != MAP_LOCATION_SHAPE_POLYGONS)
    return;

  void *blob = NULL;
  gint bytes = 0;
  if(!dt_location_repository_get_polygon(ld->id, &blob, &bytes))
    return;

  dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)blob;
  ld->data.plg_pts = bytes / (gint)sizeof(dt_geo_map_display_point_t);

  GList *pol = NULL;
  for(int i = 0; i < ld->data.plg_pts; i++, p++)
    pol = g_list_prepend(pol, p);
  ld->data.polygons = g_list_reverse(pol);
}

void dt_map_location_free_polygons(dt_location_draw_t *ld)
{
  if(ld->data.shape == MAP_LOCATION_SHAPE_POLYGONS && ld->data.polygons)
  {
    dt_free(ld->data.polygons->data);
    g_list_free(ld->data.polygons);
    ld->data.polygons = NULL;
  }
  ld->data.polygons = NULL;
  ld->data.plg_pts = 0;
}

static gboolean _is_point_in_polygon(const dt_geo_map_display_point_t *pt,
                                     const gint plg_pts, const dt_geo_map_display_point_t *plp)
{
  gboolean inside = FALSE;
  dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)plp;
  float lat1 = plp->lat;
  float lon1 = plp->lon;
  float lat2, lon2;
  for(int i = 0; i < plg_pts; i++)
  {
    if(i < plg_pts - 1)
    {
      p++;
      lat2 = p->lat;
      lon2 = p->lon;
    }
    else
    {
      lat2 = plp->lat;
      lon2 = plp->lon;
    }
    if(!(((lat1 > pt->lat) && (lat2 > pt->lat)) ||
         ((lat1 < pt->lat) && (lat2 < pt->lat))))
    {
      const float sl = lon1 + (lon2 - lon1) * (pt->lat - lat1) / (lat2 - lat1);
      if(pt->lon > sl)
        inside = !inside;
    }
    lat1 = lat2;
    lon1 = lon2;
  }
  return inside;
}

static void _free_result_item(dt_map_location_t *t, gpointer unused)
{
  dt_free(t->tag);
  dt_free(t);
}

// free map location list
void dt_map_location_free_result(GList **result)
{
  if(result && *result)
  {
    g_list_free_full(*result, (GDestroyNotify)_free_result_item);
    *result = NULL;
  }
}

static gint _sort_by_path(gconstpointer a, gconstpointer b)
{
  const dt_map_location_t *tuple_a = (const dt_map_location_t *)a;
  const dt_map_location_t *tuple_b = (const dt_map_location_t *)b;

  return g_strcmp0(tuple_a->tag, tuple_b->tag);
}

// sort the tag list considering the '|' character
GList *dt_map_location_sort(GList *tags)
{
  // order such that sub tags are coming directly behind their parent
  GList *sorted_tags;
  for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
  {
    gchar *tag = ((dt_map_location_t *)taglist->data)->tag;
    for(char *letter = tag; *letter; letter++)
      if(*letter == '|') *letter = '\1';
  }
  sorted_tags = g_list_sort(tags, _sort_by_path);
  for(GList *taglist = sorted_tags; taglist; taglist = g_list_next(taglist))
  {
    gchar *tag = ((dt_map_location_t *)taglist->data)->tag;
    for(char *letter = tag; *letter; letter++)
      if(*letter == '\1') *letter = '|';
  }
  return sorted_tags;
}

// get location's data
dt_map_location_data_t *dt_map_location_get_data(const guint locid)
{
  if(locid == -1) return NULL;
  return dt_location_repository_get_data(locid, location_tag_prefix);
}

// set locations's data
void dt_map_location_set_data(const guint locid, const dt_map_location_data_t *g)
{
  if(locid == -1) return;
  dt_location_repository_set_data(locid, g);
}

// find locations which match with that image
GList *dt_map_location_find_locations(const int32_t imgid)
{
  /* The SQL bounds a polygon location by its box; whether the image is actually inside
   * the polygon is geometry, so it is decided here. */
  GList *candidates = dt_location_repository_find_locations_for_image(imgid);

  GList *tags = NULL;
  for(GList *c = candidates; c; c = g_list_next(c))
  {
    const dt_location_candidate_t *cand = (const dt_location_candidate_t *)c->data;
    gboolean inside = TRUE;

    if(cand->shape == MAP_LOCATION_SHAPE_POLYGONS)
    {
      void *blob = NULL;
      gint bytes = 0;
      inside = FALSE;
      if(dt_location_repository_get_polygon(cand->id, &blob, &bytes))
      {
        const dt_geo_map_display_point_t pt = { .lon = cand->lon, .lat = cand->lat };
        inside = _is_point_in_polygon(&pt, bytes / (gint)sizeof(dt_geo_map_display_point_t), blob);
        dt_free(blob);
      }
    }

    if(inside) tags = g_list_prepend(tags, GINT_TO_POINTER(cand->id));
  }
  g_list_free_full(candidates, g_free);

  return tags;
}

// find images which match with that location
GList *_map_location_find_images(dt_location_draw_t *ld)
{
  GList *candidates = dt_location_repository_find_images_for_location(ld->id, ld->data.shape);

  GList *imgs = NULL;
  for(GList *c = candidates; c; c = g_list_next(c))
  {
    const dt_location_candidate_t *cand = (const dt_location_candidate_t *)c->data;
    if(ld->data.shape == MAP_LOCATION_SHAPE_POLYGONS)
    {
      const dt_geo_map_display_point_t pt = { .lon = cand->lon, .lat = cand->lat };
      if(_is_point_in_polygon(&pt, ld->data.plg_pts, ld->data.polygons->data))
        imgs = g_list_prepend(imgs, GINT_TO_POINTER(cand->id));
    }
    else
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(cand->id));
  }
  g_list_free_full(candidates, g_free);

  return imgs;
}

// update image's locations - remove old ones and add new ones
void dt_map_location_update_locations(const int32_t imgid, const GList *tags)
{
  // get current locations
  GList *old_tags = dt_location_repository_get_image_locations(imgid);

  // clean up locations which are not valid anymore
  for(GList *tag = old_tags; tag; tag = g_list_next(tag))
  {
    if(!g_list_find((GList *)tags, tag->data))
    {
      dt_tag_detach(GPOINTER_TO_INT(tag->data), imgid,
      FALSE, FALSE);
    }
  }

  // add new locations
  for(GList *tag = (GList *)tags; tag; tag = g_list_next(tag))
  {
    if(!g_list_find(old_tags, tag->data))
    {
      dt_tag_attach(GPOINTER_TO_INT(tag->data), imgid,
                    FALSE, FALSE);
    }
  }
  g_list_free(old_tags);
  old_tags = NULL;
}

// update location's images - remove old ones and add new ones
gboolean dt_map_location_update_images(dt_location_draw_t *ld)
{
  // get previous images
  GList *imgs = dt_tag_get_images(ld->id);

  // find images in that location
  GList *new_imgs = _map_location_find_images(ld);

  gboolean res = FALSE;
  // detach images which are not in location anymore
  for(GList *img = imgs; img; img = g_list_next(img))
  {
    if(!g_list_find(new_imgs, img->data))
    {
      dt_tag_detach(ld->id, GPOINTER_TO_INT(img->data), FALSE, FALSE);
      res = TRUE;
    }
  }

  // add new images to location
  for(GList *img = new_imgs; img; img = g_list_next(img))
  {
    if(!g_list_find(imgs, img->data))
    {
      dt_tag_attach(ld->id, GPOINTER_TO_INT(img->data), FALSE, FALSE);
      res = TRUE;
    }
  }
  g_list_free(new_imgs);
  new_imgs = NULL;
  g_list_free(imgs);
  imgs = NULL;
  return res;
}

// return root tag for location geotagging
const char *dt_map_location_data_tag_root()
{
  return location_tag;
}

// tell if the point (lon, lat) belongs to location
gboolean dt_map_location_included(const float lon, const float lat,
                                  dt_map_location_data_t *g)
{
  gboolean included = FALSE;
  if((g->shape == MAP_LOCATION_SHAPE_ELLIPSE &&
     (((g->lon - lon) * (g->lon - lon) / (g->delta1 * g->delta1) +
       (g->lat - lat) * (g->lat - lat) / (g->delta2 * g->delta2)) <= 1.0))
     ||
     (g->shape == MAP_LOCATION_SHAPE_RECTANGLE &&
      lon > g->lon - g->delta1 && lon < g->lon + g->delta1 &&
      lat > g->lat - g->delta2 && lat < g->lat + g->delta2))
  {
    included = TRUE;
  }
  return included;
}

// get the map box containing the polygon + flat polygons
GList *dt_map_location_convert_polygons(void *polygons, dt_map_box_t *bbox, int *nb_pts)
{
  const int nb = g_list_length(polygons);
  dt_geo_map_display_point_t *points = malloc(nb * sizeof(dt_geo_map_display_point_t));
  dt_geo_map_display_point_t *p = points;
  dt_map_box_t bb = {180.0, -90.0, -180, 90.0};
  GList *npol = NULL;

  for(GList *pol = polygons; pol; pol = g_list_next(pol), p++)
  {
    dt_geo_map_display_point_t *pt = (dt_geo_map_display_point_t *)pol->data;
    p->lat = pt->lat;
    p->lon = pt->lon;
    npol = g_list_prepend(npol, p);
    if(bbox)
    {
      bb.lon1 = (pt->lon < bb.lon1) ? pt->lon : bb.lon1;
      bb.lon2 = (pt->lon > bb.lon2) ? pt->lon : bb.lon2;
      bb.lat1 = (pt->lat > bb.lat1) ? pt->lat : bb.lat1;
      bb.lat2 = (pt->lat < bb.lat2) ? pt->lat : bb.lat2;
    }
  }
  npol = g_list_reverse(npol);
  if(bbox)
    memcpy(bbox, &bb, sizeof(dt_map_box_t));
  if(nb_pts)
    *nb_pts = nb;
  return (npol);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
