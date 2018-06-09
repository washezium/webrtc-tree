/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include <algorithm>

#include "p2p/base/relayport.h"
#include "rtc_base/asyncpacketsocket.h"
#include "rtc_base/checks.h"
#include "rtc_base/helpers.h"
#include "rtc_base/logging.h"

namespace cricket {

static const int kMessageConnectTimeout = 1;
static const int kKeepAliveDelay           = 10 * 60 * 1000;
static const int kRetryTimeout             = 50 * 1000;  // ICE says 50 secs
// How long to wait for a socket to connect to remote host in milliseconds
// before trying another connection.
static const int kSoftConnectTimeoutMs     = 3 * 1000;

// Handles a connection to one address/port/protocol combination for a
// particular RelayEntry.
class RelayConnection : public sigslot::has_slots<> {
 public:
  RelayConnection(const ProtocolAddress* protocol_address,
                  rtc::AsyncPacketSocket* socket,
                  rtc::Thread* thread);
  ~RelayConnection() override;
  rtc::AsyncPacketSocket* socket() const { return socket_; }

  const ProtocolAddress* protocol_address() {
    return protocol_address_;
  }

  rtc::SocketAddress GetAddress() const {
    return protocol_address_->address;
  }

  ProtocolType GetProtocol() const {
    return protocol_address_->proto;
  }

  int SetSocketOption(rtc::Socket::Option opt, int value);

  // Validates a response to a STUN allocate request.
  bool CheckResponse(StunMessage* msg);

  // Sends data to the relay server.
  int Send(const void* pv, size_t cb, const rtc::PacketOptions& options);

  // Sends a STUN allocate request message to the relay server.
  void SendAllocateRequest(RelayEntry* entry, int delay);

  // Return the latest error generated by the socket.
  int GetError() { return socket_->GetError(); }

  // Called on behalf of a StunRequest to write data to the socket.  This is
  // already STUN intended for the server, so no wrapping is necessary.
  void OnSendPacket(const void* data, size_t size, StunRequest* req);

 private:
  rtc::AsyncPacketSocket* socket_;
  const ProtocolAddress* protocol_address_;
  StunRequestManager *request_manager_;
};

// Manages a number of connections to the relayserver, one for each
// available protocol. We aim to use each connection for only a
// specific destination address so that we can avoid wrapping every
// packet in a STUN send / data indication.
class RelayEntry : public rtc::MessageHandler,
                   public sigslot::has_slots<> {
 public:
  RelayEntry(RelayPort* port, const rtc::SocketAddress& ext_addr);
  ~RelayEntry() override;

  RelayPort* port() { return port_; }

  const rtc::SocketAddress& address() const { return ext_addr_; }
  void set_address(const rtc::SocketAddress& addr) { ext_addr_ = addr; }

  bool connected() const { return connected_; }
  bool locked() const { return locked_; }

  // Returns the last error on the socket of this entry.
  int GetError();

  // Returns the most preferred connection of the given
  // ones. Connections are rated based on protocol in the order of:
  // UDP, TCP and SSLTCP, where UDP is the most preferred protocol
  static RelayConnection* GetBestConnection(RelayConnection* conn1,
                                            RelayConnection* conn2);

  // Sends the STUN requests to the server to initiate this connection.
  void Connect();

  // Called when this entry becomes connected.  The address given is the one
  // exposed to the outside world on the relay server.
  void OnConnect(const rtc::SocketAddress& mapped_addr,
                 RelayConnection* socket);

  // Sends a packet to the given destination address using the socket of this
  // entry.  This will wrap the packet in STUN if necessary.
  int SendTo(const void* data, size_t size,
             const rtc::SocketAddress& addr,
             const rtc::PacketOptions& options);

  // Schedules a keep-alive allocate request.
  void ScheduleKeepAlive();

  void SetServerIndex(size_t sindex) { server_index_ = sindex; }

  // Sets this option on the socket of each connection.
  int SetSocketOption(rtc::Socket::Option opt, int value);

  size_t ServerIndex() const { return server_index_; }

  // Try a different server address
  void HandleConnectFailure(rtc::AsyncPacketSocket* socket);

  // Implementation of the MessageHandler Interface.
  void OnMessage(rtc::Message* pmsg) override;

 private:
  RelayPort* port_;
  rtc::SocketAddress ext_addr_;
  size_t server_index_;
  bool connected_;
  bool locked_;
  RelayConnection* current_connection_;

  // Called when a TCP connection is established or fails
  void OnSocketConnect(rtc::AsyncPacketSocket* socket);
  void OnSocketClose(rtc::AsyncPacketSocket* socket, int error);

  // Called when a packet is received on this socket.
  void OnReadPacket(
    rtc::AsyncPacketSocket* socket,
    const char* data, size_t size,
    const rtc::SocketAddress& remote_addr,
    const rtc::PacketTime& packet_time);

  void OnSentPacket(rtc::AsyncPacketSocket* socket,
                    const rtc::SentPacket& sent_packet);

  // Called when the socket is currently able to send.
  void OnReadyToSend(rtc::AsyncPacketSocket* socket);

  // Sends the given data on the socket to the server with no wrapping.  This
  // returns the number of bytes written or -1 if an error occurred.
  int SendPacket(const void* data, size_t size,
                 const rtc::PacketOptions& options);
};

// Handles an allocate request for a particular RelayEntry.
class AllocateRequest : public StunRequest {
 public:
  AllocateRequest(RelayEntry* entry, RelayConnection* connection);
  ~AllocateRequest() override = default;

