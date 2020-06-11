#include "onyxwasm.h"

// NOTE: Allows easier testing of types since most of the characters
// corresponding to these values are not printable
#if 0
const WasmType WASM_TYPE_INT32 = 0x7F;
const WasmType WASM_TYPE_INT64 = 0x7E;
const WasmType WASM_TYPE_FLOAT32 = 0x7D;
const WasmType WASM_TYPE_FLOAT64 = 0x7C;
#else
const WasmType WASM_TYPE_INT32 = 'A';
const WasmType WASM_TYPE_INT64 = 'B';
const WasmType WASM_TYPE_FLOAT32 = 'C';
const WasmType WASM_TYPE_FLOAT64 = 'D';
#endif

static const char* wi_string(WasmInstructionType wit) {
	switch (wit) {
		case WI_UNREACHABLE: return "WI_UNREACHABLE";
		case WI_NOP: return "WI_NOP";
		case WI_BLOCK_START: return "WI_BLOCK_START";
		case WI_BLOCK_END: return "WI_BLOCK_END";
		case WI_LOOP_START: return "WI_LOOP_START";
		case WI_IF_START: return "WI_IF_START";
		case WI_ELSE: return "WI_ELSE";
		case WI_JUMP: return "WI_JUMP";
		case WI_COND_JUMP: return "WI_COND_JUMP";
		case WI_JUMP_TABLE: return "WI_JUMP_TABLE";
		case WI_RETURN: return "WI_RETURN";
		case WI_CALL: return "WI_CALL";
		case WI_CALL_INDIRECT: return "WI_CALL_INDIRECT";
		case WI_DROP: return "WI_DROP";
		case WI_SELECT: return "WI_SELECT";
		case WI_LOCAL_GET: return "WI_LOCAL_GET";
		case WI_LOCAL_SET: return "WI_LOCAL_SET";
		case WI_LOCAL_TEE: return "WI_LOCAL_TEE";
		case WI_GLOBAL_GET: return "WI_GLOBAL_GET";
		case WI_GLOBAL_SET: return "WI_GLOBAL_SET";
		case WI_I32_LOAD: return "WI_I32_LOAD";
		case WI_I64_LOAD: return "WI_I64_LOAD";
		case WI_F32_LOAD: return "WI_F32_LOAD";
		case WI_F64_LOAD: return "WI_F64_LOAD";
		case WI_I32_LOAD_8_S: return "WI_I32_LOAD_8_S";
		case WI_I32_LOAD_8_U: return "WI_I32_LOAD_8_U";
		case WI_I32_LOAD_16_S: return "WI_I32_LOAD_16_S";
		case WI_I32_LOAD_16_U: return "WI_I32_LOAD_16_U";
		case WI_I64_LOAD_8_S: return "WI_I64_LOAD_8_S";
		case WI_I64_LOAD_8_U: return "WI_I64_LOAD_8_U";
		case WI_I64_LOAD_16_S: return "WI_I64_LOAD_16_S";
		case WI_I64_LOAD_16_U: return "WI_I64_LOAD_16_U";
		case WI_I64_LOAD_32_S: return "WI_I64_LOAD_32_S";
		case WI_I64_LOAD_32_U: return "WI_I64_LOAD_32_U";
		case WI_I32_STORE: return "WI_I32_STORE";
		case WI_I64_STORE: return "WI_I64_STORE";
		case WI_F32_STORE: return "WI_F32_STORE";
		case WI_F64_STORE: return "WI_F64_STORE";
		case WI_I32_STORE_8: return "WI_I32_STORE_8";
		case WI_I32_STORE_16: return "WI_I32_STORE_16";
		case WI_I64_STORE_8: return "WI_I64_STORE_8";
		case WI_I64_STORE_16: return "WI_I64_STORE_16";
		case WI_I64_STORE_32: return "WI_I64_STORE_32";
		case WI_MEMORY_SIZE: return "WI_MEMORY_SIZE";
		case WI_MEMORY_GROW: return "WI_MEMORY_GROW";
		case WI_I32_CONST: return "WI_I32_CONST";
		case WI_I64_CONST: return "WI_I64_CONST";
		case WI_F32_CONST: return "WI_F32_CONST";
		case WI_F64_CONST: return "WI_F64_CONST";
		case WI_I32_EQZ: return "WI_I32_EQZ";
		case WI_I32_EQ: return "WI_I32_EQ";
		case WI_I32_NE: return "WI_I32_NE";
		case WI_I32_LT_S: return "WI_I32_LT_S";
		case WI_I32_LT_U: return "WI_I32_LT_U";
		case WI_I32_GT_S: return "WI_I32_GT_S";
		case WI_I32_GT_U: return "WI_I32_GT_U";
		case WI_I32_LE_S: return "WI_I32_LE_S";
		case WI_I32_LE_U: return "WI_I32_LE_U";
		case WI_I32_GE_S: return "WI_I32_GE_S";
		case WI_I32_GE_U: return "WI_I32_GE_U";
		case WI_I64_EQZ: return "WI_I64_EQZ";
		case WI_I64_EQ: return "WI_I64_EQ";
		case WI_I64_NE: return "WI_I64_NE";
		case WI_I64_LT_S: return "WI_I64_LT_S";
		case WI_I64_LT_U: return "WI_I64_LT_U";
		case WI_I64_GT_S: return "WI_I64_GT_S";
		case WI_I64_GT_U: return "WI_I64_GT_U";
		case WI_I64_LE_S: return "WI_I64_LE_S";
		case WI_I64_LE_U: return "WI_I64_LE_U";
		case WI_I64_GE_S: return "WI_I64_GE_S";
		case WI_I64_GE_U: return "WI_I64_GE_U";
		case WI_F32_EQ: return "WI_F32_EQ";
		case WI_F32_NE: return "WI_F32_NE";
		case WI_F32_LT: return "WI_F32_LT";
		case WI_F32_GT: return "WI_F32_GT";
		case WI_F32_LE: return "WI_F32_LE";
		case WI_F32_GE: return "WI_F32_GE";
		case WI_F64_EQ: return "WI_F64_EQ";
		case WI_F64_NE: return "WI_F64_NE";
		case WI_F64_LT: return "WI_F64_LT";
		case WI_F64_GT: return "WI_F64_GT";
		case WI_F64_LE: return "WI_F64_LE";
		case WI_F64_GE: return "WI_F64_GE";
		case WI_I32_CLZ: return "WI_I32_CLZ";
		case WI_I32_CTZ: return "WI_I32_CTZ";
		case WI_I32_POPCNT: return "WI_I32_POPCNT";
		case WI_I32_ADD: return "WI_I32_ADD";
		case WI_I32_SUB: return "WI_I32_SUB";
		case WI_I32_MUL: return "WI_I32_MUL";
		case WI_I32_DIV_S: return "WI_I32_DIV_S";
		case WI_I32_DIV_U: return "WI_I32_DIV_U";
		case WI_I32_REM_S: return "WI_I32_REM_S";
		case WI_I32_REM_U: return "WI_I32_REM_U";
		case WI_I32_AND: return "WI_I32_AND";
		case WI_I32_OR: return "WI_I32_OR";
		case WI_I32_XOR: return "WI_I32_XOR";
		case WI_I32_SHL: return "WI_I32_SHL";
		case WI_I32_SHR_S: return "WI_I32_SHR_S";
		case WI_I32_SHR_U: return "WI_I32_SHR_U";
		case WI_I32_ROTL: return "WI_I32_ROTL";
		case WI_I32_ROTR: return "WI_I32_ROTR";
		case WI_I64_CLZ: return "WI_I64_CLZ";
		case WI_I64_CTZ: return "WI_I64_CTZ";
		case WI_I64_POPCNT: return "WI_I64_POPCNT";
		case WI_I64_ADD: return "WI_I64_ADD";
		case WI_I64_SUB: return "WI_I64_SUB";
		case WI_I64_MUL: return "WI_I64_MUL";
		case WI_I64_DIV_S: return "WI_I64_DIV_S";
		case WI_I64_DIV_U: return "WI_I64_DIV_U";
		case WI_I64_REM_S: return "WI_I64_REM_S";
		case WI_I64_REM_U: return "WI_I64_REM_U";
		case WI_I64_AND: return "WI_I64_AND";
		case WI_I64_OR: return "WI_I64_OR";
		case WI_I64_XOR: return "WI_I64_XOR";
		case WI_I64_SHL: return "WI_I64_SHL";
		case WI_I64_SHR_S: return "WI_I64_SHR_S";
		case WI_I64_SHR_U: return "WI_I64_SHR_U";
		case WI_I64_ROTL: return "WI_I64_ROTL";
		case WI_I64_ROTR: return "WI_I64_ROTR";
		case WI_F32_ABS: return "WI_F32_ABS";
		case WI_F32_NEG: return "WI_F32_NEG";
		case WI_F32_CEIL: return "WI_F32_CEIL";
		case WI_F32_FLOOR: return "WI_F32_FLOOR";
		case WI_F32_TRUNC: return "WI_F32_TRUNC";
		case WI_F32_NEAREST: return "WI_F32_NEAREST";
		case WI_F32_SQRT: return "WI_F32_SQRT";
		case WI_F32_ADD: return "WI_F32_ADD";
		case WI_F32_SUB: return "WI_F32_SUB";
		case WI_F32_MUL: return "WI_F32_MUL";
		case WI_F32_DIV: return "WI_F32_DIV";
		case WI_F32_MIN: return "WI_F32_MIN";
		case WI_F32_MAX: return "WI_F32_MAX";
		case WI_F32_COPYSIGN: return "WI_F32_COPYSIGN";
		case WI_F64_ABS: return "WI_F64_ABS";
		case WI_F64_NEG: return "WI_F64_NEG";
		case WI_F64_CEIL: return "WI_F64_CEIL";
		case WI_F64_FLOOR: return "WI_F64_FLOOR";
		case WI_F64_TRUNC: return "WI_F64_TRUNC";
		case WI_F64_NEAREST: return "WI_F64_NEAREST";
		case WI_F64_SQRT: return "WI_F64_SQRT";
		case WI_F64_ADD: return "WI_F64_ADD";
		case WI_F64_SUB: return "WI_F64_SUB";
		case WI_F64_MUL: return "WI_F64_MUL";
		case WI_F64_DIV: return "WI_F64_DIV";
		case WI_F64_MIN: return "WI_F64_MIN";
		case WI_F64_MAX: return "WI_F64_MAX";
		case WI_F64_COPYSIGN: return "WI_F64_COPYSIGN";
		case WI_I32_FROM_I64: return "WI_I32_FROM_I64";
		case WI_I32_FROM_F32_S: return "WI_I32_FROM_F32_S";
		case WI_I32_FROM_F32_U: return "WI_I32_FROM_F32_U";
		case WI_I32_FROM_F64_S: return "WI_I32_FROM_F64_S";
		case WI_I32_FROM_F64_U: return "WI_I32_FROM_F64_U";
		case WI_I64_FROM_I32_S: return "WI_I64_FROM_I32_S";
		case WI_I64_FROM_I32_U: return "WI_I64_FROM_I32_U";
		case WI_I64_FROM_F32_S: return "WI_I64_FROM_F32_S";
		case WI_I64_FROM_F32_U: return "WI_I64_FROM_F32_U";
		case WI_I64_FROM_F64_S: return "WI_I64_FROM_F64_S";
		case WI_I64_FROM_F64_U: return "WI_I64_FROM_F64_U";
		case WI_F32_FROM_I32_S: return "WI_F32_FROM_I32_S";
		case WI_F32_FROM_I32_U: return "WI_F32_FROM_I32_U";
		case WI_F32_FROM_I64_S: return "WI_F32_FROM_I64_S";
		case WI_F32_FROM_I64_U: return "WI_F32_FROM_I64_U";
		case WI_F32_FROM_F64: return "WI_F32_FROM_F64";
		case WI_F64_FROM_I32_S: return "WI_F64_FROM_I32_S";
		case WI_F64_FROM_I32_U: return "WI_F64_FROM_I32_U";
		case WI_F64_FROM_I64_S: return "WI_F64_FROM_I64_S";
		case WI_F64_FROM_I64_U: return "WI_F64_FROM_I64_U";
		case WI_F64_FROM_F32: return "WI_F64_FROM_F32";
		case WI_I32_REINTERPRET_F32: return "WI_I32_REINTERPRET_F32";
		case WI_I64_REINTERPRET_F64: return "WI_I64_REINTERPRET_F64";
		case WI_F32_REINTERPRET_I32: return "WI_F32_REINTERPRET_I32";
		case WI_F64_REINTERPRET_I64: return "WI_F64_REINTERPRET_I64";
	}
}

