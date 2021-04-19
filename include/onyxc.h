#ifndef ONYXC_H
#define ONYXC_H

#include "bh.h"
#include "onyxastnodes.h"

typedef struct CMemoryReservation {
    i32   number;
    i32   size;
    void* initial_value;
} CMemoryReservation;

typedef struct CStringLiteral {
    i32   number;
    i32   size;
    char* data;
} CStringLiteral;

typedef struct OnyxCFile {
    
    bh_arr(CMemoryReservation) memory_reservations;

    u32 next_string_literal_idx;
    bh_table(CStringLiteral) string_literals;


} OnyxCFile;


OnyxCFile onyx_c_file_create(bh_allocator alloc);
void onyx_output_c_file(OnyxCFile* cfile, bh_file file);

#endif
