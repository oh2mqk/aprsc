/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

#ifndef FILTER_H
#define FILTER_H 1

#include "worker.h"


extern void filter_init(void);
extern int  filter_parse(struct client_t *c, const char *filt, int is_user_filter);
extern void filter_free(struct filter_t *c);
extern int  filter_process(struct worker_t *self, struct client_t *c, struct pbuf_t *pb);
extern int  filter_commands(struct worker_t *self, struct client_t *c, const char *s, const int len);

extern float filter_lat2rad(float lat);
extern float filter_lon2rad(float lon);

#endif /* FILTER_H */
