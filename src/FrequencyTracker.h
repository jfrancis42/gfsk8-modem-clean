/**
 * @file FrequencyTracker.h
 * @brief Kalman-style frequency and timing trackers for the JS8 decoder.
 *
 * This header is a redirect to tracker.h, which contains the full header-only
 * implementation of gfsk8::FrequencyTracker and gfsk8::TimingTracker with cleaner
 * organisation and additional inline documentation.
 *
 * Including either FrequencyTracker.h or tracker.h is equivalent;
 * JS8.cpp and api.cpp include FrequencyTracker.h by convention.
 */
#pragma once

#include "tracker.h"

// gfsk8::FrequencyTracker and gfsk8::TimingTracker are defined in tracker.h.
