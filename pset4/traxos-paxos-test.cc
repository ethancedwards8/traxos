#include "netsim.hh"
#include "random_source.hh"
#include "traxos-paxos-msgs.hh"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <deque>
#include <format>
#include <getopt.h>
#include <cstring>
#include <iostream>
#include <optional>
#include <print>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace cot = cotamer;
using namespace std::chrono_literals;

enum class failure_mode {
    failed_leader,
    failed_replica,
    none
};

struct testinfo {
    random_source randomness;
    double loss = 0.01;
    bool verbose = false;
    size_t nreplicas = 3;
    size_t initial_leader = 0;
    int failed_replica = -1;
    failure_mode mode = failure_mode::none;

    template <typename T>
    void configure_port(netsim::port<T>& port) {
        port.set_verbose(verbose);
    }
    template <typename T>
    void configure_channel(netsim::channel<T>& chan) {
        chan.set_loss(loss);
        chan.set_verbose(verbose);
    }
};

static size_t replica_index_from_source_id(const std::string& source_id) {
    return static_cast<size_t>(std::stoull(source_id.substr(1)));
}

static unsigned long long message_round(const paxos_message& msg) {
    return std::visit([](const auto& m) {
        return m.round;
    }, msg);
}

static bool same_request(const request& a, const request& b) {
    return message_serial(a) == message_serial(b);
}

class dummy_clients {
public:
    dummy_clients(size_t nreplicas, random_source& randomness)
        : randomness_(randomness),
          out_(nreplicas),
          in_(randomness, "clients") {
        for (size_t i = 0; i != nreplicas; ++i) {
            out_[i].reset(new netsim::channel<request>(randomness, "clients"));
        }
        receive_task_ = receive_loop();
    }

    void connect_replica(size_t replicaid,
                         netsim::port<request>& request_input,
                         netsim::channel<response>& response_channel) {
        out_[replicaid]->connect(request_input);
        response_channel.connect(in_);
    }

    netsim::channel<request>& request_channel(size_t replicaid) {
        return *out_[replicaid];
    }

    void start() {
        set_nclients(nclients_);
        running_ = true;
        for (size_t i = 0; i != nclients_; ++i) {
            tasks_.push_back(client(i));
        }
    }

    void stop() {
        running_ = false;
    }

    std::optional<std::string> check(const tracker_state_machine& state) const {
        std::vector<uint64_t> expected;
        for (size_t cid = 0; cid != acknowledged_.size(); ++cid) {
            for (uint64_t serial = cid + serial_step; serial <= acknowledged_[cid];
                 serial += serial_step) {
                expected.push_back(serial);
            }
        }
        std::sort(expected.begin(), expected.end());
        auto actual = state.log;
        std::sort(actual.begin(), actual.end());
        if (!std::includes(actual.begin(), actual.end(), expected.begin(), expected.end())) {
            return std::format("expected at least {} acknowledged entries, found {}",
                               expected.size(), actual.size());
        }
        return std::nullopt;
    }

    unsigned long complete = 0;

private:
    random_source& randomness_;
    std::vector<std::unique_ptr<netsim::channel<request>>> out_;
    netsim::port<response> in_;
    std::deque<response> inq_;
    std::vector<cot::event> client_events_;
    std::vector<cot::task<>> tasks_;
    cot::task<> receive_task_;
    std::vector<uint64_t> acknowledged_;
    size_t nclients_ = 8;
    bool running_ = false;

    static constexpr size_t max_clients = 4096;
    static constexpr uint64_t client_mask = max_clients - 1;
    static constexpr uint64_t serial_step = max_clients;

    void set_nclients(size_t n) {
        client_events_.resize(n);
        acknowledged_.resize(n, 0);
    }

    size_t random_replica() {
        return randomness_.uniform(size_t(0), out_.size() - 1);
    }

    static void fill_bytes(char (&out)[20], unsigned first, unsigned second, char tag) {
        std::string bytes = std::format("{:04x}{:04x}{}tracker", first, second, tag);
        bytes.resize(20, tag);
        memcpy(out, bytes.data(), 20);
    }

