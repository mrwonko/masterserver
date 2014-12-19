#pragma once

#include <functional>

/**
 * Defer execution of a function until end of scope.
 **/
class Defer
{
public:
	template< class T >
	Defer( T&& function )
		: m_deferred( std::forward< T >( function ) ) 
	{
	}
	~Defer()
	{
		if( m_deferred )
		{
			m_deferred();
		}
	}
private:
	std::function< void() > m_deferred;
};
