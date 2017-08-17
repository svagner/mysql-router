/*
  Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifdef _WIN32
#  define NOMINMAX
#endif

#include "common.h"
#include "dest_first_available.h"
#include "dest_first_ring_available.h"
#include "dest_metadata_cache.h"
#include "logger.h"
#include "mysql_routing.h"
#include "mysqlrouter/metadata_cache.h"
#include "mysqlrouter/routing.h"
#include "mysqlrouter/uri.h"
#include "mysqlrouter/utils.h"
#include "plugin_config.h"
#include "protocol/protocol.h"

#include <ctime>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>

#include <sys/types.h>

#ifdef _WIN32
/* before winsock inclusion */
#  define FD_SETSIZE 4096
#else
#  undef __FD_SETSIZE
#  define __FD_SETSIZE 4096
#endif

#ifndef _WIN32
#  include <netinet/in.h>
#  include <fcntl.h>
#  include <sys/un.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#else
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

using std::runtime_error;
using std::string;
using mysql_harness::get_strerror;
using mysqlrouter::string_format;
using mysqlrouter::to_string;
using routing::AccessMode;
using mysqlrouter::URI;
using mysqlrouter::URIError;
using mysqlrouter::URIQuery;
using mysqlrouter::TCPAddress;
using mysqlrouter::is_valid_socket_name;

static int kListenQueueSize = 1024;

static const char *kDefaultReplicaSetName = "default";
static const int kAcceptorStopPollInterval_ms = 1000;

MySQLRouting::MySQLRouting(routing::AccessMode mode, uint16_t port,
                           const Protocol::Type protocol,
                           const string &bind_address,
                           const mysql_harness::Path& named_socket,
                           const string &route_name,
                           int max_connections,
                           int destination_connect_timeout,
                           unsigned long long max_connect_errors,
                           unsigned long long max_connect_errors_timeout,
                           unsigned int client_connect_timeout,
                           unsigned int net_buffer_length,
                           SocketOperationsBase *socket_operations)
    : name(route_name),
      mode_(mode),
      max_connections_(set_max_connections(max_connections)),
      destination_connect_timeout_(set_destination_connect_timeout(destination_connect_timeout)),
      max_connect_errors_(max_connect_errors),
      max_connect_errors_timeout_(max_connect_errors_timeout),
      client_connect_timeout_(client_connect_timeout),
      net_buffer_length_(net_buffer_length),
      bind_address_(TCPAddress(bind_address, port)),
      bind_named_socket_(named_socket),
      service_tcp_(0),
      service_named_socket_(0),
      stopping_(false),
      info_active_routes_(0),
      info_handled_routes_(0),
      socket_operations_(socket_operations),
      protocol_(Protocol::create(protocol, socket_operations)) {

  assert(socket_operations_ != nullptr);

  #ifdef _WIN32
  if (named_socket.is_set()) {
    throw std::invalid_argument(string_format("'socket' configuration item is not supported on Windows platform"));
  }
  #endif

  // This test is only a basic assertion.  Calling code is expected to check the validity of these arguments more thoroughally.
  // At the time of writing, routing_plugin.cc : init() is one such place.
  if (!bind_address_.port && !named_socket.is_set()) {
    throw std::invalid_argument(string_format("No valid address:port (%s:%d) or socket (%s) to bind to", bind_address.c_str(), port, named_socket.c_str()));
  }
}

bool MySQLRouting::check_client_errors_time(const std::array<uint8_t, 16> &client_ip_array) {
  size_t timediff;
  if (max_connect_errors_timeout_ == 0) {
    return false;
  }
  std::time_t curtime = std::time(nullptr);
  timediff = curtime - conn_error_counters_[client_ip_array].last_attempt;
  if (timediff > max_connect_errors_timeout_) {
    conn_error_counters_[client_ip_array].count = 0;
    return true;
  };
  return false;
}

