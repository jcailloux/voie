#pragma once

#include <voie/types.h>
#include <voie/handler.h>
#include <voie/ctx.h>
#include <voie/group.h>
#include <voie/app.h>

#include <memory>
#include <string>
#include <string_view>

namespace voie {

// Pre-build a complete HTTP response at startup.
// At request time, the pre-built bytes are sent directly — no parsing,
// no allocation, no build_response(). Ideal for static/benchmark routes.
inline handler prebuilt(std::string_view body, std::string_view content_type) {
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: ";
    resp += content_type;
    resp += "\r\nContent-Length: ";
    resp += std::to_string(body.size());
    resp += "\r\nConnection: keep-alive\r\n\r\n";
    resp += body;

    auto shared = std::make_shared<std::string>(std::move(resp));
    return handler([shared](ctx& c) {
        c.send_prebuilt(shared->data(), shared->size());
    });
}

} // namespace voie
