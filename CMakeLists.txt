cmake_minimum_required(VERSION 3.0)

project(masterserver)

list( APPEND CMAKE_CXX_FLAGS "-std=c++11" )

add_executable( masterserver
	main.cpp
	signal.hpp signal.cpp
	master.hpp master.cpp
	defer.hpp
	json11/json11.hpp json11/json11.cpp
	consolelog.hpp consolelog.cpp
	)

target_link_libraries( masterserver microhttpd pthread )
