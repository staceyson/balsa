/* -*-mode:c; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 *
 * Copyright (C) 1997-2002 Stuart Parmenter and others,
 *                         See the file AUTHORS for a list.
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

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H
# include "config.h"
#endif                          /* HAVE_CONFIG_H */

#define _BSD_SOURCE     1 
#define _POSIX_C_SOURCE 199309L
#include "send.h"

#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#ifdef BALSA_USE_THREADS
#include <pthread.h>
#endif

#include <string.h>

#include "libbalsa.h"
#include "libbalsa_private.h"

#include "server.h"
#include "misc.h"
#include "missing.h"
#include "information.h"

#if ENABLE_ESMTP
#include "smtp-server.h"
#include <stdarg.h>

#include <sys/types.h>
#include <sys/stat.h>
#endif /* ESMTP */

#include <sys/utsname.h>

#include <glib/gi18n.h>

typedef struct _MessageQueueItem MessageQueueItem;

struct _MessageQueueItem {
    char tempfile[_POSIX_PATH_MAX];
    LibBalsaMessage *orig;
    GMimeStream *stream;
    LibBalsaFccboxFinder finder;
#if !ENABLE_ESMTP
    MessageQueueItem *next_message;
    enum {MQI_WAITING, MQI_FAILED,MQI_SENT} status;
#else
    long message_size;
    long sent;
    long acc;
    long update;
    int refcount;
    gboolean error;
#endif
};

typedef struct _SendMessageInfo SendMessageInfo;

struct _SendMessageInfo{
    LibBalsaMailbox* outbox;
#if ENABLE_ESMTP
    /* [BCS] - The smtp_session_t structure holds all the information
       needed to transfer the message to the SMTP server.  This structure
       is opaque to the application. */
    smtp_session_t session;
#else
    LibBalsaFccboxFinder finder;
#endif
    gboolean debug;
};

/* Variables storing the state of the sending thread.
 * These variables are protected in MT by send_messages_lock mutex */
#if ENABLE_ESMTP
#else
static MessageQueueItem *message_queue;
#endif
static int sending_threads = 0; /* how many sending threads are active? */
/* end of state variables section */

gboolean
libbalsa_is_sending_mail(void)
{
    return sending_threads>0;
}

/* libbalsa_wait_for_sending_thread:
   wait for the sending thread but not longer than max_time seconds.
   -1 means wait indefinetely (almost).
*/
void
libbalsa_wait_for_sending_thread(gint max_time)
{
    gint sleep_time = 0;
#define DOZE_LENGTH (20*1000)
    static const struct timespec req = { 0, DOZE_LENGTH*1000 };/*nanoseconds*/

    if(max_time<0) max_time = G_MAXINT;
    else max_time *= 1000000; /* convert to microseconds */
    while(sending_threads>0 && sleep_time<max_time) {
        while(gtk_events_pending())
            gtk_main_iteration_do(FALSE);
        nanosleep(&req, NULL);
        sleep_time += DOZE_LENGTH;
    }
}


static MessageQueueItem *
msg_queue_item_new(LibBalsaFccboxFinder finder)
{
    MessageQueueItem *mqi;

    mqi = g_new(MessageQueueItem, 1);
    mqi->finder = finder;
#if !ENABLE_ESMTP
    mqi->next_message = NULL;
    mqi->status = MQI_WAITING;
#else
    mqi->error = FALSE;
#endif
    mqi->stream=NULL;
    mqi->tempfile[0] = '\0';
    return mqi;
}

static void
msg_queue_item_destroy(MessageQueueItem * mqi)
{
    if (*mqi->tempfile)
	unlink(mqi->tempfile);
    if (mqi->stream)
	g_object_unref(mqi->stream);
    if (mqi->orig)
	g_object_unref(mqi->orig);
    g_free(mqi);
}

#if ENABLE_ESMTP
static SendMessageInfo *
send_message_info_new(LibBalsaMailbox* outbox, smtp_session_t session,
                      gboolean debug)
{
    SendMessageInfo *smi;

    smi=g_new(SendMessageInfo,1);
    smi->session = session;
    smi->outbox = outbox;
    smi->debug = debug;

    return smi;
}
#else
static SendMessageInfo *
send_message_info_new(LibBalsaMailbox* outbox,
                      LibBalsaFccboxFinder finder, gboolean debug)
{
    SendMessageInfo *smi;

    smi=g_new(SendMessageInfo,1);
    smi->outbox = outbox;
    smi->finder = finder;
    smi->debug = debug;

    return smi;
}
#endif

static void
send_message_info_destroy(SendMessageInfo *smi)
{
    g_free(smi);
}


#if HAVE_GPGME
static LibBalsaMsgCreateResult
libbalsa_create_rfc2440_buffer(LibBalsaMessage *message, GMimePart *mime_part,
			       GtkWindow * parent, GError ** error);
static LibBalsaMsgCreateResult
do_multipart_crypto(LibBalsaMessage * message, GMimeObject ** mime_root,
		    GtkWindow * parent, GError ** error);
#endif

static guint balsa_send_message_real(SendMessageInfo* info);
static LibBalsaMsgCreateResult
libbalsa_message_create_mime_message(LibBalsaMessage* message,
				     gboolean flow, gboolean postponing,
				     GError ** error);
static LibBalsaMsgCreateResult libbalsa_create_msg(LibBalsaMessage * message,
						   gboolean flow, GError ** error);
static LibBalsaMsgCreateResult
libbalsa_fill_msg_queue_item_from_queu(LibBalsaMessage * message,
                                       MessageQueueItem *mqi);

#ifdef BALSA_USE_THREADS
void balsa_send_thread(MessageQueueItem * first_message);

GtkWidget *send_progress_message = NULL;
GtkWidget *send_dialog = NULL;
GtkWidget *send_dialog_bar = NULL;

static void
send_dialog_response_cb(GtkWidget* w, gint response)
{
    if(response == GTK_RESPONSE_CLOSE)
	gtk_widget_destroy(w);
}

static void
send_dialog_destroy_cb(GtkWidget* w)
{
    send_dialog = NULL;
    send_progress_message = NULL;
    send_dialog_bar = NULL;
}
/* ensure_send_progress_dialog:
   ensures that there is send_dialog available.
*/
static void
ensure_send_progress_dialog()
{
    GtkWidget* label;
    GtkBox *content_box;

    if(send_dialog) return;

    send_dialog = gtk_dialog_new_with_buttons(_("Sending Mail..."), 
                                              NULL,
                                              GTK_DIALOG_DESTROY_WITH_PARENT,
                                              _("_Hide"), 
                                              GTK_RESPONSE_CLOSE,
                                              NULL);
    gtk_window_set_wmclass(GTK_WINDOW(send_dialog), "send_dialog", "Balsa");
    label = gtk_label_new(_("Sending Mail..."));
    content_box =
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(send_dialog)));
    gtk_box_pack_start(content_box, label, FALSE, FALSE, 0);

    send_progress_message = gtk_label_new("");
    gtk_box_pack_start(content_box, send_progress_message, FALSE, FALSE, 0);

    send_dialog_bar = gtk_progress_bar_new();
    gtk_box_pack_start(content_box, send_dialog_bar, FALSE, FALSE, 0);
    gtk_window_set_default_size(GTK_WINDOW(send_dialog), 250, 100);
    gtk_widget_show_all(send_dialog);
    g_signal_connect(G_OBJECT(send_dialog), "response", 
		     G_CALLBACK(send_dialog_response_cb), NULL);
    g_signal_connect(G_OBJECT(send_dialog), "destroy", 
		     G_CALLBACK(send_dialog_destroy_cb), NULL);
    /* Progress bar done */
}

/* define commands for locking and unlocking: it makes deadlock debugging
 * easier. */
#define send_lock()   pthread_mutex_lock(&send_messages_lock); 
#define send_unlock() pthread_mutex_unlock(&send_messages_lock);

#else
#define ensure_send_progress_dialog()
#define send_lock()   
#define send_unlock() 
#endif

static void
lbs_set_content(GMimePart * mime_part, gchar * content)
{
    GMimeStream *stream;
    GMimeDataWrapper *wrapper;

    stream = g_mime_stream_mem_new();
    g_mime_stream_write(stream, content, strlen(content));

    wrapper =
        g_mime_data_wrapper_new_with_stream(stream,
                                            GMIME_CONTENT_ENCODING_DEFAULT);
    g_object_unref(stream);

    g_mime_part_set_content_object(mime_part, wrapper);
    g_object_unref(wrapper);
}

#ifdef HAVE_GPGME
static GMimeObject *
add_mime_body_plain(LibBalsaMessageBody *body, gboolean flow, gboolean postpone,
                    guint use_gpg_mode,
                    LibBalsaMsgCreateResult * crypt_res, GError ** error)
