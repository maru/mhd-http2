/*
  This file is part of libmicrohttpd
  Copyright (C) 2007-2017 Daniel Pittman and Christian Grothoff
  Copyright (C) 2018 Maru Berezin

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

/**
 * @file microhttpd/http2/h2_connection.c
 * @brief Methods for managing HTTP/2 connections
 * @author Maru Berezin
 */

#include "connection.h"
#include "memorypool.h"
#include "response.h"
#include "mhd_str.h"
#include "http2/h2.h"
#include "http2/h2_internal.h"

#define H2_MAGIC_TOKEN "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_MAGIC_TOKEN_LEN_MIN 16
#define H2_MAGIC_TOKEN_LEN 24

/* ================================================================ */
/*                          HTTP2 MHD API                           */
/* ================================================================ */

/**
 * Read data from the connection.
 *
 * @param connection connection to handle
 */
void
h2_connection_handle_read (struct MHD_Connection *connection)
{
  ssize_t bytes_read;

  if ( (MHD_CONNECTION_CLOSED == connection->state) ||
       (connection->suspended) )
    return;

#ifdef HTTPS_SUPPORT
  if (MHD_TLS_CONN_NO_TLS != connection->tls_state)
    { /* HTTPS connection. */
      mhd_assert (MHD_TLS_CONN_CONNECTED <= connection->tls_state);
    }
#endif /* HTTPS_SUPPORT */

  /* make sure "read" has a reasonable number of bytes
     in buffer to use per system call (if possible) */
  if (connection->read_buffer_offset + connection->daemon->pool_increment >
      connection->read_buffer_size)
    try_grow_read_buffer (connection);

  if (connection->read_buffer_size == connection->read_buffer_offset)
    return; /* No space for receiving data. */

  struct h2_session_t *h2 = connection->h2;
  if (NULL == h2) return; // MHD_NO;
  h2_debug_vprintf ("[id=%zu]", h2->session_id);

  bytes_read = connection->recv_cls (connection,
                                     &connection->read_buffer
                                     [connection->read_buffer_offset],
                                     connection->read_buffer_size -
                                     connection->read_buffer_offset);

  h2_debug_vprintf ("read %zd / %zu", bytes_read, connection->read_buffer_size);

  if (bytes_read < 0)
    {
      if (MHD_ERR_AGAIN_ == bytes_read)
          return; /* No new data to process. */
      if (MHD_ERR_CONNRESET_ == bytes_read)
        {
           connection_close_error (connection,
                                   _("Socket is unexpectedly disconnected when reading request.\n"));
           return;
        }
      connection_close_error (connection,
                                _("Connection socket is closed due to error when reading request.\n"));
      return;
    }

  if (0 == bytes_read)
    { /* Remote side closed connection. */
      connection->read_closed = true;
      MHD_connection_close_ (connection,
                             MHD_REQUEST_TERMINATED_CLIENT_ABORT);
      return;
    }
  connection->read_buffer_offset += bytes_read;
  MHD_update_last_activity_ (connection);

  mhd_assert(connection->read_closed == 0);
}


/**
 * Write data to the connection.
 *
 * @param connection connection to handle
 */
void
h2_connection_handle_write (struct MHD_Connection *connection)
{
  struct h2_session_t *h2 = connection->h2;
  if (NULL == h2) return; // MHD_NO;
  h2_debug_vprintf ("[id=%zu]", h2->session_id);

  h2_debug_vprintf ("write_buffer send=%d append=%d = %d", connection->write_buffer_send_offset, connection->write_buffer_append_offset, connection->write_buffer_append_offset - connection->write_buffer_send_offset);
  if (connection->write_buffer_append_offset - connection->write_buffer_send_offset > 0)
    {
      ssize_t ret;
      ret = connection->send_cls (connection,
                                  &connection->write_buffer
                                  [connection->write_buffer_send_offset],
                                  connection->write_buffer_append_offset -
                                  connection->write_buffer_send_offset);
      h2_debug_vprintf ("send_cls ret=%ld", ret);
      if (ret < 0)
        {
          if (MHD_ERR_AGAIN_ == ret)
            {
              /* TODO: Transmission could not be accomplished. Try again. */
              h2_debug_vprintf (" =================== ADD WRITE EVENT ================== ret=%zd", ret);
              return; // MHD_YES;
            }
          MHD_connection_close_ (connection, MHD_REQUEST_TERMINATED_WITH_ERROR);
          return; // MHD_NO;
        }
      connection->write_buffer_send_offset += ret;

      if (connection->write_buffer_append_offset - connection->write_buffer_send_offset == 0)
        {
          /* Reset offsets */
          connection->write_buffer_append_offset = 0;
          connection->write_buffer_send_offset = 0;
        }
    }

  /* Fill write buffer */
  if (h2_fill_write_buffer (h2->session, h2) != 0)
    {
      MHD_connection_close_ (connection, MHD_REQUEST_TERMINATED_WITH_ERROR);
      return;
    }

  h2_debug_vprintf ("h2_fill_write_buffer: send=%d append=%d = %d", connection->write_buffer_send_offset, connection->write_buffer_append_offset, connection->write_buffer_append_offset - connection->write_buffer_send_offset);

  /* More to write */
  if (connection->write_buffer_append_offset - connection->write_buffer_send_offset != 0)
    {
      /* TODO: Add new write event */
      h2_debug_vprintf (" =================== ADD WRITE EVENT2 ==================");
    }

  h2_debug_vprintf ("want_read %d want_write %d append-send %d", nghttp2_session_want_read (h2->session),
                        nghttp2_session_want_write (h2->session),
                        connection->write_buffer_append_offset - connection->write_buffer_send_offset);
  if ( (nghttp2_session_want_read (h2->session) == 0) &&
       (nghttp2_session_want_write (h2->session) == 0) &&
       (connection->write_buffer_append_offset - connection->write_buffer_send_offset == 0) )
    {
      MHD_connection_close_ (connection, MHD_REQUEST_TERMINATED_COMPLETED_OK);
      return;
    }
  else
    {
      // MHD_connection_close_ (connection, MHD_REQUEST_TERMINATED_WITH_ERROR);
    }
  MHD_update_last_activity_ (connection);
  connection->event_loop_info = MHD_EVENT_LOOP_INFO_READ;
#ifdef EPOLL_SUPPORT
  MHD_connection_epoll_update_ (connection);
#endif /* EPOLL_SUPPORT */
}


