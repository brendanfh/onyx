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




void onyx_output_c_file(OnyxCFile* cfile, bh_file file) {
    bh_file_write(&file, BOILERPLATE_TOP, strlen(BOILERPLATE_TOP));
}
