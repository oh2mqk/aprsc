/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

/*
 *	config.c: configuration parsing, based on Tomi's code
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/resource.h>

#include "config.h"
#include "hmalloc.h"
#include "hlog.h"
#include "cfgfile.h"
#include "worker.h"
#include "filter.h"

char def_cfgfile[] = "aprsc.conf";
char def_webdir[] = "web";

char *cfgfile = def_cfgfile;
char *pidfile;
char *new_rundir;
char *rundir;
char *new_logdir;
char *logdir;	/* access logs go here */
//char *new_webdir;
char *webdir = def_webdir;
char *chrootdir = NULL;
char *setuid_s = NULL;

char def_logname[] = "aprsc";
char *logname = def_logname;	/* syslog entries use this program name */

char *mycall;
char *myemail;
char *myadmin;
char *new_mycall;
char *new_myemail;
char *new_myadmin;

struct uplink_config_t *uplink_config;
struct uplink_config_t *new_uplink_config;
struct peerip_config_t *peerip_config;
struct peerip_config_t *new_peerip_config;

char http_bind_default[] = "0.0.0.0";
char *http_bind = http_bind_default;	/* http address string to listen on */
int http_port = 14501;
char *new_http_bind;
int new_http_port;
char *http_bind_upload = NULL;	/* http address string to listen on */
int http_port_upload = 8080;
char *new_http_bind_upload;
int new_http_port_upload;

int fork_a_daemon;	/* fork a daemon */

int dump_splay;	/* print splay tree information */

int workers_configured =  2;	/* number of workers to run */

int expiry_interval    = 30;
int stats_interval     = 1 * 60;

int lastposition_storetime = 24*60*60;	/* how long the last position packet of each station is stored */
int dupefilter_storetime   =     30;	/* how long to store information required for dupe filtering */

int heard_list_storetime   =     3*60*60; /* how long to store "client X has heard station Y" information,
                                           * to support text message routing */
int courtesy_list_storetime   =    30*60; /* how long to store "client X has been given MSG from station Y" information,
                                           * to support courtesy position transmission after text message routing */

int pbuf_global_expiration       = 10;//35*60; /* 35 minutes */
int pbuf_global_dupe_expiration  = 10;//10*60; /* 10 minutes */

int upstream_timeout      = 60;		/* after N seconds of no input from an upstream, disconnect */
int client_timeout        = 30*60;	/* after N seconds of no input from a client, disconnect */
int client_login_timeout  = 30;		/* after N seconds of no login command from a client, disconnect */

int ibuf_size = 8100;			/* size of input buffer for clients */
int obuf_size = 32*1024;		/* size of output buffer for clients */

int new_fileno_limit;

int disallow_unverified = 1;		/* don't allow unverified clients to transmit packets with srccall != login */

int verbose;

/* address:port pairs being listened */
struct listen_config_t *listen_config = NULL, *listen_config_new = NULL;

int do_httpstatus(char *new, int argc, char **argv);
int do_httpupload(char *new, int argc, char **argv);
int do_listen(struct listen_config_t **lq, int argc, char **argv);
int do_interval(int *dest, int argc, char **argv);
int do_peergroup(struct peerip_config_t **lq, int argc, char **argv);
int do_uplink(struct uplink_config_t **lq, int argc, char **argv);

/*
 *	Configuration file commands
 */

#define _CFUNC_ (int (*)(void *dest, int argc, char **argv))

