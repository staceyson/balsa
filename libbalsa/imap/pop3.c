/* libimap library.
 * Copyright (C) 2004 Pawel Salek.
 * APop authentication routines are copyright (C) 2004 Balsa team,
 *                                 See the file AUTHORS for a list. 
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  
 * 02111-1307, USA.
 */

/* The POP protocol is defined in RFC1939. The extensions are in
   RFC2449. There is also SASL RFC but we do not implement that yet.
 */
#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(USE_TLS)
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include "pop3.h"
#include "siobuf.h"
#include "imap_private.h"
#include "md5-utils.h"

#define ELEMENTS(x) (sizeof (x) / sizeof(x[0]))

enum {
  POP_CAP_USER = 0,    /* RFC 1939 */
  POP_CAP_TOP,         /* RFC 1939 */
  POP_CAP_LOGIN_DELAY, /* RFC 2449 */
  POP_CAP_UIDL,        /* RFC 1939 */
  POP_CAP_PIPELINING,  /* RFC 2449 */
  POP_CAP_STLS,        /* RFC 2449 */
  /* AUTH and SASL are another kettle of fish and go at the end. */
  POP_CAP_AUTH_CRAM,   /* RFC 2195 */
  POP_CAP_SASL_ANON,   /* RFC 2245 */
  POP_CAP_SASL_PLAIN,  /* RFC 2595 +  draft-ietf-sasl-plain*/
  POP_CAP_SASL_LOGIN,  /* draft-murchison-sasl-login */
  POP_CAP_SASL_OTP,    /* RFC 2444 */
  POP_CAP_SASL_CRAM,   /* RFC 2195 */
  POP_CAP_SASL_DIGEST, /* RFC 2831 */
  POP_CAP_LAST
};

typedef enum {
  POP_REQ_TYPE_RETR,
  POP_REQ_TYPE_DELE
} PopReqType;

/* Max pop line length */
#define POP_LINE_LEN 513

/* arbitrary queue length to be used with pipelining. */
#define POP_QUEUE_LEN 123

static const char* capa_names[] = {
  "USER", "TOP", "LOGIN-DELAY", "UIDL", "PIPELINING", "STLS"
};

struct PopHandle_ {
  int sd;  /* socket descriptor */
  int timeout; /* timeout in milliseconds */
  char capabilities[POP_CAP_LAST];
  struct siobuf *sio;
  gchar *host;
  ImapConnectionState state;

  unsigned      msg_cnt;
  unsigned long total_size;
  GPtrArray *uids;

  ImapMonitorCb monitor_cb;
  void*         monitor_arg;
  ImapUserCb    user_cb;
  void*         user_arg;
  /* request queue */
  struct PopRequest {
    PopAsyncCb     cb;
    GDestroyNotify notify;
    void          *arg;
    PopReqType     type;
  } requests[POP_QUEUE_LEN];
  unsigned max_req_queue_len;
  unsigned req_insert_pos;
  /* various options */
  unsigned disable_apop:1;
  unsigned filter_cr:1;
  unsigned over_ssl:1;
  unsigned enable_pipe:1;
  unsigned completing_requests:1; /* internal flag of the queuing code */
};
#define pop_can_do(pop, cap) ((pop)->capabilities[cap])

PopHandle *
pop_new(void)
{
  PopHandle *pop = g_new0(PopHandle, 1);
  pop->timeout = -1;
  return pop;
}

void
pop_set_option(PopHandle *pop, PopOption opt, gboolean state)
{
  switch(opt) {
  case IMAP_POP_OPT_DISABLE_APOP: pop->disable_apop = state; break;
  case IMAP_POP_OPT_FILTER_CR   : pop->filter_cr    = state; break;
  case IMAP_POP_OPT_OVER_SSL    : pop->over_ssl     = state; break;
  case IMAP_POP_OPT_PIPELINE    : pop->enable_pipe  = state; break;
  }
}

void
pop_set_timeout(PopHandle *pop, int milliseconds)
{
  pop->timeout = milliseconds;
  if(pop->sio)
    sio_set_timeout(pop->sio, milliseconds);
}

void
pop_set_monitorcb(PopHandle *pop, PopMonitorCb cb, void *arg)
{
  pop->monitor_cb  = cb;
  pop->monitor_arg = arg;
}

