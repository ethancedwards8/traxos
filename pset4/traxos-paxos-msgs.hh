#pragma once

#include "tracker-state.hh"
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct request {
    uint64_t serial;
    bt_tracker_announce_request announce;
};

struct response {
    uint64_t serial;
    bool redirect = false;
    size_t redirection = 0;
    std::string body;
};

inline uint64_t message_serial(const request& req) {
    return req.serial;
}

inline uint64_t message_serial(const response& resp) {
    return resp.serial;
}

struct tracker_state_machine {
    bt_tracker_state tracker;
    std::vector<uint64_t> log;
    unsigned long started_count = 0;
    unsigned long stopped_count = 0;
    unsigned long completed_count = 0;

    response process(const request& req) {
        apply_announce(tracker, req.announce);
        log.push_back(req.serial);
        if (req.announce.event == started) {
            ++started_count;
        } else if (req.announce.event == stopped) {
            ++stopped_count;
        } else if (req.announce.event == completed) {
            ++completed_count;
        }
        return {req.serial, false, 0, tracker_success_response(tracker, req.announce)};
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