bool MySQLRouting::block_client_host(const std::array<uint8_t, 16> &client_ip_array,
                                     const string &client_ip_str, int server) {
  bool blocked = false;
  {
    std::lock_guard<std::mutex> lock(mutex_conn_errors_);
    struct tm *curtime = localtime(&conn_error_counters_[client_ip_array].last_attempt);
    conn_error_counters_[client_ip_array].last_attempt = std::time(0);

    if (++conn_error_counters_[client_ip_array].count >= max_connect_errors_) {
      log_warning("[%s] blocking client host %s", name.c_str(), client_ip_str.c_str());
      blocked = true;
    } else {
      auto time_str = asctime(curtime);
      time_str[strlen(time_str)-1] = '\0';
      log_info("[%s] %d connection errors for %s (max %u). last attempt: %s",
               name.c_str(), conn_error_counters_[client_ip_array].count, client_ip_str.c_str(), max_connect_errors_, time_str);
    }
  }

  if (server >= 0) {
    protocol_->on_block_client_host(server, name);
  }

  return blocked;
}

const std::vector<std::array<uint8_t, 16>> MySQLRouting::get_blocked_client_hosts() const {
  std::lock_guard<std::mutex> lock(mutex_conn_errors_);

  std::vector<std::array<uint8_t, 16>> result;
  for(const auto& client_ip: conn_error_counters_) {
    if (client_ip.second.count >= max_connect_errors_) {
      result.push_back(client_ip.first);
    }
  }

  return result;
}

/*static*/
std::string MySQLRouting::make_thread_name(const std::string& config_name, const std::string& prefix) {

  const char* p = config_name.c_str();

  // at the time of writing, config_name starts with:
  //   "routing:<config_from_conf_file>" (with key)
  // or with:
  //   "routing" (without key).
  // Verify this assumption
  constexpr char kRouting[] = "routing";
  size_t kRoutingLen = sizeof(kRouting) - 1;  // -1 to ignore string terminator
  if (memcmp(p, kRouting, kRoutingLen))
    return prefix + ":parse err";

  // skip over "routing[:]"
  p += kRoutingLen;
  if (*p == ':')
    p++;

  // at the time of writing, bootstrap generates 4 routing configurations by default,
  // which will result in <config_from_conf_file> having one of below 4 values:
  //   "<cluster_name>_default_ro",   "<cluster_name>_default_rw",
  //   "<cluster_name>_default_x_ro", "<cluster_name>_default_x_rw"
  // since we're limited to 15 chars for thread name, we skip over
  // "<cluster_name>_default_" so that suffixes ("x_ro", etc) can fit
  std::string key = p;
  const char kPrefix[] = "_default_";
  if (key.find(kPrefix) != key.npos) {
    key = key.substr(key.find(kPrefix) + sizeof(kPrefix) - 1);  // -1 for string terminator
  }

  // now put everything together
  std::string thread_name = prefix + ":" + key;
  thread_name.resize(15); // max for pthread_setname_np()

  return thread_name;
}