void
pop_set_usercb(PopHandle *pop, ImapUserCb user_cb, void *arg_cb)
{
  pop->user_cb  = user_cb;
  pop->user_arg = arg_cb;
}

/* parses single-line; we could have used a buffer just as well. */
static gboolean
pop_check_status(PopHandle *pop, GError **err)
{
  char buf[POP_LINE_LEN];
  gboolean res;

  if(!sio_gets(pop->sio, buf, sizeof(buf))) {
    pop->state = IMHS_DISCONNECTED;
    sio_detach(pop->sio); pop->sio = NULL; close(pop->sd);
    g_set_error(err, IMAP_ERROR, IMAP_POP_SEVERED_ERROR,
                "Connection severed");
    return FALSE;
  }
     
  if(strncmp(buf, "+OK", 3) == 0)
    res = TRUE;
  else if(strncmp(buf, "-ERR", 4) == 0)
    res = FALSE;
  else
    res = FALSE;

  if(!res) {
    buf[POP_LINE_LEN-1] = '\0';
    g_set_error(err, IMAP_ERROR, IMAP_POP_PROTOCOL_ERROR, buf);
  }

  return res;
}

static gboolean
pop_exec(PopHandle *pop, const char *cmd, GError **err)
{
  sio_write(pop->sio, cmd, strlen(cmd));
  sio_flush(pop->sio);
  return pop_check_status(pop, err);
}

/** pop_get_capa tries to get capabilities */
static gboolean
pop_get_capa(PopHandle *pop, GError **err)
{
  char buf[POP_LINE_LEN];
  char *line;

  memset(pop->capabilities, '\0', sizeof(pop->capabilities));
  if(!pop_exec(pop, "CAPA\r\n", err)) {
    pop->capabilities[POP_CAP_USER] = 1;
    return FALSE;
  }

  while( (line=sio_gets(pop->sio, buf, sizeof(buf))) &&
        strcmp(buf, ".\r\n")) {
    unsigned i;
    for(i=0; buf[i]; i++) buf[i] = toupper(buf[i]);
    if(strncmp(buf, "AUTH", 4) == 0) {
      /* FIXME: implement proper AUTH support */
      if(strstr(buf, " CRAM-MD5"))
        pop->capabilities[POP_CAP_AUTH_CRAM] = 1;
    } else if(strncmp(buf, "SASL", 4) == 0) {
      /* FIXME: implement SASL support */
    } else {
      unsigned i;
      for(i=0; i<ELEMENTS(capa_names); i++) {
        unsigned len = strlen(capa_names[i]);
        if(strncmp(buf, capa_names[i], len) == 0)
          pop->capabilities[i] = 1;
      }
    }
  }
  if(!line) {
    pop->state = IMHS_DISCONNECTED;
    sio_detach(pop->sio); pop->sio = NULL; close(pop->sd);
    g_set_error(err, IMAP_ERROR, IMAP_POP_SEVERED_ERROR,
                "Connection severed");
    return FALSE;
  }
  return TRUE;
}
/* ===================================================================
   AUTHENTICATION SECTION
   ===================================================================
*/

static ImapResult
pop_auth_cram(PopHandle *pop, const char *greeting, GError **err)
{
  return IMAP_AUTH_UNAVAIL;
}

/* getApopStamp:
   Get the Server Timestamp for APOP authentication -kabir 
   return TRUE on success.
*/

static gboolean
get_apop_stamp(const char *greeting, char *stamp)
{
  char *start, *finish;

  start = strchr(greeting, '<');
  if (start) {
    finish = strchr(start, '>');
    if (finish) {
      strncpy(stamp, start, finish - start + 1);
      return TRUE;
    }
  } 
  return FALSE;
}

/* Compute the authentication hash to send to the server - kabir */

static void
compute_auth_hash(char *stamp, char *hash, const char *passwd)
{
  MD5Context mdContext;
  register unsigned char *dp;
  register char *cp;
  unsigned char *ep;
  unsigned char digest[16];
  
  md5_init(&mdContext);
  md5_update(&mdContext, (unsigned char *)stamp, strlen(stamp));
  md5_update(&mdContext, (unsigned char *)passwd, strlen(passwd));
  md5_final(&mdContext, digest);
  
  cp = hash;
  dp = digest;
  for(ep = dp + sizeof(digest)/sizeof(digest[0]); dp < ep; cp += 2) {
    sprintf(cp, "%02x", *dp);
    dp++;
  }
    
  *cp = '\0';
}