static struct cfgcmd cfg_cmds[] = {
	{ "rundir",		_CFUNC_ do_string,	&new_rundir		},
	{ "logdir",		_CFUNC_ do_string,	&new_logdir		},
	{ "mycall",		_CFUNC_ do_string,	&new_mycall		},
	{ "myemail",		_CFUNC_ do_string,	&new_myemail		},
	{ "myadmin",		_CFUNC_ do_string,	&new_myadmin		},
	{ "workerthreads",	_CFUNC_ do_int,		&workers_configured	},
	{ "statsinterval",	_CFUNC_ do_interval,	&stats_interval		},
	{ "expiryinterval",	_CFUNC_ do_interval,	&expiry_interval	},
	{ "lastpositioncache",	_CFUNC_ do_interval,	&lastposition_storetime	},
	{ "upstreamtimeout",	_CFUNC_ do_interval,	&upstream_timeout	},
	{ "clienttimeout",	_CFUNC_ do_interval,	&client_timeout		},
	{ "logintimeout",	_CFUNC_ do_interval,	&client_login_timeout	},
	{ "filelimit",		_CFUNC_ do_int,		&new_fileno_limit	},
	{ "httpstatus",		_CFUNC_ do_httpstatus,	&new_http_bind		},
	{ "httpupload",		_CFUNC_ do_httpupload,	&new_http_bind_upload	},
	{ "listen",		_CFUNC_ do_listen,	&listen_config_new	},
	{ "uplink",		_CFUNC_ do_uplink,	&new_uplink_config	},
	{ "peergroup",		_CFUNC_ do_peergroup,	&new_peerip_config	},
	{ "disallow_unverified",	_CFUNC_ do_boolean,	&disallow_unverified	},
	{ NULL,			NULL,			NULL			}
};

/*
 *	Parse a command line to argv, not honoring quotes or such
 */
 
int parse_args_noshell(char *argv[],char *cmd)
{
	int ct = 0;
	
	while (ct < 255)
	{
		while (*cmd && isspace((int)*cmd))
			cmd++;
		if (*cmd == 0)
			break;
		argv[ct++] = cmd;
		while (*cmd && !isspace((int)*cmd))
			cmd++;
		if (*cmd)
			*cmd++ = 0;
	}
	argv[ct] = NULL;
	return ct;
}

/*
 *	Free a listen config tree
 */

void free_listen_config(struct listen_config_t **lc)
{
	struct listen_config_t *this;
	int i;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		hfree((void*)this->name);
		hfree((void*)this->host);
		for (i = 0; i < (sizeof(this->filters)/sizeof(this->filters[0])); ++i)
			if (this->filters[i])
				hfree((void*)this->filters[i]);
		freeaddrinfo(this->ai);
		if (this->acl)
			acl_free(this->acl);
		hfree(this);
	}
}

/*
 *	Free a peer-ip config tree
 */

void free_peerip_config(struct peerip_config_t **lc)
{
	struct peerip_config_t *this;
	int i;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		hfree((void*)this->name);
		hfree((void*)this->host);
		for (i = 0; i < (sizeof(this->filters)/sizeof(this->filters[0])); ++i)
			if (this->filters[i])
				hfree((void*)this->filters[i]);
		freeaddrinfo(this->ai);
		hfree(this);
	}
}

/*
 *	Free a uplink config tree
 */

void free_uplink_config(struct uplink_config_t **lc)
{
	struct uplink_config_t *this;
	int i;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		hfree((void*)this->name);
		hfree((void*)this->proto);
		hfree((void*)this->host);
		hfree((void*)this->port);
		for (i = 0; i < (sizeof(this->filters)/sizeof(this->filters[0])); ++i)
			if (this->filters[i])
				hfree((void*)this->filters[i]);
		hfree(this);
	}
}

/*
 *	parse an interval specification
 */
 
time_t parse_interval(char *origs)
{
	time_t t = 0;
	int i;
	char *s, *np, *p, c;
	
	np = p = s = hstrdup(origs);
	
	while (*p) {
		if (!isdigit((int)*p)) {
			c = tolower(*p);
			*p = '\0';
			i = atoi(np);
			if (c == 's')
				t += i;
			else if (c == 'm')
				t += 60 * i;
			else if (c == 'h')
				t += 60 * 60 * i;
			else if (c == 'd')
				t += 24 * 60 * 60 * i;
			np = p + 1;
		}
		p++;
	}
	
	if (*np)
		t += atoi(np);
		
	hfree(s);
	return t;
}


/*
 *	Parse an interval configuration entry
 */

int do_interval(int *dest, int argc, char **argv)
{
	if (argc < 2)
		return -1;
		
	*dest = parse_interval(argv[1]);
	return 0;
}

/*
 *	Parse a peer definition directive
 *
 *	"keyword" <token?> [udp|sctp] <localhost>:<localport> <remotehost1>:<remoteport> <remote2> ...
 *
 */

int parse_hostport(char *s, char **host_s, char **port_s)
{
	char *colon;
	
	colon = strrchr(s, ':');
	if (colon == NULL)
		return -1;
	
	*colon = 0;
	
	*host_s = s;
	*port_s = colon+1;
	
	return 0;
}

