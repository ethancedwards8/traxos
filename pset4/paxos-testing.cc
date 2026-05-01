#include "pt-paxos.hh"
#include "lockseq_model.hh"
#include <algorithm>
#include <iostream>
#include <memory>
#include <print>
#include <vector>

namespace cot = cotamer;
using namespace std::chrono_literals;

cot::task<> up_down_randomly(pt_paxos_instance& inst,
                             int replica,
                             cot::duration d,
                             std::shared_ptr<bool> running = nullptr);
cot::task<> partition_groups_forever(pt_paxos_instance& inst,
                                     std::vector<size_t> left,
                                     std::vector<size_t> right);


// Test functions

void set_replica_channel_loss(pt_paxos_instance& inst, size_t replica, double loss) {
    for (size_t i = 0; i < inst.tester.nreplicas; ++i) {
        inst.replicas[replica]->to_replicas_[i]->set_loss(loss);
        inst.replicas[i]->to_replicas_[replica]->set_loss(loss);
    }
    inst.clients.request_channel(replica).set_loss(loss);
    inst.replicas[replica]->to_clients_.set_loss(loss);
}

void set_partition_loss(pt_paxos_instance& inst,
                        const std::vector<size_t>& left,
                        const std::vector<size_t>& right,
                        double loss) {
    for (size_t l : left) {
        for (size_t r : right) {
            inst.replicas[l]->to_replicas_[r]->set_loss(loss);
            inst.replicas[r]->to_replicas_[l]->set_loss(loss);
        }
    }
}

void fail(pt_paxos_instance& inst, size_t a, size_t b) {
    inst.replicas[a]->to_replicas_[b]->set_loss(1.0);
    inst.replicas[b]->to_replicas_[a]->set_loss(1.0);
}

void recover(pt_paxos_instance& inst, size_t a, size_t b, double loss) {
    inst.replicas[a]->to_replicas_[b]->set_loss(loss);
    inst.replicas[b]->to_replicas_[a]->set_loss(loss);
}

void fail_replica(pt_paxos_instance& inst, size_t idx) {
    set_replica_channel_loss(inst, idx, 1.0);
}

void recover_replica(pt_paxos_instance& inst, size_t idx, double base_loss) {
    set_replica_channel_loss(inst, idx, base_loss);
}

inline void set_replica_link_loss(pt_paxos_instance& inst, size_t i, size_t j, double loss) {
    if (i == j)
        return;
    inst.replicas[i]->to_replicas_[j]->set_loss(loss);
}

inline void set_within_side_links(pt_paxos_instance& inst, size_t left_size, double loss) {
    const size_t n = inst.replicas.size();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j)
                continue;
            const bool same_side = (i < left_size) == (j < left_size);
            if (same_side)
                set_replica_link_loss(inst, i, j, loss);
        }
    }
}

inline cot::task<> heal_all_directed_edges_one_by_one(pt_paxos_instance& inst,
                                                      testinfo& tester,
                                                      cot::duration step_pause) {
    const size_t n = inst.replicas.size();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j)
                continue;
            set_replica_link_loss(inst, i, j, tester.loss);
            co_await cot::after(step_pause);
        }
    }
}

cot::task<> recover_replica_after(pt_paxos_instance& inst,
                                  testinfo& tester,
                                  size_t replica,
                                  cot::duration delay,
                                  std::shared_ptr<std::vector<bool>> active_failures,
                                  std::shared_ptr<size_t> currently_failed) {
    co_await cot::after(delay);
    if (replica >= tester.nreplicas || !(*active_failures)[replica])
        co_return;
    set_replica_channel_loss(inst, replica, tester.loss);
    (*active_failures)[replica] = false;
    if (*currently_failed > 0)
        --*currently_failed;
}

