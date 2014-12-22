#include "master.hpp"
#include "defer.hpp"
#include "json11/json11.hpp"

#include <stdexcept>
#include <vector>
#include <utility>
#include <algorithm>
#include <set>

#define MAX_BODY_SIZE (1024*1024)

using Lock = std::lock_guard< std::mutex >;

MasterServer::MasterServer( const unsigned short port, std::shared_ptr< ConsoleLog > log )
	:  m_log( std::move( log ) )
{
	m_daemonClosure = new DaemonClosure { this };
	m_daemon = MHD_start_daemon(
		// single worker thread
		MHD_USE_SELECT_INTERNALLY,
		port,
		// no auth
		NULL, NULL,
		// access handler & its userdata
		&handleAccess, m_daemonClosure,
		// (vararg options start here)
		// request completion handler
		MHD_OPTION_NOTIFY_COMPLETED, &completeRequest, m_daemonClosure,
		// no more vararg options
		MHD_OPTION_END
		);
	m_pruneThread = std::thread( pruneThread, m_daemonClosure );
}

MasterServer::~MasterServer()
{
	if( m_daemon )
	{
		m_stopPruneThread = true;
		MHD_stop_daemon( m_daemon );
		m_pruneThread.join();
		delete m_daemonClosure;
	}
}

MasterServer::MasterServer( MasterServer&& rhs )
	: m_daemon( rhs.m_daemon )
	, m_daemonClosure( rhs.m_daemonClosure )
	, m_pruneThread( std::move( rhs.m_pruneThread ) )
{
	{
		std::lock_guard< std::mutex > lock( m_daemonClosure->mutex );
		m_daemonClosure->master = this;
	}
	rhs.m_daemon = nullptr;
	rhs.m_daemonClosure = nullptr;
}

int MasterServer::sendResponse( MHD_Connection *connection, const MasterServer::Response& response, std::shared_ptr< ConsoleLog > log )
{
	MHD_Response* mhdResponse = MHD_create_response_from_buffer( response.body.length(), const_cast< char* >( response.body.data() ), MHD_RESPMEM_MUST_COPY );
	if( !mhdResponse )
	{
		log->log( ConsoleLog::LogLevel::Error, "Failed to create response" );
		return MHD_NO;
	}
	Defer destroyResponse( [ mhdResponse ](){ MHD_destroy_response( mhdResponse ); } );
	for( const auto& entry : response.header )
	{
		if( MHD_add_response_header( mhdResponse, entry.first.c_str(), entry.second.c_str() ) != MHD_YES )
		{
			log->log( ConsoleLog::LogLevel::Error, "Failed to add response header" );
			return MHD_NO;
		}
	}
	int ret = MHD_queue_response( connection, response.code, mhdResponse );
	if( ret != MHD_YES )
	{
		log->log( ConsoleLog::LogLevel::Error, "MHD_queue_response failed!" );
	}
	return ret;
}

