/*
 *
 * rsa.c
 *
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 *
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 *
 * Created: Fri Mar  3 22:07:06 1995 ylo
 *
 * Description of the RSA algorithm can be found e.g. from the following sources:
 *
 *   Bruce Schneier: Applied Cryptography.  John Wiley & Sons, 1994.
 *
 *   Jennifer Seberry and Josed Pieprzyk: Cryptography: An Introduction to
 *   Computer Security.  Prentice-Hall, 1989.
 *
 *   Man Young Rhee: Cryptography and Secure Data Communications.  McGraw-Hill,
 *   1994.
 *
 *   R. Rivest, A. Shamir, and L. M. Adleman: Cryptographic Communications
 *   System and Method.  US Patent 4,405,829, 1983.
 *
 *   Hans Riesel: Prime Numbers and Computer Methods for Factorization.
 *   Birkhauser, 1994.
 *
 *   The RSA Frequently Asked Questions document by RSA Data Security, Inc., 1995.
 *
 *   RSA in 3 lines of perl by Adam Back <aba@atlax.ex.ac.uk>, 1995, as included
 *   below:
 *
 *     [gone - had to be deleted - what a pity]
 *
*/

#include "includes.h"
RCSID("$OpenBSD: rsa.c,v 1.15 2000/06/20 01:39:44 markus Exp $");

#include "rsa.h"
#include "ssh.h"
#include "xmalloc.h"
#include "entropy.h"

int rsa_verbose = 1;

int
rsa_alive()
{
	RSA *key;

	seed_rng();
	key = RSA_generate_key(32, 3, NULL, NULL);
	if (key == NULL)
		return (0);
	RSA_free(key);
	return (1);
}

/*
 * Key generation progress meter callback
 */
void
keygen_progress(int p, int n, void *arg)
{
	const char progress_chars[] = ".o+O?";

	if ((p < 0) || (p > (sizeof(progress_chars) - 2)))
		p = sizeof(progress_chars) - 2;

	putchar(progress_chars[p]);
	fflush(stdout);
}

/*
 * Generates RSA public and private keys.  This initializes the data
 * structures; they should be freed with rsa_clear_private_key and
 * rsa_clear_public_key.
 */

void
rsa_generate_key(RSA *prv, RSA *pub, unsigned int bits)
{
	RSA *key;

	seed_rng();
	
	if (rsa_verbose) {
		printf("Generating RSA keys:  ");
		fflush(stdout);
		key = RSA_generate_key(bits, 35, keygen_progress, NULL);
		printf("\n");
	} else {
		key = RSA_generate_key(bits, 35, NULL, NULL);
	}
	if (key == NULL)
		fatal("rsa_generate_key: key generation failed.");

	/* Copy public key parameters */
	pub->n = BN_new();
	BN_copy(pub->n, key->n);
	pub->e = BN_new();
	BN_copy(pub->e, key->e);

	/* Copy private key parameters */
	prv->n = BN_new();
	BN_copy(prv->n, key->n);
	prv->e = BN_new();
	BN_copy(prv->e, key->e);
	prv->d = BN_new();
	BN_copy(prv->d, key->d);
	prv->p = BN_new();
	BN_copy(prv->p, key->p);
	prv->q = BN_new();
	BN_copy(prv->q, key->q);

	prv->dmp1 = BN_new();
	BN_copy(prv->dmp1, key->dmp1);

	prv->dmq1 = BN_new();
	BN_copy(prv->dmq1, key->dmq1);

	prv->iqmp = BN_new();
	BN_copy(prv->iqmp, key->iqmp);

	RSA_free(key);

	if (rsa_verbose)
		printf("Key generation complete.\n");
}

void
rsa_public_encrypt(BIGNUM *out, BIGNUM *in, RSA *key)
{
	unsigned char *inbuf, *outbuf;
	int len, ilen, olen;

	if (BN_num_bits(key->e) < 2 || !BN_is_odd(key->e))
		fatal("rsa_public_encrypt() exponent too small or not odd");

	olen = BN_num_bytes(key->n);
	outbuf = xmalloc(olen);

	ilen = BN_num_bytes(in);
	inbuf = xmalloc(ilen);
	BN_bn2bin(in, inbuf);

	if ((len = RSA_public_encrypt(ilen, inbuf, outbuf, key,
	    RSA_PKCS1_PADDING)) <= 0)
		fatal("rsa_public_encrypt() failed");

	BN_bin2bn(outbuf, len, out);

	memset(outbuf, 0, olen);
	memset(inbuf, 0, ilen);
	xfree(outbuf);
	xfree(inbuf);
}

void
rsa_private_decrypt(BIGNUM *out, BIGNUM *in, RSA *key)
{
	unsigned char *inbuf, *outbuf;
	int len, ilen, olen;

	olen = BN_num_bytes(key->n);
	outbuf = xmalloc(olen);

	ilen = BN_num_bytes(in);
	inbuf = xmalloc(ilen);
	BN_bn2bin(in, inbuf);

	if ((len = RSA_private_decrypt(ilen, inbuf, outbuf, key,
	    RSA_PKCS1_PADDING)) <= 0)
		fatal("rsa_private_decrypt() failed");

	BN_bin2bn(outbuf, len, out);

	memset(outbuf, 0, olen);
	memset(inbuf, 0, ilen);
	xfree(outbuf);
	xfree(inbuf);
}

/* Set whether to output verbose messages during key generation. */

void
rsa_set_verbose(int verbose)
{
	rsa_verbose = verbose;
}