int do_peergroup(struct peerip_config_t **lq, int argc, char **argv)
{
	int localport, port, i, d;
	struct peerip_config_t *pe;
	struct listen_config_t *li;
	struct addrinfo req, *ai, *a;
	char *fullhost, *host_s, *port_s;
	int af;

	if (argc < 4)
		return -1;
	
	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;

	// Only UDP and SCTP are acceptable for peergroups
	if (strcasecmp(argv[2], "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#if defined(SOCK_SEQPACKET) && defined(IPPROTO_SCTP)
	} else if (strcasecmp(argv[2], "sctp") == 0) {
		req.ai_socktype = SOCK_SEQPACKET;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "PeerGroup: Unsupported protocol '%s'", argv[2]);
		return -2;
	}
	
	fullhost = hstrdup(argv[3]);
	
	if (parse_hostport(argv[3], &host_s, &port_s)) {
		hlog(LOG_ERR, "PeerGroup: Invalid local host:port specification '%s'", argv[3]);
		hfree(fullhost);
		return -2;
	}
	
	localport = atoi(port_s);
	if (localport < 1 || localport > 65535) {
		hlog(LOG_ERR, "PeerGroup: Invalid local port number '%s'", port_s);
		hfree(fullhost);
		return -2;
	}
	
	i = getaddrinfo(host_s, port_s, &req, &ai);
	if (i != 0) {
		hlog(LOG_ERR, "PeerGroup: address parsing or hostname lookup failure for %s : %s", host_s, port_s);
		hfree(fullhost);
		return i;
	}
	
	d = 0;
	for (a = ai; (a); a = a->ai_next, ++d);
	if (d != 1) {
		hlog(LOG_ERR, "PeerGroup: address parsing for local address %s returned %d addresses - can only have one", host_s, d);
		return -2;
	}
	
	af = ai->ai_family;
	
	hlog(LOG_DEBUG, "PeerGroup: configuring with local address %s:%s (local port %d)", host_s, port_s, localport);
	
	/* Configure a listener */
	li = hmalloc(sizeof(*li));
	memset(li, 0, sizeof(*li));
	li->corepeer = 1;
	li->name = hstrdup(argv[1]);
	li->host = fullhost;
	li->portnum      = localport;
	li->client_flags = 0;
	li->clients_max  = 1;
	li->ai = ai;
	li->acl = NULL;
	li->next = NULL;
	li->prevp = NULL;
	
	/* there are no filters between peers */
	for (i = 0; i < LISTEN_MAX_FILTERS; i++)
		li->filters[i] = NULL;
	
	/* put in the list */
	li->next = listen_config_new;
	if (li->next)
		li->next->prevp = &li->next;
	listen_config_new = li;
	
	for (i = 4; i < argc; i++) {
		hlog(LOG_DEBUG, "PeerGroup: configuring peer %s", argv[i]);
		
		if (parse_hostport(argv[i], &host_s, &port_s)) {
			hlog(LOG_ERR, "PeerGroup: Invalid remote host:port specification '%s'", argv[i]);
			//hfree(fullhost);
			return -2;
		}
		
		port = atoi(port_s);
		if (port < 1 || port > 65535) {
			hlog(LOG_ERR, "PeerGroup: Invalid port number '%s' for host '%s'", port_s, host_s);
			//hfree(fullhost);
			return -2;
		}
		
		d = getaddrinfo(host_s, port_s, &req, &ai);
		if (d != 0) {
			hlog(LOG_ERR, "PeerGroup: address parsing or hostname lookup failure for %s : %s", host_s, port_s);
			//hfree(fullhost);
			return d;
		}
		
		/* we can only allow one address per peer at this point, SCTP multihoming ignored */
		d = 0;
		for (a = ai; (a); a = a->ai_next, ++d);
		if (d != 1) {
			hlog(LOG_ERR, "PeerGroup: address parsing for remote %s returned %d addresses - can only have one", host_s, d);
			return -2;
		}
		
		if (ai->ai_family != af) {
			hlog(LOG_ERR, "PeerGroup: remote address %s has different address family than the local address - mixing IPv4 and IPv6, are we?", host_s);
			return -2;
		}
		
		pe = hmalloc(sizeof(*pe));
		memset(pe, 0, sizeof(*pe));
		pe->name = hstrdup(host_s);
		pe->host = hstrdup(host_s);
		pe->local_port = localport;
		pe->remote_port = port;
		pe->client_flags = 0; // ???
		pe->ai = ai;
		
		/* there are no filters between peers */
		for (d = 0; d < (sizeof(pe->filters)/sizeof(pe->filters[0])); ++d)
			pe->filters[d] = NULL;
		
		/* put in the list */
		pe->next = *lq;
		if (pe->next)
			pe->next->prevp = &pe->next;
		*lq = pe;
	}
	
	return 0;
}

