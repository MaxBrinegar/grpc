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

#ifndef GRPCXX_IMPL_CALL_H
#define GRPCXX_IMPL_CALL_H

#include <grpc/grpc.h>
#include <grpc++/client_context.h>
#include <grpc++/completion_queue.h>
#include <grpc++/config.h>
#include <grpc++/status.h>
#include <grpc++/impl/serialization_traits.h>

#include <memory>
#include <map>

struct grpc_call;
struct grpc_op;

namespace grpc {

class ByteBuffer;
class Call;

void FillMetadataMap(grpc_metadata_array* arr,
                     std::multimap<grpc::string, grpc::string>* metadata);
grpc_metadata* FillMetadataArray(
    const std::multimap<grpc::string, grpc::string>& metadata);

class CallNoOp {
 protected:
  void AddOp(grpc_op* ops, size_t* nops) {}
  void FinishOp(void* tag, bool* status, int max_message_size) {}
};

class CallOpSendInitialMetadata {
 public:
  CallOpSendInitialMetadata() : send_(false) {}

  void SendInitialMetadata(
      const std::multimap<grpc::string, grpc::string>& metadata) {
    send_ = true;
    initial_metadata_count_ = metadata.size();
    initial_metadata_ = FillMetadataArray(metadata);
  }

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (!send_) return;
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_SEND_INITIAL_METADATA;
    op->data.send_initial_metadata.count = initial_metadata_count_;
    op->data.send_initial_metadata.metadata = initial_metadata_;
  }
  void FinishOp(void* tag, bool* status, int max_message_size) {
    // nothing to do
  }

  bool send_;
  size_t initial_metadata_count_;
  grpc_metadata* initial_metadata_;
};

class CallOpSendMessage {
 public:
  CallOpSendMessage() : send_buf_(nullptr) {}

  template <class M>
  bool SendMessage(const M& message) GRPC_MUST_USE_RESULT {
    return SerializationTraits<M>::Serialize(message, &send_buf_);
  }

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (send_buf_ == nullptr) return;
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_SEND_MESSAGE;
    op->data.send_message = send_buf_;
  }
  void FinishOp(void* tag, bool* status, int max_message_size) {
    grpc_byte_buffer_destroy(send_buf_);
  }

 private:
  grpc_byte_buffer* send_buf_;
};

template <class R>
class CallOpRecvMessage {
 public:
  CallOpRecvMessage() : got_message(false), message_(nullptr) {}

  void RecvMessage(R* message) { message_ = message; }

  bool got_message;

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (message_ == nullptr) return;
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_RECV_MESSAGE;
    op->data.recv_message = &recv_buf_;
  }

  void FinishOp(void* tag, bool* status, int max_message_size) {
    if (message_ == nullptr) return;
    if (recv_buf_) {
      if (*status) {
        got_message = true;
        *status = SerializationTraits<R>::Deserialize(recv_buf_, message_,
                                                      max_message_size)
                      .IsOk();
      } else {
        got_message = false;
        grpc_byte_buffer_destroy(recv_buf_);
      }
    } else {
      got_message = false;
      *status = false;
    }
  }

 private:
  R* message_;
  grpc_byte_buffer* recv_buf_;
};

class CallOpGenericRecvMessage {
 public:
  CallOpGenericRecvMessage() : got_message(false) {}

  template <class R>
  void RecvMessage(R* message) {
    deserialize_ = [message](grpc_byte_buffer* buf,
                             int max_message_size) -> Status {
      return SerializationTraits<R>::Deserialize(buf, message,
                                                 max_message_size);
    };
  }

  bool got_message;

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (!deserialize_) return;
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_RECV_MESSAGE;
    op->data.recv_message = &recv_buf_;
  }

  void FinishOp(void* tag, bool* status, int max_message_size) {
    if (!deserialize_) return;
    if (recv_buf_) {
      if (*status) {
        got_message = true;
        *status = deserialize_(recv_buf_, max_message_size).IsOk();
      } else {
        got_message = false;
        grpc_byte_buffer_destroy(recv_buf_);
      }
    } else {
      got_message = false;
      *status = false;
    }
  }

 private:
  std::function<Status(grpc_byte_buffer*, int)> deserialize_;
  grpc_byte_buffer* recv_buf_;
};

class CallOpClientSendClose {
 public:
  CallOpClientSendClose() : send_(false) {}

  void ClientSendClose() { send_ = true; }

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (!send_) return;
    ops[(*nops)++].op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  }
  void FinishOp(void* tag, bool* status, int max_message_size) {
    // nothing to do
  }

 private:
  bool send_;
};

class CallOpServerSendStatus {
 public:
  CallOpServerSendStatus() : send_status_available_(false) {}

