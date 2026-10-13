// Utilitaires partagés (header seul).
#pragma once
#include <cstdint>

// SplitMix64 : dérive des graines décorrélées à partir de (graine, indice).
// Seeder des mt19937_64 avec 0,1,2... donne des flux voisins ; on les disperse.
inline uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}