#else
static GMimeObject *
add_mime_body_plain(LibBalsaMessageBody *body, gboolean flow, gboolean postpone)
#endif
{
    GMimePart *mime_part;
    const gchar * charset;
#ifdef HAVE_GPGME
    GtkWindow * parent = g_object_get_data(G_OBJECT(body->message), "parent-window");
#endif

    g_return_val_if_fail(body, NULL);
    
    charset=body->charset;

    if (body->content_type) {
        /* Use the suplied mime type */
        gchar *type, *subtype;

        /* FIXME: test sending with different mime types */
        g_message("path active");
        type = g_strdup (body->content_type);
        if ((subtype = strchr (type, '/'))) {
            *subtype++ = 0;
            mime_part = g_mime_part_new_with_type(type, subtype);
        } else {
            mime_part = g_mime_part_new_with_type("text", "plain");
        }
        g_free (type);
    } else {
        mime_part = g_mime_part_new_with_type("text", "plain");
    }

    g_mime_object_set_disposition(GMIME_OBJECT(mime_part), GMIME_DISPOSITION_INLINE);
    g_mime_part_set_content_encoding(mime_part, GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE);
    g_mime_object_set_content_type_parameter(GMIME_OBJECT(mime_part),
                                             "charset",
                                             charset ? charset :
                                             "us-ascii");
    if (flow) {
	g_mime_object_set_content_type_parameter(GMIME_OBJECT(mime_part),
						 "DelSp", "Yes");
        g_mime_object_set_content_type_parameter(GMIME_OBJECT(mime_part),
						 "Format", "Flowed");
    }

    if (charset &&
	g_ascii_strcasecmp(charset, "UTF-8")!=0 &&
	g_ascii_strcasecmp(charset, "UTF8")!=0)
    {
	GMimeStream *stream, *filter_stream;
	GMimeFilter *filter;
	GMimeDataWrapper *wrapper;

	stream = g_mime_stream_mem_new();
	filter_stream = g_mime_stream_filter_new(stream);
	filter = g_mime_filter_charset_new("UTF-8", charset);
	g_mime_stream_filter_add(GMIME_STREAM_FILTER(filter_stream), filter);
	g_object_unref(G_OBJECT(filter));

	g_mime_stream_write(filter_stream, body->buffer, strlen(body->buffer));
	g_object_unref(filter_stream);

        wrapper =
            g_mime_data_wrapper_new_with_stream(stream,
                                                GMIME_CONTENT_ENCODING_DEFAULT);
	g_object_unref(stream);

	g_mime_part_set_content_object(mime_part, wrapper);
	g_object_unref(G_OBJECT(wrapper));
    } else
	lbs_set_content(mime_part, body->buffer);

#ifdef HAVE_GPGME
    /* rfc 2440 sign/encrypt if requested */
    if (use_gpg_mode != 0) {
        *crypt_res =
            libbalsa_create_rfc2440_buffer(body->message,
                                           GMIME_PART(mime_part),
                                           parent, error);

        if (*crypt_res != LIBBALSA_MESSAGE_CREATE_OK) {
            g_object_unref(G_OBJECT(mime_part));
            return NULL;
        }
    }
#endif

    /* if requested, add a text/html version in a multipart/alternative */
    if (body->html_buffer && !postpone) {
        GMimeMultipart *mpa = g_mime_multipart_new_with_subtype("alternative");

        g_mime_multipart_add(mpa, GMIME_OBJECT(mime_part));
        g_object_unref(G_OBJECT(mime_part));

        mime_part = g_mime_part_new_with_type("text", "html");
        g_mime_multipart_add(mpa, GMIME_OBJECT(mime_part));
        g_object_unref(G_OBJECT(mime_part));
        g_mime_object_set_disposition(GMIME_OBJECT(mime_part), GMIME_DISPOSITION_INLINE);
        g_mime_part_set_content_encoding(mime_part, GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE);
        g_mime_object_set_content_type_parameter(GMIME_OBJECT(mime_part),
                                                 "charset", "UTF-8");
	lbs_set_content(mime_part, body->html_buffer);

#ifdef HAVE_GPGME
        if (use_gpg_mode != 0 &&
            (use_gpg_mode & LIBBALSA_PROTECT_MODE) != LIBBALSA_PROTECT_SIGN) {
            *crypt_res =
                libbalsa_create_rfc2440_buffer(body->message,
                                               GMIME_PART(mime_part),
                                               parent, error);

            if (*crypt_res != LIBBALSA_MESSAGE_CREATE_OK) {
                g_object_unref(G_OBJECT(mpa));
                return NULL;
            }
        }
#endif

        return GMIME_OBJECT(mpa);
    } else
        return GMIME_OBJECT(mime_part);
}

#if 0
/* you never know when you will need this one... */
static void dump_queue(const char*msg)
{
    MessageQueueItem *mqi = message_queue;
    printf("dumping message queue at %s:\n", msg);
    while(mqi) {
	printf("item: %p\n", mqi);
	mqi = mqi->next_message;
    }
}
#endif

/* libbalsa_message_queue:
   places given message in the outbox.
*/
static void libbalsa_set_message_id(GMimeMessage * mime_message);
LibBalsaMsgCreateResult
libbalsa_message_queue(LibBalsaMessage * message, LibBalsaMailbox * outbox,
		       LibBalsaMailbox * fccbox,
#if ENABLE_ESMTP
                       LibBalsaSmtpServer * smtp_server,
#endif /* ESMTP */
		       gboolean flow, GError ** error)
{
    LibBalsaMsgCreateResult result;
#if ENABLE_ESMTP
    guint big_message;
#endif /* ESMTP */
    gboolean rc;

    g_assert(error != NULL);
    g_return_val_if_fail(message, LIBBALSA_MESSAGE_CREATE_ERROR);

    if ((result = libbalsa_create_msg(message, flow, error)) !=
	LIBBALSA_MESSAGE_CREATE_OK)
        return result;

    if (fccbox)
        g_mime_object_set_header(GMIME_OBJECT(message->mime_msg), "X-Balsa-Fcc",
                                  fccbox->url);
#if ENABLE_ESMTP
    g_mime_object_set_header(GMIME_OBJECT(message->mime_msg), "X-Balsa-SmtpServer",
	                      libbalsa_smtp_server_get_name(smtp_server));

    big_message = libbalsa_smtp_server_get_big_message(smtp_server);
    if (big_message > 0) {
        GMimeMessage *mime_msg;
        GMimeMessage **mime_msgs;
        size_t nparts;
        guint i;

        mime_msg = message->mime_msg;
        mime_msgs =
            g_mime_message_partial_split_message(mime_msg, big_message,
                                                 &nparts);
        rc = TRUE;
        for (i = 0; i < nparts; ++i) {
            if (nparts > 1) {
                /* RFC 2046, 5.2.2: "...it is specified that entities of
                 * type "message/partial" must always have a content-
                 * transfer-encoding of 7bit (the default)" */
                g_mime_part_set_content_encoding(GMIME_PART
                                         (mime_msgs[i]->mime_part),
                                         GMIME_CONTENT_ENCODING_7BIT);
                libbalsa_set_message_id(mime_msgs[i]);
            }
            if (rc) {
                message->mime_msg = mime_msgs[i];
                rc = libbalsa_message_copy(message, outbox, error);
            }
            g_object_unref(mime_msgs[i]);
        }
        g_free(mime_msgs);
        message->mime_msg = mime_msg;
    } else
        rc = libbalsa_message_copy(message, outbox, error);
#else                           /* ESMTP */
    rc = libbalsa_message_copy(message, outbox, error);
#endif                          /* ESMTP */

    return rc ?  LIBBALSA_MESSAGE_CREATE_OK : LIBBALSA_MESSAGE_QUEUE_ERROR;
}

/* libbalsa_message_send:
   send the given messsage (if any, it can be NULL) and all the messages
   in given outbox.
*/
#if ENABLE_ESMTP
static gboolean lbs_process_queue(LibBalsaMailbox * outbox,
                                  LibBalsaFccboxFinder finder,
                                  LibBalsaSmtpServer * smtp_server,
                                  gboolean debug);

LibBalsaMsgCreateResult
libbalsa_message_send(LibBalsaMessage * message, LibBalsaMailbox * outbox,
                      LibBalsaMailbox * fccbox,
                      LibBalsaFccboxFinder finder,
                      LibBalsaSmtpServer * smtp_server,
                      gboolean flow, gboolean debug,
		      GError ** error)
{
    LibBalsaMsgCreateResult result = LIBBALSA_MESSAGE_CREATE_OK;

    g_return_val_if_fail(smtp_server != NULL,
                         LIBBALSA_MESSAGE_SERVER_ERROR);

    if (message != NULL)
        result = libbalsa_message_queue(message, outbox, fccbox,
                                        smtp_server, flow, error);

    if (result == LIBBALSA_MESSAGE_CREATE_OK
        && !lbs_process_queue(outbox, finder, smtp_server, debug))
            return LIBBALSA_MESSAGE_SEND_ERROR;

    return result;
}
#else /* ENABLE_ESMTP */
LibBalsaMsgCreateResult
libbalsa_message_send(LibBalsaMessage* message, LibBalsaMailbox* outbox,
		      LibBalsaMailbox* fccbox, LibBalsaFccboxFinder finder,
                      gboolean flow, gboolean debug,
		      GError ** error)
{
    LibBalsaMsgCreateResult result = LIBBALSA_MESSAGE_CREATE_OK;

    if (message != NULL)
 	result = libbalsa_message_queue(message, outbox, fccbox, flow, error);
    if (result == LIBBALSA_MESSAGE_CREATE_OK)
 	if (!libbalsa_process_queue(outbox, finder, debug))
 	    return LIBBALSA_MESSAGE_SEND_ERROR;
 
    return result;
}
#endif /* ENABLE_ESMTP */

#if ENABLE_ESMTP
/* [BCS] - libESMTP uses a callback function to read the message from the
   application to the SMTP server.
 */
#define BUFLEN	8192

static const char *
libbalsa_message_cb (void **buf, int *len, void *arg)
{
    MessageQueueItem *current_message = arg;
    GMimeStreamMem *mem_stream = GMIME_STREAM_MEM(current_message->stream);
    char *ptr;

    if (len == NULL) {
	g_mime_stream_reset(current_message->stream);
	return NULL;
    }

    *len = g_mime_stream_length(current_message->stream)
	    - g_mime_stream_tell(current_message->stream);
    ptr = (char *) mem_stream->buffer->data
	    + g_mime_stream_tell(current_message->stream);
    g_mime_stream_seek(current_message->stream, *len, GMIME_STREAM_SEEK_CUR);

    return ptr;
}

static void
add_recipients(smtp_message_t message,
               InternetAddressList * recipient_list)
{
    const InternetAddress *ia;
    int i;
    
    if (recipient_list == NULL)
	return;
    
    for (i = 0; i < internet_address_list_length (recipient_list); i++) {
        ia = internet_address_list_get_address (recipient_list, i);

	if (INTERNET_ADDRESS_IS_MAILBOX (ia))
	    smtp_add_recipient (message, INTERNET_ADDRESS_MAILBOX (ia)->addr);
	else
	    add_recipients(message, INTERNET_ADDRESS_GROUP (ia)->members);

            /* XXX  - this is where to add DSN requests.  It would be
               cool if LibBalsaAddress could contain DSN options
               for a particular recipient. */
    }
}

