cmake_minimum_required (VERSION 3.5)

project(auto-apps)

include(GNUInstallDirs)

find_package (vsomeip3 2.6.0 REQUIRED)
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
find_package( Boost 1.55 COMPONENTS system thread log REQUIRED )

# libcaca decodes native multi-frame canvases and ASCII/ANSI artwork.  Keep
# ncursesw as the only terminal renderer (never invoke libcaca's display API).
find_path(LIBCACA_INCLUDE_DIR NAMES caca.h)
find_library(LIBCACA_LIBRARY NAMES caca)
if(NOT LIBCACA_INCLUDE_DIR OR NOT LIBCACA_LIBRARY)
    message(FATAL_ERROR "libcaca development headers/library not found (install libcaca-devel)")
endif()

set (INSTALL_LIB_DIR ${CMAKE_INSTALL_LIBDIR} CACHE PATH "Installation directory for libraries")
set (INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR} CACHE PATH "Installation directory for executables")
set (INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR} CACHE PATH "Installation directory for header files")

include_directories (
    ${Boost_INCLUDE_DIR}
    ${VSOMEIP_INCLUDE_DIRS}
)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(multimedia-service mm-service.cpp vsomeip-identity.hpp services.hpp demo-health.hpp mm-demo-health.hpp demo-modes.hpp radio-stations.cpp)
target_link_libraries(multimedia-service  vsomeip3 ${Boost_LIBRARIES})

add_executable(multimedia-client mm-client.cpp mm-video.cpp mm-video.hpp vsomeip-identity.hpp
               mm-client-state.hpp demo-health.hpp demo-modes.hpp services.hpp
               sensors-protocol.hpp sensors-client-state.hpp)
