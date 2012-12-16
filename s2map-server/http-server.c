/*
  A trivial static http webserver using Libevent's evhttp.

  This is not the best code in the world, and it does some fairly stupid stuff
  that you would never want to do in a production webserver. Caveat hackor!

 */
#include <boost/scoped_ptr.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sstream>

#include <sys/types.h>
#include <sys/stat.h>

#include "s2cellid.h"
#include "s2cell.h"
#include "s2.h"
#include "s2polygon.h"
#include "s2polygonbuilder.h"
#include "s2latlng.h"
#include "s2regioncoverer.h"
#include "strings/strutil.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifndef S_ISDIR
#define S_ISDIR(x) (((x) & S_IFMT) == S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#ifdef _EVENT_HAVE_NETINET_IN_H
#include <netinet/in.h>
# ifdef _XOPEN_SOURCE_EXTENDED
#  include <arpa/inet.h>
# endif
#endif

/* Compatibility for possible missing IPv6 declarations */
#include "../util-internal.h"

#ifdef WIN32
#define stat _stat
#define fstat _fstat
#define open _open
#define close _close
#define O_RDONLY _O_RDONLY
#endif

char uri_root[512];

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while(std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}


std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    return split(s, delim, elems);
}

#include <errno.h>

void s2cellidToJson(S2CellId* s2cellid, std::ostringstream& stringStream, bool last) {
  S2Cell cell(*s2cellid);
  S2LatLng center(cell.id().ToPoint());

  stringStream << "{" << endl
    << "\"id\": \"" << Int64ToString(cell.id().id()) << "\","  << endl
    << "\"token\": \"" << cell.id().ToToken() << "\"," << endl
    << "\"pos\":" << cell.id().pos() << ","  << endl
    << "\"face\":" << cell.id().face() << ","  << endl
    << "\"level\":" << cell.id().level() << ","  << endl
      << "\"ll\": { " << endl
        << "\"lat\":" << center.lat().degrees() << ","  << endl
        << "\"lng\":" << center.lng().degrees() << "" << endl
     << "}," << endl
     << "\"shape\": [ " << endl;

   for (int i = 0; i < 4; i++) {
     S2LatLng vertex(cell.GetVertex(i));
     stringStream << "{ " << endl
        << "\"lat\":" << vertex.lat().degrees() << ","  << endl
        << "\"lng\":" << vertex.lng().degrees() << "" << endl
        << "}" << endl;
      if (i != 3) {
        stringStream << ",";
      }
    }

   stringStream
     << "]" << endl
    << "}";

    if (!last) {
      stringStream << ",";
    }
    
    stringStream << endl;
}


// TODO
// get it outtputting json for s2info
// make sure everything is in a scoped_ptr
// s2 covering output

static char *s2CellIdsToJson(char* callback, std::vector<S2CellId> ids)  {
  std::ostringstream stringStream;
  if (callback) {
    stringStream << callback << "(";
  }

  stringStream << "[";

  for (int i = 0; i < ids.size(); i++) {
    s2cellidToJson(&ids[i], stringStream, i == ids.size() - 1);
  }
    
  stringStream << "]";

  if (callback) {
    stringStream << ")";
  }
  
  return strdup(stringStream.str().c_str());
}

static void
s2cover_request_cb(struct evhttp_request *req, void *arg)
{
  struct evkeyvalq  args;
	const char *uri = evhttp_request_get_uri(req);
  evhttp_parse_query(uri, &args);

  char* callback = (char *)evhttp_find_header(&args, "callback");

  char* points = (char *)evhttp_find_header(&args, "points");
  std::vector<S2CellId> cellids_vector;
  if (points != NULL) {
    printf(points);
    scoped_ptr<S2PolygonBuilder> builder(new S2PolygonBuilder(S2PolygonBuilderOptions::DIRECTED_XOR()));
    
    std::vector<std::string> points_vector = split(string(points), ',');
    std::vector<S2Point> s2points_vector;

    for (int i = 0; i < points_vector.size(); i += 2) {
      char *endptr;
      s2points_vector.push_back(S2LatLng::FromDegrees(
        strtod(points_vector[i].c_str(), &endptr),
        strtod(points_vector[i+1].c_str(), &endptr)
      ).ToPoint());
    }

    cout << s2points_vector.size() << endl;
    
    for (int i = 0; i < s2points_vector.size(); i++) {
      builder->AddEdge(
        s2points_vector[i],
        s2points_vector[(i + 1) % s2points_vector.size()]);
    }

    S2Polygon polygon;
    typedef vector<pair<S2Point, S2Point> > EdgeList;
    EdgeList edgeList;
    builder->AssemblePolygon(&polygon, &edgeList);

    S2RegionCoverer coverer;

    char* min_level = (char *)evhttp_find_header(&args, "min_level");
    if (min_level) {
      coverer.set_min_level(atoi(min_level));
    }

    char* max_level = (char *)evhttp_find_header(&args, "max_level");
    if (max_level) {
      coverer.set_max_level(atoi(max_level));
    }

    char* level_mod = (char *)evhttp_find_header(&args, "level_mod");
    if (level_mod) {
      coverer.set_level_mod(atoi(level_mod));
    }

    char* max_cells = (char *)evhttp_find_header(&args, "max_cells");
    if (max_cells) {
      coverer.set_max_cells(atoi(max_cells));
    }

    coverer.GetCovering(polygon, &cellids_vector); 
  }

  printf("\n");

	evhttp_add_header(evhttp_request_get_output_headers(req),
		    "Content-Type", "application/json");
	struct evbuffer *evb = NULL;
	evb = evbuffer_new();
  char* json = s2CellIdsToJson(callback, cellids_vector);
  evbuffer_add_printf(evb, "%s", json);
	evhttp_send_reply(req, 200, "OK", evb);
 
  free(json);

  if (evb)
    evbuffer_free(evb);
}