/* libbalsa_process_queue:
   treats given mailbox as a set of messages to send. Loads them up and
   launches sending thread/routine.
   NOTE that we do not close outbox after reading. send_real/thread message 
   handler does that.
*/
/* This version uses libESMTP. It has slightly different semantics than
   sendmail version so don't get fooled by similar variable names.
 */
static gboolean
lbs_process_queue(LibBalsaMailbox * outbox, LibBalsaFccboxFinder finder,
		  LibBalsaSmtpServer * smtp_server, gboolean debug)
{
    LibBalsaServer *server = LIBBALSA_SERVER(smtp_server);
    MessageQueueItem *new_message;
    SendMessageInfo *send_message_info;
    LibBalsaMessage *msg;
    smtp_session_t session;
    smtp_message_t message, bcc_message;
    const gchar *phrase, *mailbox, *subject;
    long estimate;
    guint msgno;
    gchar *host_with_port;
    send_lock();

    if (!libbalsa_mailbox_open(outbox, NULL)) {
	send_unlock();
	return FALSE;
    }
    if (!libbalsa_mailbox_total_messages(outbox)) {
	libbalsa_mailbox_close(outbox, TRUE);
	send_unlock();
	return TRUE;
    }
    /* We create here the progress bar */
    ensure_send_progress_dialog();

    /* Create the libESMTP session.  Loop over the out box and add the
       messages to the session. */

    /* FIXME - check for failure returns in the smtp_xxx() calls */
    host_with_port = strchr(server->host, ':') ?
        g_strdup(server->host) : g_strconcat(server->host, ":smtp", NULL);
    session = smtp_create_session ();
    smtp_set_server (session, host_with_port);
    g_free(host_with_port);

    /* Tell libESMTP how to use the SMTP STARTTLS extension.  */
    smtp_starttls_enable (session, server->tls_mode);

    /* Now tell libESMTP it can use the SMTP AUTH extension.  */
    smtp_auth_set_context(session,
                          libbalsa_smtp_server_get_authctx(smtp_server));
 
    /* At present Balsa can't handle one recipient only out of many
       failing.  Make libESMTP require all specified recipients to
       succeed before transferring a message.  */
    smtp_option_require_all_recipients (session, 1);

    for (msgno = libbalsa_mailbox_total_messages(outbox);
	 msgno > 0; msgno--) {
	const gchar *smtp_server_name;
        LibBalsaMsgCreateResult created;

        /* Skip this message if it either FLAGGED or DELETED: */
        if (!libbalsa_mailbox_msgno_has_flags
            (outbox, msgno, 0,
             (LIBBALSA_MESSAGE_FLAG_FLAGGED |
              LIBBALSA_MESSAGE_FLAG_DELETED)))
            continue;

	msg = libbalsa_mailbox_get_message(outbox, msgno);
        if (!msg) /* error? */
            continue;
        libbalsa_message_body_ref(msg, TRUE, TRUE);
        smtp_server_name =
            libbalsa_message_get_user_header(msg, "X-Balsa-SmtpServer");
        if (!smtp_server_name)
            smtp_server_name = libbalsa_smtp_server_get_name(NULL);
        if (strcmp(smtp_server_name,
                   libbalsa_smtp_server_get_name(smtp_server)) != 0) {
            libbalsa_message_body_unref(msg);
            g_object_unref(msg);
            continue;
        }

	new_message = msg_queue_item_new(finder);
        created = libbalsa_fill_msg_queue_item_from_queu(msg, new_message);
        libbalsa_message_body_unref(msg);

	if (created != LIBBALSA_MESSAGE_CREATE_OK) {
	    msg_queue_item_destroy(new_message);
	} else {
            gboolean has_open_recipients;
            guint n_bcc_recipients;
	    const InternetAddress *ia;

	    libbalsa_message_change_flags(msg,
                                          LIBBALSA_MESSAGE_FLAG_FLAGGED, 0);
	    /*
	       The message needs to be filtered and the newlines converted to
	       \r\n because internally the lines foolishly terminate with the
	       Unix \n despite RFC 2822 calling for \r\n.  Furthermore RFC 822
	       states that bare \n and \r are acceptable in messages and that
	       individually they do not constitute a line termination.  This
	       requirement cannot be reconciled with storing messages with Unix
	       line terminations.  RFC 2822 relieves this situation slightly by
	       prohibiting bare \r and \n.

	       The following code cannot therefore work correctly in all
	       situations.  Furthermore it is very inefficient since it must
	       search for the \n.
	     */
	    {
		GMimeStream *mem_stream;
		GMimeStream *filter_stream;
		GMimeFilter *filter;

		mem_stream = new_message->stream;
		filter_stream =
		    g_mime_stream_filter_new(mem_stream);
		filter =
		    g_mime_filter_crlf_new( TRUE,
					    FALSE);
		g_mime_stream_filter_add(GMIME_STREAM_FILTER(filter_stream),
					 filter);
		g_object_unref(G_OBJECT(filter));
		mem_stream = g_mime_stream_mem_new();
		g_mime_stream_write_to_stream(filter_stream, mem_stream);
		g_object_unref(filter_stream);
		g_mime_stream_reset(mem_stream);
		g_object_unref(new_message->stream);
		new_message->stream = mem_stream;
	    }

            /* If the message has To: or Cc: recipients, and the Bcc:
             * recipient list is present and contains exactly one
             * address, add an additional copy of the message to the
             * session for that one recipient, in which the Bcc: header
             * is preserved. */
            has_open_recipients =
                libbalsa_address_n_mailboxes_in_list(msg->headers->
                                                     to_list) > 0
                || libbalsa_address_n_mailboxes_in_list(msg->headers->
                                                        cc_list) > 0;
            n_bcc_recipients =
                libbalsa_address_n_mailboxes_in_list(msg->headers->
                                                     bcc_list);
            if (has_open_recipients && n_bcc_recipients == 1)
		bcc_message = smtp_add_message (session);
	    else
		bcc_message = NULL;
            new_message->refcount = bcc_message ? 2 : 1;

	    /* Add this after the Bcc: copy. */
	    message = smtp_add_message (session);

            /* The main copy must not contain a Bcc: header, unless the
             * message has no To: recipients and no Cc: recipients, and
             * exactly one Bcc: recipient: */
            if ((has_open_recipients && n_bcc_recipients > 0)
                || n_bcc_recipients > 1)
		smtp_set_header_option (message, "Bcc", Hdr_PROHIBIT, 1);

	    smtp_message_set_application_data (message, new_message);
	    smtp_set_messagecb (message, libbalsa_message_cb, new_message);
	    if (bcc_message) {
		smtp_message_set_application_data (bcc_message, new_message);
		smtp_set_messagecb (bcc_message,
				    libbalsa_message_cb, new_message);
	    }

#define LIBESMTP_ADDS_HEADERS
#ifdef LIBESMTP_ADDS_HEADERS
	    /* XXX - The following calls to smtp_set_header() probably
		     aren't necessary since they should already be in the
		     message. */

	    smtp_set_header (message, "Date", &msg->headers->date);
	    if (bcc_message)
		smtp_set_header (bcc_message, "Date", &msg->headers->date);

	    /* RFC 2822 does not require a message to have a subject.
	               I assume this is NULL if not present */
	    subject = LIBBALSA_MESSAGE_GET_SUBJECT(msg);
	    if (subject) {
	    	smtp_set_header (message, "Subject", subject);
		if (bcc_message)
		    smtp_set_header (bcc_message, "Subject", subject);
	    }

	    /* Add the sender info */
            if (msg->headers->from
		&& (ia = internet_address_list_get_address (msg->headers->from, 0))) {
	        phrase = ia->name;
		while (ia && INTERNET_ADDRESS_IS_GROUP (ia))
		    ia = internet_address_list_get_address (INTERNET_ADDRESS_GROUP (ia)->members, 0);
	        mailbox = ia ? INTERNET_ADDRESS_MAILBOX (ia)->addr : "";
            } else
                phrase = mailbox = "";
	    smtp_set_reverse_path (message, mailbox);
	    smtp_set_header (message, "From", phrase, mailbox);
	    if (bcc_message) {
		smtp_set_reverse_path (bcc_message, mailbox);
	        smtp_set_header (bcc_message, "From", phrase, mailbox);
	    }

	    if (msg->headers->reply_to
		&& (ia = internet_address_list_get_address (msg->headers->reply_to, 0))) {
		phrase = ia->name;
		while (ia && INTERNET_ADDRESS_IS_GROUP (ia))
		    ia = internet_address_list_get_address (INTERNET_ADDRESS_GROUP (ia)->members, 0);
	        mailbox = ia ? INTERNET_ADDRESS_MAILBOX (ia)->addr : "";
		smtp_set_header (message, "Reply-To", phrase, mailbox);
		if (bcc_message)
		    smtp_set_header (bcc_message, "Reply-To", phrase, mailbox);
	    }

	    if (msg->headers->dispnotify_to
		&& (ia = internet_address_list_get_address (msg->headers->dispnotify_to, 0))) {
		phrase = ia->name;
		while (ia && INTERNET_ADDRESS_IS_GROUP (ia))
		    ia = internet_address_list_get_address (INTERNET_ADDRESS_GROUP (ia)->members, 0);
	        mailbox = ia ? INTERNET_ADDRESS_MAILBOX (ia)->addr : "";
		smtp_set_header (message, "Disposition-Notification-To",
				 phrase, mailbox);
		if (bcc_message)
		    smtp_set_header (bcc_message, "Disposition-Notification-To",
		    	             phrase, mailbox);
	    }
#endif

	    /* Now need to add the recipients to the message.  The main
	       copy of the message gets the To and Cc recipient list, and
	       the Bcc recipient list, when it has more than one address.
	       The bcc copy gets the single Bcc recipient.  */

            add_recipients(message, msg->headers->to_list);
            add_recipients(message, msg->headers->cc_list);

            add_recipients(bcc_message ? bcc_message : message, 
                           msg->headers->bcc_list);

	    /* Prohibit status headers. */
	    smtp_set_header_option(message, "Status", Hdr_PROHIBIT, 1);
	    smtp_set_header_option(message, "X-Status", Hdr_PROHIBIT, 1);
            /* ... and all X-Balsa-* headers. */
            smtp_set_header_option(message, "X-Balsa-", Hdr_PROHIBIT, 1);


	    /* Estimate the size of the message.  This need not be exact
	       but it's better to err on the large side since some message
	       headers may be altered during the transfer. */
	    new_message->message_size = g_mime_stream_length(new_message->stream);

	    if (new_message->message_size > 0) {
		estimate = new_message->message_size;
		estimate += 1024 - (estimate % 1024);
		smtp_size_set_estimate (message, estimate);
		if (bcc_message)
		    smtp_size_set_estimate (bcc_message, estimate);
	    }

	    /* Set up counters for the progress bar.  Update is the byte
	       count when the progress bar should be updated.  This is
	       capped around 5k so that the progress bar moves about once
	       per second on a slow line.  On small messages it is smaller
	       to allow smooth progress of the bar. */
	    new_message->update = new_message->message_size / 20;
	    if (new_message->update < 100)
	        new_message->update = 100;
	    else if (new_message->update > 5 * 1024)
	        new_message->update = 5 * 1024;
	    new_message->sent = 0;
	    new_message->acc = 0;
	}
	g_object_unref(msg);
    }

   /* At this point the session is ready to be sent.  As I've written the
      code, a new smtp session is created for every call here.  Therefore
      a new thread is always required to dispatch it.
    */

    send_message_info=send_message_info_new(outbox, session, debug);

#ifdef BALSA_USE_THREADS
    sending_threads++;
    pthread_create(&send_mail, NULL,
		   (void *) &balsa_send_message_real, send_message_info);
    /* Detach so we don't need to pthread_join
     * This means that all resources will be
     * reclaimed as soon as the thread exits
     */
    pthread_detach(send_mail);
    send_unlock();
#else				/*non-threaded code */
    balsa_send_message_real(send_message_info);
#endif
    return TRUE;
}