    request make_announce(uint64_t serial, unsigned cid, unsigned value) const {
        bt_tracker_announce_request announce{};
        unsigned torrent = cid % 4;
        unsigned lifecycle_step = value % 4;
        unsigned lifecycle_generation = value / 4;
        fill_bytes(announce.info_hash, torrent, 0, 't');
        fill_bytes(announce.peer_id, cid, lifecycle_generation, 'p');
        announce.ip = (10U << 24) | (0U << 16) | (static_cast<uint32_t>(cid & 0xff) << 8)
            | static_cast<uint32_t>((value % 254) + 1);
        announce.port = static_cast<uint16_t>(6881 + (cid % 1000));
        announce.uploaded = value * 1024;
        announce.downloaded = lifecycle_step * 1024;
        if (lifecycle_step == 2) {
            announce.left = 0;
            announce.event = completed;
        } else if (lifecycle_step == 3) {
            announce.left = 0;
            announce.event = stopped;
        } else {
            announce.left = 4096 - lifecycle_step * 1024;
            announce.event = started;
        }
        announce.numwant = 50;
        return {serial, announce};
    }

    cot::task<> receive_loop() {
        while (true) {
            auto msg = co_await in_.receive();
            size_t cid = message_serial(msg) & client_mask;
            if (cid >= client_events_.size()) {
                continue;
            }
            inq_.push_back(std::move(msg));
            client_events_[cid].trigger();
        }
    }

    cot::task<std::optional<response>> receive_response(
        size_t& replicaid, uint64_t serial
    ) {
        size_t cid = serial & client_mask;
        while (true) {
            for (auto it = inq_.begin(); it != inq_.end(); ) {
                uint64_t in_serial = message_serial(*it);
                if ((in_serial & client_mask) != cid) {
                    ++it;
                } else if (in_serial != serial) {
                    it = inq_.erase(it);
                } else if (it->redirect) {
                    replicaid = it->redirection;
                    it = inq_.erase(it);
                    co_return std::nullopt;
                } else {
                    auto r = std::move(*it);
                    it = inq_.erase(it);
                    co_return r;
                }
            }
            cot::driver_guard guard;
            co_await client_events_[cid].arm();
        }
    }

    cot::task<> client(unsigned cid) {
        size_t leader = random_replica();
        uint64_t serial = cid;
        unsigned value = 0;
        while (running_) {
            if (value % 4 == 0) {
                co_await cot::after(randomness_.normal(500ms, 100ms));
            } else {
                co_await cot::after(1ms);
            }
            serial += serial_step;
            request req = make_announce(serial, cid, value);

            for (unsigned tries = 0; true; ++tries) {
                co_await out_[leader]->send(req);
                auto resp = co_await cot::attempt(
                    receive_response(leader, serial),
                    cot::after(randomness_.normal(3s, 1s))
                );
                if (!resp) {
                    leader = tries % 3 == 2 ? random_replica() : leader;
                    continue;
                }
                acknowledged_[cid] = serial;
                ++value;
                ++complete;
                break;
            }
        }
    }
};

struct replica;
struct instance {
    testinfo& tester;
    dummy_clients& clients;
    std::vector<std::unique_ptr<replica>> replicas;
    instance(testinfo&, dummy_clients&);
};

struct replica {
    size_t index_;
    size_t nreplicas_;
    size_t leader_index_ = 0;
    netsim::port<request> from_clients_;
    netsim::port<paxos_message> from_replicas_;
    netsim::channel<response> to_clients_;
    std::vector<std::unique_ptr<netsim::channel<paxos_message>>> to_replicas_;
    tracker_state_machine state_;

    unsigned long long next_round_ = 1;
    unsigned long long promised_round_ = 0;
    unsigned long long accepted_round_ = 0;
    std::deque<request> accepted_values_;
    std::deque<unsigned long long> accepted_rounds_;
    unsigned long long commit_index_ = 0;
    unsigned long long applied_index_ = 0;
    std::vector<unsigned long long> match_index_;
    std::vector<unsigned long long> next_index_;
    std::unordered_map<uint64_t, response> client_response_cache_;
    std::unordered_set<uint64_t> pending_client_serials_;
    cot::duration heartbeat_interval_ = 200ms;
    cot::duration failure_timeout_;
    unsigned long quorum_;

