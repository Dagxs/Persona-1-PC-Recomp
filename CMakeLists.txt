cmake_minimum_required(VERSION 3.20)
project(persona1 C)

# gen/ is produced by tools/recomp.py from the player's own Persona.exe (never committed).
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/gen/table.c)
  message(FATAL_ERROR "gen/ is missing: run  python -I tools/recomp.py data/Persona.exe gen")
endif()

set(SDL3_ROOT "C:/msys64/clang64" CACHE PATH "SDL3 install prefix")
find_path(SDL3_INCLUDE SDL3/SDL.h HINTS ${SDL3_ROOT}/include)
find_library(SDL3_LIB NAMES SDL3 SDL3.dll HINTS ${SDL3_ROOT}/lib)

file(GLOB GEN_SRC ${CMAKE_SOURCE_DIR}/gen/*.c)
file(GLOB RT_SRC ${CMAKE_SOURCE_DIR}/runtime/*.c)

set(WARN_OFF -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality -Wno-unused-value
             -Wno-unused-but-set-variable -Wno-misleading-indentation)
# The generated code is big and simple: -O1 compiles much faster than -O2 at little cost.
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-O1;${WARN_OFF}")

add_executable(persona1 ${GEN_SRC} ${RT_SRC})
target_include_directories(persona1 PRIVATE runtime gen ${SDL3_INCLUDE})
target_compile_options(persona1 PRIVATE -std=gnu11 -fno-strict-aliasing -fwrapv -g ${WARN_OFF})
target_link_libraries(persona1 PRIVATE ${SDL3_LIB} m)
if(WIN32)
  # deep guest call chains run on the host stack
  target_link_options(persona1 PRIVATE -Wl,--stack,67108864)
  add_custom_command(TARGET persona1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL3_ROOT}/bin/SDL3.dll $<TARGET_FILE_DIR:persona1>)
endif()
