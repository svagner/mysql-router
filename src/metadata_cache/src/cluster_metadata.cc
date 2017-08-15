/*
  Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

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

#include "cluster_metadata.h"
#include "dim.h"
#include "group_replication_metadata.h"
#include "logger.h"
#include "mysqlrouter/datatypes.h"
#include "mysqlrouter/mysql_session.h"
#include "mysqlrouter/uri.h"
#include "mysqlrouter/utils.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql.h>
#include <errmsg.h>

using mysqlrouter::MySQLSession;
using mysqlrouter::strtoi_checked;

/**
 * Return a string representation of the input character string.
 *
 * @param input_str A character string.
 *
 * @return A string object encapsulation of the input character string. An empty
 *         string if input string is nullptr.
 */
std::string get_string(const char *input_str) {
  if (input_str == nullptr) {
    return "";
  }
  return std::string(input_str);
}

ClusterMetadata::ClusterMetadata(const std::string &user,
                                 const std::string &password,
                                 int connection_timeout,
                                 int /*connection_attempts*/,
                                 unsigned int ttl,
                                 const mysqlrouter::SSLOptions &ssl_options) {
  this->ttl_ = ttl;
  this->user_ = user;
  this->password_ = password;
  this->connection_timeout_ = connection_timeout;
#if 0 // not used so far
  this->metadata_uuid_ = "";
  this->message_ = "";
  this->connection_attempts_ = connection_attempts;
  this->reconnect_tries_ = 0;
#endif

  if (ssl_options.mode.empty()) {
    ssl_mode_ = SSL_MODE_PREFERRED; // default mode
  } else {
    try {
      ssl_mode_ = MySQLSession::parse_ssl_mode(ssl_options.mode);
      log_info("Connections using ssl_mode '%s'", ssl_options.mode.c_str());
    } catch (const std::logic_error& e) {
      throw metadata_cache::metadata_error("Error initializing metadata cache: invalid configuration item 'ssl_mode=" + ssl_options.mode + "'");
    }
  }
  ssl_options_ = ssl_options;
}

/** @brief Destructor
 *
 * Disconnect and release the connection to the metadata node.
 * (RAII will close the connection in metadata_connection_)
 */
ClusterMetadata::~ClusterMetadata() {}

bool ClusterMetadata::do_connect(MySQLSession& connection, const metadata_cache::ManagedInstance &mi) {

  std::string host = (mi.host == "localhost" ? "127.0.0.1" : mi.host);
  try {
    connection.set_ssl_options(ssl_mode_,
                               ssl_options_.tls_version,
                               ssl_options_.cipher,
                               ssl_options_.ca, ssl_options_.capath,
                               ssl_options_.crl, ssl_options_.crlpath);
    connection.connect(host, static_cast<unsigned int>(mi.port), user_, password_,
        "" /* unix-socket */, "" /* default-schema */, connection_timeout_);
    return true;
  } catch (const MySQLSession::Error& e) {
    return false; // error is logged in calling function
  }
}

// this function establishes connection to the first metadata server that succeeds from the list given
// (its name is not very informative, API requirement)
bool ClusterMetadata::connect(const std::vector<metadata_cache::ManagedInstance>
                           & metadata_servers) noexcept {

  // Get a clean metadata server connection object
  // (RAII will close the old one if needed).
  try {
    metadata_connection_ = mysql_harness::DIM::instance().new_MySQLSession();
  } catch (const std::logic_error& e) {
    // defensive programming, shouldn't really happen
    log_error("Failed connecting with Metadata Server: %s", e.what());
    return false;
  }

  // Iterate through the list of servers in the metadata replicaset
  // to fetch a valid connection from which the metadata can be
  // fetched.
  for (const metadata_cache::ManagedInstance& mi : metadata_servers) {
    if (do_connect(*metadata_connection_, mi)) {
      log_info("Connected with metadata server running on %s:%i", mi.host.c_str(), mi.port);
      break;
    } else {
      log_error("Failed connecting with Metadata Server %s:%d: %s (%i)",
                mi.host.c_str(), mi.port,
                metadata_connection_->last_error(),
                metadata_connection_->last_errno());
    }
  }

  if (metadata_connection_->is_connected()) {
    return true;
  } else {
    log_error("Failed connecting with any of the bootstrap servers");
    metadata_connection_.reset();
    return false;
  }
}