    replica(size_t index, size_t nreplicas, random_source& randomness)
        : index_(index),
          nreplicas_(nreplicas),
          from_clients_(randomness, std::format("R{}", index_)),
          from_replicas_(randomness, std::format("R{}/r", index_)),
          to_clients_(randomness, from_clients_.id()),
          to_replicas_(nreplicas),
          match_index_(nreplicas, 0),
          next_index_(nreplicas, 0),
          failure_timeout_(randomness.uniform(10s, 12s)),
          quorum_(nreplicas / 2 + 1) {
        for (size_t s = 0; s != nreplicas_; ++s) {
            to_replicas_[s].reset(new netsim::channel<paxos_message>(
                randomness, from_clients_.id()
            ));
        }
        next_round_ = index_ + 1;
    }

    void initialize(instance& inst) {
        leader_index_ = inst.tester.initial_leader;
        inst.clients.connect_replica(index_, from_clients_, to_clients_);
        inst.tester.configure_port(from_clients_);
        inst.tester.configure_port(from_replicas_);
        to_clients_.set_verbose(inst.tester.verbose);
        inst.clients.request_channel(index_).set_loss(0);
        for (size_t s = 0; s != nreplicas_; ++s) {
            to_replicas_[s]->connect(inst.replicas[s]->from_replicas_);
            inst.tester.configure_channel(*to_replicas_[s]);
        }
    }

    cot::task<> run();
    cot::task<> run_as_leader();
    cot::task<> run_as_follower();
    cot::task<> send_to_other_replicas(const paxos_message& msg);

    unsigned long long reserve_next_round() {
        auto round = next_round_;
        next_round_ += nreplicas_;
        return round;
    }

    void advance_next_round_past(unsigned long long round) {
        while (next_round_ <= round) {
            next_round_ += nreplicas_;
        }
    }

    ack_msg make_ack(unsigned long long round, bool success) const {
        return {base_message{round}, success, accepted_values_.size()};
    }

    void apply_committed_up_to(unsigned long long committed_slot) {
        auto new_commit = std::max(
            commit_index_,
            std::min(committed_slot, (unsigned long long) accepted_values_.size())
        );
        for (auto i = commit_index_; i < new_commit; ++i) {
            state_.process(accepted_values_[i]);
        }
        commit_index_ = new_commit;
        applied_index_ = commit_index_;
    }
};

instance::instance(testinfo& tester, dummy_clients& clients)
    : tester(tester), clients(clients), replicas(tester.nreplicas) {
    for (size_t s = 0; s != tester.nreplicas; ++s) {
        replicas[s].reset(new replica(s, tester.nreplicas, tester.randomness));
    }
    for (size_t s = 0; s != tester.nreplicas; ++s) {
        replicas[s]->initialize(*this);
    }
}

cot::task<> replica::run() {
    while (true) {
        if (index_ == leader_index_) {
            co_await run_as_leader();
        } else {
            co_await run_as_follower();
        }
    }
}

cot::task<> replica::send_to_other_replicas(const paxos_message& msg) {
    for (size_t s = 0; s != nreplicas_; ++s) {
        if (s != index_) {
            co_await to_replicas_[s]->send(msg);
        }
    }
}

