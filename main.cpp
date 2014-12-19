#include <iostream>
#include <chrono>

#include "signal.hpp"
#include "master.hpp"

#define PORT 1234

int main( int argc, char** argv )
{
	MasterServer server( PORT );
	std::cout << "Send a SIGINT (Ctrl+C) to exit." << std::endl;
	awaitSigint();
	std::cout << "Shutting down." << std::endl;
	return 0;
}
