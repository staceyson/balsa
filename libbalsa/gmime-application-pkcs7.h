/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/*
 * S/MIME application/pkcs7-mime support for gmime/balsa
 * Copyright (C) 2004 Albrecht Dre� <albrecht.dress@arcor.de>
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

#ifndef __GMIME_APPLICATION_PKCS7_H__
#define __GMIME_APPLICATION_PKCS7_H__

#ifdef __cplusplus
extern "C" {
#pragma }
#endif				/* __cplusplus */

#include <gmime/gmime-part.h>
#include <gmime/gmime-cipher-context.h>

#undef HAS_APPLICATION_PKCS7_MIME_SIGNED_SUPPORT


#ifdef HAS_APPLICATION_PKCS7_MIME_SIGNED_SUPPORT
/* Note: application/pkcs7-mime signed parts are not used within balsa, as
 * they can not be viewed by mMUA's without S/MIME support. Therefore,
 * Balsa always encodes S/MIME signed stuff as multipart/signed. */
int g_mime_application_pkcs7_sign(GMimePart * pkcs7,
				  GMimeObject * content,
				  GMimeCipherContext * ctx,
				  const char *userid, GError ** err);
#endif

GMimeObject *g_mime_application_pkcs7_verify(GMimePart * pkcs7,
					     GMimeSignatureValidity ** validity,
					     GMimeCipherContext * ctx, GError ** err);

int g_mime_application_pkcs7_encrypt(GMimePart * pkcs7,
				     GMimeObject * content,
				     GMimeCipherContext * ctx,
				     GPtrArray * recipients, GError ** err);

GMimeObject *g_mime_application_pkcs7_decrypt(GMimePart * pkcs7,
					      GMimeCipherContext * ctx, GError ** err);

#ifdef __cplusplus
}
#endif				/* __cplusplus */
#endif				/* __GMIME_APPLICATION_PKCS7_H__ */