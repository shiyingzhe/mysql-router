/*
  Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

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

// must have these first, before #includes that rely on it
#include <gtest/gtest_prod.h>

#include "cluster_metadata.h"
#include "group_replication_metadata.h"
#include "metadata_cache.h"
#include "mysqlrouter/mysql_session.h"

#include <memory>
#include <map>
#include <cmath>
#include <algorithm>
#include <set>

#ifdef __clang__
// GMock doesn't know about override keyword which Clang complains about
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#pragma clang diagnostic push
// GMock throws sign-conversions warnings on Clang
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#include "gmock/gmock.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

using ::testing::_;
using ::testing::Assign;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Mock;
using ::testing::Return;
using ::testing::StartsWith;
using ::testing::Throw;

using mysqlrouter::MySQLSession;
using mysqlrouter::MySQLSessionFactory;
using metadata_cache::ManagedInstance;
using metadata_cache::ServerMode;

using State = GroupReplicationMember::State;
using Role  = GroupReplicationMember::Role;
using RS    = metadata_cache::ReplicasetStatus;

////////////////////////////////////////////////////////////////////////////////
//
// These tests focus on testing functionality implemented in metadata_cache.{h,cc}.
//
// Notes:
// - throughout tests we use human-readable UUIDs ("intance-1", "instance-2", etc)
//   for clarity, but actual code will deal with proper GUIDs (such as
//   "3acfe4ca-861d-11e6-9e56-08002741aeb6"). At the time of writing, these IDs
//   are treated like any other plain strings in production code (we call empty(),
//   operator==(), etc, on them, but we never parse them), thus allowing us to use
//   human-readable UUIDs in tests.
// - the test groups are arranged in order that they run in production. This should
//   help diagnose problems faster, as the stuff tested later depends on the stuff
//   tested earlier.
//
// TODO: At the time of writing, tests don't test multiple replicaset scenarios.
//       The code will probably work as is, but "it doesn't work until it's proven
//       by a unit test".
//
////////////////////////////////////////////////////////////////////////////////



// query #1 (occurrs first) - fetches expected (configured) topology from metadata server
std::string query_metadata = "SELECT "
    "R.replicaset_name, I.mysql_server_uuid, I.role, I.weight, I.version_token, H.location, "
    "I.addresses->>'$.mysqlClassic', I.addresses->>'$.mysqlX' "
    "FROM mysql_innodb_cluster_metadata.clusters AS F "
    "JOIN mysql_innodb_cluster_metadata.replicasets AS R ON F.cluster_id = R.cluster_id "
    "JOIN mysql_innodb_cluster_metadata.instances AS I ON R.replicaset_id = I.replicaset_id "
    "JOIN mysql_innodb_cluster_metadata.hosts AS H ON I.host_id = H.host_id "
    "WHERE F.cluster_name = " /*'<cluster name>';"*/;

// query #2 (occurs second) - fetches primary member as seen by a particular node
std::string query_primary_member = "show status like 'group_replication_primary_member'";

// query #3 (occurs last) - fetches current topology as seen by a particular node
std::string query_status = "SELECT "
    "member_id, member_host, member_port, member_state, @@group_replication_single_primary_mode "
    "FROM performance_schema.replication_group_members "
    "WHERE channel_name = 'group_replication_applier'";



////////////////////////////////////////////////////////////////////////////////
//
// mock classes
//
////////////////////////////////////////////////////////////////////////////////

class MockMySQLSession: public MySQLSession {
 public:
  MOCK_METHOD2(query, void(const std::string& query, const RowProcessor& processor));
  MOCK_METHOD2(flag_succeed, void(const std::string&, unsigned int));
  MOCK_METHOD2(flag_fail, void(const std::string&, unsigned int));

  void connect(const std::string& host,
               unsigned int port,
               const std::string&,
               const std::string&,
               int = kDefaultConnectionTimeout) override {
    connect_cnt_++;

    std::string host_port = host + ':' + std::to_string(port);
    if (good_conns_.count(host_port))
      connect_succeed(host, port);
    else
      connect_fail(host, port); // throws Error
  }

  void set_good_conns(std::set<std::string>&& conns) {
    good_conns_ = std::move(conns);
  }

  void query_impl(const RowProcessor &processor,
                  const std::vector<Row>& resultset,
                  bool should_succeed = true) const {

    // emulate real MySQLSession::query() error-handling logic
    if (!connected_)
      throw std::logic_error("Not connected");
    if (!should_succeed) {
      std::string s = "Error executing MySQL query: some error(42)";
      throw Error(s.c_str(), 42);
    }

    for(const Row& row : resultset) {
      if (!processor(row))  // processor is allowed to throw
        break;
    }
  }

 private:
  void connect_succeed(const std::string& host, unsigned int port) {
    flag_succeed(host, port);

    // emulate real MySQLSession::connect() behaviour on success
    connected_ = true;
    connection_address_ = host + ":" + std::to_string(port);
  }