gboolean
libbalsa_process_queue(LibBalsaMailbox * outbox,
                       LibBalsaFccboxFinder finder,
                       GSList * smtp_servers,
		       gboolean debug)
{
    for (; smtp_servers; smtp_servers = smtp_servers->next) {
        LibBalsaSmtpServer *smtp_server =
		LIBBALSA_SMTP_SERVER(smtp_servers->data);
        if (!lbs_process_queue(outbox, finder, smtp_server, debug))
            return FALSE;
    }

    return TRUE;
}

static void
disp_recipient_status(smtp_recipient_t recipient,
                      const char *mailbox, void *arg)
{
  const smtp_status_t *status = smtp_recipient_status (recipient);

  if(status->code != 0 && status->code != 250) {
      libbalsa_information(
                           LIBBALSA_INFORMATION_ERROR,
                           _("Could not send the message to %s:\n"
                             "%d: %s\n"
                             "Message left in your outbox.\n"), 
                           mailbox, status->code, status->text);
      (*(int*)arg)++;
  }
}

static void
handle_successful_send(smtp_message_t message, void *be_verbose)
{
    MessageQueueItem *mqi;
    const smtp_status_t *status;

    send_lock();
    /* Get the app data and decrement the reference count.  Only delete
       structures if refcount reaches zero */
    mqi = smtp_message_get_application_data (message);
    if (mqi != NULL)
      mqi->refcount--;

    if(mqi != NULL && mqi->orig != NULL && mqi->orig->mailbox && !mqi->error)
	libbalsa_message_change_flags(mqi->orig, 0,
                                      LIBBALSA_MESSAGE_FLAG_FLAGGED);
    else printf("mqi: %p mqi->orig: %p mqi->orig->mailbox: %p\n",
                  mqi, mqi ? mqi->orig : NULL, 
                  mqi&&mqi->orig ? mqi->orig->mailbox : NULL);
    status = smtp_message_transfer_status (message);
    if (status->code / 100 == 2) {
	if (mqi != NULL && mqi->orig != NULL && mqi->refcount <= 0 &&
            mqi->orig->mailbox && !mqi->error) {
            gboolean remove = TRUE;
            const gchar *fccurl =
                libbalsa_message_get_user_header(mqi->orig, "X-Balsa-Fcc");

	    if (fccurl) {
                LibBalsaMailbox *fccbox = mqi->finder(fccurl);
                GError *err = NULL;
                libbalsa_message_change_flags(mqi->orig, 0,
                                              LIBBALSA_MESSAGE_FLAG_NEW |
                                              LIBBALSA_MESSAGE_FLAG_FLAGGED);
		libbalsa_mailbox_sync_storage(mqi->orig->mailbox, FALSE);
                remove = libbalsa_message_copy(mqi->orig, fccbox, &err);
                if(!remove) {
                    libbalsa_information
                        (LIBBALSA_INFORMATION_ERROR, 
                         _("Saving sent message to %s failed: %s"),
                         fccbox->url, err ? err->message : "?");
                    g_clear_error(&err);
                }
            }
            /* If copy failed, mark the message again as flagged -
               otherwise it will get resent again. And again, and
               again... */
            libbalsa_message_change_flags(mqi->orig, remove ?
                                          LIBBALSA_MESSAGE_FLAG_DELETED :
                                          LIBBALSA_MESSAGE_FLAG_FLAGGED, 0);
	}
    } else {
        /* Record the error, so that the message will not be deleted
         * later... */
        mqi->error = TRUE;
        /* ...and mark it as:
         *   - flagged, so it will not be sent again until the error
         *     is fixed and the user manually clears the flag;
         *   - undeleted, in case it was already deleted. */
        if (mqi->orig && mqi->orig->mailbox)
            libbalsa_message_change_flags(mqi->orig,
                                          LIBBALSA_MESSAGE_FLAG_FLAGGED,
                                          LIBBALSA_MESSAGE_FLAG_DELETED);
	/* Check whether it was a problem with transfer. */
        if(*(gboolean*)be_verbose) {
            int cnt = 0;
            if(status->code != 250 && status->code != 0) {
                libbalsa_information(
                                     LIBBALSA_INFORMATION_ERROR,
                                     _("Relaying refused:\n"
                                       "%d: %s\n"
                                       "Message left in your outbox.\n"), 
                                     status->code, status->text);
                cnt++;
            }
            smtp_enumerate_recipients (message, disp_recipient_status, &cnt);
            if(cnt==0) { /* other error, maybe sender or message size? */
                status = smtp_reverse_path_status(message);
                if(status->code != 250 && status->code != 0) {
                    libbalsa_information
                        (LIBBALSA_INFORMATION_ERROR,
                         _("Relaying refused:\n"
                           "%d: %s\n"
                           "Message left in your outbox.\n"), 
                         status->code, status->text);
                } else
                    libbalsa_information
                        (LIBBALSA_INFORMATION_ERROR,
                         _("Message submission problem, placing it into your outbox.\n"
                           "System will attempt to resubmit the message until you delete it."));
                
            }
        }
    }
    if (mqi != NULL && mqi->refcount <= 0)
        msg_queue_item_destroy(mqi);
    send_unlock();
}

