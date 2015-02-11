#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "fetcher/mock_continuous_fetcher.h"
#include "log/cluster_state_controller-inl.h"
#include "log/logged_certificate.h"
#include "log/test_db.h"
#include "proto/ct.pb.h"
#include "util/fake_etcd.h"
#include "util/libevent_wrapper.h"
#include "util/mock_masterelection.h"
#include "util/testing.h"
#include "util/util.h"

using ct::ClusterConfig;
using ct::ClusterNodeState;
using ct::SignedTreeHead;
using std::make_shared;
using std::shared_ptr;
using std::string;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;
using testing::_;
using util::StatusOr;

namespace cert_trans {

const char kNodeId1[] = "node1";
const char kNodeId2[] = "node2";
const char kNodeId3[] = "node3";


class ClusterStateControllerTest : public ::testing::Test {
 public:
  ClusterStateControllerTest()
      : pool_(2),
        base_(make_shared<libevent::Base>()),
        pump_(base_),
        etcd_(base_),
        store1_(new EtcdConsistentStore<LoggedCertificate>(&pool_, &etcd_,
                                                           &election1_, "",
                                                           kNodeId1)),
        store2_(new EtcdConsistentStore<LoggedCertificate>(&pool_, &etcd_,
                                                           &election2_, "",
                                                           kNodeId2)),
        store3_(new EtcdConsistentStore<LoggedCertificate>(&pool_, &etcd_,
                                                           &election3_, "",
                                                           kNodeId3)),
        controller_(&pool_, base_, test_db_.db(), store1_.get(), &election1_,
                    &fetcher_) {
    // Set default cluster config:
    ct::ClusterConfig default_config;
    default_config.set_minimum_serving_nodes(1);
    default_config.set_minimum_serving_fraction(1);
    store1_->SetClusterConfig(default_config);

    controller_.SetNodeHostPort(kNodeId1, 9001);

    // Set up some handy STHs
    sth100_.set_tree_size(100);
    sth100_.set_timestamp(100);
    sth200_.set_tree_size(200);
    sth200_.set_timestamp(200);
    sth300_.set_tree_size(300);
    sth300_.set_timestamp(300);

    cns100_.set_hostname(kNodeId1);
    cns100_.set_log_port(9001);
    cns100_.mutable_newest_sth()->CopyFrom(sth100_);

    cns200_.set_hostname(kNodeId2);
    cns200_.set_log_port(9001);
    cns200_.mutable_newest_sth()->CopyFrom(sth200_);

    cns300_.set_hostname(kNodeId3);
    cns300_.set_log_port(9001);
    cns300_.mutable_newest_sth()->CopyFrom(sth300_);
  }

 protected:
  ct::ClusterNodeState GetLocalState() {
    return controller_.local_node_state_;
  }

  ct::ClusterNodeState GetNodeStateView(const string& node_id) {
    auto it(controller_.all_peers_.find("/nodes/" + node_id));
    CHECK(it != controller_.all_peers_.end());
    return it->second->state();
  }

  static void SetClusterConfig(ConsistentStore<LoggedCertificate>* store,
                               const int min_nodes,
                               const double min_fraction) {
    ClusterConfig config;
    config.set_minimum_serving_nodes(min_nodes);
    config.set_minimum_serving_fraction(min_fraction);
    CHECK(store->SetClusterConfig(config).ok());
  }


  SignedTreeHead sth100_, sth200_, sth300_;
  ClusterNodeState cns100_, cns200_, cns300_;

  ThreadPool pool_;
  shared_ptr<libevent::Base> base_;
  StrictMock<MockContinuousFetcher> fetcher_;
  libevent::EventPumpThread pump_;
  FakeEtcdClient etcd_;
  TestDB<FileDB<LoggedCertificate>> test_db_;
  NiceMock<MockMasterElection> election1_;
  NiceMock<MockMasterElection> election2_;
  NiceMock<MockMasterElection> election3_;
  std::unique_ptr<EtcdConsistentStore<LoggedCertificate>> store1_;
  std::unique_ptr<EtcdConsistentStore<LoggedCertificate>> store2_;
  std::unique_ptr<EtcdConsistentStore<LoggedCertificate>> store3_;
  ClusterStateController<LoggedCertificate> controller_;
};


typedef class EtcdConsistentStoreTest EtcdConsistentStoreDeathTest;


TEST_F(ClusterStateControllerTest, TestNewTreeHead) {
  ct::SignedTreeHead sth;
  sth.set_tree_size(234);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(1);
  controller_.NewTreeHead(sth);
  EXPECT_EQ(sth.DebugString(), GetLocalState().newest_sth().DebugString());
}


TEST_F(ClusterStateControllerTest, TestCalculateServingSTHAt50Percent) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller50(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 1 /* nodes */, 0.5 /* fraction */);

