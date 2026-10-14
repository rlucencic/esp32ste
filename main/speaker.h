#pragma once

#include <stdbool.h>

/* Stream the ST's sound to the MAX98357A amp (takes over GPIO19/20, so the
 * native USB port goes away). Runs its own task on core 0.
 */
bool speaker_start(void);