  void Prepare(StunMessage* request) override;

  void OnSent() override;
  int resend_delay() override;

  void OnResponse(StunMessage* response) override;
  void OnErrorResponse(StunMessage* response) override;
  void OnTimeout() override;

 private:
  RelayEntry* entry_;
  RelayConnection* connection_;
  int64_t start_time_;
};

RelayPort::RelayPort(rtc::Thread* thread,
                     rtc::PacketSocketFactory* factory,
                     rtc::Network* network,
                     uint16_t min_port,
                     uint16_t max_port,
                     const std::string& username,
                     const std::string& password)
    : Port(thread,
           RELAY_PORT_TYPE,
           factory,
           network,
           min_port,
           max_port,
           username,
           password),
      ready_(false),
      error_(0) {
  entries_.push_back(
      new RelayEntry(this, rtc::SocketAddress()));
  // TODO(?): set local preference value for TCP based candidates.
}

RelayPort::~RelayPort() {
  for (size_t i = 0; i < entries_.size(); ++i)
    delete entries_[i];
  thread()->Clear(this);
}

void RelayPort::AddServerAddress(const ProtocolAddress& addr) {
  // Since HTTP proxies usually only allow 443,
  // let's up the priority on PROTO_SSLTCP
  if (addr.proto == PROTO_SSLTCP &&
      (proxy().type == rtc::PROXY_HTTPS ||
       proxy().type == rtc::PROXY_UNKNOWN)) {
    server_addr_.push_front(addr);
  } else {
    server_addr_.push_back(addr);
  }
}

void RelayPort::AddExternalAddress(const ProtocolAddress& addr) {
  std::string proto_name = ProtoToString(addr.proto);
  for (std::vector<ProtocolAddress>::iterator it = external_addr_.begin();
       it != external_addr_.end(); ++it) {
    if ((it->address == addr.address) && (it->proto == addr.proto)) {
      RTC_LOG(INFO) << "Redundant relay address: " << proto_name << " @ "
                    << addr.address.ToSensitiveString();
      return;
    }
  }
  external_addr_.push_back(addr);
}

void RelayPort::SetReady() {
  if (!ready_) {
    std::vector<ProtocolAddress>::iterator iter;
    for (iter = external_addr_.begin();
         iter != external_addr_.end(); ++iter) {
      std::string proto_name = ProtoToString(iter->proto);
      // In case of Gturn, related address is set to null socket address.
      // This is due to as mapped address stun attribute is used for allocated
      // address.
      AddAddress(iter->address, iter->address, rtc::SocketAddress(), proto_name,
                 proto_name, "", RELAY_PORT_TYPE, ICE_TYPE_PREFERENCE_RELAY_UDP,
                 0, "", false);
    }
    ready_ = true;
    SignalPortComplete(this);
  }
}

const ProtocolAddress * RelayPort::ServerAddress(size_t index) const {
  if (index < server_addr_.size())
    return &server_addr_[index];
  return NULL;
}

bool RelayPort::HasMagicCookie(const char* data, size_t size) {
  if (size < 24 + sizeof(TURN_MAGIC_COOKIE_VALUE)) {
    return false;
  } else {
    return memcmp(data + 24,
                  TURN_MAGIC_COOKIE_VALUE,
                  sizeof(TURN_MAGIC_COOKIE_VALUE)) == 0;
  }
}

void RelayPort::PrepareAddress() {
  // We initiate a connect on the first entry.  If this completes, it will fill
  // in the server address as the address of this port.
  RTC_DCHECK(entries_.size() == 1);
  entries_[0]->Connect();
  ready_ = false;
}

Connection* RelayPort::CreateConnection(const Candidate& address,
                                        CandidateOrigin origin) {
  // We only create conns to non-udp sockets if they are incoming on this port
  if ((address.protocol() != UDP_PROTOCOL_NAME) &&
      (origin != ORIGIN_THIS_PORT)) {
    return 0;
  }

  // We don't support loopback on relays
  if (address.type() == Type()) {
    return 0;
  }

  if (!IsCompatibleAddress(address.address())) {
    return 0;
  }

  size_t index = 0;
  for (size_t i = 0; i < Candidates().size(); ++i) {
    const Candidate& local = Candidates()[i];
    if (local.protocol() == address.protocol()) {
      index = i;
      break;
    }
  }

  Connection * conn = new ProxyConnection(this, index, address);
  AddOrReplaceConnection(conn);
  return conn;
}

int RelayPort::SendTo(const void* data, size_t size,
                      const rtc::SocketAddress& addr,
                      const rtc::PacketOptions& options,
                      bool payload) {
  // Try to find an entry for this specific address.  Note that the first entry
  // created was not given an address initially, so it can be set to the first
  // address that comes along.
  RelayEntry* entry = 0;

  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->address().IsNil() && payload) {
      entry = entries_[i];
      entry->set_address(addr);
      break;
    } else if (entries_[i]->address() == addr) {
      entry = entries_[i];
      break;
    }
  }

  // If we did not find one, then we make a new one.  This will not be useable
  // until it becomes connected, however.
  if (!entry && payload) {
    entry = new RelayEntry(this, addr);
    if (!entries_.empty()) {
      entry->SetServerIndex(entries_[0]->ServerIndex());
    }
    entry->Connect();
    entries_.push_back(entry);
  }

  // If the entry is connected, then we can send on it (though wrapping may
  // still be necessary).  Otherwise, we can't yet use this connection, so we
  // default to the first one.
  if (!entry || !entry->connected()) {
    RTC_DCHECK(!entries_.empty());
    entry = entries_[0];
    if (!entry->connected()) {
      error_ = ENOTCONN;
      return SOCKET_ERROR;
    }
  }

  // Send the actual contents to the server using the usual mechanism.
  int sent = entry->SendTo(data, size, addr, options);
  if (sent <= 0) {
    RTC_DCHECK(sent < 0);
    error_ = entry->GetError();
    return SOCKET_ERROR;
  }
  // The caller of the function is expecting the number of user data bytes,
  // rather than the size of the packet.
  return static_cast<int>(size);
}

