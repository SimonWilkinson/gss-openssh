/*
 * Copyright (c) 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"
RCSID("$OpenBSD: kex.c,v 1.26 2001/04/03 19:53:29 markus Exp $");

#include <openssl/crypto.h>

#include "ssh2.h"
#include "xmalloc.h"
#include "buffer.h"
#include "bufaux.h"
#include "packet.h"
#include "compat.h"
#include "cipher.h"
#include "kex.h"
#include "key.h"
#include "log.h"
#include "mac.h"
#include "match.h"
#include "dispatch.h"

#define KEX_COOKIE_LEN	16

void	kex_kexinit_finish(Kex *kex);
void	kex_choose_conf(Kex *k);

/* put algorithm proposal into buffer */
void
kex_prop2buf(Buffer *b, char *proposal[PROPOSAL_MAX])
{
	u_int32_t rand = 0;
	int i;

	buffer_clear(b);
	for (i = 0; i < KEX_COOKIE_LEN; i++) {
		if (i % 4 == 0)
			rand = arc4random();
		buffer_put_char(b, rand & 0xff);
		rand >>= 8;
	}
	for (i = 0; i < PROPOSAL_MAX; i++)
		buffer_put_cstring(b, proposal[i]);
	buffer_put_char(b, 0);			/* first_kex_packet_follows */
	buffer_put_int(b, 0);			/* uint32 reserved */
}

/* parse buffer and return algorithm proposal */
char **
kex_buf2prop(Buffer *raw)
{
	Buffer b;
	int i;
	char **proposal;

	proposal = xmalloc(PROPOSAL_MAX * sizeof(char *));

	buffer_init(&b);
	buffer_append(&b, buffer_ptr(raw), buffer_len(raw));
	/* skip cookie */
	for (i = 0; i < KEX_COOKIE_LEN; i++)
		buffer_get_char(&b);
	/* extract kex init proposal strings */
	for (i = 0; i < PROPOSAL_MAX; i++) {
		proposal[i] = buffer_get_string(&b,NULL);
		debug2("kex_parse_kexinit: %s", proposal[i]);
	}
	/* first kex follows / reserved */
	i = buffer_get_char(&b);
	debug2("kex_parse_kexinit: first_kex_follows %d ", i);
	i = buffer_get_int(&b);
	debug2("kex_parse_kexinit: reserved %d ", i);
	buffer_free(&b);
	return proposal;
}

void
kex_prop_free(char **proposal)
{
	int i;

	for (i = 0; i < PROPOSAL_MAX; i++)
		xfree(proposal[i]);
	xfree(proposal);
}

void
kex_protocol_error(int type, int plen, void *ctxt)
{
        error("Hm, kex protocol error: type %d plen %d", type, plen);
}

void
kex_send_newkeys(void)
{
	packet_start(SSH2_MSG_NEWKEYS);
	packet_send();
	/* packet_write_wait(); */
	debug("SSH2_MSG_NEWKEYS sent");
}

void
kex_input_newkeys(int type, int plen, void *ctxt)
{
	Kex *kex = ctxt;
	int i;

	debug("SSH2_MSG_NEWKEYS received");
	kex->newkeys = 1;
	for (i = 30; i <= 49; i++)
		dispatch_set(i, &kex_protocol_error);
	buffer_clear(&kex->peer);
	buffer_clear(&kex->my);
	kex->flags &= ~KEX_INIT_SENT;
}

void
kex_send_kexinit(Kex *kex)
{
	packet_start(SSH2_MSG_KEXINIT);
	packet_put_raw(buffer_ptr(&kex->my), buffer_len(&kex->my));
	packet_send();
	debug("SSH2_MSG_KEXINIT sent");
	kex->flags |= KEX_INIT_SENT;
}

void
kex_input_kexinit(int type, int plen, void *ctxt)
{
	char *ptr;
	int dlen;
	Kex *kex = (Kex *)ctxt;

	dispatch_set(SSH2_MSG_KEXINIT, &kex_protocol_error);
	debug("SSH2_MSG_KEXINIT received");

	ptr = packet_get_raw(&dlen);
	buffer_append(&kex->peer, ptr, dlen);

	kex_kexinit_finish(kex);
}