  void connect_fail(const std::string& host, unsigned int port) {
    flag_fail(host, port);

    // emulate real MySQLSession::connect() behaviour on failure
    std::string s = "Error connecting to MySQL server at ";
    s += host + ":" + std::to_string(port) + ": some error(42)";
    throw Error(s.c_str(), 42);
  }

  int connect_cnt_ = 0;
  std::set<std::string> good_conns_;
};

class MockMySQLSessionFactory : public MySQLSessionFactory {
  const int kInstances = 4;

 public:
  MockMySQLSessionFactory()
  {
    // we pre-allocate instances and then return those in create() and get()
    for (int i = 0; i < kInstances; i++) {
      sessions_.emplace_back(new MockMySQLSession);
    }
  }

  std::shared_ptr<MySQLSession> create() const override {
    return sessions_.at(next_++);
  }

  MockMySQLSession& get(unsigned i) const {
    return *sessions_.at(i);
  }

  int create_cnt() const {
    // without cast, we'd need to type 'u' everywhere, like so:
    // EXPECT_EQ(1u, factory.create_cnt());
    return static_cast<int>(next_);
  }

 private:
  // can't use vector<MockMySQLSession>, because MockMySQLSession is not copyable
  // due to GMock (produces weird linker errors)
  std::vector<std::shared_ptr<MockMySQLSession>> sessions_;

  mutable unsigned next_ = 0;
};

static bool cmp_mi_FIFMS(const ManagedInstance& lhs, const ManagedInstance& rhs) {

  // This function compares fields set by Metadata::fetch_instances_from_metadata_server().
  // Ignored fields (they're not being set at the time of writing):
  //   ServerMode mode;

  return lhs.replicaset_name == rhs.replicaset_name
      && lhs.mysql_server_uuid == rhs.mysql_server_uuid
      && lhs.role == rhs.role
      && std::fabs(lhs.weight - rhs.weight) < 0.001
      && lhs.version_token == rhs.version_token
      && lhs.location == rhs.location
      && lhs.host == rhs.host
      && lhs.port == rhs.port
      && lhs.xport == rhs.xport;
}

static bool cmp_mi_FI(const ManagedInstance& lhs, const ManagedInstance& rhs) {

  // This function compares fields set by Metadata::fetch_instances().
  // Ignored fields (they're not being set at the time of writing):
  //   std::string role;
  //   float weight;
  //   unsigned int version_token;
  //   std::string location;

  return lhs.replicaset_name == rhs.replicaset_name
      && lhs.mysql_server_uuid == rhs.mysql_server_uuid
      && lhs.mode == rhs.mode
      && lhs.host == rhs.host
      && lhs.port == rhs.port
      && lhs.xport == rhs.xport;
}



////////////////////////////////////////////////////////////////////////////////
//
// test class
//
////////////////////////////////////////////////////////////////////////////////

class MetadataTest : public ::testing::Test {

 public:

  //---- helper functions --------------------------------------------------------

  void connect_to_first_metadata_server() {

    std::vector<ManagedInstance> metadata_servers {
      {"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100},
      {"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "127.0.0.1", 3320, 33200},
      {"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300},
    };
    session_factory.get(0).set_good_conns({"127.0.0.1:3310", "127.0.0.1:3320", "127.0.0.1:3330"});

    EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3310)).Times(1);
    EXPECT_TRUE(metadata.connect(metadata_servers));
  }

  void enable_connection(unsigned session, unsigned port) {
    session_factory.get(session).set_good_conns({std::string("127.0.0.1:") + std::to_string(port)});  // \_ new connection
    EXPECT_CALL(session_factory.get(session), flag_succeed(_, port)).Times(1);                        // /  should succeed
  }

  //----- mock SQL queries -------------------------------------------------------

  std::function<void(const std::string&, const MySQLSession::RowProcessor& processor)> query_primary_member_ok(unsigned session) {
    return [this, session](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(session).query_impl(processor, {{"group_replication_primary_member", "instance-1"}}); // typical response
    };
  }

  std::function<void(const std::string&, const MySQLSession::RowProcessor& processor)> query_primary_member_empty(unsigned session) {
    return [this, session](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(session).query_impl(processor, {{"group_replication_primary_member", ""}}); // empty response
    };
  }

  std::function<void(const std::string&, const MySQLSession::RowProcessor& processor)> query_primary_member_fail(unsigned session) {
    return [this, session](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(session).query_impl(processor, {}, false); // false = induce fail query
    };
  }

  std::function<void(const std::string&, const MySQLSession::RowProcessor& processor)> query_status_fail(unsigned session) {
    return [this, session](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(session).query_impl(processor, {}, false); // false = induce fail query
    };
  }

  std::function<void(const std::string&, const MySQLSession::RowProcessor& processor)> query_status_ok(unsigned session) {
    return [this, session](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(session).query_impl(processor, {
        {"instance-1", "ubuntu", "3310", "ONLINE", "1"},  // \.
        {"instance-2", "ubuntu", "3320", "ONLINE", "1"},  //  > typical response
        {"instance-3", "ubuntu", "3330", "ONLINE", "1"},  // /
      });
    };
  }



 private: // toggling between public and private because we require these vars in this particular order
  std::unique_ptr<MockMySQLSessionFactory> up_session_factory_{new MockMySQLSessionFactory()};
 public:
  MockMySQLSessionFactory& session_factory = *up_session_factory_; // hack: we can do this because unique_ptr will outlive our tests
  ClusterMetadata metadata{"user", "pass", 0, 0, 0, std::move(up_session_factory_)};

  // set instances that would be returned by successful metadata.fetch_instances_from_metadata_server()
  // for a healthy 3-node setup. Only some tests need this variable.
  std::vector<ManagedInstance> typical_instances {
    // will be set ----------------------vvvvvvvvvvvvvvvvvvvvvvv  v--v--vv--- ignored at the time of writing
    {"replicaset-1", "instance-1", "HA", ServerMode::Unavailable, 0, 0, "", "localhost", 3310, 33100},
    {"replicaset-1", "instance-2", "HA", ServerMode::Unavailable, 0, 0, "", "localhost", 3320, 33200},
    {"replicaset-1", "instance-3", "HA", ServerMode::Unavailable, 0, 0, "", "localhost", 3330, 33300},
    // ignored at time of writing -^^^^--------------------------------------------------------^^^^^
    // TODO: ok to ignore xport?
  };
};



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::connect()
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, ConnectToMetadataServer_1st) {

  std::vector<ManagedInstance> metadata_servers {
    {"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100},  // good
    {"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "127.0.0.1", 3320, 33200},
    {"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300},
  };
  session_factory.get(0).set_good_conns({"127.0.0.1:3310"});

  // should connect to 1st server
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3310)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3310)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3320)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3320)).Times(0);
  EXPECT_TRUE(metadata.connect(metadata_servers));

  EXPECT_EQ(1, session_factory.create_cnt());
}

