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

#include "config.h"

#include "cockpitwebserver.h"
#include "cockpitwebresponse.h"

#include "common/cockpitsystem.h"
#include "common/cockpittest.h"

#include "websocket/websocket.h"
#include "websocket/websocketprivate.h"

#include <string.h>

typedef struct {
  CockpitWebServer *web_server;
  gchar *localport;
  gchar *hostport;
} TestCase;

typedef struct {
  const gchar *cert_file;
  gboolean local_only;
  gboolean inet_only;
  CockpitWebServerFlags server_flags;
} TestFixture;

#define SKIP_NO_HOSTPORT if (!tc->hostport) { g_test_skip ("No non-loopback network interface available"); return; }

static void
setup (TestCase *tc,
       gconstpointer data)
{
  const TestFixture *fixture = data;
  GTlsCertificate *cert = NULL;
  GError *error = NULL;
  GInetAddress *inet;
  gchar *str = NULL;
  gint port;

  inet = cockpit_test_find_non_loopback_address ();
  /* this can fail in environments with only localhost */
  if (inet != NULL)
    str = g_inet_address_to_string (inet);

  if (fixture && fixture->cert_file)
    {
      cert = g_tls_certificate_new_from_file (fixture->cert_file, &error);
      g_assert_no_error (error);

      /* don't require system SSL cert database in build environments */
      cockpit_expect_possible_log ("GLib-Net", G_LOG_LEVEL_WARNING, "couldn't load TLS file database: * No such file or directory");
    }

  gchar *address;
  if (fixture && fixture->local_only)
    address = "127.0.0.1";
  else if (fixture && fixture->inet_only)
    address = str;
  else
    address = NULL;

  tc->web_server = cockpit_web_server_new (cert,
                                           fixture ? fixture->server_flags : COCKPIT_WEB_SERVER_NONE);
  g_clear_object (&cert);

  port = cockpit_web_server_add_inet_listener (tc->web_server, address, 0, &error);
  g_assert_no_error (error);
  g_assert (port != 0);

  cockpit_web_server_start (tc->web_server);

  /* HACK: this should be "localhost", but this fails on COPR; https://github.com/cockpit-project/cockpit/issues/12423 */
  tc->localport = g_strdup_printf ("127.0.0.1:%d", port);
  if (str)
    tc->hostport = g_strdup_printf ("[%s]:%d", str, port);
  if (inet)
    g_object_unref (inet);
  g_free (str);
}

static void
teardown (TestCase *tc,
          gconstpointer data)
{
  cockpit_assert_expected ();

  /* Verifies that we're not leaking the web server */
  g_object_add_weak_pointer (G_OBJECT (tc->web_server), (gpointer *)&tc->web_server);
  g_object_unref (tc->web_server);
  g_assert (tc->web_server == NULL);

  g_free (tc->localport);
  g_free (tc->hostport);
}

static void
test_table (void)
{
  GHashTable *table;

  table = cockpit_web_server_new_table ();

  /* Case insensitive keys */
  g_hash_table_insert (table, g_strdup ("Blah"), g_strdup ("value"));
  g_hash_table_insert (table, g_strdup ("blah"), g_strdup ("another"));
  g_hash_table_insert (table, g_strdup ("Different"), g_strdup ("One"));

  g_assert_cmpstr (g_hash_table_lookup (table, "BLAH"), ==, "another");
  g_assert_cmpstr (g_hash_table_lookup (table, "differeNT"), ==, "One");

  g_hash_table_destroy (table);
}

static void
test_cookie_simple (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  g_hash_table_insert (table, g_strdup ("Cookie"), g_strdup ("cookie1=value"));

  result = cockpit_web_server_parse_cookie (table, "cookie1");
  g_assert_cmpstr (result, ==, "value");

  g_free (result);
  g_hash_table_unref (table);
}

static void
test_cookie_multiple (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  g_hash_table_insert (table, g_strdup ("Cookie"), g_strdup ("cookie1=value;cookie2=value2; cookie23=value3"));

  result = cockpit_web_server_parse_cookie (table, "cookie1");
  g_assert_cmpstr (result, ==, "value");
  g_free (result);

  result = cockpit_web_server_parse_cookie (table, "cookie2");
  g_assert_cmpstr (result, ==, "value2");
  g_free (result);

  result = cockpit_web_server_parse_cookie (table, "cookie23");
  g_assert_cmpstr (result, ==, "value3");
  g_free (result);

  g_hash_table_unref (table);
}

