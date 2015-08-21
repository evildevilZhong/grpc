/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/security/secure_endpoint.h"
#include "src/core/support/string.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/slice_buffer.h>
#include <grpc/support/slice.h>
#include <grpc/support/sync.h>
#include "src/core/tsi/transport_security_interface.h"
#include "src/core/debug/trace.h"

#define STAGING_BUFFER_SIZE 8192

typedef struct {
  grpc_endpoint base;
  grpc_endpoint *wrapped_ep;
  struct tsi_frame_protector *protector;
  gpr_mu protector_mu;
  /* saved upper level callbacks and user_data. */
  grpc_iomgr_closure *read_cb;
  grpc_iomgr_closure *write_cb;
  grpc_iomgr_closure on_read;
  gpr_slice_buffer *read_buffer;
  gpr_slice_buffer source_buffer;
  /* saved handshaker leftover data to unprotect. */
  gpr_slice_buffer leftover_bytes;
  /* buffers for read and write */
  gpr_slice read_staging_buffer;

  gpr_slice write_staging_buffer;
  gpr_slice_buffer output_buffer;

  gpr_refcount ref;
} secure_endpoint;

int grpc_trace_secure_endpoint = 0;

static void destroy(secure_endpoint *secure_ep) {
  secure_endpoint *ep = secure_ep;
  grpc_endpoint_destroy(ep->wrapped_ep);
  tsi_frame_protector_destroy(ep->protector);
  gpr_slice_buffer_destroy(&ep->leftover_bytes);
  gpr_slice_unref(ep->read_staging_buffer);
  gpr_slice_unref(ep->write_staging_buffer);
  gpr_slice_buffer_destroy(&ep->output_buffer);
  gpr_slice_buffer_destroy(&ep->source_buffer);
  gpr_mu_destroy(&ep->protector_mu);
  gpr_free(ep);
}

#define GRPC_SECURE_ENDPOINT_REFCOUNT_DEBUG
#define SECURE_ENDPOINT_UNREF(ep, reason) \
  secure_endpoint_unref((ep), (reason), __FILE__, __LINE__)
#define SECURE_ENDPOINT_REF(ep, reason) \
  secure_endpoint_ref((ep), (reason), __FILE__, __LINE__)
static void secure_endpoint_unref(secure_endpoint *ep, const char *reason,
                                  const char *file, int line) {
  gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG, "SECENDP unref %p : %s %d -> %d",
          ep, reason, ep->ref.count, ep->ref.count - 1);
  if (gpr_unref(&ep->ref)) {
    destroy(ep);
  }
}

static void secure_endpoint_ref(secure_endpoint *ep, const char *reason,
                                const char *file, int line) {
  gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG, "SECENDP   ref %p : %s %d -> %d",
          ep, reason, ep->ref.count, ep->ref.count + 1);
  gpr_ref(&ep->ref);
}
#ifdef GRPC_SECURE_ENDPOINT_REFCOUNT_DEBUG
#else
#define SECURE_ENDPOINT_UNREF(ep, reason) secure_endpoint_unref((ep))
#define SECURE_ENDPOINT_REF(ep, reason) secure_endpoint_ref((ep))
static void secure_endpoint_unref(secure_endpoint *ep) {
  if (gpr_unref(&ep->ref)) {
    destroy(ep);
  }
}

static void secure_endpoint_ref(secure_endpoint *ep) { gpr_ref(&ep->ref); }
#endif

static void flush_read_staging_buffer(secure_endpoint *ep, gpr_uint8 **cur,
                                      gpr_uint8 **end) {
  gpr_slice_buffer_add(ep->read_buffer, ep->read_staging_buffer);
  ep->read_staging_buffer = gpr_slice_malloc(STAGING_BUFFER_SIZE);
  *cur = GPR_SLICE_START_PTR(ep->read_staging_buffer);
  *end = GPR_SLICE_END_PTR(ep->read_staging_buffer);
}

static void call_read_cb(secure_endpoint *ep, int success) {
  if (grpc_trace_secure_endpoint) {
    size_t i;
    for (i = 0; i < ep->read_buffer->count; i++) {
      char *data = gpr_dump_slice(ep->read_buffer->slices[i],
                                  GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_DEBUG, "READ %p: %s", ep, data);
      gpr_free(data);
    }
  }
  ep->read_buffer = NULL;
  ep->read_cb->cb(ep->read_cb->cb_arg, success);
  SECURE_ENDPOINT_UNREF(ep, "read");
}