static ImapResult
pop_auth_apop(PopHandle *pop, const char *greeting, GError **err)
{
  char stamp[POP_LINE_LEN], auth_hash[POP_LINE_LEN];
  char *user = NULL, *pass = NULL;
  int ok = 0;

  memset(stamp, '\0', sizeof(stamp));

  if( !get_apop_stamp(greeting, stamp) ) return IMAP_AUTH_UNAVAIL;

  if(pop->user_cb)
    pop->user_cb(IME_GET_USER_PASS, pop->user_arg,
                    "APOP", &user, &pass, &ok);
  if(!ok || user == NULL || pass == NULL) {
      g_set_error(err, IMAP_ERROR, IMAP_POP_AUTH_ERROR,
                  "APOP Authentication cancelled");
    return IMAP_AUTH_FAILURE;
  }

  compute_auth_hash(stamp, auth_hash, pass);
  g_snprintf(stamp, sizeof(stamp), "APop %s %s\r\n", user, auth_hash);
  g_free(user); g_free(pass);

  return pop_exec(pop, stamp, err) ?  IMAP_SUCCESS : IMAP_AUTH_FAILURE;
}

static ImapResult
pop_auth_user(PopHandle *pop, const char *greeting, GError **err)
{
  char *user = NULL, *pass = NULL;
  int ok = 0;
  char line[POP_LINE_LEN];

  if(!pop_can_do(pop, POP_CAP_USER))
    return IMAP_AUTH_UNAVAIL;

  if(pop->user_cb)
    pop->user_cb(IME_GET_USER_PASS, pop->user_arg,
                    "LOGIN", &user, &pass, &ok);
    if(!ok || user == NULL || pass == NULL) {
      g_set_error(err, IMAP_ERROR, IMAP_POP_AUTH_ERROR,
                  "USER Authentication cancelled");
      return IMAP_AUTH_FAILURE;
    }

  g_snprintf(line, sizeof(line), "User %s\r\n", user);
  g_free(user); 
  if(!pop_exec(pop, line, err)) { /* RFC 1939: User is optional */
    g_free(pass);
    g_clear_error(err);
    return IMAP_AUTH_UNAVAIL;
  }
  g_snprintf (line, sizeof(line), "Pass %s\r\n", pass);
  g_free(pass);
  return pop_exec(pop, line, err) ? IMAP_SUCCESS : IMAP_AUTH_FAILURE;
}

typedef ImapResult (*PopAuthenticator)(PopHandle*, const char*, GError **err);
/* ordered from strongest to weakest */
static const PopAuthenticator pop_authenticators_arr[] = {
  pop_auth_cram,
  pop_auth_apop,
  pop_auth_user,
  NULL
};


static gboolean
pop_authenticate(PopHandle *pop, const char *greeting, GError **err)
{
  const PopAuthenticator *authenticator;
  ImapResult r;

  if(g_strncasecmp(greeting, "+OK", 3) != 0) {
    g_set_error(err, IMAP_ERROR, IMAP_POP_AUTH_ERROR,
                "Server does not welcome us: %s", greeting);
    return FALSE;
  }

  for(authenticator = pop_authenticators_arr;
      *authenticator; authenticator++) {
    if ((r = (*authenticator)(pop, greeting, err)) 
        != IMAP_AUTH_UNAVAIL) {
      return r == IMAP_SUCCESS;
    }
  }
  g_set_error(err, IMAP_ERROR, IMAP_POP_AUTH_ERROR,
              "No authentication method available");
              
  return FALSE;
}

/* ===================================================================
   END OF AUTHENTICATION SECTION
   ===================================================================
*/

#ifdef USE_TLS
static gboolean
pop_stls(PopHandle *pop, GError **err)
{
  SSL *ssl = imap_create_ssl();

  if(!ssl)
    return FALSE;

  if(!pop_exec(pop, "STLS\r\n", err)) {
    SSL_free(ssl);
    return FALSE;
  }
  if(imap_setup_ssl(pop->sio, pop->host, ssl,
                    pop->user_cb, pop->user_arg)) {
    return TRUE;
  } else {
    SSL_free(ssl);
    sio_detach(pop->sio); pop->sio = NULL; close(pop->sd);
    pop->state = IMHS_DISCONNECTED;
    return FALSE;
  }
}
#endif


