cmake_minimum_required(VERSION 3.16)
project(motioneye-nvr VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia MultimediaWidgets)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)

add_executable(motioneye-nvr
  src/main.cpp
  src/Config.cpp
  src/MotionEyeClient.cpp
  src/CameraTile.cpp
  src/MosaicWidget.cpp
  src/MainWindow.cpp
  src/RecordingsDialog.cpp
  src/SettingsDialog.cpp
)
target_compile_definitions(motioneye-nvr PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(motioneye-nvr PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets PkgConfig::TURBOJPEG)

install(TARGETS motioneye-nvr RUNTIME DESTINATION bin)