void MySQLRouting::routing_select_thread(int client, const sockaddr_storage& client_addr) noexcept {
  mysql_harness::rename_thread(make_thread_name(name, "RtS").c_str());  // "Rt select() thread" would be too long :(

  int nfds;
  int res;
  int error = 0;
  size_t bytes_down = 0;
  size_t bytes_up = 0;
  size_t bytes_read = 0;
  string extra_msg = "";
  RoutingProtocolBuffer buffer(net_buffer_length_);
  bool handshake_done = false;

  int server = destination_->get_server_socket(destination_connect_timeout_, &error);

  if (!(server > 0 && client > 0)) {
    std::stringstream os;
    os << "Can't connect to remote MySQL server for client '"
      << bind_address_.addr << ":" << bind_address_.port << "'";

    log_warning("[%s] %s", name.c_str(), os.str().c_str());

    // at this point, it does not matter whether client gets the error
    protocol_->send_error(client, 2003, os.str(), "HY000", name);

    socket_operations_->shutdown(client);
    socket_operations_->shutdown(server);

    if (client > 0) {
      socket_operations_->close(client);
    }
    if (server > 0) {
      socket_operations_->close(server);
    }
    return;
  }

  std::pair<std::string, int> c_ip = get_peer_name(client);
  std::pair<std::string, int> s_ip = get_peer_name(server);

  std::string info;
  if (c_ip.second == 0) {
    // Unix socket/Windows Named pipe
    info = string_format("[%s] source %s - dest [%s]:%d",
                         name.c_str(), bind_named_socket_.c_str(),
                         s_ip.first.c_str(), s_ip.second);
  } else {
    info = string_format("[%s] source [%s]:%d - dest [%s]:%d",
                         name.c_str(), c_ip.first.c_str(), c_ip.second,
                         s_ip.first.c_str(), s_ip.second);
  }
  log_debug(info.c_str());

  ++info_active_routes_;
  ++info_handled_routes_;

  nfds = std::max(client, server) + 1;

  int pktnr = 0;
  while (true) {
    fd_set readfds;
    fd_set errfds;
    // Reset on each loop
    FD_ZERO(&readfds);
    FD_ZERO(&errfds);
    FD_SET(client, &readfds);
    FD_SET(server, &readfds);

    if (handshake_done) {
      res = select(nfds, &readfds, nullptr, &errfds, nullptr);
    } else {
      // Handshake reply timeout
      struct timeval timeout_val;
      timeout_val.tv_sec = client_connect_timeout_;
      timeout_val.tv_usec = 0;
      res = select(nfds, &readfds, nullptr, &errfds, &timeout_val);
    }

    if (res <= 0) {
      if (res == 0) {
        extra_msg = string("Select timed out");
      } else if (errno > 0) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        extra_msg = string("Select failed with error: " + get_strerror(errno));
#ifdef _WIN32
      } else if (WSAGetLastError() > 0) {
        extra_msg = string("Select failed with error: " + get_message_error(WSAGetLastError()));
#endif
      } else {
        extra_msg = string("Select failed (" + to_string(res) + ")");
      }

      break;
    }

    // Handle traffic from Server to Client
    // Note: In classic protocol Server _always_ talks first
    if (protocol_->copy_packets(server, client,
                                &readfds, buffer, &pktnr,
                                handshake_done, &bytes_read, true) == -1) {
#ifndef _WIN32
      if (errno > 0) {
#else
      if (errno > 0 || WSAGetLastError() != 0) {
#endif
        extra_msg = string("Copy server-client failed: " + to_string(get_message_error(errno)));
      }
      break;
    }
    bytes_up += bytes_read;

    // Handle traffic from Client to Server
    if (protocol_->copy_packets(client, server,
                                &readfds, buffer, &pktnr,
                                handshake_done, &bytes_read, false) == -1) {
      break;
    }
    bytes_down += bytes_read;

  } // while (true)

  if (!handshake_done) {
    auto ip_array = in_addr_to_array(client_addr);
    log_debug("[%s] Routing failed for %s: %s", name.c_str(), c_ip.first.c_str(), extra_msg.c_str());
    check_client_errors_time(ip_array);
    block_client_host(ip_array, c_ip.first.c_str(), server);
  }

  // Either client or server terminated
  socket_operations_->shutdown(client);
  socket_operations_->shutdown(server);
  socket_operations_->close(client);
  socket_operations_->close(server);

  --info_active_routes_;
#ifndef _WIN32
  log_debug("[%s] Routing stopped (up:%zub;down:%zub) %s", name.c_str(), bytes_up, bytes_down, extra_msg.c_str());
#else
  log_debug("[%s] Routing stopped (up:%Iub;down:%Iub) %s", name.c_str(), bytes_up, bytes_down, extra_msg.c_str());
#endif
}

void MySQLRouting::start() {

  mysql_harness::rename_thread(make_thread_name(name, "RtM").c_str());  // "Rt main" would be too long :(
  if (bind_address_.port > 0) {
    try {
      setup_tcp_service();
    } catch (const runtime_error &exc) {
      stop();
      throw runtime_error(
          string_format("Setting up TCP service using %s: %s", bind_address_.str().c_str(), exc.what()));
    }
    log_info("[%s] started: listening on %s; %s", name.c_str(), bind_address_.str().c_str(),
             routing::get_access_mode_name(mode_).c_str());
  }
#ifndef _WIN32
  if (bind_named_socket_.is_set()) {
    try {
      setup_named_socket_service();
    } catch (const runtime_error &exc) {
      stop();
      throw runtime_error(
          string_format("Setting up named socket service '%s': %s", bind_named_socket_.c_str(), exc.what()));
    }
    log_info("[%s] started: listening using %s; %s", name.c_str(), bind_named_socket_.c_str(),
             routing::get_access_mode_name(mode_).c_str());
  }
#endif
  if (bind_address_.port > 0 || bind_named_socket_.is_set()) {
    //XXX this thread seems unnecessary, since we block on it right after anyway
    thread_acceptor_ = std::thread(&MySQLRouting::start_acceptor, this);
    if (thread_acceptor_.joinable()) {
      thread_acceptor_.join();
    }
#ifndef _WIN32
    if (bind_named_socket_.is_set() && unlink(bind_named_socket_.str().c_str()) == -1) {
      if (errno != ENOENT)
        log_warning(("Failed removing socket file " + bind_named_socket_.str() + " (" + get_strerror(errno) + " (" + to_string(errno) + "))").c_str());
    }
#endif
  }
}