static void
test_cookie_overlap (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  g_hash_table_insert (table, g_strdup ("Cookie"), g_strdup ("cookie1cookie1cookie1=value;cookie1=cookie23-value2;   cookie2=a value for cookie23=inline; cookie23=value3"));

  result = cockpit_web_server_parse_cookie (table, "cookie1cookie1cookie1");
  g_assert_cmpstr (result, ==, "value");
  g_free (result);

  result = cockpit_web_server_parse_cookie (table, "cookie1");
  g_assert_cmpstr (result, ==, "cookie23-value2");
  g_free (result);

  result = cockpit_web_server_parse_cookie (table, "cookie2");
  g_assert_cmpstr (result, ==, "a value for cookie23=inline");
  g_free (result);

  result = cockpit_web_server_parse_cookie (table, "cookie23");
  g_assert_cmpstr (result, ==, "value3");
  g_free (result);

  g_hash_table_unref (table);
}

static void
test_cookie_no_header (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  result = cockpit_web_server_parse_cookie (table, "cookie2");
  g_assert_cmpstr (result, ==, NULL);

  g_hash_table_unref (table);
}

static void
test_cookie_substring (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  g_hash_table_insert (table, g_strdup ("Cookie"), g_strdup ("cookie1=value; cookie2=value2; cookie23=value3"));

  result = cockpit_web_server_parse_cookie (table, "okie2");
  g_assert_cmpstr (result, ==, NULL);

  result = cockpit_web_server_parse_cookie (table, "cookie");
  g_assert_cmpstr (result, ==, NULL);

  result = cockpit_web_server_parse_cookie (table, "ook");
  g_assert_cmpstr (result, ==, NULL);

  g_hash_table_unref (table);
}

static void
test_cookie_decode (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  g_hash_table_insert (table, g_strdup ("Cookie"), g_strdup ("cookie1=val%20ue"));

  result = cockpit_web_server_parse_cookie (table, "cookie1");
  g_assert_cmpstr (result, ==, "val ue");
  g_free (result);

  g_hash_table_unref (table);
}

static void
test_cookie_decode_bad (void)
{
  GHashTable *table = cockpit_web_server_new_table ();
  gchar *result;

  g_hash_table_insert (table, g_strdup ("Cookie"), g_strdup ("cookie1=val%"));

  result = cockpit_web_server_parse_cookie (table, "cookie1");
  g_assert_cmpstr (result, ==, NULL);

  g_hash_table_unref (table);
}

static void
test_accept_list_simple (void)
{
  gchar **result;
  gchar *string;

  result = cockpit_web_server_parse_accept_list ("en-us,en, de", NULL);
  g_assert (result != NULL);

  string = g_strjoinv (", ", result);
  g_assert_cmpstr (string, ==, "en-us, en, de, en");

  g_free (string);
  g_strfreev (result);
}

static void
test_accept_list_cookie (void)
{
  gchar **result;
  gchar *string;

  result = cockpit_web_server_parse_accept_list ("en-us,en, de", "pig");
  g_assert (result != NULL);

  string = g_strjoinv (", ", result);
  g_assert_cmpstr (string, ==, "en-us, en, de, pig, en");

  g_free (string);
  g_strfreev (result);
}

static void
test_accept_list_no_header (void)
{
  gchar **result;

  result = cockpit_web_server_parse_accept_list (NULL, NULL);
  g_assert (result != NULL);
  g_assert (result[0] == NULL);

  g_strfreev (result);
}

static void
test_accept_list_order (void)
{
  gchar **result;
  gchar *string;

  result = cockpit_web_server_parse_accept_list ("de;q=xx, en-us;q=0.1,en;q=1,in;q=5", NULL);
  g_assert (result != NULL);

  string = g_strjoinv (", ", result);
  g_assert_cmpstr (string, ==, "in, en, en-us, en");

  g_free (string);
  g_strfreev (result);
}