#ifdef BALSA_USE_THREADS
static void
libbalsa_smtp_event_cb (smtp_session_t session, int event_no, void *arg, ...)
{
    SendThreadMessage *threadmsg;
    MessageQueueItem *mqi;
    char buf[1024];
    va_list ap;
    const char *mailbox;
    smtp_message_t message;
    smtp_recipient_t recipient;
    const smtp_status_t *status;
    int len;
    float percent;

    va_start (ap, arg);
    switch (event_no) {
    case SMTP_EV_CONNECT:
	MSGSENDTHREAD(threadmsg, MSGSENDTHREADPROGRESS,
		      _("Connected to MTA"),
		      NULL, NULL, 0);
        break;
    case SMTP_EV_MAILSTATUS:
        mailbox = va_arg (ap, const char *);
        message = va_arg (ap, smtp_message_t);
	status = smtp_reverse_path_status (message);

        /* status code, mailbox */
        snprintf (buf, sizeof buf, _("From: %d <%s>"), status->code, mailbox);
	MSGSENDTHREAD(threadmsg, MSGSENDTHREADPROGRESS, buf, NULL, NULL, 0);

        /* mailbox, status code, status text */
        snprintf (buf, sizeof buf, _("From %s: %d %s"), 
	          mailbox, status->code, status->text);
        g_strchomp(buf);
	libbalsa_information(LIBBALSA_INFORMATION_DEBUG, "%s", buf);
        break;
    case SMTP_EV_RCPTSTATUS:
        mailbox = va_arg (ap, const char *);
        recipient = va_arg (ap, smtp_recipient_t);
	status = smtp_recipient_status (recipient);

        /* status code, mailbox */
        snprintf (buf, sizeof buf, _("To: %d <%s>"),  status->code, mailbox);
	MSGSENDTHREAD(threadmsg, MSGSENDTHREADPROGRESS, buf, NULL, NULL, 0);

        /* mailbox, status code, status text */
        snprintf (buf, sizeof buf, _("To %s: %d %s"),
	          mailbox, status->code, status->text);
        g_strchomp(buf);
	libbalsa_information(LIBBALSA_INFORMATION_DEBUG, "%s", buf);
        break;
    case SMTP_EV_MESSAGEDATA:
        message = va_arg (ap, smtp_message_t);
        len = va_arg (ap, int);
        mqi = smtp_message_get_application_data (message);
        if (mqi != NULL && mqi->message_size > 0) {
	    mqi->acc += len;
	    if (mqi->acc >= mqi->update) {
		mqi->sent += mqi->acc;
		mqi->acc = 0;

		percent = (float) mqi->sent / (float) mqi->message_size;
		if(percent>1) percent = 1;
		MSGSENDTHREAD(threadmsg, MSGSENDTHREADPROGRESS, "", NULL, NULL,
			      percent);
	    }
	}
        break;
    case SMTP_EV_MESSAGESENT:
        message = va_arg (ap, smtp_message_t);
        status = smtp_message_transfer_status (message);
	snprintf (buf, sizeof buf, "%d %s", status->code, status->text);
        g_strchomp(buf);
	MSGSENDTHREAD(threadmsg, MSGSENDTHREADPROGRESS, buf, NULL, NULL, 0);
	libbalsa_information(LIBBALSA_INFORMATION_DEBUG, "%s", buf);
        /* Reset 'mqi->sent' for the next message (i.e. bcc copy) */
        mqi = smtp_message_get_application_data (message);
        if (mqi != NULL) {
	    mqi->sent = 0;
	    mqi->acc = 0;
        }
        break;
    case SMTP_EV_DISCONNECT:
	MSGSENDTHREAD(threadmsg, MSGSENDTHREADPROGRESS,
		      _("Disconnected"),
		      NULL, NULL, 0);
        break;

#ifdef USE_TLS
        /* SMTP_TLS related things. Observe that we need to have SSL
	 * enabled in balsa to properly interpret libesmtp
	 * messages. */
    case SMTP_EV_INVALID_PEER_CERTIFICATE: {
        long vfy_result;
	SSL  *ssl;
	X509 *cert;
        int *ok;
        vfy_result = va_arg(ap, long); ok = va_arg(ap, int*);
	ssl = va_arg(ap, SSL*);
	cert = SSL_get_peer_certificate(ssl);
	if(cert) {
	    *ok = libbalsa_is_cert_known(cert, vfy_result);
	    X509_free(cert);
	}
        break;
    }
    case SMTP_EV_NO_PEER_CERTIFICATE:
    case SMTP_EV_WRONG_PEER_CERTIFICATE:
#if LIBESMTP_1_0_3_AVAILABLE
    case SMTP_EV_NO_CLIENT_CERTIFICATE:
#endif
    {
	int *ok;
	printf("SMTP-TLS event_no=%d\n", event_no);
	ok = va_arg(ap, int*);
	*ok = 1;
	break;
    }
#endif /* USE_TLS */
    }
    va_end (ap);
}
#else /* BALSA: USE_THREADS */
static void
libbalsa_smtp_event_cb_serial(smtp_session_t session, int event_no,
                              void *arg, ...)
{
    va_list ap;

    va_start (ap, arg);
    switch (event_no) {
#ifdef USE_TLS
        /* SMTP_TLS related things. Observe that we need to have SSL
	 * enabled in balsa to properly interpret libesmtp
	 * messages. */
    case SMTP_EV_INVALID_PEER_CERTIFICATE: {
        long vfy_result;
	SSL  *ssl;
	X509 *cert;
        int *ok;
        vfy_result = va_arg(ap, long); ok = va_arg(ap, int*);
	ssl = va_arg(ap, SSL*);
	cert = SSL_get_peer_certificate(ssl);
	if(cert) {
	    *ok = libbalsa_is_cert_known(cert, vfy_result);
	    X509_free(cert);
	}
        break;
    }
    case SMTP_EV_NO_PEER_CERTIFICATE:
    case SMTP_EV_WRONG_PEER_CERTIFICATE:
#if LIBESMTP_1_0_3_AVAILABLE
    case SMTP_EV_NO_CLIENT_CERTIFICATE:
#endif
    {
	int *ok;
	ok = va_arg(ap, int*);
	*ok = 1;
	break;
    }
#endif /* USE_TLS */
    }
    va_end (ap);
}
#endif /* BALSA_USE_THREADS */

#else /* ESMTP */

/* CHBM: non-esmtp version */

/* libbalsa_process_queue:
   treats given mailbox as a set of messages to send. Loads them up and
   launches sending thread/routine.
   NOTE that we do not close outbox after reading. send_real/thread message 
   handler does that.
*/
gboolean 
libbalsa_process_queue(LibBalsaMailbox* outbox, LibBalsaFccboxFinder finder,
                       gboolean debug)
{
    MessageQueueItem *mqi = NULL, *new_message;
    SendMessageInfo *send_message_info;
    LibBalsaMessage *msg;
    guint msgno;

    /* We do messages in queue now only if where are not sending them already */

    send_lock();

#ifdef BALSA_USE_THREADS
    if (sending_threads>0) {
	send_unlock();
	return TRUE;
    }
    sending_threads++;
#endif

    ensure_send_progress_dialog();
    if (!libbalsa_mailbox_open(outbox, NULL)) {
#ifdef BALSA_USE_THREADS
	sending_threads--;
	send_unlock();
#endif
	return FALSE;
    }
    for (msgno = libbalsa_mailbox_total_messages(outbox);
	 msgno > 0; msgno--) {
        LibBalsaMsgCreateResult created;

        if (libbalsa_mailbox_msgno_has_flags(outbox, msgno, 
                                             (LIBBALSA_MESSAGE_FLAG_FLAGGED |
                                              LIBBALSA_MESSAGE_FLAG_DELETED),
                                             0))
            continue;

	msg = libbalsa_mailbox_get_message(outbox, msgno);
        if (!msg) /* error? */
            continue;
        libbalsa_message_body_ref(msg, TRUE, TRUE); /* FIXME: do we need
                                                      * all headers? */
	new_message = msg_queue_item_new(finder);
        created = libbalsa_fill_msg_queue_item_from_queu(msg, new_message);
        libbalsa_message_body_unref(msg);
	
	if (created != LIBBALSA_MESSAGE_CREATE_OK) {
	    msg_queue_item_destroy(new_message);
	} else {
	    libbalsa_message_change_flags(msg,
                                          LIBBALSA_MESSAGE_FLAG_FLAGGED, 0);
	    if (mqi)
		mqi->next_message = new_message;
	    else
		message_queue = new_message;
	    mqi = new_message;
	}
    }

    send_message_info=send_message_info_new(outbox, finder, debug);
    
#ifdef BALSA_USE_THREADS
    
    pthread_create(&send_mail, NULL,
		   (void *) &balsa_send_message_real, send_message_info);
    /* Detach so we don't need to pthread_join
     * This means that all resources will be
     * reclaimed as soon as the thread exits
     */
    pthread_detach(send_mail);
    
#else				/*non-threaded code */
    
    balsa_send_message_real(send_message_info);
#endif
    send_unlock();
    return TRUE;
}

static void
handle_successful_send(MessageQueueItem *mqi, LibBalsaFccboxFinder finder)
{
    if (mqi->orig->mailbox) {
        gboolean remove = TRUE;
        const gchar *fccurl =
            libbalsa_message_get_user_header(mqi->orig, "X-Balsa-Fcc");

        libbalsa_message_change_flags(mqi->orig, 0,
                                      LIBBALSA_MESSAGE_FLAG_NEW |
                                      LIBBALSA_MESSAGE_FLAG_FLAGGED);
	libbalsa_mailbox_sync_storage(mqi->orig->mailbox, FALSE);

        if (mqi->orig->mailbox && fccurl) {
            LibBalsaMailbox *fccbox = mqi->finder(fccurl);
            remove =
                libbalsa_message_copy(mqi->orig, fccbox, NULL)>=0;
        }
        /* If copy failed, mark the message again as flagged -
           otherwise it will get resent again. And again, and
           again... */
        libbalsa_message_change_flags(mqi->orig, remove ?
                                      LIBBALSA_MESSAGE_FLAG_DELETED :
                                      LIBBALSA_MESSAGE_FLAG_FLAGGED, 0);
    }
    mqi->status = MQI_SENT;
}

/* get_msg2send: 
   returns first waiting message on the message_queue.
*/
static MessageQueueItem* get_msg2send()
{
    MessageQueueItem* res = message_queue;
    send_lock();

    while(res && res->status != MQI_WAITING)
	res = res->next_message;

    send_unlock();
    return res;
}

#endif /* ESMTP */

#if ENABLE_ESMTP
static void
monitor_cb (const char *buf, int buflen, int writing, void *arg)
{
  FILE *fp = arg;

  if (writing == SMTP_CB_HEADERS)
    {
      fputs ("H: ", fp);
      if (fwrite (buf, 1, buflen, fp) != (size_t) buflen)
        /* FIXME */ return;
      return;
    }

 fputs (writing ? "C: " : "S: ", fp);
 if (writing && g_ascii_strncasecmp(buf, "auth plain", 10) == 0) {
     fputs("AUTH (details hidden)\n", fp);
     return;
 }
 if (fwrite (buf, 1, buflen, fp) != (size_t) buflen)
   /* FIXME */ return;
 if (buf[buflen - 1] != '\n')
   putc ('\n', fp);
}

/* balsa_send_message_real:
   does the actual message sending. 
   This function may be called as a thread and should therefore do
   proper gdk_threads_{enter/leave} stuff around GTK or libbalsa calls.
   Also, structure info should be freed before exiting.
*/

/* [BCS] radically different since it uses the libESMTP interface.
 */
