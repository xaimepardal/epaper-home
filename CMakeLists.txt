# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper_mqtt)

idf_component_get_property(spi_lib cleishm__idfxx_spi COMPONENT_LIB)
target_compile_options(${spi_lib} PRIVATE -Wno-missing-field-initializers)
