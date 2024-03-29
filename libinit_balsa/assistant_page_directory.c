/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
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
#include "assistant_page_directory.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include <glib/gi18n.h>
#include "balsa-app.h"
#include "save-restore.h"
#include "misc.h"
#include "server.h"
#include "url.h"

#if !defined(ENABLE_TOUCH_UI)
#define INBOX_NAME    "Inbox"
#define OUTBOX_NAME   "Outbox"
#define SENTBOX_NAME  "Sentbox"
#define DRAFTBOX_NAME "Draftbox"
#else /* defined(ENABLE_TOUCH_UI) */
#define INBOX_NAME    "In"
#define OUTBOX_NAME   "Out"
#define SENTBOX_NAME  "Sent"
#define DRAFTBOX_NAME "Drafts"
#endif /* defined(ENABLE_TOUCH_UI) */
#define TRASH_NAME    "Trash"

static const gchar * const init_mbnames[NUM_EDs] = {
#if defined(ENABLE_TOUCH_UI)
    "_In:", "_Out:", "_Sent:", "_Drafts:", "_Trash:"
#else
    N_("_Inbox:"), N_("_Outbox:"), N_("_Sentbox:"), N_("_Draftbox:"),
    N_("_Trash:")
#endif
};

static void balsa_druid_page_directory_prepare(GtkAssistant * druid,
                                               GtkWidget * page,
                                               BalsaDruidPageDirectory * dir);
static gboolean balsa_druid_page_directory_next(GtkAssistant * druid,
                                                GtkWidget * page,
                                                BalsaDruidPageDirectory *
                                                dir);
static void unconditional_mailbox(const gchar * path,
                                  const gchar * prettyname,
                                  LibBalsaMailbox ** box, gchar ** error);

static void
unconditional_mailbox(const gchar * path, const gchar * prettyname,
                      LibBalsaMailbox ** box, gchar ** error)
{
    gchar *dup;
    gchar *index;
    char tmp[32] = "/tmp/balsa.XXXXXX";
    ciss_url_t url;
    gboolean ssl = FALSE, is_remote = FALSE;

    if ((*error) != NULL)
        return;

    dup = g_strdup(path);
    index = strrchr(dup, G_DIR_SEPARATOR);

    if (index == NULL) {
        (*error) =
            g_strdup_printf(_
                            ("The pathname \"%s\" must be specified"
                             " canonically -- it must start with a \'/\'."),
                            dup);
        g_free(dup);
        return;
    }

    *index = '\0';           /*Split off the dirs from the file. */

    if (balsa_init_create_to_directory(dup, error)) {
        /*TRUE->error */
        g_free(dup);
        return;
    }

    *index = G_DIR_SEPARATOR;

    url_parse_ciss(&url, dup);

    switch (url.scheme) {
    case U_IMAPS:
        ssl = TRUE;
    case U_IMAP:
        *box = (LibBalsaMailbox *) libbalsa_mailbox_imap_new();
        libbalsa_mailbox_imap_set_path((LibBalsaMailboxImap *) * box,
                                       url.path);
        is_remote = TRUE;
        break;
    case U_POPS:
        ssl = TRUE;
    case U_POP:
        *box = (LibBalsaMailbox *) libbalsa_mailbox_pop3_new();
        is_remote = TRUE;
        break;
    case U_FILE:
        *box =
            (LibBalsaMailbox *) libbalsa_mailbox_local_new(url.path, TRUE);
        break;
    default:
        *box = (LibBalsaMailbox *) libbalsa_mailbox_local_new(path, TRUE);
    }

    if (is_remote) {
        libbalsa_server_set_host(LIBBALSA_MAILBOX_REMOTE_SERVER(*box),
                                 url.host, ssl);
        libbalsa_server_set_username(LIBBALSA_MAILBOX_REMOTE_SERVER(*box),
                                     getenv("USER"));
    }
    g_free(dup);


    if (*box == NULL) {
        if (strcmp("/var/spool/mail/", path)) {
	    /* Don't fail if you can't create the spool mailbox. */
	    close(mkstemp(tmp));
		*box = (LibBalsaMailbox*)libbalsa_mailbox_local_new(tmp, FALSE);
		if (*box) {
			free((*box)->url);
			(*box)->url = g_strdup_printf("file://%s",path);
		}
		unlink(tmp);
	}
    }
    if ( *box == NULL) {
            (*error) =
                g_strdup_printf(_
                                ("The mailbox \"%s\" does not appear to be valid."),
                                path);
        return;
    }

    (*box)->name = g_strdup(gettext(prettyname));

    config_mailbox_add(*box, (char *) prettyname);
    if (box == &balsa_app.outbox)
        (*box)->no_reassemble = TRUE;
}