cot::task<> partition_replicas_after(pt_paxos_instance& inst,
                                     testinfo& tester,
                                     size_t a,
                                     size_t b,
                                     cot::duration delay,
                                     cot::duration heal_after = 0ms,
                                     bool two_way = true) {
    co_await cot::after(delay);
    set_replica_link_loss(inst, a, b, 1.0);
    if (two_way)
        set_replica_link_loss(inst, b, a, 1.0);

    if (heal_after <= 0ms)
        co_return;

    co_await cot::after(heal_after);
    set_replica_link_loss(inst, a, b, tester.loss);
    if (two_way)
        set_replica_link_loss(inst, b, a, tester.loss);
}

void set_replica_to_replica_loss(pt_paxos_instance& inst, double loss) {
    for (size_t from = 0; from != inst.tester.nreplicas; ++from) {
        for (size_t to = 0; to != inst.tester.nreplicas; ++to) {
            if (from == to) {
                continue;
            }
            inst.replicas[from]->to_replicas_[to]->set_loss(loss);
        }
    }
}

void apply_star_partition(pt_paxos_instance& inst, const std::vector<size_t>& hubs) {
    std::vector<bool> is_hub(inst.tester.nreplicas, false);
    for (size_t hub : hubs) {
        if (hub < inst.tester.nreplicas) {
            is_hub[hub] = true;
        }
    }

    set_replica_to_replica_loss(inst, 1);
    for (size_t from = 0; from != inst.tester.nreplicas; ++from) {
        for (size_t to = 0; to != inst.tester.nreplicas; ++to) {
            if (from == to) {
                continue;
            }
            if (is_hub[from] || is_hub[to]) {
                inst.replicas[from]->to_replicas_[to]->set_loss(inst.tester.loss);
            }
        }
    }
}

void apply_split_brain(pt_paxos_instance& inst) {
    std::vector<size_t> minority{inst.tester.initial_leader};
    std::vector<size_t> majority;
    majority.reserve(inst.tester.nreplicas - 1);
    for (size_t i = 0; i != inst.tester.nreplicas; ++i) {
        if (i != inst.tester.initial_leader) {
            majority.push_back(i);
        }
    }
    set_partition_loss(inst, minority, majority, 1);
}

cot::task<> minority_partition(pt_paxos_instance& inst, size_t leader_id, cot::duration duration) {
    co_await cot::after(10s);

    size_t num_replicas = inst.replicas.size();
    size_t majority = num_replicas / 2 + 1;

    std::vector<size_t> majority_set;
    std::vector<size_t> minority_set{leader_id};

    for (size_t i = 0; i < num_replicas; ++i) {
        if (i == leader_id) {
            continue;
        }
        if (majority_set.size() < majority) {
            majority_set.push_back(i);
        } else {
            minority_set.push_back(i);
        }
    }

    for (size_t a : majority_set) {
        for (size_t b : minority_set) {
            fail(inst, a, b);
            inst.replicas[b]->to_clients_.set_loss(1.0);
            inst.clients.request_channel(b).set_loss(1.0);
        }
    }

    co_await cot::after(duration);

    double loss = inst.tester.loss;
    for (size_t a : majority_set) {
        for (size_t b : minority_set) {
            recover(inst, a, b, loss);
            inst.replicas[b]->to_clients_.set_loss(loss);
            inst.clients.request_channel(b).set_loss(loss);
        }
    }
}

cot::task<> fail_primary_after(pt_paxos_instance& inst, cot::duration d) {
    co_await cot::after(d);
    set_replica_channel_loss(inst, inst.tester.initial_leader, 1);
}

cot::task<> cascading_star_partition_scenario(pt_paxos_instance& inst) {
    if (inst.tester.nreplicas < 3) {
        co_return;
    }

    std::vector<size_t> hubs{inst.tester.initial_leader};
    apply_star_partition(inst, hubs);

    std::vector<size_t> candidates;
    candidates.reserve(inst.tester.nreplicas - 1);
    for (size_t i = 0; i != inst.tester.nreplicas; ++i) {
        if (i != inst.tester.initial_leader) {
            candidates.push_back(i);
        }
    }
    if (candidates.size() < 2) {
        co_return;
    }

    std::shuffle(candidates.begin(), candidates.end(), inst.tester.randomness.engine());
    size_t first_successor = candidates[0];
    size_t second_successor = candidates[1];
    inst.tester.excluded_replicas = {
        inst.tester.initial_leader,
        first_successor,
        second_successor
    };

    co_await cot::after(30s);
    set_replica_channel_loss(inst, inst.tester.initial_leader, 1);

    co_await cot::after(5s);
    hubs.push_back(first_successor);
    apply_star_partition(inst, hubs);
    cot::task<> first_flap = up_down_randomly(inst, static_cast<int>(first_successor), 3s);

    co_await cot::after(10s);
    hubs.push_back(second_successor);
    apply_star_partition(inst, hubs);

    co_await cot::after(10s);
    cot::task<> second_flap = up_down_randomly(inst, static_cast<int>(second_successor), 3s);

    co_await cot::after(1h);
}

