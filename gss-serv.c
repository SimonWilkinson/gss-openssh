/* $OpenBSD: gss-serv.c,v 1.22 2008/05/08 12:02:23 djm Exp $ */

/*
 * Copyright (c) 2001-2009 Simon Wilkinson. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
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

#ifdef GSSAPI

#include <sys/types.h>
#include <sys/param.h>

#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "openbsd-compat/sys-queue.h"
#include "xmalloc.h"
#include "buffer.h"
#include "key.h"
#include "hostfile.h"
#include "auth.h"
#include "log.h"
#include "channels.h"
#include "session.h"
#include "misc.h"
#include "servconf.h"
#include "uidswap.h"

#include "ssh-gss.h"
#include "monitor_wrap.h"

extern ServerOptions options;

static ssh_gssapi_client gssapi_client =
  { {0, NULL}, GSS_C_EMPTY_BUFFER, GSS_C_EMPTY_BUFFER,
    GSS_C_NO_CREDENTIAL, GSS_C_NO_NAME, GSS_C_NO_NAME, NULL, {NULL, NULL, NULL}, 0, 0};

ssh_gssapi_mech gssapi_null_mech =
    { NULL, NULL, {0, NULL}, NULL, NULL, NULL, NULL, NULL};
/* Generic GSS-API support*/
#ifdef HAVE_GSS_USEROK
static int ssh_gssapi_generic_userok(
				 ssh_gssapi_client *client, char *user) {
  if (gss_userok(client->ctx_name, user)) {
    debug("userok succeded for %s", user);
    return 1;
  } else {
    debug("userok failed for %s", user);
    return 0;
  }
}
#endif

static int
ssh_gssapi_generic_localname(ssh_gssapi_client *client,
			  char **localname) {
  #ifdef HAVE_GSS_LOCALNAME
  gss_buffer_desc lbuffer;
  OM_uint32 major, minor;
  *localname = NULL;
  major = gss_localname(&minor, client->cred_name, NULL, &lbuffer);
  if (GSS_ERROR(major))
    return 0;
  if (lbuffer.value == NULL)
    return 0;
  *localname = xmalloc(lbuffer.length+1);
  if (*localname) {
    memcpy(*localname, lbuffer.value, lbuffer.length);
    *localname[lbuffer.length] = '\0';
  }
  gss_release_buffer(&minor, &lbuffer);
  if (*localname)
    return 1;
  return 0;
  #else
  debug("No generic gss_localname");
  return 0;
  #endif
      }

#ifdef HAVE_GSS_USEROK
static ssh_gssapi_mech ssh_gssapi_generic_mech = {
  NULL, NULL,
  {0, NULL},
  NULL, /* dochild */
  ssh_gssapi_generic_userok,
  ssh_gssapi_generic_localname,
  NULL,
  NULL};
static const ssh_gssapi_mech *ssh_gssapi_generic_mech_ptr = &ssh_gssapi_generic_mech;
#else /*HAVE_GSS_USEROK*/
static const ssh_gssapi_mech ssh_gssapi_generic_mech_ptr = NULL;
#endif

#ifdef KRB5
extern ssh_gssapi_mech gssapi_kerberos_mech;
#endif


ssh_gssapi_mech* supported_mechs[]= {
#ifdef KRB5
	&gssapi_kerberos_mech,
#endif
	&gssapi_null_mech,
};


/*
 * Acquire credentials for a server running on the current host.
 * Requires that the context structure contains a valid OID
 */

/* Returns a GSSAPI error code */
/* Privileged (called from ssh_gssapi_server_ctx) */
static OM_uint32
ssh_gssapi_acquire_cred(Gssctxt *ctx)
{
	OM_uint32 status;
	char lname[MAXHOSTNAMELEN];
	gss_OID_set oidset;

	if (options.gss_strict_acceptor) {
		gss_create_empty_oid_set(&status, &oidset);
		gss_add_oid_set_member(&status, ctx->oid, &oidset);

		if (gethostname(lname, MAXHOSTNAMELEN)) {
			gss_release_oid_set(&status, &oidset);
			return (-1);
		}

		if (GSS_ERROR(ssh_gssapi_import_name(ctx, lname))) {
			gss_release_oid_set(&status, &oidset);
			return (ctx->major);
		}

		if ((ctx->major = gss_acquire_cred(&ctx->minor,
		    ctx->name, 0, oidset, GSS_C_ACCEPT, &ctx->creds, 
		    NULL, NULL)))
			ssh_gssapi_error(ctx);

		gss_release_oid_set(&status, &oidset);
		return (ctx->major);
	} else {
		ctx->name = GSS_C_NO_NAME;
		ctx->creds = GSS_C_NO_CREDENTIAL;
	}
	return GSS_S_COMPLETE;
}

