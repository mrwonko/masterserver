#pragma once

#include <iostream>
#include <mutex>

class ConsoleLog
{
	using Lock = std::lock_guard< std::mutex >;
	
public:
	enum class LogLevel
	{
		None,
		Error,
		Warning,
		Log,
		Info,
		Debug
	};
	
	ConsoleLog( LogLevel level );
	
	template< typename... Args >
	void log( LogLevel level, Args... args )
	{
		if( static_cast< unsigned int >( level ) <= static_cast< unsigned int >( m_level ) )
		{
			Lock lock( m_mutex );
			write( level == LogLevel::Error ? std::cerr : std::cout, args... );
		}
	}
	
private:
	void write( std::ostream& stream )
	{
		stream << std::endl;
	}
	
	template< typename Head, typename... Tail >
	void write( std::ostream& stream, Head head, Tail... tail )
	{
		stream << head;
		write( stream, tail... );
	}
	
private:
	LogLevel m_level;
	std::mutex m_mutex;
};