int RelayPort::SetOption(rtc::Socket::Option opt, int value) {
  int result = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->SetSocketOption(opt, value) < 0) {
      result = -1;
      error_ = entries_[i]->GetError();
    }
  }
  options_.push_back(OptionValue(opt, value));
  return result;
}

int RelayPort::GetOption(rtc::Socket::Option opt, int* value) {
  std::vector<OptionValue>::iterator it;
  for (it = options_.begin(); it < options_.end(); ++it) {
    if (it->first == opt) {
      *value = it->second;
      return 0;
    }
  }
  return SOCKET_ERROR;
}

int RelayPort::GetError() {
  return error_;
}

bool RelayPort::SupportsProtocol(const std::string& protocol) const {
  // Relay port may create both TCP and UDP connections.
  return true;
}

ProtocolType RelayPort::GetProtocol() const {
  // We shouldn't be using RelayPort, but we need to provide an implementation
  // here.
  return PROTO_UDP;
}

void RelayPort::OnReadPacket(
    const char* data, size_t size,
    const rtc::SocketAddress& remote_addr,
    ProtocolType proto,
    const rtc::PacketTime& packet_time) {
  if (Connection* conn = GetConnection(remote_addr)) {
    conn->OnReadPacket(data, size, packet_time);
  } else {
    Port::OnReadPacket(data, size, remote_addr, proto);
  }
}