static void
on_ready_get_result (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
  GAsyncResult **retval = user_data;
  g_assert (retval && *retval == NULL);
  *retval = g_object_ref (result);
}

static gchar *
perform_request (const gchar *hostport,
                 const gchar *request,
                 gsize *length,
                 gboolean tls)
{
  GSocketConnectable *connectable;
  GSocketClient *client;
  GSocketConnection *conn;
  GAsyncResult *result;
  GIOStream *tls_conn = NULL;
  GInputStream *input;
  GOutputStream *output;
  GError *error = NULL;
  GString *reply;
  gsize len;
  gssize ret;

  connectable = g_network_address_parse (hostport, 0, &error);
  g_assert_no_error (error);

  client = g_socket_client_new ();

  result = NULL;
  g_socket_client_connect_async (client, connectable, NULL, on_ready_get_result, &result);
  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);
  conn = g_socket_client_connect_finish (client, result, &error);
  g_object_unref (result);
  g_assert_no_error (error);

  if (tls)
    {
      tls_conn = g_tls_client_connection_new (G_IO_STREAM (conn), connectable, &error);
      g_tls_client_connection_set_validation_flags (G_TLS_CLIENT_CONNECTION (tls_conn), 0);
      output = g_io_stream_get_output_stream (G_IO_STREAM (tls_conn));
      input = g_io_stream_get_input_stream (G_IO_STREAM (tls_conn));
    }
  else
    {
      output = g_io_stream_get_output_stream (G_IO_STREAM (conn));
      input = g_io_stream_get_input_stream (G_IO_STREAM (conn));
    }

  result = NULL;
  g_output_stream_write_all_async (output, request, strlen (request), G_PRIORITY_DEFAULT, NULL,
                                   on_ready_get_result, &result);
  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);
  g_output_stream_write_all_finish (output, result, NULL, &error);
  g_object_unref (result);
  g_assert_no_error (error);

  if (tls)
    {
      g_output_stream_close (output, NULL, &error);
      g_assert_no_error (error);
    }

  g_socket_shutdown (g_socket_connection_get_socket (conn), FALSE, TRUE, &error);
  g_assert_no_error (error);

  reply = g_string_new ("");
  for (;;)
    {
      result = NULL;
      len = reply->len;
      g_string_set_size (reply, len + 1024);
      g_input_stream_read_async (input, reply->str + len, 1024, G_PRIORITY_DEFAULT,
                                 NULL, on_ready_get_result, &result);
      while (result == NULL)
        g_main_context_iteration (NULL, TRUE);
      ret = g_input_stream_read_finish (input, result, &error);
      g_object_unref (result);
      g_assert_no_error (error);
      g_assert (ret >= 0);
      g_string_set_size (reply, len + ret);
      if (ret == 0)
        break;
    }

  if (tls)
    g_object_unref (tls_conn);
  g_object_unref (conn);
  g_object_unref (client);
  g_object_unref (connectable);

  if (length)
    *length = reply->len;
  return g_string_free (reply, FALSE);
}

static gchar *
perform_http_request (const gchar *hostport,
                      const gchar *request,
                      gsize *length)
{
  return perform_request (hostport, request, length, FALSE);
}

static gchar *
perform_https_request (const gchar *hostport,
                       const gchar *request,
                       gsize *length)
{
  return perform_request (hostport, request, length, TRUE);
}

static gboolean
on_shell_index_html (CockpitWebServer *server,
                     const gchar *path,
                     GHashTable *headers,
                     CockpitWebResponse *response,
                     gpointer user_data)
{
  GBytes *bytes;
  const gchar *data;

  g_assert_cmpstr (path, ==, "/shell/index.html");
  data = "<!DOCTYPE html><html><body>index.html</body></html>";
  bytes = g_bytes_new_static (data, strlen (data));

  cockpit_web_response_content (response, NULL, bytes, NULL);
  g_bytes_unref (bytes);
  return TRUE;
}

static void
test_with_query_string (TestCase *tc,
                        gconstpointer user_data)
{
  gchar *resp;
  gsize length;

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_http_request (tc->localport, "GET /shell/index.html?blah HTTP/1.0\r\nHost:test\r\n\r\n", &length);
  g_assert (resp != NULL);
  g_assert_cmpuint (length, >, 0);

  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\nContent-Length: *\r\n\r\n<!DOCTYPE html>*");
  g_free (resp);
}

