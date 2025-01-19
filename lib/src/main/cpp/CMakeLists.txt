cmake_minimum_required(VERSION 3.22.1)

project(TiffBitmapFactory)

include_directories("${CMAKE_SOURCE_DIR}/include")
include_directories("${CMAKE_SOURCE_DIR}/../../../../3rd-party/jpeg-9d/include")
include_directories("${CMAKE_SOURCE_DIR}/../../../../3rd-party/tiff-4.7.0/include")

add_library(imageOps
             SHARED
             src/NativeExceptions.cpp
             src/NativeDecoder.cpp
             src/NativeTiffBitmapFactory.cpp
             src/NativeTiffSaver.cpp)

find_library(log-lib log)
target_link_libraries(imageOps PRIVATE ${log-lib})

find_library(jnigraphics-lib jnigraphics)
target_link_libraries(imageOps PRIVATE -ljnigraphics)

add_library(ZLIB SHARED IMPORTED)
set_target_properties(ZLIB PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}/libz.so")

add_library(JPEGLIB SHARED IMPORTED)
set_target_properties(JPEGLIB PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}/libjpeg.so")

add_library(TIFFLIB SHARED IMPORTED)
set_target_properties(TIFFLIB PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}/libtiff.so")

target_link_libraries(imageOps PUBLIC JPEGLIB)
target_link_libraries(imageOps PUBLIC TIFFLIB)
target_link_libraries(imageOps PUBLIC ZLIB)
