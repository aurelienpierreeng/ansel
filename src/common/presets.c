/*
    This file is part of darktable,
    Copyright (C) 2019-2020 Pascal Obry.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2021 Aldric Renaudin.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2022 Aurélien PIERRE.
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

#include "common/presets.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "database/preset_repository.h"
#include "common/exif.h"
#include "libs/lib.h"

#include <libxml/xmlwriter.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>

#include <glib.h>
#include <inttypes.h>

void dt_presets_save_to_file(const int rowid, const char *preset_name, const char *filedir)
{
  dt_preset_t *preset = dt_preset_repository_get_by_rowid(rowid);
  if(IS_NULL_PTR(preset)) return;

  // generate filename based on name of preset
  // convert all characters to underscore which are not allowed in filenames
  gchar *presetname = g_strdup(preset_name);
  gchar *filename = g_strdup_printf("%s/%s.dtpreset", filedir, g_strdelimit(presetname, "/<>:\"\\|*?[]", '_'));
  dt_free(presetname);

  /* The two blobs are base64'd for XML. Both were leaked once per export before -- the
   * encoder allocates and the results went straight into a "%s" argument. */
  char *op_params = dt_exif_xmp_encode(preset->op_params, preset->op_params_size, NULL);
  char *blendop_params = dt_exif_xmp_encode(preset->blendop_params, preset->blendop_params_size, NULL);

  xmlTextWriterPtr writer = xmlNewTextWriterFilename(filename, 0);
  if(IS_NULL_PTR(writer))
  {
    fprintf(stderr, "[dt_presets_save_to_file] Error creating the xml writer\n, path: %s", filename);
    goto cleanup;
  }

  if(xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL) < 0)
  {
    fprintf(stderr, "[dt_presets_save_to_file]: Error on encoding setting");
    goto cleanup;
  }

  xmlTextWriterStartElement(writer, BAD_CAST "darktable_preset");
  xmlTextWriterWriteAttribute(writer, BAD_CAST "version", BAD_CAST "1.0");

  xmlTextWriterStartElement(writer, BAD_CAST "preset");
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "name", "%s", preset->name);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "description", "%s", preset->description);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "operation", "%s", preset->operation);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "op_params", "%s", op_params);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "op_version", "%d", preset->op_version);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "enabled", "%d", preset->enabled);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "autoapply", "%d", preset->autoapply);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "model", "%s", preset->model);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "maker", "%s", preset->maker);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "lens", "%s", preset->lens);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "iso_min", "%f", preset->iso_min);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "iso_max", "%f", preset->iso_max);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "exposure_min", "%f", preset->exposure_min);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "exposure_max", "%f", preset->exposure_max);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "aperture_min", "%f", preset->aperture_min);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "aperture_max", "%f", preset->aperture_max);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "focal_length_min", "%d", preset->focal_length_min);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "focal_length_max", "%d", preset->focal_length_max);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_params", "%s", blendop_params);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_version", "%d", preset->blendop_version);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_priority", "%d", preset->multi_priority);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_name", "%s", preset->multi_name);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "filter", "%d", preset->filter);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "def", "%d", preset->def);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "format", "%d", preset->format);
  xmlTextWriterEndElement(writer);

  xmlTextWriterEndDocument(writer);

cleanup:
  if(writer) xmlFreeTextWriter(writer);
  dt_free(op_params);
  dt_free(blendop_params);
  dt_free(filename);
  dt_preset_free(preset);
}

static gchar *get_preset_element(xmlDocPtr doc, gchar *name)
{
  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
  char xpath[128] = { 0 };
  snprintf(xpath, sizeof(xpath), "//%s", name);
  gchar *result = NULL;

  xmlXPathObjectPtr xpathObj =
    xmlXPathEvalExpression((const xmlChar *)xpath, xpathCtx);

  if(xpathObj)
  {
    const xmlNodeSetPtr xnodes = xpathObj->nodesetval;
    if(xnodes->nodeTab)
    {
      const xmlNodePtr xnode = xnodes->nodeTab[0];
      xmlChar *value = xmlNodeListGetString(doc, xnode->xmlChildrenNode, 1);

      if(value)
        result = g_strdup((gchar *)value);
      else
        result = g_strdup("");

      xmlFree(value);
    }
    xmlXPathFreeObject(xpathObj);
  }

  xmlXPathFreeContext(xpathCtx);
  return result;
}