RelayConnection::RelayConnection(const ProtocolAddress* protocol_address,
                                 rtc::AsyncPacketSocket* socket,
                                 rtc::Thread* thread)
    : socket_(socket),
      protocol_address_(protocol_address) {
  request_manager_ = new StunRequestManager(thread);
  request_manager_->SignalSendPacket.connect(this,
                                             &RelayConnection::OnSendPacket);
}

RelayConnection::~RelayConnection() {
  delete request_manager_;
  delete socket_;
}

int RelayConnection::SetSocketOption(rtc::Socket::Option opt,
                                     int value) {
  if (socket_) {
    return socket_->SetOption(opt, value);
  }
  return 0;
}

bool RelayConnection::CheckResponse(StunMessage* msg) {
  return request_manager_->CheckResponse(msg);
}

void RelayConnection::OnSendPacket(const void* data, size_t size,
                                   StunRequest* req) {
  // TODO(mallinath) Find a way to get DSCP value from Port.
  rtc::PacketOptions options;  // Default dscp set to NO_CHANGE.
  int sent = socket_->SendTo(data, size, GetAddress(), options);
  if (sent <= 0) {
    RTC_LOG(LS_VERBOSE) << "OnSendPacket: failed sending to "
                        << GetAddress().ToString()
                        << strerror(socket_->GetError());
    RTC_DCHECK(sent < 0);
  }
}

int RelayConnection::Send(const void* pv, size_t cb,
                          const rtc::PacketOptions& options) {
  return socket_->SendTo(pv, cb, GetAddress(), options);
}

void RelayConnection::SendAllocateRequest(RelayEntry* entry, int delay) {
  request_manager_->SendDelayed(new AllocateRequest(entry, this), delay);
}

RelayEntry::RelayEntry(RelayPort* port,
                       const rtc::SocketAddress& ext_addr)
    : port_(port), ext_addr_(ext_addr),
      server_index_(0), connected_(false), locked_(false),
      current_connection_(NULL) {
}

RelayEntry::~RelayEntry() {
  // Remove all RelayConnections and dispose sockets.
  delete current_connection_;
  current_connection_ = NULL;
}

void RelayEntry::Connect() {
  // If we're already connected, return.
  if (connected_)
    return;

  // If we've exhausted all options, bail out.
  const ProtocolAddress* ra = port()->ServerAddress(server_index_);
  if (!ra) {
    RTC_LOG(LS_WARNING) << "No more relay addresses left to try";
    return;
  }

  // Remove any previous connection.
  if (current_connection_) {
    port()->thread()->Dispose(current_connection_);
    current_connection_ = NULL;
  }

  // Try to set up our new socket.
  RTC_LOG(LS_INFO) << "Connecting to relay via " << ProtoToString(ra->proto)
                   << " @ " << ra->address.ToSensitiveString();

  rtc::AsyncPacketSocket* socket = NULL;

  if (ra->proto == PROTO_UDP) {
    // UDP sockets are simple.
    socket = port_->socket_factory()->CreateUdpSocket(
        rtc::SocketAddress(port_->Network()->GetBestIP(), 0), port_->min_port(),
        port_->max_port());
  } else if (ra->proto == PROTO_TCP || ra->proto == PROTO_SSLTCP) {
    int opts = (ra->proto == PROTO_SSLTCP)
                   ? rtc::PacketSocketFactory::OPT_TLS_FAKE
                   : 0;
    socket = port_->socket_factory()->CreateClientTcpSocket(
        rtc::SocketAddress(port_->Network()->GetBestIP(), 0), ra->address,
        port_->proxy(), port_->user_agent(), opts);
  } else {
    RTC_LOG(LS_WARNING) << "Unknown protocol: " << ra->proto;
  }

  // If we failed to get a socket, move on to the next protocol.
  if (!socket) {
    RTC_LOG(LS_WARNING) << "Socket creation failed";
    port()->thread()->Post(RTC_FROM_HERE, this, kMessageConnectTimeout);
    return;
  }

  // Otherwise, create the new connection and configure any socket options.
  socket->SignalReadPacket.connect(this, &RelayEntry::OnReadPacket);
  socket->SignalSentPacket.connect(this, &RelayEntry::OnSentPacket);
  socket->SignalReadyToSend.connect(this, &RelayEntry::OnReadyToSend);
  current_connection_ = new RelayConnection(ra, socket, port()->thread());
  for (size_t i = 0; i < port_->options().size(); ++i) {
    current_connection_->SetSocketOption(port_->options()[i].first,
                                         port_->options()[i].second);
  }

  // If we're trying UDP, start binding requests.
  // If we're trying TCP, wait for connection with a fixed timeout.
  if ((ra->proto == PROTO_TCP) || (ra->proto == PROTO_SSLTCP)) {
    socket->SignalClose.connect(this, &RelayEntry::OnSocketClose);
    socket->SignalConnect.connect(this, &RelayEntry::OnSocketConnect);
    port()->thread()->PostDelayed(RTC_FROM_HERE, kSoftConnectTimeoutMs, this,
                                  kMessageConnectTimeout);
  } else {
    current_connection_->SendAllocateRequest(this, 0);
  }
}

