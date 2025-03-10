/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __COCKPIT_WEB_SERVER_H__
#define __COCKPIT_WEB_SERVER_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define COCKPIT_TYPE_WEB_SERVER  (cockpit_web_server_get_type ())
G_DECLARE_FINAL_TYPE(CockpitWebServer, cockpit_web_server, COCKPIT, WEB_SERVER, GObject)

extern guint cockpit_webserver_request_timeout;

typedef enum {
  COCKPIT_WEB_SERVER_NONE = 0,
  COCKPIT_WEB_SERVER_FOR_TLS_PROXY = 1 << 0,
  /* http → https redirection for non-localhost addresses */
  COCKPIT_WEB_SERVER_REDIRECT_TLS = 1 << 1,
  COCKPIT_WEB_SERVER_FLAGS_MAX = 1 << 2
} CockpitWebServerFlags;


CockpitWebServer * cockpit_web_server_new           (GTlsCertificate *certificate,
                                                     CockpitWebServerFlags flags);

void               cockpit_web_server_start         (CockpitWebServer *self);

GHashTable *       cockpit_web_server_new_table     (void);

gchar *            cockpit_web_server_parse_cookie    (GHashTable *headers,
                                                       const gchar *name);

gchar **           cockpit_web_server_parse_accept_list   (const gchar *accept,
                                                           const gchar *first);

CockpitWebServerFlags cockpit_web_server_get_flags         (CockpitWebServer *self);

guint16
cockpit_web_server_add_inet_listener (CockpitWebServer *self,
                                      const gchar *address,
                                      guint16 port,
                                      GError **error);

gboolean
cockpit_web_server_add_fd_listener (CockpitWebServer *self,
                                    int fd,
                                    GError **error);

GIOStream *
cockpit_web_server_connect (CockpitWebServer *self);

G_END_DECLS

#endif /* __COCKPIT_WEB_SERVER_H__ */
