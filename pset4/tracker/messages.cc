#include "tracker/messages.hh"

#include "tracker/http.hh"
#include "tracker/state.hh"

namespace tracker {

static announce_response process_announce(bt_tracker_state& state,
                                          const announce_request& req) {
    apply_announce(state, req.announce);
    return {response_header(req), tracker_success_response(state, req.announce)};
}

response process_request(bt_tracker_state& state, const request& req) {
    return std::visit([&](auto&& reqt) -> response {
        return process_announce(state, reqt);
    }, req);
}

}
