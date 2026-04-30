#pragma once
#include <stdbool.h>
#include <stdint.h>

void buttons_init(void);

/* Pop one (pressed, doomKey) event from the queue. Returns 1 if an event
 * was popped, 0 if the queue was empty. Used directly by DG_GetKey. */
int buttons_pop_doomkey(int *pressed, unsigned char *doomKey);