cot::task<> replica::run_as_leader() {
    probe_msg probe;
    probe.round = reserve_next_round();
    promised_round_ = std::max(promised_round_, probe.round);

    co_await send_to_other_replicas(probe);

    std::vector<bool> prepared(nreplicas_, false);
    size_t prepare_count = 0;
    std::deque<request> selected_values = accepted_values_;
    std::deque<unsigned long long> selected_rounds = accepted_rounds_;
    selected_rounds.resize(selected_values.size(), accepted_round_);

    while (prepare_count < quorum_ - 1) {
        auto received = co_await cot::attempt(
            from_replicas_.receive_with_id(),
            cot::after(200ms)
        );
        if (!received) {
            co_await send_to_other_replicas(probe);
            continue;
        }

        auto& [paxos_msg, source_id] = *received;
        if (message_round(paxos_msg) > probe.round) {
            promised_round_ = message_round(paxos_msg);
            advance_next_round_past(promised_round_);
            leader_index_ = replica_index_from_source_id(source_id);
            co_return;
        }

        auto* prepare = std::get_if<prepare_msg>(&paxos_msg);
        if (!prepare || prepare->round != probe.round) {
            continue;
        }

        size_t sender_index = replica_index_from_source_id(source_id);
        if (prepared[sender_index]) {
            continue;
        }
        prepared[sender_index] = true;

        for (size_t i = 0; i != prepare->accepted_values.size(); ++i) {
            if (i < commit_index_) {
                continue;
            }
            unsigned long long slot_round = prepare->accepted_round;
            if (i < prepare->accepted_rounds.size()) {
                slot_round = prepare->accepted_rounds[i];
            }
            if (i >= selected_values.size()) {
                selected_values.push_back(prepare->accepted_values[i]);
                selected_rounds.push_back(slot_round);
            } else if (slot_round > selected_rounds[i]) {
                selected_values[i] = prepare->accepted_values[i];
                selected_rounds[i] = slot_round;
            }
        }
        ++prepare_count;
    }

    accepted_values_ = std::move(selected_values);
    accepted_rounds_ = std::move(selected_rounds);
    accepted_round_ = 0;
    for (auto round : accepted_rounds_) {
        accepted_round_ = std::max(accepted_round_, round);
    }
    std::fill(match_index_.begin(), match_index_.end(), 0);
    std::fill(next_index_.begin(), next_index_.end(), 0);
    match_index_[index_] = accepted_values_.size();
    next_index_[index_] = accepted_values_.size();

    std::deque<std::pair<unsigned long long, uint64_t>> pending_clients;
    std::deque<response> ready_client_responses;
    const size_t follower_quorum = quorum_ - 1;

    while (true) {
        auto received = co_await cot::attempt(
            from_clients_.receive(),
            cot::after(heartbeat_interval_)
        );

        propose_msg propose;
        propose.round = probe.round;
        propose.batch_start = accepted_values_.size();
        propose.committed_slot = commit_index_;

        auto append_request = [&](request req) {
            uint64_t serial = message_serial(req);
            if (auto cached = client_response_cache_.find(serial);
                cached != client_response_cache_.end()) {
                ready_client_responses.push_back(cached->second);
                return;
            }
            if (!pending_client_serials_.insert(serial).second) {
                return;
            }
            pending_clients.emplace_back(accepted_values_.size(), serial);
            accepted_values_.push_back(std::move(req));
            accepted_rounds_.push_back(propose.round);
            propose.entries.push_back(accepted_values_.back());
        };

        if (received) {
            append_request(std::move(*received));
            accepted_round_ = propose.round;
            promised_round_ = std::max(promised_round_, propose.round);
            match_index_[index_] = accepted_values_.size();
        }

        std::vector<bool> acked(nreplicas_, false);
        std::vector<propose_msg> in_flight(nreplicas_, propose);
        auto rebuild_suffix = [&](size_t s, unsigned long long batch_start) {
            auto& msg = in_flight[s];
            msg.round = propose.round;
            msg.batch_start = std::min<unsigned long long>(batch_start, accepted_values_.size());
            msg.committed_slot = propose.committed_slot;
            msg.entries.clear();
            for (size_t i = msg.batch_start; i != accepted_values_.size(); ++i) {
                msg.entries.push_back(accepted_values_[i]);
            }
        };

        for (size_t s = 0; s != nreplicas_; ++s) {
            if (s == index_) {
                continue;
            }
            if (match_index_[s] < accepted_values_.size()) {
                rebuild_suffix(s, next_index_[s]);
            }
            co_await to_replicas_[s]->send(in_flight[s]);
        }

        size_t ack_count = 0;
        while (ack_count < follower_quorum) {
            auto ack_received = co_await cot::attempt(
                from_replicas_.receive_with_id(),
                cot::after(200ms)
            );
            if (!ack_received) {
                for (size_t s = 0; s != nreplicas_; ++s) {
                    if (s != index_ && !acked[s]) {
                        co_await to_replicas_[s]->send(in_flight[s]);
                    }
                }
                continue;
            }

            auto& [paxos_msg, source_id] = *ack_received;
            size_t sender_index = replica_index_from_source_id(source_id);
            if (message_round(paxos_msg) < promised_round_) {
                co_await to_replicas_[sender_index]->send(make_ack(promised_round_, false));
                continue;
            }
            if (message_round(paxos_msg) > propose.round) {
                promised_round_ = message_round(paxos_msg);
                advance_next_round_past(promised_round_);
                leader_index_ = sender_index;
                co_return;
            }

            auto* ack = std::get_if<ack_msg>(&paxos_msg);
            if (!ack || ack->round != propose.round || acked[sender_index]) {
                continue;
            }

            if (!ack->success || ack->highest_accepted < accepted_values_.size()) {
                match_index_[sender_index] = std::max(
                    match_index_[sender_index], ack->highest_accepted
                );
                next_index_[sender_index] = ack->highest_accepted;
                rebuild_suffix(sender_index, ack->highest_accepted);
                co_await to_replicas_[sender_index]->send(in_flight[sender_index]);
                continue;
            }

            acked[sender_index] = true;
            match_index_[sender_index] = std::max(
                match_index_[sender_index], ack->highest_accepted
            );
            next_index_[sender_index] = match_index_[sender_index];
            ++ack_count;
        }

        auto sorted = match_index_;
        std::sort(sorted.begin(), sorted.end());
        commit_index_ = std::max(commit_index_, sorted[nreplicas_ - quorum_]);

        for (auto i = applied_index_; i < commit_index_; ++i) {
            auto resp = state_.process(accepted_values_[i]);
            if (!pending_clients.empty() && pending_clients.front().first == i) {
                uint64_t serial = pending_clients.front().second;
                pending_client_serials_.erase(serial);
                auto it = client_response_cache_.emplace(serial, resp).first;
                ready_client_responses.push_back(it->second);
                pending_clients.pop_front();
            }
        }
        applied_index_ = commit_index_;

        commit_msg commit;
        commit.round = propose.round;
        commit.committed_slot = commit_index_;
        for (size_t s = 0; s != nreplicas_; ++s) {
            if (s != index_ && match_index_[s] >= commit_index_) {
                co_await to_replicas_[s]->send(commit);
            }
        }

        while (!ready_client_responses.empty()) {
            co_await to_clients_.send(std::move(ready_client_responses.front()));
            ready_client_responses.pop_front();
        }
    }
}