/** pop_connect connects and authenticates using usercb */
gboolean
pop_connect(PopHandle *pop, const char *host, GError **err)
{
  static const int SIO_BUFSZ=8192;
  static const char *service = "pop3";
  char line[POP_LINE_LEN];

#ifdef USE_TLS
  if(pop->over_ssl) service = "pop3s";
#endif

  g_free(pop->host);
  pop->host = g_strdup(host);

  pop->sd = imap_socket_open(pop->host, service);
  if(pop->sd<0) {
    g_set_error(err, IMAP_ERROR, IMAP_POP_CONNECT_ERROR,
                "Could not connect to %s", host); /* FIXME: translate */
    return FALSE;
  }
  /* Add buffering to the socket */
  pop->sio = sio_attach(pop->sd, pop->sd, SIO_BUFSZ);
  if (pop->sio == NULL) {
    close(pop->sd);
    g_set_error(err, IMAP_ERROR, IMAP_POP_CONNECT_ERROR,
                "Could not connect to %s", host); /* FIXME: translate */
    return FALSE;
  }
  if(pop->timeout>0)
    sio_set_timeout(pop->sio, pop->timeout);
#ifdef USE_TLS
  if(pop->over_ssl) {
    SSL *ssl = imap_create_ssl();
    if(!ssl || !imap_setup_ssl(pop->sio, pop->host, ssl,
                               pop->user_cb, pop->user_arg)) {
      sio_detach(pop->sio); pop->sio = NULL; close(pop->sd);
      pop->state = IMHS_DISCONNECTED;
      g_set_error(err, IMAP_ERROR, IMAP_POP_CONNECT_ERROR,
                  "Could not set up SSL");
      return IMAP_UNSECURE;
    }
  }
#endif
  if(pop->monitor_cb) 
    sio_set_monitorcb(pop->sio, pop->monitor_cb, pop->monitor_arg);

  if(!sio_gets(pop->sio, line, sizeof(line))) { /* get initial greeting */
      g_set_error(err, IMAP_ERROR, IMAP_POP_CONNECT_ERROR,
                  "Did not get initial greeting.");
      return FALSE;
  }
  if(!pop_get_capa(pop, err)) { /* ignore -ERR */
    if(g_error_matches(*err, IMAP_ERROR, IMAP_POP_PROTOCOL_ERROR))
      g_clear_error(err);
    else return FALSE;
  }
  
#ifdef USE_TLS
  if(pop_can_do(pop, POP_CAP_STLS)) {
    if(!pop_stls(pop, err))
      return FALSE;
  }
#endif
  
  if(!pop_authenticate(pop, line, err)) {
    if(pop->state != IMHS_DISCONNECTED) { /* we might have been already off */
      sio_detach(pop->sio); pop->sio = NULL; close(pop->sd);
      pop->state = IMHS_DISCONNECTED;
    }
    return FALSE;
  }
  pop->state = IMHS_AUTHENTICATED;
  /* get basic information about the mailbox */
  sio_write(pop->sio, "Stat\r\n", 6);
  sio_flush(pop->sio);
  if(sio_gets(pop->sio, line, sizeof(line)) && strncmp(line, "+OK", 3) == 0) {
    if( sscanf(line + 3, " %u %lu", &pop->msg_cnt, &pop->total_size) < 2 ) {
      g_set_error(err, IMAP_ERROR, IMAP_POP_PROTOCOL_ERROR,
                  "Server %s did not return message count: %s",
                  pop->host, line);
      return FALSE;
    }
  } else {
    g_set_error(err, IMAP_ERROR, IMAP_POP_PROTOCOL_ERROR,
                "Server %s did not respond to Stat command.",
                pop->host);
    return FALSE;
  }
  pop->max_req_queue_len = 
    pop_can_do(pop, POP_CAP_PIPELINING) /* && pop->enable_pipe */
    ? POP_QUEUE_LEN/2 : 1;
  printf("batching requests in groups of %u.\n",
         pop->max_req_queue_len);
  return TRUE;
}

