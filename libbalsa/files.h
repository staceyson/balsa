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

#ifndef _BALSA_FILES_H
#define _BALSA_FILES_H

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include "body.h"
#include "libbalsa-vfs.h"

/* filename is the filename (naw!)
 * splice is what to put in between the prefix and the filename, if desired
 * prefixes is a null-termed array of strings of prefixes to try. There are defaults that are always
 *   tried.
 * We ignore proper slashing of names. Ie, /prefix//splice//file won't be caught.
 */

gchar *balsa_file_finder(const gchar * filename, const gchar * splice,
			 const gchar ** prefixes, gboolean warn);

#define balsa_pixmap_finder(filename) \
    (balsa_file_finder((filename), "pixmaps", NULL, TRUE))
#define balsa_pixmap_finder_no_warn(filename) \
    (balsa_file_finder((filename), "pixmaps", NULL, FALSE))

GdkPixbuf *libbalsa_icon_finder(const char *mime_type, const LibbalsaVfs * for_file,
				gchar** used_type, GtkIconSize size);

#endif