/**
 * Process data.
 *
 * @param connection connection to handle
 * @return #MHD_YES if no error
 *         #MHD_NO otherwise, connection must be closed.
 */
int
h2_connection_handle_idle (struct MHD_Connection *connection)
{
  struct h2_session_t *h2 = connection->h2;
  connection->in_idle = true;

  if ((connection->state == MHD_CONNECTION_CLOSED) || (NULL == h2))
    {
      cleanup_connection (connection);
      connection->in_idle = false;
      return MHD_NO;
    }

  h2_debug_vprintf ("[id=%zu]", h2->session_id, connection->read_buffer_start_offset);

  ssize_t bytes_read = connection->read_buffer_offset - connection->read_buffer_start_offset;
  ssize_t rv;
  rv = nghttp2_session_mem_recv (h2->session, &connection->read_buffer[connection->read_buffer_start_offset], bytes_read);
  if (rv < 0)
    {
      if (rv != NGHTTP2_ERR_BAD_CLIENT_MAGIC)
        {
          h2_debug_vprintf("nghttp2_session_mem_recv () returned error: %s %zd", nghttp2_strerror (rv), rv);
        }
      /* Should send a GOAWAY frame with last stream_id successfully received */
      nghttp2_submit_goaway(h2->session, NGHTTP2_FLAG_NONE, h2->accepted_max,
                            NGHTTP2_PROTOCOL_ERROR, NULL, 0);
      nghttp2_session_send(h2->session);
      MHD_connection_close_ (connection, MHD_REQUEST_TERMINATED_WITH_ERROR);
      return MHD_NO;
    }
  else
  {
    h2_debug_vprintf ("nghttp2_session_mem_recv: %d/%d", rv, bytes_read);
    MHD_update_last_activity_ (connection);

    /* Update read_buffer offsets */
    connection->read_buffer_start_offset += rv;
    if (connection->read_buffer_offset == connection->read_buffer_start_offset)
      {
        connection->read_buffer_offset = 0;
        connection->read_buffer_start_offset = 0;
      }

    connection->event_loop_info = MHD_EVENT_LOOP_INFO_WRITE;
#ifdef EPOLL_SUPPORT
    MHD_connection_epoll_update_ (connection);
#endif /* EPOLL_SUPPORT */
  }

  if (connection->write_buffer_append_offset - connection->write_buffer_send_offset != 0)
    {
      connection->event_loop_info = MHD_EVENT_LOOP_INFO_WRITE;
#ifdef EPOLL_SUPPORT
      MHD_connection_epoll_update_ (connection);
#endif /* EPOLL_SUPPORT */
    }

  return MHD_YES;

//   /* TODO: resume all deferred streams */
//   if (h2 && h2->deferred_stream > 0)
//   {
//     nghttp2_session_resume_data(h2->session, h2->deferred_stream);
//     struct h2_stream_t *stream;
//     stream = nghttp2_session_get_stream_user_data (h2->session, h2->deferred_stream);
//     if (NULL == stream)
//       return 0;
//     size_t unused = 0;
//     return h2_call_connection_handler (connection, stream, NULL, &unused);
//   }
}


void
h2_stream_resume (struct MHD_Connection *connection)
{
}


/**
 * Suspend handling of network data for the current stream.
 * @param connection connection to handle
 */
void
h2_stream_suspend (struct MHD_Connection *connection)
{
}

/**
 * Queue a response to be transmitted to the client (as soon as
 * possible but after #MHD_AccessHandlerCallback returns).
 *
 * @param connection the connection identifying the client
 * @param status_code HTTP status code (i.e. #MHD_HTTP_OK)
 * @param response response to transmit
 * @return #MHD_NO on error (i.e. reply already sent),
 *         #MHD_YES on success or if message has been queued
 * @ingroup response
 */
