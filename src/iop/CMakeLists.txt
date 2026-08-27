include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include iop/iop_api.h)

set(_iop_list "")
set(_iop_default_visible_list "")

# parameters:
# 1. iop name
# 2. iop main source file
# and after that you may pass additional iop source files
# and DEFAULT_VISIBLE keyword - which means that the iop will be visible/listed
# by default (user can manipulate iop visibility via "more modules" lib)
macro(add_iop _lib _src)
  string(LENGTH ${_lib} _lib_namelength)
  if(${_lib_namelength} GREATER 19)
    message(FATAL_ERROR "IOP name \"${_lib}\" is too long (${_lib_namelength} symbols), it should not be greater than 19 symbols.")
  endif(${_lib_namelength} GREATER 19)

  set(_input ${CMAKE_CURRENT_SOURCE_DIR}/${_src})
  set(_output ${CMAKE_CURRENT_BINARY_DIR}/introspection_${_src}) # keep ${_src} in the end to keep the file extension (c vs. cc)
  add_custom_command(
    # Depend on the WHOLE generator, not just ast.pm: editing scanner.pm or code_gen.pm used
    # to leave every introspection_*.c stale, so a generator fix appeared to do nothing.
    DEPENDS ${_input} ${CMAKE_SOURCE_DIR}/src/common/introspection.h
            ${CMAKE_SOURCE_DIR}/tools/introspection/parser.pl
            ${CMAKE_SOURCE_DIR}/tools/introspection/ast.pm
            ${CMAKE_SOURCE_DIR}/tools/introspection/parser.pm
            ${CMAKE_SOURCE_DIR}/tools/introspection/scanner.pm
            ${CMAKE_SOURCE_DIR}/tools/introspection/code_gen.pm
    OUTPUT ${_output}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/introspection/
    COMMAND ${perl_BIN} ${CMAKE_SOURCE_DIR}/tools/introspection/parser.pl ${CMAKE_CURRENT_SOURCE_DIR}/../ ${_input} ${_output}
  )

  # do not want compiler to look for "DEFAULT_VISIBLE"
  set(OPT_SRC "${ARGN}")
  list(REMOVE_ITEM OPT_SRC "DEFAULT_VISIBLE")

  set_source_files_properties(${_input} PROPERTIES LANGUAGE "")

  # IOP modules are compiled into lib_ansel, not dlopen'd from a plugins directory. See
  # tools/generate_iop_static.py for why, and doc/static-iop.md for the whole decision.
  #
  # Two files are generated per module. ${_lib}_present.h answers, per entry point, the
  # question g_module_symbol() used to answer at load time -- does this module define it?
  # -- by running this compiler's preprocessor over these sources with these flags.
  # ${_lib}_static.c is the resulting dt_iop_module_so_t filler.
  set(_present ${CMAKE_CURRENT_BINARY_DIR}/${_lib}_present.h)
  set(_fill ${CMAKE_CURRENT_BINARY_DIR}/${_lib}_static.c)

  # Scan the generated introspection wrapper -- which #includes the module source, and adds
  # introspection_init()/get_p()/get_f()/get_introspection*() itself -- plus every secondary
  # source, since an entry point may live in one (iop/highlights/, iop/drawlayer/) and one
  # the generator missed would be a silently NULL function pointer, not a link error.
  set(_scan_sources ${_output})
  foreach(_extra IN LISTS OPT_SRC)
    list(APPEND _scan_sources ${CMAKE_CURRENT_SOURCE_DIR}/${_extra})
  endforeach()

  add_library(${_lib} OBJECT ${_output} ${_fill} ${OPT_SRC} ${_input})
  # NOT lib_ansel: these objects are compiled INTO it, and naming it here would be a
  # dependency cycle. Everything the modules actually took from lib_ansel was its PUBLIC
  # usage requirements -- ansel_deps and OpenMP -- so take those directly. Its symbols
  # need no linking now that the objects live inside it.
  target_link_libraries(${_lib} PRIVATE ansel_deps)
  if(OpenMP_C_FOUND)
    target_link_libraries(${_lib} PRIVATE OpenMP::OpenMP_C)
  endif()
  if(OpenMP_CXX_FOUND)
    target_link_libraries(${_lib} PRIVATE OpenMP::OpenMP_CXX)
  endif()

  # This is what makes every module's `process' a distinct symbol while every module's
  # source still writes plain `int process(...)'. See common/module_api.h.
  target_compile_definitions(${_lib} PRIVATE DT_MODULE_SYMBOL_PREFIX=dt_iop_${_lib}__)
  target_include_directories(${_lib} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

  add_custom_command(
    OUTPUT ${_present} ${_fill}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generate_iop_static.py module
            --module ${_lib}
            --module-dir ${CMAKE_CURRENT_SOURCE_DIR}
            --api ${CMAKE_CURRENT_SOURCE_DIR}/iop_api.h
            --present ${_present}
            --fill ${_fill}
            --cc ${CMAKE_C_COMPILER}
            --sources ${_scan_sources}
            --
              "-I$<JOIN:$<TARGET_PROPERTY:${_lib},INCLUDE_DIRECTORIES>,;-I>"
              "$<$<BOOL:$<TARGET_PROPERTY:${_lib},COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:${_lib},COMPILE_DEFINITIONS>,;-D>>"
              "$<TARGET_PROPERTY:${_lib},COMPILE_OPTIONS>"
    DEPENDS ${_scan_sources} ${_input} ${CMAKE_SOURCE_DIR}/tools/generate_iop_static.py
            ${CMAKE_CURRENT_SOURCE_DIR}/iop_api.h ${CMAKE_CURRENT_SOURCE_DIR}/../common/module_api.h
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Scanning iop/${_lib} for its API surface"
  )

  # Referencing the objects from lib_ansel is what links them in. lib_ansel is a shared
  # library: on Windows it cannot leave symbols for the executable to resolve, so the
  # modules have to live inside it rather than beside it.
  target_sources(lib_ansel PRIVATE $<TARGET_OBJECTS:${_lib}>)

  list(APPEND _iop_list ${_lib})

  # find whether magic keyword DEFAULT_VISIBLE was passed
  # it defines whether the iop will be listed by default
  cmake_parse_arguments(add_iop "DEFAULT_VISIBLE" "" "" ${ARGN})
  if(${add_iop_DEFAULT_VISIBLE})
    list(APPEND _iop_default_visible_list ${_lib})
  endif(${add_iop_DEFAULT_VISIBLE})
endmacro(add_iop)

# @@_NEW_MODULE: when adding a new module here, please grep @@_NEW_MODULE in the code and adjust accordingly.

# add_iop(useless "useless.c")
add_iop(basebuffer "basebuffer.c")
add_iop(rawprepare "rawprepare.c")
add_iop(soften "soften.c")
add_iop(bloom "bloom.c")
add_iop(highpass "highpass.c")
add_iop(lowpass "lowpass.c")
add_iop(shadhi "shadhi.c")
add_iop(colorreconstruct "colorreconstruction.c")
add_iop(tonemap "tonemap.cc")
add_iop(tonecurve "tonecurve.c")
add_iop(colisa "colisa.c")
add_iop(gamma "gamma.c")
add_iop(temperature "temperature.c" DEFAULT_VISIBLE)
add_iop(colorcorrection "colorcorrection.c")
add_iop(basicadj "basicadj.c")
add_iop(exposure "exposure.c" DEFAULT_VISIBLE)
add_iop(rgbcurve "rgbcurve.c")
add_iop(colorbalance "colorbalance.c" DEFAULT_VISIBLE)
add_iop(colorin "colorin.c" DEFAULT_VISIBLE)
add_iop(colorout "colorout.c")
add_iop(colorchecker "colorchecker.c")
add_iop(clipping "clipping.c" DEFAULT_VISIBLE)
add_iop(crop "crop.c" DEFAULT_VISIBLE)
add_iop(sharpen "sharpen.c" DEFAULT_VISIBLE)
add_iop(dither "dither.c")
add_iop(monochrome "monochrome.c")
add_iop(basecurve "basecurve.c" DEFAULT_VISIBLE)
add_iop(colorzones "colorzones.c")
add_iop(highlights "highlights.c"
        "highlights/gather.c"
        "highlights/clip.c"
        "highlights/lch.c"
        "highlights/inpaint.c"
        "highlights/laplacian.c"
        "highlights/blur.c"
        "highlights/segmentation.c"
        "highlights/pde.c"
        "highlights/knee.c"
        "highlights/dome.c"
        "highlights/chroma.c"
        "highlights/coefficient_field.c"
        "highlights/core.c"
        "highlights/region.c"
        "highlights/process.c"
        "highlights/selftests.c"
        DEFAULT_VISIBLE)
add_iop(rawoverexposed "rawoverexposed.c")
add_iop(velvia "velvia.c")
add_iop(vignette "vignette.c")
add_iop(splittoning "splittoning.c")
add_iop(grain "grain.c")
add_iop(crystgrain "crystgrain.c")
add_iop(bilateral "bilateral.cc")
add_iop(profile_gamma "profile_gamma.c")
add_iop(colormapping "colormapping.c")
add_iop(channelmixer "channelmixer.c")
add_iop(graduatednd "graduatednd.c")
add_iop(relight "relight.c")
add_iop(zonesystem "zonesystem.c")
add_iop(demosaic "demosaic.c" "demosaic/amaze.cc" DEFAULT_VISIBLE)
add_iop(detailmask "detailmask.c")
add_iop(rotatepixels "rotatepixels.c")
add_iop(scalepixels "scalepixels.c")
add_iop(atrous "atrous.c")
add_iop(cacorrect "cacorrect.c")
add_iop(overexposed "overexposed.c")
add_iop(hotpixels "hotpixels.c")
add_iop(lowlight "lowlight.c")
add_iop(spots "spots.c")
add_iop(retouch "retouch.c")
add_iop(liquify "liquify.c")
add_iop(rawdenoise "rawdenoise.c")
add_iop(rawdenoiseai "rawdenoiseai.c")
add_iop(borders "borders.c")
add_iop(nlmeans "nlmeans.c")
add_iop(colorcontrast "colorcontrast.c")
add_iop(levels "levels.c")
add_iop(rgblevels "rgblevels.c" DEFAULT_VISIBLE)
add_iop(colorize "colorize.c")
add_iop(invert "invert.c")
add_iop(vibrance "vibrance.c")
add_iop(flip "flip.c" DEFAULT_VISIBLE)
add_iop(finalscale "finalscale.c")
add_iop(globaltonemap "globaltonemap.c")
add_iop(bilat "bilat.c" DEFAULT_VISIBLE)
add_iop(denoiseprofile "denoiseprofile.c" DEFAULT_VISIBLE)
add_iop(defringe "defringe.c")
add_iop(ashift "ashift.c")
add_iop(hazeremoval "hazeremoval.c" DEFAULT_VISIBLE)
add_iop(filmic "filmic.c")
add_iop(mask_manager "shape_manager.c")
add_iop(lut3d "lut3d.c")
add_iop(toneequal "toneequal.c" DEFAULT_VISIBLE)
add_iop(filmicrgb "filmicrgb.c")
add_iop(negadoctor "negadoctor.c")
add_iop(channelmixerrgb "channelmixerrgb.c" "../math/homography.c")
add_iop(censorize "censorize.c")
add_iop(splittoningrgb "splittoningrgb.c")
add_iop(colorprimaries "colorprimaries.c")
add_iop(colorbalancergb "colorbalancergb.c")
add_iop(colorequal "colorequal.c")
add_iop(drawlayer "drawlayer.c" "drawlayer/brush.c" "drawlayer/cache.c" "drawlayer/io.c" "drawlayer/paint.c" "drawlayer/runtime.c" "drawlayer/widgets.c" DEFAULT_VISIBLE)
add_iop(cacorrectrgb "cacorrectrgb.c")
add_iop(diffuse "diffuse.c")
add_iop(blurs "blurs.c")
add_iop(initialscale "initialscale.c")
add_iop(watermark "watermark.c")
add_iop(lens "lens.c" DEFAULT_VISIBLE)
target_link_libraries(lens PRIVATE lensserious) # headers; symbols come via lib_ansel
if(TARGET lensserious_db)
  target_link_libraries(lens PRIVATE lensserious_db) # ditto, for the calibration reader
endif()

# fix for Mac when OpenMP is only available in C compiler
if(APPLE)
  set_target_properties(demosaic PROPERTIES LINKER_LANGUAGE C)
endif(APPLE)

# Shared implementation used by more than one module. It used to be #include'd textually
# into each consumer, because two shared objects could not each carry a copy of its globals.
# With one link there is one copy, so it is a translation unit again.
add_library(iop_shared_channelmixer OBJECT channelmixerrgb_shared.c)
target_link_libraries(iop_shared_channelmixer PRIVATE ansel_deps)
if(OpenMP_C_FOUND)
  target_link_libraries(iop_shared_channelmixer PRIVATE OpenMP::OpenMP_C)
endif()
target_sources(lib_ansel PRIVATE $<TARGET_OBJECTS:iop_shared_channelmixer>)

# The registry of every module built into this binary, generated at CONFIGURE time: the
# module list is a CMake variable, so there is nothing to discover later, and this keeps
# lib_ansel's source list free of a cross-directory build-time dependency.
set(_iop_registry ${CMAKE_CURRENT_BINARY_DIR}/iop_registry.c)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generate_iop_static.py registry
          --out ${_iop_registry} --modules ${_iop_list}
  RESULT_VARIABLE _iop_registry_result
)
if(NOT _iop_registry_result EQUAL 0)
  message(FATAL_ERROR "Failed to generate the IOP registry (${_iop_registry_result})")
endif()
target_sources(lib_ansel PRIVATE ${_iop_registry})

list(LENGTH _iop_list _iop_count)
message(STATUS "IOP modules linked into lib_ansel: ${_iop_count}")

set_property(GLOBAL PROPERTY DT_PLUGIN_IOPS ${_iop_list})
set_property(GLOBAL PROPERTY DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT ${_iop_default_visible_list})
