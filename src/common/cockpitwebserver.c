/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013-2014 Red Hat, Inc.
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

#include "config.h"

#include "cockpitwebserver.h"

#include "cockpithash.h"
#include "cockpitmemfdread.h"
#include "cockpitmemory.h"
#include "cockpitsocket.h"
#include "cockpitwebresponse.h"

#include "websocket/websocket.h"

#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Used during testing */
gboolean cockpit_webserver_want_certificate = FALSE;

guint cockpit_webserver_request_timeout = 30;
const gsize cockpit_webserver_request_maximum = 8192;

struct _CockpitWebServer {
  GObject parent_instance;

  GTlsCertificate *certificate;
  GString *ssl_exception_prefix;
  GString *url_root;
  gint request_timeout;
  gint request_max;
  CockpitWebServerFlags flags;

  GSocketService *socket_service;
  GMainContext *main_context;
  GHashTable *requests;
};

enum
{
  PROP_0,
  PROP_CERTIFICATE,
  PROP_SSL_EXCEPTION_PREFIX,
  PROP_FLAGS,
  PROP_URL_ROOT,
};

static gint sig_handle_stream = 0;
static gint sig_handle_resource = 0;

static void cockpit_request_free (gpointer data);

static void cockpit_request_start (CockpitWebServer *self,
                                   GIOStream *stream,
                                   gboolean first);

static gboolean on_incoming (GSocketService *service,
                             GSocketConnection *connection,
                             GObject *source_object,
                             gpointer user_data);

G_DEFINE_TYPE (CockpitWebServer, cockpit_web_server, G_TYPE_OBJECT)

/* ---------------------------------------------------------------------------------------------------- */

static void
cockpit_web_server_init (CockpitWebServer *server)
{
  server->requests = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                            cockpit_request_free, NULL);
  server->main_context = g_main_context_ref_thread_default ();
  server->ssl_exception_prefix = g_string_new ("");
  server->url_root = g_string_new ("");

  server->socket_service = g_socket_service_new ();

  /* The web server has to be explicitly started */
  g_socket_service_stop (server->socket_service);

  g_signal_connect (server->socket_service, "incoming",
                    G_CALLBACK (on_incoming), server);
}

static void
cockpit_web_server_dispose (GObject *object)
{
  CockpitWebServer *self = COCKPIT_WEB_SERVER (object);

  g_hash_table_remove_all (self->requests);

  G_OBJECT_CLASS (cockpit_web_server_parent_class)->dispose (object);
}

static void
cockpit_web_server_finalize (GObject *object)
{
  CockpitWebServer *server = COCKPIT_WEB_SERVER (object);

  g_clear_object (&server->certificate);
  g_hash_table_destroy (server->requests);
  if (server->main_context)
    g_main_context_unref (server->main_context);
  g_string_free (server->ssl_exception_prefix, TRUE);
  g_string_free (server->url_root, TRUE);
  g_clear_object (&server->socket_service);

  G_OBJECT_CLASS (cockpit_web_server_parent_class)->finalize (object);
}