static void
test_webserver_not_found (TestCase *tc,
                          gconstpointer user_data)
{
  gchar *resp;
  gsize length;
  guint status;
  gssize off;

  resp = perform_http_request (tc->localport, "GET /non-existent HTTP/1.0\r\nHost:test\r\n\r\n", &length);
  g_assert (resp != NULL);
  g_assert_cmpuint (length, >, 0);

  off = web_socket_util_parse_status_line (resp, length, NULL, &status, NULL);
  g_assert_cmpuint (off, >, 0);
  g_assert_cmpint (status, ==, 404);

  g_free (resp);
}

static void
test_webserver_tls (TestCase *tc,
                    gconstpointer user_data)
{
  gchar *resp;
  gsize length;

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_https_request (tc->localport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", &length);
  g_assert (resp != NULL);
  g_assert_cmpuint (length, >, 0);

  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\nContent-Length: *\r\n\r\n<!DOCTYPE html>*");
  g_free (resp);
}

static gboolean
on_big_header (CockpitWebServer *server,
               const gchar *path,
               GHashTable *headers,
               CockpitWebResponse *response,
               gpointer user_data)
{
  GBytes *bytes;
  const gchar *big_header;

  big_header = g_hash_table_lookup (headers, "BigHeader");
  g_assert (big_header);
  g_assert_cmpint (strlen (big_header), ==, 7000);
  g_assert_cmpint (big_header[strlen (big_header) - 1], ==, '1');

  bytes = g_bytes_new_static ("OK", 2);
  cockpit_web_response_content (response, NULL, bytes, NULL);
  g_bytes_unref (bytes);
  return TRUE;
}

static void
test_webserver_tls_big_header (TestCase *tc,
                               gconstpointer user_data)
{
  g_autofree gchar *req = NULL;
  g_autofree gchar *resp = NULL;
  gsize length;

  /* max request size is 8KiB (2 * cockpit_webserver_request_maximum), stay slightly below that */
  req = g_strdup_printf ("GET /test HTTP/1.0\r\nHost:test\r\nBigHeader: %07000i\r\n\r\n", 1);

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_big_header), NULL);
  resp = perform_https_request (tc->localport, req, &length);
  g_assert (resp != NULL);
  g_assert_cmpuint (length, >, 0);

  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\nContent-Length: 2\r\n*\r\n\r\nOK");
}

static void
test_webserver_tls_request_too_large (TestCase *tc,
                                      gconstpointer user_data)
{
  g_autofree gchar *req = NULL;
  g_autofree gchar *resp = NULL;
  gsize length;

  /* request bigger than 16 KiB should be rejected */
  /* FIXME: This really should be 8 KiB, but due to pipelining we reserve twice
   * that amount in the buffer */
  cockpit_expect_log ("cockpit-protocol", G_LOG_LEVEL_MESSAGE, "received HTTP request that was too large");
  req = g_strdup_printf ("GET /test HTTP/1.0\r\nHost:test\r\nBigHeader: %016500i\r\n\r\n", 1);
  resp = perform_https_request (tc->localport, req, &length);
  g_assert (resp != NULL);
  g_assert_cmpuint (length, ==, 0);
  g_assert_cmpstr (resp, ==, "");
}

static const TestFixture fixture_with_cert = {
    .cert_file = SRCDIR "/src/ws/mock-combined.crt"
};

static const TestFixture fixture_with_cert_redirect = {
    .server_flags = COCKPIT_WEB_SERVER_REDIRECT_TLS,
    .cert_file = SRCDIR "/src/ws/mock-combined.crt"
};