int RelayEntry::GetError() {
  if (current_connection_ != NULL) {
    return current_connection_->GetError();
  }
  return 0;
}

RelayConnection* RelayEntry::GetBestConnection(RelayConnection* conn1,
                                               RelayConnection* conn2) {
  return conn1->GetProtocol() <= conn2->GetProtocol() ? conn1 : conn2;
}

void RelayEntry::OnConnect(const rtc::SocketAddress& mapped_addr,
                           RelayConnection* connection) {
  // We are connected, notify our parent.
  ProtocolType proto = PROTO_UDP;
  RTC_LOG(INFO) << "Relay allocate succeeded: " << ProtoToString(proto) << " @ "
                << mapped_addr.ToSensitiveString();
  connected_ = true;

  port_->AddExternalAddress(ProtocolAddress(mapped_addr, proto));
  port_->SetReady();
}

int RelayEntry::SendTo(const void* data, size_t size,
                       const rtc::SocketAddress& addr,
                       const rtc::PacketOptions& options) {
  // If this connection is locked to the address given, then we can send the
  // packet with no wrapper.
  if (locked_ && (ext_addr_ == addr))
    return SendPacket(data, size, options);

  // Otherwise, we must wrap the given data in a STUN SEND request so that we
  // can communicate the destination address to the server.
  //
  // Note that we do not use a StunRequest here.  This is because there is
  // likely no reason to resend this packet. If it is late, we just drop it.
  // The next send to this address will try again.

  RelayMessage request;
  request.SetType(STUN_SEND_REQUEST);

  auto magic_cookie_attr =
      StunAttribute::CreateByteString(STUN_ATTR_MAGIC_COOKIE);
  magic_cookie_attr->CopyBytes(TURN_MAGIC_COOKIE_VALUE,
                               sizeof(TURN_MAGIC_COOKIE_VALUE));
  request.AddAttribute(std::move(magic_cookie_attr));

  auto username_attr = StunAttribute::CreateByteString(STUN_ATTR_USERNAME);
  username_attr->CopyBytes(port_->username_fragment().c_str(),
                           port_->username_fragment().size());
  request.AddAttribute(std::move(username_attr));

  auto addr_attr = StunAttribute::CreateAddress(STUN_ATTR_DESTINATION_ADDRESS);
  addr_attr->SetIP(addr.ipaddr());
  addr_attr->SetPort(addr.port());
  request.AddAttribute(std::move(addr_attr));

  // Attempt to lock
  if (ext_addr_ == addr) {
    auto options_attr = StunAttribute::CreateUInt32(STUN_ATTR_OPTIONS);
    options_attr->SetValue(0x1);
    request.AddAttribute(std::move(options_attr));
  }

  auto data_attr = StunAttribute::CreateByteString(STUN_ATTR_DATA);
  data_attr->CopyBytes(data, size);
  request.AddAttribute(std::move(data_attr));

  // TODO(?): compute the HMAC.

  rtc::ByteBufferWriter buf;
  request.Write(&buf);

  return SendPacket(buf.Data(), buf.Length(), options);
}