/*
 *	Parse a uplink definition directive
 *
 *	uplink <label> <token> {tcp|udp|sctp} <hostname> <portnum> [<filter> [..<more_filters>]]
 *
 */

int do_uplink(struct uplink_config_t **lq, int argc, char **argv)
{
	struct uplink_config_t *l;
	int i, port;
	struct addrinfo req, *ai;
	int clflags = CLFLAGS_UPLINKPORT;

	if (argc < 5)
		return -1;

	/* argv[1] is  name label  for this uplink */

	if (strcasecmp(argv[2], "ro")==0) {
		clflags |= CLFLAGS_PORT_RO;
	} else if (strcasecmp(argv[2], "multiro")==0) {
		clflags |= CLFLAGS_PORT_RO|CLFLAGS_UPLINKMULTI;
	} else if (strcasecmp(argv[2], "full") == 0) {
		/* regular */
	} else {
		hlog(LOG_ERR, "Uplink: Unsupported uplink type '%s'", argv[2]);
		return -2;
	}

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;

	if (strcasecmp(argv[3], "tcp") == 0) {
		// well, do nothing for now.
	} else if (strcasecmp(argv[3], "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#if defined(SOCK_SEQPACKET) && defined(IPPROTO_SCTP)
	} else if (strcasecmp(argv[3], "sctp") == 0) {
		req.ai_socktype = SOCK_SEQPACKET;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "Uplink: Unsupported protocol '%s'", argv[3]);
		return -2;
	}
	
	port = atoi(argv[5]);
	if (port < 1 || port > 65535) {
		hlog(LOG_ERR, "Uplink: Invalid port number '%s'", argv[5]);
		return -2;
	}

#if 0
	i = getaddrinfo(argv[4], argv[5], &req, &ai);
	if (i != 0) {
		hlog(LOG_INFO,"Uplink: address resolving failure of '%s' '%s'",argv[4],argv[5]);
		/* But do continue, this is perhaps a temporary glitch ? */
	}
	if (ai)
		freeaddrinfo(ai);
#endif
	l = hmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->name  = hstrdup(argv[1]);
	l->proto = hstrdup(argv[3]);
	l->host  = hstrdup(argv[4]);
	l->port  = hstrdup(argv[5]);
	l->client_flags = clflags;
	l->state = UPLINK_ST_UNKNOWN;

	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
		l->filters[i] = NULL;
		if (argc - 6 > i) {
			if (filter_parse(NULL,argv[i+6],0) < 0) {
			  hlog( LOG_ERR,"Bad filter definition on '%s' port %s: '%s'",
				argv[1],argv[5],argv[i+6] );
			  continue;
			}
			l->filters[i] = hstrdup(argv[i+6]);
		}
	}
	
	/* put in the list */
	l->next = *lq;
	if (l->next)
		l->next->prevp = &l->next;
	*lq = l;
	
	return 0;
}

/*
 *	Parse a Listen directive
 *
 *	listen <label> <token> [tcp|udp|sctp] <hostname> <portnum> [<filter> [..<more_filters>]]
 *
 */

