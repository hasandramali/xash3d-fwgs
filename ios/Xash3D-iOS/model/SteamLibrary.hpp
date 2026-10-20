#pragma once
#include "SteamWire.hpp"

namespace svensteam {
inline Buffer svenOwnedGamesQuery(uint64_t steamID) {
    if (!steamID) throw std::runtime_error("Steam login required");
    Buffer request;
    number(request, 1, steamID);
    number(request, 3, 1);
    number(request, 4, 225840);
    number(request, 5, 1);
    number(request, 6, 0);
    return request;
}

inline Buffer svenFreeLicenseQuery() {
    Buffer request; number(request, 2, 225840); return request;
}

inline bool ownsSven(const Buffer &response) {
    bool owned = false;
    for (const auto &field : fields(response)) {
        if (field.id != 2) continue;
        if (field.wire != 2) throw std::runtime_error("Invalid Steam library response");
        uint64_t appID = 0;
        for (const auto &game : fields(field.bytes)) {
            if (game.id != 1) continue;
            if (game.wire != 0) throw std::runtime_error("Invalid Steam library response");
            appID = game.value;
        }
        if (appID == 225840) owned = true;
    }
    return owned;
}
}