  store1_->SetClusterNodeState(cns100_);
  sleep(1);
  util::StatusOr<SignedTreeHead> sth(controller50.GetCalculatedServingSTH());
  // Can serve sth1 because all nodes have it !
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());

  store2_->SetClusterNodeState(cns200_);
  sleep(1);
  // Can serve sth2 because 50% of nodes have it
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  store3_->SetClusterNodeState(cns300_);
  sleep(1);
  // Can serve sth2 because 66% of nodes have it (or higher)
  // Can't serve sth3 because only 33% of nodes cover it.
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());
}


TEST_F(ClusterStateControllerTest, TestCalculateServingSTHAt70Percent) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller70(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 1 /* nodes */, 0.7 /* fraction */);
  store1_->SetClusterNodeState(cns100_);
  sleep(1);
  util::StatusOr<SignedTreeHead> sth(controller70.GetCalculatedServingSTH());
  // Can serve sth1 because all nodes have it !
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());

  store2_->SetClusterNodeState(cns200_);
  sleep(1);
  // Can still only serve sth1 because only 50% of nodes have sth2
  sth = controller70.GetCalculatedServingSTH();
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());

  store3_->SetClusterNodeState(cns300_);
  sleep(1);
  // Can still only serve sth1 because only 66% of nodes have sth2
  sth = controller70.GetCalculatedServingSTH();
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());
}


TEST_F(ClusterStateControllerTest,
       TestCalculateServingSTHAt60PercentTwoNodeMin) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller60(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 2 /* nodes */, 0.6 /* fraction */);
  store1_->SetClusterNodeState(cns100_);
  sleep(1);
  util::StatusOr<SignedTreeHead> sth(controller60.GetCalculatedServingSTH());
  // Can't serve at all because not enough nodes
  EXPECT_FALSE(sth.ok());

  store2_->SetClusterNodeState(cns200_);
  sleep(1);
  // Can serve sth1 because there are two nodes, but < 60% coverage for sth2
  sth = controller60.GetCalculatedServingSTH();
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());

  store3_->SetClusterNodeState(cns300_);
  sleep(1);
  sth = controller60.GetCalculatedServingSTH();
  // Can serve sth2 because there are two out of three nodes with sth2 or above
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());
}


TEST_F(ClusterStateControllerTest, TestCalculateServingSTHAsClusterMoves) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller50(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 1 /* nodes */, 0.5 /* fraction */);
  ct::ClusterNodeState node_state(cns100_);
  store1_->SetClusterNodeState(node_state);
  node_state.set_hostname(kNodeId2);
  store2_->SetClusterNodeState(node_state);
  node_state.set_hostname(kNodeId3);
  store3_->SetClusterNodeState(node_state);
  sleep(1);
  util::StatusOr<SignedTreeHead> sth(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());

  node_state = cns200_;
  node_state.set_hostname(kNodeId1);
  store1_->SetClusterNodeState(node_state);
  sleep(1);
  // Node1@200
  // Node2 and Node3 @100:
  // Still have to serve at sth100
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth100_.tree_size(), sth.ValueOrDie().tree_size());

  node_state.set_hostname(kNodeId3);
  store3_->SetClusterNodeState(node_state);
  sleep(1);
  // Node1 and Node3 @200
  // Node2 @100:
  // Can serve at sth200
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  node_state = cns300_;
  node_state.set_hostname(kNodeId2);
  store2_->SetClusterNodeState(node_state);
  sleep(1);
  // Node1 and Node3 @200
  // Node2 @300:
  // Still have to serve at sth200
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());
}


