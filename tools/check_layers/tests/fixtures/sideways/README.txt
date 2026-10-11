src/anim/x.c includes <dengine/de_physics.h> and src/physics/y.c includes "audio/mixer.h"
([[forbid]] edges), and src/input/z.c includes <dengine/de_net.h> (same band, no allow edge).
Expected: exit 1.
