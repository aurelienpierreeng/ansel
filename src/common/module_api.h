/*
    This file is part of darktable,
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2010-2012, 2014 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Diederik Ter Rahe.
    Copyright (C) 2022 Martin Bařinka.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/* NO #include may be added to this file, and it deliberately has NO include guard:
 * it is an X-macro header, re-included several times with different macros defined
 * (INCLUDE_API_FROM_MODULE_LOAD, ...) and expanded INSIDE struct bodies to generate
 * members. An include at the top of it lands inside those structs.
 * dt_version()/dt_print()/IS_NULL_PTR used below must therefore be provided by each
 * consuming .c (common/module_versioning.h, common/logging.h, system/macros.h). */

#include <glib.h>

#undef OPTIONAL
#undef REQUIRED
#undef DEFAULT

#undef FULL_API_H

#ifdef INCLUDE_API_FROM_MODULE_LOAD
  #define OPTIONAL(return_type, function_name, ...) \
      if(!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name))) \
          module->function_name = NULL
  #define REQUIRED(return_type, function_name, ...) \
      if(!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name))) \
          goto api_h_error
  #define DEFAULT(return_type, function_name, ...) \
      if(!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name))) \
          module->function_name = default_ ## function_name

  dt_print(DT_DEBUG_CONTROL, "[" INCLUDE_API_FROM_MODULE_LOAD "] loading `%s' from %s\n", module_name, libname);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(IS_NULL_PTR(module->module)) goto api_h_error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer) & (version))) goto api_h_error;
  if(version() != dt_version())
  {
    fprintf(stderr,
            "[" INCLUDE_API_FROM_MODULE_LOAD "] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n",
            libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()),
            dt_version() < 0 ? "debug" : "opt");
    goto api_h_error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version", (gpointer) & (module->version))) goto api_h_error;

  goto skip_error;