unsigned
pop_get_exists(PopHandle *pop, GError **err)
{
  return pop->msg_cnt;
}

unsigned long
pop_get_total_size(PopHandle *pop)
{
  return pop->total_size;
}

const char*
pop_get_uid(PopHandle *pop, unsigned msgno, GError **err)
{
  /* FIXME: should we check capabilities here? */
  if(!pop->uids) {
    char line[POP_LINE_LEN];
    unsigned curr_msgno = 1;
    if(!pop_exec(pop, "UIDL\r\n", err))
      return NULL;
    pop->uids = g_ptr_array_sized_new(pop->msg_cnt);
    while( sio_gets(pop->sio, line, sizeof(line)) &&
           strcmp(line, ".\r\n") ) {
      char* cr, *space = strchr(line, ' ');
      unsigned read_msgno = atoi(line);
      if(!space ||read_msgno != curr_msgno) /* Parsing error? */
        continue;

      if( (cr = strrchr(space, '\r')) || (cr = strrchr(space, '\n')))
        *cr = '\0';
      g_ptr_array_add(pop->uids, g_strdup(space+1));
      curr_msgno++;
    }
  }
  return g_ptr_array_index(pop->uids, msgno-1);
}

gboolean
pop_fetch_message_s(PopHandle *pop, unsigned msgno,
                    int (*cb)(unsigned len, char*buf, void *arg),
                    void *cb_arg, GError **err)
{
  gboolean resp;
  char line[POP_LINE_LEN];
  sio_printf(pop->sio, "RETR %u\r\n", msgno);
  sio_flush(pop->sio);
  resp = pop_check_status(pop, err);
  /* do here the fetch */
  if(resp) {
    while( sio_gets(pop->sio, line, sizeof(line)) &&
           strcmp(line, ".\r\n") ) {
      char *arg = line[0] == '.' ? line+1 : line;
      unsigned len = strlen(arg);
      if(pop->filter_cr && len>=2 && arg[len-2] == '\r') 
        arg[(--len)-1] = '\n';
      if(resp) 
        if(!cb(len, arg, cb_arg)) {
          g_set_error(err, IMAP_ERROR, IMAP_POP_SAVE_ERROR,
                      "Saving message failed.");
          resp = FALSE;
        }
    }
  }
  return resp;
}
gboolean
pop_delete_message_s(PopHandle *pop, unsigned msgno, GError **err)
{
  sio_printf(pop->sio, "DELE %u\r\n", msgno);
  sio_flush(pop->sio);
  return pop_check_status(pop, err);
}

/* be extremely careful here: we might have a severed connection by now.. */
gboolean
pop_destroy(PopHandle *pop, GError **err)
{
  gboolean res = TRUE;

  while(pop->req_insert_pos && pop->state == IMHS_AUTHENTICATED)
    pop_complete_pending_requests(pop);

  if(pop->sio) {
    res = pop_exec(pop, "Quit\r\n", err);
    sio_detach(pop->sio); pop->sio = NULL;
    close(pop->sd);
  }
  if(pop->uids) {
    while(pop->uids->len) {
      char *s = g_ptr_array_index(pop->uids, pop->uids->len-1);
      g_ptr_array_remove_index(pop->uids, pop->uids->len-1);
      g_free(s);
    }
    g_ptr_array_free(pop->uids, TRUE);
  }
  g_free(pop->host);
  g_free(pop);
  return res;
}

/* The asynchronous interface is implemented to handle command
 * pipelining. The general usage structure is that the client files
 * fetch or delete requests. The request queue is flushed when filled,
 * when explicitely requested or on the close of the connection.  The
 * callback format includes means to provide asynchronously
 * information whether the command was executed successfully or not.
 *
 * We always send the request and poll whether we can read anything.
*/

static void
add_to_queue(PopHandle *pop, PopReqType rt, PopAsyncCb cb, 
             GDestroyNotify notify, void *arg)
{
  g_return_if_fail(pop->req_insert_pos<POP_QUEUE_LEN);
  pop->requests[pop->req_insert_pos].cb     = cb;
  pop->requests[pop->req_insert_pos].notify = notify;
  pop->requests[pop->req_insert_pos].arg    = arg;
  pop->requests[pop->req_insert_pos].type   = rt;
  if(++pop->req_insert_pos == pop->max_req_queue_len)  
    pop_complete_pending_requests(pop);
}