static WasmType onyx_type_to_wasm_type(OnyxTypeInfo* type) {
	if (type->is_bool) return WASM_TYPE_INT32;
	else if (type->is_int) {
		if (type->size == 4) return WASM_TYPE_INT32;
		if (type->size == 8) return WASM_TYPE_INT64;
	}
	else if (type->is_float) {
		if (type->size == 4) return WASM_TYPE_FLOAT32;
		if (type->size == 8) return WASM_TYPE_FLOAT64;
	}

	// TODO: Should produce an error message if this isn't successful
	return WASM_TYPE_INT32;
}

static void process_block(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNodeBlock* block, b32 top_level);
static void process_statement(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* stmt);
static void process_assign_lval(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* lval);
static void process_assignment(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* assign);
static void process_expression(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* expr);
static void process_cast(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* cast);
static void process_return(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* ret);

static void process_block(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNodeBlock* block, b32 top_level) {
	if (!top_level) {
		bh_arr_push(func->code, ((WasmInstruction){ WI_BLOCK_START, 0x40 }));
	}

	forll (OnyxAstNode, stmt, block->body, next) {
		process_statement(mod, func, stmt);
	}

	if (!top_level) {
		bh_arr_push(func->code, ((WasmInstruction){ WI_BLOCK_END, 0x00 }));
	}
}