/* Callback used for the /dump URI, and for every non-GET request:
 * dumps all information to stdout and gives back a trivial 200 ok */
static void
s2info_request_cb(struct evhttp_request *req, void *arg)
{

  struct evkeyvalq    args;
	const char *uri = evhttp_request_get_uri(req);
  evhttp_parse_query(uri, &args);

  char* callback = (char *)evhttp_find_header(&args, "callback");

  char* ids = (char *)evhttp_find_header(&args, "id");
  std::vector<S2CellId> cellids_vector;
  if (ids != NULL) {
    printf("%s\n", ids);
    std::vector<std::string> ids_vector = split(string(ids), ',');
    for (int i = 0; i < ids_vector.size(); i++) {
      const char *str = ids_vector[i].c_str();
      errno = 0;    /* To distinguish success/failure after call */
      char *endptr;
      long long int id = strtoll(str, &endptr, 10);
      printf("endptr %d\n", strlen(endptr));
      printf("str %s\n", str);

      if (strlen(endptr) != 0) {
        printf("failed to parse as long long\n");
        cellids_vector.push_back(S2CellId(S2CellId::FromToken(str).id()));
      } else {
        printf("%lld\n", id);
        printf("id != 0 ? %d -- %s %d\n", (id != 0), str, strlen(str));
        cellids_vector.push_back(S2CellId(id));
      } 
    }
  }

	evhttp_add_header(evhttp_request_get_output_headers(req),
		    "Content-Type", "application/json");
	struct evbuffer *evb = NULL;
	evb = evbuffer_new();
  char* json = s2CellIdsToJson(callback, cellids_vector);
  evbuffer_add_printf(evb, "%s", json);
	evhttp_send_reply(req, 200, "OK", evb);
 
  free(json);

  if (evb)
    evbuffer_free(evb);
}

int
main(int argc, char **argv)
{
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *handle;

	unsigned short port = atoi(argv[1]);
#ifdef WIN32
	WSADATA WSAData;
	WSAStartup(0x101, &WSAData);
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return (1);
#endif
	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		return 1;
	}

	/* Create a new evhttp object to handle requests. */
	http = evhttp_new(base);
	if (!http) {
		fprintf(stderr, "couldn't create evhttp. Exiting.\n");
		return 1;
	}

	evhttp_set_cb(http, "/s2cover", s2cover_request_cb, NULL);
	evhttp_set_cb(http, "/s2info", s2info_request_cb, NULL);

	/* Now we tell the evhttp what port to listen on */
	handle = evhttp_bind_socket_with_handle(http, "0.0.0.0", port);
	if (!handle) {
		fprintf(stderr, "couldn't bind to port %d. Exiting.\n",
		    (int)port);
		return 1;
	}

	{
		/* Extract and display the address we're listening on. */
		struct sockaddr_storage ss;
		evutil_socket_t fd;
		ev_socklen_t socklen = sizeof(ss);
		char addrbuf[128];
		void *inaddr;
		const char *addr;
		int got_port = -1;
		fd = evhttp_bound_socket_get_fd(handle);
		memset(&ss, 0, sizeof(ss));
		if (getsockname(fd, (struct sockaddr *)&ss, &socklen)) {
			perror("getsockname() failed");
			return 1;
		}
		if (ss.ss_family == AF_INET) {
			got_port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
			inaddr = &((struct sockaddr_in*)&ss)->sin_addr;
		} else if (ss.ss_family == AF_INET6) {
			got_port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
			inaddr = &((struct sockaddr_in6*)&ss)->sin6_addr;
		} else {
			fprintf(stderr, "Weird address family %d\n",
			    ss.ss_family);
			return 1;
		}
		addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf,
		    sizeof(addrbuf));
		if (addr) {
			printf("HI Listening on %s:%d\n", addr, got_port);
			evutil_snprintf(uri_root, sizeof(uri_root),
			    "http://%s:%d",addr,got_port);
		} else {
			fprintf(stderr, "evutil_inet_ntop failed\n");
			return 1;
		}
	}

	event_base_dispatch(base);

	return 0;
}