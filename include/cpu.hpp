// Instructions propres au processeur, avec repli portable hors x86.
//   cpu_pause()              _mm_pause() sur x86 (allège une attente active), sinon std::this_thread::yield()
//   tsc()                    compteur d'horodatage (rdtsc) sur x86, sinon steady_clock en nanosecondes
//   tsc_debut() / tsc_fin()  lectures encadrées de lfence (x86) pour le benchmark de latence
// Seul x86-64 a été exécuté (g++ MinGW en local, g++ Linux en CI). Le repli (ARM par exemple) n'a été
// compilé et exécuté que sous x86, en forçant -DMARCHE_SANS_X86 ; sur ARM, les latences mesurées seraient
// celles de steady_clock, pas d'un compteur de cycles.
#pragma once
#include <chrono>
#include <cstdint>
#include <thread>

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__)) && !defined(MARCHE_SANS_X86)
#include <x86intrin.h>
#define MARCHE_X86 1
#endif

namespace marche {

inline void cpu_pause() {
#ifdef MARCHE_X86
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

inline uint64_t tsc() {
#ifdef MARCHE_X86
  return __rdtsc();
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

inline uint64_t tsc_debut() {
#ifdef MARCHE_X86
  _mm_lfence();
  const uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return tsc();
#endif
}

inline uint64_t tsc_fin() {
#ifdef MARCHE_X86
  unsigned aux;
  const uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return tsc();
#endif
}

}  // namespace marche
