# File: CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(Q_SNASB_Flight_Software VERSION 4.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-O3 -Wall -Wextra -Wpedantic)

# Layer 1: Hard-Real-Time Quantum Sensing Firmware
add_executable(quantum_core_driver firmware/quantum_core_driver.cpp)

# Layer 2: 6-Bot Swarm WCI, GCC-AKF & CCSDS DTN Middleware Node
add_executable(swarm_wci_dtn_node middleware/swarm_wci_dtn_node.cpp)