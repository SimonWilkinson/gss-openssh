/*	$OpenBSD: canohost.h,v 1.7 2001/06/26 06:32:49 itojun Exp $	*/

/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

/*
 * Return the canonical name of the host in the other side of the current
 * connection (as returned by packet_get_connection).  The host name is
 * cached, so it is efficient to call this several times.
 */
const char *get_canonical_hostname(int);

/*
 * Returns the IP-address of the remote host as a string.  The returned
 * string is cached and must not be freed.
 */
const char *get_remote_ipaddr(void);

const char *get_remote_name_or_ip(u_int, int);

/* Returns the ipaddr/port number of the peer of the socket. */
char *	get_peer_ipaddr(int);
int     get_peer_port(int);
char *	get_local_ipaddr(int);
char *	get_local_name(int);

/* Returns the port number of the remote/local host. */
int     get_remote_port(void);
int	get_local_port(void);