void MySQLRouting::start_acceptor() {
  mysql_harness::rename_thread(make_thread_name(name, "RtA").c_str());  // "Rt Acceptor" would be too long :(

  int sock_client;
  struct sockaddr_storage client_addr;
  socklen_t sin_size = static_cast<socklen_t>(sizeof client_addr);
  int opt_nodelay = 1;
  int nfds = 0;

  destination_->start();

  if (service_tcp_ > 0) {
    routing::set_socket_blocking(service_tcp_, false);
  }
  if (service_named_socket_ > 0) {
    routing::set_socket_blocking(service_named_socket_, false);
  }
  nfds = std::max(service_tcp_, service_named_socket_) + 1;
  fd_set readfds;
  fd_set errfds;
  struct timeval timeout_val;
  while (!stopping()) {
    // Reset on each loop
    FD_ZERO(&readfds);
    FD_ZERO(&errfds);
    if (service_tcp_ > 0) {
      FD_SET(service_tcp_, &readfds);
    }
    if (service_named_socket_ > 0) {
      FD_SET(service_named_socket_, &readfds);
    }
    timeout_val.tv_sec = kAcceptorStopPollInterval_ms / 1000;
    timeout_val.tv_usec = (kAcceptorStopPollInterval_ms % 1000) * 1000;
    int ready_fdnum = select(nfds, &readfds, nullptr, &errfds, &timeout_val);
    if (ready_fdnum <= 0) {
      if (ready_fdnum == 0) {
        // timeout - just check if stopping and continue
        continue;
      } else if (errno > 0) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        log_error("[%s] Select failed with error: %s", name.c_str(), get_strerror(errno).c_str());
        break;
  #ifdef _WIN32
      } else if (WSAGetLastError() > 0) {
        log_error("[%s] Select failed with error: %s", name.c_str(), get_message_error(WSAGetLastError()));
  #endif
        break;
      } else {
        log_error("[%s] Select failed (%i)", name.c_str(), errno);
        break;
      }
    }
    while (ready_fdnum > 0) {
      bool is_tcp = false;
      if (FD_ISSET(service_tcp_, &readfds)) {
        FD_CLR(service_tcp_, &readfds);
        --ready_fdnum;
        if ((sock_client = accept(service_tcp_, (struct sockaddr *) &client_addr, &sin_size)) < 0) {
          log_error("[%s] Failed accepting TCP connection: %s", name.c_str(), get_message_error(errno).c_str());
          continue;
        }
        is_tcp = true;
        log_debug("[%s] TCP connection from %i accepted at %s", name.c_str(),
                  sock_client, bind_address_.str().c_str());
      }
      if (FD_ISSET(service_named_socket_, &readfds)) {
        FD_CLR(service_named_socket_, &readfds);
        --ready_fdnum;
        if ((sock_client = accept(service_named_socket_, (struct sockaddr *) &client_addr, &sin_size)) < 0) {
          log_error("[%s] Failed accepting socket connection: %s", name.c_str(), get_message_error(errno).c_str());
          continue;
        }
        log_debug("[%s] UNIX socket connection from %i accepted at %s", name.c_str(),
                  sock_client, bind_address_.str().c_str());
      }

      if (conn_error_counters_[in_addr_to_array(client_addr)].count >= max_connect_errors_) {
        if (!check_client_errors_time(in_addr_to_array(client_addr))) {
          std::stringstream os;
          os << "Too many connection errors from " << get_peer_name(sock_client).first;
          protocol_->send_error(sock_client, 1129, os.str(), "HY000", name);
          log_info("%s", os.str().c_str());
          socket_operations_->close(sock_client); // no shutdown() before close()
          continue;
        }
      }

      if (info_active_routes_.load(std::memory_order_relaxed) >= max_connections_) {
        protocol_->send_error(sock_client, 1040, "Too many connections", "HY000", name);
        socket_operations_->close(sock_client); // no shutdown() before close()
        log_warning("[%s] reached max active connections (%d max=%d)", name.c_str(),
                   info_active_routes_.load(), max_connections_);
        continue;
      }

      if (is_tcp && setsockopt(sock_client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&opt_nodelay), static_cast<socklen_t>(sizeof(int))) == -1) {
        log_error("[%s] client setsockopt error: %s", name.c_str(), get_message_error(errno).c_str());
        continue;
      }

      std::thread(&MySQLRouting::routing_select_thread, this, sock_client, client_addr).detach();
    }
  } // while (!stopping())
  log_info("[%s] stopped", name.c_str());
}