  void ServerSendStatus(
      const std::multimap<grpc::string, grpc::string>& trailing_metadata,
      const Status& status) {
    trailing_metadata_count_ = trailing_metadata.size();
    trailing_metadata_ = FillMetadataArray(trailing_metadata);
    send_status_available_ = true;
    send_status_code_ = static_cast<grpc_status_code>(status.code());
    send_status_details_ = status.details();
  }

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
    op->data.send_status_from_server.trailing_metadata_count =
        trailing_metadata_count_;
    op->data.send_status_from_server.trailing_metadata = trailing_metadata_;
    op->data.send_status_from_server.status = send_status_code_;
    op->data.send_status_from_server.status_details =
        send_status_details_.empty() ? nullptr : send_status_details_.c_str();
  }

  void FinishOp(void* tag, bool* status, int max_message_size) {
    // nothing to do
  }

 private:
  bool send_status_available_;
  grpc_status_code send_status_code_;
  grpc::string send_status_details_;
  size_t trailing_metadata_count_;
  grpc_metadata* trailing_metadata_;
};

class CallOpRecvInitialMetadata {
 public:
  CallOpRecvInitialMetadata() : recv_initial_metadata_(nullptr) {
    memset(&recv_initial_metadata_arr_, 0, sizeof(recv_initial_metadata_arr_));
  }

  void RecvInitialMetadata(ClientContext* context) {
    context->initial_metadata_received_ = true;
    recv_initial_metadata_ = &context->recv_initial_metadata_;
  }

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (!recv_initial_metadata_) return;
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_RECV_INITIAL_METADATA;
    op->data.recv_initial_metadata = &recv_initial_metadata_arr_;
  }
  void FinishOp(void* tag, bool* status, int max_message_size) {
    FillMetadataMap(&recv_initial_metadata_arr_, recv_initial_metadata_);
  }

 private:
  std::multimap<grpc::string, grpc::string>* recv_initial_metadata_;
  grpc_metadata_array recv_initial_metadata_arr_;
};

class CallOpClientRecvStatus {
 public:
  CallOpClientRecvStatus() { memset(this, 0, sizeof(*this)); }

  void ClientRecvStatus(ClientContext* context, Status* status) {
    recv_trailing_metadata_ = &context->trailing_metadata_;
    recv_status_ = status;
  }

 protected:
  void AddOp(grpc_op* ops, size_t* nops) {
    if (recv_status_ == nullptr) return;
    grpc_op* op = &ops[(*nops)++];
    op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
    op->data.recv_status_on_client.trailing_metadata =
        &recv_trailing_metadata_arr_;
    op->data.recv_status_on_client.status = &status_code_;
    op->data.recv_status_on_client.status_details = &status_details_;
    op->data.recv_status_on_client.status_details_capacity =
        &status_details_capacity_;
  }

  void FinishOp(void* tag, bool* status, int max_message_size) {
    FillMetadataMap(&recv_trailing_metadata_arr_, recv_trailing_metadata_);
    *recv_status_ = Status(
        static_cast<StatusCode>(status_code_),
        status_details_ ? grpc::string(status_details_) : grpc::string());
  }

 private:
  std::multimap<grpc::string, grpc::string>* recv_trailing_metadata_;
  Status* recv_status_;
  grpc_metadata_array recv_trailing_metadata_arr_;
  grpc_status_code status_code_;
  char* status_details_;
  size_t status_details_capacity_;
};

class CallOpSetInterface : public CompletionQueueTag {
 public:
  CallOpSetInterface() : max_message_size_(0) {}
  virtual void FillOps(grpc_op* ops, size_t* nops) = 0;

  void set_max_message_size(int max_message_size) {
    max_message_size_ = max_message_size;
  }

 protected:
  int max_message_size_;
};

template <class T, int I>
class WrapAndDerive : public T {};

template <class Op1 = CallNoOp, class Op2 = CallNoOp, class Op3 = CallNoOp,
          class Op4 = CallNoOp, class Op5 = CallNoOp, class Op6 = CallNoOp>
class CallOpSet : public CallOpSetInterface,
                  public WrapAndDerive<Op1, 1>,
                  public WrapAndDerive<Op2, 2>,
                  public WrapAndDerive<Op3, 3>,
                  public WrapAndDerive<Op4, 4>,
                  public WrapAndDerive<Op5, 5>,
                  public WrapAndDerive<Op6, 6> {
 public:
  CallOpSet() : return_tag_(this) {}
  void FillOps(grpc_op* ops, size_t* nops) GRPC_OVERRIDE {
    this->WrapAndDerive<Op1, 1>::AddOp(ops, nops);
    this->WrapAndDerive<Op2, 2>::AddOp(ops, nops);
    this->WrapAndDerive<Op3, 3>::AddOp(ops, nops);
    this->WrapAndDerive<Op4, 4>::AddOp(ops, nops);
    this->WrapAndDerive<Op5, 5>::AddOp(ops, nops);
    this->WrapAndDerive<Op6, 6>::AddOp(ops, nops);
  }

  bool FinalizeResult(void** tag, bool* status) GRPC_OVERRIDE {
    this->WrapAndDerive<Op1, 1>::FinishOp(*tag, status, max_message_size_);
    this->WrapAndDerive<Op2, 2>::FinishOp(*tag, status, max_message_size_);
    this->WrapAndDerive<Op3, 3>::FinishOp(*tag, status, max_message_size_);
    this->WrapAndDerive<Op4, 4>::FinishOp(*tag, status, max_message_size_);
    this->WrapAndDerive<Op5, 5>::FinishOp(*tag, status, max_message_size_);
    this->WrapAndDerive<Op6, 6>::FinishOp(*tag, status, max_message_size_);
    *tag = return_tag_;
    return true;
  }

  void set_output_tag(void* return_tag) { return_tag_ = return_tag; }

 private:
  void* return_tag_;
};

