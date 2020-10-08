/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2020, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

#include "curl_setup.h"

#if !defined(CURL_DISABLE_HTTP) && defined(USE_HYPER)

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#include <net/if.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#include <hyper.h>
#include "urldata.h"
#include "sendf.h"
#include "transfer.h"

static size_t read_cb(void *userp, hyper_context *ctx,
                      uint8_t *buf, size_t buflen)
{
  struct connectdata *conn = (struct connectdata *)userp;
  struct Curl_easy *data = conn->data;
  CURLcode result;
  ssize_t nread;

  (void)ctx;

  result = Curl_read(conn, conn->sockfd, (char *)buf, buflen, &nread);
  if(result == CURLE_AGAIN) {
    /* would block, register interest */
    if(data->hyp.read_waker)
      hyper_waker_free(data->hyp.read_waker);
    data->hyp.read_waker = hyper_context_waker(ctx);
    if(!data->hyp.read_waker) {
      failf(data, "Couldn't make the read hyper_context_waker");
      return HYPER_IO_ERROR;
    }
    return HYPER_IO_PENDING;
  }
  DEBUGF(infof(data, "Hyper: READ %u bytes\n", (int)nread));
  return (size_t)nread;
}

static size_t write_cb(void *userp, hyper_context *ctx,
                       const uint8_t *buf, size_t buflen)
{
  struct connectdata *conn = (struct connectdata *)userp;
  struct Curl_easy *data = conn->data;
  CURLcode result;
  ssize_t nwrote;

  result = Curl_write(conn, conn->sockfd, (void *)buf, buflen, &nwrote);
  if(result == CURLE_AGAIN) {
    /* would block, register interest */
    if(data->hyp.write_waker)
      hyper_waker_free(data->hyp.write_waker);
    data->hyp.write_waker = hyper_context_waker(ctx);
    if(!data->hyp.write_waker) {
      failf(data, "Couldn't make the write hyper_context_waker");
      return HYPER_IO_ERROR;
    }
    return HYPER_IO_PENDING;
  }
  else if(result)
    return HYPER_IO_ERROR;
  DEBUGF(infof(data, "Hyper: WROTE %u bytes\n", (int)nwrote));
  return (size_t)nwrote;
}

/*
 * Curl_http() gets called from the generic multi_do() function when a HTTP
 * request is to be performed. This creates and sends a properly constructed
 * HTTP request.
 */
CURLcode Curl_http(struct connectdata *conn, bool *done)
{
  struct Curl_easy *data = conn->data;
  struct hyptransfer *h = &data->hyp;
  hyper_io *io = NULL;
  hyper_clientconn_options *options = NULL;
  hyper_task *htask = NULL; /* for the handshake */
  hyper_task *sendtask = NULL; /* for the send */
  hyper_clientconn *client = NULL;
  hyper_request *req = NULL;
  hyper_headers *headers = NULL;

  /* Always consider the DO phase done after this function call, even if there
     may be parts of the request that is not yet sent, since we can deal with
     the rest of the request in the PERFORM phase. */
  *done = TRUE;

  infof(data, "Time for the Hyper dance\n");

  io = hyper_io_new();
  if(!io) {
    failf(data, "Couldn't create hyper IO");
    goto error;
  }
  /* tell Hyper how to read/write network data */
  hyper_io_set_userdata(io, conn);
  hyper_io_set_read(io, read_cb);
  hyper_io_set_write(io, write_cb);

  /* create an executor to poll futures */
  if(!h->exec) {
    h->exec = hyper_executor_new();
    if(!h->exec) {
      failf(data, "Couldn't create hyper executor");
      goto error;
    }
  }

  options = hyper_clientconn_options_new();
  if(!options) {
    failf(data, "Couldn't create hyper client options");
    goto error;
  }
  hyper_clientconn_options_exec(options, h->exec);

  if(!h->handshake) {
    /* "Both the `io` and the `options` are consumed in this function call" */
    h->handshake = hyper_clientconn_handshake(io, options);
    if(!h->handshake) {
      failf(data, "Couldn't create hyper client handshake");
      goto error;
    }
    io = NULL;
    options = NULL;
  }

  if(HYPERE_OK != hyper_executor_push(h->exec, h->handshake)) {
    failf(data, "Couldn't hyper_executor_push the handshake");
    goto error;
  }

  htask = hyper_executor_poll(h->exec);
  if(!htask) {
    failf(data, "Couldn't hyper_executor_poll the handshake");
    goto error;
  }

  client = hyper_task_value(htask);
  hyper_task_free(htask);

  req = hyper_request_new();
  if(!req) {
    failf(data, "Couldn't hyper_request_new");
    goto error;
  }
  /* lame fixed method for early testing */
  if(hyper_request_set_method(req, (uint8_t *)"GET", 3)) {
    failf(data, "error setting method\n");
    goto error;
  }
  if(hyper_request_set_uri(req, (uint8_t *)data->state.up.path,
                           strlen(data->state.up.path))) {
    failf(data, "error setting path\n");
    goto error;
  }

  headers = hyper_request_headers(req);
  if(!headers) {
    failf(data, "hyper_request_headers\n");
    goto error;
  }
  /* **WRONG** Host: header logic */
  if(HYPERE_OK != hyper_headers_add(headers, (uint8_t *)"Host", 4,
                                    (uint8_t *)conn->host.name,
                                    strlen(conn->host.name))) {
    failf(data, "hyper_headers_add\n");
    goto error;
  }

  sendtask = hyper_clientconn_send(client, req);
  if(!sendtask) {
    failf(data, "hyper_clientconn_send\n");
    goto error;
  }

  if(HYPERE_OK != hyper_executor_push(h->exec, sendtask)) {
    failf(data, "Couldn't hyper_executor_push the send");
    goto error;
  }

  hyper_clientconn_free(client);

  /* kick off the request */
  (void)hyper_executor_poll(h->exec);
  return CURLE_OK;
  error:

  /*** Lack of cleanup functions: LEAKS MEMORY ON ERRORS ***/

  if(io)
    hyper_io_free(io);

  /*
  if(options)
    hyper_clientconn_options_free(options);
  */
  if(h->handshake)
    hyper_task_free(h->handshake);

  return CURLE_OUT_OF_MEMORY;
}

#endif /* !defined(CURL_DISABLE_HTTP) && defined(USE_HYPER) */