target_include_directories(multimedia-client PRIVATE ${CURSES_INCLUDE_DIRS} ${LIBCACA_INCLUDE_DIR})
target_compile_definitions(multimedia-client PRIVATE
    MM_DEFAULT_VIDEO_DIR=\"${CMAKE_INSTALL_FULL_DATADIR}/auto-apps/animations\")
target_link_libraries(multimedia-client  vsomeip3 ${Boost_LIBRARIES} ${CURSES_LIBRARIES} ${LIBCACA_LIBRARY})

add_executable(engine-service engine-service.cpp vsomeip-identity.hpp services.hpp demo-health.hpp engine-memory.hpp demo-modes.hpp)
target_link_libraries(engine-service  vsomeip3 ${Boost_LIBRARIES})

add_executable(controller-service controller-service.cpp controller-auto-policy.hpp vsomeip-identity.hpp controller-systemd.hpp
               controller-protocol.hpp demo-tuning.hpp services.hpp demo-health.hpp)
target_link_libraries(controller-service vsomeip3 ${Boost_LIBRARIES})
add_executable(controller-host-agent controller-host-agent.cpp controller-agent-scope.hpp controller-host-ipc.hpp
               controller-systemd.hpp controller-systemd-notify.hpp controller-protocol.hpp)
target_link_libraries(controller-host-agent pthread)

add_executable(controller-qm-agent controller-qm-agent.cpp vsomeip-identity.hpp controller-host-ipc.hpp
               controller-protocol.hpp services.hpp)
target_link_libraries(controller-qm-agent vsomeip3 ${Boost_LIBRARIES})

add_executable(sensors-service sensors-service.cpp vsomeip-identity.hpp sensors-simulation.cpp
               sensors-activation.hpp sensors-simulation.hpp sensors-protocol.hpp
               sensors-health.hpp demo-health.hpp services.hpp)
target_link_libraries(sensors-service vsomeip3 ${Boost_LIBRARIES})

install (
    TARGETS multimedia-service multimedia-client engine-service sensors-service
            controller-service controller-qm-agent controller-host-agent
    RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin
)

# Every application gets its stable SOME/IP client ID while retaining the
# shared Unix-domain routingmanagerd IPC path.  The host/partition global
# /etc/vsomeip.json is deliberately not overwritten.
install(FILES config/vsomeip-ipc.json
        DESTINATION "${CMAKE_INSTALL_DATADIR}/auto-apps")

# The native QM agent must not inherit the TCP-routing configuration from QM
# /etc/vsomeip.json.  Generate an agent-only UDS config at each start.
install(PROGRAMS tools/configure-qm-agent-vsomeip.py
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/auto-apps")

# Pack an immediately playable 100x30/10fps sample into RPM and container.
install(DIRECTORY animations/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/auto-apps/animations"
        FILES_MATCHING PATTERN "*.caca" PATTERN "*.ansi" PATTERN "*.ans"
        PATTERN "*.txt" PATTERN "*.asc" PATTERN "*.fps")

include(CTest)
if(BUILD_TESTING)
    add_executable(controller-auto-policy-test tests/controller-auto-policy-test.cpp
                   controller-auto-policy.hpp demo-health.hpp)
    target_include_directories(controller-auto-policy-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME controller-auto-policy-test COMMAND controller-auto-policy-test)
    add_executable(controller-host-ipc-test tests/controller-host-ipc-test.cpp
                   controller-host-ipc.hpp controller-protocol.hpp)
    target_include_directories(controller-host-ipc-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(controller-host-ipc-test pthread)
    add_test(NAME controller-host-ipc-test COMMAND controller-host-ipc-test)
    add_executable(controller-systemd-notify-test tests/controller-systemd-notify-test.cpp
                   controller-systemd-notify.hpp)
    target_include_directories(controller-systemd-notify-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME controller-systemd-notify-test COMMAND controller-systemd-notify-test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME qm-agent-config-test COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-qm-agent-config.py)
        add_test(NAME vsomeip-ipc-deployment-test COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-vsomeip-ipc-deployment.py)
    endif()
    add_executable(controller-test tests/controller-test.cpp controller-agent-scope.hpp controller-protocol.hpp
                   controller-systemd.hpp controller-ui-state.hpp demo-tuning.hpp
                   fast-warning.hpp)
    target_include_directories(controller-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(controller-test pthread)
    add_test(NAME controller-test COMMAND controller-test)
    add_executable(demo-modes-test tests/demo-modes-test.cpp demo-modes.hpp
                   mm-client-state.hpp mm-demo-health.hpp)
    target_include_directories(demo-modes-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME demo-modes-test COMMAND demo-modes-test)
    add_executable(mm-video-test tests/mm-video-test.cpp mm-video.cpp mm-video.hpp)
    target_include_directories(mm-video-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${LIBCACA_INCLUDE_DIR})
    target_link_libraries(mm-video-test ${LIBCACA_LIBRARY})
    target_compile_definitions(mm-video-test PRIVATE
        VIDEO_DEMO_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/animations/city-traffic.ansi\")
    add_test(NAME mm-video-test COMMAND mm-video-test)
    add_executable(sensors-health-test tests/sensors-health-test.cpp sensors-health.hpp demo-health.hpp)
    target_include_directories(sensors-health-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME sensors-health-test COMMAND sensors-health-test)
    add_executable(sensors-test tests/sensors-test.cpp sensors-simulation.cpp
                   sensors-protocol.hpp sensors-client-state.hpp sensors-activation.hpp)
    target_include_directories(sensors-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME sensors-test COMMAND sensors-test)
    add_executable(demo-health-test tests/demo-health-test.cpp
                   demo-health.hpp engine-memory.hpp mm-client-state.hpp)
    target_include_directories(demo-health-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME demo-health-test COMMAND demo-health-test)
    add_executable(mm-demo-health-test tests/mm-demo-health-test.cpp
                   demo-health.hpp mm-demo-health.hpp mm-client-state.hpp)
    target_include_directories(mm-demo-health-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME mm-demo-health-test COMMAND mm-demo-health-test)
endif()