Kex *
kex_start(char *proposal[PROPOSAL_MAX])
{
	Kex *kex;
	int i;

	kex = xmalloc(sizeof(*kex));
	memset(kex, 0, sizeof(*kex));
	buffer_init(&kex->peer);
	buffer_init(&kex->my);
	kex_prop2buf(&kex->my, proposal);
	kex->newkeys = 0;

	kex_send_kexinit(kex);					/* we start */
	/* Numbers 30-49 are used for kex packets */
	for (i = 30; i <= 49; i++)
		dispatch_set(i, kex_protocol_error);

	dispatch_set(SSH2_MSG_KEXINIT, &kex_input_kexinit);
	dispatch_set(SSH2_MSG_NEWKEYS, &kex_input_newkeys);
	return kex;
}

void
kex_kexinit_finish(Kex *kex)
{
	if (!(kex->flags & KEX_INIT_SENT))
		kex_send_kexinit(kex);

	kex_choose_conf(kex);

	switch(kex->kex_type) {
	case DH_GRP1_SHA1:
		kexdh(kex);
		break;
	case DH_GEX_SHA1:
		kexgex(kex);
		break;
	default:
		fatal("Unsupported key exchange %d", kex->kex_type);
	}
}

void
choose_enc(Enc *enc, char *client, char *server)
{
	char *name = match_list(client, server, NULL);
	if (name == NULL)
		fatal("no matching cipher found: client %s server %s", client, server);
	enc->cipher = cipher_by_name(name);
	if (enc->cipher == NULL)
		fatal("matching cipher is not supported: %s", name);
	enc->name = name;
	enc->enabled = 0;
	enc->iv = NULL;
	enc->key = NULL;
}
void
choose_mac(Mac *mac, char *client, char *server)
{
	char *name = match_list(client, server, NULL);
	if (name == NULL)
		fatal("no matching mac found: client %s server %s", client, server);
	if (mac_init(mac, name) < 0)
		fatal("unsupported mac %s", name);
	/* truncate the key */
	if (datafellows & SSH_BUG_HMAC)
		mac->key_len = 16;
	mac->name = name;
	mac->key = NULL;
	mac->enabled = 0;
}
void
choose_comp(Comp *comp, char *client, char *server)
{
	char *name = match_list(client, server, NULL);
	if (name == NULL)
		fatal("no matching comp found: client %s server %s", client, server);
	if (strcmp(name, "zlib") == 0) {
		comp->type = 1;
	} else if (strcmp(name, "none") == 0) {
		comp->type = 0;
	} else {
		fatal("unsupported comp %s", name);
	}
	comp->name = name;
}
void
choose_kex(Kex *k, char *client, char *server)
{
	k->name = match_list(client, server, NULL);
	if (k->name == NULL)
		fatal("no kex alg");
	if (strcmp(k->name, KEX_DH1) == 0) {
		k->kex_type = DH_GRP1_SHA1;
	} else if (strcmp(k->name, KEX_DHGEX) == 0) {
		k->kex_type = DH_GEX_SHA1;
	} else
		fatal("bad kex alg %s", k->name);
}
void
choose_hostkeyalg(Kex *k, char *client, char *server)
{
	char *hostkeyalg = match_list(client, server, NULL);
	if (hostkeyalg == NULL)
		fatal("no hostkey alg");
	k->hostkey_type = key_type_from_name(hostkeyalg);
	if (k->hostkey_type == KEY_UNSPEC)
		fatal("bad hostkey alg '%s'", hostkeyalg);
	xfree(hostkeyalg);
}