TEST_F(MetadataTest, ConnectToMetadataServer_2nd) {

  std::vector<ManagedInstance> metadata_servers {
    {"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100},  // bad
    {"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "127.0.0.1", 3320, 33200},  // good
    {"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300},
  };
  session_factory.get(0).set_good_conns({"127.0.0.1:3320"});

  // should connect to 2nd server
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3310)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3310)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3320)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3320)).Times(0);
  EXPECT_TRUE(metadata.connect(metadata_servers));

  EXPECT_EQ(1, session_factory.create_cnt());
}

TEST_F(MetadataTest, ConnectToMetadataServer_3rd) {

  std::vector<ManagedInstance> metadata_servers {
    {"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100},  // bad
    {"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "127.0.0.1", 3320, 33200},  // bad
    {"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300},  // good
  };
  session_factory.get(0).set_good_conns({"127.0.0.1:3330"});

  // should connect to 3rd server
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3310)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3310)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3320)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3320)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3330)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3330)).Times(0);
  EXPECT_TRUE(metadata.connect(metadata_servers));

  EXPECT_EQ(1, session_factory.create_cnt());
}

TEST_F(MetadataTest, ConnectToMetadataServer_none) {

  std::vector<ManagedInstance> metadata_servers {
    {"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100},  // bad
    {"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "127.0.0.1", 3320, 33200},  // bad
    {"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300},  // bad
  };
  session_factory.get(0).set_good_conns({});

  // should connect to first server
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3310)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3310)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3320)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3320)).Times(1);
  EXPECT_CALL(session_factory.get(0), flag_succeed(_, 3330)).Times(0);
  EXPECT_CALL(session_factory.get(0), flag_fail   (_, 3330)).Times(1);
  EXPECT_FALSE(metadata.connect(metadata_servers));

  EXPECT_EQ(1, session_factory.create_cnt());
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::fetch_instances_from_metadata_server()
// [QUERY #1: query_metadata]
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, FetchInstancesFromMetadataServer) {

  connect_to_first_metadata_server();

  // test automatic conversions
  {
    auto resultset_metadata = [this](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(0).query_impl(processor, {
        {"replicaset-1", "instance-1", "HA",               "0.2", "0", "location1", "localhost:3310", "localhost:33100"},
        {"replicaset-1", "instance-2", "arbitrary_string", "1.5", "1", "s.o_loc",   "localhost:3320", NULL},
        {"replicaset-1", "instance-3", "",                 "0.0", "99", "",         "localhost", NULL},
        {"replicaset-1", "instance-4", "",                  NULL, NULL, "",         NULL, NULL},
      });
    };
    EXPECT_CALL(session_factory.get(0), query(StartsWith(query_metadata), _)).Times(1).WillOnce(Invoke(resultset_metadata));

    ClusterMetadata::InstancesByReplicaSet rs = metadata.fetch_instances_from_metadata_server("replicaset-1");

    EXPECT_EQ(1u, rs.size());
    EXPECT_EQ(4u, rs.at("replicaset-1").size()); // not set/checked ----------------------------vvvvvvvvvvvvvvvvvvvvvvv
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-1", "HA",               ServerMode::Unavailable, 0.2f, 0, "location1", "localhost", 3310, 33100}, rs.at("replicaset-1").at(0)));
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-2", "arbitrary_string", ServerMode::Unavailable, 1.5f, 1, "s.o_loc",   "localhost", 3320, 33200}, rs.at("replicaset-1").at(1)));
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-3", "",                 ServerMode::Unavailable, 0.0f, 99, "",         "localhost", 3306, 33060}, rs.at("replicaset-1").at(2)));
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-4", "",                 ServerMode::Unavailable, 0.0f, 0, "",          "", 3306, 33060}, rs.at("replicaset-1").at(3)));
    // TODO is this really right behavior? ---------------------------------------------------------------------------------------------------^^
  }

  // empty result
  {
    auto resultset_metadata = [this](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(0).query_impl(processor, {
      });
    };
    EXPECT_CALL(session_factory.get(0), query(StartsWith(query_metadata), _)).Times(1).WillOnce(Invoke(resultset_metadata));

    ClusterMetadata::InstancesByReplicaSet rs = metadata.fetch_instances_from_metadata_server("replicaset-1");

    EXPECT_EQ(0u, rs.size());
  }

  // multiple replicasets
  {
    auto resultset_metadata = [this](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(0).query_impl(processor, {
        {"replicaset-2", "instance-4", "HA", NULL, NULL, "", "localhost2:3333", NULL},
        {"replicaset-1", "instance-1", "HA", NULL, NULL, "", "localhost1:1111", NULL},
        {"replicaset-1", "instance-2", "HA", NULL, NULL, "", "localhost1:2222", NULL},
        {"replicaset-1", "instance-3", "HA", NULL, NULL, "", "localhost1:3333", NULL},
        {"replicaset-3", "instance-5", "HA", NULL, NULL, "", "localhost3:3333", NULL},
        {"replicaset-3", "instance-6", "HA", NULL, NULL, "", "localhost3:3333", NULL},
      });
    };
    EXPECT_CALL(session_factory.get(0), query(StartsWith(query_metadata), _)).Times(1).WillOnce(Invoke(resultset_metadata));

    ClusterMetadata::InstancesByReplicaSet rs = metadata.fetch_instances_from_metadata_server("replicaset-1");

    EXPECT_EQ(3u, rs.size());
    EXPECT_EQ(3u, rs.at("replicaset-1").size());
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-1", "HA", ServerMode::Unavailable, 0, 0, "", "localhost1", 1111, 11110}, rs.at("replicaset-1").at(0)));
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-2", "HA", ServerMode::Unavailable, 0, 0, "", "localhost1", 2222, 22220}, rs.at("replicaset-1").at(1)));
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-1", "instance-3", "HA", ServerMode::Unavailable, 0, 0, "", "localhost1", 3333, 33330}, rs.at("replicaset-1").at(2)));
    EXPECT_EQ(1u, rs.at("replicaset-2").size());
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-2", "instance-4", "HA", ServerMode::Unavailable, 0, 0, "", "localhost2", 3333, 33330}, rs.at("replicaset-2").at(0)));
    EXPECT_EQ(2u, rs.at("replicaset-3").size());
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-3", "instance-5", "HA", ServerMode::Unavailable, 0, 0, "", "localhost3", 3333, 33330}, rs.at("replicaset-3").at(0)));
    EXPECT_TRUE(cmp_mi_FIFMS(ManagedInstance{"replicaset-3", "instance-6", "HA", ServerMode::Unavailable, 0, 0, "", "localhost3", 3333, 33330}, rs.at("replicaset-3").at(1)));
  }

  // query fails
  {
    auto resultset_metadata = [this](const std::string&, const MySQLSession::RowProcessor& processor) {
      session_factory.get(0).query_impl(processor, {}, false);
    };
    EXPECT_CALL(session_factory.get(0), query(StartsWith(query_metadata), _)).Times(1).WillOnce(Invoke(resultset_metadata));

    // exception thrown by MySQLSession::query() should get repackaged in metadata_cache::metadata_error
    ClusterMetadata::InstancesByReplicaSet rs;
    try {
      rs = metadata.fetch_instances_from_metadata_server("replicaset-1");
      FAIL() << "Expected metadata_cache::metadata_error to be thrown";
    } catch (const metadata_cache::metadata_error& e) {
      EXPECT_STREQ("Error executing MySQL query: some error(42)", e.what());
      EXPECT_EQ(0u, rs.size());
    } catch (...) {
      FAIL() << "Expected metadata_cache::metadata_error to be thrown";
    }
  }
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::check_replicaset_status()
//
////////////////////////////////////////////////////////////////////////////////

#if 0 // TODO fix this
TEST_F(MetadataTest, CheckReplicasetStatus_3NodeSetup) {

  std::vector<ManagedInstance> expected_servers {
    // ServerMode doesn't matter ------vvvvvvvvvvv
    {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
  };

  // typical
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
  }

  // less typical
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-2", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);

    auto r = {ServerMode::ReadOnly, ServerMode::ReadWrite, ServerMode::ReadOnly};
    EXPECT_TRUE(std::equal(r.begin(), r.end(), expected_servers.begin(), [](ServerMode mode, ManagedInstance mi) {
      return mode == mi.mode;
    }));
  }

  // less typical
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-3", {"", "", 0, State::Online, Role::Primary  } },
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(2).mode);

    auto r = {ServerMode::ReadOnly, ServerMode::ReadOnly, ServerMode::ReadWrite};
    EXPECT_TRUE(std::equal(r.begin(), r.end(), expected_servers.begin(), [](ServerMode mode, ManagedInstance mi) {
      return mode == mi.mode;
    }));
  }

  // no primary
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableReadOnly, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);

    auto r = {ServerMode::ReadOnly, ServerMode::ReadOnly, ServerMode::ReadOnly};
    EXPECT_TRUE(std::equal(r.begin(), r.end(), expected_servers.begin(), [](ServerMode mode, ManagedInstance mi) {
      return mode == mi.mode;
    }));
  }

  // multi-primary (currently unsupported, but treat as single-primary)
  // TODO: this behaviour should change, probably turn all Primary -> Unavailable but leave Secondary alone
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Primary} },
      { "instance-2", {"", "", 0, State::Online, Role::Primary} },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    #ifdef NDEBUG // guardian assert() should fail in Debug
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);

    auto r = {ServerMode::ReadWrite, ServerMode::ReadWrite, ServerMode::ReadOnly};
    EXPECT_TRUE(std::equal(r.begin(), r.end(), expected_servers.begin(), [](ServerMode mode, ManagedInstance mi) {
      return mode == mi.mode;
    }));
    #endif
  }

  // 1 node missing
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-2) defined in metadata not found in actual replicaset"

  }

  // 1 node missing, no primary
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableReadOnly, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-1) defined in metadata not found in actual replicaset"
  }

  // 2 nodes missing
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
    };
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-2) defined in metadata not found in actual replicaset"
    // should log warning "Member <host>:<port> (instance-3) defined in metadata not found in actual replicaset"
  }

  // 2 nodes missing, no primary
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-1) defined in metadata not found in actual replicaset"
    // should log warning "Member <host>:<port> (instance-2) defined in metadata not found in actual replicaset"
  }

  // all nodes missing
  {
    std::map<std::string, GroupReplicationMember> server_status {};
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-1) defined in metadata not found in actual replicaset"
    // should log warning "Member <host>:<port> (instance-2) defined in metadata not found in actual replicaset"
    // should log warning "Member <host>:<port> (instance-3) defined in metadata not found in actual replicaset"
  }

  // 1 unknown id
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-4", {"instance-4", "host4", 4444, State::Online, Role::Secondary} },
      { "instance-2", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-1) defined in metadata not found in actual replicaset"
    // instance-4 will be silently ignored
  }

  // 2 unknown ids
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-4", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-2", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-5", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-1) defined in metadata not found in actual replicaset"
    // should log warning "Member <host>:<port> (instance-3) defined in metadata not found in actual replicaset"
    // instance-4 and -5 will be silently ignored
  }

  // more nodes than expected
  {
    std::map<std::string, GroupReplicationMember> server_status {
      { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
      { "instance-4", {"", "", 0, State::Online, Role::Primary  } },
      { "instance-5", {"", "", 0, State::Online, Role::Secondary} },
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // instance-4 and -5 will be silently ignored
  }
}

TEST_F(MetadataTest, CheckReplicasetStatus_VariableNodeSetup) {

  std::map<std::string, GroupReplicationMember> server_status {
    { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
    { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
    { "instance-3", {"", "", 0, State::Online, Role::Secondary} },
  };

  {
    std::vector<ManagedInstance> expected_servers {
      // ServerMode doesn't matter ------vvvvvvvvvvv
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-4", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-5", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-6", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-7", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-*) defined in metadata not found in actual replicaset"
    // for instanes 4-7
  }

  {
    std::vector<ManagedInstance> expected_servers {
      // ServerMode doesn't matter ------vvvvvvvvvvv
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-4", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-5", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-6", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-*) defined in metadata not found in actual replicaset"
    // for instanes 4-6
  }

  {
    std::vector<ManagedInstance> expected_servers {
      // ServerMode doesn't matter ------vvvvvvvvvvv
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-4", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-5", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-*) defined in metadata not found in actual replicaset"
    // for instanes 4-5
  }

  {
    std::vector<ManagedInstance> expected_servers {
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-4", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
    // should log warning "Member <host>:<port> (instance-4) defined in metadata not found in actual replicaset"
  }

  {
    std::vector<ManagedInstance> expected_servers {
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(2).mode);
  }

  // 2-node setup - TODO: do we like this behaviour?
  {
    std::vector<ManagedInstance> expected_servers {
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
      {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
    // instance-3 will be silently ignored
  }

  // 1-node setup - TODO: do we like this behaviour?
  {
    std::vector<ManagedInstance> expected_servers {
      {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    };
    EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
    EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
    // instance-2 and -3 will be silently ignored
  }

  {
    std::vector<ManagedInstance> expected_servers {};
    EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
    // instance-1, -2 and -3 will be silently ignored
  }

}
#endif

TEST_F(MetadataTest, CheckReplicasetStatus_VariousStatuses) {

  std::vector<ManagedInstance> expected_servers {
    // ServerMode doesn't matter ------vvvvvvvvvvv
    {"", "instance-1", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    {"", "instance-2", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
    {"", "instance-3", "", ServerMode::Unavailable, 0, 0, "", "", 0, 0},
  };

//TODO fix, Role::Other has been removed
//// should keep quorum
//{
//  std::map<std::string, GroupReplicationMember> server_status {
//    { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
//    { "instance-2", {"", "", 0, State::Online, Role::Secondary} },
//    { "instance-3", {"", "", 0, State::Online, Role::Other    } },
//  };
//  EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
//  EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
//  EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
//  EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
//}

//// should lose quorum
//{
//  std::map<std::string, GroupReplicationMember> server_status {
//    { "instance-1", {"", "", 0, State::Online, Role::Primary  } },
//    { "instance-2", {"", "", 0, State::Online, Role::Other    } },
//    { "instance-3", {"", "", 0, State::Online, Role::Other    } },
//  };
//  EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
//  EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
//  EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(1).mode);
//  EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
//}

  for (State state : {State::Offline, State::Recovering, State::Unreachable, State::Other}) {

    // should keep quorum
    {
      std::map<std::string, GroupReplicationMember> server_status {
        { "instance-1", {"", "", 0, State::Online,  Role::Primary  } },
        { "instance-2", {"", "", 0, State::Online,  Role::Secondary} },
        { "instance-3", {"", "", 0, state,          Role::Secondary} },
      };
      EXPECT_EQ(RS::AvailableWritable, metadata.check_replicaset_status(expected_servers, server_status));
      EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
      EXPECT_EQ(ServerMode::ReadOnly,    expected_servers.at(1).mode);
      EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
    }

    // should lose quorum
    {
      std::map<std::string, GroupReplicationMember> server_status {
        { "instance-1", {"", "", 0, State::Online,  Role::Primary  } },
        { "instance-2", {"", "", 0, state,          Role::Secondary} },
        { "instance-3", {"", "", 0, state,          Role::Secondary} },
      };
      EXPECT_EQ(RS::Unavailable, metadata.check_replicaset_status(expected_servers, server_status));
      EXPECT_EQ(ServerMode::ReadWrite,   expected_servers.at(0).mode);
      EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(1).mode);
      EXPECT_EQ(ServerMode::Unavailable, expected_servers.at(2).mode);
    }
  }
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::update_replicaset_status() - connection failures
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, UpdateReplicasetStatus_PrimaryMember_FailConnectOnNode2) {

  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member FAILS
  //   iteration 2 (instance-2): CAN'T CONNECT
  //   iteration 3 (instance-3): query_primary_member OK, query_status OK

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member should go to existing connection (shared with metadata server) -> make the query fail
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));

  // since 1st query_primary_member failed, update_replicaset_status() should try to connect to
  // instance-2. Let's make that new connections fail by NOT using enable_connection(session)
  //enable_connection(++session, 3320); // we don't call this on purpose
  EXPECT_CALL(session_factory.get(++session), flag_fail(_, 3320)).Times(1);

  // Next
  // instance-2. Let's make that new connections fail by NOT using enable_connection(session)
  //enable_connection(++session, 3320); // we don't call this on purpose

  // since 2nd connection failed, update_replicaset_status() should try to connect to instance-3.
  // Let's allow this.
  enable_connection(++session, 3330);

  // 3rd query_primary_member: let's return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 3rd query_status: let's return good data
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_fail(session)));

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  try {
    metadata.update_replicaset_status("replicaset-1", typical_instances);
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  } catch (const metadata_cache::metadata_error& e) {
    EXPECT_STREQ("Unable to fetch live group_replication member data from any server in replicaset 'replicaset-1'", e.what());
  } catch (...) {
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  }

  EXPECT_EQ(3, session_factory.create_cnt());          // +2 from new connections to localhost:3320 and :3330
}

TEST_F(MetadataTest, UpdateReplicasetStatus_PrimaryMember_FailConnectOnAllNodes) {

  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member FAILS
  //   iteration 2 (instance-2): CAN'T CONNECT
  //   iteration 3 (instance-3): CAN'T CONNECT

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member should go to existing connection (shared with metadata server) -> make the query fail
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));

  // since 1st query_primary_member failed, update_replicaset_status() should try to connect to
  // instance-2, then instance-3. Let's make those new connections fail by NOT using enable_connection(session)
  EXPECT_CALL(session_factory.get(++session), flag_fail(_, 3320)).Times(1);
  EXPECT_CALL(session_factory.get(++session), flag_fail(_, 3330)).Times(1);

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  try {
    metadata.update_replicaset_status("replicaset-1", typical_instances);
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  } catch (const metadata_cache::metadata_error& e) {
    EXPECT_STREQ("Unable to fetch live group_replication member data from any server in replicaset 'replicaset-1'", e.what());
  } catch (...) {
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  }

  EXPECT_EQ(3, session_factory.create_cnt());          // +2 from new connections to localhost:3320 and :3330
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::update_replicaset_status() - query_primary_member failures
// [QUERY #2: query_primary_member]
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, UpdateReplicasetStatus_PrimaryMember_FailQueryOnNode1) {

  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member FAILS
  //   iteration 2 (instance-2): query_primary_member OK, query_status OK

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member should go to existing connection (shared with metadata server) -> make the query fail
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));

  // since 1st query_primary_member failed, 2nd should to to instance-2 -> make it succeed.
  // Note that the connection to instance-2 has to be created first
  enable_connection(++session, 3320);

  // 2nd query_primary_member: let's return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 2nd query_status: let's return good data
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_ok(session)));

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  EXPECT_NO_THROW(metadata.update_replicaset_status("replicaset-1", typical_instances));

  EXPECT_EQ(2, session_factory.create_cnt());          // +1 from new connection to localhost:3320 (instance-2)

  // query_status reported back from instance-2
  EXPECT_EQ(3u, typical_instances.size());
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100}, typical_instances.at(0)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3320, 33200}, typical_instances.at(1)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300}, typical_instances.at(2)));
}