void ClusterMetadata::update_replicaset_status(const std::string &name,
    metadata_cache::ManagedReplicaSet &replicaset) { // throws metadata_cache::metadata_error
  log_debug("Updating replicaset status from GR for '%s'", name.c_str());
  // iterate over all cadidate nodes until we find the node that is part of quorum
  bool found_quorum = false;

  std::shared_ptr<MySQLSession> gr_member_connection;
  for (const metadata_cache::ManagedInstance& mi : replicaset.members) {
    std::string mi_addr = (mi.host == "localhost" ? "127.0.0.1" : mi.host) + ":" + std::to_string(mi.port);

    // this function could test these in an if() instead of assert(),
    // but so far the logic that calls this function ensures this
    assert(metadata_connection_->is_connected());

    // connect to node
    if (mi_addr == metadata_connection_->get_address()) { // optimisation: if node is the same as metadata server,
      gr_member_connection = metadata_connection_;        //               share the established connection
    } else {
      try {
        gr_member_connection = mysql_harness::DIM::instance().new_MySQLSession();
      } catch (const std::logic_error& e) {
        // defensive programming, shouldn't really happen. If it does, there's nothing we can do really, we give up
        log_error("While updating metadata, could not initialise MySQL connetion structure");
        throw metadata_cache::metadata_error(e.what());
      }

      if (!do_connect(*gr_member_connection, mi)) {
        log_error("While updating metadata, could not establish a connection to replicaset '%s' through %s",
                  name.c_str(), mi_addr.c_str());
        continue; // server down, next!
      }
    }

    assert(gr_member_connection->is_connected());

    try {
      bool single_primary_mode = true;

      // this node's perspective: give status of all nodes you see
      std::map<std::string, GroupReplicationMember> member_status =
          fetch_group_replication_members(*gr_member_connection,
                                          single_primary_mode); // throws metadata_cache::metadata_error
      log_debug("Replicaset '%s' has %i members in metadata, %i in status table",
                name.c_str(), replicaset.members.size(), member_status.size());

      // check status of all nodes; updates instances ------------------vvvvvvvvvvvvvvvvvv
      metadata_cache::ReplicasetStatus status = check_replicaset_status(replicaset.members, member_status);
      switch (status) {
        case metadata_cache::ReplicasetStatus::AvailableWritable: // we have quorum, good!
          found_quorum = true;
          break;
        case metadata_cache::ReplicasetStatus::AvailableReadOnly: // have quorum, but only RO
          found_quorum = true;
          break;
        case metadata_cache::ReplicasetStatus::Unavailable:       // we have nothing
          log_warning("%s is not part of quorum for replicaset '%s'", mi_addr.c_str(), name.c_str());
          continue;   // this server is no good, next!
      }

      if (found_quorum) {
        replicaset.single_primary_mode = single_primary_mode;
        break; // break out of the member iteration loop
      }

    } catch (const metadata_cache::metadata_error& e) {
      log_warning("Unable to fetch live group_replication member data from %s from replicaset '%s': %s",
                  mi_addr.c_str(), name.c_str(), e.what());
      continue; // faulty server, next!
    } catch (...) {
      assert(0);  // unexpected exception
      log_warning("Unable to fetch live group_replication member data from %s from replicaset '%s'");
      continue; // faulty server, next!
    }

  } // for (const metadata_cache::ManagedInstance& mi : instances)
  log_debug("End updating replicaset for '%s'", name.c_str());

  if (!found_quorum) {
    std::string msg("Unable to fetch live group_replication member data from any server in replicaset '");
    msg += name + "'";
    log_error("%s", msg.c_str());

    // if we don't have a quorum, we want to give "nothing" to the Routing plugin, so it doesn't
    // route anything. Routing plugin is dumb, it has no idea what a quorum is, etc.
    replicaset.members.clear();
  }
}