TEST_F(ClusterStateControllerTest, TestKeepsNewerSTH) {
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(1);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(1);

  store1_->SetClusterNodeState(cns100_);

  // Create a node with an identically sized but newer STH:
  SignedTreeHead newer_sth(sth100_);
  newer_sth.set_timestamp(newer_sth.timestamp() + 1);
  ClusterNodeState newer_cns;
  newer_cns.set_hostname("somenode.example.net");
  newer_cns.set_log_port(9001);
  *newer_cns.mutable_newest_sth() = newer_sth;
  store2_->SetClusterNodeState(newer_cns);
  sleep(1);

  util::StatusOr<SignedTreeHead> sth(controller_.GetCalculatedServingSTH());
  EXPECT_EQ(newer_sth.tree_size(), sth.ValueOrDie().tree_size());
  EXPECT_EQ(newer_sth.timestamp(), sth.ValueOrDie().timestamp());
}


TEST_F(ClusterStateControllerTest, TestCannotSelectSmallerSTH) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller50(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 1 /* nodes */, 0.5 /* fraction */);

  ct::ClusterNodeState node_state(cns200_);
  node_state.set_hostname(kNodeId1);
  store1_->SetClusterNodeState(node_state);
  node_state.set_hostname(kNodeId2);
  store2_->SetClusterNodeState(node_state);
  node_state.set_hostname(kNodeId3);
  store3_->SetClusterNodeState(node_state);
  sleep(1);
  util::StatusOr<SignedTreeHead> sth(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  node_state = cns100_;
  node_state.set_hostname(kNodeId1);
  store1_->SetClusterNodeState(node_state);
  sleep(1);
  // Node1@100
  // Node2 and Node3 @200:
  // Still have to serve at sth200
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  node_state.set_hostname(kNodeId3);
  store3_->SetClusterNodeState(node_state);
  sleep(1);
  // Node1 and Node3 @100
  // Node2 @200
  // But cannot select an earlier STH than the one we last served with, so must
  // stick with sth200:
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  node_state.set_hostname(kNodeId2);
  store2_->SetClusterNodeState(node_state);
  sleep(1);
  // Still have to serve at sth200
  sth = controller50.GetCalculatedServingSTH();
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());
}


TEST_F(ClusterStateControllerTest, TestUsesLargestSTHWithIdenticalTimestamp) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller50(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 1 /* nodes */, 0.5 /* fraction */);

  ClusterNodeState cns1;
  cns1.set_hostname(kNodeId1);
  cns1.set_log_port(9001);
  cns1.mutable_newest_sth()->set_timestamp(1000);
  cns1.mutable_newest_sth()->set_tree_size(1000);
  store1_->SetClusterNodeState(cns1);

  ClusterNodeState cns2(cns1);
  cns2.set_hostname(kNodeId2);
  cns2.set_log_port(9001);
  cns2.mutable_newest_sth()->set_timestamp(1000);
  cns2.mutable_newest_sth()->set_tree_size(1001);
  store2_->SetClusterNodeState(cns2);

  ClusterNodeState cns3;
  cns3.set_hostname(kNodeId3);
  cns3.set_log_port(9001);
  cns3.mutable_newest_sth()->set_timestamp(1004);
  cns3.mutable_newest_sth()->set_tree_size(999);
  store3_->SetClusterNodeState(cns3);
  sleep(1);

  util::StatusOr<SignedTreeHead> sth(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(cns2.newest_sth().tree_size(), sth.ValueOrDie().tree_size());
  EXPECT_EQ(cns2.newest_sth().timestamp(), sth.ValueOrDie().timestamp());
}


