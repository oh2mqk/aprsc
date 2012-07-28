/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 */

/*
 *	http.c: the HTTP server thread, serving status pages and taking position uploads
 */

#include <signal.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <event2/event.h>  
#include <event2/http.h>  
#include <event2/buffer.h>  

#include "http.h"
#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "worker.h"
#include "status.h"

int http_shutting_down;
int http_reconfiguring;

unsigned long http_requests = 0;

struct http_static_t {
	char	*name;
	char	*filename;
};

struct evhttp_bound_socket *http_status_handle = NULL;
struct evhttp_bound_socket *http_upload_handle = NULL;

/*
 *	This is a list of files that the http server agrees to serve.
 *	Due to security concerns the list is static.
 *	It's a lot of work to actually implement a full-blown secure web
 *	server, and that's not what we're trying to do here.
 */

static struct http_static_t http_static_files[] = {
	{ "/", "index.html" },
	{ "/favicon.ico", "favicon.ico" },
	{ "/aprsc.css", "aprsc.css" },
	{ "/aprsc.js", "aprsc.js" },
	{ "/aprsc-logo1.png", "aprsc-logo1.png" },
	{ "/aprsc-logo2.png", "aprsc-logo2.png" },
	{ NULL, NULL }
};

/*
 *	Content types for the required file extensions
 */

static struct http_static_t http_content_types[] = {
	{ ".html", "text/html; charset=UTF-8" },
	{ ".ico", "image/x-icon" },
	{ ".css", "text/css; charset=UTF-8" },
	{ ".js", "application/x-javascript; charset=UTF-8" },
	{ ".jpg", "image/jpeg" },
	{ ".jpeg", "image/jpeg" },
	{ ".png", "image/png" },
	{ ".gif", "image/gif" },
	{ NULL, NULL }
};

/*
 *	Find a content-type for a file name
 */

char *http_content_type(char *fn)
{
	struct http_static_t *cmdp;
	static char default_ctype[] = "text/html";
	char *s;
	
	s = strrchr(fn, '.');
	if (!s)
		return default_ctype;
		
	for (cmdp = http_content_types; cmdp->name != NULL; cmdp++)
		if (strcasecmp(cmdp->name, s) == 0)
			break;
	
	if (cmdp->name == NULL)
		return default_ctype;
	
	return cmdp->filename;
}

/*
 *	HTTP date formatting
 */

int http_date(char *buf, int len, time_t t)
{
	struct tm tb;
	char *wkday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", NULL };
	char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
			"Sep", "Oct", "Nov", "Dec", NULL };
	
	gmtime_r(&t, &tb);
	
	return snprintf(buf, len, "%s, %02d %s %04d %02d:%02d:%02d GMT",
		wkday[tb.tm_wday], tb.tm_mday, mon[tb.tm_mon], tb.tm_year + 1900,
		tb.tm_hour, tb.tm_min, tb.tm_sec);
}

/*
 *	Accept a POST containing a position
 */

#define MAX_HTTP_POST_DATA 2048

void http_upload_position(struct evhttp_request *r)
{
	struct evbuffer *bufin, *bufout;
	struct evkeyvalq *req_headers;
	const char *ctype, *clength;
	int clength_i;
	char post[MAX_HTTP_POST_DATA+1];
	ev_ssize_t l;
	char *cr, *lf, *end;
	char *packet;
	
	req_headers = evhttp_request_get_input_headers(r);
	ctype = evhttp_find_header(req_headers, "Content-Type");
	
	if (!ctype || strcasecmp(ctype, "application/octet-stream") != 0) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, wrong or missing content-type");
		return;
	}
	
	clength = evhttp_find_header(req_headers, "Content-Length");
	if (!clength) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, missing content-length");
		return;
	}
	
	clength_i = atoi(clength);
	if (clength_i > MAX_HTTP_POST_DATA) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, too large body");
		return;
	}
	
	/* get the HTTP POST body */
	bufin = evhttp_request_get_input_buffer(r);
	l = evbuffer_copyout(bufin, post, MAX_HTTP_POST_DATA);
	
	/* Just for convenience and safety, null-terminate. Easier to log. */
	post[MAX_HTTP_POST_DATA] = 0;
	if (l <= MAX_HTTP_POST_DATA)
		post[l] = 0;
	
	if (l != clength_i) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Body size does not match content-length");
		return;
	}
	
	hlog(LOG_DEBUG, "got post data: %s", post);
	
	// find line feed, terminate string
	end = post + l;
	lf = memchr(post, '\n', l);
	if (!lf) {
		evhttp_send_error(r, HTTP_BADREQUEST, "No newline (LF) found in data");
		return;
	}
	*lf = 0;
	
	// find optional carriage return, terminate string
	cr = memchr(post, '\r', lf-post);
	if (cr)
		*cr = 0;
	
	// now the first line contains a login string. Go for the packet body:
	packet = lf + 1;
	lf = memchr(packet, '\n', end-packet);
	if (lf) {
		*lf = 0;
		end = lf;
	}
	
	// find optional carriage return, terminate string
	cr = memchr(packet, '\r', end-packet);
	if (cr) {
		*cr = 0;
		end = cr;
	}
	
	hlog(LOG_DEBUG, "login string: %s", post);
	hlog(LOG_DEBUG, "packet: %s", packet);
	
	bufout = evbuffer_new();
	evbuffer_add(bufout, "ok\n", 3);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	evhttp_add_header(headers, "Content-Type", "text/plain; charset=UTF-8");
	
	evhttp_send_reply(r, HTTP_OK, "OK", bufout);
	evbuffer_free(bufout);
}

