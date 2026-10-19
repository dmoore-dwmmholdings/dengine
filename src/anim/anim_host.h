#ifndef DE_ANIM_HOST_H
#define DE_ANIM_HOST_H
#include <dengine/de_anim.h>
/* Host's optional renderer follows engine changes until the game explicitly binds one. */
void de__anim_bind_renderer(de_anim_system *, de_renderer *);
#endif