static void process_statement(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* stmt) {
	switch (stmt->kind) {
		case ONYX_AST_NODE_KIND_SCOPE: break;
		case ONYX_AST_NODE_KIND_RETURN: process_return(mod, func, stmt); break;
		case ONYX_AST_NODE_KIND_ASSIGNMENT: process_assignment(mod, func, stmt); break;
		default: process_expression(mod, func, stmt);
	}
}

static void process_assign_lval(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* lval) {
	if (lval->kind == ONYX_AST_NODE_KIND_LOCAL || lval->kind == ONYX_AST_NODE_KIND_PARAM) {
		onyx_token_null_toggle(*lval->token);
		i32 localidx = bh_hash_get(i32, mod->local_map, lval->token->token);
		onyx_token_null_toggle(*lval->token);

		bh_arr_push(func->code, ((WasmInstruction){ WI_LOCAL_SET, localidx }));
	} else {
		assert(("Invalid lval", 0));
	}
}

static void process_assignment(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* assign) {
	process_expression(mod, func, assign->right);
	process_assign_lval(mod, func, assign->left);
}

#define BIN_OP_PROCESS(ast_binop, wasm_binop) \
	case ONYX_AST_NODE_KIND_##ast_binop: \
		{ \
			WasmInstructionType instr_type; \
			switch (expr->type->kind) { \
				case ONYX_TYPE_INFO_KIND_INT32: instr_type = WI_I32_##wasm_binop;		break; \
				case ONYX_TYPE_INFO_KIND_INT64: instr_type = WI_I64_##wasm_binop;		break; \
				case ONYX_TYPE_INFO_KIND_FLOAT32: instr_type = WI_F32_##wasm_binop;		break; \
				case ONYX_TYPE_INFO_KIND_FLOAT64: instr_type = WI_F64_##wasm_binop;		break; \
				default: assert(("Invalid type", 0)); \
			} \
 \
			process_expression(mod, func, expr->left); \
			process_expression(mod, func, expr->right); \
			bh_arr_push(func->code, ((WasmInstruction){ instr_type, 0x00 })); \
			break; \
		}