/*
 *	Generate a status JSON response
 */

void http_status(struct evhttp_request *r)
{
	char *json;
	struct evbuffer *buffer = evbuffer_new();
	
	json = status_json_string(0);
	evbuffer_add(buffer, json, strlen(json));
	free(json);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	evhttp_add_header(headers, "Content-Type", "application/json; charset=UTF-8");
	
	evhttp_send_reply(r, HTTP_OK, "OK", buffer);
	evbuffer_free(buffer);
}

/*
 *	HTTP static file server
 */

#define HTTP_FNAME_LEN 1024

static void http_route_static(struct evhttp_request *r, const char *uri)
{
	struct http_static_t *cmdp;
	struct stat st;
	char fname[HTTP_FNAME_LEN];
	char last_modified[128];
	char *contenttype;
	int fd;
	int file_size;
	char *buf;
	struct evbuffer *buffer;
	struct evkeyvalq *req_headers;
	const char *ims;
	
	for (cmdp = http_static_files; cmdp->name != NULL; cmdp++)
		if (strncasecmp(cmdp->name, uri, strlen(uri)) == 0)
			break;
			
	if (cmdp->name == NULL) {
		hlog(LOG_DEBUG, "HTTP: 404");
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	snprintf(fname, HTTP_FNAME_LEN, "%s/%s", webdir, cmdp->filename);
	
	hlog(LOG_DEBUG, "static file request %s", uri);
	
	if (stat(fname, &st) == -1) {
		hlog(LOG_ERR, "http static file '%s' not found", fname);
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	/* Generate Last-Modified header, and other headers
	 * (they need to be present in an IMS hit)
	 */
	http_date(last_modified, sizeof(last_modified), st.st_mtime);
	
	contenttype = http_content_type(cmdp->filename);
	//hlog(LOG_DEBUG, "found content-type %s", contenttype);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	evhttp_add_header(headers, "Content-Type", contenttype);
	evhttp_add_header(headers, "Last-Modified", last_modified);
	
	/* Consider an IMS hit */
	req_headers = evhttp_request_get_input_headers(r);
	ims = evhttp_find_header(req_headers, "If-Modified-Since");
	
	if ((ims) && strcasecmp(ims, last_modified) == 0) {
		hlog(LOG_DEBUG, "http static file '%s' IMS hit", fname);
		evhttp_send_reply(r, HTTP_NOTMODIFIED, "Not modified", NULL);
		return;
	}
	
	file_size = st.st_size;  
	
	fd = open(fname, 0, O_RDONLY);
	if (fd < 0) {
		hlog(LOG_ERR, "http static file '%s' could not be opened for reading: %s", fname, strerror(errno));
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		return;
	}
	
	buf = hmalloc(file_size);
	int n = read(fd, buf, file_size);
	
	if (close(fd) < 0) {
		hlog(LOG_ERR, "http static file '%s' could not be closed after reading: %s", fname, strerror(errno));
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		hfree(buf);
		return;
	}
	
	if (n != file_size) {
		hlog(LOG_ERR, "http static file '%s' could only read %d of %d bytes", fname, n, file_size);
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		hfree(buf);
		return;
	}
	
	buffer = evbuffer_new();
	evbuffer_add(buffer, buf, n);
	hfree(buf);
	
	evhttp_send_reply(r, HTTP_OK, "OK", buffer);
	evbuffer_free(buffer);
}

/*
 *	HTTP request router
 */

void http_router(struct evhttp_request *r, void *which_server)
{
	char *remote_host;
	ev_uint16_t remote_port;
	const char *uri = evhttp_request_get_uri(r);
	struct evhttp_connection *conn = evhttp_request_get_connection(r);
	evhttp_connection_get_peer(conn, &remote_host, &remote_port);
	
	hlog(LOG_DEBUG, "http %s [%s] request %s", (which_server == (void *)1) ? "status" : "upload", remote_host, uri);
	
	http_requests++;
	
	/* status server routing */
	if (which_server == (void *)1) {
		if (strncmp(uri, "/status.json", 12) == 0) {
			http_status(r);
			return;
		}
		
		http_route_static(r, uri);
		return;
	}
	
	/* position upload server routing */
	if (which_server == (void *)2) {
		if (strncmp(uri, "/", 7) == 0) {
			http_upload_position(r);
			return;
		}
		
		hlog(LOG_DEBUG, "http request on upload server for '%s': 404 not found", uri);
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	hlog(LOG_ERR, "http request on unknown server for '%s': 404 not found", uri);
	evhttp_send_error(r, HTTP_NOTFOUND, "Server not found");
	
	return;
}

struct event *ev_timer = NULL;
struct evhttp *srvr_status = NULL;
struct evhttp *srvr_upload = NULL;
struct event_base *libbase = NULL;

/*
 *	HTTP timer event, mainly to catch the shutdown signal
 */

void http_timer(evutil_socket_t fd, short events, void *arg)
{
	struct timeval http_timer_tv;
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	//hlog(LOG_DEBUG, "http_timer fired");
	
	if (http_shutting_down) {
		http_timer_tv.tv_usec = 1000;
		event_base_loopexit(libbase, &http_timer_tv);
		return;
	}
	
	event_add(ev_timer, &http_timer_tv);
}

void http_srvr_defaults(struct evhttp *srvr)
{
	// limit what the clients can do a bit
	evhttp_set_allowed_methods(srvr, EVHTTP_REQ_GET);
	evhttp_set_timeout(srvr, 30);
	evhttp_set_max_body_size(srvr, 10*1024);
	evhttp_set_max_headers_size(srvr, 10*1024);
	
	// TODO: How to limit the amount of concurrent HTTP connections?
}

/*
 *	HTTP server thread
 */

void http_thread(void *asdf)
{
	sigset_t sigs_to_block;
	
	struct timeval http_timer_tv;
	
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	pthreads_profiling_reset("http");
	
	sigemptyset(&sigs_to_block);
	sigaddset(&sigs_to_block, SIGALRM);
	sigaddset(&sigs_to_block, SIGINT);
	sigaddset(&sigs_to_block, SIGTERM);
	sigaddset(&sigs_to_block, SIGQUIT);
	sigaddset(&sigs_to_block, SIGHUP);
	sigaddset(&sigs_to_block, SIGURG);
	sigaddset(&sigs_to_block, SIGPIPE);
	sigaddset(&sigs_to_block, SIGUSR1);
	sigaddset(&sigs_to_block, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);
	
	/* start the http thread, which will start server threads */
	hlog(LOG_INFO, "HTTP thread starting...");
	
	http_reconfiguring = 1;
	while (!http_shutting_down) {
		if (http_reconfiguring) {
			http_reconfiguring = 0;
			
			// shut down existing instance
			if (ev_timer) {
				event_del(ev_timer);
			}
			if (srvr_status) {
				evhttp_free(srvr_status);
				srvr_status = NULL;
			}
			if (libbase) {
				event_base_free(libbase);
				libbase = NULL;
			}
			
			// do init
			libbase = event_base_new();
			
			// timer for the whole libevent, to catch shutdown signal
			ev_timer = event_new(libbase, -1, EV_TIMEOUT, http_timer, NULL);
			event_add(ev_timer, &http_timer_tv);
			
			if (http_bind) {
				hlog(LOG_INFO, "Binding HTTP status socket %s:%d", http_bind, http_port);
				
				srvr_status = evhttp_new(libbase);
				http_srvr_defaults(srvr_status);
				
				http_status_handle = evhttp_bind_socket_with_handle(srvr_status, http_bind, http_port);
				if (!http_status_handle) {
					hlog(LOG_ERR, "Failed to bind HTTP status socket %s:%d: %s", http_bind, http_port, strerror(errno));
					// TODO: should exit?
				}
				
				evhttp_set_gencb(srvr_status, http_router, (void *)1);
			}
			
			if (http_bind_upload) {
				hlog(LOG_INFO, "Binding HTTP upload socket %s:%d", http_bind_upload, http_port_upload);
				
				srvr_upload = evhttp_new(libbase);
				http_srvr_defaults(srvr_upload);
				evhttp_set_allowed_methods(srvr_upload, EVHTTP_REQ_POST); /* uploads are POSTs, after all */
				
				http_upload_handle = evhttp_bind_socket_with_handle(srvr_upload, http_bind_upload, http_port_upload);
				if (!http_upload_handle) {
					hlog(LOG_ERR, "Failed to bind HTTP upload socket %s:%d: %s", http_bind_upload, http_port_upload, strerror(errno));
					// TODO: should exit?
				}
				
				evhttp_set_gencb(srvr_upload, http_router, (void *)2);
			}
			
			hlog(LOG_INFO, "HTTP thread ready.");
		}
		
		event_base_dispatch(libbase);
	}
	
	hlog(LOG_DEBUG, "HTTP thread shutting down...");
}