int
h2_queue_response (struct MHD_Connection *connection,
                   unsigned int status_code,
                   struct MHD_Response *response)
{
  struct h2_session_t *h2 = connection->h2;
  struct h2_stream_t *stream;

  mhd_assert (h2 != NULL);
  h2_debug_vprintf ("[id=%zu]", connection->h2->session_id);

  stream = nghttp2_session_get_stream_user_data (h2->session, h2->current_stream_id);
  if (NULL == stream)
    {
      return MHD_NO;
    }

  MHD_increment_response_rc (response);
  stream->response = response;
  stream->response_code = status_code;

  if ( ( (NULL != stream->method) &&
         (MHD_str_equal_caseless_ (stream->method,
                                   MHD_HTTP_METHOD_HEAD)) ) ||
       (MHD_HTTP_OK > status_code) ||
       (MHD_HTTP_NO_CONTENT == status_code) ||
       (MHD_HTTP_NOT_MODIFIED == status_code) )
    {
      /* if this is a "HEAD" request, or a status code for
         which a body is not allowed, pretend that we
         have already sent the full message body. */
      stream->response_write_position = response->total_size;
    }

  int r = h2_session_build_stream_headers (h2, stream, response);
  if (r != 0)
    {
      return MHD_NO;
    }

  connection->event_loop_info = MHD_EVENT_LOOP_INFO_WRITE;
#ifdef EPOLL_SUPPORT
  MHD_connection_epoll_update_ (connection);
#endif /* EPOLL_SUPPORT */
  return MHD_YES;
}

void
h2_connection_close (struct MHD_Connection *connection)
{
  h2_session_destroy (connection->h2);
  connection->h2 = NULL;

  connection->state = MHD_CONNECTION_CLOSED;
  MHD_pool_destroy (connection->pool);
  connection->pool = NULL;
  connection->read_buffer = NULL;
  connection->read_buffer_size = 0;
  connection->write_buffer = NULL;
  connection->write_buffer_size = 0;
}

/**
 * Set HTTP/1 read/idle/write callbacks for this connection.
 * Handle data from/to socket.
 *
 * @param connection connection to initialize
 */
void
h2_set_h1_callbacks (struct MHD_Connection *connection)
{
  connection->version = MHD_HTTP_VERSION_1_1;
  connection->http_version = HTTP_VERSION(1, 1);

  connection->handle_read_cls = &MHD_connection_handle_read;
  connection->handle_idle_cls = &MHD_connection_handle_idle;
  connection->handle_write_cls = &MHD_connection_handle_write;
}


/**
 * Set HTTP/2 read/idle/write callbacks for this connection.
 * Handle data from/to socket.
 * Create HTTP/2 session.
 *
 * @param connection connection to initialize
 */
void
h2_set_h2_callbacks (struct MHD_Connection *connection)
{
  mhd_assert (MHD_TLS_CONN_CONNECTED == connection->tls_state);
  connection->version = MHD_HTTP_VERSION_2_0;
  connection->http_version = HTTP_VERSION(2, 0);
  connection->keepalive = MHD_CONN_USE_KEEPALIVE;
  // connection->state = MHD_CONNECTION_HTTP2_INIT;

  connection->handle_read_cls = &h2_connection_handle_read;
  connection->handle_idle_cls = &h2_connection_handle_idle;
  connection->handle_write_cls = &h2_connection_handle_write;

  mhd_assert (NULL == connection->h2);
  connection->h2 = h2_session_create (connection);
  if (NULL == connection->h2)
    {
      /* Error, close connection */
      MHD_connection_close_ (connection,
                             MHD_REQUEST_TERMINATED_WITH_ERROR);
    }

  /* Send preface */
  connection->event_loop_info = MHD_EVENT_LOOP_INFO_WRITE;
#ifdef EPOLL_SUPPORT
  MHD_connection_epoll_update_ (connection);
#endif /* EPOLL_SUPPORT */
}


/**
 * Check if first bytes are the h2 preface.
 * If the buffer has at least H2_MAGIC_TOKEN_LEN bytes, check full preface.
 * Otherwise, just check the first H2_MAGIC_TOKEN_LEN_MIN bytes, because
 * MHD_connection_handle_idle will find the first "\r\n" and believe it is an
 * HTTP/1 request.
 *
 * @param connection connection
 * @return #MHD_YES for success, #MHD_NO for failure
 */
int
h2_is_h2_preface (struct MHD_Connection *connection)
{
  if (connection->read_buffer_offset >= H2_MAGIC_TOKEN_LEN)
    {
      return (!memcmp(H2_MAGIC_TOKEN, connection->read_buffer, H2_MAGIC_TOKEN_LEN));
    }
  else if (connection->read_buffer_offset >= H2_MAGIC_TOKEN_LEN_MIN)
    {
      return (!memcmp(H2_MAGIC_TOKEN, connection->read_buffer, H2_MAGIC_TOKEN_LEN_MIN));
    }
  return MHD_NO;
}

/* end of h2_connection.c */