void MySQLRouting::stop() {
  stopping_.store(true);
}

void MySQLRouting::setup_tcp_service() {
  struct addrinfo *servinfo, *info, hints;
  int err;
  int option_value;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  errno = 0;

  err = getaddrinfo(bind_address_.addr.c_str(), to_string(bind_address_.port).c_str(), &hints, &servinfo);
  if (err != 0) {
    throw runtime_error(string_format("[%s] Failed getting address information (%s)",
                                      name.c_str(), gai_strerror(err)));
  }

  // Try to setup socket and bind
  for (info = servinfo; info != nullptr; info = info->ai_next) {
    if ((service_tcp_ = socket(info->ai_family, info->ai_socktype, info->ai_protocol)) == -1) {
       // in windows, WSAGetLastError() will be called by get_message_error()
      std::string error = get_message_error(errno);
      freeaddrinfo(servinfo);
      throw std::runtime_error(error);
    }

#ifndef _WIN32
    option_value = 1;
    if (setsockopt(service_tcp_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option_value),
            static_cast<socklen_t>(sizeof(int))) == -1) {
      std::string error = get_message_error(errno);
      freeaddrinfo(servinfo);
      socket_operations_->close(service_tcp_);
      throw std::runtime_error(error);
    }
#endif

    if (::bind(service_tcp_, info->ai_addr, info->ai_addrlen) == -1) {
      std::string error = get_message_error(errno);
      freeaddrinfo(servinfo);
      socket_operations_->close(service_tcp_);
      throw std::runtime_error(error);
    }
    break;
  }
  freeaddrinfo(servinfo);

  if (info == nullptr) {
    throw runtime_error(string_format("[%s] Failed to setup server socket", name.c_str()));
  }

  if (listen(service_tcp_, kListenQueueSize) < 0) {
    throw runtime_error(string_format("[%s] Failed to start listening for connections using TCP", name.c_str()));
  }
}