metadata_cache::ReplicasetStatus ClusterMetadata::check_replicaset_status(
    std::vector<metadata_cache::ManagedInstance> &instances,
    const std::map<std::string, GroupReplicationMember> &member_status) const noexcept {

  // In ideal world, the best way to write this function would be to completely ignore
  // nodes in `instances` and operate on information from `member_status` only. However,
  // there is one problem: the host:port information contained there may not be accurate
  // (localhost vs external addressing issues), and we are forced to use the host:port
  // from `instances` instead. This leads to nasty corner-cases if inconsistencies
  // exist between the two sets, however.

  // Therefore, this code will work well only under one assumption:
  // All nodes in `member_status` are present in `instances`. This assumption should
  // hold unless a user "manually" adds new nodes to the replicaset without adding them
  // to metadata (and the user is not allowed to do that).

  // Detect violation of above assumption (alarm if there's a node in `member_status` not present
  // in `instances`). It's O(n*m), but the CPU time is negligible while keeping code simple.
  for (const auto& status_node : member_status) {
    using MI = metadata_cache::ManagedInstance;
    auto found = std::find_if(instances.begin(), instances.end(), [&status_node] (const MI& metadata_node) {
      return status_node.first == metadata_node.mysql_server_uuid;
    });
    if (found == instances.end()) {
      log_error("Member %s:%d (%s) found in replicaset, yet is not defined in metadata!",
                status_node.second.host.c_str(), status_node.second.port, status_node.first.c_str());
    }
  }

  using metadata_cache::ServerMode;
  using metadata_cache::ReplicasetStatus;
  using GR_State = GroupReplicationMember::State;
  using GR_Role  = GroupReplicationMember::Role;

  // we do two things here:
  // 1. for all `instances`, set .mode according to corresponding .status found in `member_status`
  // 2. count nodes which are part of quorum (online nodes)
  unsigned int online_count = 0;
  bool have_primary_instance = false;
  for (auto &member : instances) {
    auto status = member_status.find(member.mysql_server_uuid);
    if (status != member_status.end()) {
      switch (status->second.state) {
        case GR_State::Online:
          switch (status->second.role) {
            case GR_Role::Primary:
              have_primary_instance = true;
              member.mode = ServerMode::ReadWrite;
              online_count++;
              break;
            case GR_Role::Secondary:
              member.mode = ServerMode::ReadOnly;
              online_count++;
              break;
          }
          break;
        case GR_State::Recovering:
        case GR_State::Unreachable:
        case GR_State::Offline:  // online node with disabled GR maps to this
        case GR_State::Other:
          member.mode = ServerMode::Unavailable;
          break;
        // maybe we should also add State::Error, docs define such value
      }
    } else {
      member.mode = ServerMode::Unavailable;
      log_warning("Member %s:%d (%s) defined in metadata not found in actual replicaset",
                   member.host.c_str(), member.port, member.mysql_server_uuid.c_str());
    }
  }

  // online_count is based on nodes from `instances` instead of `member_status`.
  // This is okay, because all nodes in `member_status` are present in `instances`
  // (our assumption described at the top)
  bool have_quorum = (online_count > member_status.size()/2);

  if (have_quorum) {
    // if primary node was not elected yet, we can only allow reads (typically this is
    // a temporary state shortly after a node failure, but could also be more permanent)
    return have_primary_instance
        ? ReplicasetStatus::AvailableWritable
        : ReplicasetStatus::AvailableReadOnly;
  } else {
    // if we don't have quorum, we don't allow any access. Some configurations might
    // allow RO access in this case, but we don't support it at the momemnt
    return ReplicasetStatus::Unavailable;
  }
}

// throws metadata_cache::metadata_error
ClusterMetadata::ReplicaSetsByName ClusterMetadata::fetch_instances(
    const std::string &cluster_name) {
  log_debug("Updating metadata information for cluster '%s'", cluster_name.c_str());

  assert(metadata_connection_->is_connected());

  // fetch existing replicasets in the cluster from the metadata server (this is the topology that was configured,
  // it will be compared later against current topology reported by (a server in) replicaset)
  ReplicaSetsByName replicasets(fetch_instances_from_metadata_server(cluster_name)); // throws metadata_cache::metadata_error
  if (replicasets.empty())
    log_warning("No replicasets defined for cluster '%s'", cluster_name.c_str());

  // now connect to each replicaset and query it for the list and status of its members.
  // (more precisely, foreach replicaset: search and connect to a member which is part of quorum to retrieve this data)
  for (auto &&rs : replicasets) {
    update_replicaset_status(rs.first, rs.second);  // throws metadata_cache::metadata_error
  }

  return replicasets;
}

