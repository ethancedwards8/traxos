#pragma once

#include "tracker/types.hh"

#include <cstdint>
#include <string>
#include <variant>

namespace tracker {

enum class errc {
    ok = 0,
    redirect = -1
};

struct message_base {
    uint64_t serial = 0;
};

struct request_base : public message_base {
};

struct response_base : public message_base {
    errc errcode = errc::ok;
};

struct announce_request : public request_base {
    using response_type = struct announce_response;
    bt_tracker_announce_request announce;
};

struct announce_response : public response_base {
    std::string body;
};

struct redirection_response : public response_base {
    size_t redirection = 0;
};

using request = std::variant<announce_request>;

using response = std::variant<
    redirection_response,
    announce_response
>;

inline constexpr uint64_t message_serial(const message_base& msg) noexcept {
    return msg.serial;
}

inline constexpr uint64_t message_serial(const request& req) noexcept {
    return std::visit([](auto&& reqt) -> uint64_t {
        return reqt.serial;
    }, req);
}

inline constexpr uint64_t message_serial(const response& resp) noexcept {
    return std::visit([](auto&& respt) -> uint64_t {
        return respt.serial;
    }, resp);
}

inline constexpr response_base response_header(const request_base& req,
                                               errc errcode = errc::ok) noexcept {
    return {{req.serial}, errcode};
}

inline constexpr response_base response_header(const request& req,
                                               errc errcode = errc::ok) noexcept {
    return {{message_serial(req)}, errcode};
}

response process_request(bt_tracker_state& state, const request& req);

}
