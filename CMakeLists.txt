cmake_minimum_required(VERSION 3.24)
project(SpaceTrains VERSION 0.1.0 LANGUAGES CXX)

# ./run configures a fresh build without arguments: default to an optimised one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(spacetrains_core
    src/data_loader/DataLoader.cpp
    src/domain/ShipDesign.cpp
    src/celestial/CelestialMechanics.cpp
    src/trajectory/ChaseSolver.cpp
    src/trajectory/Lambert.cpp
    src/trajectory/PathSampling.cpp
    src/trajectory/TrajectoryAudit.cpp
    src/trajectory/TrajectoryPlanner.cpp
    src/trajectory/VariableIspTrajectoryPlanner.cpp
    src/variable_isp/VariableIsp.cpp
    src/variable_isp/GeneralTransfer.cpp
    src/economy/EconomySystem.cpp
    src/simulation/Simulation.cpp
    src/simulation/CrewEnvironment.cpp
    src/simulation/Passengers.cpp
    src/simulation/Households.cpp
    src/simulation/StationSystems.cpp
    src/simulation/MoneyJournal.cpp
    src/simulation/Statistics.cpp
    src/util/Profiling.cpp
    src/util/ThreadPool.cpp
    src/persistence/Json.cpp
    src/persistence/SimulationPersistence.cpp
)

target_include_directories(spacetrains_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(spacetrains_core PUBLIC Threads::Threads)
target_compile_options(spacetrains_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(spacetrains_headless src/app/main.cpp)
target_link_libraries(spacetrains_headless PRIVATE spacetrains_core Threads::Threads)

add_executable(spacetrains_bridge src/bridge/main.cpp)
target_link_libraries(spacetrains_bridge PRIVATE spacetrains_core)

add_executable(spacetrains_visp_bench tools/visp_bench/main.cpp)
target_link_libraries(spacetrains_visp_bench PRIVATE spacetrains_core)
add_executable(spacetrains_atlas_fill tools/atlas_fill/main.cpp)
target_link_libraries(spacetrains_atlas_fill PRIVATE spacetrains_core)

enable_testing()
add_executable(spacetrains_tests tests/test_main.cpp)
target_link_libraries(spacetrains_tests PRIVATE spacetrains_core)
add_test(NAME spacetrains_tests COMMAND spacetrains_tests)
set_tests_properties(spacetrains_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(spacetrains_variable_isp_tests tests/variable_isp_test.cpp)
target_link_libraries(spacetrains_variable_isp_tests PRIVATE spacetrains_core)
add_test(NAME spacetrains_variable_isp_tests COMMAND spacetrains_variable_isp_tests)
set_tests_properties(spacetrains_variable_isp_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(spacetrains_economy_tests tests/economy_test.cpp)
target_link_libraries(spacetrains_economy_tests PRIVATE spacetrains_core)
add_test(NAME spacetrains_economy_tests COMMAND spacetrains_economy_tests)
set_tests_properties(spacetrains_economy_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME ship_class_builder COMMAND ${Python3_EXECUTABLE} tests/test_build_ship_classes.py)
    set_tests_properties(ship_class_builder PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME key_bindings COMMAND ${Python3_EXECUTABLE} tests/test_key_bindings.py)
    set_tests_properties(key_bindings PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