static void
cockpit_web_server_get_property (GObject *object,
                                 guint prop_id,
                                 GValue *value,
                                 GParamSpec *pspec)
{
  CockpitWebServer *server = COCKPIT_WEB_SERVER (object);

  switch (prop_id)
    {
    case PROP_CERTIFICATE:
      g_value_set_object (value, server->certificate);
      break;

    case PROP_SSL_EXCEPTION_PREFIX:
      g_value_set_string (value, server->ssl_exception_prefix->str);
      break;

    case PROP_URL_ROOT:
      if (server->url_root->len)
        g_value_set_string (value, server->url_root->str);
      else
        g_value_set_string (value, NULL);
      break;

    case PROP_FLAGS:
      g_value_set_int (value, server->flags);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cockpit_web_server_set_property (GObject *object,
                                 guint prop_id,
                                 const GValue *value,
                                 GParamSpec *pspec)
{
  CockpitWebServer *server = COCKPIT_WEB_SERVER (object);
  GString *str;

  switch (prop_id)
    {
    case PROP_CERTIFICATE:
      server->certificate = g_value_dup_object (value);
      break;

    case PROP_SSL_EXCEPTION_PREFIX:
      g_string_assign (server->ssl_exception_prefix, g_value_get_string (value));
      break;

    case PROP_URL_ROOT:
      str = g_string_new (g_value_get_string (value));

      while (str->str[0] == '/')
        g_string_erase (str, 0, 1);

      if (str->len)
        {
          while (str->str[str->len - 1] == '/')
            g_string_truncate (str, str->len - 1);
        }

      if (str->len)
        g_string_printf (server->url_root, "/%s", str->str);
      else
        g_string_assign (server->url_root, str->str);

      g_string_free (str, TRUE);
      break;

    case PROP_FLAGS:
      server->flags = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
on_io_closed (GObject *stream,
              GAsyncResult *result,
              gpointer user_data)
{
  GError *error = NULL;

  if (!g_io_stream_close_finish (G_IO_STREAM (stream), result, &error))
    {
      if (!cockpit_web_should_suppress_output_error ("http", error))
        g_message ("http close error: %s", error->message);
      g_error_free (error);
    }
}

static void
close_io_stream (GIOStream *io)
{
  g_io_stream_close_async (io, G_PRIORITY_DEFAULT, NULL, on_io_closed, NULL);
}

static void
on_web_response_done (CockpitWebResponse *response,
                      gboolean reusable,
                      gpointer user_data)
{
  CockpitWebServer *self = user_data;
  GIOStream *io;

  io = cockpit_web_response_get_stream (response);
  if (reusable)
    cockpit_request_start (self, io, FALSE);
  else
    close_io_stream (io);
}

static gboolean
cockpit_web_server_default_handle_resource (CockpitWebServer *self,
                                            const gchar *path,
                                            GHashTable *headers,
                                            CockpitWebResponse *response)
{
  cockpit_web_response_error (response, 404, NULL, NULL);
  return TRUE;
}

static gboolean
cockpit_web_server_default_handle_stream (CockpitWebServer *self,
                                          const gchar *original_path,
                                          const gchar *path,
                                          const gchar *method,
                                          GIOStream *io_stream,
                                          GHashTable *headers,
                                          GByteArray *input)
{
  CockpitWebResponse *response;
  gboolean claimed = FALSE;
  GQuark detail;
  gchar *pos;
  gchar *orig_pos;
  gchar bak;

  /* Yes, we happen to know that we can modify this string safely. */
  pos = strchr (path, '?');
  if (pos != NULL)
    {
      *pos = '\0';
      pos++;
    }

  /* We also have to strip original_path so that CockpitWebResponse
     can rediscover url_root. */
  orig_pos = strchr (original_path, '?');
  if (orig_pos != NULL)
    *orig_pos = '\0';

  /* TODO: Correct HTTP version for response */
  response = cockpit_web_response_new (io_stream, original_path, path, pos, headers,
                                       (self->flags & COCKPIT_WEB_SERVER_FOR_TLS_PROXY) ?
                                         COCKPIT_WEB_RESPONSE_FOR_TLS_PROXY : COCKPIT_WEB_RESPONSE_NONE);
  cockpit_web_response_set_method (response, method);
  g_signal_connect_data (response, "done", G_CALLBACK (on_web_response_done),
                         g_object_ref (self), (GClosureNotify)g_object_unref, 0);

  /*
   * If the path has more than one component, then we search
   * for handlers registered under the detail like this:
   *
   *   /component/
   *
   * Otherwise we search for handlers registered under detail
   * of the entire path:
   *
   *  /component
   */

  /* Temporarily null terminate string after first component */
  pos = NULL;
  if (path[0] != '\0')
    {
      pos = strchr (path + 1, '/');
      if (pos != NULL)
        {
          pos++;
          bak = *pos;
          *pos = '\0';
        }
    }
  detail = g_quark_try_string (path);
  if (pos != NULL)
    *pos = bak;

  /* See if we have any takers... */
  g_signal_emit (self,
                 sig_handle_resource, detail,
                 path,
                 headers,
                 response,
                 &claimed);

  if (!claimed)
    claimed = cockpit_web_server_default_handle_resource (self, path, headers, response);

  /* TODO: Here is where we would plug keep-alive into response */
  g_object_unref (response);

  return claimed;
}

static void
cockpit_web_server_class_init (CockpitWebServerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = cockpit_web_server_dispose;
  gobject_class->finalize = cockpit_web_server_finalize;
  gobject_class->set_property = cockpit_web_server_set_property;
  gobject_class->get_property = cockpit_web_server_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_CERTIFICATE,
                                   g_param_spec_object ("certificate", NULL, NULL,
                                                        G_TYPE_TLS_CERTIFICATE,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SSL_EXCEPTION_PREFIX,
                                   g_param_spec_string ("ssl-exception-prefix", NULL, NULL, "",
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_URL_ROOT,
                                   g_param_spec_string ("url-root", NULL, NULL, "",
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FLAGS,
                                   g_param_spec_int ("flags", NULL, NULL, 0, COCKPIT_WEB_SERVER_FLAGS_MAX, 0,
                                                     G_PARAM_READABLE |
                                                     G_PARAM_WRITABLE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));

  sig_handle_stream = g_signal_new ("handle-stream",
                                    G_OBJECT_CLASS_TYPE (klass),
                                    G_SIGNAL_RUN_LAST,
                                    0, /* class offset */
                                    g_signal_accumulator_true_handled,
                                    NULL, /* accu_data */
                                    g_cclosure_marshal_generic,
                                    G_TYPE_BOOLEAN,
                                    6,
                                    G_TYPE_STRING,
                                    G_TYPE_STRING,
                                    G_TYPE_STRING,
                                    G_TYPE_IO_STREAM,
                                    G_TYPE_HASH_TABLE,
                                    G_TYPE_BYTE_ARRAY);

  sig_handle_resource = g_signal_new ("handle-resource",
                                      G_OBJECT_CLASS_TYPE (klass),
                                      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                      0, /* class offset */
                                      g_signal_accumulator_true_handled,
                                      NULL, /* accu_data */
                                      g_cclosure_marshal_generic,
                                      G_TYPE_BOOLEAN,
                                      3,
                                      G_TYPE_STRING,
                                      G_TYPE_HASH_TABLE,
                                      COCKPIT_TYPE_WEB_RESPONSE);
}

CockpitWebServer *
cockpit_web_server_new (GTlsCertificate *certificate,
                        CockpitWebServerFlags flags)
{
  return g_object_new (COCKPIT_TYPE_WEB_SERVER,
                       "certificate", certificate,
                       "flags", flags,
                       NULL);
}

/* ---------------------------------------------------------------------------------------------------- */

CockpitWebServerFlags
cockpit_web_server_get_flags (CockpitWebServer *self)
{
  g_return_val_if_fail (COCKPIT_IS_WEB_SERVER (self), COCKPIT_WEB_SERVER_NONE);

  return self->flags;
}

GHashTable *
cockpit_web_server_new_table (void)
{
  return g_hash_table_new_full (cockpit_str_case_hash, cockpit_str_case_equal, g_free, g_free);
}

gchar *
cockpit_web_server_parse_cookie (GHashTable *headers,
                                 const gchar *name)
{
  const gchar *header;
  const gchar *pos;
  const gchar *value;
  const gchar *end;
  gboolean at_start = TRUE;
  gchar *decoded;
  gint diff;
  gint offset;

  header = g_hash_table_lookup (headers, "Cookie");
  if (!header)
    return NULL;

  for (;;)
    {
      pos = strstr (header, name);
      if (!pos)
        return NULL;

      if (pos != header)
        {
          diff = strlen (header) - strlen (pos);
          offset = 1;
          at_start = FALSE;
          while (offset < diff)
            {
              if (!g_ascii_isspace (*(pos - offset)))
                {
                  at_start = *(pos - offset) == ';';
                  break;
                }
              offset++;
            }
        }

      pos += strlen (name);
      if (*pos == '=' && at_start)
        {
          value = pos + 1;
          end = strchr (value, ';');
          if (end == NULL)
            end = value + strlen (value);

          decoded = g_uri_unescape_segment (value, end, NULL);
          if (!decoded)
            g_debug ("invalid cookie encoding");

          return decoded;
        }
      else
        {
          at_start = FALSE;
        }
      header = pos;
    }
}

typedef struct {
  double qvalue;
  const gchar *value;
} Language;

static gint
sort_qvalue (gconstpointer a,
             gconstpointer b)
{
  const Language *la = *((Language **)a);
  const Language *lb = *((Language **)b);
  if (lb->qvalue == la->qvalue)
    return 0;
  return lb->qvalue < la->qvalue ? -1 : 1;
}

gchar **
cockpit_web_server_parse_accept_list (const gchar *accept,
                                      const gchar *defawlt)
{
  Language *lang;
  GPtrArray *langs;
  GPtrArray *ret;
  gchar *copy;
  gchar *value;
  gchar *next;
  gchar *pos;
  guint i;

  langs = g_ptr_array_new_with_free_func (g_free);

  if (defawlt)
    {
      lang = g_new0 (Language, 1);
      lang->qvalue = 0.1;
      lang->value = defawlt;
      g_ptr_array_add (langs, lang);
    }

  /* First build up an array we can sort */
  accept = copy = g_strdup (accept);

  while (accept)
    {
      next = strchr (accept, ',');
      if (next)
        {
          *next = '\0';
          next++;
        }

      lang = g_new0 (Language, 1);
      lang->qvalue = 1;

      pos = strchr (accept, ';');
      if (pos)
        {
          *pos = '\0';
          if (strncmp (pos + 1, "q=", 2) == 0)
            {
              lang->qvalue = g_ascii_strtod (pos + 3, NULL);
              if (lang->qvalue < 0)
                lang->qvalue = 0;
            }
        }

      lang->value = accept;
      g_ptr_array_add (langs, lang);
      accept = next;
    }

  g_ptr_array_sort (langs, sort_qvalue);

  /* Now in the right order add all the prefs */
  ret = g_ptr_array_new ();
  for (i = 0; i < langs->len; i++)
    {
      lang = langs->pdata[i];
      if (lang->qvalue > 0)
        {
          value = g_strstrip (g_ascii_strdown (lang->value, -1));
          g_ptr_array_add (ret, value);
        }
    }

  /* Add base languages after that */
  for (i = 0; i < langs->len; i++)
    {
      lang = langs->pdata[i];
      if (lang->qvalue > 0)
        {
          pos = strchr (lang->value, '-');
          if (pos)
            {
              value = g_strstrip (g_ascii_strdown (lang->value, pos - lang->value));
              g_ptr_array_add (ret, value);
            }
        }
    }

  g_free (copy);
  g_ptr_array_add (ret, NULL);
  g_ptr_array_free (langs, TRUE);
  return (gchar **)g_ptr_array_free (ret, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  int state;
  GIOStream *io;
  GByteArray *buffer;
  gint delayed_reply;
  CockpitWebServer *web_server;
  gboolean eof_okay;
  GSource *source;
  GSource *timeout;
  gboolean check_tls_redirect;
} CockpitRequest;

static void
cockpit_request_free (gpointer data)
{
  CockpitRequest *request = data;
  if (request->timeout)
    {
      g_source_destroy (request->timeout);
      g_source_unref (request->timeout);
    }
  if (request->source)
    {
      g_source_destroy (request->source);
      g_source_unref (request->source);
    }

  /*
   * Request memory is either cleared or used elsewhere, by
   * handle-stream handlers (eg: the default handler. Don't
   * clear it here. The buffer may still be in use.
   */
  g_byte_array_unref (request->buffer);
  g_object_unref (request->io);
  g_free (request);
}

static void
cockpit_request_finish (CockpitRequest *request)
{
  g_hash_table_remove (request->web_server->requests, request);
}

static void
process_delayed_reply (CockpitRequest *request,
                       const gchar *path,
                       GHashTable *headers)
{
  CockpitWebResponse *response;
  const gchar *host;
  const gchar *body;
  GBytes *bytes;
  gsize length;
  gchar *url;

  g_assert (request->delayed_reply > 299);

  response = cockpit_web_response_new (request->io, NULL, NULL, NULL, headers,
                                       (request->web_server->flags & COCKPIT_WEB_SERVER_FOR_TLS_PROXY) ?
                                         COCKPIT_WEB_RESPONSE_FOR_TLS_PROXY : COCKPIT_WEB_RESPONSE_NONE);
  g_signal_connect_data (response, "done", G_CALLBACK (on_web_response_done),
                         g_object_ref (request->web_server), (GClosureNotify)g_object_unref, 0);

  if (request->delayed_reply == 301)
    {
      body = "<html><head><title>Moved</title></head>"
        "<body>Please use TLS</body></html>";
      host = g_hash_table_lookup (headers, "Host");
      url = g_strdup_printf ("https://%s%s",
                             host != NULL ? host : "", path);
      length = strlen (body);
      cockpit_web_response_headers (response, 301, "Moved Permanently", length,
                                    "Content-Type", "text/html",
                                    "Location", url,
                                    NULL);
      g_free (url);
      bytes = g_bytes_new_static (body, length);
      if (cockpit_web_response_queue (response, bytes))
        cockpit_web_response_complete (response);
      g_bytes_unref (bytes);
    }
  else
    {
      cockpit_web_response_error (response, request->delayed_reply, NULL, NULL);
    }

  g_object_unref (response);
}

static gboolean
path_has_prefix (const gchar *path,
                 GString *prefix)
{
  return prefix->len > 0 &&
         strncmp (path, prefix->str, prefix->len) == 0 &&
         (path[prefix->len] == '\0' || path[prefix->len] == '/');
}

static gboolean
is_localhost_connection (GSocketConnection *conn)
{
  g_autoptr (GSocketAddress) addr = g_socket_connection_get_local_address (conn, NULL);
  if (G_IS_INET_SOCKET_ADDRESS (addr))
    {
      GInetAddress *inet = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (addr));
      return g_inet_address_get_is_loopback (inet);
    }

  return FALSE;
}

static void
process_request (CockpitRequest *request,
                 const gchar *method,
                 const gchar *path,
                 const gchar *host,
                 GHashTable *headers)
{
  gboolean claimed = FALSE;
  const gchar *actual_path;

  if (request->web_server->url_root->len &&
      !path_has_prefix (path, request->web_server->url_root))
    {
      request->delayed_reply = 404;
    }

  /* Redirect to TLS? */
  if (!request->delayed_reply && request->check_tls_redirect)
    {
      request->check_tls_redirect = FALSE;

      /* Certain paths don't require us to redirect */
      if (!path_has_prefix (path, request->web_server->ssl_exception_prefix))
        {
          if (!is_localhost_connection (G_SOCKET_CONNECTION (request->io)))
            {
              g_debug ("redirecting request from Host: %s to TLS", host);
              request->delayed_reply = 301;
            }
        }
    }

  if (request->delayed_reply)
    {
      process_delayed_reply (request, path, headers);
      return;
    }

  actual_path = path + request->web_server->url_root->len;

  /* See if we have any takers... */
  g_signal_emit (request->web_server,
                 sig_handle_stream, 0,
                 path,
                 actual_path,
                 method,
                 request->io,
                 headers,
                 request->buffer,
                 &claimed);

  if (!claimed)
    claimed = cockpit_web_server_default_handle_stream (request->web_server, path, actual_path, method,
                                                        request->io, headers, request->buffer);

  if (!claimed)
    g_critical ("no handler responded to request: %s", actual_path);
}

static gboolean
parse_and_process_request (CockpitRequest *request)
{
  gboolean again = FALSE;
  GHashTable *headers = NULL;
  gchar *method = NULL;
  gchar *path = NULL;
  const gchar *str;
  gchar *end = NULL;
  gssize off1;
  gssize off2;
  guint64 length;

  /* The hard input limit, we just terminate the connection */
  if (request->buffer->len > cockpit_webserver_request_maximum * 2)
    {
      g_message ("received HTTP request that was too large");
      goto out;
    }

  off1 = web_socket_util_parse_req_line ((const gchar *)request->buffer->data,
                                         request->buffer->len,
                                         &method,
                                         &path);
  if (off1 == 0)
    {
      again = TRUE;
      goto out;
    }
  if (off1 < 0)
    {
      g_message ("received invalid HTTP request line");
      request->delayed_reply = 400;
      goto out;
    }
  if (!path || path[0] != '/')
    {
      g_message ("received invalid HTTP path");
      request->delayed_reply = 400;
      goto out;
    }

  off2 = web_socket_util_parse_headers ((const gchar *)request->buffer->data + off1,
                                        request->buffer->len - off1,
                                        &headers);
  if (off2 == 0)
    {
      again = TRUE;
      goto out;
    }
  if (off2 < 0)
    {
      g_message ("received invalid HTTP request headers");
      request->delayed_reply = 400;
      goto out;
    }

  /* If we get a Content-Length then verify it is zero */
  length = 0;
  str = g_hash_table_lookup (headers, "Content-Length");
  if (str != NULL)
    {
      end = NULL;
      length = g_ascii_strtoull (str, &end, 10);
      if (!end || end[0])
        {
          g_message ("received invalid Content-Length");
          request->delayed_reply = 400;
          goto out;
        }

      /* The soft limit, we return 413 */
      if (length != 0)
        {
          g_debug ("received non-zero Content-Length");
          request->delayed_reply = 413;
        }
    }

  /* Not enough data yet */
  if (request->buffer->len < off1 + off2 + length)
    {
      again = TRUE;
      goto out;
    }

  if (!g_str_equal (method, "GET") && !g_str_equal (method, "HEAD"))
    {
      g_message ("received unsupported HTTP method");
      request->delayed_reply = 405;
    }

  str = g_hash_table_lookup (headers, "Host");
  if (!str || g_str_equal (str, ""))
    {
      g_message ("received HTTP request without Host header");
      request->delayed_reply = 400;
    }

  g_byte_array_remove_range (request->buffer, 0, off1 + off2);
  process_request (request, method, path, str, headers);

out:
  if (headers)
    g_hash_table_unref (headers);
  g_free (method);
  g_free (path);
  if (!again)
    cockpit_request_finish (request);
  return again;
}

#if !GLIB_CHECK_VERSION(2,43,2)
#define G_IO_ERROR_CONNECTION_CLOSED G_IO_ERROR_BROKEN_PIPE
#endif

static gboolean
should_suppress_request_error (GError *error,
                               gsize received)
{
  if (g_error_matches (error, G_TLS_ERROR, G_TLS_ERROR_EOF) ||
      g_error_matches (error, G_TLS_ERROR, G_TLS_ERROR_NOT_TLS))
    {
      g_debug ("request error: %s", error->message);
      return TRUE;
    }

  /* If no bytes received, then don't worry about ECONNRESET and friends */
  if (received > 0)
    return FALSE;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    {
      g_debug ("request error: %s", error->message);
      return TRUE;
    }

#if !GLIB_CHECK_VERSION(2,43,2)
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) &&
      strstr (error->message, g_strerror (ECONNRESET)))
    {
      g_debug ("request error: %s", error->message);
      return TRUE;
    }
#endif

  return FALSE;
}

static gboolean
on_request_input (GObject *pollable_input,
                  gpointer user_data)
{
  GPollableInputStream *input = (GPollableInputStream *)pollable_input;
  CockpitRequest *request = user_data;
  GError *error = NULL;
  gsize length;
  gssize count;

  length = request->buffer->len;

  /* With a GTlsServerConnection, the GSource callback is not called again if
   * there is still pending data in GnuTLS'es buffer.
   * (https://gitlab.gnome.org/GNOME/glib-networking/issues/20). Thus read up
   * to our allowed maximum size to ensure we got everything that's pending.
   * Add one extra byte so that parse_and_process_request() correctly rejects
   * requests that are > maximum, instead of hanging.
   *
   * FIXME: This may still hang for several large requests that are pipelined;
   * for these this needs to be changed into a loop.
   */
  g_byte_array_set_size (request->buffer, length + cockpit_webserver_request_maximum + 1);

  count = g_pollable_input_stream_read_nonblocking (input, request->buffer->data + length,
                                                    cockpit_webserver_request_maximum + 1, NULL, &error);
  if (count < 0)
    {
      g_byte_array_set_size (request->buffer, length);

      /* Just wait and try again */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          g_error_free (error);
          return TRUE;
        }

      if (!should_suppress_request_error (error, length))
        g_message ("couldn't read from connection: %s", error->message);

      cockpit_request_finish (request);
      g_error_free (error);
      return FALSE;
    }

  g_byte_array_set_size (request->buffer, length + count);

  if (count == 0)
    {
      if (request->eof_okay)
        close_io_stream (request->io);
      else
        g_debug ("caller closed connection early");
      cockpit_request_finish (request);
      return FALSE;
    }

  /* Once we receive data EOF is unexpected (until possible next request) */
  request->eof_okay = FALSE;

  return parse_and_process_request (request);
}

static void
start_request_input (CockpitRequest *request)
{
  GPollableInputStream *poll_in;
  GInputStream *in;

  /* Both GSocketConnection and GTlsServerConnection are pollable */
  in = g_io_stream_get_input_stream (request->io);
  poll_in = NULL;
  if (G_IS_POLLABLE_INPUT_STREAM (in))
    poll_in = (GPollableInputStream *)in;

  if (!poll_in || !g_pollable_input_stream_can_poll (poll_in))
    {
      if (in)
        g_critical ("cannot use a non-pollable input stream: %s", G_OBJECT_TYPE_NAME (in));
      else
        g_critical ("no input stream available");

      cockpit_request_finish (request);
      return;
    }

  /* Replace with a new source */
  if (request->source)
    {
      g_source_destroy (request->source);
      g_source_unref (request->source);
    }

  request->source = g_pollable_input_stream_create_source (poll_in, NULL);
  g_source_set_callback (request->source, (GSourceFunc)on_request_input, request, NULL);
  g_source_attach (request->source, request->web_server->main_context);
}

static gboolean
on_accept_certificate (GTlsConnection *conn,
                       GTlsCertificate *peer_cert,
                       GTlsCertificateFlags errors,
                       gpointer user_data)
{
  /* Only used during testing */
  g_assert (cockpit_webserver_want_certificate == TRUE);
  return TRUE;
}

static gboolean
on_socket_input (GSocket *socket,
                 GIOCondition condition,
                 gpointer user_data)
{
  CockpitRequest *request = user_data;
  guchar first_byte;
  GInputVector vector[1] = { { &first_byte, 1 } };
  gint flags = G_SOCKET_MSG_PEEK;
  GError *error = NULL;
  GIOStream *tls_stream;
  gssize num_read;
  g_auto(CockpitControlMessages) ccm;

  num_read = g_socket_receive_message (socket,
                                       NULL, /* out GSocketAddress */
                                       vector,
                                       1,
                                       &ccm.messages,
                                       &ccm.n_messages,
                                       &flags,
                                       NULL, /* GCancellable* */
                                       &error);

  if (num_read < 0)
    {
      /* Just wait and try again */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          g_error_free (error);
          return TRUE;
        }

      if (!should_suppress_request_error (error, 0))
        g_message ("couldn't read from socket: %s", error->message);

      cockpit_request_finish (request);
      g_error_free (error);
      return FALSE;
    }

  JsonObject *metadata = cockpit_memfd_read_json_from_control_messages (&ccm, &error);
  if (metadata)
    {
      g_assert (G_IS_SOCKET_CONNECTION (request->io));
      g_object_set_qdata_full (G_OBJECT (request->io),
                               g_quark_from_static_string ("metadata"),
                               metadata, (GDestroyNotify) json_object_unref);
    }
  else if (error != NULL)
    {
      g_warning ("Failed while reading metadata from new connection: %s", error->message);
      g_clear_error (&error);
    }

  /*
   * TLS streams are guaranteed to start with octet 22.. this way we can distinguish them
   * from regular HTTP requests
   */
  if (first_byte == 22 || first_byte == 0x80)
    {
      if (request->web_server->certificate == NULL)
        {
          g_warning ("Received unexpected TLS connection and no certificate was configured");
          cockpit_request_finish (request);
          return FALSE;
        }

      tls_stream = g_tls_server_connection_new (request->io,
                                                request->web_server->certificate,
                                                &error);
      if (tls_stream == NULL)
        {
          g_warning ("couldn't create new TLS stream: %s", error->message);
          cockpit_request_finish (request);
          g_error_free (error);
          return FALSE;
        }

      if (cockpit_webserver_want_certificate)
        {
          g_object_set (tls_stream, "authentication-mode", G_TLS_AUTHENTICATION_REQUESTED, NULL);
          g_signal_connect (tls_stream, "accept-certificate", G_CALLBACK (on_accept_certificate), NULL);
        }

      g_object_unref (request->io);
      request->io = G_IO_STREAM (tls_stream);
    }
  else
    {
      if (request->web_server->certificate || request->web_server->flags & COCKPIT_WEB_SERVER_REDIRECT_TLS)
        {
          /* non-TLS stream; defer redirection check until after header parsing */
          if (cockpit_web_server_get_flags (request->web_server) & COCKPIT_WEB_SERVER_REDIRECT_TLS)
            request->check_tls_redirect = TRUE;
        }
    }

  start_request_input (request);

  /* No longer run *this* source */
  return FALSE;
}

static gboolean
on_request_timeout (gpointer data)
{
  CockpitRequest *request = data;
  if (request->eof_okay)
    g_debug ("request timed out, closing");
  else
    g_message ("request timed out, closing");
  cockpit_request_finish (request);
  return FALSE;
}

static void
cockpit_request_start (CockpitWebServer *self,
                       GIOStream *io,
                       gboolean first)
{
  GSocketConnection *connection;
  CockpitRequest *request;
  GSocket *socket;

  request = g_new0 (CockpitRequest, 1);
  request->web_server = self;
  request->io = g_object_ref (io);
  request->buffer = g_byte_array_new ();

  /* Right before a request, EOF is not unexpected */
  request->eof_okay = TRUE;

  request->timeout = g_timeout_source_new_seconds (cockpit_webserver_request_timeout);
  g_source_set_callback (request->timeout, on_request_timeout, request, NULL);
  g_source_attach (request->timeout, self->main_context);

  if (first)
    {
      connection = G_SOCKET_CONNECTION (io);
      socket = g_socket_connection_get_socket (connection);
      g_socket_set_blocking (socket, FALSE);

      request->source = g_socket_create_source (g_socket_connection_get_socket (connection),
                                                G_IO_IN, NULL);
      g_source_set_callback (request->source, (GSourceFunc)on_socket_input, request, NULL);
      g_source_attach (request->source, self->main_context);
    }
  else
    start_request_input (request);

  /* Owns the request */
  g_hash_table_add (self->requests, request);
}

static gboolean
on_incoming (GSocketService *service,
             GSocketConnection *connection,
             GObject *source_object,
             gpointer user_data)
{
  CockpitWebServer *self = COCKPIT_WEB_SERVER (user_data);
  cockpit_request_start (self, G_IO_STREAM (connection), TRUE);

  /* handled */
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

guint16
cockpit_web_server_add_inet_listener (CockpitWebServer *self,
                                      const gchar *address,
                                      guint16 port,
                                      GError **error)
{
  if (address != NULL)
    {
      g_autoptr(GSocketAddress) socket_address = g_inet_socket_address_new_from_string (address, port);
      if (socket_address == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Couldn't parse IP address from `%s`", address);
          return 0;
        }

      g_autoptr(GSocketAddress) result_address = NULL;
      if (!g_socket_listener_add_address (G_SOCKET_LISTENER (self->socket_service), socket_address,
                                          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                          NULL, &result_address, error))
        return 0;

      port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (result_address));
      g_assert (port != 0);

      return port;
    }

  else if (port > 0)
    {
      if (g_socket_listener_add_inet_port (G_SOCKET_LISTENER (self->socket_service), port, NULL, error))
        return port;
      else
        return 0;
    }
  else
    return g_socket_listener_add_any_inet_port (G_SOCKET_LISTENER (self->socket_service), NULL, error);
}

gboolean
cockpit_web_server_add_fd_listener (CockpitWebServer *self,
                                    int fd,
                                    GError **error)
{
  g_autoptr(GSocket) socket = g_socket_new_from_fd (fd, error);
  if (socket == NULL)
    {
      g_prefix_error (error, "Failed to acquire passed socket %i: ", fd);
      return FALSE;
    }

  if (!g_socket_listener_add_socket (G_SOCKET_LISTENER (self->socket_service), socket, NULL, error))
    {
      g_prefix_error (error, "Failed to add listener for socket %i: ", fd);
      return FALSE;
    }

  return TRUE;
}

void
cockpit_web_server_start (CockpitWebServer *self)
{
  g_return_if_fail (COCKPIT_IS_WEB_SERVER (self));
  g_socket_service_start (self->socket_service);
}

GIOStream *
cockpit_web_server_connect (CockpitWebServer *self)
{
  g_return_val_if_fail (COCKPIT_IS_WEB_SERVER (self), NULL);

  g_autoptr(GIOStream) server = NULL;
  g_autoptr(GIOStream) client = NULL;

  cockpit_socket_streampair (&client, &server);

  cockpit_request_start (self, server, TRUE);

  return g_steal_pointer (&client);
}

/* ---------------------------------------------------------------------------------------------------- */
