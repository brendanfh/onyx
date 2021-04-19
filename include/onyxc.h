#ifndef ONYXC_H
#define ONYXC_H

#include "bh.h"
#include "onyxastnodes.h"

typedef struct CMemoryReservation {
    i32   number;
    i32   size;
    char* initial_value;
} CMemoryReservation;

typedef struct CStringLiteral {
    i32   number;
    i32   size;
    char* data;
} CStringLiteral;

typedef struct OnyxCFile {
    
    bh_arr(CMemoryReservation) memory_reservations;
    bh_arr(CStringLiteral)     string_literals;

} OnyxCFile;


void emit_c_entity(Entity* ent);
void onyx_output_c_file(OnyxCFile* cfile, bh_file file);

#endif