// throws metadata_cache::metadata_error
ClusterMetadata::ReplicaSetsByName ClusterMetadata::fetch_instances_from_metadata_server(
    const std::string &cluster_name) {

  // Get expected topology (what was configured) from metadata server. This will later be compared against
  // current topology (what exists NOW) obtained from one of the nodes belonging to a quorum.
  // Note that this topology will also be successfully returned when a particular metadata server
  // is not part of GR, as serving metadata and being part of replicaset are two orthogonal ideas.
  std::string query("SELECT "
                    "R.replicaset_name, "
                    "I.mysql_server_uuid, "
                    "I.role, "
                    "I.weight, "
                    "I.version_token, "
                    "H.location, "
                    "I.addresses->>'$.mysqlClassic', "
                    "I.addresses->>'$.mysqlX' "
                    "FROM "
                    "mysql_innodb_cluster_metadata.clusters AS F "
                    "JOIN mysql_innodb_cluster_metadata.replicasets AS R "
                    "ON F.cluster_id = R.cluster_id "
                    "JOIN mysql_innodb_cluster_metadata.instances AS I "
                    "ON R.replicaset_id = I.replicaset_id "
                    "JOIN mysql_innodb_cluster_metadata.hosts AS H "
                    "ON I.host_id = H.host_id "
                    "WHERE F.cluster_name = " + metadata_connection_->quote(cluster_name) + ";");

  // example response
  // +-----------------+--------------------------------------+------+--------+---------------+----------+--------------------------------+--------------------------+
  // | replicaset_name | mysql_server_uuid                    | role | weight | version_token | location | I.addresses->>'$.mysqlClassic' | I.addresses->>'$.mysqlX' |
  // +-----------------+--------------------------------------+------+--------+---------------+----------+--------------------------------+--------------------------+
  // | default         | 30ec658e-861d-11e6-9988-08002741aeb6 | HA   |   NULL |          NULL | blabla   | localhost:3310                 | NULL                     |
  // | default         | 3acfe4ca-861d-11e6-9e56-08002741aeb6 | HA   |   NULL |          NULL | blabla   | localhost:3320                 | NULL                     |
  // | default         | 4c08b4a2-861d-11e6-a256-08002741aeb6 | HA   |   NULL |          NULL | blabla   | localhost:3330                 | NULL                     |
  // +-----------------+--------------------------------------+------+--------+---------------+----------+--------------------------------+--------------------------+

  // The following instance map stores a list of servers mapped to every
  // replicaset name.
  // {
  //   {replicaset_1:[host1:port1, host2:port2, host3:port3]},
  //   {replicaset_2:[host4:port4, host5:port5, host6:port6]},
  //   ...
  //   {replicaset_n:[hostj:portj, hostk:portk, hostl:portl]}
  // }
  ReplicaSetsByName replicaset_map;

  // Deserialize the resultset into a map that stores a list of server
  // instance objects mapped to each replicaset.
  auto result_processor = [&replicaset_map](const MySQLSession::Row& row) -> bool {

    if (row.size() != 8) {  // TODO write a testcase for this
      throw metadata_cache::metadata_error("Unexpected number of fields in the resultset. "
                                           "Expected = 8, got = " + std::to_string(row.size()));
    }

    metadata_cache::ManagedInstance s;
    s.replicaset_name = get_string(row[0]);
    s.mysql_server_uuid = get_string(row[1]);
    s.role = get_string(row[2]);
    s.weight = row[3] ? std::strtof(row[3], nullptr) : 0;
    s.version_token = row[4] ? static_cast<unsigned int>(strtoi_checked(row[4])) : 0;
    s.location = get_string(row[5]);
    try {
      std::string uri = get_string(row[6]);
      std::string::size_type p;
      if ((p = uri.find(':')) != std::string::npos) {
        s.host = uri.substr(0, p);
        s.port = static_cast<unsigned int>(strtoi_checked(uri.substr(p+1).c_str()));
      } else {
        s.host = uri;
        s.port = 3306;
      }
    } catch (std::runtime_error &e) {
      log_warning("Error parsing URI in metadata for instance %s: '%s': %s",
          row[1], row[6], e.what());
      return true;  // next row
    }
    // X protocol support is not mandatory
    if (row[7] && *row[7]) {
      try {
        std::string uri = get_string(row[7]);
        std::string::size_type p;
        if ((p = uri.find(':')) != std::string::npos) {
          s.host = uri.substr(0, p);
          s.xport = static_cast<unsigned int>(strtoi_checked(uri.substr(p+1).c_str()));
        } else {
          s.host = uri;
          s.xport = 33060;
        }
      } catch (std::runtime_error &e) {
        log_warning("Error parsing URI in metadata for instance %s: '%s': %s",
            row[1], row[7], e.what());
        return true;  // next row
      }
    } else {
      s.xport = s.port * 10;
    }

    auto &rset(replicaset_map[s.replicaset_name]);
    rset.members.push_back(s);
    rset.name = s.replicaset_name;
    rset.single_primary_mode = true; // actual value set elsewhere from GR metadata

    return true;  // false = I don't want more rows
  };

  assert(metadata_connection_->is_connected());

  try {
    metadata_connection_->query(query, result_processor);
  } catch (const MySQLSession::Error& e) {
    throw metadata_cache::metadata_error(e.what());
  } catch (const metadata_cache::metadata_error& e) {
    throw;
  } catch (...) {
    assert(0);  // don't expect anything else to be thrown -> catch dev's attention
    throw;      // in production, rethrow anyway just in case
  }

  return replicaset_map;
}

#if 0 // not used so far
unsigned int ClusterMetadata::fetch_ttl() {
  return ttl_;
}
#endif