cot::task<> up_down_randomly(pt_paxos_instance& inst,
                             int replica,
                             cot::duration d,
                             std::shared_ptr<bool> running) {
    if (replica < 0 || replica >= (int) inst.tester.nreplicas) {
        co_return;
    }
    while (!running || *running) {
        co_await cot::after(inst.tester.randomness.uniform(d / 2, d * 3 / 2));
        if (running && !*running) {
            break;
        }
        set_replica_channel_loss(inst, replica, 1);
        co_await cot::after(inst.tester.randomness.uniform(d, d * 3));
        if (running && !*running) {
            break;
        }
        set_replica_channel_loss(inst, replica, inst.tester.loss);
    }
}

cot::task<> partition_groups_forever(pt_paxos_instance& inst,
                                     std::vector<size_t> left,
                                     std::vector<size_t> right) {
    set_partition_loss(inst, left, right, 1);
    co_await cot::after(1h);
}

cot::task<> failure_split_brain(pt_paxos_instance& inst,
                                cot::duration after,
                                cot::duration duration) {
    co_await cot::after(after);

    size_t n = inst.replicas.size();
    size_t mid = n / 2;

    std::vector<size_t> left, right;
    for (size_t i = 0; i < mid; ++i) left.push_back(i);
    for (size_t i = mid; i < n; ++i) right.push_back(i);

    set_partition_loss(inst, left, right, 1.0);
    co_await cot::after(duration);
    set_partition_loss(inst, left, right, inst.tester.loss);
}

cot::task<> split_brain_isolate_heal_schedule(pt_paxos_instance& inst,
                                              testinfo& tester,
                                              cot::duration t_split = 10s,
                                              cot::duration t_isolate_within = 10s,
                                              cot::duration step_pause = 1s,
                                              size_t left_size = 0) {
    const size_t n = tester.nreplicas;
    if (left_size == 0)
        left_size = n / 2;
    if (n <= 1 || left_size == 0 || left_size >= n)
        co_return;

    std::vector<size_t> left;
    std::vector<size_t> right;
    left.reserve(left_size);
    right.reserve(n - left_size);
    for (size_t i = 0; i < n; ++i) {
        if (i < left_size)
            left.push_back(i);
        else
            right.push_back(i);
    }

    co_await cot::after(t_split);
    set_partition_loss(inst, left, right, 1.0);

    co_await cot::after(t_isolate_within);
    set_within_side_links(inst, left_size, 1.0);

    co_await heal_all_directed_edges_one_by_one(inst, tester, step_pause);
}