int MasterServer::handleAccess( void *globalUserdata, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **requestUserdata )
{
	DaemonClosure& closure = *static_cast< DaemonClosure* >( globalUserdata );
	auto getLog = [ &closure ]()
	{
		Lock( closure.mutex );
		return closure.master->m_log;
	};
	
	static const Response s_tooLarge = { MHD_HTTP_REQUEST_ENTITY_TOO_LARGE, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Request Entity Too Large\" }" };
	
	// in case of a new request, read content-length and create a request object in closure
	if( *requestUserdata == nullptr )
	{
		// Check method type
		static const std::set< std::string > s_allowedMethods{ "GET", "POST" };
		static const auto s_get = s_allowedMethods.find( "GET" );
		static const auto s_post = s_allowedMethods.find( "POST" );
		static const auto s_invalid = s_allowedMethods.end();
		const auto it = s_allowedMethods.find( method );
		if( it == s_invalid )
		{
			static const Response s_methodNotAllowed = { MHD_HTTP_METHOD_NOT_ALLOWED, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Method not allowed!\" }" };
			return sendResponse( connection, s_methodNotAllowed, getLog() );
		}
		
		// it's POST or GET, check URL
		static const std::regex s_regex( "/([-_. [:alnum:]]+)/([[:digit:]]+)/?" );
		std::smatch match;
		if( !std::regex_match( std::string( url ), match, s_regex ) )
		{
			static const Response s_fileNotFound = { MHD_HTTP_NOT_FOUND, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"File not Found!\" }" };
			return sendResponse( connection, s_fileNotFound, getLog() );
		}
		const std::string game = match[1].str();
		unsigned long version = 0;
		try
		{
			version = std::stoul( match[2].str() );
		}
		catch( std::exception& e )
		{
			static const Response s_badHeader = { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Version too high\" }" };
			return sendResponse( connection, s_badHeader, getLog() );
		}
		
		// GET request
		if( it == s_get )
		{
			Lock lock( closure.mutex );
			MasterServer& self = *closure.master;
			return self.sendResponse( connection, { MHD_HTTP_OK, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, self.getServers( game, version ) }, self.m_log );
		}
		
		// POST request
		
		// read content-length string
		const char* sSize = MHD_lookup_connection_value( connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_LENGTH );
		if( sSize == nullptr )
		{
			static const Response s_lengthRequired = { MHD_HTTP_LENGTH_REQUIRED, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Length Required\" }" };
			return sendResponse( connection, s_lengthRequired, getLog() );
		}
		// parse content-length
		unsigned long size = 0;
		try
		{
			size = std::stoul( sSize );
		}
		catch( std::exception& e )
		{
			static const Response s_badHeader = { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Could not parse content-length\" }" };
			return sendResponse( connection, s_badHeader, getLog() );
		}
		// abort if too large
		if( size > MAX_BODY_SIZE )
		{
			return sendResponse( connection, s_tooLarge, getLog() );
		}
		
		// early bail if empty body
		if( size == 0 )
		{
			static const Response s_noBody = { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Must supply request body!\" }" };
			return sendResponse( connection, s_noBody, getLog() );
		}
		
		// create request, store as userdata
		PostRequest* pRequest = new PostRequest;
		pRequest->body.reserve( size );
		pRequest->bodySize = size;
		pRequest->game = game;
		pRequest->version = version;
		*requestUserdata = pRequest;
		
		if( *upload_data_size == 0 )
		{
			return MHD_YES;
		}
	}
	// is there more data than the header claimed?
	PostRequest& request = *static_cast< PostRequest* >( *requestUserdata );
	if( request.body.size() + *upload_data_size > request.bodySize )
	{
		return sendResponse( connection, s_tooLarge, getLog() );
	}
	// are we done? (must wait for additional call with size 0)
	if( *upload_data_size == 0 )
	{
		if( request.body.size() < request.bodySize )
		{
			static const Response s_tooSmall { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Request Body shorter than content-length claimed!\" }" };
			return sendResponse( connection, s_tooSmall, getLog() );
		}
		const std::string body( request.body.data(), request.body.size() );
		std::lock_guard< std::mutex > lock( closure.mutex );
		MasterServer& self = *closure.master;
		return self.sendResponse( connection, self.updateServer( request.game, request.version, body ), self.m_log );
	}
	else
	{
		// read supplied body
		std::copy( upload_data, upload_data + *upload_data_size, std::back_inserter( request.body ) );
		*upload_data_size = 0;
		// not quite done yet, keep connection open
		return MHD_YES;
	}
}

void MasterServer::completeRequest( void *globalUserdata, MHD_Connection *connection, void **requestUserdata, MHD_RequestTerminationCode reason )
{
	if( !*requestUserdata ) return;
	PostRequest* request = static_cast< PostRequest* >( *requestUserdata );
	
	delete request;
	*requestUserdata = nullptr;
}

void MasterServer::pruneThread( DaemonClosure* closure )
{
	while( true )
	{
		std::this_thread::sleep_for( std::chrono::seconds( PRUNE_INTERVAL ) );
		Lock lock( closure->mutex );
		MasterServer& self = *closure->master;
		if( self.m_stopPruneThread ) break;
		self.pruneServers();
	}
}

void MasterServer::pruneServers()
{
	m_log->log( ConsoleLog::LogLevel::Info, "Pruning..." );
	static std::chrono::steady_clock::duration timeout = std::chrono::seconds( SERVER_TIMEOUT );
	TimePoint cutoff = std::chrono::steady_clock::now() - timeout;
	for( auto gameIt = m_servers.begin(); gameIt != m_servers.end(); )
	{
		auto& versions = gameIt->second;
		
		for( auto versionIt = versions.begin(); versionIt != versions.end(); )
		{
			Servers& servers = versionIt->second;
			servers.prune( cutoff, m_log );
			if( servers.empty() )
			{
				versionIt = versions.erase( versionIt );
			}
			else
			{
				++versionIt;
			}
		}
		
		if( versions.empty() )
		{
			gameIt = m_servers.erase( gameIt );
		}
		else
		{
			++gameIt;
		}
	}
}