/* Privileged */
OM_uint32
ssh_gssapi_server_ctx(Gssctxt **ctx, gss_OID oid)
{
	if (*ctx)
		ssh_gssapi_delete_ctx(ctx);
	ssh_gssapi_build_ctx(ctx);
	ssh_gssapi_set_oid(*ctx, oid);
	return (ssh_gssapi_acquire_cred(*ctx));
}

/* Unprivileged */
char *
ssh_gssapi_server_mechanisms() {
	gss_OID_set	supported;

	ssh_gssapi_supported_oids(&supported);
	return (ssh_gssapi_kex_mechs(supported, &ssh_gssapi_server_check_mech,
	    NULL, NULL));
}

/* Unprivileged */
int
ssh_gssapi_server_check_mech(Gssctxt **dum, gss_OID oid, const char *data,
    const char *dummy) {
	Gssctxt *ctx = NULL;
	int res;
 
	res = !GSS_ERROR(PRIVSEP(ssh_gssapi_server_ctx(&ctx, oid)));
	ssh_gssapi_delete_ctx(&ctx);

	return (res);
}

/* Unprivileged */
void
ssh_gssapi_supported_oids(gss_OID_set *oidset)
{
	int i = 0;
	OM_uint32 min_status;
	int present;
	gss_OID_set supported;
	/* If we have a generic mechanism all OIDs supported */
	if (ssh_gssapi_generic_mech_ptr) {
	  gss_OID_desc except_oids[3];
	  gss_OID_set_desc except_attrs;
	  except_oids[0] = *GSS_C_MA_MECH_NEGO;
	  except_oids[1] = *GSS_C_MA_NOT_MECH;
	  except_oids[2] = *GSS_C_MA_DEPRECATED;

	  except_attrs.count = sizeof(except_oids)/sizeof(except_oids[0]);
	  except_attrs.elements = except_oids;

	  if (!GSS_ERROR(gss_indicate_mechs_by_attrs(&min_status,
						     GSS_C_NO_OID_SET,
						     &except_attrs,
						     GSS_C_NO_OID_SET,
						     oidset)))
	    return;
	}

	gss_create_empty_oid_set(&min_status, oidset);

	if (GSS_ERROR(gss_indicate_mechs(&min_status, &supported)))
		return;

	while (supported_mechs[i]->name != NULL) {
		if (GSS_ERROR(gss_test_oid_set_member(&min_status,
		    &supported_mechs[i]->oid, supported, &present)))
			present = 0;
		if (present)
			gss_add_oid_set_member(&min_status,
			    &supported_mechs[i]->oid, oidset);
		i++;
	}

	gss_release_oid_set(&min_status, &supported);
}


/* Wrapper around accept_sec_context
 * Requires that the context contains:
 *    oid
 *    credentials	(from ssh_gssapi_acquire_cred)
 */
/* Privileged */
OM_uint32
ssh_gssapi_accept_ctx(Gssctxt *ctx, gss_buffer_desc *recv_tok,
    gss_buffer_desc *send_tok, OM_uint32 *flags)
{
	OM_uint32 status;
	gss_OID mech;

	ctx->major = gss_accept_sec_context(&ctx->minor,
	    &ctx->context, ctx->creds, recv_tok,
	    GSS_C_NO_CHANNEL_BINDINGS, &ctx->client, &mech,
	    send_tok, flags, NULL, &ctx->client_creds);

	if (GSS_ERROR(ctx->major))
		ssh_gssapi_error(ctx);

	if (ctx->client_creds)
		debug("Received some client credentials");
	else
		debug("Got no client credentials");

	status = ctx->major;

	/* Now, if we're complete and we have the right flags, then
	 * we flag the user as also having been authenticated
	 */

	if (((flags == NULL) || ((*flags & GSS_C_MUTUAL_FLAG) &&
	    (*flags & GSS_C_INTEG_FLAG))) && (ctx->major == GSS_S_COMPLETE)) {
		if (ssh_gssapi_getclient(ctx, &gssapi_client))
			fatal("Couldn't convert client name");
	}

	return (status);
}

/*
 * This parses an exported name, extracting the mechanism specific portion
 * to use for ACL checking. It verifies that the name belongs the mechanism
 * originally selected.
 */
