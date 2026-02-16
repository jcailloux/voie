#pragma once

/// @file
/// Optional macro to reduce lambda boilerplate.
///
/// Not included by `<voie/voie.h>` — users must opt in explicitly.
///
/// @code
/// #include <voie/macros.h>
/// app.get("/", V { c.text("hello"); });
/// @endcode

/// Shorthand for `[](voie::ctx& c)`.
#define V [](voie::ctx& c)