static int on_read(void *user_data, int success) {
  unsigned i;
  gpr_uint8 keep_looping = 0;
  tsi_result result = TSI_OK;
  secure_endpoint *ep = (secure_endpoint *)user_data;
  gpr_uint8 *cur = GPR_SLICE_START_PTR(ep->read_staging_buffer);
  gpr_uint8 *end = GPR_SLICE_END_PTR(ep->read_staging_buffer);

  if (!success) {
    gpr_slice_buffer_reset_and_unref(ep->read_buffer);
    return 0;
  }

  /* TODO(yangg) check error, maybe bail out early */
  for (i = 0; i < ep->source_buffer.count; i++) {
    gpr_slice encrypted = ep->source_buffer.slices[i];
    gpr_uint8 *message_bytes = GPR_SLICE_START_PTR(encrypted);
    size_t message_size = GPR_SLICE_LENGTH(encrypted);

    while (message_size > 0 || keep_looping) {
      size_t unprotected_buffer_size_written = (size_t)(end - cur);
      size_t processed_message_size = message_size;
      gpr_mu_lock(&ep->protector_mu);
      result = tsi_frame_protector_unprotect(ep->protector, message_bytes,
                                             &processed_message_size, cur,
                                             &unprotected_buffer_size_written);
      gpr_mu_unlock(&ep->protector_mu);
      if (result != TSI_OK) {
        gpr_log(GPR_ERROR, "Decryption error: %s",
                tsi_result_to_string(result));
        break;
      }
      message_bytes += processed_message_size;
      message_size -= processed_message_size;
      cur += unprotected_buffer_size_written;

      if (cur == end) {
        flush_read_staging_buffer(ep, &cur, &end);
        /* Force to enter the loop again to extract buffered bytes in protector.
           The bytes could be buffered because of running out of staging_buffer.
           If this happens at the end of all slices, doing another unprotect
           avoids leaving data in the protector. */
        keep_looping = 1;
      } else if (unprotected_buffer_size_written > 0) {
        keep_looping = 1;
      } else {
        keep_looping = 0;
      }
    }
    if (result != TSI_OK) break;
  }

  if (cur != GPR_SLICE_START_PTR(ep->read_staging_buffer)) {
    gpr_slice_buffer_add(
        ep->read_buffer,
        gpr_slice_split_head(
            &ep->read_staging_buffer,
            (size_t)(cur - GPR_SLICE_START_PTR(ep->read_staging_buffer))));
  }

  /* TODO(yangg) experiment with moving this block after read_cb to see if it
     helps latency */
  gpr_slice_buffer_reset_and_unref(&ep->source_buffer);

  if (result != TSI_OK) {
    gpr_slice_buffer_reset_and_unref(ep->read_buffer);
    return 0;
  }

  return 1;
}

static void on_read_cb(void *user_data, int success) {
  call_read_cb(user_data, on_read(user_data, success));
}

static grpc_endpoint_op_status endpoint_read(grpc_endpoint *secure_ep,
                                             gpr_slice_buffer *slices,
                                             grpc_iomgr_closure *cb) {
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  int immediate_read_success = -1;
  ep->read_cb = cb;
  ep->read_buffer = slices;
  gpr_slice_buffer_reset_and_unref(ep->read_buffer);

  if (ep->leftover_bytes.count) {
    gpr_slice_buffer_swap(&ep->leftover_bytes, &ep->source_buffer);
    GPR_ASSERT(ep->leftover_bytes.count == 0);
    return on_read(ep, 1) ? GRPC_ENDPOINT_DONE : GRPC_ENDPOINT_ERROR;
  }

  SECURE_ENDPOINT_REF(ep, "read");

  switch (
      grpc_endpoint_read(ep->wrapped_ep, &ep->source_buffer, &ep->on_read)) {
    case GRPC_ENDPOINT_DONE:
      immediate_read_success = on_read(ep, 1);
      break;
    case GRPC_ENDPOINT_PENDING:
      return GRPC_ENDPOINT_PENDING;
    case GRPC_ENDPOINT_ERROR:
      immediate_read_success = on_read(ep, 0);
      break;
  }

  GPR_ASSERT(immediate_read_success != -1);
  SECURE_ENDPOINT_UNREF(ep, "read");

  return immediate_read_success ? GRPC_ENDPOINT_DONE : GRPC_ENDPOINT_ERROR;
}

static void flush_write_staging_buffer(secure_endpoint *ep, gpr_uint8 **cur,
                                       gpr_uint8 **end) {
  gpr_slice_buffer_add(&ep->output_buffer, ep->write_staging_buffer);
  ep->write_staging_buffer = gpr_slice_malloc(STAGING_BUFFER_SIZE);
  *cur = GPR_SLICE_START_PTR(ep->write_staging_buffer);
  *end = GPR_SLICE_END_PTR(ep->write_staging_buffer);
}