TEST_F(MetadataTest, UpdateReplicasetStatus_PrimaryMember_FailQueryOnAllNodes) {

  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member FAILS
  //   iteration 2 (instance-2): query_primary_member FAILS
  //   iteration 3 (instance-3): query_primary_member FAILS

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member should go to existing connection (shared with metadata server) -> make the query fail
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));

  // since 1st query_primary_member failed, should issue 2nd query to instance-2 -> also make the query fail
  // Note that the connection to instance-2 has to be created first
  enable_connection(++session, 3320);

  // 2nd query_primary_member: let's fail again
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));

  // since 2nd query_primary_member failed, should issue 3rd query to instance-3 -> also make the query fail
  // Note that the connection to instance-3 has to be created first
  enable_connection(++session, 3330);

  // 3rd query_primary_member: let's fail again
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  try {
    metadata.update_replicaset_status("replicaset-1", typical_instances);
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  } catch (const metadata_cache::metadata_error& e) {
    EXPECT_STREQ("Unable to fetch live group_replication member data from any server in replicaset 'replicaset-1'", e.what());
  } catch (...) {
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  }

  EXPECT_EQ(3, session_factory.create_cnt());          // +2 from new connections to localhost:3320 and :3330
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::update_replicaset_status() - query_status failures
// [QUERY #3: query_status]
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, UpdateReplicasetStatus_Status_FailQueryOnNode1) {

  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member OK, query_status FAILS
  //   iteration 2 (instance-2): query_primary_member OK, query_status OK

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member: let's return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 1st query_status: let's fail the query
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_fail(session)));

  // since 1st query_status failed, update_replicaset_status() should start another iteration, but on instance-2 this time.
  // Note that the connection to instance-2 has to be created first
  enable_connection(++session, 3320);

  // 2nd query_primary_member: let's again return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 2nd query_status: let's return good data
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_ok(session)));

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  EXPECT_NO_THROW(metadata.update_replicaset_status("replicaset-1", typical_instances));

  EXPECT_EQ(2, session_factory.create_cnt());          // +1 from new connection to localhost:3320 (instance-2)

  // query_status reported back from instance-1
  EXPECT_EQ(3u, typical_instances.size());
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100}, typical_instances.at(0)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3320, 33200}, typical_instances.at(1)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300}, typical_instances.at(2)));
}