cot::task<> random_failure_schedule_task(pt_paxos_instance& inst) {
    size_t n = inst.tester.nreplicas;
    size_t max_failed = (n - 1) / 2;
    auto active_failures = std::make_shared<std::vector<bool>>(n, false);
    auto currently_failed = std::make_shared<size_t>(0);
    std::vector<cot::task<>> scheduled_events;
    random_source& rng = inst.tester.randomness;

    while (true) {
        co_await cot::after(rng.uniform(200ms, 800ms));

        double roll = rng.uniform(0.0, 1.0);
        if (roll < 0.4 && *currently_failed < max_failed) {
            std::vector<size_t> candidates;
            candidates.reserve(n);
            for (size_t i = 0; i != n; ++i) {
                if (!(*active_failures)[i])
                    candidates.push_back(i);
            }
            if (candidates.empty())
                continue;

            size_t victim = candidates[rng.uniform<size_t>(0, candidates.size() - 1)];
            bool recovers = rng.coin_flip();
            cot::duration crash_dur = recovers ? rng.uniform(100ms, 500ms) : 0ms;

            (*active_failures)[victim] = true;
            ++*currently_failed;
            set_replica_channel_loss(inst, victim, 1.0);

            if (crash_dur > 0ms) {
                scheduled_events.push_back(recover_replica_after(
                    inst, inst.tester, victim, crash_dur, active_failures, currently_failed
                ));
            } else if (std::find(inst.tester.excluded_replicas.begin(),
                                 inst.tester.excluded_replicas.end(),
                                 victim) == inst.tester.excluded_replicas.end()) {
                inst.tester.excluded_replicas.push_back(victim);
            }
        } else if (roll < 0.7) {
            if (n < 2)
                continue;
            size_t a = rng.uniform<size_t>(0, n - 1);
            size_t b = a;
            while (b == a)
                b = rng.uniform<size_t>(0, n - 1);

            bool two_way = rng.coin_flip();
            bool heals = rng.coin_flip();
            cot::duration heal_after = heals ? rng.uniform(100ms, 400ms) : 0ms;

            scheduled_events.push_back(partition_replicas_after(
                inst, inst.tester, a, b, 0ms, heal_after, two_way
            ));
        }
    }
}

cot::task<> random_link_schedule(pt_paxos_instance& inst,
                                  testinfo& tester,
                                  std::shared_ptr<bool> running) {
    size_t n = tester.nreplicas;
    if (n < 2)
        co_return;
    double base_loss = tester.loss / 5.0;

    std::vector<std::vector<bool>> failed(n, std::vector<bool>(n, false));
    size_t num_failed_links = 0;
    size_t max_failed_links = n;

    while (*running) {
        co_await cot::after(tester.randomness.uniform(5s, 15s));
        if (!*running)
            break;

        if (num_failed_links < max_failed_links && tester.randomness.coin_flip(0.6)) {
            size_t i = tester.randomness.uniform(size_t(0), n - 1);
            size_t j = (i + 1 + tester.randomness.uniform(size_t(0), n - 2)) % n;
            if (!failed[i][j]) {
                fail(inst, i, j);
                failed[i][j] = true;
                failed[j][i] = true;
                ++num_failed_links;
            }
        } else if (num_failed_links > 0) {
            for (size_t i = 0; i < n; ++i) {
                for (size_t j = i + 1; j < n; ++j) {
                    if (failed[i][j] && tester.randomness.coin_flip(0.1)) {
                        recover(inst, i, j, base_loss);
                        failed[i][j] = false;
                        failed[j][i] = false;
                        --num_failed_links;
                    }
                }
            }
        }
    }
}

cot::task<> disruptive_isolate_routine(pt_paxos_instance& inst,
                                       cot::duration split_time = 10s,
                                       cot::duration heal_time = 10s) {
    co_await cot::after(split_time);

    size_t victim = inst.tester.nreplicas;
    std::vector<size_t> followers;
    followers.reserve(inst.tester.nreplicas);
    for (size_t s = 0; s < inst.tester.nreplicas; ++s) {
        if (inst.replicas[s]->leader_index_ != s)
            followers.push_back(s);
    }

    if (!followers.empty()) {
        victim = followers[inst.tester.randomness.uniform<size_t>(0, followers.size() - 1)];
        for (size_t s = 0; s < inst.tester.nreplicas; ++s) {
            if (s != victim)
                inst.replicas[s]->to_replicas_[victim]->set_loss(1.0);
        }
    }

    co_await cot::after(heal_time);

    if (victim != inst.tester.nreplicas) {
        for (size_t s = 0; s < inst.tester.nreplicas; ++s) {
            if (s != victim)
                inst.replicas[s]->to_replicas_[victim]->set_loss(inst.tester.loss);
        }
    }
}

