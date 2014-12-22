# Master Server

A game-agnostic REST-based master server

# Building

After cloning, remember to do `git submodule init` and `git submodule update` to fetch json11. Build with cmake. libmicrohttpd and pthreads are required and only linux builds have been tested.

# Using

To retrieve the game list, send an HTTP GET request for the url `/<game>/<version>/`, where `<game>` is the game name (`[-_. [:alnum:]]+`) and  `<version>` is the version (a number).

To add a server, send an HTTP POST request for the same url with the following JSON body:

    {
        "ip": "myserver.net",
        "port": 1234
	}

Where `ip` is an arbitrary string - what's actually supported depends on the game - and `port` is the port number. Both are mandatory.

Serversget pruned after 60 seconds, so send regular heartbeats, i.e. repeated POST requests.
