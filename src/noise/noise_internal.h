/* noise module internals */
#ifndef CW_NOISE_INTERNAL_H
#define CW_NOISE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/* the gen://noise generator: query is what follows "gen://noise?" (worker
 * safe; logs and returns false on a bad query) */
bool cw__noise_image_query(const char *query, uint8_t **rgba, int *w, int *h);

#endif