static void process_expression(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* expr) {
	switch (expr->kind) {
		BIN_OP_PROCESS(ADD, ADD);
		BIN_OP_PROCESS(MINUS, SUB);
		BIN_OP_PROCESS(MULTIPLY, MUL);

		case ONYX_AST_NODE_KIND_DIVIDE:
			{
				WasmInstructionType instr_type;
				switch (expr->type->kind) {
					case ONYX_TYPE_INFO_KIND_INT32:
						if (expr->type->is_unsigned) instr_type = WI_I32_DIV_U;
						else instr_type = WI_I32_DIV_S;
						break;
					case ONYX_TYPE_INFO_KIND_INT64:
						if (expr->type->is_unsigned) instr_type = WI_I64_DIV_U;
						else instr_type = WI_I64_DIV_S;
						break;
					case ONYX_TYPE_INFO_KIND_FLOAT32: instr_type = WI_F32_DIV;		break;
					case ONYX_TYPE_INFO_KIND_FLOAT64: instr_type = WI_F64_DIV;		break;
					default: assert(("Invalid type", 0));
				}

				process_expression(mod, func, expr->left);
				process_expression(mod, func, expr->right);
				bh_arr_push(func->code, ((WasmInstruction){ instr_type, 0x00 }));
				break;
			}

		case ONYX_AST_NODE_KIND_MODULUS:
			{
				WasmInstructionType instr_type;
				switch (expr->type->kind) {
					case ONYX_TYPE_INFO_KIND_INT32:
						if (expr->type->is_unsigned) instr_type = WI_I32_REM_U;
						else instr_type = WI_I32_REM_S;
						break;
					case ONYX_TYPE_INFO_KIND_INT64:
						if (expr->type->is_unsigned) instr_type = WI_I64_REM_U;
						else instr_type = WI_I64_REM_S;
						break;
					default: assert(("Invalid type", 0));
				}

				process_expression(mod, func, expr->left);
				process_expression(mod, func, expr->right);
				bh_arr_push(func->code, ((WasmInstruction){ instr_type, 0x00 }));
				break;
			}

		case ONYX_AST_NODE_KIND_LOCAL:
		case ONYX_AST_NODE_KIND_PARAM:
			{
				onyx_token_null_toggle(*expr->token);
				i32 localidx = bh_hash_get(i32, mod->local_map, expr->token->token);
				onyx_token_null_toggle(*expr->token);

				bh_arr_push(func->code, ((WasmInstruction){ WI_LOCAL_GET, localidx }));
				break;
			}

		case ONYX_AST_NODE_KIND_CAST: process_cast(mod, func, expr); break;
		case ONYX_AST_NODE_KIND_LITERAL:
			{
				// TODO: Implement proper literal type detection and parsing
				i64 value = 0;

				bh_arr_push(func->code, ((WasmInstruction){ WI_I64_CONST, value }));
				break;
			}

		case ONYX_AST_NODE_KIND_BLOCK: process_block(mod, func, (OnyxAstNodeBlock *) expr, 0); break;

		default:
			bh_printf("Unhandled case: %d\n", expr->kind);
			assert(0);
	}
}

