#include "master.hpp"
#include "defer.hpp"

#include <iostream>

#include <stdexcept>
#include <vector>
#include <utility>
#include <algorithm>
#include <set>
#include <chrono>

#define MAX_BODY_SIZE (1024*1024)

MasterServer::MasterServer( const unsigned short port )
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

int MasterServer::sendResponse( MHD_Connection *connection, const MasterServer::Response& response )
{
	MHD_Response* mhdResponse = MHD_create_response_from_buffer( response.body.length(), const_cast< char* >( response.body.data() ), MHD_RESPMEM_MUST_COPY );
	if( !mhdResponse )
	{
		std::cerr << "Failed to create response" << std::endl;
		return MHD_NO;
	}
	Defer destroyResponse( [ mhdResponse ](){ MHD_destroy_response( mhdResponse ); } );
	for( const auto& entry : response.header )
	{
		if( MHD_add_response_header( mhdResponse, entry.first.c_str(), entry.second.c_str() ) != MHD_YES )
		{
			std::cerr << "Failed to add response header" << std::endl;
			return MHD_NO;
		}
	}
	int ret = MHD_queue_response( connection, response.code, mhdResponse );
	if( ret != MHD_YES )
	{
		std::cerr << "MHD_queue_response failed!" << std::endl;
	}
	return ret;
}

int MasterServer::handleAccess( void *globalUserdata, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **requestUserdata )
{
	DaemonClosure& closure = *static_cast< DaemonClosure* >( globalUserdata );
	
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
			return sendResponse( connection, s_methodNotAllowed );
		}
		
		// it's POST or GET, check URL
		static const std::regex s_regex( "/([-_. [:alnum:]]+)/([[:digit:]]+)/?" );
		std::smatch match;
		if( !std::regex_match( std::string( url ), match, s_regex ) )
		{
			static const Response s_fileNotFound = { MHD_HTTP_NOT_FOUND, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"File not Found!\" }" };
			return sendResponse( connection, s_fileNotFound );
		}
		const std::string game = match[1].str();
		const std::string version = match[2].str();
		
		// GET request
		if( it == s_get )
		{
			std::lock_guard< std::mutex > lock( closure.mutex );
			MasterServer& self = *closure.master;
			return sendResponse( connection, { MHD_HTTP_OK, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, self.getServers( game, version ) } );
		}
		
		// POST request
		
		// read content-length string
		const char* sSize = MHD_lookup_connection_value( connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_LENGTH );
		if( sSize == nullptr )
		{
			static const Response s_lengthRequired = { MHD_HTTP_LENGTH_REQUIRED, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Length Required\" }" };
			return sendResponse( connection, s_lengthRequired );
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
			return sendResponse( connection, s_badHeader );
		}
		// abort if too large
		if( size > MAX_BODY_SIZE )
		{
			return sendResponse( connection, s_tooLarge );
		}
		
		// create request, store as userdata
		PostRequest* pRequest = new PostRequest;
		pRequest->body.reserve( size );
		pRequest->bodySize = size;
		*requestUserdata = pRequest;
	}
	// is there more data than the header claimed?
	PostRequest& request = *static_cast< PostRequest* >( *requestUserdata );
	if( request.body.size() + *upload_data_size > request.bodySize )
	{
		return sendResponse( connection, s_tooLarge );
	}
	// are we done? (must wait for additional call with size 0)
	if( *upload_data_size == 0 )
	{
		if( request.body.size() < request.bodySize )
		{
			static const Response s_tooSmall { MHD_HTTP_BAD_REQUEST, { { MHD_HTTP_HEADER_CONTENT_TYPE, "application/json" } }, "{ \"error\": \"Request Body shorter than content-length claimed!\" }" };
			return sendResponse( connection, s_tooSmall );
		}
		const std::string body( request.body.data(), request.body.size() );
		std::lock_guard< std::mutex > lock( closure.mutex );
		MasterServer& self = *closure.master;
		return sendResponse( connection, self.updateServer( request.game, request.version, body ) );
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
		std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
		std::lock_guard< std::mutex > lock( closure->mutex );
		MasterServer& self = *closure->master;
		if( self.m_stopPruneThread ) break;
		self.pruneServers();
	}
}

void MasterServer::pruneServers()
{
}

const std::string MasterServer::getServers( const std::string& game, const std::string& version ) const
{
	return "{}";
}

const MasterServer::Response MasterServer::updateServer( const std::string& game, const std::string& version, const std::string& body )
{
	return { MHD_HTTP_NOT_IMPLEMENTED, {}, "not implemented" };
}
