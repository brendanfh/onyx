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

typedef struct CFile {
    
    bh_arr(CMemoryReservation) memory_reservations;
    bh_arr(CStringLiteral)     string_literals;

} CFile;


#endif