static guint
balsa_send_message_real(SendMessageInfo* info)
{
    gboolean session_started;
#ifdef BALSA_USE_THREADS
    SendThreadMessage *threadmsg;

    /* The event callback is used to write messages to the the progress
       dialog shown when transferring a message to the SMTP server. 
       This callback is only used in MT build, we do not show any
       feedback in non-MT version.
    */
    smtp_set_eventcb (info->session, libbalsa_smtp_event_cb, NULL);
#else
    smtp_set_eventcb (info->session, libbalsa_smtp_event_cb_serial, NULL);
#endif

    /* Add a protocol monitor when debugging is enabled. */
    if(info->debug)
        smtp_set_monitorcb (info->session, monitor_cb, stderr, 1);

    /* Kick off the connection with the MTA.  When this returns, all
       messages with valid recipients have been sent. */
    if ( !(session_started = smtp_start_session (info->session)) ){
        char buf[256];
        int smtp_err = smtp_errno();
        switch(smtp_err) {
        case -ECONNREFUSED:
            libbalsa_information
                (LIBBALSA_INFORMATION_ERROR,
                 _("SMTP server refused connection.\n"
                   "Check your internet connection."));
        case -EHOSTUNREACH:
            libbalsa_information
                (LIBBALSA_INFORMATION_ERROR,
                 _("SMTP server cannot be reached.\n"
                   "Check your internet connection."));
            break;
        case SMTP_ERR_NOTHING_TO_DO: /* silence this one? */
            break;
        case SMTP_ERR_EAI_AGAIN:
            /* this is really problem with DNS but it is nowadays
               caused by general connection problems. */
            libbalsa_information(LIBBALSA_INFORMATION_MESSAGE,
                                 _("Message left in Outbox (try again later)"));
            break;
        default:
            libbalsa_information (LIBBALSA_INFORMATION_ERROR,
                                  _("SMTP server problem (%d): %s\n"
                                    "Message is left in outbox."),
                                  smtp_errno(),
                                  smtp_strerror (smtp_errno (), 
                                                 buf, sizeof buf));
        }
    } 
    /* We give back all the resources used and delete the sent messages */
    /* Quite a bit of status info has been gathered about messages and
       their recipients.  The following will do a libbalsa_message_delete()
       on the messages with a 2xx status recorded against them.  However
       its possible for individual recipients to fail too.  Need a way to
       report it all.  */
    smtp_enumerate_messages (info->session, handle_successful_send, 
                             &session_started);

    libbalsa_mailbox_close(info->outbox, TRUE);
    /*
     * gdk_flush();
     * gdk_threads_leave();
     */

#ifdef BALSA_USE_THREADS
    send_lock();
    MSGSENDTHREAD(threadmsg, MSGSENDTHREADFINISHED, "", NULL, NULL, 0);
    sending_threads--;
    send_unlock();
#endif
        
    smtp_destroy_session (info->session);
    send_message_info_destroy(info);	
    return TRUE;
}

#else /* ESMTP */
static void
sendmail_add_recipients(GPtrArray *args, InternetAddressList* recipient_list)
{
    const InternetAddress *ia;
    int i;
    
    if (recipient_list == NULL)
	return;
    
    for (i = 0; i < internet_address_list_length(recipient_list); i++) {
        ia = internet_address_list_get_address(recipient_list, i);

	if (INTERNET_ADDRESS_IS_MAILBOX(ia))
	    g_ptr_array_add(args, INTERNET_ADDRESS_MAILBOX(ia)->addr);
	else
	    sendmail_add_recipients(args, INTERNET_ADDRESS_GROUP(ia)->members);
    }
}

/* balsa_send_message_real:
   does the actual message sending. 
   This function may be called as a thread and should therefore do
   proper gdk_threads_{enter/leave} stuff around GTK,libbalsa or
   libmutt calls. Also, structure info should be freed before exiting.
*/

static guint
balsa_send_message_real(SendMessageInfo* info)
{
    MessageQueueItem *mqi, *next_message;
#ifdef BALSA_USE_THREADS
    SendThreadMessage *threadmsg;
    send_lock();
    if (!message_queue) {
	sending_threads--;
	send_unlock();
	MSGSENDTHREAD(threadmsg, MSGSENDTHREADFINISHED, "", NULL, NULL, 0);
	send_message_info_destroy(info);	
	return TRUE;
    }
    send_unlock();
#else
    if(!message_queue){
	send_message_info_destroy(info);	
	return TRUE;
    }	
#endif


    while ( (mqi = get_msg2send()) != NULL) {
	GPtrArray *args = g_ptr_array_new();
	LibBalsaMessage *msg = LIBBALSA_MESSAGE(mqi->orig);
	InternetAddress *ia;
	gchar *cmd;
	FILE *sendmail;
	GMimeStream *out;

	g_ptr_array_add(args, SENDMAIL);

        /* Determine the sender info */
        if (msg->headers->from
            && (ia=internet_address_list_get_address(msg->headers->from, 0))) {
            while (ia && INTERNET_ADDRESS_IS_GROUP (ia))
                ia = internet_address_list_get_address
                    (INTERNET_ADDRESS_GROUP (ia)->members, 0);

            if (ia) {
                g_ptr_array_add(args, "-f");
                g_ptr_array_add(args, INTERNET_ADDRESS_MAILBOX (ia)->addr);
            }
        } 

	g_ptr_array_add(args, "--");
        
        sendmail_add_recipients(args, msg->headers->to_list);
        sendmail_add_recipients(args, msg->headers->cc_list);
        sendmail_add_recipients(args, msg->headers->bcc_list);

	g_ptr_array_add(args, NULL);
	cmd = g_strjoinv(" ", (gchar**)args->pdata);
	g_ptr_array_free(args, FALSE);
	if ( (sendmail=popen(cmd, "w")) == NULL) {
	    /* Error while sending */
	    mqi->status = MQI_FAILED;
	} else {
	    out = g_mime_stream_file_new(sendmail);
	    g_mime_stream_file_set_owner(GMIME_STREAM_FILE(out), FALSE);
	    if (g_mime_stream_write_to_stream(mqi->stream, out) == -1)
		mqi->status = MQI_FAILED;
	    g_object_unref(out);
	    if (pclose(sendmail) != 0)
		mqi->status = MQI_FAILED;
	    if (mqi->status != MQI_FAILED)
		mqi->status = MQI_SENT;
	}
	g_free(cmd);
    }

    /* We give back all the resources used and delete the sent messages */
    
    send_lock();
    mqi = message_queue;
    
    while (mqi != NULL) {
	if (mqi->status == MQI_SENT) 
	    handle_successful_send(mqi, info->finder);
	next_message = mqi->next_message;
	msg_queue_item_destroy(mqi);
	mqi = next_message;
    }
    
    gdk_threads_enter();
    libbalsa_mailbox_close(info->outbox, TRUE);
    gdk_threads_leave();

    message_queue = NULL;
#ifdef BALSA_USE_THREADS
    sending_threads--;
    MSGSENDTHREAD(threadmsg, MSGSENDTHREADFINISHED, "", NULL, NULL, 0);
#endif
    send_message_info_destroy(info);	
    send_unlock();
    return TRUE;
}

#endif /* ESMTP */


static void
message_add_references(LibBalsaMessage * message, GMimeMessage * msg)
{
    /* If the message has references set, add them to the envelope */
    if (message->references != NULL) {
	GList *list = message->references;
	GString *str = g_string_new(NULL);

	do {
	    if (str->len > 0)
		g_string_append_c(str, ' ');
	    g_string_append_printf(str, "<%s>", (gchar *) list->data);
	} while ((list = list->next) != NULL);
	g_mime_object_set_header(GMIME_OBJECT(msg), "References", str->str);
	g_string_free(str, TRUE);
    }

    if (message->in_reply_to)
	/* There's no specific header function for In-Reply-To */
	g_mime_object_set_header(GMIME_OBJECT(msg), "In-Reply-To",
				  message->in_reply_to->data);
}

#ifdef HAVE_GPGME
static GList *
get_mailbox_names(GList *list, InternetAddressList *address_list)
{
    gint i;

    for (i = 0; i < internet_address_list_length(address_list); i++) {
	InternetAddress *ia =
            internet_address_list_get_address(address_list, i);

	if (INTERNET_ADDRESS_IS_MAILBOX(ia))
	    list = g_list_append(list, g_strdup(((InternetAddressMailbox *) ia)->addr));
	else 
	    list = get_mailbox_names(list, ((InternetAddressGroup *) ia)->members);
    }

    return list;
}
#endif

/* We could have used g_strsplit_set(s, "/;", 3) but it is not
 * available in older glib. */
static gchar**
parse_content_type(const char* content_type)
{
    gchar ** ret = g_new0(gchar*, 3);
    char *delim, *slash = strchr(content_type, '/');
    if(!slash) {
        ret[0] = g_strdup(content_type);
        return ret;
    }
    ret[0] = g_strndup(content_type, slash-content_type);
    slash++;
    for(delim=slash; *delim && *delim != ';' && *delim != ' '; delim++);
    ret[1] = g_strndup(slash, delim-slash);
    return ret;
}

/* get_tz_offset() returns tz offset in minutes. NOTE: not all hours
   have 60 seconds! Once in a while they get corrected.  */
#define MIN_SEC		60		/* seconds in a minute */
#define	HOUR_MIN	60		/* minutes in an hour */
#define DAY_MIN		(24 * HOUR_MIN)	/* minutes in a day */

static int
get_tz_offset(time_t *t)
{
    struct tm gmt, lt;
    int off;
    gmtime_r(t, &gmt);
    localtime_r(t, &lt);

    off = (lt.tm_hour - gmt.tm_hour) * HOUR_MIN + lt.tm_min - gmt.tm_min;
    if (lt.tm_year < gmt.tm_year)       off -= DAY_MIN;
    else if (lt.tm_year > gmt.tm_year)	off += DAY_MIN;
    else if (lt.tm_yday < gmt.tm_yday)  off -= DAY_MIN;
    else if (lt.tm_yday > gmt.tm_yday)  off += DAY_MIN;

    /* special case: funny minutes */
    if (lt.tm_sec <= gmt.tm_sec - MIN_SEC) off -= 1;
    else if (lt.tm_sec >= gmt.tm_sec + MIN_SEC)	off += 1;

    return (off*100)/60; /* time zone offset in hundreds of hours (funny
                          * unit required by gmime) */
}