TEST_F(MetadataTest, UpdateReplicasetStatus_Status_FailQueryOnAllNodes) {

  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member OK, query_status FAILS
  //   iteration 2 (instance-2): query_primary_member OK, query_status FAILS
  //   iteration 2 (instance-2): query_primary_member OK, query_status FAILS

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member: let's return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 1st query_status: let's fail the query
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_fail(session)));

  // since 1st query_status failed, update_replicaset_status() should start another iteration, but on instance-2 this time.
  // Note that the connection to instance-2 has to be created first
  enable_connection(++session, 3320);

  // 2nd query_primary_member: let's again return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 2nd query_status: let's fail the query
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_fail(session)));

  // since 2st query_status failed, update_replicaset_status() should start another iteration, but on instance-3 this time.
  // Note that the connection to instance-3 has to be created first
  enable_connection(++session, 3330);

  // 3rd query_primary_member: let's again return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 3rd query_status: let's fail the query
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_fail(session)));

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  try {
    metadata.update_replicaset_status("replicaset-1", typical_instances);
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  } catch (const metadata_cache::metadata_error& e) {
    EXPECT_STREQ("Unable to fetch live group_replication member data from any server in replicaset 'replicaset-1'", e.what());
  } catch (...) {
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  }

  EXPECT_EQ(3, session_factory.create_cnt());          // +2 from new connections to localhost:3320 and :3330
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::update_replicaset_status() - success scenarios
// [QUERY #2 + #3]
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, UpdateReplicasetStatus_SimpleSunnyDayScenario) {
  connect_to_first_metadata_server();

  // TEST SCENARIO:
  //   iteration 1 (instance-1): query_primary_member OK, query_status OK

  // update_replicaset_status() first iteration: all requests go to existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  // 1st query_primary_member: let's return "instance-1"
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));

  // 1st query_status as seen from instance-1
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_ok(session)));

  EXPECT_EQ(1, session_factory.create_cnt());          // caused by connect_to_first_metadata_server()

  EXPECT_NO_THROW(metadata.update_replicaset_status("replicaset-1", typical_instances));

  EXPECT_EQ(1, session_factory.create_cnt());          // should resuse localhost:3310 connection,

  // query_status reported back from instance-1
  EXPECT_EQ(3u, typical_instances.size());
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100}, typical_instances.at(0)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-2", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3320, 33200}, typical_instances.at(1)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-3", "", ServerMode::ReadOnly,  0, 0, "", "localhost", 3330, 33300}, typical_instances.at(2)));
}