/* here are local prototypes */
static void balsa_druid_page_directory_init(BalsaDruidPageDirectory * dir,
                                            GtkWidget * page,
                                            GtkAssistant * druid);
static gboolean balsa_druid_page_directory_back(GtkAssistant * druid,
                                                GtkWidget * page,
                                                BalsaDruidPageDirectory *
                                                dir);
static void
balsa_druid_page_directory_init(BalsaDruidPageDirectory * dir,
                                GtkWidget * page,
                                GtkAssistant * druid)
{
    GtkTable *table;
    GtkLabel *label;
    int i;
    GtkWidget **init_widgets[NUM_EDs];
    gchar *imap_inbox = libbalsa_guess_imap_inbox();
    gchar *init_presets[NUM_EDs] = { NULL, NULL, NULL, NULL, NULL };

    dir->paths_locked = FALSE;

    dir->emaster.setbits = 0;
    dir->emaster.numentries = 0;
    dir->emaster.donemask = 0;

    table = GTK_TABLE(gtk_table_new(NUM_EDs + 1, 2, FALSE));

    label =
        GTK_LABEL(gtk_label_new
                  (_
                   ("Please verify the locations of your default mail files.\n"
                    "These will be created if necessary.")));
    gtk_label_set_justify(label, GTK_JUSTIFY_RIGHT);
    gtk_label_set_line_wrap(label, TRUE);

    gtk_table_attach(table, GTK_WIDGET(label), 0, 2, 0, 1,
                     GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 8, 4);

    if (0 /* FIXME: libbalsa_mailbox_exists(imap_inbox) */ )
        init_presets[INBOX] = imap_inbox;
    else {
        g_free(imap_inbox);
        init_presets[INBOX] = libbalsa_guess_mail_spool();
    }

    init_widgets[INBOX] = &(dir->inbox);
    init_widgets[OUTBOX] = &(dir->outbox);
    init_widgets[SENTBOX] = &(dir->sentbox);
    init_widgets[DRAFTBOX] = &(dir->draftbox);
    init_widgets[TRASH] = &(dir->trash);

    for (i = 0; i < NUM_EDs; i++) {
        gchar *preset;

        dir->ed[i].master = &(dir->emaster);

        if (init_presets[i])
            preset = init_presets[i];
        else
            preset = g_strdup("[Dummy value]");

#if defined(ENABLE_TOUCH_UI)
        balsa_init_add_table_entry(table, i, init_mbnames[i], preset,
                                   &(dir->ed[i]), druid, page, init_widgets[i]);
#else
        balsa_init_add_table_entry(table, i, _(init_mbnames[i]), preset,
                                   &(dir->ed[i]), druid, page, init_widgets[i]);
#endif

        g_free(preset);
    }

    gtk_box_pack_start(GTK_BOX(page), GTK_WIDGET(table), FALSE, TRUE,
                       8);
    gtk_widget_show_all(GTK_WIDGET(table));

    g_signal_connect(G_OBJECT(druid), "prepare",
                     G_CALLBACK(balsa_druid_page_directory_prepare),
                     dir);
    dir->my_num = 98765;
    dir->need_set = FALSE;
}


void
balsa_druid_page_directory(GtkAssistant * druid, GdkPixbuf * default_logo)
{
    BalsaDruidPageDirectory *dir;

    dir = g_new0(BalsaDruidPageDirectory, 1);
    dir->page = gtk_vbox_new(FALSE, FALSE);
    gtk_assistant_append_page(druid, dir->page);
    gtk_assistant_set_page_title(druid, dir->page, _("Mail Files"));
    gtk_assistant_set_page_header_image(druid, dir->page, default_logo);
    balsa_druid_page_directory_init(dir, dir->page, druid);
    g_object_weak_ref(G_OBJECT(druid), (GWeakNotify)g_free, dir);
}

