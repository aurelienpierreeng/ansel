#
# install opencl kernel source files
#
# common.h belongs with the other kernel HEADERS below, not with the programs. It is
# #included by 31 of the 34 .cl files, so its contents are already compiled 31 times over;
# listing it here additionally test-compiled it as a translation unit of its own, which is
# not how it is ever used and which warns "#pragma once in main file". Both lists install to
# the same directory, so this changes what gets test-compiled and nothing else.
FILE(GLOB DT_OPENCL_KERNELS "*.cl")
FILE(GLOB DT_OPENCL_EXTRA "programs.conf" "common.h" "compensated.h" "hl_real.h" "rgb_norms.h" "noise_generator.h" "color_conversion.h" "colorspace.h")

# The lens kernels evaluate LensSerious's closed forms directly, from the very header
# liblensserious compiles as C99 -- one source text for CPU and GPU (see
# src/external/LensSerious/include/lensserious_eval.h). It has to sit beside the kernels
# at run time: dt_opencl_init() passes exactly ONE -I to the driver, the installed kernel
# directory, so an #include can reach nothing else.
set(LENSSERIOUS_EVAL_HEADER "${CMAKE_SOURCE_DIR}/src/external/LensSerious/include/lensserious_eval.h")
configure_file(${LENSSERIOUS_EVAL_HEADER} ${CMAKE_CURRENT_BINARY_DIR}/lensserious_eval.h COPYONLY)
list(APPEND DT_OPENCL_EXTRA ${CMAKE_CURRENT_BINARY_DIR}/lensserious_eval.h)

add_custom_target(testcompile_opencl_kernels ALL)

# A test-compile depends on every kernel header, not only on its .cl: compensated.h and
# hl_real.h are code whose correctness hangs on the order of operations, and an edit there
# must re-run the check.
FILE(GLOB DT_OPENCL_HEADERS "*.h")

# SUFFIX names a variant of the same program; any further argument is passed to the compiler.
macro(testcompile_opencl_kernel IN SUFFIX)
  get_filename_component(KERNAME ${IN} NAME)

  set(TOUCH "${CMAKE_CURRENT_BINARY_DIR}/${KERNAME}${SUFFIX}.touch")

  set(KERNAME_OUT "opencl-program-${KERNAME}${SUFFIX}")

  add_custom_command(
    OUTPUT  ${TOUCH}
    COMMAND ${CLANG_OPENCL_COMPILER} -cc1 -cl-std=CL1.2 -isystem ${CLANG_OPENCL_INCLUDE_DIR} -D__IMAGE_SUPPORT__=1 -finclude-default-header -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_BINARY_DIR} ${ARGN} ${IN}
    COMMAND ${CMAKE_COMMAND} -E touch ${TOUCH} # will be empty!
    DEPENDS ${IN} ${DT_OPENCL_HEADERS}
    COMMENT "Test-compiling OpenCL program ${KERNAME}${SUFFIX}"
  )

  add_custom_target(
    ${KERNAME_OUT}
    DEPENDS ${TOUCH} # will be empty!
    DEPENDS ${IN}
  )

  add_dependencies(testcompile_opencl_kernels ${KERNAME_OUT})
endmacro(testcompile_opencl_kernel)

if(TESTBUILD_OPENCL_PROGRAMS)
  foreach(KERNEL IN ITEMS ${DT_OPENCL_KERNELS})
    testcompile_opencl_kernel(${KERNEL} "")
  endforeach()
  # The host passes -DDT_DEVICE_FP64=1 to a device with real fp64 (common/opencl.c), which
  # selects the native-double half of hl_real.h -- what every NVIDIA and AMD card runs. Without
  # this second pass that half would only ever be compiled by the users' drivers. The host
  # target of `clang -cc1` has no fp64, so the extension is declared the way a driver would.
  testcompile_opencl_kernel(${CMAKE_CURRENT_SOURCE_DIR}/highlights_sparse.cl "-fp64" -cl-ext=+cl_khr_fp64 -DDT_DEVICE_FP64=1)
endif()

install(FILES ${DT_OPENCL_KERNELS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/kernels COMPONENT DTApplication)

install(FILES ${DT_OPENCL_EXTRA} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/kernels COMPONENT DTApplication)