TEST_F(ClusterStateControllerTest, TestDoesNotReuseSTHTimestamp) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller50(&pool_, base_,
                                                         test_db_.db(),
                                                         store1_.get(),
                                                         &election_is_master,
                                                         &fetcher_);
  SetClusterConfig(store1_.get(), 3 /* nodes */, 1 /* fraction */);

  ClusterNodeState cns1;
  cns1.set_hostname(kNodeId1);
  cns1.set_log_port(9001);
  cns1.mutable_newest_sth()->set_timestamp(1002);
  cns1.mutable_newest_sth()->set_tree_size(10);
  store1_->SetClusterNodeState(cns1);

  ClusterNodeState cns2(cns1);
  cns2.set_hostname(kNodeId2);
  cns2.set_log_port(9001);
  cns2.mutable_newest_sth()->set_timestamp(1000);
  cns2.mutable_newest_sth()->set_tree_size(11);
  store2_->SetClusterNodeState(cns2);

  ClusterNodeState cns3;
  cns3.set_hostname(kNodeId3);
  cns3.set_log_port(9001);
  cns3.mutable_newest_sth()->set_timestamp(1002);
  cns3.mutable_newest_sth()->set_tree_size(9);
  store3_->SetClusterNodeState(cns3);
  sleep(1);

  // Have to choose cns3 (9@1002) here because we need 100% coverage:
  util::StatusOr<SignedTreeHead> sth1(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(cns3.newest_sth().tree_size(), sth1.ValueOrDie().tree_size());
  EXPECT_EQ(cns3.newest_sth().timestamp(), sth1.ValueOrDie().timestamp());

  // Now cns3 moves to 13@1004
  cns3.mutable_newest_sth()->set_timestamp(1004);
  cns3.mutable_newest_sth()->set_tree_size(13);
  store3_->SetClusterNodeState(cns3);
  sleep(1);

  // Which means that the only STH from the current set that we can serve
  // must be 10@1002 (because coverage).
  // However, that timestamp was already used above, so the serving STH can't
  // have changed:
  util::StatusOr<SignedTreeHead> sth2(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(sth1.ValueOrDie().DebugString(), sth2.ValueOrDie().DebugString());

  // Now cns1 moves to 13@1003
  cns3.mutable_newest_sth()->set_timestamp(1003);
  cns3.mutable_newest_sth()->set_tree_size(13);
  store3_->SetClusterNodeState(cns3);
  sleep(1);

  // Which means that the only STH from the current set that we can serve
  // must be 11@1000 (because coverage).
  // But that's in the past compared to Serving STH, so no dice.
  util::StatusOr<SignedTreeHead> sth3(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(sth1.ValueOrDie().DebugString(), sth3.ValueOrDie().DebugString());

  // Finally cns2 moves to 13@1006
  cns2.mutable_newest_sth()->set_timestamp(1006);
  cns2.mutable_newest_sth()->set_tree_size(13);
  store2_->SetClusterNodeState(cns2);
  // And cns1 moves to 16@1003
  cns1.mutable_newest_sth()->set_timestamp(1006);
  cns1.mutable_newest_sth()->set_tree_size(13);
  store1_->SetClusterNodeState(cns1);
  sleep(1);

  // And we've got: 16@1002, 13@1006, 13@1003
  // So the cluster can move forward with its Serving STH
  util::StatusOr<SignedTreeHead> sth4(controller50.GetCalculatedServingSTH());
  EXPECT_EQ(cns2.newest_sth().tree_size(), sth4.ValueOrDie().tree_size());
  EXPECT_EQ(cns2.newest_sth().timestamp(), sth4.ValueOrDie().timestamp());
}


TEST_F(ClusterStateControllerTest,
       TestConfigChangesCauseServingSTHToBeRecalculated) {
  NiceMock<MockMasterElection> election_is_master;
  EXPECT_CALL(election_is_master, IsMaster()).WillRepeatedly(Return(true));
  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId2, _)).Times(2);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId3, _)).Times(2);
  ClusterStateController<LoggedCertificate> controller(&pool_, base_,
                                                       test_db_.db(),
                                                       store1_.get(),
                                                       &election_is_master,
                                                       &fetcher_);
  SetClusterConfig(store1_.get(), 0 /* nodes */, 0.5 /* fraction */);
  store1_->SetClusterNodeState(cns100_);
  store2_->SetClusterNodeState(cns200_);
  store3_->SetClusterNodeState(cns300_);
  sleep(1);
  StatusOr<SignedTreeHead> sth(controller.GetCalculatedServingSTH());
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  SetClusterConfig(store1_.get(), 0 /* nodes */, 0.9 /* fraction */);
  sleep(1);
  sth = controller.GetCalculatedServingSTH();
  // You might expect sth100 here, but we shouldn't move to a smaller STH
  EXPECT_EQ(sth200_.tree_size(), sth.ValueOrDie().tree_size());

  SetClusterConfig(store1_.get(), 0 /* nodes */, 0.3 /* fraction */);
  sleep(1);
  sth = controller.GetCalculatedServingSTH();
  // Should be able to move to sth300 now.
  EXPECT_EQ(sth300_.tree_size(), sth.ValueOrDie().tree_size());
}