static void
test_webserver_redirect_notls (TestCase *tc,
                               gconstpointer data)
{
  gchar *resp;

  SKIP_NO_HOSTPORT;

  g_assert (cockpit_web_server_get_flags (tc->web_server) == COCKPIT_WEB_SERVER_REDIRECT_TLS);

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_http_request (tc->hostport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  cockpit_assert_strmatch (resp, "HTTP/* 301 *\r\nLocation: https://*");
  g_free (resp);
}

static void
test_webserver_noredirect_localhost (TestCase *tc,
                                     gconstpointer data)
{
  gchar *resp;

  g_assert (cockpit_web_server_get_flags (tc->web_server) == COCKPIT_WEB_SERVER_REDIRECT_TLS);

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_http_request (tc->localport, "GET /shell/index.html HTTP/1.0\r\nHost: localhost\r\n\r\n", NULL);
  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\n*");
  g_free (resp);
}

static void
test_webserver_noredirect_exception (TestCase *tc,
                                     gconstpointer data)
{
  gchar *resp;

  SKIP_NO_HOSTPORT;

  g_object_set (tc->web_server, "ssl-exception-prefix", "/shell", NULL);
  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_http_request (tc->hostport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\n*");
  g_free (resp);
}

static void
test_webserver_noredirect_override (TestCase *tc,
                                    gconstpointer data)
{
  gchar *resp;

  SKIP_NO_HOSTPORT;

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_http_request (tc->hostport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\n*");
  g_free (resp);
}

static gboolean
on_oh_resource (CockpitWebServer *server,
                const gchar *path,
                GHashTable *headers,
                CockpitWebResponse *response,
                const gchar **invoked)
{
  gchar *data;
  GBytes *bytes;

  g_assert (*invoked == NULL);
  *invoked = "oh";

  data = g_strdup_printf ("Scruffy says: %s", path);
  bytes = g_bytes_new_take (data, strlen (data));
  cockpit_web_response_content (response, NULL, bytes, NULL);
  g_bytes_unref (bytes);
  return TRUE;
}

static gboolean
on_scruffy_resource (CockpitWebServer *server,
                     const gchar *path,
                     GHashTable *headers,
                     CockpitWebResponse *response,
                     const gchar **invoked)
{
  const gchar *data;
  GBytes *bytes;

  g_assert (*invoked == NULL);
  *invoked = "scruffy";

  data = "Scruffy is here";
  bytes = g_bytes_new_static (data, strlen (data));
  cockpit_web_response_content (response, NULL, bytes, NULL);
  g_bytes_unref (bytes);
  return TRUE;
}

static gboolean
on_index_resource (CockpitWebServer *server,
                   const gchar *path,
                   GHashTable *headers,
                   CockpitWebResponse *response,
                   const gchar **invoked)
{
  const gchar *data;
  GBytes *bytes;

  g_assert (*invoked == NULL);
  *invoked = "index";

  data = "Yello from index";
  bytes = g_bytes_new_static (data, strlen (data));
  cockpit_web_response_content (response, NULL, bytes, NULL);
  g_bytes_unref (bytes);
  return TRUE;
}

static gboolean
on_default_resource (CockpitWebServer *server,
                     const gchar *path,
                     GHashTable *headers,
                     CockpitWebResponse *response,
                     const gchar **invoked)
{
  GBytes *bytes;

  g_assert (*invoked == NULL);
  *invoked = "default";

  bytes = g_bytes_new_static ("default", 7);
  cockpit_web_response_content (response, NULL, bytes, NULL);
  g_bytes_unref (bytes);
  return TRUE;
}


static void
test_handle_resource (TestCase *tc,
                      gconstpointer data)
{
  const gchar *invoked = NULL;
  gchar *resp;

  g_signal_connect (tc->web_server, "handle-resource::/oh/",
                    G_CALLBACK (on_oh_resource), &invoked);
  g_signal_connect (tc->web_server, "handle-resource::/scruffy",
                    G_CALLBACK (on_scruffy_resource), &invoked);
  g_signal_connect (tc->web_server, "handle-resource::/",
                    G_CALLBACK (on_index_resource), &invoked);
  g_signal_connect (tc->web_server, "handle-resource",
                    G_CALLBACK (on_default_resource), &invoked);

  /* Should call the /oh/ handler */
  resp = perform_http_request (tc->localport, "GET /oh/marmalade HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "oh");
  invoked = NULL;
  cockpit_assert_strmatch (resp, "*Scruffy says: /oh/marmalade");
  g_free (resp);

  /* Should call the /oh/ handler */
  resp = perform_http_request (tc->localport, "GET /oh/ HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "oh");
  cockpit_assert_strmatch (resp, "*Scruffy says: /oh/");
  invoked = NULL;
  g_free (resp);

  /* Should call the default handler */
  g_free (perform_http_request (tc->localport, "GET /oh HTTP/1.0\r\nHost:test\r\n\r\n", NULL));
  g_assert_cmpstr (invoked, ==, "default");
  invoked = NULL;

  /* Should call the scruffy handler */
  resp = perform_http_request (tc->localport, "GET /scruffy HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "scruffy");
  invoked = NULL;
  cockpit_assert_strmatch (resp, "*Scruffy is here");
  g_free (resp);

  /* Should call the default handler */
  g_free (perform_http_request (tc->localport, "GET /scruffy/blah HTTP/1.0\r\nHost:test\r\n\r\n", NULL));
  g_assert_cmpstr (invoked, ==, "default");
  invoked = NULL;

  /* Should call the index handler */
  resp = perform_http_request (tc->localport, "GET / HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "index");
  invoked = NULL;
  cockpit_assert_strmatch (resp, "*Yello from index");
  g_free (resp);

  /* Should call the default handler */
  g_free (perform_http_request (tc->localport, "GET /oooo HTTP/1.0\r\nHost:test\r\n\r\n", NULL));
  g_assert_cmpstr (invoked, ==, "default");
  invoked = NULL;
}

static void
test_webserver_host_header (TestCase *tc,
                            gconstpointer data)
{
  gsize length;
  guint status;
  gssize off;
  gchar *resp;

  cockpit_expect_log ("cockpit-protocol", G_LOG_LEVEL_MESSAGE, "received HTTP request without Host header");
  resp = perform_http_request (tc->localport, "GET /index.html HTTP/1.0\r\n\r\n", &length);
  g_assert (resp != NULL);
  g_assert_cmpuint (length, >, 0);

  off = web_socket_util_parse_status_line (resp, length, NULL, &status, NULL);
  g_assert_cmpuint (off, >, 0);
  g_assert_cmpint (status, ==, 400);

  g_free (resp);
}

static void
test_url_root (TestCase *tc,
                 gconstpointer unused)
{
  gchar *url_root = NULL;

  g_object_get (tc->web_server, "url-root", &url_root, NULL);
  g_assert (url_root == NULL);

  g_object_set (tc->web_server, "url-root", "/", NULL);
  g_object_get (tc->web_server, "url-root", &url_root, NULL);
  g_assert (url_root == NULL);

  g_object_set (tc->web_server, "url-root", "/path/", NULL);
  g_object_get (tc->web_server, "url-root", &url_root, NULL);
  g_assert_cmpstr (url_root, ==, "/path");
  g_free (url_root);
  url_root = NULL;

  g_object_set (tc->web_server, "url-root", "//path//", NULL);
  g_object_get (tc->web_server, "url-root", &url_root, NULL);
  g_assert_cmpstr (url_root, ==, "/path");
  g_free (url_root);
  url_root = NULL;

  g_object_set (tc->web_server, "url-root", "path/", NULL);
  g_object_get (tc->web_server, "url-root", &url_root, NULL);
  g_assert_cmpstr (url_root, ==, "/path");
  g_free (url_root);
  url_root = NULL;

  g_object_set (tc->web_server, "url-root", "path", NULL);
  g_object_get (tc->web_server, "url-root", &url_root, NULL);
  g_assert_cmpstr (url_root, ==, "/path");
  g_free (url_root);
  url_root = NULL;
}


static void
test_handle_resource_url_root (TestCase *tc,
                                 gconstpointer unused)
{
  const gchar *invoked = NULL;
  gchar *resp;

  g_object_set (tc->web_server, "url-root", "/path/", NULL);

  g_signal_connect (tc->web_server, "handle-resource::/oh/",
                    G_CALLBACK (on_oh_resource), &invoked);
  g_signal_connect (tc->web_server, "handle-resource::/scruffy",
                    G_CALLBACK (on_scruffy_resource), &invoked);
  g_signal_connect (tc->web_server, "handle-resource::/",
                    G_CALLBACK (on_index_resource), &invoked);
  g_signal_connect (tc->web_server, "handle-resource",
                    G_CALLBACK (on_default_resource), &invoked);

  /* Should call the /oh/ handler */
  resp = perform_http_request (tc->localport, "GET /path/oh/marmalade HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "oh");
  invoked = NULL;
  cockpit_assert_strmatch (resp, "*Scruffy says: /oh/marmalade");
  g_free (resp);

  /* Should call the /oh/ handler */
  resp = perform_http_request (tc->localport, "GET /path/oh/ HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "oh");
  cockpit_assert_strmatch (resp, "*Scruffy says: /oh/");
  invoked = NULL;
  g_free (resp);

  /* Should call the default handler */
  g_free (perform_http_request (tc->localport, "GET /path/oh HTTP/1.0\r\nHost:test\r\n\r\n", NULL));
  g_assert_cmpstr (invoked, ==, "default");
  invoked = NULL;

  /* Should call the scruffy handler */
  resp = perform_http_request (tc->localport, "GET /path/scruffy HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "scruffy");
  invoked = NULL;
  cockpit_assert_strmatch (resp, "*Scruffy is here");
  g_free (resp);

  /* Should call the default handler */
  g_free (perform_http_request (tc->localport, "GET /path/scruffy/blah HTTP/1.0\r\nHost:test\r\n\r\n", NULL));
  g_assert_cmpstr (invoked, ==, "default");
  invoked = NULL;

  /* Should call the index handler */
  resp = perform_http_request (tc->localport, "GET /path/ HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  g_assert_cmpstr (invoked, ==, "index");
  invoked = NULL;
  cockpit_assert_strmatch (resp, "*Yello from index");
  g_free (resp);

  /* Should call the default handler */
  g_free (perform_http_request (tc->localport, "GET /path/oooo HTTP/1.0\r\nHost:test\r\n\r\n", NULL));
  g_assert_cmpstr (invoked, ==, "default");
  invoked = NULL;

  /* Should fail */
  if (tc->hostport)
    {
      resp = perform_http_request (tc->hostport, "GET /oooo HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
      cockpit_assert_strmatch (resp, "HTTP/* 404 *\r\n");
      g_free (resp);
      g_assert (invoked == NULL);
    }
}

static void
assert_cannot_connect (const gchar *hostport)
{
  GSocketClient *client;
  GSocketConnection *conn;
  GAsyncResult *result;
  GError *error = NULL;

  client = g_socket_client_new ();

  result = NULL;
  g_socket_client_connect_to_host_async (client, hostport, 1, NULL, on_ready_get_result, &result);
  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);
  conn = g_socket_client_connect_to_host_finish (client, result, &error);
  g_object_unref (result);
  g_assert_null (conn);
  g_assert_error (error,  G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED);
  g_clear_error (&error);
  g_object_unref (client);
}

static void
test_address (TestCase *tc,
              gconstpointer data)
{
  gchar *resp = NULL;
  const TestFixture *fix = data;

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  if (fix->local_only)
    {
      resp = perform_http_request (tc->localport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
      cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\n*");
      g_free (resp);
      resp = NULL;
    }
  else
    {
      /* If there is only one interface, then cockpit_web_server_new will get a NULL address and thus do listen on loopback */
      if (tc->hostport)
        assert_cannot_connect (tc->localport);
    }

  if (tc->hostport)
    {
      if (fix->inet_only)
        {
          resp = perform_http_request (tc->hostport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
          cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\n*");
          g_free (resp);
          resp = NULL;
        }
      else
        {
          assert_cannot_connect (tc->hostport);
        }
    }
}

static void
test_bad_address (TestCase *tc,
                  gconstpointer unused)
{
  gint port;

  g_autoptr(CockpitWebServer) server = cockpit_web_server_new (NULL, COCKPIT_WEB_SERVER_NONE);
  g_autoptr(GError) error = NULL;
  port = cockpit_web_server_add_inet_listener (server, "bad", 0, &error);
  g_assert (port == 0);
  cockpit_assert_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Couldn't parse IP address from `bad`");
}

static const TestFixture fixture_for_tls_proxy = {
    .local_only = TRUE,
    .server_flags = COCKPIT_WEB_SERVER_FOR_TLS_PROXY,
};

static void
test_webserver_for_tls_proxy (TestCase *tc,
                              gconstpointer data)
{
  gchar *resp;

  g_assert (cockpit_web_server_get_flags (tc->web_server) == COCKPIT_WEB_SERVER_FOR_TLS_PROXY);

  g_signal_connect (tc->web_server, "handle-resource", G_CALLBACK (on_shell_index_html), NULL);
  resp = perform_http_request (tc->localport, "GET /shell/index.html HTTP/1.0\r\nHost:test\r\n\r\n", NULL);
  cockpit_assert_strmatch (resp, "HTTP/* 200 *\r\n*");
  g_free (resp);
}

static const TestFixture fixture_inet_address = {
    .inet_only = TRUE
};

static const TestFixture fixture_local_address = {
    .local_only = TRUE
};

int
main (int argc,
      char *argv[])
{
  cockpit_setenv_check ("GSETTINGS_BACKEND", "memory", TRUE);
  cockpit_setenv_check ("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  cockpit_setenv_check ("GIO_USE_VFS", "local", TRUE);

  cockpit_test_init (&argc, &argv);

  g_test_add_func ("/web-server/table", test_table);

  g_test_add_func ("/web-server/cookie/simple", test_cookie_simple);
  g_test_add_func ("/web-server/cookie/multiple", test_cookie_multiple);
  g_test_add_func ("/web-server/cookie/overlap", test_cookie_overlap);
  g_test_add_func ("/web-server/cookie/no-header", test_cookie_no_header);
  g_test_add_func ("/web-server/cookie/substring", test_cookie_substring);
  g_test_add_func ("/web-server/cookie/decode", test_cookie_decode);
  g_test_add_func ("/web-server/cookie/decode-bad", test_cookie_decode_bad);

  g_test_add_func ("/web-server/accept-list/simple", test_accept_list_simple);
  g_test_add_func ("/web-server/accept-listlanguages/cookie", test_accept_list_cookie);
  g_test_add_func ("/web-server/accept-list/no-header", test_accept_list_no_header);
  g_test_add_func ("/web-server/accept-list/order", test_accept_list_order);

  g_test_add ("/web-server/query-string", TestCase, NULL,
              setup, test_with_query_string, teardown);
  g_test_add ("/web-server/host-header", TestCase, NULL,
              setup, test_webserver_host_header, teardown);
  g_test_add ("/web-server/not-found", TestCase, NULL,
              setup, test_webserver_not_found, teardown);
  g_test_add ("/web-server/tls", TestCase, &fixture_with_cert,
              setup, test_webserver_tls, teardown);
  g_test_add ("/web-server/tls-big-header", TestCase, &fixture_with_cert,
              setup, test_webserver_tls_big_header, teardown);
  g_test_add ("/web-server/tls-request-too-large", TestCase, &fixture_with_cert,
              setup, test_webserver_tls_request_too_large, teardown);

  g_test_add ("/web-server/redirect-notls", TestCase, &fixture_with_cert_redirect,
              setup, test_webserver_redirect_notls, teardown);
  g_test_add ("/web-server/no-redirect-localhost", TestCase, &fixture_with_cert_redirect,
              setup, test_webserver_noredirect_localhost, teardown);
  g_test_add ("/web-server/no-redirect-exception", TestCase, &fixture_with_cert_redirect,
              setup, test_webserver_noredirect_exception, teardown);
  g_test_add ("/web-server/no-redirect-override", TestCase, &fixture_with_cert,
              setup, test_webserver_noredirect_override, teardown);

  g_test_add ("/web-server/handle-resource", TestCase, NULL,
              setup, test_handle_resource, teardown);

  g_test_add ("/web-server/url-root", TestCase, NULL,
              setup, test_url_root, teardown);
  g_test_add ("/web-server/url-root-handlers", TestCase, NULL,
              setup, test_handle_resource_url_root, teardown);

  g_test_add ("/web-server/local-address-only", TestCase, &fixture_local_address,
              setup, test_address, teardown);
  g_test_add ("/web-server/inet-address-only", TestCase, &fixture_inet_address,
              setup, test_address, teardown);
  g_test_add ("/web-server/bad-address", TestCase, NULL,
              NULL, test_bad_address, NULL);

  g_test_add ("/web-server/for-tls-proxy", TestCase, &fixture_for_tls_proxy,
              setup, test_webserver_for_tls_proxy, teardown);

  return g_test_run ();
}
