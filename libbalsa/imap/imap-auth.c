/* libimap library.
 * Copyright (C) 2003-2004 Pawel Salek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option) 
 * any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
/* imap_authenticate: Attempt to authenticate using either user-specified
 *   authentication method if specified, or any.
 * returns 0 on success */

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "imap-handle.h"
#include "imap-auth.h"
#include "util.h"
#include "imap_private.h"
#include "siobuf.h"

static ImapResult imap_auth_anonymous(ImapMboxHandle* handle);
static ImapResult imap_auth_plain(ImapMboxHandle* handle);

/* ordered from strongest to weakest. Auth anonymous does not really
 * belong here, does it? */
static ImapAuthenticator imap_authenticators_arr[] = {
  imap_auth_anonymous, /* will be tried only if enabled */
  imap_auth_gssapi,
  imap_auth_cram,
  imap_auth_plain,
  imap_auth_login, /* login is deprecated */
  NULL
};

ImapResult
imap_authenticate(ImapMboxHandle* handle)
{
  ImapAuthenticator* authenticator;
  ImapResult r = IMAP_AUTH_UNAVAIL;

  g_return_val_if_fail(handle, IMAP_AUTH_UNAVAIL);

  if (imap_mbox_is_authenticated(handle) || imap_mbox_is_selected(handle))
    return IMAP_SUCCESS;

  for(authenticator = imap_authenticators_arr;
      *authenticator; authenticator++) {
    if ((r = (*authenticator)(handle)) 
        != IMAP_AUTH_UNAVAIL) {
      if (r == IMAP_SUCCESS)
	imap_mbox_handle_set_state(handle, IMHS_AUTHENTICATED);
      return r;
    }
  }
  imap_mbox_handle_set_msg(handle, "No way to authenticate is known");
  return r;
}

/* =================================================================== */
/*                           AUTHENTICATORS                            */
/* =================================================================== */
#define SHORT_STRING 64
/* imap_auth_login: Plain LOGIN support */
ImapResult
imap_auth_login(ImapMboxHandle* handle)
{
  char q_user[SHORT_STRING], q_pass[SHORT_STRING];
  char buf[2*SHORT_STRING+7];
  char *user = NULL, *pass = NULL;
  ImapResponse rc;
  int ok;
  
  if (imap_mbox_handle_can_do(handle, IMCAP_LOGINDISABLED))
    return IMAP_AUTH_UNAVAIL;
  
  ok = 0;
  if(!ok && handle->user_cb)
    handle->user_cb(IME_GET_USER_PASS, handle->user_arg,
                    "LOGIN", &user, &pass, &ok);
  if(!ok || user == NULL || pass == NULL) {
    imap_mbox_handle_set_msg(handle, "Authentication cancelled");
    return IMAP_AUTH_CANCELLED;
  }

  imap_quote_string(q_user, sizeof (q_user), user);
  imap_quote_string(q_pass, sizeof (q_pass), pass);
  g_free(user); g_free(pass); /* FIXME: clean passwd first */
  g_snprintf (buf, sizeof (buf), "LOGIN %s %s", q_user, q_pass);
  rc = imap_cmd_exec(handle, buf);
  return  (rc== IMR_OK) ?  IMAP_SUCCESS : IMAP_AUTH_FAILURE;
}

/* =================================================================== */
/* SASL PLAIN RFC-2595                                                 */
/* =================================================================== */
#define WAIT_FOR_PROMPT(rc,handle,cmdno) \
    do (rc) = imap_cmd_step((handle), (cmdno)); while((rc) == IMR_UNTAGGED);

