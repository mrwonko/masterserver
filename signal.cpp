#include "signal.hpp"

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>

// ""s suffix is available in C++14, but gcc 4.8 doesn't support it yet
static constexpr std::chrono::seconds operator ""_s(unsigned long long s)
{
    return std::chrono::seconds(s);
}

static std::atomic_bool g_exitRequested;

static void sigintHandler( int signal )
{
	// changing an atomic value is pretty much the only well-defined operation in a signal handler...
	// can't use a condition variable here
	g_exitRequested = true;
}

void awaitSigint()
{
	g_exitRequested = false;
	auto previousHandler = std::signal( SIGINT, sigintHandler );
	if( previousHandler == SIG_ERR )
	{
		std::cerr << "Failed to install SIGINT Handler!" << std::endl;
		return;
	}
	{
		while( !g_exitRequested )
		{
			std::this_thread::sleep_for( 1_s );
		}
	}
	
	if( std::signal( SIGINT, previousHandler ) == SIG_ERR )
	{
		std::cerr << "Failed to reset to previous SIGINT Handler!" << std::endl;
	}
	return;
}