int config_parse_listen_filter(struct listen_config_t *l, char *filt_string, char *portname)
{
	int argc;
	char *argv[256];
	int i;
	
	argc = parse_args_noshell(argv, filt_string);
	if (argc == 0) {
		hlog(LOG_ERR, "Listen: Bad filter definition for '%s': '%s' - no filter arguments found",
			portname, filt_string);
		return -1;
	}
	
	if (argc > LISTEN_MAX_FILTERS) {
		hlog(LOG_ERR, "Listen: Bad filter definition for '%s': '%s' - too many (%d) filter arguments found, max %d",
			portname, filt_string, argc, LISTEN_MAX_FILTERS);
		return -1;
	}
	
	for (i = 0; i < argc && i < LISTEN_MAX_FILTERS; i++) {
		if (filter_parse(NULL, argv[i], 0) < 0) {
			hlog(LOG_ERR, "Listen: Bad filter definition for '%s': '%s' - filter parsing failed",
				portname, argv[i]);
			return -1;
		}
		l->filters[i] = hstrdup(argv[i]);
	}
	
	return 0;
}

int do_listen(struct listen_config_t **lq, int argc, char **argv)
{
	int i, port;
	struct listen_config_t *l;
	struct addrinfo req, *ai;
	/* default parameters for a listener */
	int clflags = CLFLAGS_INPORT;
	int clients_max = 200;

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;

	if (argc < 6)
		return -1;

	if (strcasecmp(argv[2], "userfilter") == 0) {
	  clflags |= CLFLAGS_USERFILTEROK;
	} else if (strcasecmp(argv[2], "fullfeed") == 0) {
	  clflags |= CLFLAGS_FULLFEED;
	} else if (strcasecmp(argv[2], "dupefeed") == 0) {
	  clflags |= CLFLAGS_DUPEFEED;
	} else if (strcasecmp(argv[2], "messageonly") == 0) {
	  clflags |= CLFLAGS_MESSAGEONLY;
	  clflags |= CLFLAGS_USERFILTEROK;
	} else if (strcasecmp(argv[2], "clientonly") == 0) {
	  clflags |= CLFLAGS_MESSAGEONLY;
	  clflags |= CLFLAGS_CLIENTONLY;
	  clflags |= CLFLAGS_USERFILTEROK;
	} else if (strcasecmp(argv[2], "igate") == 0) {
	  clflags |= CLFLAGS_MESSAGEONLY;
	  clflags |= CLFLAGS_IGATE;
	  clflags |= CLFLAGS_USERFILTEROK;
	} else if (strcasecmp(argv[2], "uplinksim") == 0) {
	  clflags = CLFLAGS_UPLINKSIM; /* _removes_ INPORT flag! */
	} else {
	  hlog(LOG_ERR, "Listen: unknown quality token: %s", argv[2]);
	}
	
	if (strcasecmp(argv[3], "tcp") == 0) {
		/* well, do nothing for now. */
	} else if (strcasecmp(argv[3], "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#if defined(SOCK_SEQPACKET) && defined(IPPROTO_SCTP)
	} else if (strcasecmp(argv[3], "sctp") == 0) {
		req.ai_socktype = SOCK_SEQPACKET;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "Listen: Unsupported protocol '%s'\n", argv[3]);
		return -2;
	}
	
	port = atoi(argv[5]);
	if (port < 1 || port > 65535) {
		hlog(LOG_ERR, "Listen: Invalid port number '%s'\n", argv[5]);
		return -2;
	}

	i = getaddrinfo(argv[4], argv[5], &req, &ai);
	if (i != 0) {
		hlog(LOG_ERR, "Listen: address parse failure of '%s' '%s'", argv[4], argv[5]);
		return i;
	}
	
	l = hmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->name = hstrdup(argv[1]);
	l->host = hstrdup(argv[4]);
	l->portnum      = port;
	l->client_flags = clflags;
	l->clients_max  = clients_max;
	l->ai = ai;
	l->acl = NULL;
	l->next = NULL;
	l->prevp = NULL;
	
	/* by default, no filters */
	for (i = 0; i < LISTEN_MAX_FILTERS; i++)
		l->filters[i] = NULL;
	
	/* parse rest of arguments */
	i = 6;
	while (i < argc) {
		if (strcasecmp(argv[i], "filter") == 0) {
			/* set a filter for the clients */
			i++;
			if (i >= argc) {
				hlog(LOG_ERR, "Listen: 'filter' argument is missing the filter parameter for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			
			if (config_parse_listen_filter(l, argv[i], argv[1])) {
				free_listen_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "maxclients") == 0) {
			/* Limit amount of clients */
			i++;
			if (i >= argc) {
				hlog(LOG_ERR, "Listen: 'maxclients' argument is missing the numeric max clients limit for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			l->clients_max = atoi(argv[i]);
		} else if (strcasecmp(argv[i], "acl") == 0) {
			/* Access list */
			i++;
			if (i >= argc) {
				hlog(LOG_ERR, "Listen: 'acl' argument is missing the acl parameter for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			
			if (l->acl) {
				hlog(LOG_ERR, "Listen: second 'acl' not allowed for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			
			l->acl = acl_load(argv[i]);
			if (!l->acl) {
				free_listen_config(&l);
				return -2;
			}
			
		} else {
			hlog(LOG_ERR, "Listen: Unknown argument '%s' for '%s'", argv[i], argv[1]);
			free_listen_config(&l);
			return -2;
		}
		i++;
	}
	
	/* put in the list */
	l->next = *lq;
	if (l->next)
		l->next->prevp = &l->next;
	*lq = l;
	
	return 0;
}

int do_http_listener(char *what, char **bind, int *port, int argc, char **argv)
{
	if (argc != 3) {
		hlog(LOG_ERR, "%s: Invalid number of arguments", what);
		return -1;
	}
	
	*bind = hstrdup(argv[1]);
	*port = atoi(argv[2]);
	
	return 0;
}

int do_httpstatus(char *new, int argc, char **argv)
{
	return do_http_listener("HTTPStatus", &new_http_bind, &new_http_port, argc, argv);
}

int do_httpupload(char *new, int argc, char **argv)
{
	return do_http_listener("HTTPUpload", &new_http_bind_upload, &new_http_port_upload, argc, argv);
}

/*
 *	Validate an APRS-IS callsign
 */

int valid_aprsis_call(char *s)
{
	// TODO: use the other function in q parser which is stricter
	if (strlen(s) > 12)
		return 0;
	if (strlen(s) < 3)
		return 0;
	
	return 1;
}

/*
 *	upcase
 */
 
char *strupr(char *s)
{
	char *p;
	
	for (p = s; (*p); p++)
		*p = toupper(*p);
		
	return s;
}

/*
 *	Read configuration files, should add checks for this program's
 *	specific needs and obvious misconfigurations!
 */

int read_config(void)
{
	int failed = 0;
	char *s;
	
	if (read_cfgfile(cfgfile, cfg_cmds))
		return -1;
	
	/* these parameters will only be used when reading the configuration
	 * for the first time.
	 */
	if (!rundir) {
		if (new_rundir) {
			rundir = new_rundir;
			new_rundir = NULL;
		} else {
			hlog(LOG_CRIT, "Config: rundir not defined.");
			failed = 1;
		}
	}
	if (!logdir) {
		if (new_logdir) {
			logdir = new_logdir;
			new_logdir = NULL;
		} else {
			hlog(LOG_CRIT, "Config: logdir not defined.");
			failed = 1;
		}
	}
	
	/* mycall is only applied when running for the first time. */
	if (mycall) {
		if (new_mycall && strcasecmp(new_mycall, mycall) != 0)
			hlog(LOG_WARNING, "Config: Not changing mycall while running.");
		hfree(new_mycall);
		new_mycall = NULL;
	} else {
		if (!new_mycall) {
			hlog(LOG_CRIT, "Config: mycall is not defined.");
			failed = 1;
		} else if (!valid_aprsis_call(new_mycall)) {
			hlog(LOG_CRIT, "Config: mycall '%s' is not valid.", new_mycall);
			failed = 1;
		} else {
			strupr(new_mycall);
			mycall = new_mycall;
			new_mycall = NULL;
		}
	}
	
	if (new_myadmin) {
		if (myadmin)
			hfree(myadmin);
		myadmin = new_myadmin;
		new_myadmin = NULL;
	} else {
		hlog(LOG_WARNING, "Config: myadmin is not defined.");
		failed = 1;
	}
	
	if (new_myemail) {
		if (myemail)
			hfree(myemail);
		myemail = new_myemail;
		new_myemail = NULL;
	} else {
		hlog(LOG_WARNING, "Config: myemail is not defined.");
		failed = 1;
	}
	
	if (new_http_bind) {
		if (http_bind && http_bind != http_bind_default)
			hfree(http_bind);
		http_bind = new_http_bind;
		new_http_bind = NULL;
		http_port = new_http_port;
	}
	
	if (new_http_bind_upload) {
		if (http_bind_upload)
			hfree(http_bind_upload);
		http_bind_upload = new_http_bind_upload;
		new_http_bind_upload = NULL;
		http_port_upload = new_http_port_upload;
	}
	
	/* validate uplink config: if there is a single 'multiro' connection
	 * configured, all of the uplinks must be 'multiro'
	 */
	int uplink_config_failed = 0;
	int got_multiro = 0;
	int got_non_multiro = 0;
	struct uplink_config_t *up;
	for (up = new_uplink_config; (up); up = up->next) {
		if (up->client_flags & CLFLAGS_UPLINKMULTI)
			got_multiro = 1;
		else
			got_non_multiro = 1;
		if ((up->client_flags & CLFLAGS_UPLINKMULTI) && !(up->client_flags & CLFLAGS_PORT_RO)) {
			uplink_config_failed = 1;
			hlog(LOG_WARNING, "Config: uplink with non-RO MULTI uplink - would cause a loop, not allowed.");
		}
	}
	if ((got_multiro) && (got_non_multiro)) {
		hlog(LOG_WARNING, "Config: Configured both multiro and non-multiro uplinks - would cause a loop, not allowed.");
		failed = 1;
		free_uplink_config(&new_uplink_config);
	}
	if (uplink_config_failed)
		free_uplink_config(&new_uplink_config);

	if (new_fileno_limit > 0 && new_fileno_limit != fileno_limit) {
		/* Adjust process global fileno limit */
		int e;
		struct rlimit rlim;
		e = getrlimit(RLIMIT_NOFILE, &rlim);
		rlim.rlim_cur = rlim.rlim_max = new_fileno_limit;
		e = setrlimit(RLIMIT_NOFILE, &rlim);
		e = getrlimit(RLIMIT_NOFILE, &rlim);
		fileno_limit = rlim.rlim_cur;
		if (fileno_limit < new_fileno_limit)
			hlog(LOG_WARNING, "Configuration could not raise FileLimit (possibly not running as root), it is now %d", fileno_limit);
		else
			hlog(LOG_INFO, "After configuration FileLimit is %d", fileno_limit);
	}
	
	if (workers_configured < 1) {
		hlog(LOG_WARNING, "Configured less than 1 worker threads. Using 1.");
	} else if (workers_configured > 32) {
		hlog(LOG_WARNING, "Configured more than 32 worker threads. Using 32.");
		workers_configured = 32;
	}
	
	/* put in the new listening config */
	free_listen_config(&listen_config);
	listen_config = listen_config_new;
	if (listen_config)
		listen_config->prevp = &listen_config;
	listen_config_new = NULL;

	/* put in the new aprsis-uplink  config */
	free_uplink_config(&uplink_config);
	uplink_config = new_uplink_config;
	if (uplink_config)
		uplink_config->prevp = &uplink_config;
	new_uplink_config = NULL;

	/* put in the new aprsis-peerip  config */
	free_peerip_config(&peerip_config);
	peerip_config = new_peerip_config;
	if (peerip_config)
		peerip_config->prevp = &peerip_config;
	new_peerip_config = NULL;

	
	if (failed)
		return -1;
	
	if (!pidfile) {
		s = hmalloc(strlen(logdir) + 10 + 3);
		sprintf(s, "%s/%s.pid", logdir, logname);
		
		pidfile = s;
	}
	
	return 0;
}

/*
 *	Free configuration variables
 */

void free_config(void)
{
	if (logdir)
		hfree(logdir);
	logdir = NULL;
	if (rundir)
		hfree(rundir);
	rundir = NULL;
	if (pidfile)
		hfree(pidfile);
	pidfile = NULL;
	if (cfgfile != def_cfgfile)
		hfree(cfgfile);
	cfgfile = NULL;
	if (logname != def_logname)
		hfree(logname);
	if (webdir != def_webdir)
		hfree(webdir);
	hfree(mycall);
	hfree(myemail);
	hfree(myadmin);
	mycall = myemail = myadmin = NULL;
	logname = NULL;
	free_listen_config(&listen_config);
	free_uplink_config(&uplink_config);
}