MasterServer::Servers::Servers( const std::string& json )
	: m_json( "[" + json + "]" )
	, m_orderedByLastHeartbeat { { json, std::chrono::steady_clock::now() } }
	, m_byJson { { json, m_orderedByLastHeartbeat.begin() } }
{
}

void MasterServer::Servers::updateJson()
{
	m_json = "[";
	bool first = true;
	for( const Server& server : m_orderedByLastHeartbeat )
	{
		if( !first )
		{
			m_json += ',';
		}
		first = false;
		m_json += server.json;
	}
	m_json += ']';
}

void MasterServer::Servers::prune( const MasterServer::TimePoint& cutoff, std::shared_ptr< ConsoleLog > log )
{
	auto it = m_orderedByLastHeartbeat.begin();
	bool changed = false;
	while( it != m_orderedByLastHeartbeat.end() && it->lastHeartbeat < cutoff )
	{
		if( m_byJson.erase( it->json ) == 0 )
		{
			log->log( ConsoleLog::LogLevel::Error, "Servers lost internal consistency! Discarding." );
			m_orderedByLastHeartbeat.clear();
			m_byJson.clear();
			return;
		}
		log->log( ConsoleLog::LogLevel::Log, '\t', it->json );
		it = m_orderedByLastHeartbeat.erase( it );
		changed = true;
	}
	if( changed )
	{
		updateJson();
	}
}

void MasterServer::Servers::post( const std::string& json )
{
	auto it = m_byJson.find( json );
	if( it == m_byJson.end() )
	{
		// new, create
		m_orderedByLastHeartbeat.push_back( { json, std::chrono::steady_clock::now() } );
		m_byJson[ json ] = --m_orderedByLastHeartbeat.end();
		updateJson();
	}
	else
	{
		// update last heartbeat time
		it->second->lastHeartbeat = std::chrono::steady_clock::now();
		// move to back
		if( it->second != --m_orderedByLastHeartbeat.end() )
		{
			auto spliceEnd = it->second;
			++spliceEnd;
			m_orderedByLastHeartbeat.splice( m_orderedByLastHeartbeat.end(), m_orderedByLastHeartbeat, it->second, spliceEnd );
		}
		// json stays the same, order notwithstanding
	}
}

const std::string& MasterServer::getServers( const std::string& game, const unsigned long version ) const
{
	static const std::string s_none = "[]";
	auto gameIt = m_servers.find( game );
	if( gameIt == m_servers.end() )
	{
		return s_none;
	}
	auto& versions = gameIt->second;
	auto versionIt = versions.find( version );
	if( versionIt == versions.end() )
	{
		return s_none;
	}
	const Servers& servers = versionIt->second;
	return servers.toJson();
}

const MasterServer::Response MasterServer::updateServer( const std::string& game, const unsigned long version, const std::string& body )
{
	std::string error;
	json11::Json jsonIn = json11::Json::parse( body, error );
	if( !error.empty() )
	{
		const json11::Json errorJson = json11::Json::object { { "error", "JSON Parse Error: " + error } };
		return { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, errorJson.dump() };
	}
	
	if( !jsonIn.has_shape( {
			{ "ip", json11::Json::STRING },
			{ "port", json11::Json::NUMBER }
		}, error ) )
	{
		const json11::Json errorJson = json11::Json::object { { "error", "Malformed JSON Object: " + error } };
		return { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, errorJson.dump() };
	}
	
	std::string ip = jsonIn[ "ip" ].string_value();
	int port = jsonIn[ "port" ].int_value();
	if( port < 1 || port >= (1 << 16 ) )
	{
		return { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Invalid port number!\" }" };;
	}
	json11::Json jsonOut( 
	{
		{ "ip", json11::Json( ip ) },
		{ "port", json11::Json( port ) }
	} );
	
	static const Response s_ok = { MHD_HTTP_OK, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"timeout\": " + std::to_string( SERVER_TIMEOUT ) + " }" };
	
	auto gameIt = m_servers.find( game );
	if( gameIt == m_servers.end() )
	{
		Servers servers( jsonOut.dump() );
		std::unordered_map< unsigned long, Servers > versions;
		versions.emplace( version, std::move( servers ) );
		m_servers.emplace( game, std::move( versions ) );
		return s_ok;
	}
	auto& versions = gameIt->second;
	auto versionIt = versions.find( version );
	if( versionIt == versions.end() )
	{
		versions.emplace( version, Servers( jsonOut.dump() ) );
		return s_ok;
	}
	Servers& servers = versionIt->second;
	servers.post( jsonOut.dump() );
	return s_ok;
}
