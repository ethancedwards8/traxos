#include "pt-paxos.hh"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <print>
#include <random>

namespace cot = cotamer;
using namespace std::chrono_literals;

bool try_one_seed(testinfo& tester, unsigned long seed);

static size_t replica_index_from_source_id(const std::string& source_id) {
    return from_str_chars<size_t>(source_id.substr(1));
}

static unsigned long long message_round(const paxos_message& msg) {
    return std::visit([](const auto& m) {
        return m.round;
    }, msg);
}


// Configuration and initialization

pt_paxos_replica::pt_paxos_replica(size_t index, size_t nreplicas, random_source& randomness)
    : index_(index),
      nreplicas_(nreplicas),
      from_clients_(randomness, std::format("R{}", index_)),
      from_replicas_(randomness, std::format("R{}/r", index_)),
      to_clients_(randomness, from_clients_.id()),
      to_replicas_(nreplicas),
      match_index_(nreplicas, 0),
      failure_timeout_(randomness.uniform(10s, 12s)) {
    for (size_t s = 0UL; s != nreplicas_; ++s) {
        to_replicas_[s].reset(new netsim::channel<paxos_message>(
            randomness, from_clients_.id()
        ));
    }
    next_round_ = index_ + 1;
}

void pt_paxos_replica::initialize(pt_paxos_instance& inst) {
    leader_index_ = inst.tester.initial_leader;
    inst.clients.connect_replica(index_, from_clients_, to_clients_);
    inst.tester.configure_port(from_clients_);
    inst.tester.configure_port(from_replicas_);
    to_clients_.set_verbose(inst.tester.verbose);
    inst.clients.request_channel(index_).set_loss(0);
    for (size_t s = 0UL; s != nreplicas_; ++s) {
        to_replicas_[s]->connect(inst.replicas[s]->from_replicas_);
        inst.tester.configure_channel(*to_replicas_[s]);
    }
}

pt_paxos_instance::pt_paxos_instance(testinfo& tester, client_model& clients)
    : tester(tester), clients(clients), replicas(tester.nreplicas) {
    for (size_t s = 0UL; s != tester.nreplicas; ++s) {
        replicas[s].reset(new pt_paxos_replica(s, tester.nreplicas, tester.randomness));
    }
    for (size_t s = 0UL; s != tester.nreplicas; ++s) {
        replicas[s]->initialize(*this);
    }
}



// ********** PANCY SERVICE CODE **********

cot::task<> pt_paxos_replica::run() {
    while (true) {
        if (index_ == leader_index_) {
            co_await run_as_leader();
        } else {
            co_await run_as_follower();
        }
    }
}

cot::task<> pt_paxos_replica::send_to_other_replicas(const paxos_message& msg) {
    for (size_t s = 0; s != nreplicas_; ++s) {
        if (s == index_)
            continue;

        co_await to_replicas_[s]->send(msg);
    }
}

unsigned long long pt_paxos_replica::reserve_next_round() {
    auto round = next_round_;
    next_round_ += nreplicas_;
    return round;
}

void pt_paxos_replica::advance_next_round_past(unsigned long long round) {
    while (next_round_ <= round) {
        next_round_ += nreplicas_;
    }
}

ack_msg pt_paxos_replica::make_ack(unsigned long long round, bool success) const {
    ack_msg ack;
    ack.round = round;
    ack.success = success;
    ack.highest_accepted = accepted_values_.size();
    ack.applied_up_to = commit_index_;
    return ack;
}

void pt_paxos_replica::apply_committed_up_to(unsigned long long committed_slot) {
    auto new_commit = std::max(
        commit_index_,
        std::min(committed_slot, (unsigned long long) accepted_values_.size())
    );
    for (auto i = commit_index_; i < new_commit; ++i) {
        db_.process_req(accepted_values_[i]);
    }
    commit_index_ = new_commit;
    applied_index_ = commit_index_;
}