static const WasmInstructionType cast_map[][6] = {
	// 			I32					U32					I64					U64					F32					F64
	/* I32 */ {	WI_NOP, 			WI_NOP,				WI_I64_FROM_I32_S, 	WI_I64_FROM_I32_S,	WI_F32_FROM_I32_S,	WI_F64_FROM_I32_S },
	/* U32 */ {	WI_NOP, 			WI_NOP,				WI_I64_FROM_I32_U, 	WI_I64_FROM_I32_U,	WI_F32_FROM_I32_U,	WI_F64_FROM_I32_U },
	/* I64 */ { WI_I32_FROM_I64,	WI_I32_FROM_I64,	WI_NOP,				WI_NOP,				WI_F32_FROM_I64_S,	WI_F64_FROM_I64_S },
	/* U64 */ { WI_I32_FROM_I64,	WI_I32_FROM_I64,	WI_NOP,				WI_NOP,				WI_F32_FROM_I64_U,	WI_F64_FROM_I64_U },
	/* F32 */ { WI_I32_FROM_F32_S,	WI_I32_FROM_F32_U,	WI_I64_FROM_F32_S,	WI_I64_FROM_F32_U,	WI_NOP,				WI_F64_FROM_F32	  },
	/* F64 */ { WI_I32_FROM_F64_S,	WI_I32_FROM_F64_U,	WI_I64_FROM_F64_S,	WI_I64_FROM_F64_U,	WI_F32_FROM_F64,	WI_NOP,			  },
};

