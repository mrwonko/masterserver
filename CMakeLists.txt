cmake_minimum_required(VERSION 3.0)

project(masterserver)

list( APPEND CMAKE_CXX_FLAGS "-std=c++11" )

add_executable( masterserver
	main.cpp
	signal.hpp signal.cpp
	master.hpp master.cpp
	defer.hpp
	)

target_link_libraries( masterserver microhttpd pthread )