static OM_uint32
ssh_gssapi_parse_ename(Gssctxt *ctx, gss_buffer_t ename, gss_buffer_t name)
{
	u_char *tok;
	OM_uint32 offset;
	OM_uint32 oidl;

	tok = ename->value;

	/*
	 * Check that ename is long enough for all of the fixed length
	 * header, and that the initial ID bytes are correct
	 */

	if (ename->length < 6 || memcmp(tok, "\x04\x01", 2) != 0)
		return GSS_S_FAILURE;

	/*
	 * Extract the OID, and check it. Here GSSAPI breaks with tradition
	 * and does use the OID type and length bytes. To confuse things
	 * there are two lengths - the first including these, and the
	 * second without.
	 */

	oidl = get_u16(tok+2); /* length including next two bytes */
	oidl = oidl-2; /* turn it into the _real_ length of the variable OID */

	/*
	 * Check the BER encoding for correct type and length, that the
	 * string is long enough and that the OID matches that in our context
	 */
	if (tok[4] != 0x06 || tok[5] != oidl ||
	    ename->length < oidl+6 ||
	    !ssh_gssapi_check_oid(ctx, tok+6, oidl))
		return GSS_S_FAILURE;

	offset = oidl+6;

	if (ename->length < offset+4)
		return GSS_S_FAILURE;

	name->length = get_u32(tok+offset);
	offset += 4;

	if (ename->length < offset+name->length)
		return GSS_S_FAILURE;

	name->value = xmalloc(name->length+1);
	memcpy(name->value, tok+offset, name->length);
	((char *)name->value)[name->length] = 0;

	return GSS_S_COMPLETE;
}

/* Extract the client details from a given context. This can only reliably
 * be called once for a context */

/* Privileged (called from accept_secure_ctx) */
OM_uint32
ssh_gssapi_getclient(Gssctxt *ctx, ssh_gssapi_client *client)
{
	int i = 0;
	int equal = 0;
	gss_name_t new_name = GSS_C_NO_NAME;
	gss_buffer_desc ename = GSS_C_EMPTY_BUFFER;

	if (options.gss_store_rekey && client->used && ctx->client_creds) {
		if (client->oid.length != ctx->oid->length ||
		    (memcmp(client->oid.elements,
		     ctx->oid->elements, ctx->oid->length) !=0)) {
			debug("Rekeyed credentials have different mechanism");
			return GSS_S_COMPLETE;
		}

		if ((ctx->major = gss_inquire_cred_by_mech(&ctx->minor, 
		    ctx->client_creds, ctx->oid, &new_name, 
		    NULL, NULL, NULL))) {
			ssh_gssapi_error(ctx);
			return (ctx->major);
		}

		ctx->major = gss_compare_name(&ctx->minor, client->cred_name, 
		    new_name, &equal);

		if (GSS_ERROR(ctx->major)) {
			ssh_gssapi_error(ctx);
			return (ctx->major);
		}
 
		if (!equal) {
			debug("Rekeyed credentials have different name");
			return GSS_S_COMPLETE;
		}

		debug("Marking rekeyed credentials for export");

		gss_release_name(&ctx->minor, &client->cred_name);
		gss_release_cred(&ctx->minor, &client->creds);
		client->cred_name = new_name;
		client->creds = ctx->client_creds;
        	ctx->client_creds = GSS_C_NO_CREDENTIAL;
		client->updated = 1;
		return GSS_S_COMPLETE;
	}

	client->mech = NULL;

	while (supported_mechs[i]->name != NULL) {
		if (supported_mechs[i]->oid.length == ctx->oid->length &&
		    (memcmp(supported_mechs[i]->oid.elements,
		    ctx->oid->elements, ctx->oid->length) == 0))
			client->mech = supported_mechs[i];
		i++;
	}

	if (client->oid.elements == NULL)
	  client->oid = *ctx->oid;
	if (client->mech == NULL) {
	  if (ssh_gssapi_generic_mech_ptr)
	    client->mech = (ssh_gssapi_mech *) ssh_gssapi_generic_mech_ptr;
	  else return GSS_S_FAILURE;
	}

	if (ctx->client_creds &&
	    (ctx->major = gss_inquire_cred_by_mech(&ctx->minor,
	     ctx->client_creds, ctx->oid, &client->cred_name, NULL, NULL, NULL))) {
		ssh_gssapi_error(ctx);
		return (ctx->major);
	}

	if ((ctx->major = gss_display_name(&ctx->minor, ctx->client,
	    &client->displayname, NULL))) {
		ssh_gssapi_error(ctx);
		return (ctx->major);
	}

	if ((ctx->major = gss_export_name(&ctx->minor, ctx->client,
	    &ename))) {
		ssh_gssapi_error(ctx);
		return (ctx->major);
	}

	if ((client->mech->oid.elements != NULL) &&
	    (ctx->major = ssh_gssapi_parse_ename(ctx,&ename,
	    &client->exportedname))) {
		return (ctx->major);
	}

	if ((ctx->major = gss_duplicate_name(&ctx->minor, ctx->client,
					     &client->ctx_name)))
	  return ctx->major;
	    
	gss_release_buffer(&ctx->minor, &ename);

	/* We can't copy this structure, so we just move the pointer to it */
	client->creds = ctx->client_creds;
	ctx->client_creds = GSS_C_NO_CREDENTIAL;
	return (ctx->major);
}