void RelayEntry::ScheduleKeepAlive() {
  if (current_connection_) {
    current_connection_->SendAllocateRequest(this, kKeepAliveDelay);
  }
}

int RelayEntry::SetSocketOption(rtc::Socket::Option opt, int value) {
  // Set the option on all available sockets.
  int socket_error = 0;
  if (current_connection_) {
    socket_error = current_connection_->SetSocketOption(opt, value);
  }
  return socket_error;
}

void RelayEntry::HandleConnectFailure(
    rtc::AsyncPacketSocket* socket) {
  // Make sure it's the current connection that has failed, it might
  // be an old socked that has not yet been disposed.
  if (!socket ||
      (current_connection_ && socket == current_connection_->socket())) {
    if (current_connection_)
      port()->SignalConnectFailure(current_connection_->protocol_address());

    // Try to connect to the next server address.
    server_index_ += 1;
    Connect();
  }
}

void RelayEntry::OnMessage(rtc::Message *pmsg) {
  RTC_DCHECK(pmsg->message_id == kMessageConnectTimeout);
  if (current_connection_) {
    const ProtocolAddress* ra = current_connection_->protocol_address();
    RTC_LOG(LS_WARNING) << "Relay " << ra->proto << " connection to "
                        << ra->address.ToString() << " timed out";

    // Currently we connect to each server address in sequence. If we
    // have more addresses to try, treat this is an error and move on to
    // the next address, otherwise give this connection more time and
    // await the real timeout.
    //
    // TODO(?): Connect to servers in parallel to speed up connect time
    // and to avoid giving up too early.
    port_->SignalSoftTimeout(ra);
    HandleConnectFailure(current_connection_->socket());
  } else {
    HandleConnectFailure(NULL);
  }
}

void RelayEntry::OnSocketConnect(rtc::AsyncPacketSocket* socket) {
  RTC_LOG(INFO) << "relay tcp connected to "
                << socket->GetRemoteAddress().ToSensitiveString();
  if (current_connection_ != NULL) {
    current_connection_->SendAllocateRequest(this, 0);
  }
}

void RelayEntry::OnSocketClose(rtc::AsyncPacketSocket* socket,
                               int error) {
  RTC_LOG_ERR_EX(LERROR, error) << "Relay connection failed: socket closed";
  HandleConnectFailure(socket);
}

void RelayEntry::OnReadPacket(
    rtc::AsyncPacketSocket* socket,
    const char* data, size_t size,
    const rtc::SocketAddress& remote_addr,
    const rtc::PacketTime& packet_time) {
  // RTC_DCHECK(remote_addr == port_->server_addr());
  // TODO(?): are we worried about this?

  if (current_connection_ == NULL || socket != current_connection_->socket()) {
    // This packet comes from an unknown address.
    RTC_LOG(WARNING) << "Dropping packet: unknown address";
    return;
  }

  // If the magic cookie is not present, then this is an unwrapped packet sent
  // by the server,  The actual remote address is the one we recorded.
  if (!port_->HasMagicCookie(data, size)) {
    if (locked_) {
      port_->OnReadPacket(data, size, ext_addr_, PROTO_UDP, packet_time);
    } else {
      RTC_LOG(WARNING) << "Dropping packet: entry not locked";
    }
    return;
  }

  rtc::ByteBufferReader buf(data, size);
  RelayMessage msg;
  if (!msg.Read(&buf)) {
    RTC_LOG(INFO) << "Incoming packet was not STUN";
    return;
  }

  // The incoming packet should be a STUN ALLOCATE response, SEND response, or
  // DATA indication.
  if (current_connection_->CheckResponse(&msg)) {
    return;
  } else if (msg.type() == STUN_SEND_RESPONSE) {
    if (const StunUInt32Attribute* options_attr =
        msg.GetUInt32(STUN_ATTR_OPTIONS)) {
      if (options_attr->value() & 0x1) {
        locked_ = true;
      }
    }
    return;
  } else if (msg.type() != STUN_DATA_INDICATION) {
    RTC_LOG(INFO) << "Received BAD stun type from server: " << msg.type();
    return;
  }

  // This must be a data indication.

  const StunAddressAttribute* addr_attr =
      msg.GetAddress(STUN_ATTR_SOURCE_ADDRESS2);
  if (!addr_attr) {
    RTC_LOG(INFO) << "Data indication has no source address";
    return;
  } else if (addr_attr->family() != 1) {
    RTC_LOG(INFO) << "Source address has bad family";
    return;
  }

  rtc::SocketAddress remote_addr2(addr_attr->ipaddr(), addr_attr->port());

  const StunByteStringAttribute* data_attr = msg.GetByteString(STUN_ATTR_DATA);
  if (!data_attr) {
    RTC_LOG(INFO) << "Data indication has no data";
    return;
  }

  // Process the actual data and remote address in the normal manner.
  port_->OnReadPacket(data_attr->bytes(), data_attr->length(), remote_addr2,
                      PROTO_UDP, packet_time);
}