/* FIXME: handle severed connections! */
static void
pop_complete_retr(PopHandle *pop, PopAsyncCb cb, void *arg)
{
  char line[POP_LINE_LEN];
  GError *err = NULL;
  gboolean resp;
  PopReqCode rc;

  if(pop->state != IMHS_AUTHENTICATED) return;
  resp = pop_check_status(pop, &err);
  rc = resp ? POP_REQ_OK : POP_REQ_ERR;
  if(cb)
    cb(rc, arg, &err);
  if(resp) { /* same code as in fetch_message() */
    while( sio_gets(pop->sio, line, sizeof(line)) &&
           strcmp(line, ".\r\n") ) {
      char *buf = line[0] == '.' ? line+1 : line;
      unsigned len = strlen(buf);
      if(pop->filter_cr && len>=2 && buf[len-2] == '\r') 
        buf[(--len)-1] = '\n';
      if(cb) 
        cb(POP_REQ_DATA, arg, len, buf);
    }
    if(cb)
      cb(POP_REQ_DONE, arg, err);
  }

  g_clear_error(&err);
}

static void
pop_complete_dele(PopHandle *pop, PopAsyncCb cb, void *arg)
{
  GError *err = NULL;
  PopReqCode res;

  if(pop->state != IMHS_AUTHENTICATED) return;
  res = pop_check_status(pop, &err) ? POP_REQ_OK : POP_REQ_ERR;
  if(cb)
    cb(res, arg, err); /* FIXME: it cannot be taken care of by callback! */
  g_clear_error(&err); /* in case it was not taken care of by callback */
}

void
pop_fetch_message(PopHandle *pop, unsigned msgno, 
                  PopAsyncCb cb, void *cb_arg, GDestroyNotify notify)
{
  g_return_if_fail(pop);
  if(!pop->sio || pop->state != IMHS_AUTHENTICATED)
    return; /* server has disconnected */
  sio_printf(pop->sio, "RETR %u\r\n", msgno);
  add_to_queue(pop, POP_REQ_TYPE_RETR, cb, notify, cb_arg);
}
void
pop_delete_message(PopHandle *pop, unsigned msgno, 
                   PopAsyncCb cb, void *cb_arg, GDestroyNotify notify)
{
  g_return_if_fail(pop);
  if(!pop->sio || pop->state != IMHS_AUTHENTICATED)
    return; /* server has disconnected */
  sio_printf(pop->sio, "DELE %u\r\n", msgno);
  add_to_queue(pop, POP_REQ_TYPE_DELE, cb, notify, cb_arg);
}

/* pop_complete_pending_requests:
   remember current mark so that we do not wait for completions of requests
   filed from completion handlers that are obviously not flushed.
   We must be careful not to trigger recursive completion from the
   completion callbacks.
*/
void
pop_complete_pending_requests(PopHandle *pop)
{
  unsigned i, current_mark;
  if(pop->completing_requests || pop->state != IMHS_AUTHENTICATED)
    return;
  pop->completing_requests = 1;
  do {
    current_mark = pop->req_insert_pos;
    sio_flush(pop->sio);
    for(i=0; i<current_mark; i++) {
      switch(pop->requests[i].type) {
      case POP_REQ_TYPE_RETR:
        pop_complete_retr(pop, pop->requests[i].cb, pop->requests[i].arg);
        break;
      case POP_REQ_TYPE_DELE:
        pop_complete_dele(pop, pop->requests[i].cb, pop->requests[i].arg);
        break;
      default: g_assert_not_reached();
      }
      if(pop->requests[i].notify)
        pop->requests[i].notify(pop->requests[i].arg);
    }
      
    memmove(&pop->requests[0], &pop->requests[current_mark],
            (pop->req_insert_pos-current_mark)*sizeof(struct PopRequest));
    pop->req_insert_pos -= current_mark;
  } while(pop->req_insert_pos>=pop->max_req_queue_len &&
          pop->state == IMHS_AUTHENTICATED);
  pop->completing_requests = 0;
}