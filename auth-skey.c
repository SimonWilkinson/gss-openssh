#include "includes.h"
#ifdef SKEY
RCSID("$OpenBSD: auth-skey.c,v 1.7 2000/06/20 01:39:38 markus Exp $");

#include "ssh.h"
#include "packet.h"
#include <openssl/sha.h>

/* from %OpenBSD: skeylogin.c,v 1.32 1999/08/16 14:46:56 millert Exp % */

/*
 * try skey authentication,
 * return 1 on success, 0 on failure, -1 if skey is not available
 */

int
auth_skey_password(struct passwd * pw, const char *password)
{
	if (strncasecmp(password, "s/key", 5) == 0) {
		char *skeyinfo = skey_keyinfo(pw->pw_name);
		if (skeyinfo == NULL) {
			debug("generating fake skeyinfo for %.100s.",
			    pw->pw_name);
			skeyinfo = skey_fake_keyinfo(pw->pw_name);
		}
		if (skeyinfo != NULL)
			packet_send_debug(skeyinfo);
		/* Try again. */
		return 0;
	} else if (skey_haskey(pw->pw_name) == 0 &&
		   skey_passcheck(pw->pw_name, (char *) password) != -1) {
		/* Authentication succeeded. */
		return 1;
	}
	/* Fall back to ordinary passwd authentication. */
	return -1;
}

/* from %OpenBSD: skeylogin.c,v 1.32 1999/08/16 14:46:56 millert Exp % */

#define ROUND(x)   (((x)[0] << 24) + (((x)[1]) << 16) + (((x)[2]) << 8) + \
		    ((x)[3]))

/*
 * hash_collapse()
 */
static u_int32_t
hash_collapse(s)
	u_char *s;
{
	int len, target;
	u_int32_t i;
	
	if ((strlen(s) % sizeof(u_int32_t)) == 0)
		target = strlen(s);    /* Multiple of 4 */
	else
		target = strlen(s) - (strlen(s) % sizeof(u_int32_t));

	for (i = 0, len = 0; len < target; len += 4)
		i ^= ROUND(s + len);

	return i;
}

char *
skey_fake_keyinfo(char *username)
{
	int i;
	u_int ptr;
	u_char hseed[SKEY_MAX_SEED_LEN], flg = 1, *up;
	char pbuf[SKEY_MAX_PW_LEN+1];
	static char skeyprompt[SKEY_MAX_CHALLENGE+1];
	char *secret = NULL;
	size_t secretlen = 0;
	SHA_CTX ctx;
	char *p, *u;

	/*
	 * Base first 4 chars of seed on hostname.
	 * Add some filler for short hostnames if necessary.
	 */
	if (gethostname(pbuf, sizeof(pbuf)) == -1)
		*(p = pbuf) = '.';
	else
		for (p = pbuf; *p && isalnum(*p); p++)
			if (isalpha(*p) && isupper(*p))
				*p = tolower(*p);
	if (*p && pbuf - p < 4)
		(void)strncpy(p, "asjd", 4 - (pbuf - p));
	pbuf[4] = '\0';

	/* Hash the username if possible */
	up = malloc(SHA_DIGEST_LENGTH);
	if (up != NULL) {
		struct stat sb;
		time_t t;
		int fd;

		SHA1_Init(&ctx);
		SHA1_Update(&ctx, username, strlen(username));
		SHA1_Final(up, &ctx);

		/* Collapse the hash */
		ptr = hash_collapse(up);
		memset(up, 0, strlen(up));

		/* See if the random file's there, else use ctime */
		if ((fd = open(_SKEY_RAND_FILE_PATH_, O_RDONLY)) != -1
		    && fstat(fd, &sb) == 0 &&
		    sb.st_size > (off_t)SKEY_MAX_SEED_LEN &&
		    lseek(fd, ptr % (sb.st_size - SKEY_MAX_SEED_LEN),
		    SEEK_SET) != -1 && read(fd, hseed,
		    SKEY_MAX_SEED_LEN) == SKEY_MAX_SEED_LEN) {
			close(fd);
			fd = -1;
			secret = hseed;
			secretlen = SKEY_MAX_SEED_LEN;
			flg = 0;
		} else if (!stat(_PATH_MEM, &sb) || !stat("/", &sb)) {
			t = sb.st_ctime;
			secret = ctime(&t);
			secretlen = strlen(secret);
			flg = 0;
		}
		if (fd != -1)
			close(fd);
	}

	/* Put that in your pipe and smoke it */
	if (flg == 0) {
		/* Hash secret value with username */
		SHA1_Init(&ctx);
		SHA1_Update(&ctx, secret, secretlen);
		SHA1_Update(&ctx, username, strlen(username));
		SHA1_Final(up, &ctx);
		
		/* Zero out */
		memset(secret, 0, secretlen);

		/* Now hash the hash */
		SHA1_Init(&ctx);
		SHA1_Update(&ctx, up, strlen(up));
		SHA1_Final(up, &ctx);
		
		ptr = hash_collapse(up + 4);
		
		for (i = 4; i < 9; i++) {
			pbuf[i] = (ptr % 10) + '0';
			ptr /= 10;
		}
		pbuf[i] = '\0';

		/* Sequence number */
		ptr = ((up[2] + up[3]) % 99) + 1;

		memset(up, 0, SHA_DIGEST_LENGTH); /* SHA1 specific */
		free(up);

		(void)snprintf(skeyprompt, sizeof skeyprompt,
			      "otp-%.*s %d %.*s",
			      SKEY_MAX_HASHNAME_LEN,
			      skey_get_algorithm(),
			      ptr, SKEY_MAX_SEED_LEN,
			      pbuf);
	} else {
		/* Base last 8 chars of seed on username */
		u = username;
		i = 8;
		p = &pbuf[4];
		do {
			if (*u == 0) {
				/* Pad remainder with zeros */
				while (--i >= 0)
					*p++ = '0';
				break;
			}

			*p++ = (*u++ % 10) + '0';
		} while (--i != 0);
		pbuf[12] = '\0';

		(void)snprintf(skeyprompt, sizeof skeyprompt,
			      "otp-%.*s %d %.*s",
			      SKEY_MAX_HASHNAME_LEN,
			      skey_get_algorithm(),
			      99, SKEY_MAX_SEED_LEN, pbuf);
	}
	return skeyprompt;
}

#endif /* SKEY */