TEST_F(ClusterStateControllerTest, TestGetLocalNodeState) {
  SignedTreeHead sth;
  sth.set_timestamp(10000);
  sth.set_tree_size(2344);
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(1);
  controller_.NewTreeHead(sth);

  ClusterNodeState state;
  controller_.GetLocalNodeState(&state);
  EXPECT_EQ(sth.DebugString(), state.newest_sth().DebugString());
}


TEST_F(ClusterStateControllerTest, TestLeavesElectionIfDoesNotHaveLocalData) {
  const int kTreeSize(2345);
  const int kTreeSizeSmaller(kTreeSize - 1);
  const int kTreeSizeLarger(kTreeSize + 1);

  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(1);

  {
    SignedTreeHead local_sth;
    local_sth.set_timestamp(10000);
    local_sth.set_tree_size(kTreeSizeSmaller);
    controller_.NewTreeHead(local_sth);
    sleep(1);
  }

  SignedTreeHead sth;
  {
    EXPECT_CALL(election1_, StartElection()).Times(1);
    sth.set_timestamp(10000);
    sth.set_tree_size(kTreeSizeSmaller);
    store1_->SetServingSTH(sth);
    sleep(1);
  }

  {
    EXPECT_CALL(election1_, StopElection()).Times(1);
    sth.set_timestamp(sth.timestamp() + 1);
    sth.set_tree_size(kTreeSizeLarger);
    store1_->SetServingSTH(sth);
    sleep(1);
  }
}


TEST_F(ClusterStateControllerTest, TestJoinsElectionIfHasLocalData) {
  const int kTreeSizeSmaller(2345);
  const int kTreeSizeLarger(kTreeSizeSmaller + 1);

  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(1);

  {
    SignedTreeHead local_sth;
    local_sth.set_timestamp(10000);
    local_sth.set_tree_size(kTreeSizeSmaller);
    controller_.NewTreeHead(local_sth);
    sleep(1);
  }

  SignedTreeHead sth;
  sth.set_timestamp(10000);
  sth.set_tree_size(kTreeSizeSmaller - 10);
  store1_->SetServingSTH(sth);
  sleep(1);

  {
    EXPECT_CALL(election1_, StartElection()).Times(1);
    sth.set_timestamp(sth.timestamp() + 1);
    sth.set_tree_size(kTreeSizeLarger);
    controller_.NewTreeHead(sth);
    sleep(1);
  }
}


TEST_F(ClusterStateControllerTest, TestNodeHostPort) {
  const string kHost("myhostname");
  const int kPort(9999);

  // Calls to the continuous fetcher are duplicated, because there are
  // two ClusterStateController instances in this test.
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(2);

  controller_.SetNodeHostPort(kHost, kPort);
  sleep(1);

  const ClusterNodeState node_state(GetNodeStateView(kNodeId1));
  EXPECT_EQ(kHost, node_state.hostname());
  EXPECT_EQ(kPort, node_state.log_port());
}


TEST_F(ClusterStateControllerTest, TestStoresServingSthInDatabase) {
  EXPECT_CALL(fetcher_, AddPeer(string("/nodes/") + kNodeId1, _)).Times(1);

  SignedTreeHead sth;
  sth.set_timestamp(10000);
  sth.set_tree_size(2000);
  store1_->SetServingSTH(sth);
  sleep(1);

  {
    SignedTreeHead db_sth;
    EXPECT_EQ(Database<LoggedCertificate>::LOOKUP_OK,
              test_db_.db()->LatestTreeHead(&db_sth));
    EXPECT_EQ(sth.DebugString(), db_sth.DebugString());
  }
}


}  // namespace cert_trans


int main(int argc, char** argv) {
  cert_trans::test::InitTesting(argv[0], &argc, &argv, true);
  return RUN_ALL_TESTS();
}