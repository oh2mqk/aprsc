/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef CLIENT_HEARD_H
#define CLIENT_HEARD_H

#include "worker.h"
#include "cellmalloc.h"

extern void client_heard_update(struct client_t *c, struct pbuf_t *pb);
extern void client_courtesy_update(struct client_t *c, struct pbuf_t *pb);
extern int client_heard_check(struct client_t *c, const char *callsign, int call_len);
extern int client_courtesy_needed(struct client_t *c, const char *callsign, int call_len);

extern void client_heard_free(struct client_t *c);
extern void client_heard_init(void);

#ifndef _FOR_VALGRIND_
extern void client_heard_cell_stats(struct cellstatus_t *cellst);
#endif

#endif