cot::task<> stop_clients_and_clear_after(lockseq_model& clients,
                                         cot::duration run_for,
                                         cot::duration settle_for) {
    co_await cot::after(run_for);
    clients.stop();
    co_await cot::after(settle_for);
    cot::clear();
}

cot::task<> stop_clients_heal_links_and_clear_after(lockseq_model& clients,
                                                    pt_paxos_instance& inst,
                                                    std::shared_ptr<bool> link_schedule_running,
                                                    cot::duration run_for,
                                                    cot::duration settle_for) {
    co_await cot::after(run_for);
    clients.stop();
    *link_schedule_running = false;
    set_replica_to_replica_loss(inst, inst.tester.loss);
    co_await cot::after(settle_for);
    cot::clear();
}

cot::task<> heal_replicas_stop_clients_and_clear_after(lockseq_model& clients,
                                                       pt_paxos_instance& inst,
                                                       std::shared_ptr<bool> flapping_running,
                                                       cot::duration run_for,
                                                       cot::duration recovered_for,
                                                       cot::duration settle_for) {
    co_await cot::after(run_for);
    *flapping_running = false;
    for (size_t replica = 0; replica != inst.tester.nreplicas; ++replica) {
        set_replica_channel_loss(inst, replica, inst.tester.loss);
    }
    co_await cot::after(recovered_for);
    clients.stop();
    co_await cot::after(settle_for);
    cot::clear();
}