cot::task<> pt_paxos_replica::run_as_leader() {
    probe_msg probe;
    probe.round = reserve_next_round();
    promised_round_ = std::max(promised_round_, probe.round);

    co_await send_to_other_replicas(probe);

    std::vector<bool> prepared(nreplicas_, false);
    size_t prepare_count = 0;
    std::deque<pancy::request> selected_values = accepted_values_;
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
        if (!prepare)
            continue;

        if (prepare->round != probe.round)
            continue;

        size_t sender_index = replica_index_from_source_id(source_id);
        if (prepared[sender_index])
            continue;

        prepared[sender_index] = true;

        for (size_t i = 0; i != prepare->accepted_values.size(); ++i) {
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

    // Adopt the highest accepted value per slot but do NOT apply to db yet.
    accepted_values_ = std::move(selected_values);
    accepted_rounds_ = std::move(selected_rounds);
    accepted_round_ = 0;
    for (auto round : accepted_rounds_) {
        accepted_round_ = std::max(accepted_round_, round);
    }
    std::fill(match_index_.begin(), match_index_.end(), 0);
    match_index_[index_] = accepted_values_.size();

    std::deque<std::pair<unsigned long long, uint64_t>> pending_clients;
    std::deque<pancy::response> ready_client_responses;
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

        auto append_request = [&](pancy::request req) {
            uint64_t serial = pancy::message_serial(req);
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

            constexpr size_t max_batch_size = 64;
            constexpr cot::duration batch_window = 5ms;
            while (propose.entries.size() < max_batch_size) {
                auto next_req = co_await cot::attempt(
                    from_clients_.receive(),
                    cot::after(batch_window)
                );
                if (!next_req) {
                    break;
                }
                append_request(std::move(*next_req));
            }

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
            if (s == index_)
                continue;
            co_await to_replicas_[s]->send(in_flight[s]);
        }

        for (size_t s = 0; s != nreplicas_; ++s) {
            if (s == index_)
                continue;
            if (match_index_[s] < accepted_values_.size()) {
                rebuild_suffix(s, match_index_[s]);
                co_await to_replicas_[s]->send(in_flight[s]);
            }
        }

        size_t ack_count = 0;
        while (ack_count < follower_quorum) {
            auto ack_received = co_await cot::attempt(
                from_replicas_.receive_with_id(),
                cot::after(200ms)
            );

            if (!ack_received) {
                for (size_t s = 0; s != nreplicas_; ++s) {
                    if (s == index_ || acked[s])
                        continue;
                    co_await to_replicas_[s]->send(in_flight[s]);
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

            if (!ack)
                continue;

            if (ack->round != propose.round)
                continue;

            if (acked[sender_index])
                continue;

            if (!ack->success || ack->highest_accepted < accepted_values_.size()) {
                match_index_[sender_index] = ack->highest_accepted;

                rebuild_suffix(sender_index, ack->highest_accepted);
                co_await to_replicas_[sender_index]->send(in_flight[sender_index]);
                continue;
            }

            acked[sender_index] = true;
            match_index_[sender_index] = ack->highest_accepted;

            ++ack_count;
        }

        // Compute new commit index: highest index a quorum has reached
        auto sorted = match_index_;
        std::sort(sorted.begin(), sorted.end());
        commit_index_ = sorted[nreplicas_ - quorum_];

        unsigned long long new_applied = commit_index_;

        for (auto i = applied_index_; i < new_applied; ++i) {
            auto resp = db_.process_req(accepted_values_[i]);
            if (!pending_clients.empty() && pending_clients.front().first == i) {
                uint64_t serial = pending_clients.front().second;
                pending_client_serials_.erase(serial);
                auto it = client_response_cache_.emplace(serial, resp).first;
                ready_client_responses.push_back(it->second);
                pending_clients.pop_front();
            }
        }
        applied_index_ = new_applied;

        commit_msg commit;
        commit.round = propose.round;
        commit.committed_slot = commit_index_;
        for (size_t s = 0; s != nreplicas_; ++s) {
            if (s == index_)
                continue;
            co_await to_replicas_[s]->send(commit);
        }

        while (!ready_client_responses.empty()) {
            co_await to_clients_.send(std::move(ready_client_responses.front()));
            ready_client_responses.pop_front();
        }
    }
}

cot::task<> pt_paxos_replica::run_as_follower() {
    auto leader_deadline = cot::steady_now() + failure_timeout_;
    while (true) {
        auto msg = co_await cot::first(
            from_clients_.receive(),
            from_replicas_.receive_with_id(),
            cot::at(leader_deadline)
        );

        auto* req = std::get_if<pancy::request>(&msg);
        if (req) {
            co_await cot::after(.02s); // to make it more obvious in the viz
            co_await to_clients_.send(pancy::redirection_response{
                pancy::response_header(*req, pancy::errc::redirect), leader_index_
            });
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

        if (auto* commit = std::get_if<commit_msg>(&paxos_msg)) {
            leader_index_ = sender_index;
            apply_committed_up_to(commit->committed_slot);
            co_await to_replicas_[leader_index_]->send(
                make_ack(commit->round, commit_index_ >= commit->committed_slot)
            );
            continue;
        }

        auto* propose = std::get_if<propose_msg>(&paxos_msg);
        if (!propose) {
            auto* probe = std::get_if<probe_msg>(&paxos_msg);
            if (!probe)
                continue;

            if (probe->round < promised_round_) {
                continue;
            }

            prepare_msg prepare;
            prepare.round = probe->round;
            prepare.accepted_round = accepted_round_;
            prepare.applied_up_to = commit_index_;
            prepare.accepted_values = accepted_values_;
            prepare.accepted_rounds = accepted_rounds_;
            co_await to_replicas_[sender_index]->send(prepare);
            continue;
        }

        leader_index_ = sender_index;

        // Reject stale messages (e.g., old heartbeats arriving after newer proposes)
        if (propose->round < promised_round_) {
            continue;
        }

        auto suffix_end = propose->batch_start + propose->entries.size();
        auto append_start = std::max<unsigned long long>(propose->batch_start, commit_index_);
        bool stale_same_round_suffix = propose->round == accepted_round_
            && suffix_end < accepted_values_.size();
        bool no_gap = stale_same_round_suffix
            || (append_start <= accepted_values_.size() && append_start <= suffix_end);
        if (no_gap && !stale_same_round_suffix) {
            accepted_values_.resize(append_start);
            accepted_rounds_.resize(append_start);
            for (auto i = append_start; i != suffix_end; ++i) {
                accepted_values_.push_back(propose->entries[i - propose->batch_start]);
                accepted_rounds_.push_back(propose->round);
            }
            promised_round_ = std::max(promised_round_, propose->round);
            accepted_round_ = std::max(accepted_round_, propose->round);
        }

        if (no_gap) {
            apply_committed_up_to(propose->committed_slot);
        }

        co_await to_replicas_[leader_index_]->send(make_ack(propose->round, no_gap));
    }
}

// ******** end Pancy service code ********


// Argument parsing

static struct option options[] = {
    { "count", required_argument, nullptr, 'n' },
    { "seed", required_argument, nullptr, 'S' },
    { "random-seeds", required_argument, nullptr, 'R' },
    { "loss", required_argument, nullptr, 'l' },
    { "verbose", no_argument, nullptr, 'V' },
    { "print-db", no_argument, nullptr, 'p' },
    { "quiet", no_argument, nullptr, 'q' },
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
        } else if (ch == 'p') {
            tester.print_db = true;
        } else if (ch == 'r') {
            tester.failed_replica = from_str_chars<int>(optarg);
        } else if (ch == 'f') {
            if (strcmp(optarg, "failed_leader") == 0) {
                tester.mode = failure_mode::failed_leader;
            } else if (strcmp(optarg, "failed_replica") == 0) {
                tester.mode = failure_mode::failed_replica;
                if (tester.failed_replica < 0) {
                    std::cerr << "must use -r <int> with failed_replica mode\n";
                    return -1;
                }
            } else if (strcmp(optarg, "multiple_random_up_down") == 0) {
                if (tester.failed_replica < 0) {
                    std::cerr << "must use -r <int> with multiple_random_up_down mode\n";
                    return -1;
                }
                tester.mode = failure_mode::multiple_random_up_down;
            } else if (strcmp(optarg, "unstable_leader_mixed") == 0) {
                tester.mode = failure_mode::unstable_leader_mixed;
            } else if (strcmp(optarg, "random_failure_schedule") == 0) {
                tester.mode = failure_mode::random_failure_schedule;
            } else if (strcmp(optarg, "william_link_schedule") == 0
                       || strcmp(optarg, "random_link_schedule") == 0) {
                tester.mode = failure_mode::william_link_schedule;
            } else if (strcmp(optarg, "disruptive_isolate") == 0) {
                tester.mode = failure_mode::disruptive_isolate;
            } else if (strcmp(optarg, "split_brain") == 0) {
                tester.mode = failure_mode::split_brain;
            } else if (strcmp(optarg, "minority_partition") == 0) {
                tester.mode = failure_mode::minority_partition_failure;
            } else if (strcmp(optarg, "vihaan_split_brain") == 0) {
                tester.mode = failure_mode::vihaan_split_brain_failure;
            } else if (strcmp(optarg, "cascading_star_partition") == 0) {
                tester.mode = failure_mode::cascading_star_partition;
            } else if (strcmp(optarg, "split_brain_isolate_heal") == 0) {
                tester.mode = failure_mode::split_brain_isolate_heal;
            } else if (strcmp(optarg, "delayed_leader_failure") == 0) {
                tester.mode = failure_mode::delayed_leader_failure;
            }
        } else {
            std::print(std::cerr, "Unknown option\n");
            return 1;
        }
    }

    bool ok;
    if (first_seed) {
        ok = try_one_seed(tester, *first_seed);
    } else {
        std::mt19937_64 seed_generator = randomly_seeded<std::mt19937_64>();
        for (unsigned long i = 0; i != seed_count; ++i) {
            if (i > 0 && i % 1000 == 0) {
                std::print(std::cerr, ".");
            }
            unsigned long seed = seed_generator();
            ok = try_one_seed(tester, seed);
            if (!ok) {
                break;
            }
        }
        if (ok && seed_count >= 1000) {
            std::print(std::cerr, "\n");
        }
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
