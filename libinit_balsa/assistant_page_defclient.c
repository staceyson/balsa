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
#include "assistant_page_defclient.h"

#if HAVE_GNOME
/* setting the default Gnome mail client doesn't make sense if we don't build
 for Gnome */

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gconf/gconf-client.h>

#include <glib/gi18n.h>
#include "balsa-app.h"

/* here are local prototypes */

static void balsa_druid_page_defclient_init(BalsaDruidPageDefclient *defclient,
                                            GtkWidget *page,
                                            GtkAssistant *druid);
static void balsa_druid_page_defclient_toggle(GtkWidget * page,
                                          BalsaDruidPageDefclient * defclient);

static void
balsa_druid_page_defclient_init(BalsaDruidPageDefclient * defclient,
                                GtkWidget * page,
                                GtkAssistant * druid)
{
    GtkLabel *label;
    GtkWidget *yes, *no;

    defclient->default_client = 1;

    label =
        GTK_LABEL(gtk_label_new
                  (_("Use Balsa as default email client?")));
    gtk_label_set_justify(label, GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(label, TRUE);

    yes = gtk_radio_button_new_with_mnemonic(NULL, _("_Yes"));
    no = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(yes),
                                                         _("_No"));    

    g_signal_connect(G_OBJECT(yes), "toggled",
                       G_CALLBACK(balsa_druid_page_defclient_toggle),
                       defclient);

    gtk_box_pack_start(GTK_BOX(page), GTK_WIDGET(label), TRUE, TRUE, 8);
    gtk_box_pack_start(GTK_BOX(page), GTK_WIDGET(yes),   TRUE, TRUE, 2);
    gtk_box_pack_start(GTK_BOX(page), GTK_WIDGET(no),    TRUE, TRUE, 2);

    return;
}

void
balsa_druid_page_defclient(GtkAssistant *druid, GdkPixbuf *default_logo)
{
    BalsaDruidPageDefclient *defclient;
    GtkWidget *page;
    GConfClient *gc;

    gc = gconf_client_get_default(); /* FIXME: error handling */
    if(gc) {
        GError *err = NULL;
        gchar *cmd;
        gboolean set_to_balsa_already;
        cmd = 
            gconf_client_get_string
            (gc, "/desktop/gnome/url-handlers/mailto/command", &err);
        set_to_balsa_already = !err && cmd && strncmp(cmd,"balsa",5)==0;
        if(err) g_error_free(err);
        g_free(cmd);
        if(set_to_balsa_already)
            return;
    }
    defclient = g_new0(BalsaDruidPageDefclient, 1);
    page = gtk_vbox_new(FALSE, FALSE);
    gtk_assistant_append_page(druid, page);
    gtk_assistant_set_page_title(druid, page, _("Default Client"));
    gtk_assistant_set_page_header_image(druid, page, default_logo);
    balsa_druid_page_defclient_init(defclient, page, druid);
    /* This one is ready to pass through. */
    gtk_assistant_set_page_complete(druid, page, TRUE);
    g_object_weak_ref(G_OBJECT(druid), (GWeakNotify)g_free, defclient);
}

static void
balsa_druid_page_defclient_toggle(GtkWidget * page, 
                                  BalsaDruidPageDefclient * defclient)
{
    defclient->default_client = ! (defclient->default_client);
}

#endif /* HAVE_GNOME */
