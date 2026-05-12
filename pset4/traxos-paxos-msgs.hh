#pragma once

#include <cstdint>
#include <deque>
#include <utility>
#include <variant>
#include <vector>

struct request {
    uint64_t serial;
    unsigned client = 0;
    unsigned value = 0;
};

struct response {
    uint64_t serial;
    bool redirect = false;
    size_t redirection = 0;
};

inline uint64_t message_serial(const request& req) {
    return req.serial;
}

inline uint64_t message_serial(const response& resp) {
    return resp.serial;
}

struct dummy_state {
    std::vector<std::pair<unsigned, unsigned>> log;

    response process(const request& req) {
        log.emplace_back(req.client, req.value);
        return {req.serial};
    }
};

struct base_message {
    unsigned long long round = 0;
};

struct probe_msg : base_message {
};

struct prepare_msg : base_message {
    unsigned long long accepted_round = 0;
    std::deque<request> accepted_values;
    std::deque<unsigned long long> accepted_rounds;
};

struct propose_msg : base_message {
    unsigned long long committed_slot = 0;
    unsigned long long batch_start = 0;
    std::deque<request> entries;
};

struct commit_msg : base_message {
    unsigned long long committed_slot = 0;
};

struct ack_msg : base_message {
    bool success = false;
    unsigned long long highest_accepted = 0;
};

using paxos_message = std::variant<propose_msg, probe_msg, prepare_msg, commit_msg, ack_msg>;