api_h_error:
  fprintf(stderr, "[" INCLUDE_API_FROM_MODULE_LOAD "] failed to open `%s': %s\n", module_name, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
skip_error:
  #undef INCLUDE_API_FROM_MODULE_LOAD
#elif defined(INCLUDE_API_FROM_MODULE_H)
  #define OPTIONAL(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
  #define REQUIRED(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
  #define DEFAULT(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
  int (*version)();
  #undef INCLUDE_API_FROM_MODULE_H
#elif defined(INCLUDE_API_FROM_MODULE_STATIC)
  /* Statically-linked modules: bind the module's own entry points into its
   * dt_..._module_so_t. Expanded INSIDE the module's own translation unit, where the
   * plain API names are in scope and carry this module's asm label (see FULL_API_H
   * below), so `module->process = process` stores the address of THIS module's process.
   *
   * DT_MODULE_HAS_<fn> answers what g_module_symbol() used to answer at dlopen time:
   * whether the module defines this entry point at all. The generated presence header
   * defines one for EVERY name in the API, 0 or 1 -- a missing one is a compile error
   * (DT_MODULE_PICK_DT_MODULE_HAS_foo is not a macro), never a silent NULL.
   *
   * DEFAULT's fallback is NOT applied here: the default_<fn> implementations are static
   * to develop/imageop.c. INCLUDE_API_FROM_MODULE_STATIC_DEFAULTS, expanded there, fills
   * them in afterwards. REQUIRED takes the symbol unconditionally, so a module missing
   * one fails to link instead of failing to load. */
  #define DT_MODULE_PICK_0(fn, fb) fb
  #define DT_MODULE_PICK_1(fn, fb) fn
  #define DT_MODULE_PICK_C(h, fn, fb) DT_MODULE_PICK_ ## h(fn, fb)
  #define DT_MODULE_PICK_B(h, fn, fb) DT_MODULE_PICK_C(h, fn, fb)
  #define DT_MODULE_PICK(fn, fb) DT_MODULE_PICK_B(DT_MODULE_HAS_ ## fn, fn, fb)
  #define OPTIONAL(return_type, function_name, ...) module->function_name = DT_MODULE_PICK(function_name, NULL)
  #define REQUIRED(return_type, function_name, ...) module->function_name = function_name
  #define DEFAULT(return_type, function_name, ...) module->function_name = DT_MODULE_PICK(function_name, NULL)
  #undef INCLUDE_API_FROM_MODULE_STATIC
#elif defined(INCLUDE_API_FROM_MODULE_STATIC_DEFAULTS)
  /* Second half of the static bind, expanded where the default_<fn> fallbacks are in
   * scope. Only DEFAULT entries have one; OPTIONAL legitimately stays NULL and REQUIRED
   * was already bound. */
  #define OPTIONAL(return_type, function_name, ...) (void)0
  #define REQUIRED(return_type, function_name, ...) (void)0
  #define DEFAULT(return_type, function_name, ...) \
      if(IS_NULL_PTR(module->function_name)) module->function_name = default_ ## function_name
  #undef INCLUDE_API_FROM_MODULE_STATIC_DEFAULTS
#elif defined(INCLUDE_API_FROM_MODULE_LOAD_BY_SO)
  #define OPTIONAL(return_type, function_name, ...) module->function_name = so->function_name
  #define REQUIRED(return_type, function_name, ...) module->function_name = so->function_name
  #define DEFAULT(return_type, function_name, ...) module->function_name = so->function_name
  #undef INCLUDE_API_FROM_MODULE_LOAD_BY_SO
#else
  #define FULL_API_H
  /* Symbol namespacing for statically-linked modules.
   *
   * Every module defines the same ~30 entry points -- `process', `name', `commit_params'.
   * That sameness IS the API: it is what makes a module read like an implementation of an
   * abstract class, and it is not negotiable. It is also, once the modules stop being one
   * shared object each and land in a single link, ~30 duplicate symbols per module.
   *
   * An asm label renames the emitted SYMBOL and leaves the identifier alone, so the module
   * source keeps writing `int process(...)' and nothing substitutes tokens: struct members,
   * locals and `->name' are untouched by construction, which a `#define process ...' could
   * not promise. The label attaches here, on the first declaration, and the definition in
   * the module source inherits it. Re-declaring it identically (iop_api.h is re-included by
   * most modules, by design -- it has no guard) is accepted by GCC and Clang alike.
   *
   * DT_MODULE_SYMBOL_PREFIX is set per module by the build. Undefined -- for src/libs and
   * src/views, still one shared object each and still dlopen'd -- this is a no-op and the
   * symbols keep their plain names.
   *
   * Mach-O prefixes symbols with an underscore and an asm label is emitted VERBATIM, so the
   * label has to carry it; ELF and PE/COFF x86-64 do not. */
  #ifdef DT_MODULE_SYMBOL_PREFIX
    #define DT_MODULE_SYM_STR_(x) #x
    #define DT_MODULE_SYM_STR(x) DT_MODULE_SYM_STR_(x)
    #define DT_MODULE_SYM_CAT_(a, b) a ## b
    #define DT_MODULE_SYM_CAT(a, b) DT_MODULE_SYM_CAT_(a, b)
    #ifdef __APPLE__
      #define DT_MODULE_SYM(fn) __asm__("_" DT_MODULE_SYM_STR(DT_MODULE_SYM_CAT(DT_MODULE_SYMBOL_PREFIX, fn)))
    #else
      #define DT_MODULE_SYM(fn) __asm__(DT_MODULE_SYM_STR(DT_MODULE_SYM_CAT(DT_MODULE_SYMBOL_PREFIX, fn)))
    #endif
  #else
    #define DT_MODULE_SYM(fn)
  #endif
  #define OPTIONAL(return_type, function_name, ...) return_type function_name(__VA_ARGS__) DT_MODULE_SYM(function_name)
  #define REQUIRED(return_type, function_name, ...) return_type function_name(__VA_ARGS__) DT_MODULE_SYM(function_name)
  #define DEFAULT(return_type, function_name, ...) return_type function_name(__VA_ARGS__) DT_MODULE_SYM(function_name)
  #ifdef __cplusplus
  extern "C" {
  #endif
  // these 2 functions are defined by DT_MODULE() macro.
  #pragma GCC visibility push(default)
  // returns the version of dt's module interface at the time this module was build
  int dt_module_dt_version() DT_MODULE_SYM(dt_module_dt_version);
  // returns the version of this module
  int dt_module_mod_version() DT_MODULE_SYM(dt_module_mod_version);
  #pragma GCC visibility pop
  #ifdef __cplusplus
  }
  #endif
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