static LibBalsaMsgCreateResult
libbalsa_message_create_mime_message(LibBalsaMessage* message, gboolean flow,
				     gboolean postponing, GError ** error)
{
    gchar **mime_type;
    GMimeObject *mime_root = NULL;
    GMimeMessage *mime_message;
    LibBalsaMessageBody *body;
    InternetAddressList *ia_list;
    gchar *tmp;
    GList *list;
#ifdef HAVE_GPGME
    GtkWindow * parent = g_object_get_data(G_OBJECT(message), "parent-window");
#endif

    body = message->body_list;
    if (body && body->next)
	mime_root=GMIME_OBJECT(g_mime_multipart_new_with_subtype(message->subtype));

    while (body) {
	GMimeObject *mime_part;
	mime_part=NULL;

	if (body->file_uri || body->filename) {
	    if (body->content_type) {
		mime_type = parse_content_type(body->content_type);
	    } else {
                gchar * mt = g_strdup(libbalsa_vfs_get_mime_type(body->file_uri));
		mime_type = g_strsplit(mt,"/", 2);
		g_free(mt);
	    }

	    if (body->attach_mode == LIBBALSA_ATTACH_AS_EXTBODY) {
		GMimeContentType *content_type =
		    g_mime_content_type_new("message", "external-body");
		mime_part=g_mime_object_new_type("message", "external-body");
		g_mime_object_set_content_type(mime_part, content_type);
		g_mime_part_set_content_encoding(GMIME_PART(mime_part),
			                 GMIME_CONTENT_ENCODING_7BIT);
		if (body->filename && !strncmp(body->filename, "URL", 3)) {
		    g_mime_object_set_content_type_parameter(mime_part,
					     "access-type", "URL");
		    g_mime_object_set_content_type_parameter(mime_part,
					     "URL", body->filename + 4);
		} else {
		    g_mime_object_set_content_type_parameter(mime_part,
					     "access-type", "local-file");
		    g_mime_object_set_content_type_parameter(mime_part,
                                             "name", libbalsa_vfs_get_uri_utf8(body->file_uri));
		}
		lbs_set_content(GMIME_PART(mime_part),
                                "Note: this is _not_ the real body!\n");
	    } else if (g_ascii_strcasecmp(mime_type[0], "message") == 0) {
		GMimeStream *stream;
		GMimeParser *parser;
		GMimeMessage *mime_message;
		GError *err = NULL;

		stream = libbalsa_vfs_create_stream(body->file_uri, 0, FALSE, &err);
		if(!stream) {
		    if(err) {
			gchar *msg = 
			    err->message
			    ? g_strdup_printf(_("Cannot read %s: %s"),
					      libbalsa_vfs_get_uri_utf8(body->file_uri),
					      err->message)
			    : g_strdup_printf(_("Cannot read %s"),
					      libbalsa_vfs_get_uri_utf8(body->file_uri));
			g_set_error(error, err->domain, err->code, "%s", msg);
			g_clear_error(&err);
			g_free(msg);
		    }
		    return LIBBALSA_MESSAGE_CREATE_ERROR;
		}
		parser = g_mime_parser_new_with_stream(stream);
		g_object_unref(stream);
		mime_message = g_mime_parser_construct_message(parser);
		g_object_unref(parser);
                mime_part =
                    GMIME_OBJECT(g_mime_message_part_new_with_message
                                 (mime_type[1], mime_message));
		g_object_unref(mime_message);
	    } else {
		const gchar *charset = NULL;
		GMimeStream *stream;
		GMimeDataWrapper *content;
		GError *err = NULL;

		if (!g_ascii_strcasecmp(mime_type[0], "text")
		    && !(charset = body->charset)) {
		    charset = libbalsa_vfs_get_charset(body->file_uri);
		    if (!charset) {
			static const gchar default_type[] =
			    "application/octet-stream";

			libbalsa_information(LIBBALSA_INFORMATION_WARNING,
					     _("Cannot determine charset "
					       "for text file `%s'; "
					       "sending as mime type `%s'"),
					     libbalsa_vfs_get_uri_utf8(body->file_uri),
                                             default_type);
			g_strfreev(mime_type);
			mime_type = g_strsplit(default_type, "/", 2);
		    }
		}

		/* use BASE64 encoding for non-text mime types 
		   use 8BIT for message */
		mime_part =
		    GMIME_OBJECT(g_mime_part_new_with_type(mime_type[0],
				                           mime_type[1]));
		g_mime_object_set_disposition(mime_part,
			body->attach_mode == LIBBALSA_ATTACH_AS_INLINE ?
			GMIME_DISPOSITION_INLINE : GMIME_DISPOSITION_ATTACHMENT);
		if(g_ascii_strcasecmp(mime_type[0],"text") != 0)
		{
		    g_mime_part_set_content_encoding(GMIME_PART(mime_part),
			    GMIME_CONTENT_ENCODING_BASE64);
		} else {
		    /* is text */
		    g_mime_object_set_content_type_parameter(mime_part,
							     "charset",
							     charset);
		}

		g_mime_part_set_filename(GMIME_PART(mime_part),
                                         libbalsa_vfs_get_basename_utf8(body->file_uri));
		stream = libbalsa_vfs_create_stream(body->file_uri, 0, FALSE, &err);
		if(!stream) {
		    if(err) {
			gchar *msg = 
			    err->message
			    ? g_strdup_printf(_("Cannot read %s: %s"),
					      libbalsa_vfs_get_uri_utf8(body->file_uri),
					      err->message)
			    : g_strdup_printf(_("Cannot read %s"),
					      libbalsa_vfs_get_uri_utf8(body->file_uri));
			g_set_error(error, err->domain, err->code, "%s", msg);
			g_clear_error(&err);
			g_free(msg);
		    }
		    g_object_unref(G_OBJECT(mime_part));
		    return LIBBALSA_MESSAGE_CREATE_ERROR;
		}
		content = g_mime_data_wrapper_new_with_stream(stream,
			GMIME_CONTENT_ENCODING_DEFAULT);
		g_object_unref(stream);
		g_mime_part_set_content_object(GMIME_PART(mime_part),
			                       content);
		g_object_unref(content);
	    }
	    g_strfreev(mime_type);
	} else if (body->buffer) {
#ifdef HAVE_GPGME
            guint use_gpg_mode;
            LibBalsaMsgCreateResult crypt_res = LIBBALSA_MESSAGE_CREATE_OK;

	    /* in '2440 mode, touch *only* the first body! */
	    if (!postponing && body == body->message->body_list &&
		message->gpg_mode > 0 &&
		(message->gpg_mode & LIBBALSA_PROTECT_OPENPGP) != 0)
                use_gpg_mode = message->gpg_mode;
            else
                use_gpg_mode = 0;
            mime_part = add_mime_body_plain(body, flow, postponing, use_gpg_mode,
                                            &crypt_res, error);
            if (!mime_part) {
                if (mime_root)
                    g_object_unref(G_OBJECT(mime_root));
                return crypt_res;
            }
#else
            mime_part = add_mime_body_plain(body, flow, postponing);
#endif /* HAVE_GPGME */
	}

	if (mime_root) {
	    g_mime_multipart_add(GMIME_MULTIPART(mime_root),
				 GMIME_OBJECT(mime_part));
	    g_object_unref(G_OBJECT(mime_part));
	} else {
	    mime_root = mime_part;
	}

	body = body->next;
    }

#ifdef HAVE_GPGME
    if (message->body_list != NULL && !postponing) {
	LibBalsaMsgCreateResult crypt_res =
	    do_multipart_crypto(message, &mime_root, parent, error);
	if (crypt_res != LIBBALSA_MESSAGE_CREATE_OK)
	    return crypt_res;
    }
#endif
    
    mime_message = g_mime_message_new(TRUE);
    if (mime_root) {
	GList *param = message->parameters;

 	while (param) {
 	    gchar **vals = (gchar **)param->data;
 
	    g_mime_object_set_content_type_parameter(GMIME_OBJECT(mime_root),
						     vals[0], vals[1]);
 	    param = param->next;
 	}
	g_mime_message_set_mime_part(mime_message, mime_root);
	g_object_unref(G_OBJECT(mime_root));
    }
    message_add_references(message, mime_message);

    if (message->headers->from) {
	tmp = internet_address_list_to_string(message->headers->from,
		                              TRUE);
	if (tmp) {
	    g_mime_message_set_sender(mime_message, tmp);
	    g_free(tmp);
	}
    }
    if (message->headers->reply_to) {
	tmp = internet_address_list_to_string(message->headers->reply_to,
		                              TRUE);
	if (tmp) {
	    g_mime_message_set_reply_to(mime_message, tmp);
	    g_free(tmp);
	}
    }

    if (LIBBALSA_MESSAGE_GET_SUBJECT(message))
	g_mime_message_set_subject(mime_message,
				   LIBBALSA_MESSAGE_GET_SUBJECT(message));

    g_mime_message_set_date(mime_message, message->headers->date,
                            get_tz_offset(&message->headers->date));

    if ((ia_list = message->headers->to_list)) {
        InternetAddressList *recipients =
            g_mime_message_get_recipients(mime_message,
                                          GMIME_RECIPIENT_TYPE_TO);
        internet_address_list_append(recipients, ia_list);
    }

    if ((ia_list = message->headers->cc_list)) {
        InternetAddressList *recipients =
            g_mime_message_get_recipients(mime_message,
                                          GMIME_RECIPIENT_TYPE_CC);
        internet_address_list_append(recipients, ia_list);
    }

    if ((ia_list = message->headers->bcc_list)) {
        InternetAddressList *recipients =
            g_mime_message_get_recipients(mime_message,
                                          GMIME_RECIPIENT_TYPE_BCC);
        internet_address_list_append(recipients, ia_list);
    }

    if (message->headers->dispnotify_to) {
        tmp = internet_address_list_to_string(message->headers->dispnotify_to, TRUE);
	if (tmp) {
	    g_mime_object_append_header(GMIME_OBJECT(mime_message),
				      "Disposition-Notification-To", tmp);
	    g_free(tmp);
	}
    }

    for (list = message->headers->user_hdrs; list; list = list->next) {
	gchar **pair = list->data;
	g_strchug(pair[1]);
	g_mime_object_append_header(GMIME_OBJECT(mime_message), pair[0], pair[1]);
#if DEBUG_USER_HEADERS
        printf("adding header '%s:%s'\n", pair[0], pair[1]);
#endif
    }

    tmp = g_strdup_printf("Balsa %s", VERSION);
    g_mime_object_append_header(GMIME_OBJECT(mime_message), "X-Mailer", tmp);
    g_free(tmp);

    message->mime_msg = mime_message;

    return LIBBALSA_MESSAGE_CREATE_OK;
}

/* When we postpone a message in the compose window, we lose track of
 * the message we were replying to.  We *could* save some identifying
 * information in a dummy header, but it could still be hard to track it
 * down: it might have been filed in another mailbox, for instance.  For
 * now, we'll just let it go...
 */
