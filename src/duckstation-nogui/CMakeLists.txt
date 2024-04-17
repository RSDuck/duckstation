add_executable(duckstation-nogui
  nogui_host.cpp
  nogui_host.h
  nogui_platform.h
)

target_precompile_headers(duckstation-nogui PRIVATE "pch.h")
target_link_libraries(duckstation-nogui PRIVATE core util common imgui scmversion)

add_core_resources(duckstation-nogui)

if(NINTENDO_SWITCH)
  enable_language(ASM)

  target_sources(duckstation-nogui PRIVATE
    switch_nogui_platform.cpp
    switch_nogui_platform.h
    switch_asm.S
  )
  target_link_libraries(duckstation-nogui PUBLIC "${NX_LIBRARIES}")

  nx_generate_nacp (duckstation.nacp
    NAME    "duckstation"
    AUTHOR  "Stenzek, RSDuck (port)"
    VERSION "pre-release 1"
  )

  # Create folder early otherwise the CMake script complains
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/romfs)

  if (CMAKE_BUILD_TYPE STREQUAL Debug)
    nx_create_nro(duckstation-nogui
      OUTPUT ${CMAKE_BINARY_DIR}/duckstation.nro
      NACP  duckstation.nacp
      ICON  "${CMAKE_SOURCE_DIR}/extras/icons/icon-switch.jpg"
    )
  else()
    nx_create_nro(duckstation-nogui
      OUTPUT ${CMAKE_BINARY_DIR}/duckstation.nro
      NACP  duckstation.nacp
      ICON  "${CMAKE_SOURCE_DIR}/extras/icons/icon-switch.jpg"
      ROMFS ${CMAKE_BINARY_DIR}/romfs
    )
  endif()
endif()
