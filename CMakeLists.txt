cmake_minimum_required(VERSION 3.10)
project(FTPServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add the executable
add_executable(ftpserver
    main.cpp
    server.cpp
)

# Find required packages
find_package(Threads REQUIRED)

# Link against required libraries
target_link_libraries(ftpserver PRIVATE
    Threads::Threads
    stdc++fs
)
