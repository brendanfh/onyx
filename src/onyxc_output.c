#include "onyxc.h"


static char* BOILERPLATE_TOP =
    "#include <stdlib.h>\n"
    "#include <stdint.h>\n"
    "typedef uint8_t  u8;\n"
    "typedef uint16_t u16;\n"
    "typedef uint32_t u32;\n"
    "typedef uint64_t u64;\n"
    "typedef int8_t   i8;\n"
    "typedef int16_t  i16;\n"
    "typedef int32_t  i32;\n"
    "typedef int64_t  i64;\n"
    "typedef float    f32;\n"
    "typedef double   f64;\n"
    "typedef void    *rawptr;\n";

static char* REGISTER_DEFINITION =
    "typedef union Register {\n"
    "    u8 u8;\n"
    "    i8 i8;\n"
    "    i16 i16;\n"
    "    u16 u16;\n"
    "    i32 i32;\n"
    "    u32 u32;\n"
    "    i64 i64;\n"
    "    u64 u64;\n"
    "    f32 f32;\n"
    "    f64 f64;\n"
    "    rawptr rawptr;\n"
    "} Register;\n";

static char* ENTRYPOINT =
    "int main(int argc, char* argv[]) {\n"
    "    puts(\"Nothing so far!\");\n"
    "    return 0;\n"
    "}\n";

static void output_string_literals(OnyxCFile* c_file, bh_file file) {
    bh_table_each_start(CStringLiteral, c_file->string_literals);
        bh_fprintf(&file, "static const char __string%d[] = {", value.number);
        fori (i, 0, value.size) {
            bh_fprintf(&file, "%d,", (u32) value.data[i]);
        }
        bh_fprintf(&file, "0};\n");
    bh_table_each_end;
}

void onyx_output_c_file(OnyxCFile* c_file, bh_file file) {
    bh_file_write(&file, BOILERPLATE_TOP, strlen(BOILERPLATE_TOP));
    bh_file_write(&file, REGISTER_DEFINITION, strlen(REGISTER_DEFINITION));

    output_string_literals(c_file, file);

    bh_file_write(&file, ENTRYPOINT, strlen(ENTRYPOINT));
}