static void process_cast(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* cast) {
	process_expression(mod, func, cast->left);

	OnyxTypeInfo* from = cast->left->type;
	OnyxTypeInfo* to = cast->type;

	i32 fromidx = 0, toidx = 0;
	if (from->is_int) {
		if (from->size == 4 && !from->is_unsigned) fromidx = 0;
		else if (from->size == 4 && from->is_unsigned) fromidx = 1;
		else if (from->size == 8 && !from->is_unsigned) fromidx = 2;
		else if (from->size == 8 && from->is_unsigned) fromidx = 3;
	} else if (from->is_float) {
		if (from->size == 4) fromidx = 4;
		else if (from->size == 8) fromidx = 5;
	}

	if (to->is_int) {
		if (to->size == 4 && !to->is_unsigned) toidx = 0;
		else if (to->size == 4 && to->is_unsigned) toidx = 1;
		else if (to->size == 8 && !to->is_unsigned) toidx = 2;
		else if (to->size == 8 && to->is_unsigned) toidx = 3;
	} else if (to->is_float) {
		if (to->size == 4) toidx = 4;
		else if (to->size == 8) toidx = 5;
	}

	WasmInstructionType cast_op = cast_map[fromidx][toidx];
	if (cast_op != WI_NOP) {
		bh_arr_push(func->code, ((WasmInstruction){ cast_op, 0x00 }));
	}
}

static void process_return(OnyxWasmModule* mod, WasmFunc* func, OnyxAstNode* ret) {
	if (ret->left) {
		process_expression(mod, func, ret->left);
	}

	bh_arr_push(func->code, ((WasmInstruction){ WI_RETURN, 0x00 }));
}

