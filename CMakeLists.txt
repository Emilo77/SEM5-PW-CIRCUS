cmake_minimum_required(VERSION 3.16)
project(PW-CIRCUS)

set(CMAKE_CXX_STANDARD "20")

set(CMAKE_CXX_FLAGS "-std=c++20 -Wall -Wextra -Werror -pedantic -fsanitize=undefined,thread")
#set(CMAKE_CXX_FLAGS "-std=c++20 -Wall -Wextra -fsanitize=undefined,thread")
#set(CMAKE_CXX_FLAGS "-std=c++20 -Wall -Wextra")

add_executable(demo
        demo.cpp
        machine.hpp
        system.cpp)

add_executable(demo2
        demo2.cpp
        machine.hpp
        system.cpp)

add_executable(test test.cpp)


