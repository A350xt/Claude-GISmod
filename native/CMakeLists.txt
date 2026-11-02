cmake_minimum_required(VERSION 3.21)
project(claude_gis VERSION 0.4.0 LANGUAGES CXX)

# 依赖由 vcpkg 提供（vcpkg.json）：
#   cmake -B build -S native -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
#         -DVCPKG_TARGET_TRIPLET=x64-windows-static
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(GDAL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(claude-gis ${SOURCES})
target_include_directories(claude-gis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(claude-gis PRIVATE GDAL::GDAL nlohmann_json::nlohmann_json)
target_compile_definitions(claude-gis PRIVATE CLAUDE_GIS_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(claude-gis PRIVATE /utf-8 /W4 /permissive- /EHsc /bigobj)
  # 静态 triplet（x64-windows-static）用静态 CRT，产物是单个 exe
  if(VCPKG_TARGET_TRIPLET MATCHES "static")
    set_property(TARGET claude-gis PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
else()
  target_compile_options(claude-gis PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 发布目录：exe + share/proj/proj.db（PROJ 必需的坐标系数据库）
set(PROJ_DB "")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  set(PROJ_DB "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/proj/proj.db")
endif()
install(TARGETS claude-gis RUNTIME DESTINATION .)
if(PROJ_DB AND EXISTS "${PROJ_DB}")
  install(FILES "${PROJ_DB}" DESTINATION share/proj)
  # 构建目录里也放一份，方便直接从 build/ 运行测试
  add_custom_command(TARGET claude-gis POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:claude-gis>/share/proj"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DB}" "$<TARGET_FILE_DIR:claude-gis>/share/proj/proj.db")
endif()
