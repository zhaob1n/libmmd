#ifndef CMM_NSIMPLEX_H
#define CMM_NSIMPLEX_H
#include "base.h"
/* rank[] must hold a feasible initial ranking (e.g. longest path). Edges with valid[e]==0 are ignored. */
[[nodiscard]] bool network_simplex(Arena *A, size_t N, size_t E, const int32_t eu[], const int32_t ev[],
                                   const int32_t minlen[], const uint8_t valid[], const uint8_t hidden[],
                                   int32_t rank[], size_t max_iter);
#endif
#ifndef CMM_FAS_DECL
#define CMM_FAS_DECL
[[nodiscard]] bool greedy_fas(Arena *A, size_t N, size_t E, const int32_t ef[], const int32_t et[],
                              const uint8_t valid[], const uint8_t hidden[], uint8_t rev[]);
#endif
