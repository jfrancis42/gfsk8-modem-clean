#pragma once
/**
 * @file constants.h
 * @brief JS8 protocol constants and per-submode parameters.
 *
 * Provides all compile-time constants needed by js8codec.cpp.
 * The global mutable state (audio buffer, FFT mutex) is bridged from
 * commons.h so that both JS8.cpp and js8codec.cpp share the same
 * runtime globals defined in api.cpp.
 */

#include "commons.h"  // RxAudioBuffer dec_data, fftw_mutex, all GFSK8_* macros

// ── Name aliases bridging js8codec.cpp to commons.h globals ──────────────────
// js8codec.cpp references g_decodeData / g_fftMutex; commons.h declares the
// same data as dec_data / fftw_mutex.  These #defines bridge the gap.

#define g_decodeData dec_data
#define g_fftMutex   fftw_mutex

// ── Type alias ────────────────────────────────────────────────────────────────
// js8codec.cpp uses DecodeData as the struct type name.
using DecodeData = RxAudioBuffer;

// ── Spectrum alias ────────────────────────────────────────────────────────────
// js8codec.cpp may reference g_specData; map it to the commons.h specData.
#define g_specData specData

// ── Per-submode constants ─────────────────────────────────────────────────────
// These are also defined in commons.h, so no redefinition needed.
// (GFSK8_NORMAL_SYMBOL_SAMPLES, GFSK8_NORMAL_TX_SECONDS, etc. are already provided above.)

// ── UTC helpers ───────────────────────────────────────────────────────────────
// Provided by commons.h as code_time() and decode_time().
// HourMinuteSecond is aliased here.
using HourMinuteSecond = hour_minute_second;
