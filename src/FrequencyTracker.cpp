/**
 * @file FrequencyTracker.cpp
 * @brief Stub translation unit for the header-only tracker implementation.
 *
 * FrequencyTracker and TimingTracker are now fully defined in tracker.h
 * (included via FrequencyTracker.h).  This .cpp exists solely so that
 * CMakeLists.txt can list FrequencyTracker.cpp in the source set without
 * modification; there is nothing to compile here.
 *
 * Background: the original FrequencyTracker.cpp contained ~185 lines of
 * member-function definitions.  The rewritten implementation inlines all
 * methods directly in the class bodies in tracker.h, which is the
 * idiomatic approach for small, performance-sensitive classes like these
 * Kalman-style trackers.  Inlining also lets the compiler inline the tiny
 * per-sample apply() loop into the decode hot path.
 */
#include "FrequencyTracker.h"

// Nothing to define here — all methods are inline in tracker.h.