#if 0
class CallOpBuffer : public CompletionQueueTag {
 public:
  CallOpBuffer();
  ~CallOpBuffer();

  void Reset(void* next_return_tag);

  // Does not take ownership.
  void AddSendInitialMetadata(
      std::multimap<grpc::string, grpc::string>* metadata);
  void AddSendInitialMetadata(ClientContext* ctx);
  void AddRecvInitialMetadata(ClientContext* ctx);
  void AddSendMessage(const grpc::protobuf::Message& message);
  void AddSendMessage(const ByteBuffer& message);
  void AddRecvMessage(grpc::protobuf::Message* message);
  void AddRecvMessage(ByteBuffer* message);
  void AddClientSendClose();
  void AddClientRecvStatus(ClientContext* ctx, Status* status);
  void AddServerSendStatus(std::multimap<grpc::string, grpc::string>* metadata,
                           const Status& status);
  void AddServerRecvClose(bool* cancelled);

  // INTERNAL API:

  // Convert to an array of grpc_op elements
  void FillOps(grpc_op* ops, size_t* nops);

  // Called by completion queue just prior to returning from Next() or Pluck()
  bool FinalizeResult(void** tag, bool* status) GRPC_OVERRIDE;

  void set_max_message_size(int max_message_size) {
    max_message_size_ = max_message_size;
  }

  bool got_message;

 private:
  void* return_tag_;
  // Send initial metadata
  bool send_initial_metadata_;
  size_t initial_metadata_count_;
  grpc_metadata* initial_metadata_;
  // Recv initial metadta
  std::multimap<grpc::string, grpc::string>* recv_initial_metadata_;
  grpc_metadata_array recv_initial_metadata_arr_;
  // Send message
  const grpc::protobuf::Message* send_message_;
  const ByteBuffer* send_message_buffer_;
  grpc_byte_buffer* send_buf_;
  // Recv message
  grpc::protobuf::Message* recv_message_;
  ByteBuffer* recv_message_buffer_;
  grpc_byte_buffer* recv_buf_;
  int max_message_size_;
  // Client send close
  bool client_send_close_;
  // Client recv status
  std::multimap<grpc::string, grpc::string>* recv_trailing_metadata_;
  Status* recv_status_;
  grpc_metadata_array recv_trailing_metadata_arr_;
  grpc_status_code status_code_;
  char* status_details_;
  size_t status_details_capacity_;
  // Server send status
  bool send_status_available_;
  grpc_status_code send_status_code_;
  grpc::string send_status_details_;
  size_t trailing_metadata_count_;
  grpc_metadata* trailing_metadata_;
  int cancelled_buf_;
  bool* recv_closed_;
};
#endif

// SneakyCallOpBuffer does not post completions to the completion queue
template <class Op1 = CallNoOp, class Op2 = CallNoOp, class Op3 = CallNoOp,
          class Op4 = CallNoOp, class Op5 = CallNoOp, class Op6 = CallNoOp>
class SneakyCallOpSet GRPC_FINAL
    : public CallOpSet<Op1, Op2, Op3, Op4, Op5, Op6> {
 public:
  bool FinalizeResult(void** tag, bool* status) GRPC_OVERRIDE {
    return CallOpSet<Op1, Op2, Op3, Op4, Op5, Op6>::FinalizeResult(tag,
                                                                   status) &&
           false;
  }
};

// Channel and Server implement this to allow them to hook performing ops
class CallHook {
 public:
  virtual ~CallHook() {}
  virtual void PerformOpsOnCall(CallOpSetInterface* ops, Call* call) = 0;
};

// Straightforward wrapping of the C call object
class Call GRPC_FINAL {
 public:
  /* call is owned by the caller */
  Call(grpc_call* call, CallHook* call_hook_, CompletionQueue* cq);
  Call(grpc_call* call, CallHook* call_hook_, CompletionQueue* cq,
       int max_message_size);

  void PerformOps(CallOpSetInterface* ops);

  grpc_call* call() { return call_; }
  CompletionQueue* cq() { return cq_; }

  int max_message_size() { return max_message_size_; }

 private:
  CallHook* call_hook_;
  CompletionQueue* cq_;
  grpc_call* call_;
  int max_message_size_;
};

}  // namespace grpc

#endif  // GRPCXX_IMPL_CALL_H
