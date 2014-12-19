#pragma once

#include <microhttpd.h>

#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <regex>
#include <thread>

class MasterServer
{
public:
	MasterServer( const unsigned short port );
	~MasterServer();
	// non-copyable
	MasterServer( const MasterServer& ) = delete;
	MasterServer& operator=( const MasterServer& ) = delete;
	// Movable
	MasterServer( MasterServer&& rhs );
	MasterServer& operator=( MasterServer&& rhs );

private:
	struct Response
	{
		Response() = default;
		Response( Response&& rhs ) = default;
		Response( const Response& rhs ) = default;
		Response& operator=( Response&& rhs ) = default;
		Response& operator=( const Response& rhs ) = default;
		
		unsigned short code;
		std::map< std::string, std::string > header;
		std::string body;
	};
	
	struct PostRequest
	{
		PostRequest() = default;
		PostRequest( const PostRequest& rhs ) = default;
		PostRequest( PostRequest&& rhs ) = default;
		PostRequest& operator=( const PostRequest& rhs ) = default;
		PostRequest& operator=( PostRequest&& rhs ) = default;
		size_t bodySize = 0;
		std::vector< char > body;
		std::string game;
		std::string version;
	};
	
	struct DaemonClosure
	{
		MasterServer* master;
		std::mutex mutex;
	};

private:
	void pruneServers();
	const std::string getServers( const std::string& game, const std::string& version ) const;
	const MasterServer::Response updateServer( const std::string& game, const std::string& version, const std::string& body );

private:
	static int sendResponse( MHD_Connection *connection, const MasterServer::Response& response );
	static int handleAccess( void *globalUserdata, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **requestUserdata );
	static void completeRequest( void *userdata, MHD_Connection *connection, void **requestUserdata, MHD_RequestTerminationCode reason );
	
	static void pruneThread( DaemonClosure* closure );

private:
	MHD_Daemon* m_daemon = nullptr;
	DaemonClosure* m_daemonClosure = nullptr;
	bool m_stopPruneThread = false;
	std::thread m_pruneThread;
};