cot::task<> replica::run_as_follower() {
    auto leader_deadline = cot::steady_now() + failure_timeout_;
    while (true) {
        auto msg = co_await cot::first(
            from_clients_.receive(),
            from_replicas_.receive_with_id(),
            cot::at(leader_deadline)
        );

        auto* req = std::get_if<request>(&msg);
        if (req) {
            co_await to_clients_.send(response{message_serial(*req), true, leader_index_, {}});
            continue;
        }

        auto* received = std::get_if<std::pair<paxos_message, std::string>>(&msg);
        if (!received) {
            leader_index_ = index_;
            advance_next_round_past(promised_round_);
            co_return;
        }

        auto& [paxos_msg, source_id] = *received;
        leader_deadline = cot::steady_now() + failure_timeout_;
        size_t sender_index = replica_index_from_source_id(source_id);
        unsigned long long incoming_round = message_round(paxos_msg);

        if (incoming_round < promised_round_) {
            co_await to_replicas_[sender_index]->send(make_ack(promised_round_, false));
            continue;
        }

        if (incoming_round > promised_round_) {
            promised_round_ = incoming_round;
            advance_next_round_past(promised_round_);
        }

        if (std::holds_alternative<commit_msg>(paxos_msg)) {
            leader_index_ = sender_index;
            apply_committed_up_to(std::get<commit_msg>(paxos_msg).committed_slot);
            continue;
        }

        auto* propose = std::get_if<propose_msg>(&paxos_msg);
        if (!propose) {
            auto* probe = std::get_if<probe_msg>(&paxos_msg);
            if (!probe || probe->round < promised_round_) {
                continue;
            }
            prepare_msg prepare;
            prepare.round = probe->round;
            prepare.accepted_round = accepted_round_;
            prepare.accepted_values = accepted_values_;
            prepare.accepted_rounds = accepted_rounds_;
            co_await to_replicas_[sender_index]->send(prepare);
            continue;
        }

        leader_index_ = sender_index;
        if (propose->round < promised_round_) {
            continue;
        }

        auto suffix_end = propose->batch_start + propose->entries.size();
        bool accepted = propose->batch_start <= accepted_values_.size();
        unsigned long long matched_prefix = std::min<unsigned long long>(
            propose->batch_start, accepted_values_.size()
        );

        if (accepted) {
            auto overlap_end = std::min<unsigned long long>(
                suffix_end, accepted_values_.size()
            );
            for (auto i = propose->batch_start; i != overlap_end; ++i) {
                if (!same_request(accepted_values_[i],
                                  propose->entries[i - propose->batch_start])) {
                    matched_prefix = i;
                    accepted = i >= commit_index_;
                    break;
                }
                matched_prefix = i + 1;
            }
        }

        if (accepted && suffix_end < commit_index_) {
            matched_prefix = suffix_end;
            accepted = false;
        }

        if (accepted) {
            accepted_values_.resize(suffix_end);
            accepted_rounds_.resize(suffix_end);
            for (auto i = matched_prefix; i != suffix_end; ++i) {
                accepted_values_[i] = propose->entries[i - propose->batch_start];
                accepted_rounds_[i] = propose->round;
            }
            promised_round_ = std::max(promised_round_, propose->round);
            accepted_round_ = std::max(accepted_round_, propose->round);
            apply_committed_up_to(propose->committed_slot);
            matched_prefix = accepted_values_.size();
        }

        ack_msg ack = make_ack(propose->round, accepted);
        ack.highest_accepted = matched_prefix;
        co_await to_replicas_[leader_index_]->send(ack);
    }
}

