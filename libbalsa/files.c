/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 *
 * Copyright (C) 1997-2009 Stuart Parmenter and others,
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
#include "files.h"

#include <ctype.h>
#include <string.h>

#include <gio/gio.h>

#include "misc.h"
#include "libbalsa.h"
#include <glib/gi18n.h>

static const gchar *permanent_prefixes[] = {
/*	BALSA_DATA_PREFIX,
	BALSA_STD_PREFIX,
	GNOME_DATA_PREFIX
	GNOME_STD_PREFIX,
	GNOME_LIB_PREFIX,*/
    BALSA_COMMON_PREFIXES,
    "src",
    ".",
    NULL
};

/* filename is the filename (naw!)
 * splice is what to put in between the prefix and the filename, if desired
 * prefixes is a null-termed array of strings of prefixes to try. There are defaults that are always
 *   tried.
 * We ignore proper slashing of names. Ie, /prefix//splice//file won't be caught.
 */
gchar *
balsa_file_finder(const gchar * filename, const gchar * splice,
		  const gchar ** prefixes, gboolean warn)
{
    gchar *cat;
    int i;

    g_return_val_if_fail(filename, NULL);

    if (splice == NULL)
	splice = "";

    for (i = 0; permanent_prefixes[i]; i++) {
	cat =
	    g_strconcat(permanent_prefixes[i], G_DIR_SEPARATOR_S, splice,
			G_DIR_SEPARATOR_S, filename, NULL);

	if (g_file_test(cat, G_FILE_TEST_IS_REGULAR))
	    return cat;

	g_free(cat);
    }

    if(prefixes) {
        for (i = 0; prefixes[i]; i++) {
            cat =
                g_strconcat(prefixes[i], G_DIR_SEPARATOR_S, splice,
			    G_DIR_SEPARATOR_S, filename, NULL);
            
            if (g_file_test(cat, G_FILE_TEST_IS_REGULAR))
                return cat;
            
            g_free(cat);
        }
    }
    cat =  g_strconcat("images", G_DIR_SEPARATOR_S, filename, NULL);
    if (g_file_test(cat, G_FILE_TEST_IS_REGULAR))
        return cat;
    g_free(cat);

    if (warn)
        g_warning("Cannot find expected file \"%s\" "
                  "(spliced with \"%s\") %s extra prefixes",
	          filename, splice,
                  prefixes ? "even with" : "with no");
    return NULL;
}


static GdkPixbuf *
libbalsa_default_attachment_pixbuf(gint size)
{
    char *icon;
    GdkPixbuf *tmp, *retval;

    icon = balsa_pixmap_finder ("attachment.png");
    tmp = gdk_pixbuf_new_from_file(icon, NULL);
    g_free(icon);
    if (!tmp)
        return NULL;

    retval = gdk_pixbuf_scale_simple(tmp, size, size, GDK_INTERP_BILINEAR);
    g_object_unref(tmp);

    return retval;
}


/* balsa_icon_finder:
 *   locate a suitable icon (pixmap graphic) based on 'mime-type' and/or
 *   'filename', either of which can be NULL.  If both arguments are
 *   non-NULL, 'mime-type' has priority.  If both are NULL, the default
 *   'attachment.png' icon will be returned.  This function *MUST*
 *   return the complete path to the icon file.
 */
GdkPixbuf *
libbalsa_icon_finder(const char *mime_type, const LibbalsaVfs * for_file, 
                     gchar** used_type, GtkIconSize size)
{
    const gchar *content_type;
    gchar *icon = NULL;
    GdkPixbuf *pixbuf = NULL;
    gint width, height;
    GtkIconTheme *icon_theme;

    if (!gtk_icon_size_lookup(size, &width, &height))
	width = height = 16;
    
    if (mime_type)
        content_type = mime_type;
    else if (for_file) {
        content_type = libbalsa_vfs_get_mime_type(for_file);
    } else
	content_type = "application/octet-stream";

    /* ask GIO for the icon */
    if ((icon_theme = gtk_icon_theme_get_default())) {
        GIcon * icon = g_content_type_get_icon(content_type);

        if (icon && G_IS_THEMED_ICON(icon)) {
            int i = 0;
            GStrv icon_names;

            g_object_get(G_OBJECT(icon), "names", &icon_names, NULL);
            while (!pixbuf && icon_names && icon_names[i]) {
                pixbuf = gtk_icon_theme_load_icon(icon_theme, icon_names[i],
                                                  width, 0, NULL);
                i++;
            }
            g_strfreev(icon_names);
            g_object_unref(icon);

            /* last resort: try gnome-mime-<base mime type> */
            if (!pixbuf) {
                gchar * base_type_icon = g_strdup_printf("gnome-mime-%s", content_type);
                gchar * slash = strchr(base_type_icon, '/');

                if (slash)
                    *slash = '\0';
                pixbuf = gtk_icon_theme_load_icon(icon_theme, base_type_icon,
                                                   width, 0, NULL);
                g_free(base_type_icon);
            }

            /* return if we found a proper pixbuf */
	    if (pixbuf) {
		if (used_type)
		    *used_type = g_strdup(content_type);
		return pixbuf;
	    }
        }
    }

    /* load the pixbuf */
    if (icon == NULL)
	pixbuf = libbalsa_default_attachment_pixbuf(width);
    else {
	GdkPixbuf *tmp_pb;
	
	if ((tmp_pb = gdk_pixbuf_new_from_file(icon, NULL))) {
	    pixbuf = gdk_pixbuf_scale_simple(tmp_pb, width, width,
					     GDK_INTERP_BILINEAR);
	    g_object_unref(tmp_pb);
	} else
	    pixbuf = libbalsa_default_attachment_pixbuf(width);
	g_free(icon);
    }
    
    if (used_type)
        *used_type = g_strdup(content_type);
    
    return pixbuf;
}
