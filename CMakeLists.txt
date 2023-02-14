cmake_minimum_required(VERSION 3.19)

set(PROJECT_NAME tablet_wallpaper)

project(${PROJECT_NAME} C)

#set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_C_STANDARD 11)

set(WIN32_UNICODE true)
set(WIN32_LINK_SUBSYS window)

set(JJCOM_HAVE_CJSON 1)
#set(JJCOM_HAVE_SOCKET 1)
set(JJCOM_HAVE_ICONV 1)
set(JJCOM_HAVE_MATH 1)
#set(JJCOM_HAVE_UUID 1)

if (STATIC_BUILD)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --static --static-gcc ")
endif()

if (WIN32 AND MINGW)
        if (WIN32_UNICODE)
                set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -municode -DUNICODE -D_UNICODE ")
        endif()

        if (${WIN32_LINK_SUBSYS} STREQUAL "window")
                set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mwindows -DSUBSYS_WINDOW ")
        endif()

        if (${WIN32_LINK_SUBSYS} STREQUAL "console")
                set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mconsole ")
        endif()
endif()

set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -Wall -Wextra ")

if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g3 -ggdb3 -gdwarf-4 -O0 ")
elseif(${CMAKE_BUILD_TYPE} STREQUAL "Release")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 ")
endif()

set(SOURCE_FILES
    src/main.c
    )

set(APPRES_OBJS)
if (WIN32 AND MINGW)
        set(WINRES_OUT ${CMAKE_BINARY_DIR}/appres.o)

        add_custom_command(
                OUTPUT ${WINRES_OUT}
                COMMAND windres ${CMAKE_SOURCE_DIR}/asset/app.rc ${WINRES_OUT}
        )

        add_custom_target(WinResObj ALL DEPENDS ${WINRES_OUT})

        set(APPRES_OBJS ${WINRES_OUT})
endif()

add_executable(${PROJECT_NAME} ${APPRES_OBJS})
target_sources(${PROJECT_NAME} PRIVATE ${SOURCE_FILES})

add_subdirectory(lib/libjj)

target_include_directories(${PROJECT_NAME} PUBLIC ${IMGMAGICK_INCLUDE_DIRS})

target_include_directories(${PROJECT_NAME} PUBLIC asset)
target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PUBLIC lib)
target_include_directories(${PROJECT_NAME} PUBLIC lib/GraphicsMagick/include)
target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR})

target_link_directories(${PROJECT_NAME} PUBLIC lib/GraphicsMagick/lib)

target_link_libraries(${PROJECT_NAME} jjcom)
target_link_libraries(${PROJECT_NAME} ntdll)
target_link_libraries(${PROJECT_NAME} ntoskrnl)
target_link_libraries(${PROJECT_NAME} user32)
target_link_libraries(${PROJECT_NAME} GraphicsMagickWand GraphicsMagick++ GraphicsMagick bz2 z gomp jpeg png16 webp webpmux jasper)

set(INSTALL_DEST "Build-${CMAKE_BUILD_TYPE}")

install(TARGETS ${PROJECT_NAME} DESTINATION "${INSTALL_DEST}")

add_custom_target(collect_dll
                  COMMAND ${MSYS2_HOME}/usr/bin/bash.exe "${PROJECT_SOURCE_DIR}/collect_dll.sh" "${PROJECT_NAME}.exe" "${PROJECT_SOURCE_DIR}/lib"
                  WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
                  DEPENDS ${PROJECT_NAME}
                  )

message(STATUS  "CMake Config: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Compiler: " ${CMAKE_C_COMPILER})
message(STATUS "C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "Source Directory: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "External Object: ${APPRES_OBJS}")
message(STATUS "Install destination: " ${INSTALL_DEST})