////////////////////////////////////////////////////////////////////////////////
//
// test ClusterMetadata::fetch_instances()
// (this is the highest-level function, it calls everything tested above
// except connect() (which is a separate step))
//
// TODO add tests for multiple replicasets here, when we begin supporting them
//
////////////////////////////////////////////////////////////////////////////////

TEST_F(MetadataTest, FetchInstances_1Replicaset_ok) {

  connect_to_first_metadata_server();

  // update_replicaset_status() first iteration: all requests go to existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  auto resultset_metadata = [this](const std::string&, const MySQLSession::RowProcessor& processor) {
    session_factory.get(0).query_impl(processor, {
      {"replicaset-1", "instance-1", "HA", NULL, NULL, "blabla", "localhost:3310", NULL},
      {"replicaset-1", "instance-2", "HA", NULL, NULL, "blabla", "localhost:3320", NULL},
      {"replicaset-1", "instance-3", "HA", NULL, NULL, "blabla", "localhost:3330", NULL},
    });
  };
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_metadata), _)).Times(1)
    .WillOnce(Invoke(resultset_metadata));
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_ok(session)));
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_status), _)).Times(1)
    .WillOnce(Invoke(query_status_ok(session)));

  ClusterMetadata::InstancesByReplicaSet rs = metadata.fetch_instances("replicaset-1");

  EXPECT_EQ(1u, rs.size());
  EXPECT_EQ(3u, rs.at("replicaset-1").size());
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-1", "", ServerMode::ReadWrite, 0, 0, "", "localhost", 3310, 33100}, rs.at("replicaset-1").at(0)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-2", "", ServerMode::ReadOnly, 0, 0, "", "localhost", 3320, 33200}, rs.at("replicaset-1").at(1)));
  EXPECT_TRUE(cmp_mi_FI(ManagedInstance{"replicaset-1", "instance-3", "", ServerMode::ReadOnly, 0, 0, "", "localhost", 3330, 33300}, rs.at("replicaset-1").at(2)));
}