static void set_replica_channel_loss(instance& inst, size_t replica, double loss) {
    for (size_t i = 0; i < inst.tester.nreplicas; ++i) {
        inst.replicas[replica]->to_replicas_[i]->set_loss(loss);
        inst.replicas[i]->to_replicas_[replica]->set_loss(loss);
    }
    inst.clients.request_channel(replica).set_loss(loss);
    inst.replicas[replica]->to_clients_.set_loss(loss);
}

static cot::task<> stop_clients_and_clear_after(dummy_clients& clients,
                                                cot::duration run_for,
                                                cot::duration settle_for) {
    co_await cot::after(run_for);
    clients.stop();
    co_await cot::after(settle_for);
    cot::clear();
}

static bool should_skip_replica(const testinfo& tester, size_t replica) {
    if (tester.mode == failure_mode::failed_leader && replica == tester.initial_leader) {
        return true;
    }
    if (tester.mode == failure_mode::failed_replica
        && replica == static_cast<size_t>(tester.failed_replica)) {
        return true;
    }
    return false;
}

static constexpr size_t max_shutdown_commit_lag = 5;

static bool try_one_seed(testinfo& tester, unsigned long seed) {
    cot::reset();
    tester.randomness.seed(seed);

    dummy_clients clients(tester.nreplicas, tester.randomness);
    instance inst(tester, clients);
    clients.start();

    std::vector<cot::task<>> tasks;
    for (size_t s = 0; s != tester.nreplicas; ++s) {
        tasks.push_back(inst.replicas[s]->run());
    }

    if (tester.mode == failure_mode::failed_leader) {
        set_replica_channel_loss(inst, tester.initial_leader, 1);
    } else if (tester.mode == failure_mode::failed_replica) {
        set_replica_channel_loss(inst, tester.failed_replica, 1);
    }

    cot::task<> timeout_task = stop_clients_and_clear_after(clients, 20s, 5s);
    cot::loop();

    size_t reference = tester.nreplicas;
    for (size_t s = 0; s != tester.nreplicas; ++s) {
        if (should_skip_replica(tester, s)) {
            continue;
        }
        if (reference == tester.nreplicas
            || inst.replicas[s]->state_.log.size()
                > inst.replicas[reference]->state_.log.size()) {
            reference = s;
        }
    }
    if (reference == tester.nreplicas) {
        std::print(std::clog, "*** no live reference replica on seed {}\n", seed);
        return false;
    }

    auto& ref_state = inst.replicas[reference]->state_;
    std::print("{} announce: {} started, {} completed, {} stopped\n",
               ref_state.log.size(),
               ref_state.started_count,
               ref_state.completed_count,
               ref_state.stopped_count);
    for (size_t s = 0; s != tester.nreplicas; ++s) {
        if (s == reference || should_skip_replica(tester, s)) {
            continue;
        }
        auto& a = ref_state.log;
        auto& b = inst.replicas[s]->state_.log;
        size_t common_size = std::min(a.size(), b.size());
        if (!std::equal(a.begin(), a.begin() + common_size, b.begin())) {
            std::print(std::clog, "*** REPLICA DIVERGENCE on seed {} between {} and {}\n",
                       seed, reference, s);
            for (size_t r = 0; r != tester.nreplicas; ++r) {
                auto& replica = *inst.replicas[r];
                std::print(std::clog,
                           "*** R{}: log={} accepted={} commit={} applied={} leader={}\n",
                           r,
                           replica.state_.log.size(),
                           replica.accepted_values_.size(),
                           replica.commit_index_,
                           replica.applied_index_,
                           replica.leader_index_);
            }
            return false;
        }
        size_t lag = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
        if (lag > max_shutdown_commit_lag) {
            std::print(std::clog,
                       "*** REPLICA COMMIT LAG on seed {} between {} and {}: {}\n",
                       seed, reference, s, lag);
            return false;
        }
    }

    if (auto problem = clients.check(ref_state)) {
        std::print(std::clog, "*** CLIENT MODEL FAILURE on seed {}: {}\n", seed, *problem);
        return false;
    }
    return true;
}

