cmake_minimum_required(VERSION 3.10)

project(osu-fightstick C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)

add_executable(osu-fightstick osu-fightstick.c)

target_compile_options(osu-fightstick PRIVATE
    -Wall -Wextra
    ${PIPEWIRE_CFLAGS_OTHER}
    ${LIBEVDEV_CFLAGS_OTHER}
)

target_include_directories(osu-fightstick PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBEVDEV_INCLUDE_DIRS}
)

target_link_directories(osu-fightstick PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
    ${LIBEVDEV_LIBRARY_DIRS}
)

target_link_libraries(osu-fightstick PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBEVDEV_LIBRARIES}
)

install(TARGETS osu-fightstick RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   osu-fightstick.service   DESTINATION ${CMAKE_INSTALL_DATADIR}/osu-fightstick)
install(FILES   click.wav               DESTINATION ${CMAKE_INSTALL_DATADIR}/osu-fightstick)
