#include "onyxc.h"


void emit_c_entity(Entity *ent) {

    ent->state = Entity_State_Finalized;
}

#include "onyxc_output.c"