static int get_preset_element_int(xmlDocPtr doc, gchar *name)
{
  gchar *value = get_preset_element(doc, name);
  const int result = value ? atoi(value) : 0;
  dt_free(value);
  return result;
}

static int get_preset_element_float(xmlDocPtr doc, gchar *name)
{
  gchar *value = get_preset_element(doc, name);
  const float result = value ? atof(value) : 0.0f;
  dt_free(value);
  return result;
}

int dt_presets_import_from_file(const char *preset_path)
{
  xmlDocPtr doc = xmlParseFile(preset_path);
  if(IS_NULL_PTR(doc))
    return FALSE;

  xmlNodePtr root = xmlDocGetRootElement(doc);
  if(IS_NULL_PTR(root) || xmlStrcmp(root->name, BAD_CAST "darktable_preset") != 0)
  {
    xmlFreeDoc(doc);
    return FALSE;
  }

  dt_preset_t preset = { 0 };
  preset.name = get_preset_element(doc, "name");
  preset.description = get_preset_element(doc, "description");
  preset.operation = get_preset_element(doc, "operation");
  preset.autoapply = get_preset_element_int(doc, "autoapply");
  preset.model = get_preset_element(doc, "model");
  preset.maker = get_preset_element(doc, "maker");
  preset.lens = get_preset_element(doc, "lens");
  preset.iso_min = get_preset_element_float(doc, "iso_min");
  preset.iso_max = get_preset_element_float(doc, "iso_max");
  preset.exposure_min = get_preset_element_float(doc, "exposure_min");
  preset.exposure_max = get_preset_element_float(doc, "exposure_max");
  preset.aperture_min = get_preset_element_float(doc, "aperture_min");
  preset.aperture_max = get_preset_element_float(doc, "aperture_max");
  preset.focal_length_min = get_preset_element_int(doc, "focal_length_min");
  preset.focal_length_max = get_preset_element_int(doc, "focal_length_max");
  preset.op_version = get_preset_element_int(doc, "op_version");
  preset.blendop_version = get_preset_element_int(doc, "blendop_version");
  preset.enabled = get_preset_element_int(doc, "enabled");
  preset.multi_priority = get_preset_element_int(doc, "multi_priority");
  preset.multi_name = get_preset_element(doc, "multi_name");
  preset.filter = get_preset_element_int(doc, "filter");
  preset.def = get_preset_element_int(doc, "def");
  preset.format = get_preset_element_int(doc, "format");

  gchar *op_params = get_preset_element(doc, "op_params");
  gchar *blendop_params = get_preset_element(doc, "blendop_params");
  xmlFreeDoc(doc);

  /* dt_exif_xmp_decode() returns memory the caller owns; the struct borrows it for the
   * length of the insert and it is released below, not by dt_preset_free() -- `preset` is
   * a stack value, not one the repository handed out. */
  if(op_params)
    preset.op_params = (void *)dt_exif_xmp_decode(op_params, strlen(op_params), &preset.op_params_size);
  if(blendop_params)
    preset.blendop_params
        = (void *)dt_exif_xmp_decode(blendop_params, strlen(blendop_params), &preset.blendop_params_size);

  const int result = dt_preset_repository_insert(&preset) ? 1 : 0;

  dt_free(preset.name);
  dt_free(preset.description);
  dt_free(preset.operation);
  dt_free(preset.model);
  dt_free(preset.maker);
  dt_free(preset.lens);
  dt_free(preset.multi_name);
  dt_free(preset.op_params);
  dt_free(preset.blendop_params);
  dt_free(op_params);
  dt_free(blendop_params);

  return result;
}

gboolean dt_presets_module_can_autoapply(const gchar *operation)
{
  for(const GList *lib_modules = dt_lib_get_global()->plugins; lib_modules; lib_modules = g_list_next(lib_modules))
  {
    dt_lib_module_t *lib_module = (dt_lib_module_t *)lib_modules->data;
    if(!strcmp(lib_module->plugin_name, operation))
    {
      return dt_lib_presets_can_autoapply(lib_module);
    }
  }
  return TRUE;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
