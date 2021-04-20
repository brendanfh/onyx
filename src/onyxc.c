#include "onyxc.h"
#include "onyxutils.h"
#include "onyxerrors.h"

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

#include "onyxc_output.c"

