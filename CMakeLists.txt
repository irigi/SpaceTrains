cmake_minimum_required(VERSION 3.24)
project(SpaceTrains VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(spacetrains_core
    src/data_loader/DataLoader.cpp
    src/celestial/CelestialMechanics.cpp
    src/trajectory/TrajectoryPlanner.cpp
    src/trajectory/VariableIspTrajectoryPlanner.cpp
    src/variable_isp/VariableIsp.cpp
    src/economy/EconomySystem.cpp
    src/simulation/Simulation.cpp
)

target_include_directories(spacetrains_core PUBLIC src)
target_compile_options(spacetrains_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(spacetrains_headless src/app/main.cpp)
target_link_libraries(spacetrains_headless PRIVATE spacetrains_core)

add_executable(spacetrains_bridge src/bridge/main.cpp)
target_link_libraries(spacetrains_bridge PRIVATE spacetrains_core)

enable_testing()
add_executable(spacetrains_tests tests/test_main.cpp)
target_link_libraries(spacetrains_tests PRIVATE spacetrains_core)
add_test(NAME spacetrains_tests COMMAND spacetrains_tests)
set_tests_properties(spacetrains_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(spacetrains_variable_isp_tests tests/variable_isp_test.cpp)
target_link_libraries(spacetrains_variable_isp_tests PRIVATE spacetrains_core)
add_test(NAME spacetrains_variable_isp_tests COMMAND spacetrains_variable_isp_tests)
set_tests_properties(spacetrains_variable_isp_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
