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
#ifndef DT_COMMON_MODULE_VERSIONING_H
#define DT_COMMON_MODULE_VERSIONING_H

/* Module-interface versioning: the DT_MODULE()/DT_MODULE_INTROSPECTION() macros every
 * dynamically-loaded module (iop, lib, view, imageio) must instantiate, and the version
 * they are checked against at dlopen time. Application-free on purpose: module-interface
 * headers include this instead of darktable.h. */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Version of the module interface every dynamically-loaded module is checked
 * against at load time.
 *
 * @warning Bumping it invalidates EVERY module at once: each one reports this value
 * through DT_MODULE() at the version it was compiled with, and the loader refuses any
 * mismatch. Bump it whenever the layout of a struct crossing the module boundary changes,
 * and do not bump it for anything else. A module .so left over from a previous build that
 * still reports the same number will be loaded and will misread the new layout -- silently.
 */
#define DT_MODULE_VERSION 23 // version of dt's module interface


/** @brief Instantiate the version handshake every module must export. Use once, at file
 * scope, in each module's main .c file.
 *
 * @details This DEFINES two functions -- dt_module_dt_version() and
 * dt_module_mod_version() -- rather than declaring anything, so it is a definition site,
 * not a header-style annotation: exactly one per shared object.
 *
 * @param MODVER the module's OWN parameter version, independent of DT_MODULE_VERSION.
 * Bump it when that module's params struct changes, and provide legacy_params() to
 * migrate old history and presets.
 *
 * @note In a _DEBUG build the reported interface version is NEGATED. That is deliberate:
 * debug and release builds lay some shared structs out differently, so a debug module
 * dropped into a release host (or the reverse) reports a version that cannot match and is
 * refused at load, instead of loading and misreading every struct it is handed. If a
 * module mysteriously fails to load, check that it was built in the same configuration as
 * the host before looking anywhere else.
 */
// every module has to define this:
#ifdef _DEBUG
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return -DT_MODULE_VERSION;                                                                               \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#else
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return DT_MODULE_VERSION;                                                                                \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#endif

/** @brief DT_MODULE() for a module whose params struct is introspected.
 *
 * @param MODVER as for DT_MODULE().
 * @param PARAMSTYPE the params struct type.
 *
 * @warning @p PARAMSTYPE is IGNORED by the preprocessor -- this expands to plain
 * DT_MODULE(MODVER). It is read at BUILD time by the introspection generator, which parses
 * the source text and emits the field table into a generated header. Two consequences: the
 * argument must name a type the generator can find by parsing, and a change to that
 * struct's fields or $DEFAULT annotations only takes effect once the generated file is
 * regenerated. A stale generated file compiles cleanly and silently keeps the old defaults.
 */
#define DT_MODULE_INTROSPECTION(MODVER, PARAMSTYPE) DT_MODULE(MODVER)

/** @brief The interface version the HOST expects, in the host's own build configuration.
 * @return DT_MODULE_VERSION, negated in a _DEBUG build. A module loads only when its
 * dt_module_dt_version() returns the same value. */
// ..to be able to compare it against this:
static inline int dt_version()
{
#ifdef _DEBUG
  return -DT_MODULE_VERSION;
#else
  return DT_MODULE_VERSION;
#endif
}

/** @brief The application version, truncated to `<major>.<minor>`.
 * @return a newly-allocated string. **The caller owns it and must g_free() it.** */
char *dt_version_major_minor();

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_MODULE_VERSIONING_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
