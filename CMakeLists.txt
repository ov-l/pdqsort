cmake_minimum_required(VERSION 3.14)

project(pdqsort VERSION 1.0.0 LANGUAGES CXX)

# Create the interface library
add_library(pdqsort INTERFACE)
add_library(pdqsort::pdqsort ALIAS pdqsort)

# https://cmake.org/cmake/help/latest/variable/PROJECT_IS_TOP_LEVEL.html
string(COMPARE EQUAL ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} PROJECT_IS_TOP_LEVEL)

option(PDQSORT_INSTALL "Generate and install pdqsort target" ${PROJECT_IS_TOP_LEVEL})

# pdqsort requires C++20 or later
target_compile_features(pdqsort INTERFACE "cxx_std_20")

# Set the include directory
target_include_directories(pdqsort INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if (PDQSORT_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # Install the header file
    install(FILES 
    "${CMAKE_CURRENT_SOURCE_DIR}/pdqsort.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/pdqsort_int.h"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Install the license file
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/license.txt"
            DESTINATION ${CMAKE_INSTALL_DOCDIR})

    set(export_name "pdqsortTargets")
    set(namespace "pdqsort::")
    set(cmake_files_install_dir ${CMAKE_INSTALL_DATADIR}/cmake/pdqsort)

    # Install the target and export
    install(TARGETS pdqsort 
            EXPORT ${export_name}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    
    install(EXPORT ${export_name} 
            NAMESPACE ${namespace} 
            DESTINATION ${cmake_files_install_dir})
    
    export(TARGETS pdqsort 
           NAMESPACE ${namespace} 
           FILE ${export_name}.cmake)

    # Create and install package config files
    set(config_version_file "${CMAKE_CURRENT_BINARY_DIR}/pdqsortConfigVersion.cmake")
    
    write_basic_package_version_file(
        ${config_version_file} 
        COMPATIBILITY SameMajorVersion 
        ARCH_INDEPENDENT
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pdqsortConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/pdqsortConfig.cmake"
        INSTALL_DESTINATION ${cmake_files_install_dir}
    )

    install(FILES 
            "${CMAKE_CURRENT_BINARY_DIR}/pdqsortConfig.cmake"
            ${config_version_file} 
            DESTINATION ${cmake_files_install_dir})
endif()