/* As user - called on fatal/exit */
void
ssh_gssapi_cleanup_creds(void)
{
	if (gssapi_client.store.filename != NULL) {
		/* Unlink probably isn't sufficient */
		debug("removing gssapi cred file\"%s\"",
		    gssapi_client.store.filename);
		unlink(gssapi_client.store.filename);
	}
}

/* As user */
void
ssh_gssapi_storecreds(void)
{
	if (gssapi_client.mech && gssapi_client.mech->storecreds) {
		(*gssapi_client.mech->storecreds)(&gssapi_client);
	} else
		debug("ssh_gssapi_storecreds: Not a GSSAPI mechanism");
}

/* This allows GSSAPI methods to do things to the childs environment based
 * on the passed authentication process and credentials.
 */
/* As user */
void
ssh_gssapi_do_child(char ***envp, u_int *envsizep)
{

	if (gssapi_client.store.envvar != NULL &&
	    gssapi_client.store.envval != NULL) {
		debug("Setting %s to %s", gssapi_client.store.envvar,
		    gssapi_client.store.envval);
		child_set_env(envp, envsizep, gssapi_client.store.envvar,
		    gssapi_client.store.envval);
	}
}

/* Privileged */
int
ssh_gssapi_userok(char *user, struct passwd *pw)
{
	OM_uint32 lmin;

	if (gssapi_client.mech && gssapi_client.mech->userok)
		if ((*gssapi_client.mech->userok)(&gssapi_client, user)) {
			gssapi_client.used = 1;
			gssapi_client.store.owner = pw;
			return 1;
		} else {
			/* Destroy delegated credentials if userok fails */
			gss_release_buffer(&lmin, &gssapi_client.displayname);
			gss_release_buffer(&lmin, &gssapi_client.exportedname);
			gss_release_cred(&lmin, &gssapi_client.creds);
			gss_release_name(&lmin, &gssapi_client.ctx_name);
			memset(&gssapi_client, 0, sizeof(ssh_gssapi_client));
			return 0;
		}
	else
		debug("ssh_gssapi_userok: Unknown GSSAPI mechanism");
	return (0);
}

/* These bits are only used for rekeying. The unpriviledged child is running 
 * as the user, the monitor is root.
 *
 * In the child, we want to :
 *    *) Ask the monitor to store our credentials into the store we specify
 *    *) If it succeeds, maybe do a PAM update
 */

/* Stuff for PAM */

#ifdef USE_PAM
static int ssh_gssapi_simple_conv(int n, const struct pam_message **msg, 
    struct pam_response **resp, void *data)
{
	return (PAM_CONV_ERR);
}
#endif

void
ssh_gssapi_rekey_creds() {
	int ok;
	int ret;
#ifdef USE_PAM
	pam_handle_t *pamh = NULL;
	struct pam_conv pamconv = {ssh_gssapi_simple_conv, NULL};
	char *envstr;
#endif

	if (gssapi_client.store.filename == NULL && 
	    gssapi_client.store.envval == NULL &&
	    gssapi_client.store.envvar == NULL)
		return;
 
	ok = PRIVSEP(ssh_gssapi_update_creds(&gssapi_client.store));

	if (!ok)
		return;

	debug("Rekeyed credentials stored successfully");

	/* Actually managing to play with the ssh pam stack from here will
	 * be next to impossible. In any case, we may want different options
	 * for rekeying. So, use our own :)
	 */
#ifdef USE_PAM	
	if (!use_privsep) {
		debug("Not even going to try and do PAM with privsep disabled");
		return;
	}

	ret = pam_start("sshd-rekey", gssapi_client.store.owner->pw_name,
 	    &pamconv, &pamh);
	if (ret)
		return;

	xasprintf(&envstr, "%s=%s", gssapi_client.store.envvar, 
	    gssapi_client.store.envval);

	ret = pam_putenv(pamh, envstr);
	if (!ret)
		pam_setcred(pamh, PAM_REINITIALIZE_CRED);
	pam_end(pamh, PAM_SUCCESS);
#endif
}

int 
ssh_gssapi_update_creds(ssh_gssapi_ccache *store) {
	int ok = 0;

	/* Check we've got credentials to store */
	if (!gssapi_client.updated)
		return 0;

	gssapi_client.updated = 0;

	temporarily_use_uid(gssapi_client.store.owner);
	if (gssapi_client.mech && gssapi_client.mech->updatecreds)
		ok = (*gssapi_client.mech->updatecreds)(store, &gssapi_client);
	else
		debug("No update function for this mechanism");

	restore_uid();

	return ok;
}

#endif