static void process_function_definition(OnyxWasmModule* mod, OnyxAstNodeFuncDef* fd) {
	static char type_repr_buf[128];

	char* t = type_repr_buf;
	OnyxAstNodeParam* param = fd->params;
	i32 param_count = 0;
	while (param) {
		// HACK: Using these directly as part of a string feels weird but they are
		// valid characters so I don't think it is going to be much of an issue
		*(t++) = (char) onyx_type_to_wasm_type(param->type);
		param_count++;
		param = param->next;
	}
	*(t++) = ':';

	WasmType return_type = onyx_type_to_wasm_type(fd->return_type);
	*(t++) = (char) return_type;
	*t = '\0';

	i32 type_idx = 0;
	if (bh_hash_has(i32, mod->type_map, type_repr_buf)) {
		type_idx = bh_hash_get(i32, mod->type_map, type_repr_buf);
	} else {
		// NOTE: Make a new type
		// TODO: Ensure that this isn't going to break things because of alignment
		WasmFuncType* type = (WasmFuncType*) bh_alloc(mod->allocator, sizeof(WasmFuncType) + sizeof(WasmType) * param_count);
		type->return_type = return_type;
		type->param_count = param_count;

		// HACK ish thing
	 	memcpy(type->param_types, type_repr_buf, type->param_count);

		bh_arr_push(mod->functypes, type);

		bh_hash_put(i32, mod->type_map, type_repr_buf, mod->next_type_idx);
		type_idx = mod->next_type_idx;
		mod->next_type_idx++;
	}

	WasmFunc wasm_func = {
		.type_idx = type_idx,
		.code = NULL,
	};
	bh_arr_push(mod->funcs, wasm_func);
	i32 func_idx = mod->next_func_idx++;

	if (fd->flags & ONYX_AST_FLAG_EXPORTED) {
		onyx_token_null_toggle(*fd->token);

		WasmExport wasm_export = {
			.kind = WASM_EXPORT_FUNCTION,
			.idx = func_idx,
		};
		bh_hash_put(WasmExport, mod->exports, fd->token->token, wasm_export);

		onyx_token_null_toggle(*fd->token);
	}

	// If there is no body then don't process the code
	if (fd->body == NULL) return;

	// NOTE: Generate the local map
	i32 localidx = 0;
	forll (OnyxAstNodeParam, param, fd->params, next) {
		onyx_token_null_toggle(*param->token);
		bh_hash_put(i32, mod->local_map, param->token->token, localidx++);
		onyx_token_null_toggle(*param->token);
	}

	forll (OnyxAstNodeLocal, local, fd->body->scope->last_local, prev_local) {
		onyx_token_null_toggle(*local->token);
		bh_hash_put(i32, mod->local_map, local->token->token, localidx++);
		onyx_token_null_toggle(*local->token);
	}

	bh_printf("\nLocals for function: %b\n", fd->token->token, fd->token->length);
	bh_hash_each_start(i32, mod->local_map);
		bh_printf("\t%s -> %d\n", key, value);
	bh_hash_each_end;

	// Generate code
	process_block(mod, &wasm_func, fd->body, 1);

	bh_printf("Code for function:\n");
	bh_arr_each(WasmInstruction, instr, wasm_func.code) {
		bh_printf("\t%s\t%xd\n", wi_string(instr->type), instr->data.i1);
	}

	// NOTE: Clear the local map on exit of generating this function
	bh_hash_clear(mod->local_map);
}

OnyxWasmModule onyx_wasm_generate_module(bh_allocator alloc, OnyxAstNode* program) {
	OnyxWasmModule module = {
		.allocator = alloc,

		.type_map = NULL,
		.next_type_idx = 0,
		.functypes = NULL,

		.funcs = NULL,
		.next_func_idx = 0,

		.exports = NULL,
	};

	bh_arr_new(alloc, module.functypes, 4);
	bh_arr_new(alloc, module.funcs, 4);

	bh_hash_init(bh_heap_allocator(), module.local_map, 61);
	bh_hash_init(bh_heap_allocator(), module.type_map, 61);
	bh_hash_init(bh_heap_allocator(), module.exports, 61);

	OnyxAstNode* walker = program;
	while (walker) {
		switch (walker->kind) {
			case ONYX_AST_NODE_KIND_FUNCDEF:
				process_function_definition(&module, &walker->as_funcdef);
				break;
			default: break;
		}

		walker = walker->next;
	}

	return module;
}

void onyx_wasm_module_free(OnyxWasmModule* module) {
	bh_arr_free(module->functypes);
	bh_arr_free(module->funcs);
}