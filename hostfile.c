/*
 *
 * hostfile.c
 *
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 *
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 *
 * Created: Thu Jun 29 07:10:56 1995 ylo
 *
 * Functions for manipulating the known hosts files.
 *
 */

#include "includes.h"
RCSID("$OpenBSD: hostfile.c,v 1.19 2000/06/06 19:32:13 markus Exp $");

#include "packet.h"
#include "match.h"
#include "ssh.h"
#include <openssl/rsa.h>
#include <openssl/dsa.h>
#include "key.h"
#include "hostfile.h"

/*
 * Parses an RSA (number of bits, e, n) or DSA key from a string.  Moves the
 * pointer over the key.  Skips any whitespace at the beginning and at end.
 */

int
hostfile_read_key(char **cpp, unsigned int *bitsp, Key *ret)
{
	unsigned int bits;
	char *cp;

	/* Skip leading whitespace. */
	for (cp = *cpp; *cp == ' ' || *cp == '\t'; cp++)
		;

	bits = key_read(ret, &cp);
	if (bits == 0)
		return 0;

	/* Skip trailing whitespace. */
	for (; *cp == ' ' || *cp == '\t'; cp++)
		;

	/* Return results. */
	*cpp = cp;
	*bitsp = bits;
	return 1;
}

int
auth_rsa_read_key(char **cpp, unsigned int *bitsp, BIGNUM * e, BIGNUM * n)
{
	Key *k = key_new(KEY_RSA);
	int ret = hostfile_read_key(cpp, bitsp, k);
	BN_copy(e, k->rsa->e);
	BN_copy(n, k->rsa->n);
	key_free(k);
	return ret;
}

int
hostfile_check_key(int bits, Key *key, const char *host, const char *filename, int linenum)
{
	if (key == NULL || key->type != KEY_RSA || key->rsa == NULL)
		return 1;
	if (bits != BN_num_bits(key->rsa->n)) {
		log("Warning: %s, line %d: keysize mismatch for host %s: "
		    "actual %d vs. announced %d.",
		    filename, linenum, host, BN_num_bits(key->rsa->n), bits);
		log("Warning: replace %d with %d in %s, line %d.",
		    bits, BN_num_bits(key->rsa->n), filename, linenum);
	}
	return 1;
}

/*
 * Checks whether the given host (which must be in all lowercase) is already
 * in the list of our known hosts. Returns HOST_OK if the host is known and
 * has the specified key, HOST_NEW if the host is not known, and HOST_CHANGED
 * if the host is known but used to have a different host key.
 */

HostStatus
check_host_in_hostfile(const char *filename, const char *host, Key *key, Key *found)
{
	FILE *f;
	char line[8192];
	int linenum = 0;
	unsigned int kbits, hostlen;
	char *cp, *cp2;
	HostStatus end_return;

	if (key == NULL)
		fatal("no key to look up");
	/* Open the file containing the list of known hosts. */
	f = fopen(filename, "r");
	if (!f)
		return HOST_NEW;

	/* Cache the length of the host name. */
	hostlen = strlen(host);

	/*
	 * Return value when the loop terminates.  This is set to
	 * HOST_CHANGED if we have seen a different key for the host and have
	 * not found the proper one.
	 */
	end_return = HOST_NEW;

	/* Go trough the file. */
	while (fgets(line, sizeof(line), f)) {
		cp = line;
		linenum++;

		/* Skip any leading whitespace, comments and empty lines. */
		for (; *cp == ' ' || *cp == '\t'; cp++)
			;
		if (!*cp || *cp == '#' || *cp == '\n')
			continue;

		/* Find the end of the host name portion. */
		for (cp2 = cp; *cp2 && *cp2 != ' ' && *cp2 != '\t'; cp2++)
			;

		/* Check if the host name matches. */
		if (match_hostname(host, cp, (unsigned int) (cp2 - cp)) != 1)
			continue;

		/* Got a match.  Skip host name. */
		cp = cp2;

		/*
		 * Extract the key from the line.  This will skip any leading
		 * whitespace.  Ignore badly formatted lines.
		 */
		if (!hostfile_read_key(&cp, &kbits, found))
			continue;
		if (!hostfile_check_key(kbits, found, host, filename, linenum))
			continue;

		/* Check if the current key is the same as the given key. */
		if (key_equal(key, found)) {
			/* Ok, they match. */
			fclose(f);
			return HOST_OK;
		}
		/*
		 * They do not match.  We will continue to go through the
		 * file; however, we note that we will not return that it is
		 * new.
		 */
		end_return = HOST_CHANGED;
	}
	/* Clear variables and close the file. */
	fclose(f);

	/*
	 * Return either HOST_NEW or HOST_CHANGED, depending on whether we
	 * saw a different key for the host.
	 */
	return end_return;
}

/*
 * Appends an entry to the host file.  Returns false if the entry could not
 * be appended.
 */

int
add_host_to_hostfile(const char *filename, const char *host, Key *key)
{
	FILE *f;
	int success = 0;
	if (key == NULL)
		return 1;	/* XXX ? */
	f = fopen(filename, "a");
	if (!f)
		return 0;
	fprintf(f, "%s ", host);
	if (key_write(key, f)) {
		success = 1;
	} else {
		error("add_host_to_hostfile: saving key in %s failed", filename);
	}
	fprintf(f, "\n");
	fclose(f);
	return success;
}