static void
balsa_druid_page_directory_prepare(GtkAssistant * druid,
                                   GtkWidget * page,
                                   BalsaDruidPageDirectory * dir)
{
    gchar *buf;
    gint current_page_no = gtk_assistant_get_current_page(druid);

    if(page != dir->page) { /* This is not the page to be prepared. */
        if(dir->need_set) {
            if(current_page_no > dir->my_num)
                balsa_druid_page_directory_next(druid, page, dir);
            else
                balsa_druid_page_directory_back(druid, page, dir);
            dir->need_set = FALSE;
        }
        return;
    }
    dir->my_num = current_page_no;
    /* We want a change in the local mailroot to be reflected in the
     * directories here, but we don't want to trash user's custom
     * settings if needed. Hence the paths_locked variable; it should
     * work pretty well, because only a movement backwards should
     * change the mailroot; going forward should not lock the paths:
     * envision an error occurring; upon return to the Dir page the
     * entries should be the same.
     */

    if (!dir->paths_locked) {
        buf = g_build_filename(balsa_app.local_mail_directory, "outbox",
                               NULL);
        gtk_entry_set_text(GTK_ENTRY(dir->outbox), buf);
        g_free(buf);

        buf = g_build_filename(balsa_app.local_mail_directory, "sentbox",
                               NULL);
        gtk_entry_set_text(GTK_ENTRY(dir->sentbox), buf);
        g_free(buf);

        buf = g_build_filename(balsa_app.local_mail_directory, "draftbox",
                               NULL);
        gtk_entry_set_text(GTK_ENTRY(dir->draftbox), buf);
        g_free(buf);

        buf = g_build_filename(balsa_app.local_mail_directory, "trash",
                               NULL);
        gtk_entry_set_text(GTK_ENTRY(dir->trash), buf);
        g_free(buf);
    }

    /* Don't let them continue unless all entries have something. */
    gtk_assistant_set_page_complete(druid, page,
                                    ENTRY_MASTER_DONE(dir->emaster));

    dir->need_set = TRUE;
}


static gboolean
balsa_druid_page_directory_next(GtkAssistant * page, GtkWidget * druid,
                                BalsaDruidPageDirectory * dir)
{
    gchar *error = NULL;

    unconditional_mailbox(gtk_entry_get_text
                          (GTK_ENTRY(dir->inbox)), INBOX_NAME,
                          &balsa_app.inbox, &error);
    unconditional_mailbox(gtk_entry_get_text
                          (GTK_ENTRY(dir->outbox)), OUTBOX_NAME,
                          &balsa_app.outbox, &error);
    unconditional_mailbox(gtk_entry_get_text
                          (GTK_ENTRY(dir->sentbox)), SENTBOX_NAME,
                          &balsa_app.sentbox, &error);
    unconditional_mailbox(gtk_entry_get_text
                          (GTK_ENTRY(dir->draftbox)), DRAFTBOX_NAME,
                          &balsa_app.draftbox, &error);
    unconditional_mailbox(gtk_entry_get_text
                          (GTK_ENTRY(dir->trash)), TRASH_NAME,
                          &balsa_app.trash, &error);

    dir->paths_locked = TRUE;

    if (error) {
        GtkWidget *dlg =
            gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_ancestor
                                          (GTK_WIDGET(druid), 
                                           GTK_TYPE_WINDOW)),
                                   GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_OK,
                                   _("Problem Creating Mailboxes\n%s"),
                                   error);
        g_free(error);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return TRUE;
    }

    return FALSE;
}

#define SET_MAILBOX(fname, config, mbx) \
do { gchar *t=g_build_filename(balsa_app.local_mail_directory,(fname),NULL);\
 unconditional_mailbox(t, config, (mbx), &error); g_free(t);}while(0)

void
balsa_druid_page_directory_later(GtkWidget *druid)
{
    gchar *error = NULL;
    gchar *spool = libbalsa_guess_mail_spool();
    unconditional_mailbox(spool, INBOX_NAME, &balsa_app.inbox, &error);
    g_free(spool);
    SET_MAILBOX("trash",    TRASH_NAME,    &balsa_app.trash);
    SET_MAILBOX("outbox",   OUTBOX_NAME,   &balsa_app.outbox);
    SET_MAILBOX("sentbox",  SENTBOX_NAME,  &balsa_app.sentbox);
    SET_MAILBOX("draftbox", DRAFTBOX_NAME, &balsa_app.draftbox);
    if (error) {
        GtkWidget *dlg =
            gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_ancestor
                                          (GTK_WIDGET(druid), 
                                           GTK_TYPE_WINDOW)),
                                   GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_OK,
                                   _("Problem Creating Mailboxes\n%s"),
                                   error);
        g_free(error);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
}

static gboolean
balsa_druid_page_directory_back(GtkAssistant *druid, GtkWidget *page,
                                BalsaDruidPageDirectory * dir)
{
    dir->paths_locked = FALSE;
    return FALSE;
}
