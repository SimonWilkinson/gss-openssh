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
 * Returns the name of the machine at the other end of the socket.  The
 * returned string should be freed by the caller.
 */
char   *get_remote_hostname(int socket);

/*
 * Return the canonical name of the host in the other side of the current
 * connection (as returned by packet_get_connection).  The host name is
 * cached, so it is efficient to call this several times.
 */
const char *get_canonical_hostname(void);

/*
 * Returns the remote IP address as an ascii string.  The value need not be
 * freed by the caller.
 */
const char *get_remote_ipaddr(void);

/* Returns the port number of the peer of the socket. */
int     get_peer_port(int sock);

/* Returns the port number of the remote/local host. */
int     get_remote_port(void);
int	get_local_port(void);
