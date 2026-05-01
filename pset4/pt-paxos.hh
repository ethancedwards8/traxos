#pragma once

#include "client_model.hh"
#include "netsim.hh"
#include "pancydb.hh"
#include "paxos.hh"
#include "random_source.hh"
#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// testinfo
//    Holds configuration information about this test.

enum failure_mode {
    failed_leader,
    failed_replica,
    multiple_random_up_down,
    unstable_leader_mixed,
    random_failure_schedule,
    william_link_schedule,
    disruptive_isolate,
    split_brain,
    minority_partition_failure,
    vihaan_split_brain_failure,
    delayed_leader_failure,
    cascading_star_partition,
    split_brain_isolate_heal,
    none,
};

struct testinfo {
    random_source randomness;
    double loss = 0.01;
    bool verbose = false;
    bool print_db = false;
    size_t nreplicas = 3;
    size_t initial_leader = 0;

    int failed_replica = -1;
    failure_mode mode = failure_mode::none;
    std::vector<size_t> excluded_replicas;

    template <typename T>
    void configure_port(netsim::port<T>& port) {
        port.set_verbose(verbose);
    }
    template <typename T>
    void configure_channel(netsim::channel<T>& chan) {
        chan.set_loss(loss);
        chan.set_verbose(verbose);
    }
    template <typename T>
    void configure_quiet_channel(netsim::channel<T>& chan) {
        chan.set_loss(loss);
    }
};


// pt_paxos_replica, pt_paxos_instance
//    Manage a test of a Paxos-based Pancy service.
//    Initialization is more complicated than in the simpler settings;
//    we have a type, `pt_paxos_replica`, that represents a single replica,
//    and another, `pt_paxos_instance`, that constructs the replica set.

struct pt_paxos_instance;

struct pt_paxos_replica {
    size_t index_;           // index of this replica in the replica set
    size_t nreplicas_;       // number of replicas
    size_t leader_index_;    // this replica’s idea of the current leader
    netsim::port<pancy::request> from_clients_;   // port for client messages
    netsim::port<paxos_message> from_replicas_;   // port for inter-replica messages
    netsim::channel<pancy::response> to_clients_; // channel for client responses
    // channels for inter-replica messages:
    std::vector<std::unique_ptr<netsim::channel<paxos_message>>> to_replicas_;
    pancy::pancydb db_;      // our copy of the database

    // ...plus anything you want to add
    unsigned long long next_round_ = 1;
    unsigned long long promised_round_ = 0;
    unsigned long long accepted_round_ = 0;
    std::deque<pancy::request> accepted_values_;
    std::deque<unsigned long long> accepted_rounds_;
    unsigned long long commit_index_ = 0;
    unsigned long long applied_index_ = 0;
    std::vector<unsigned long long> match_index_;
    std::unordered_map<uint64_t, pancy::response> client_response_cache_;
    std::unordered_set<uint64_t> pending_client_serials_;
    cotamer::duration heartbeat_interval_ = std::chrono::milliseconds{200};
    cotamer::duration failure_timeout_;

    pt_paxos_replica(size_t index, size_t nreplicas, random_source&);
    void initialize(pt_paxos_instance&);

    cotamer::task<> run();
    cotamer::task<> run_as_leader();
    cotamer::task<> run_as_follower();
    cotamer::task<> send_to_other_replicas(const paxos_message& msg);
    unsigned long long reserve_next_round();
    void advance_next_round_past(unsigned long long round);
    ack_msg make_ack(unsigned long long round, bool success) const;
    void apply_committed_up_to(unsigned long long committed_slot);

private:
    unsigned long quorum_ = nreplicas_ / 2 + 1;
};

struct pt_paxos_instance {
    testinfo& tester;
    client_model& clients;
    std::vector<std::unique_ptr<pt_paxos_replica>> replicas;
    // ...plus anything you want to add

    pt_paxos_instance(testinfo&, client_model&);
};
