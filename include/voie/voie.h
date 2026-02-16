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

/// Build a complete HTTP response at startup for zero-copy serving.
///
/// The returned handler sends the pre-built bytes directly to the client,
/// bypassing response construction entirely.  Ideal for static content
/// or benchmark routes.
///
/// @code
/// app.get("/health", voie::prebuilt("OK", "text/plain"));
/// @endcode
///
/// @param body          Response body.
/// @param content_type  MIME type (e.g. `"text/plain"`).
/// @return A handler that sends the pre-built response.
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
