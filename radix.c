/*
 *   radix.c
 *
 *   Dug Song <dugsong@UMICH.EDU>
 */

#include "includes.h"
#include "uuencode.h"

#ifdef AFS
#include <krb.h>

typedef unsigned char my_u_char;
typedef unsigned int my_u_int32_t;
typedef unsigned short my_u_short;

/* Nasty macros from BIND-4.9.2 */

#define GETSHORT(s, cp) { \
	register my_u_char *t_cp = (my_u_char*)(cp); \
	(s) = (((my_u_short)t_cp[0]) << 8) \
	    | (((my_u_short)t_cp[1])) \
	    ; \
	(cp) += 2; \
}

#define GETLONG(l, cp) { \
	register my_u_char *t_cp = (my_u_char*)(cp); \
	(l) = (((my_u_int32_t)t_cp[0]) << 24) \
	    | (((my_u_int32_t)t_cp[1]) << 16) \
	    | (((my_u_int32_t)t_cp[2]) << 8) \
	    | (((my_u_int32_t)t_cp[3])) \
	    ; \
	(cp) += 4; \
}

#define PUTSHORT(s, cp) { \
	register my_u_short t_s = (my_u_short)(s); \
	register my_u_char *t_cp = (my_u_char*)(cp); \
	*t_cp++ = t_s >> 8; \
	*t_cp   = t_s; \
	(cp) += 2; \
}

#define PUTLONG(l, cp) { \
	register my_u_int32_t t_l = (my_u_int32_t)(l); \
	register my_u_char *t_cp = (my_u_char*)(cp); \
	*t_cp++ = t_l >> 24; \
	*t_cp++ = t_l >> 16; \
	*t_cp++ = t_l >> 8; \
	*t_cp   = t_l; \
	(cp) += 4; \
}

#define GETSTRING(s, p, p_l) {			\
    register char* p_targ = (p) + p_l;		\
    register char* s_c = (s);			\
    register char* p_c = (p);			\
    while (*p_c && (p_c < p_targ)) {		\
	*s_c++ = *p_c++;			\
    }						\
    if (p_c == p_targ) {			\
	return 1;				\
    }						\
    *s_c = *p_c++;				\
    (p_l) = (p_l) - (p_c - (p));		\
    (p) = p_c;					\
}


int
creds_to_radix(CREDENTIALS *creds, unsigned char *buf)
{
	char *p, *s;
	int len;
	char temp[2048];

	p = temp;
	*p++ = 1;		/* version */
	s = creds->service;
	while (*s)
		*p++ = *s++;
	*p++ = *s;
	s = creds->instance;
	while (*s)
		*p++ = *s++;
	*p++ = *s;
	s = creds->realm;
	while (*s)
		*p++ = *s++;
	*p++ = *s;

	s = creds->pname;
	while (*s)
		*p++ = *s++;
	*p++ = *s;
	s = creds->pinst;
	while (*s)
		*p++ = *s++;
	*p++ = *s;
	/* Null string to repeat the realm. */
	*p++ = '\0';

	PUTLONG(creds->issue_date, p);
	{
		unsigned int endTime;
		endTime = (unsigned int) krb_life_to_time(creds->issue_date,
							  creds->lifetime);
		PUTLONG(endTime, p);
	}

	memcpy(p, &creds->session, sizeof(creds->session));
	p += sizeof(creds->session);

	PUTSHORT(creds->kvno, p);
	PUTLONG(creds->ticket_st.length, p);

	memcpy(p, creds->ticket_st.dat, creds->ticket_st.length);
	p += creds->ticket_st.length;
	len = p - temp;

	return (uuencode((unsigned char *)temp, len, (char *)buf));
}

int
radix_to_creds(const char *buf, CREDENTIALS *creds)
{

	char *p;
	int len, tl;
	char version;
	char temp[2048];

	if (!(len = uudecode(buf, (unsigned char *)temp, sizeof(temp))))
		return 0;

	p = temp;

	/* check version and length! */
	if (len < 1)
		return 0;
	version = *p;
	p++;
	len--;

	GETSTRING(creds->service, p, len);
	GETSTRING(creds->instance, p, len);
	GETSTRING(creds->realm, p, len);

	GETSTRING(creds->pname, p, len);
	GETSTRING(creds->pinst, p, len);
	/* Ignore possibly different realm. */
	while (*p && len)
		p++, len--;
	if (len == 0)
		return 0;
	p++, len--;

	/* Enough space for remaining fixed-length parts? */
	if (len < (4 + 4 + sizeof(creds->session) + 2 + 4))
		return 0;

	GETLONG(creds->issue_date, p);
	len -= 4;
	{
		unsigned int endTime;
		GETLONG(endTime, p);
		len -= 4;
		creds->lifetime = krb_time_to_life(creds->issue_date, endTime);
	}

	memcpy(&creds->session, p, sizeof(creds->session));
	p += sizeof(creds->session);
	len -= sizeof(creds->session);

	GETSHORT(creds->kvno, p);
	len -= 2;
	GETLONG(creds->ticket_st.length, p);
	len -= 4;

	tl = creds->ticket_st.length;
	if (tl < 0 || tl > len || tl > sizeof(creds->ticket_st.dat))
		return 0;

	memcpy(creds->ticket_st.dat, p, tl);
	p += tl;
	len -= tl;

	return 1;
}
#endif /* AFS */