#ifndef _WIN32
void MySQLRouting::setup_named_socket_service() {
  struct sockaddr_un sock_unix;
  string socket_file = bind_named_socket_.str();
  errno = 0;

  assert(!socket_file.empty());

  std::string error_msg;
  if (!is_valid_socket_name(socket_file, error_msg)) {
    throw std::runtime_error(error_msg);
  }

  if ((service_named_socket_ = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    throw std::invalid_argument(get_strerror(errno));
  }

  sock_unix.sun_family = AF_UNIX;
  std::strncpy(sock_unix.sun_path, socket_file.c_str(), socket_file.size() + 1);

retry:
  if (::bind(service_named_socket_, (struct sockaddr *) &sock_unix, static_cast<socklen_t>(sizeof(sock_unix))) == -1) {
    int save_errno = errno;
    if (errno == EADDRINUSE) {
      // file exists, try to connect to it to see if the socket is already in use
      if (::connect(service_named_socket_,
                    (struct sockaddr *) &sock_unix, static_cast<socklen_t>(sizeof(sock_unix))) == 0) {
        log_error("Socket file %s already in use by another process", socket_file.c_str());
        throw std::runtime_error("Socket file already in use");
      } else {
        if (errno == ECONNREFUSED) {
          log_warning("Socket file %s already exists, but seems to be unused. Deleting and retrying...", socket_file.c_str());
          if (unlink(socket_file.c_str()) == -1) {
            if (errno != ENOENT) {
              log_warning(("Failed removing socket file " + socket_file + " (" + get_strerror(errno) + " (" + to_string(errno) + "))").c_str());
              throw std::runtime_error(
                  "Failed removing socket file " + socket_file + " (" + get_strerror(errno) + " (" + to_string(errno) + "))");
            }
          }
          errno = 0;
          socket_operations_->close(service_named_socket_);
          if ((service_named_socket_ = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
            throw std::runtime_error(get_strerror(errno));
          }
          goto retry;
        } else {
          errno = save_errno;
        }
      }
    }
    log_error("Error binding to socket file %s: %s", socket_file.c_str(), get_strerror(errno).c_str());
    throw std::runtime_error(get_strerror(errno));
  }

  if (listen(service_named_socket_, kListenQueueSize) < 0) {
    throw runtime_error("Failed to start listening for connections using named socket");
  }
}
#endif

void MySQLRouting::set_destinations_from_uri(const URI &uri) {
  if (uri.scheme == "metadata-cache") {
    // Syntax: metadata_cache://[<metadata_cache_key(unused)>]/<replicaset_name>?role=PRIMARY|SECONDARY
    std::string replicaset_name = kDefaultReplicaSetName;
    std::string role;

    if (uri.path.size() > 0 && !uri.path[0].empty())
      replicaset_name = uri.path[0];
    if (uri.query.find("role") == uri.query.end())
      throw runtime_error("Missing 'role' in routing destination specification");

    destination_.reset(new DestMetadataCacheGroup(uri.host, replicaset_name,
                                                  get_access_mode_name(mode_),
                                                  uri.query, protocol_->get_type()));
  } else {
    throw runtime_error(string_format("Invalid URI scheme; expecting: 'metadata-cache' is: '%s'",
                                      uri.scheme.c_str()));
  }
}

void MySQLRouting::set_destinations_from_csv(const string &csv) {
  std::stringstream ss(csv);
  std::string part;
  std::pair<std::string, uint16_t> info;


  if (AccessMode::kReadOnly == mode_) {
    destination_.reset(new RouteDestination(protocol_->get_type(), socket_operations_));
  } else if (AccessMode::kReadWrite == mode_) {
    destination_.reset(new DestFirstAvailable(protocol_->get_type(), socket_operations_));
  } else if (AccessMode::kReadWriteRing == mode_) {
    destination_.reset(new DestFirstRingAvailable(protocol_->get_type(), socket_operations_));
  } else {
    throw std::runtime_error("Unknown mode");
  }
  // Fall back to comma separated list of MySQL servers
  while (std::getline(ss, part, ',')) {
    info = mysqlrouter::split_addr_port(part);
    if (info.second == 0) {
      info.second = Protocol::get_default_port(protocol_->get_type());
    }
    TCPAddress addr(info.first, info.second);
    if (addr.is_valid()) {
      destination_->add(addr);
    } else {
      throw std::runtime_error(string_format("Destination address '%s' is invalid", addr.str().c_str()));
    }
  }

  // Check whether bind address is part of list of destinations
  for (auto &it: *destination_) {
    if (it == bind_address_) {
      throw std::runtime_error("Bind Address can not be part of destinations");
    }
  }

  if (destination_->size() == 0) {
    throw std::runtime_error("No destinations available");
  }
}

int MySQLRouting::set_destination_connect_timeout(int seconds) {
  if (seconds <= 0 || seconds > UINT16_MAX) {
    auto err = string_format("[%s] tried to set destination_connect_timeout using invalid value, was '%d'",
                             name.c_str(), seconds);
    throw std::invalid_argument(err);
  }
  destination_connect_timeout_ = seconds;
  return destination_connect_timeout_;
}

int MySQLRouting::set_max_connections(int maximum) {
  if (maximum <= 0 || maximum > UINT16_MAX) {
    auto err = string_format("[%s] tried to set max_connections using invalid value, was '%d'", name.c_str(),
                             maximum);
    throw std::invalid_argument(err);
  }
  max_connections_ = maximum;
  return max_connections_;
}