bool try_one_seed(testinfo& tester, unsigned long seed) {
    cot::reset();   // clear old events and coroutines
    tester.randomness.seed(seed);
    tester.excluded_replicas.clear();

    // Create client generator and test instance
    lockseq_model clients(tester.nreplicas, tester.randomness);
    pt_paxos_instance inst(tester, clients);

    // Start coroutines
    clients.start();
    std::vector<cot::task<>> tasks;
    for (size_t s = 0UL; s != tester.nreplicas; ++s) {
        tasks.push_back(inst.replicas[s]->run());
    }
    auto link_schedule_running =
        std::make_shared<bool>(tester.mode == failure_mode::william_link_schedule);
    auto flapping_running =
        std::make_shared<bool>(tester.mode == failure_mode::unstable_leader_mixed);

    switch (tester.mode) {
        case failure_mode::failed_leader:
            set_replica_channel_loss(inst, tester.initial_leader, 1);
            break;
        case failure_mode::failed_replica:
            set_replica_channel_loss(inst, tester.failed_replica, 1);
            break;
        case failure_mode::multiple_random_up_down:
            tasks.push_back(up_down_randomly(inst, tester.failed_replica, 3s));
            break;
        case failure_mode::unstable_leader_mixed: {
            std::vector<size_t> followers;
            followers.reserve(tester.nreplicas - 1);
            for (size_t s = 0; s != tester.nreplicas; ++s) {
                if (s != tester.initial_leader) {
                    followers.push_back(s);
                }
            }

            std::shuffle(followers.begin(), followers.end(), tester.randomness.engine());

            size_t flapping_count = followers.size() / 2;
            std::vector<size_t> flapping_followers(
                followers.begin(), followers.begin() + flapping_count
            );
            std::vector<size_t> partitioned_followers(
                followers.begin() + flapping_count, followers.end()
            );

            tester.excluded_replicas = partitioned_followers;

            std::vector<size_t> connected_group{tester.initial_leader};
            connected_group.insert(connected_group.end(),
                                   flapping_followers.begin(),
                                   flapping_followers.end());

            tasks.push_back(partition_groups_forever(
                inst, connected_group, partitioned_followers
            ));

            tasks.push_back(up_down_randomly(
                inst,
                static_cast<int>(tester.initial_leader),
                3s,
                flapping_running
            ));
            for (size_t s : flapping_followers) {
                tasks.push_back(up_down_randomly(
                    inst,
                    static_cast<int>(s),
                    3s,
                    flapping_running
                ));
            }
            break;
        }
        case failure_mode::random_failure_schedule:
            tasks.push_back(random_failure_schedule_task(inst));
            break;
        case failure_mode::william_link_schedule:
            tasks.push_back(random_link_schedule(inst, tester, link_schedule_running));
            break;
        case failure_mode::disruptive_isolate:
            tasks.push_back(disruptive_isolate_routine(inst));
            break;
        case failure_mode::split_brain:
            apply_split_brain(inst);
            break;
        case failure_mode::minority_partition_failure:
            tasks.push_back(minority_partition(inst, tester.initial_leader, 20s));
            break;
        case failure_mode::vihaan_split_brain_failure:
            tasks.push_back(failure_split_brain(inst, 10s, 20s));
            break;
        case failure_mode::cascading_star_partition:
            tasks.push_back(cascading_star_partition_scenario(inst));
            break;
        case failure_mode::split_brain_isolate_heal:
            tasks.push_back(split_brain_isolate_heal_schedule(inst, tester));
            break;
        case failure_mode::none:
            break;
        case failure_mode::delayed_leader_failure:
            tasks.push_back(fail_primary_after(inst, 10s));
            break;
    }

    cot::task<> timeout_task;
    if (tester.mode == failure_mode::william_link_schedule) {
        timeout_task = stop_clients_heal_links_and_clear_after(
            clients, inst, link_schedule_running, 100s, 10s
        );
    } else if (tester.mode == failure_mode::unstable_leader_mixed) {
        timeout_task = heal_replicas_stop_clients_and_clear_after(
            clients, inst, flapping_running, 80s, 20s, 10s
        );
    } else {
        timeout_task = stop_clients_and_clear_after(clients, 100s, 10s);
    }

    // Wait for `timeout_task`
    cot::loop();

    // Check database
    std::print("{} lock, {} write, {} clear, {} unlock\n",
               clients.lock_complete, clients.write_complete,
               clients.clear_complete, clients.unlock_complete);
    size_t reference = tester.initial_leader;
    if (tester.mode == failure_mode::failed_leader
        || tester.mode == failure_mode::split_brain)
        reference = (tester.initial_leader + 1) % tester.nreplicas;
    if (tester.mode == failure_mode::cascading_star_partition
        || tester.mode == failure_mode::unstable_leader_mixed
        || tester.mode == failure_mode::random_failure_schedule) {
        for (size_t s = 0; s != tester.nreplicas; ++s) {
            if (std::find(tester.excluded_replicas.begin(),
                          tester.excluded_replicas.end(),
                          s) == tester.excluded_replicas.end()) {
                reference = s;
                break;
            }
        }
    }
    pancy::pancydb& db = inst.replicas[reference]->db_;

    for (size_t s = 0; s != tester.nreplicas; ++s) {
        if (s == reference)
            continue;
        if (tester.mode == failure_mode::failed_leader && s == tester.initial_leader)
            continue;
        if (tester.mode == failure_mode::failed_replica
            && s == (size_t) tester.failed_replica)
            continue;
        if (tester.mode == failure_mode::delayed_leader_failure && s == tester.initial_leader)
            continue;
        if (tester.mode == failure_mode::split_brain && s == tester.initial_leader)
            continue;
        if ((tester.mode == failure_mode::cascading_star_partition
             || tester.mode == failure_mode::unstable_leader_mixed
             || tester.mode == failure_mode::random_failure_schedule)
            && std::find(tester.excluded_replicas.begin(),
                         tester.excluded_replicas.end(),
                         s) != tester.excluded_replicas.end())
            continue;
        auto problem = db.diff(
            inst.replicas[s]->db_,
            tester.mode == failure_mode::cascading_star_partition ? 15 : 5
        );
        if (problem) {
            std::print(std::clog,
                       "*** REPLICA DIVERGENCE on seed {} between replica {} and {} at key {}\n",
                       seed, reference, s, *problem);
            db.print_near(*problem, std::clog);
            inst.replicas[s]->db_.print_near(*problem, std::clog);
            return false;
        }


    }

    if (auto problem = clients.check(db)) {
        std::print(std::clog, "*** FAILURE on seed {} at key {}\n", seed, *problem);
        db.print_near(*problem, std::clog);
        return false;
    } else if (tester.print_db) {
        db.print(std::cout);
    }
    return true;
}