void RelayEntry::OnSentPacket(rtc::AsyncPacketSocket* socket,
                              const rtc::SentPacket& sent_packet) {
  port_->OnSentPacket(socket, sent_packet);
}

void RelayEntry::OnReadyToSend(rtc::AsyncPacketSocket* socket) {
  if (connected()) {
    port_->OnReadyToSend();
  }
}

int RelayEntry::SendPacket(const void* data, size_t size,
                           const rtc::PacketOptions& options) {
  int sent = 0;
  if (current_connection_) {
    // We are connected, no need to send packets anywere else than to
    // the current connection.
    sent = current_connection_->Send(data, size, options);
  }
  return sent;
}

AllocateRequest::AllocateRequest(RelayEntry* entry,
                                 RelayConnection* connection)
    : StunRequest(new RelayMessage()),
      entry_(entry),
      connection_(connection) {
  start_time_ = rtc::TimeMillis();
}

void AllocateRequest::Prepare(StunMessage* request) {
  request->SetType(STUN_ALLOCATE_REQUEST);

  auto username_attr = StunAttribute::CreateByteString(STUN_ATTR_USERNAME);
  username_attr->CopyBytes(
      entry_->port()->username_fragment().c_str(),
      entry_->port()->username_fragment().size());
  request->AddAttribute(std::move(username_attr));
}

void AllocateRequest::OnSent() {
  count_ += 1;
  if (count_ == 5)
    timeout_ = true;
}

int AllocateRequest::resend_delay() {
  if (count_ == 0) {
    return 0;
  }
  return 100 * std::max(1 << (count_-1), 2);
}


void AllocateRequest::OnResponse(StunMessage* response) {
  const StunAddressAttribute* addr_attr =
      response->GetAddress(STUN_ATTR_MAPPED_ADDRESS);
  if (!addr_attr) {
    RTC_LOG(INFO) << "Allocate response missing mapped address.";
  } else if (addr_attr->family() != 1) {
    RTC_LOG(INFO) << "Mapped address has bad family";
  } else {
    rtc::SocketAddress addr(addr_attr->ipaddr(), addr_attr->port());
    entry_->OnConnect(addr, connection_);
  }

  // We will do a keep-alive regardless of whether this request suceeds.
  // This should have almost no impact on network usage.
  entry_->ScheduleKeepAlive();
}

void AllocateRequest::OnErrorResponse(StunMessage* response) {
  const StunErrorCodeAttribute* attr = response->GetErrorCode();
  if (!attr) {
    RTC_LOG(LS_ERROR) << "Missing allocate response error code.";
  } else {
    RTC_LOG(INFO) << "Allocate error response: code=" << attr->code()
                  << " reason=" << attr->reason();
  }

  if (rtc::TimeMillis() - start_time_ <= kRetryTimeout)
    entry_->ScheduleKeepAlive();
}

void AllocateRequest::OnTimeout() {
  RTC_LOG(INFO) << "Allocate request timed out";
  entry_->HandleConnectFailure(connection_->socket());
}

}  // namespace cricket
