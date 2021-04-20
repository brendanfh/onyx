#include "onyxc.h"
#include "onyxutils.h"
#include "onyxerrors.h"

static void emit_raw_data(OnyxCFile* c_file, ptr data, AstTyped* node) {
    switch (node->kind) {
    case Ast_Kind_Array_Literal: {
        AstArrayLiteral* al = (AstArrayLiteral *) node;

        i32 i = 0;
        i32 elem_size = type_size_of(al->type->Array.elem);

        bh_arr_each(AstTyped *, expr, al->values) {
            emit_raw_data(c_file, bh_pointer_add(data, i * elem_size), *expr);
            i++;
        }

        break;    
    }

    case Ast_Kind_Struct_Literal: {
        AstStructLiteral* sl = (AstStructLiteral *) node;

        Type* sl_type = sl->type;
        assert(sl_type->kind == Type_Kind_Struct);

        i32 i = 0;
        bh_arr_each(AstTyped *, expr, sl->args.values) {
            emit_raw_data(c_file, bh_pointer_add(data, sl_type->Struct.memarr[i]->offset), *expr);
            i++;
        }

        break;
    }

    case Ast_Kind_StrLit: {
        AstStrLit* sl = (AstStrLit *) node;

        // :Incomplete this does not work because sl->addr is not the address in C.
        // Need some way to encode &__stringXX
        //
        // NOTE: This assumes the address and the length fields have been filled out
        // by emit_string_literal.
        u32* sdata = (u32 *) data;
        sdata[0] = sl->addr;
        sdata[1] = 0x00;
        sdata[2] = sl->length;
        break;
    }

    case Ast_Kind_Enum_Value: {
        AstEnumValue* ev = (AstEnumValue *) node;
        emit_raw_data(c_file, data, (AstTyped *) ev->value);
        break;
    }

    case Ast_Kind_Function: {
        AstFunction* func = (AstFunction *) node;
        *((u32 *) data) = 0; // :Incomplete
        break;
    }

    case Ast_Kind_NumLit: {
        // NOTE: This makes a big assumption that we are running on a
        // little endian machine, since WebAssembly is little endian
        // by specification. This is probably a safe assumption, but
        // should probably be fixed at some point.
        //                                - brendanfh  2020/12/15

        Type* effective_type = node->type;

        if (effective_type->kind == Type_Kind_Enum)
            effective_type = effective_type->Enum.backing;
        
        switch (effective_type->Basic.kind) {
        case Basic_Kind_Bool:
        case Basic_Kind_I8:
        case Basic_Kind_U8:
            *((i8 *) data) = (i8) ((AstNumLit *) node)->value.i;
            return;

        case Basic_Kind_I16:
        case Basic_Kind_U16:
            *((i16 *) data) = (i16) ((AstNumLit *) node)->value.i;
            return;

        case Basic_Kind_I32:
        case Basic_Kind_U32:
            *((i32 *) data) = ((AstNumLit *) node)->value.i;
            return;

        case Basic_Kind_I64:
        case Basic_Kind_U64:
        case Basic_Kind_Rawptr:
            *((i64 *) data) = ((AstNumLit *) node)->value.l;
            return;

        case Basic_Kind_F32:
            *((f32 *) data) = ((AstNumLit *) node)->value.f;
            return;

        case Basic_Kind_F64:
            *((f64 *) data) = ((AstNumLit *) node)->value.d;
            return;

        default: break;
        }

        //fallthrough
    }
    default: onyx_report_error(node->token->pos,
            "Cannot generate constant data for '%s'.",
            onyx_ast_node_kind_string(node->kind));
    }
}

static void emit_memory_reservation(OnyxCFile* c_file, AstMemRes* memres) {
    Type* effective_type = memres->type;

    u64 alignment = type_alignment_of(effective_type);
    u64 size = type_size_of(effective_type);
    u8* data = NULL;

    if (memres->initial_value != NULL) {
        data = bh_alloc(global_heap_allocator, size);
        emit_raw_data(c_file, data, memres->initial_value);
    }

    CMemoryReservation cmr = {
        .number = bh_arr_length(c_file->memory_reservations),
        .size = size,
        .initial_value = (u8 *) data,
    };

    bh_arr_push(c_file->memory_reservations, cmr);
    memres->addr = cmr.number;
}

// :Duplicated
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

// :Duplicated
static void emit_file_contents(OnyxCFile* c_file, AstFileContents* fc) {
    token_toggle_end(fc->filename);

    if (bh_table_has(LoadedFileInfo, c_file->loaded_file_info, fc->filename->text)) {
        LoadedFileInfo info = bh_table_get(LoadedFileInfo, c_file->loaded_file_info, fc->filename->text);
        fc->addr = info.number;
        fc->size = info.size;

        token_toggle_end(fc->filename);
        return;
    }

    if (!bh_file_exists(fc->filename->text)) {
        onyx_report_error(fc->filename->pos,
                "Unable to open file for reading, '%s'.",
                fc->filename->text);

        token_toggle_end(fc->filename);
        return;
    }

    bh_file_contents contents = bh_file_read_contents(global_heap_allocator, fc->filename->text);
    u8* actual_data = bh_alloc(global_heap_allocator, contents.length + 1);
    u32 length = contents.length + 1;
    memcpy(actual_data, contents.data, contents.length);
    actual_data[contents.length] = 0;
    bh_file_contents_free(&contents);

    LoadedFileInfo lfi = {
        .number = c_file->next_file_contents_idx,
        .size   = length,
        .data   = actual_data,
    };
    c_file->next_file_contents_idx++;

    bh_table_put(LoadedFileInfo, c_file->loaded_file_info, fc->filename->text, lfi);

    fc->addr = lfi.number;
    fc->size = length - 1;
}

void emit_c_entity(Entity *ent) {
    OnyxCFile* c_file = context.c_file;

    switch (ent->type) {
        case Entity_Type_String_Literal: {
            emit_string_literal(c_file, ent->strlit);
            break;
        }

        case Entity_Type_File_Contents: {
            emit_file_contents(c_file, ent->file_contents);
            break;
        }

        case Entity_Type_Memory_Reservation: {
            emit_memory_reservation(c_file, ent->mem_res);
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
    bh_table_init(alloc, cfile.string_literals, 16);

    cfile.next_file_contents_idx = 0;
    cfile.loaded_file_info = NULL;
    bh_table_init(alloc, cfile.loaded_file_info, 4);

    return cfile;
}

void onyx_c_file_free(OnyxCFile* c_file) {
    bh_arr_free(c_file->memory_reservations);
    bh_table_free(c_file->string_literals);
    bh_table_free(c_file->loaded_file_info);
}

#include "onyxc_output.c"

