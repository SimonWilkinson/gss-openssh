/*	$OpenBSD: misc.h,v 1.5 2001/05/03 23:09:52 mouring Exp $	*/

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
/* remove newline at end of string */
char	*chop(char *s);

/* return next token in configuration line */
char	*strdelim(char **s);

/* set filedescriptor to non-blocking */
void	set_nonblock(int fd);

struct passwd * pwcopy(struct passwd *pw);

/*
 * Convert ASCII string to TCP/IP port number.
 * Port must be >0 and <=65535.
 * Return 0 if invalid.
 */
int a2port(const char *s);
 
/* code from scp.c/rcp.c */
char *cleanhostname(char *host);
char *colon(char *cp);

/* wrapper for signal interface */
typedef void (*mysig_t)(int);
mysig_t mysignal(int sig, mysig_t act);
