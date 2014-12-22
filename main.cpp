#include <iostream>
#include <chrono>
#include <memory>

#include "signal.hpp"
#include "master.hpp"
#include "consolelog.hpp"

#define PORT 1234

int main( int argc, char** argv )
{
	std::shared_ptr< ConsoleLog > log = std::make_shared< ConsoleLog >( ConsoleLog::LogLevel::Info );
	MasterServer server( PORT, log );
	std::cout << "Send a SIGINT (Ctrl+C) to exit." << std::endl;
	awaitSigint();
	std::cout << "Shutting down." << std::endl;
	return 0;
}
