#pragma once

#include "consolelog.hpp"

#include <microhttpd.h>

#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <regex>
#include <thread>
#include <unordered_map>
#include <list>
#include <chrono>
#include <memory>

class MasterServer
{
private:
	enum Settings
	{
		SERVER_TIMEOUT = 60, ///< in seconds
		PRUNE_INTERVAL = 30 ///< in seconds
	};
	
public:
	MasterServer( const unsigned short port, std::shared_ptr< ConsoleLog > log );
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
		unsigned long version;
	};
	
	struct DaemonClosure
	{
		MasterServer* master;
		std::mutex mutex;
	};
	
	typedef std::chrono::steady_clock::time_point TimePoint;
	
	struct Server
	{
		const std::string json;
		TimePoint lastHeartbeat;
	};
	
	class Servers
	{
	public:
		Servers( const std::string& json );
		Servers( const Servers& ) = delete;
		Servers& operator=( const Servers& ) = delete;
		Servers( Servers&& ) = default;
		Servers& operator=( Servers&& ) = default;
		
		void post( const std::string& json );
		void prune( const TimePoint& cutoff, std::shared_ptr< ConsoleLog > log );
		const std::string& toJson() const { return m_json; }
		const bool empty() const { return m_orderedByLastHeartbeat.empty(); }
	private:
		void updateJson();
	private:
		std::string m_json;
		/// oldest heartbeat first
		std::list< Server > m_orderedByLastHeartbeat;
		std::unordered_map< std::string, std::list< Server >::iterator > m_byJson;
	};

private:
	// these need not be thread safe since they're only called in a thread-safe manner.
	void pruneServers();
	const std::string& getServers( const std::string& game, const unsigned long version ) const;
	const MasterServer::Response updateServer( const std::string& game, const unsigned long version, const std::string& body );

private:
	static int sendResponse( MHD_Connection *connection, const MasterServer::Response& response, std::shared_ptr< ConsoleLog > log );
	static int handleAccess( void *globalUserdata, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **requestUserdata );
	static void completeRequest( void *userdata, MHD_Connection *connection, void **requestUserdata, MHD_RequestTerminationCode reason );
	
	static void pruneThread( DaemonClosure* closure );

private:
	MHD_Daemon* m_daemon = nullptr;
	DaemonClosure* m_daemonClosure = nullptr;
	bool m_stopPruneThread = false;
	std::thread m_pruneThread;
	std::shared_ptr< ConsoleLog > m_log;
	
	/// game -> ( version -> servers )
	std::unordered_map< std::string, std::unordered_map< unsigned long, Servers > > m_servers;
};