static grpc_endpoint_op_status endpoint_write(grpc_endpoint *secure_ep,
                                              gpr_slice_buffer *slices,
                                              grpc_iomgr_closure *cb) {
  unsigned i;
  tsi_result result = TSI_OK;
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  gpr_uint8 *cur = GPR_SLICE_START_PTR(ep->write_staging_buffer);
  gpr_uint8 *end = GPR_SLICE_END_PTR(ep->write_staging_buffer);

  gpr_slice_buffer_reset_and_unref(&ep->output_buffer);

  if (grpc_trace_secure_endpoint) {
    for (i = 0; i < slices->count; i++) {
      char *data =
          gpr_dump_slice(slices->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_DEBUG, "WRITE %p: %s", ep, data);
      gpr_free(data);
    }
  }

  for (i = 0; i < slices->count; i++) {
    gpr_slice plain = slices->slices[i];
    gpr_uint8 *message_bytes = GPR_SLICE_START_PTR(plain);
    size_t message_size = GPR_SLICE_LENGTH(plain);
    while (message_size > 0) {
      size_t protected_buffer_size_to_send = (size_t)(end - cur);
      size_t processed_message_size = message_size;
      gpr_mu_lock(&ep->protector_mu);
      result = tsi_frame_protector_protect(ep->protector, message_bytes,
                                           &processed_message_size, cur,
                                           &protected_buffer_size_to_send);
      gpr_mu_unlock(&ep->protector_mu);
      if (result != TSI_OK) {
        gpr_log(GPR_ERROR, "Encryption error: %s",
                tsi_result_to_string(result));
        break;
      }
      message_bytes += processed_message_size;
      message_size -= processed_message_size;
      cur += protected_buffer_size_to_send;

      if (cur == end) {
        flush_write_staging_buffer(ep, &cur, &end);
      }
    }
    if (result != TSI_OK) break;
  }
  if (result == TSI_OK) {
    size_t still_pending_size;
    do {
      size_t protected_buffer_size_to_send = (size_t)(end - cur);
      gpr_mu_lock(&ep->protector_mu);
      result = tsi_frame_protector_protect_flush(ep->protector, cur,
                                                 &protected_buffer_size_to_send,
                                                 &still_pending_size);
      gpr_mu_unlock(&ep->protector_mu);
      if (result != TSI_OK) break;
      cur += protected_buffer_size_to_send;
      if (cur == end) {
        flush_write_staging_buffer(ep, &cur, &end);
      }
    } while (still_pending_size > 0);
    if (cur != GPR_SLICE_START_PTR(ep->write_staging_buffer)) {
      gpr_slice_buffer_add(
          &ep->output_buffer,
          gpr_slice_split_head(
              &ep->write_staging_buffer,
              (size_t)(cur - GPR_SLICE_START_PTR(ep->write_staging_buffer))));
    }
  }

  if (result != TSI_OK) {
    /* TODO(yangg) do different things according to the error type? */
    gpr_slice_buffer_reset_and_unref(&ep->output_buffer);
    return GRPC_ENDPOINT_ERROR;
  }

  return grpc_endpoint_write(ep->wrapped_ep, &ep->output_buffer, cb);
}

static void endpoint_shutdown(grpc_endpoint *secure_ep) {
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  grpc_endpoint_shutdown(ep->wrapped_ep);
}

static void endpoint_destroy(grpc_endpoint *secure_ep) {
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  SECURE_ENDPOINT_UNREF(ep, "destroy");
}

static void endpoint_add_to_pollset(grpc_endpoint *secure_ep,
                                    grpc_pollset *pollset) {
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  grpc_endpoint_add_to_pollset(ep->wrapped_ep, pollset);
}

static void endpoint_add_to_pollset_set(grpc_endpoint *secure_ep,
                                        grpc_pollset_set *pollset_set) {
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  grpc_endpoint_add_to_pollset_set(ep->wrapped_ep, pollset_set);
}

static char *endpoint_get_peer(grpc_endpoint *secure_ep) {
  secure_endpoint *ep = (secure_endpoint *)secure_ep;
  return grpc_endpoint_get_peer(ep->wrapped_ep);
}

static const grpc_endpoint_vtable vtable = {
    endpoint_read,           endpoint_write,
    endpoint_add_to_pollset, endpoint_add_to_pollset_set,
    endpoint_shutdown,       endpoint_destroy,
    endpoint_get_peer};

grpc_endpoint *grpc_secure_endpoint_create(
    struct tsi_frame_protector *protector, grpc_endpoint *transport,
    gpr_slice *leftover_slices, size_t leftover_nslices) {
  size_t i;
  secure_endpoint *ep = (secure_endpoint *)gpr_malloc(sizeof(secure_endpoint));
  ep->base.vtable = &vtable;
  ep->wrapped_ep = transport;
  ep->protector = protector;
  gpr_slice_buffer_init(&ep->leftover_bytes);
  for (i = 0; i < leftover_nslices; i++) {
    gpr_slice_buffer_add(&ep->leftover_bytes,
                         gpr_slice_ref(leftover_slices[i]));
  }
  ep->write_staging_buffer = gpr_slice_malloc(STAGING_BUFFER_SIZE);
  ep->read_staging_buffer = gpr_slice_malloc(STAGING_BUFFER_SIZE);
  gpr_slice_buffer_init(&ep->output_buffer);
  gpr_slice_buffer_init(&ep->source_buffer);
  ep->read_buffer = NULL;
  grpc_iomgr_closure_init(&ep->on_read, on_read_cb, ep);
  gpr_mu_init(&ep->protector_mu);
  gpr_ref_init(&ep->ref, 1);
  return &ep->base;
}
