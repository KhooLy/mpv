cmake_minimum_required(VERSION 3.22.1)
project(mpv_android_test C)

set(MPV_OUT "" CACHE PATH "ci/android/build.sh output directory")
set(MPV_LIB "${MPV_OUT}/${ANDROID_ABI}/libmpv.so")

if(NOT EXISTS "${MPV_LIB}")
    message(FATAL_ERROR "Missing ${MPV_LIB}. Run ci/android/build.sh first.")
endif()

add_library(mpv SHARED IMPORTED)
set_target_properties(mpv PROPERTIES IMPORTED_LOCATION "${MPV_LIB}")

add_library(mpvtest SHARED mpv_jni.c)
target_include_directories(mpvtest PRIVATE "${MPV_OUT}/include")
target_link_libraries(mpvtest mpv log android dl)
