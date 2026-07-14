#ifndef GSR_KDE_NIGHT_LIGHT_H
#define GSR_KDE_NIGHT_LIGHT_H

#include <stdbool.h>

typedef struct gsr_kde_night_light gsr_kde_night_light;

gsr_kde_night_light* gsr_kde_night_light_create(void);
void gsr_kde_night_light_destroy(gsr_kde_night_light *self);
/* Returns true when night light is active. |inverse_matrix| is filled with the row major 3x3 matrix that removes the night light tint from linear rgb values */
bool gsr_kde_night_light_get_inverse_matrix(gsr_kde_night_light *self, float inverse_matrix[9]);

#endif /* GSR_KDE_NIGHT_LIGHT_H */