static ImapResult
imap_auth_sasl(ImapMboxHandle* handle, ImapCapability cap,
	       const char *sasl_cmd,
	       gboolean (*getmsg)(ImapMboxHandle *h, char **msg, int *msglen))
{
  char *msg = NULL, *msg64;
  ImapResponse rc;
  int msglen, msg64len;
  unsigned cmdno;
  gboolean sasl_ir;
  
  if (!imap_mbox_handle_can_do(handle, cap))
    return IMAP_AUTH_UNAVAIL;
  sasl_ir = imap_mbox_handle_can_do(handle, IMCAP_SASLIR);
  
  if(!getmsg(handle, &msg, &msglen)) {
    imap_mbox_handle_set_msg(handle, "Authentication cancelled");
    return IMAP_AUTH_CANCELLED;
  }
  
  msg64len = 2*(msglen+2);
  msg64 = g_malloc(msg64len);
  lit_conv_to_base64(msg64, msg, msglen, msg64len);
  g_free(msg);

  if(sasl_ir) { /* save one RTT */
    ImapCmdTag tag;
    if(IMAP_MBOX_IS_DISCONNECTED(handle))
      return IMAP_AUTH_UNAVAIL;
    cmdno = imap_make_tag(tag);
    sio_write(handle->sio, tag, strlen(tag));
    sio_write(handle->sio, " ", 1);
    sio_write(handle->sio, sasl_cmd, strlen(sasl_cmd));
    sio_write(handle->sio, " ", 1);
  } else {
    int c;
    if(imap_cmd_start(handle, sasl_cmd, &cmdno) <0) {
      g_free(msg64);
      return IMAP_AUTH_FAILURE;
    }
    imap_handle_flush(handle);
    WAIT_FOR_PROMPT(rc,handle,cmdno);
    
    if (rc != IMR_RESPOND) {
      g_warning("imap %s: unexpected response.\n", sasl_cmd);
      g_free(msg64);
      return IMAP_AUTH_FAILURE;
    }
    while( (c=sio_getc((handle)->sio)) != EOF && c != '\n');
    if(c == EOF) {
      imap_handle_disconnect(handle);
      g_free(msg64);
      return IMAP_AUTH_FAILURE;
    }
  }
  sio_write(handle->sio, msg64, strlen(msg64));
  sio_write(handle->sio, "\r\n", 2);
  g_free(msg64);
  imap_handle_flush(handle);
  do
    rc = imap_cmd_step (handle, cmdno);
  while (rc == IMR_UNTAGGED);
  return  (rc== IMR_OK) ?  IMAP_SUCCESS : IMAP_AUTH_FAILURE;
}

static gboolean
getmsg_plain(ImapMboxHandle *h, char **retmsg, int *retmsglen)
{
  char *user = NULL, *pass = NULL, *msg;
  int ok, userlen, passlen, msglen;
  ok = 0;
  if(!ok && h->user_cb)
    h->user_cb(IME_GET_USER_PASS, h->user_arg,
                    "LOGIN", &user, &pass, &ok);
  if(!ok || user == NULL || pass == NULL)
    return 0;

  userlen = strlen(user);
  passlen = strlen(pass);
  msglen  = 2*userlen + passlen + 2;
  msg     = g_malloc(msglen+1);
  strcpy(msg, user); 
  strcpy(msg+userlen+1, user);
  strcpy(msg+userlen*2+2, pass); 
  g_free(user); g_free(pass);
  *retmsg = msg;
  *retmsglen = msglen;
  return 1;
}

static ImapResult
imap_auth_plain(ImapMboxHandle* handle)
{
  return imap_auth_sasl(handle, IMCAP_APLAIN, "AUTHENTICATE PLAIN",
			getmsg_plain);
}


/* =================================================================== */
/* SASL ANONYMOUS RFC-2245                                             */
/* =================================================================== */
static gboolean
getmsg_anonymous(ImapMboxHandle *h, char **retmsg, int *retmsglen)
{
  int ok = 0;
  if(!ok && h->user_cb)
    h->user_cb(IME_GET_USER, h->user_arg,
	       "ANONYMOUS", retmsg, &ok);
  if(!ok || *retmsg == NULL)
    return 0;
  *retmsglen = strlen(*retmsg);
  return 1;
}

static ImapResult
imap_auth_anonymous(ImapMboxHandle* handle)
{
  if(!handle->enable_anonymous)
    return IMAP_AUTH_UNAVAIL;
  return imap_auth_sasl(handle, IMCAP_AANONYMOUS, "AUTHENTICATE ANONYMOUS",
			getmsg_anonymous);
}

