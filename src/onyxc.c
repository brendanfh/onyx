#include "onyxc.h"
#include "onyxutils.h"

static void emit_string_literal(OnyxCFile* c_file, AstStrLit* strlit) {
    
    i8* strdata = bh_alloc_array(global_heap_allocator, i8, strlit->token->length + 1);
    i32 length  = string_process_escape_seqs(strdata, strlit->token->text, strlit->token->length);

    if (bh_table_has(CStringLiteral, c_file->string_literals, (char *) strdata)) {
        CStringLiteral sti = bh_table_get(CStringLiteral, c_file->string_literals, (char *) strdata);
        strlit->addr   = sti.number;
        strlit->length = sti.size;
        
        bh_free(global_heap_allocator, strdata);
        return;
    }

    CStringLiteral csl = {
        .number = c_file->next_string_literal_idx,
        .size   = length,
        .data   = strdata,
    };

    strlit->addr = csl.number;
    strlit->length = length;
    c_file->next_string_literal_idx += 1;

    bh_table_put(CStringLiteral, c_file->string_literals, (char *) strdata, csl);
}

void emit_c_entity(Entity *ent) {
    OnyxCFile* c_file = context.c_file;

    switch (ent->type) {
        case Entity_Type_String_Literal: {
            emit_string_literal(c_file, ent->strlit);
            break;
        }
    }

    ent->state = Entity_State_Finalized;
}


OnyxCFile onyx_c_file_create(bh_allocator alloc) {
    OnyxCFile cfile;

    cfile.memory_reservations = NULL;
    bh_arr_new(alloc, cfile.memory_reservations, 4);

    cfile.next_string_literal_idx = 0;
    cfile.string_literals = NULL;
    bh_table_init(alloc, cfile.string_literals, 4);

    return cfile;
}

#include "onyxc_output.c"