gboolean
libbalsa_message_postpone(LibBalsaMessage * message,
                          LibBalsaMailbox * draftbox,
                          LibBalsaMessage * reply_message,
			  gchar ** extra_headers, gboolean flow,
			  GError **error)
{
    if (!message->mime_msg
        && libbalsa_message_create_mime_message(message, flow,
                                                TRUE, error) !=
        LIBBALSA_MESSAGE_CREATE_OK)
        return FALSE;

    if (extra_headers) {
	gint i;

	for (i = 0; extra_headers[i] && extra_headers[i + 1]; i += 2)
	    g_mime_object_set_header(GMIME_OBJECT(message->mime_msg), extra_headers[i],
				      extra_headers[i + 1]);
    }

    return libbalsa_message_copy(message, draftbox, error);
}


/* Create a message-id and set it on the mime message.
 */
static void
libbalsa_set_message_id(GMimeMessage * mime_message)
{
    struct utsname utsbuf;
    gchar *host = "localhost";
    gchar *message_id;
#if defined(_GNU_SOURCE) && defined(HAVE_STRUCT_UTSNAME_DOMAINNAME)
    gchar *fqdn;
    gchar *domain = "localdomain";

    /* In an ideal world, uname() allows us to make a FQDN. */
    if (uname(&utsbuf) == 0) {
	if (*utsbuf.nodename)
	    host = utsbuf.nodename;
	if (*utsbuf.domainname)
	    domain = utsbuf.domainname;
    }
    fqdn = g_strconcat(host, ".", domain, NULL);
    message_id = g_mime_utils_generate_message_id(fqdn);
    g_free(fqdn);
#else				/* _GNU_SOURCE */

    if (uname(&utsbuf) == 0 && *utsbuf.nodename)
	host = utsbuf.nodename;
    message_id = g_mime_utils_generate_message_id(host);
#endif				/* _GNU_SOURCE */

    g_mime_message_set_message_id(mime_message, message_id);
    g_free(message_id);
}

/* balsa_create_msg:
   copies message to msg.
*/ 
static LibBalsaMsgCreateResult
libbalsa_create_msg(LibBalsaMessage * message, gboolean flow, GError ** error)
{
    if (!message->mime_msg) {
	LibBalsaMsgCreateResult res =
	    libbalsa_message_create_mime_message(message, flow,
						 FALSE, error);
	if (res != LIBBALSA_MESSAGE_CREATE_OK)
	    return res;
    }

    libbalsa_set_message_id(message->mime_msg);

    return LIBBALSA_MESSAGE_CREATE_OK;
}

static LibBalsaMsgCreateResult
libbalsa_fill_msg_queue_item_from_queu(LibBalsaMessage * message,
                                       MessageQueueItem *mqi)
{
    mqi->orig = message;
    g_object_ref(mqi->orig);
    if (message->mime_msg) {
        g_mime_object_remove_header(GMIME_OBJECT(message->mime_msg),
                                    "Status");
        g_mime_object_remove_header(GMIME_OBJECT(message->mime_msg),
                                    "X-Status");
        g_mime_object_remove_header(GMIME_OBJECT(message->mime_msg),
                                    "X-Balsa-Fcc");
        g_mime_object_remove_header(GMIME_OBJECT(message->mime_msg),
                                    "X-Balsa-SmtpServer");
	mqi->stream = g_mime_stream_mem_new();
        libbalsa_mailbox_lock_store(message->mailbox);
	g_mime_object_write_to_stream(GMIME_OBJECT(message->mime_msg),
                                      mqi->stream);
        libbalsa_mailbox_unlock_store(message->mailbox);
	g_mime_stream_reset(mqi->stream);
    } else
	mqi->stream = libbalsa_message_stream(message);
    if (mqi->stream == NULL)
	return LIBBALSA_MESSAGE_CREATE_ERROR;
  
    return LIBBALSA_MESSAGE_CREATE_OK;
}


#ifdef HAVE_GPGME
static const gchar *
lb_send_from(LibBalsaMessage *message)
{
    InternetAddress *ia =
        internet_address_list_get_address(message->headers->from, 0);

    if (message->force_key_id)
        return message->force_key_id;
    
    while (INTERNET_ADDRESS_IS_GROUP(ia))
        ia = internet_address_list_get_address(((InternetAddressGroup *)
                                                ia)->members, 0);

    return ((InternetAddressMailbox *) ia)->addr;
}

static LibBalsaMsgCreateResult
libbalsa_create_rfc2440_buffer(LibBalsaMessage *message, GMimePart *mime_part,
			       GtkWindow * parent, GError ** error)
{
    gint mode = message->gpg_mode;
    gboolean always_trust = (mode & LIBBALSA_PROTECT_ALWAYS_TRUST) != 0;

    switch (mode & LIBBALSA_PROTECT_MODE)
	{
	case LIBBALSA_PROTECT_SIGN:   /* sign only */
	    if (!libbalsa_rfc2440_sign_encrypt(mime_part, 
					       lb_send_from(message),
					       NULL, FALSE,
					       parent, error))
		return LIBBALSA_MESSAGE_SIGN_ERROR;
	    break;
	case LIBBALSA_PROTECT_ENCRYPT:
	case LIBBALSA_PROTECT_SIGN | LIBBALSA_PROTECT_ENCRYPT:
	    {
		GList *encrypt_for = NULL;
		gboolean result;
			    
		/* build a list containing the addresses of all to:, cc:
		   and the from: address. Note: don't add bcc: addresses
		   as they would be visible in the encrypted block. */
		encrypt_for = get_mailbox_names(encrypt_for,
			                        message->headers->to_list);
		encrypt_for = get_mailbox_names(encrypt_for,
			                        message->headers->cc_list);
		encrypt_for = get_mailbox_names(encrypt_for,
					        message->headers->from);
                if (message->headers->bcc_list)
                    libbalsa_information
                        (LIBBALSA_INFORMATION_WARNING,
                         ngettext("This message will not be encrypted "
                                  "for the BCC: recipient.",
                                  "This message will not be encrypted "
                                  "for the BCC: recipients.",
                                  internet_address_list_length
                                  (message->headers->bcc_list)));

		if (mode & LIBBALSA_PROTECT_SIGN)
		    result = 
			libbalsa_rfc2440_sign_encrypt(mime_part, 
					              lb_send_from(message),
						      encrypt_for,
						      always_trust,
						      parent, error);
		else
		    result = 
			libbalsa_rfc2440_sign_encrypt(mime_part, 
						      NULL,
						      encrypt_for,
						      always_trust,
						      parent, error);
		g_list_foreach(encrypt_for, (GFunc) g_free, NULL);
		g_list_free(encrypt_for);
		if (!result)
		    return LIBBALSA_MESSAGE_ENCRYPT_ERROR;
	    }
	    break;
	default:
	    g_assert_not_reached();
	}

    return LIBBALSA_MESSAGE_CREATE_OK;
}
  
  
/* handle rfc2633 and rfc3156 signing and/or encryption of a message */
static LibBalsaMsgCreateResult
do_multipart_crypto(LibBalsaMessage * message, GMimeObject ** mime_root,
		    GtkWindow * parent, GError ** error)
{
    gpgme_protocol_t protocol;
    gboolean always_trust;

    /* check if we shall do any protection */
    if (!(message->gpg_mode & LIBBALSA_PROTECT_MODE))
	return LIBBALSA_MESSAGE_CREATE_OK;

    /* check which protocol should be used */
    if (message->gpg_mode & LIBBALSA_PROTECT_RFC3156)
	protocol = GPGME_PROTOCOL_OpenPGP;
#ifdef HAVE_SMIME
    else if (message->gpg_mode & LIBBALSA_PROTECT_SMIMEV3)
	protocol = GPGME_PROTOCOL_CMS;
#endif
    else if (message->gpg_mode & LIBBALSA_PROTECT_OPENPGP)
	return LIBBALSA_MESSAGE_CREATE_OK;  /* already done... */
    else
	return LIBBALSA_MESSAGE_ENCRYPT_ERROR;  /* hmmm.... */

    always_trust = (message->gpg_mode & LIBBALSA_PROTECT_ALWAYS_TRUST) != 0;
    /* sign and/or encrypt */
    switch (message->gpg_mode & LIBBALSA_PROTECT_MODE)
	{
	case LIBBALSA_PROTECT_SIGN:   /* sign message */
	    if (!libbalsa_sign_mime_object(mime_root,
					   lb_send_from(message),
					   protocol, parent, error))
		return LIBBALSA_MESSAGE_SIGN_ERROR;
	    break;
	case LIBBALSA_PROTECT_ENCRYPT:
	case LIBBALSA_PROTECT_ENCRYPT | LIBBALSA_PROTECT_SIGN:
	    {
		GList *encrypt_for = NULL;
		gboolean success;
		
		/* build a list containing the addresses of all to:, cc:
		   and the from: address. Note: don't add bcc: addresses
		   as they would be visible in the encrypted block. */
		encrypt_for = get_mailbox_names(encrypt_for,
						message->headers->to_list);
		encrypt_for = get_mailbox_names(encrypt_for,
						message->headers->cc_list);
		encrypt_for = g_list_append(encrypt_for,
					    g_strdup(lb_send_from(message)));
                if (message->headers->bcc_list
                    && internet_address_list_length(message->headers->
                                                    bcc_list) > 0)
		    libbalsa_information(LIBBALSA_INFORMATION_WARNING,
					 _("This message will not be encrypted for the BCC: recipient(s)."));

		if (message->gpg_mode & LIBBALSA_PROTECT_SIGN)
		    success = 
			libbalsa_sign_encrypt_mime_object(mime_root,
							  lb_send_from(message),
							  encrypt_for, protocol,
							  always_trust, parent,
							  error);
		else
		    success = 
			libbalsa_encrypt_mime_object(mime_root, encrypt_for,
						     protocol, always_trust,
						     parent, error);
		g_list_free(encrypt_for);
		
		if (!success)
		    return LIBBALSA_MESSAGE_ENCRYPT_ERROR;
		break;
	    }
	default:
	    g_error("illegal gpg_mode %d (" __FILE__ " line %d)",
		    message->gpg_mode, __LINE__);
	}

    return LIBBALSA_MESSAGE_CREATE_OK;
}
#endif