void
kex_choose_conf(Kex *k)
{
	char **my, **peer;
	char **cprop, **sprop;
	int mode;
	int ctos;				/* direction: if true client-to-server */
	int need;

	my   = kex_buf2prop(&k->my);
	peer = kex_buf2prop(&k->peer);

	if (k->server) {
		cprop=peer;
		sprop=my;
	} else {
		cprop=my;
		sprop=peer;
	}

	for (mode = 0; mode < MODE_MAX; mode++) {
		int nenc, nmac, ncomp;
		ctos = (!k->server && mode == MODE_OUT) || (k->server && mode == MODE_IN);
		nenc  = ctos ? PROPOSAL_ENC_ALGS_CTOS  : PROPOSAL_ENC_ALGS_STOC;
		nmac  = ctos ? PROPOSAL_MAC_ALGS_CTOS  : PROPOSAL_MAC_ALGS_STOC;
		ncomp = ctos ? PROPOSAL_COMP_ALGS_CTOS : PROPOSAL_COMP_ALGS_STOC;
		choose_enc (&k->enc [mode], cprop[nenc],  sprop[nenc]);
		choose_mac (&k->mac [mode], cprop[nmac],  sprop[nmac]);
		choose_comp(&k->comp[mode], cprop[ncomp], sprop[ncomp]);
		debug("kex: %s %s %s %s",
		    ctos ? "client->server" : "server->client",
		    k->enc[mode].name,
		    k->mac[mode].name,
		    k->comp[mode].name);
	}
	choose_kex(k, cprop[PROPOSAL_KEX_ALGS], sprop[PROPOSAL_KEX_ALGS]);
	choose_hostkeyalg(k, cprop[PROPOSAL_SERVER_HOST_KEY_ALGS],
	    sprop[PROPOSAL_SERVER_HOST_KEY_ALGS]);
	need = 0;
	for (mode = 0; mode < MODE_MAX; mode++) {
	    if (need < k->enc[mode].cipher->key_len)
		    need = k->enc[mode].cipher->key_len;
	    if (need < k->enc[mode].cipher->block_size)
		    need = k->enc[mode].cipher->block_size;
	    if (need < k->mac[mode].key_len)
		    need = k->mac[mode].key_len;
	}
	/* XXX need runden? */
	k->we_need = need;

	kex_prop_free(my);
	kex_prop_free(peer);

}

u_char *
derive_key(int id, int need, u_char *hash, BIGNUM *shared_secret)
{
	Buffer b;
	EVP_MD *evp_md = EVP_sha1();
	EVP_MD_CTX md;
	char c = id;
	int have;
	int mdsz = evp_md->md_size;
	u_char *digest = xmalloc(((need+mdsz-1)/mdsz)*mdsz);

	buffer_init(&b);
	buffer_put_bignum2(&b, shared_secret);

	EVP_DigestInit(&md, evp_md);
	EVP_DigestUpdate(&md, buffer_ptr(&b), buffer_len(&b));	/* shared_secret K */
	EVP_DigestUpdate(&md, hash, mdsz);		/* transport-06 */
	EVP_DigestUpdate(&md, &c, 1);			/* key id */
	EVP_DigestUpdate(&md, hash, mdsz);		/* session id */
	EVP_DigestFinal(&md, digest, NULL);

	/* expand */
	for (have = mdsz; need > have; have += mdsz) {
		EVP_DigestInit(&md, evp_md);
		EVP_DigestUpdate(&md, buffer_ptr(&b), buffer_len(&b));
		EVP_DigestUpdate(&md, hash, mdsz);
		EVP_DigestUpdate(&md, digest, have);
		EVP_DigestFinal(&md, digest + have, NULL);
	}
	buffer_free(&b);
#ifdef DEBUG_KEX
	fprintf(stderr, "key '%c'== ", c);
	dump_digest("key", digest, need);
#endif
	return digest;
}

#define NKEYS	6
void
kex_derive_keys(Kex *k, u_char *hash, BIGNUM *shared_secret)
{
	int i;
	int mode;
	int ctos;
	u_char *keys[NKEYS];

	for (i = 0; i < NKEYS; i++)
		keys[i] = derive_key('A'+i, k->we_need, hash, shared_secret);

	for (mode = 0; mode < MODE_MAX; mode++) {
		ctos = (!k->server && mode == MODE_OUT) || (k->server && mode == MODE_IN);
		k->enc[mode].iv  = keys[ctos ? 0 : 1];
		k->enc[mode].key = keys[ctos ? 2 : 3];
		k->mac[mode].key = keys[ctos ? 4 : 5];
	}
}

#if defined(DEBUG_KEX) || defined(DEBUG_KEXDH)
void
dump_digest(char *msg, u_char *digest, int len)
{
	int i;

	fprintf(stderr, "%s\n", msg);
	for (i = 0; i< len; i++){
		fprintf(stderr, "%02x", digest[i]);
		if (i%32 == 31)
			fprintf(stderr, "\n");
		else if (i%8 == 7)
			fprintf(stderr, " ");
	}
	fprintf(stderr, "\n");
}
#endif
