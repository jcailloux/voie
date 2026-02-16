#pragma once

// Optional macro to reduce lambda boilerplate.
// Not included by voie/voie.h — users must opt in explicitly.
//
// Usage:
//   #include <voie/macros.h>
//   app.get("/", V { c.text("hello"); });

#define V [](voie::ctx& c)