TEST_F(MetadataTest, FetchInstances_1Replicaset_fail) {

  connect_to_first_metadata_server();

  // update_replicaset_status() first iteration: requests start with existing connection to instance-1 (shared with metadata server)
  unsigned session = 0;

  auto resultset_metadata = [this](const std::string&, const MySQLSession::RowProcessor& processor) {
    session_factory.get(0).query_impl(processor, {
      {"replicaset-1", "instance-1", "HA", NULL, NULL, "blabla", "localhost:3310", NULL},
      {"replicaset-1", "instance-2", "HA", NULL, NULL, "blabla", "localhost:3320", NULL},
      {"replicaset-1", "instance-3", "HA", NULL, NULL, "blabla", "localhost:3330", NULL},
    });
  };
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_metadata), _)).Times(1)
    .WillOnce(Invoke(resultset_metadata));

  // fail query_primary_member, then further connections
  EXPECT_CALL(session_factory.get(session), query(StartsWith(query_primary_member), _)).Times(1)
    .WillOnce(Invoke(query_primary_member_fail(session)));
  EXPECT_CALL(session_factory.get(++session), flag_fail(_, 3320)).Times(1);
  EXPECT_CALL(session_factory.get(++session), flag_fail(_, 3330)).Times(1);

  // should throw
  try {
    metadata.fetch_instances("replicaset-1");
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  } catch (const metadata_cache::metadata_error& e) {
    EXPECT_STREQ("Unable to fetch live group_replication member data from any server in replicaset 'replicaset-1'", e.what());
  } catch (...) {
    FAIL() << "Expected metadata_cache::metadata_error to be thrown";
  }
}