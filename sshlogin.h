/*      $OpenBSD: sshlogin.h,v 1.1 2001/03/04 01:46:30 djm Exp $        */

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
#ifndef LOGIN_H
#define LOGIN_H

/*
 * Returns the time when the user last logged in.  Returns 0 if the
 * information is not available.  This must be called before record_login.
 * The host from which the user logged in is stored in buf.
 */
u_long
get_last_login_time(uid_t uid, const char *logname,
    char *buf, u_int bufsize);

/*
 * Records that the user has logged in.  This does many things normally done
 * by login(1).
 */
void
record_login(pid_t pid, const char *ttyname, const char *user, uid_t uid,
    const char *host, struct sockaddr *addr);

#ifdef LOGIN_NEEDS_UTMPX
/*
 * Record just the utmp info for /bin/login.
 */
void
record_utmp_only(pid_t pid, const char *ttyname, const char *user,
		 const char *host, struct sockaddr * addr);
#endif

/*
 * Records that the user has logged out.  This does many thigs normally done
 * by login(1) or init.
 */
void    record_logout(pid_t pid, const char *ttyname);

#endif