static struct option options[] = {
    { "count", required_argument, nullptr, 'n' },
    { "seed", required_argument, nullptr, 'S' },
    { "random-seeds", required_argument, nullptr, 'R' },
    { "loss", required_argument, nullptr, 'l' },
    { "verbose", no_argument, nullptr, 'V' },
    { "failure-mode", required_argument, nullptr, 'f' },
    { "failed-replica", required_argument, nullptr, 'r' },
    { nullptr, 0, nullptr, 0 }
};

int main(int argc, char* argv[]) {
    testinfo tester;
    std::optional<unsigned long> first_seed;
    unsigned long seed_count = 1;

    auto shortopts = short_options_for(options);
    int ch;
    while ((ch = getopt_long(argc, argv, shortopts.c_str(), options, nullptr)) != -1) {
        if (ch == 'S') {
            first_seed = from_str_chars<unsigned long>(optarg);
        } else if (ch == 'R') {
            seed_count = from_str_chars<unsigned long>(optarg);
        } else if (ch == 'l') {
            tester.loss = from_str_chars<double>(optarg);
        } else if (ch == 'n') {
            tester.nreplicas = from_str_chars<size_t>(optarg);
        } else if (ch == 'V') {
            tester.verbose = true;
        } else if (ch == 'r') {
            tester.failed_replica = from_str_chars<int>(optarg);
        } else if (ch == 'f') {
            if (strcmp(optarg, "failed_leader") == 0) {
                tester.mode = failure_mode::failed_leader;
            } else if (strcmp(optarg, "failed_replica") == 0) {
                tester.mode = failure_mode::failed_replica;
            } else if (strcmp(optarg, "none") == 0) {
                tester.mode = failure_mode::none;
            } else {
                std::print(std::cerr, "unknown failure mode {}\n", optarg);
                return EXIT_FAILURE;
            }
        } else {
            std::print(std::cerr, "unknown option\n");
            return EXIT_FAILURE;
        }
    }

    if (tester.mode == failure_mode::failed_replica && tester.failed_replica < 0) {
        std::print(std::cerr, "failed_replica requires -r\n");
        return EXIT_FAILURE;
    }

    bool ok = true;
    if (first_seed) {
        ok = try_one_seed(tester, *first_seed);
    } else {
        std::mt19937_64 seed_generator(std::random_device{}());
        for (unsigned long i = 0; i != seed_count; ++i) {
            ok = try_one_seed(tester, seed_generator());
            if (!ok) {
                break;
            }
        }
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
