#include "onyxlex.h"
#include "onyxerrors.h"
#include "onyxparser.h"
#include "onyxutils.h"

// NOTE: The one weird define you need to know before read the code below
#define make_node(nclass, kind) onyx_ast_node_new(parser->allocator, sizeof(nclass), kind)
#define peek_token(ahead) (parser->curr + ahead)

#define STORE_PARSER_STATE \
    OnyxToken* __parser_curr = parser->curr; \
    OnyxToken* __parser_prev = parser->prev;

#define RESTORE_PARSER_STATE \
    parser->curr = __parser_curr; \
    parser->prev = __parser_prev;

static AstNode error_node = { Ast_Kind_Error, 0, NULL, NULL };

// NOTE: Forward declarations
static void consume_token(OnyxParser* parser);
static void unconsume_token(OnyxParser* parser);
static b32 is_terminating_token(TokenType token_type);
static OnyxToken* expect_token(OnyxParser* parser, TokenType token_type);

static AstNumLit*     parse_int_literal(OnyxParser* parser);
static AstNumLit*     parse_float_literal(OnyxParser* parser);
static b32            parse_possible_struct_literal(OnyxParser* parser, AstTyped* left, AstTyped** ret);
static b32            parse_possible_array_literal(OnyxParser* parser, AstTyped* left, AstTyped** ret);
static void           parse_arguments(OnyxParser* parser, TokenType end_token, Arguments* args);
static AstTyped*      parse_factor(OnyxParser* parser);
static AstTyped*      parse_compound_assignment(OnyxParser* parser, AstTyped* lhs);
static AstTyped*      parse_compound_expression(OnyxParser* parser, b32 assignment_allowed);
static AstTyped*      parse_expression(OnyxParser* parser, b32 assignment_allowed);
static AstIfWhile*    parse_if_stmt(OnyxParser* parser);
static AstIfWhile*    parse_while_stmt(OnyxParser* parser);
static AstFor*        parse_for_stmt(OnyxParser* parser);
static AstSwitch*     parse_switch_stmt(OnyxParser* parser);
static i32            parse_possible_symbol_declaration(OnyxParser* parser, AstNode** ret);
static AstReturn*     parse_return_stmt(OnyxParser* parser);
static AstNode*       parse_use_stmt(OnyxParser* parser);
static AstBlock*      parse_block(OnyxParser* parser);
static AstNode*       parse_statement(OnyxParser* parser);
static AstType*       parse_type(OnyxParser* parser);
static AstStructType* parse_struct(OnyxParser* parser);
static void           parse_function_params(OnyxParser* parser, AstFunction* func);
static b32            parse_possible_directive(OnyxParser* parser, const char* dir);
static AstFunction*   parse_function_definition(OnyxParser* parser);
static AstTyped*      parse_global_declaration(OnyxParser* parser);
static AstEnumType*   parse_enum_declaration(OnyxParser* parser);
static AstTyped*      parse_top_level_expression(OnyxParser* parser);
static AstBinding*    parse_top_level_binding(OnyxParser* parser, OnyxToken* symbol);
static AstNode*       parse_top_level_statement(OnyxParser* parser);
static AstPackage*    parse_package_name(OnyxParser* parser);

static void consume_token(OnyxParser* parser) {
    if (parser->hit_unexpected_token) return;

    parser->prev = parser->curr;
    parser->curr++;
    while (parser->curr->type == Token_Type_Comment) parser->curr++;
}

static void unconsume_token(OnyxParser* parser) {
    if (parser->hit_unexpected_token) return;

    // TODO: This is probably wrong
    while (parser->prev->type == Token_Type_Comment) parser->prev--;
    parser->curr = parser->prev;
    parser->prev--;
}

static b32 is_terminating_token(TokenType token_type) {
    return (token_type == ';')
        || (token_type == '}')
        || (token_type == '{')
        || (token_type == Token_Type_End_Stream);
}

static void find_token(OnyxParser* parser, TokenType token_type) {
    if (parser->hit_unexpected_token) return;

    while (parser->curr->type != token_type && !is_terminating_token(parser->curr->type)) {
        consume_token(parser);
    }
}

// Advances to next token no matter what
static OnyxToken* expect_token(OnyxParser* parser, TokenType token_type) {
    if (parser->hit_unexpected_token) return NULL;

    OnyxToken* token = parser->curr;
    consume_token(parser);

    if (token->type != token_type) {
        onyx_report_error(token->pos, "expected token '%s', got '%s'.", token_name(token_type), token_name(token->type));
        parser->hit_unexpected_token = 1;
        parser->curr = &parser->tokenizer->tokens[bh_arr_length(parser->tokenizer->tokens) - 1];
        return NULL;
    }

    return token;
}

static void add_node_to_process(OnyxParser* parser, AstNode* node) {
    Scope* scope = parser->file_scope;

    if (!bh_arr_is_empty(parser->block_stack)) {
        scope = parser->block_stack[bh_arr_length(parser->block_stack) - 1]->binding_scope;
        assert(scope != NULL);
    }

    bh_arr_push(parser->results.nodes_to_process, ((NodeToProcess) {
        .package = parser->package,
        .scope = scope,
        .node = node,
    }));
}


static AstNumLit* parse_int_literal(OnyxParser* parser) {
    AstNumLit* int_node = make_node(AstNumLit, Ast_Kind_NumLit);
    int_node->token = expect_token(parser, Token_Type_Literal_Integer);
    int_node->flags |= Ast_Flag_Comptime;
    int_node->value.l = 0ll;

    token_toggle_end(int_node->token);

    char* first_invalid = NULL;
    i64 value = strtoll(int_node->token->text, &first_invalid, 0);

    int_node->value.l = value;

    // NOTE: Hex literals are unsigned.
    if (int_node->token->length >= 2 && int_node->token->text[1] == 'x') {
        if ((u64) value >= ((u64) 1 << 32))
            int_node->type_node = (AstType *) &basic_type_u64;
        else
            int_node->type_node = (AstType *) &basic_type_u32;
    } else {
        int_node->type_node = (AstType *) &basic_type_int_unsized;
    }

    token_toggle_end(int_node->token);
    return int_node;
}

static AstNumLit* parse_float_literal(OnyxParser* parser) {
    AstNumLit* float_node = make_node(AstNumLit, Ast_Kind_NumLit);
    float_node->token = expect_token(parser, Token_Type_Literal_Float);
    float_node->flags |= Ast_Flag_Comptime;
    float_node->value.d = 0.0;

    AstType* type = (AstType *) &basic_type_float_unsized;
    token_toggle_end(float_node->token);

    if (float_node->token->text[float_node->token->length - 1] == 'f') {
        type = (AstType *) &basic_type_f32;
        float_node->value.f = strtof(float_node->token->text, NULL);
    } else {
        float_node->value.d = strtod(float_node->token->text, NULL);
    }

    float_node->type_node = type;

    token_toggle_end(float_node->token);
    return float_node;
}

// e
// '#' <symbol>
static b32 parse_possible_directive(OnyxParser* parser, const char* dir) {
    if (parser->curr->type != '#') return 0;

    expect_token(parser, '#');
    OnyxToken* sym = expect_token(parser, Token_Type_Symbol);

    b32 match = (strlen(dir) == (u64) sym->length) && (strncmp(dir, sym->text, sym->length) == 0);
    if (!match) {
        unconsume_token(parser);
        unconsume_token(parser);
    }
    return match;
}

static b32 parse_possible_struct_literal(OnyxParser* parser, AstTyped* left, AstTyped** ret) {
    if (parser->curr->type != '.'
        || peek_token(1)->type != '{') return 0;

    AstStructLiteral* sl = make_node(AstStructLiteral, Ast_Kind_Struct_Literal);
    sl->token = parser->curr;
    sl->stnode = left;

    arguments_initialize(&sl->args);

    expect_token(parser, '.');
    expect_token(parser, '{');

    parse_arguments(parser, '}', &sl->args);

    *ret = (AstTyped *) sl;
    return 1;
}

static b32 parse_possible_array_literal(OnyxParser* parser, AstTyped* left, AstTyped** ret) {
    if (parser->curr->type != '.'
        || peek_token(1)->type != '[') return 0;
    
    AstArrayLiteral* al = make_node(AstArrayLiteral, Ast_Kind_Array_Literal);
    al->token = parser->curr;
    al->atnode = left;

    bh_arr_new(global_heap_allocator, al->values, 4);
    fori (i, 0, 4) al->values[i] = NULL;

    expect_token(parser, '.');
    expect_token(parser, '[');
    while (parser->curr->type != ']') {
        AstTyped* value = parse_expression(parser, 0);
        bh_arr_push(al->values, value);

        if (parser->curr->type != ']')
            expect_token(parser, ',');
    }

    expect_token(parser, ']');

    *ret = (AstTyped *) al;

    return 1;
}

static void parse_arguments(OnyxParser* parser, TokenType end_token, Arguments* args) {
    while (parser->curr->type != end_token) {
        if (parser->hit_unexpected_token) return;

        if (peek_token(0)->type == Token_Type_Symbol && peek_token(1)->type == '=') {
            OnyxToken* name = expect_token(parser, Token_Type_Symbol);
            expect_token(parser, '=');

            AstNamedValue* named_value = make_node(AstNamedValue, Ast_Kind_Named_Value);
            named_value->token = name;
            named_value->value = parse_expression(parser, 0);

            bh_arr_push(args->named_values, named_value);

        } else {
            AstTyped* value = parse_expression(parser, 0);
            bh_arr_push(args->values, value);
        }

        if (parser->curr->type != end_token)
            expect_token(parser, ',');
    }

    expect_token(parser, end_token);
}

// ( <expr> )
// - <factor>
// ! <factor>
// <symbol> ( '(' <exprlist> ')' )?
// <numlit>
// 'true'
// 'false'
// All of these could be followed by a cast
static AstTyped* parse_factor(OnyxParser* parser) {
    AstTyped* retval = NULL;

    switch ((u16) parser->curr->type) {
        case '(': {
            consume_token(parser);
            AstTyped* expr = parse_compound_expression(parser, 0);
            expect_token(parser, ')');
            retval = expr;
            break;
        }

        case '-': {
            OnyxToken* negate_token = expect_token(parser, '-');
            AstTyped* factor = parse_factor(parser);

            AstUnaryOp* negate_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
            negate_node->operation = Unary_Op_Negate;
            negate_node->expr = factor;
            negate_node->token = negate_token;

            retval = (AstTyped *) negate_node;
            break;
        }

        case '!': {
            AstUnaryOp* not_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
            not_node->operation = Unary_Op_Not;
            not_node->token = expect_token(parser, '!');
            not_node->expr = parse_factor(parser);

            retval = (AstTyped *) not_node;
            break;
        }

        case '~': {
            AstUnaryOp* not_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
            not_node->operation = Unary_Op_Bitwise_Not;
            not_node->token = expect_token(parser, '~');
            not_node->expr = parse_factor(parser);

            retval = (AstTyped *) not_node;
            break;
        }

        case '*': {
            AstDereference* deref_node = make_node(AstDereference, Ast_Kind_Dereference);
            deref_node->token = expect_token(parser, '*');
            deref_node->expr  = parse_factor(parser);

            retval = (AstTyped *) deref_node;
            break;
        }

        case '^': {
            AstAddressOf* aof_node = make_node(AstAddressOf, Ast_Kind_Address_Of);
            aof_node->token = expect_token(parser, '^');
            aof_node->expr  = parse_factor(parser);

            retval = (AstTyped *) aof_node;
            break;
        }

        case Token_Type_Tilde_Tilde: {
            AstUnaryOp* ac_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
            ac_node->operation = Unary_Op_Auto_Cast;
            ac_node->token = expect_token(parser, Token_Type_Tilde_Tilde);
            ac_node->expr = parse_factor(parser);

            retval = (AstTyped *) ac_node;
            break;
        }

        case Token_Type_Keyword_Cast: {
            AstUnaryOp* cast_node = make_node(AstUnaryOp, Ast_Kind_Unary_Op);
            cast_node->token = expect_token(parser, Token_Type_Keyword_Cast);
            expect_token(parser, '(');
            cast_node->type_node = parse_type(parser);
            expect_token(parser, ')');
            cast_node->operation = Unary_Op_Cast;
            cast_node->expr = parse_factor(parser);

            retval = (AstTyped *) cast_node;
            break;
        }

        case Token_Type_Keyword_Sizeof: {
            AstSizeOf* so_node = make_node(AstSizeOf, Ast_Kind_Size_Of);
            so_node->token = expect_token(parser, Token_Type_Keyword_Sizeof);
            so_node->so_ast_type = (AstType *) parse_type(parser);
            so_node->type_node = (AstType *) &basic_type_i32;

            retval = (AstTyped *) so_node;
            break;
        }

        case Token_Type_Keyword_Alignof: {
            AstAlignOf* ao_node = make_node(AstAlignOf, Ast_Kind_Align_Of);
            ao_node->token = expect_token(parser, Token_Type_Keyword_Alignof);
            ao_node->ao_ast_type = (AstType *) parse_type(parser);
            ao_node->type_node = (AstType *) &basic_type_i32;

            retval = (AstTyped *) ao_node;
            break;
        }

        case Token_Type_Symbol: {
            OnyxToken* sym_token = expect_token(parser, Token_Type_Symbol);
            AstTyped* sym_node = make_node(AstTyped, Ast_Kind_Symbol);
            sym_node->token = sym_token;

            retval = sym_node;
            break;
        }

        case Token_Type_Literal_Integer:
            retval = (AstTyped *) parse_int_literal(parser);
            break;

        case Token_Type_Literal_Float:
            retval = (AstTyped *) parse_float_literal(parser);
            break;

        case Token_Type_Literal_String: {
            AstStrLit* str_node = make_node(AstStrLit, Ast_Kind_StrLit);
            str_node->token     = expect_token(parser, Token_Type_Literal_String);
            str_node->addr      = 0;
            str_node->flags    |= Ast_Flag_Comptime;

            add_node_to_process(parser, (AstNode *) str_node);

            retval = (AstTyped *) str_node;
            break;
        }

        case Token_Type_Literal_True: {
            AstNumLit* bool_node = make_node(AstNumLit, Ast_Kind_NumLit);
            bool_node->type_node = (AstType *) &basic_type_bool;
            bool_node->token = expect_token(parser, Token_Type_Literal_True);
            bool_node->value.i = 1;
            bool_node->flags |= Ast_Flag_Comptime;
            retval = (AstTyped *) bool_node;
            break;
        }

        case Token_Type_Literal_False: {
            AstNumLit* bool_node = make_node(AstNumLit, Ast_Kind_NumLit);
            bool_node->type_node = (AstType *) &basic_type_bool;
            bool_node->token = expect_token(parser, Token_Type_Literal_False);
            bool_node->value.i = 0;
            bool_node->flags |= Ast_Flag_Comptime;
            retval = (AstTyped *) bool_node;
            break;
        }

        case Token_Type_Keyword_Proc: {
            retval = (AstTyped *) parse_function_definition(parser);
            retval->flags |= Ast_Flag_Function_Used;

            // TODO: Maybe move this somewhere else?
            add_node_to_process(parser, (AstNode *) retval);

            break;
        }

        // :TypeValueInterchange
        case '<': {
            AstTypeAlias* alias = make_node(AstTypeAlias, Ast_Kind_Type_Alias);
            alias->token = expect_token(parser, '<');
            alias->to = parse_type(parser);
            expect_token(parser, '>');

            retval = (AstTyped *) alias;
            break;
        }

        case '#': {
            if (parse_possible_directive(parser, "file_contents")) {
                AstFileContents* fc = make_node(AstFileContents, Ast_Kind_File_Contents);
                fc->token = parser->prev - 1;
                fc->filename = expect_token(parser, Token_Type_Literal_String);
                fc->type = type_make_slice(parser->allocator, &basic_types[Basic_Kind_U8]);

                add_node_to_process(parser, (AstNode *) fc);

                retval = (AstTyped *) fc;
                break;
            }
            else if (parse_possible_directive(parser, "file")) {
                OnyxToken* dir_token = parser->curr - 2;

                OnyxToken* str_token = bh_alloc(parser->allocator, sizeof(OnyxToken));
                str_token->text  = bh_strdup(global_heap_allocator, (char *) dir_token->pos.filename);
                str_token->length = strlen(dir_token->pos.filename);
                str_token->pos = dir_token->pos;
                str_token->type = Token_Type_Literal_String;

                AstStrLit* filename = make_node(AstStrLit, Ast_Kind_StrLit);
                filename->token = str_token;
                filename->addr  = 0;

                add_node_to_process(parser, (AstNode *) filename);
                retval = (AstTyped *) filename;
                break;
            }
            else if (parse_possible_directive(parser, "line")) {
                OnyxToken* dir_token = parser->curr - 2;

                AstNumLit* line_num = make_int_literal(parser->allocator, dir_token->pos.line);
                retval = (AstTyped *) line_num;
                break;
            }
            else if (parse_possible_directive(parser, "column")) {
                OnyxToken* dir_token = parser->curr - 2;

                AstNumLit* col_num = make_int_literal(parser->allocator, dir_token->pos.column);
                retval = (AstTyped *) col_num;
                break;
            }
            else if (parse_possible_directive(parser, "char")) {
                AstNumLit* char_lit = make_node(AstNumLit, Ast_Kind_NumLit);
                char_lit->flags |= Ast_Flag_Comptime;
                char_lit->type_node = (AstType *) &basic_type_int_unsized;

                char_lit->token = expect_token(parser, Token_Type_Literal_String);

                if (char_lit->token->text[0] != '\\') {
                    char_lit->value.i = char_lit->token->text[0];
                } else {
                    switch (char_lit->token->text[1]) {
                        case '0': char_lit->value.i = '\0'; break;
                        case 'a': char_lit->value.i = '\a'; break;
                        case 'b': char_lit->value.i = '\b'; break;
                        case 'f': char_lit->value.i = '\f'; break;
                        case 'n': char_lit->value.i = '\n'; break;
                        case 't': char_lit->value.i = '\t'; break;
                        case 'r': char_lit->value.i = '\r'; break;
                        case 'v': char_lit->value.i = '\v'; break;
                        case 'e': char_lit->value.i = '\e'; break;
                        case '"': char_lit->value.i = '"'; break;
                        case 'x': {
                            // HACK: This whole way of doing this
                            u8 buf[3];
                            buf[0] = char_lit->token->text[2];
                            buf[1] = char_lit->token->text[3];
                            buf[2] = 0;
                            char_lit->value.i = strtol((const char *) buf, NULL, 16);
                            break;
                        }
                        default: {
                            onyx_report_error(char_lit->token->pos, "invalid escaped character");
                            break;
                        }
                    }
                }

                retval = (AstTyped *) char_lit;
                break;
            }
            else if (parse_possible_directive(parser, "type")) {
                AstTypeAlias* alias = make_node(AstTypeAlias, Ast_Kind_Type_Alias);
                alias->to = parse_type(parser);
                retval = (AstTyped *) alias;
                break;
            }
            else if (parse_possible_directive(parser, "solidify")) {
                AstDirectiveSolidify* solid = make_node(AstDirectiveSolidify, Ast_Kind_Directive_Solidify);
                solid->token = parser->curr - 1;

                solid->poly_proc = (AstPolyProc *) parse_factor(parser);

                solid->known_polyvars = NULL;
                bh_arr_new(global_heap_allocator, solid->known_polyvars, 2);

                expect_token(parser, '{');
                while (parser->curr->type != '}') {
                    if (parser->hit_unexpected_token) break;

                    AstNode* poly_var = make_node(AstNode, Ast_Kind_Symbol);
                    poly_var->token = expect_token(parser, Token_Type_Symbol);

                    expect_token(parser, '=');
                    AstType* poly_type = parse_type(parser);

                    bh_arr_push(solid->known_polyvars, ((AstPolySolution) {
                        .kind     = PSK_Undefined,
                        .poly_sym = poly_var,
                        .ast_type = poly_type,
                        .type     = NULL
                    }));

                    if (parser->curr->type != '}')
                        expect_token(parser, ',');
                }
                expect_token(parser, '}');

                retval = (AstTyped *) solid;
                break;
            }

            onyx_report_error(parser->curr->pos, "invalid directive in expression.");
            return NULL;
        }

        default:
            onyx_report_error(parser->curr->pos, "unexpected token '%s'.", token_name(parser->curr->type));
            return NULL;
    }

    while (1) {
        if (parser->hit_unexpected_token) return retval;

        switch ((u16) parser->curr->type) {
            case '[': {
                OnyxToken *open_bracket = expect_token(parser, '[');
                AstTyped *expr = parse_compound_expression(parser, 0);

                AstKind kind = Ast_Kind_Array_Access;

                AstArrayAccess *aa_node = make_node(AstArrayAccess, kind);
                aa_node->token = open_bracket;
                aa_node->addr = retval;
                aa_node->expr = expr;

                retval = (AstTyped *) aa_node;
                expect_token(parser, ']');
                break;
            }

            case '.': {
                if (parse_possible_struct_literal(parser, retval, &retval)) return retval;
                if (parse_possible_array_literal(parser, retval, &retval))  return retval;
                
                consume_token(parser);
                AstFieldAccess* field = make_node(AstFieldAccess, Ast_Kind_Field_Access);
                field->token = expect_token(parser, Token_Type_Symbol);
                field->expr  = retval;

                retval = (AstTyped *) field;
                break;
            }

            case '(': {
                AstCall* call_node = make_node(AstCall, Ast_Kind_Call);
                call_node->token = expect_token(parser, '(');
                call_node->callee = retval;

                arguments_initialize(&call_node->args);

                parse_arguments(parser, ')', &call_node->args);

                // Wrap expressions in AstArgument
                bh_arr_each(AstTyped *, arg, call_node->args.values)
                    *arg = (AstTyped *) make_argument(parser->allocator, *arg);

                bh_arr_each(AstNamedValue *, named_value, call_node->args.named_values)
                    (*named_value)->value = (AstTyped *) make_argument(parser->allocator, (AstTyped *) (*named_value)->value);

                retval = (AstTyped *) call_node;
                break;
            }

            default: goto factor_parsed;
        }
    }

factor_parsed:

    return retval;
}

static inline i32 get_precedence(BinaryOp kind) {
    switch (kind) {
        case Binary_Op_Assign:          return 1;
        case Binary_Op_Assign_Add:      return 1;
        case Binary_Op_Assign_Minus:    return 1;
        case Binary_Op_Assign_Multiply: return 1;
        case Binary_Op_Assign_Divide:   return 1;
        case Binary_Op_Assign_Modulus:  return 1;
        case Binary_Op_Assign_And:      return 1;
        case Binary_Op_Assign_Or:       return 1;
        case Binary_Op_Assign_Xor:      return 1;
        case Binary_Op_Assign_Shl:      return 1;
        case Binary_Op_Assign_Shr:      return 1;
        case Binary_Op_Assign_Sar:      return 1;

        case Binary_Op_Pipe:            return 2;
        case Binary_Op_Range:           return 2;

        case Binary_Op_Bool_And:        return 3;
        case Binary_Op_Bool_Or:         return 3;

        case Binary_Op_Equal:           return 4;
        case Binary_Op_Not_Equal:       return 4;

        case Binary_Op_Less_Equal:      return 5;
        case Binary_Op_Less:            return 5;
        case Binary_Op_Greater_Equal:   return 5;
        case Binary_Op_Greater:         return 5;

        case Binary_Op_And:             return 6;
        case Binary_Op_Or:              return 6;
        case Binary_Op_Xor:             return 6;
        case Binary_Op_Shl:             return 6;
        case Binary_Op_Shr:             return 6;
        case Binary_Op_Sar:             return 6;

        case Binary_Op_Add:             return 7;
        case Binary_Op_Minus:           return 7;

        case Binary_Op_Multiply:        return 8;
        case Binary_Op_Divide:          return 8;

        case Binary_Op_Modulus:         return 9;

        default:                        return -1;
    }
}

static BinaryOp binary_op_from_token_type(TokenType t) {
    switch (t) {
        case Token_Type_Equal_Equal:       return Binary_Op_Equal;
        case Token_Type_Not_Equal:         return Binary_Op_Not_Equal;
        case Token_Type_Less_Equal:        return Binary_Op_Less_Equal;
        case Token_Type_Greater_Equal:     return Binary_Op_Greater_Equal;
        case '<':                          return Binary_Op_Less;
        case '>':                          return Binary_Op_Greater;

        case '+':                          return Binary_Op_Add;
        case '-':                          return Binary_Op_Minus;
        case '*':                          return Binary_Op_Multiply;
        case '/':                          return Binary_Op_Divide;
        case '%':                          return Binary_Op_Modulus;

        case '&':                          return Binary_Op_And;
        case '|':                          return Binary_Op_Or;
        case '^':                          return Binary_Op_Xor;
        case Token_Type_Shift_Left:        return Binary_Op_Shl;
        case Token_Type_Shift_Right:       return Binary_Op_Shr;
        case Token_Type_Shift_Arith_Right: return Binary_Op_Sar;

        case Token_Type_And_And:           return Binary_Op_Bool_And;
        case Token_Type_Or_Or:             return Binary_Op_Bool_Or;

        case '=':                          return Binary_Op_Assign;
        case Token_Type_Plus_Equal:        return Binary_Op_Assign_Add;
        case Token_Type_Minus_Equal:       return Binary_Op_Assign_Minus;
        case Token_Type_Star_Equal:        return Binary_Op_Assign_Multiply;
        case Token_Type_Fslash_Equal:      return Binary_Op_Assign_Divide;
        case Token_Type_Percent_Equal:     return Binary_Op_Assign_Modulus;
        case Token_Type_And_Equal:         return Binary_Op_Assign_And;
        case Token_Type_Or_Equal:          return Binary_Op_Assign_Or;
        case Token_Type_Xor_Equal:         return Binary_Op_Assign_Xor;
        case Token_Type_Shl_Equal:         return Binary_Op_Assign_Shl;
        case Token_Type_Shr_Equal:         return Binary_Op_Assign_Shr;
        case Token_Type_Sar_Equal:         return Binary_Op_Assign_Sar;

        case Token_Type_Pipe:              return Binary_Op_Pipe;
        case Token_Type_Dot_Dot:           return Binary_Op_Range;
        default: return Binary_Op_Count;
    }
}

static AstTyped* parse_compound_assignment(OnyxParser* parser, AstTyped* lhs) {
    if (parser->curr->type != '=') return lhs;

    AstBinaryOp* assignment = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
    assignment->token = expect_token(parser, '=');
    assignment->operation = Binary_Op_Assign;
    assignment->left = lhs;
    assignment->right = parse_compound_expression(parser, 0);

    return (AstTyped *) assignment;
}

static AstTyped* parse_compound_expression(OnyxParser* parser, b32 assignment_allowed) {
    AstTyped* first = parse_expression(parser, assignment_allowed);

    if (parser->curr->type == ',') {
        AstCompound* compound = make_node(AstCompound, Ast_Kind_Compound);
        compound->token = parser->curr;

        bh_arr_new(global_heap_allocator, compound->exprs, 2);
        bh_arr_push(compound->exprs, first);

        while (parser->curr->type == ',') {
            if (parser->hit_unexpected_token) return (AstTyped *) compound;
            consume_token(parser);

            AstTyped* expr = parse_expression(parser, 0);
            bh_arr_push(compound->exprs, expr);

            if (assignment_allowed && parser->curr->type == '=') {
                return parse_compound_assignment(parser, (AstTyped *) compound);
            }
        }

        return (AstTyped *) compound;

    } else {
        return first;
    }
}

// <expr> +  <expr>
// <expr> -  <expr>
// <expr> *  <expr>
// <expr> /  <expr>
// <expr> %  <expr>
// <expr> == <expr>
// <expr> != <expr>
// <expr> <= <expr>
// <expr> >= <expr>
// <expr> <  <expr>
// <expr> >  <expr>
// <expr> =  <expr>
// <expr> += <expr>
// <expr> -= <expr>
// <expr> *= <expr>
// <expr> /= <expr>
// <expr> %= <expr>
// <factor>
// With expected precedence rules
static AstTyped* parse_expression(OnyxParser* parser, b32 assignment_allowed) {
    bh_arr(AstBinaryOp*) tree_stack = NULL;
    bh_arr_new(global_heap_allocator, tree_stack, 4);
    bh_arr_set_length(tree_stack, 0);

    AstTyped* left = parse_factor(parser);
    AstTyped* right;
    AstTyped* root = left;

    BinaryOp bin_op_kind;
    OnyxToken* bin_op_tok;

    while (1) {
        if (parser->hit_unexpected_token) return root;

        bin_op_kind = binary_op_from_token_type(parser->curr->type);
        if (bin_op_kind == Binary_Op_Count) goto expression_done;
        if (binop_is_assignment(bin_op_kind) && !assignment_allowed) goto expression_done;

        bin_op_tok = parser->curr;
        consume_token(parser);

        AstBinaryOp* bin_op;
        if (bin_op_kind == Binary_Op_Pipe) {
            bin_op = make_node(AstBinaryOp, Ast_Kind_Pipe);

        } else if (bin_op_kind == Binary_Op_Range) {
            bin_op = (AstBinaryOp *) make_node(AstRangeLiteral, Ast_Kind_Range_Literal);

        } else {
            bin_op = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
        }

        bin_op->token = bin_op_tok;
        bin_op->operation = bin_op_kind;

        while ( !bh_arr_is_empty(tree_stack) &&
                get_precedence(bh_arr_last(tree_stack)->operation) >= get_precedence(bin_op_kind))
            bh_arr_pop(tree_stack);

        if (bh_arr_is_empty(tree_stack)) {
            // NOTE: new is now the root node
            bin_op->left = root;
            root = (AstTyped *) bin_op;
        } else {
            bin_op->left = bh_arr_last(tree_stack)->right;
            bh_arr_last(tree_stack)->right = (AstTyped *) bin_op;
        }

        bh_arr_push(tree_stack, bin_op);

        right = parse_factor(parser);
        bin_op->right = right;
    }

    bh_arr_free(tree_stack);

expression_done:
    return root;
}

// 'if' <expr> <stmt> ('elseif' <cond> <stmt>)* ('else' <block>)?
static AstIfWhile* parse_if_stmt(OnyxParser* parser) {
    AstIfWhile* if_node = make_node(AstIfWhile, Ast_Kind_If);
    if_node->token = expect_token(parser, Token_Type_Keyword_If);

    AstIfWhile* root_if = if_node;

    if (peek_token(1)->type == ':') {
        OnyxToken* local_sym = expect_token(parser, Token_Type_Symbol);
        if_node->local = make_local(parser->allocator, local_sym, NULL);

        expect_token(parser, ':');

        AstBinaryOp* assignment = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
        assignment->operation = Binary_Op_Assign;
        assignment->token = expect_token(parser, '=');
        assignment->left = (AstTyped *) if_node->local;
        assignment->right = parse_expression(parser, 0);

        if_node->assignment = assignment;
        expect_token(parser, ';');
    }

    AstTyped* cond = parse_expression(parser, 1);
    AstBlock* true_stmt = parse_block(parser);

    if_node->cond = cond;
    if (true_stmt != NULL)
        if_node->true_stmt = true_stmt;

    while (parser->curr->type == Token_Type_Keyword_Elseif) {
        if (parser->hit_unexpected_token) return root_if;

        consume_token(parser);
        AstIfWhile* elseif_node = make_node(AstIfWhile, Ast_Kind_If);

        cond = parse_expression(parser, 1);
        true_stmt = parse_block(parser);

        elseif_node->cond = cond;
        if (true_stmt != NULL)
            elseif_node->true_stmt = true_stmt;

        if_node->false_stmt = (AstBlock *) elseif_node;
        if_node = elseif_node;
    }

    if (parser->curr->type == Token_Type_Keyword_Else) {
        consume_token(parser);

        AstBlock* false_stmt = parse_block(parser);
        if (false_stmt != NULL)
            if_node->false_stmt = false_stmt;
    }

    return root_if;
}

// 'while' <expr> <block>
static AstIfWhile* parse_while_stmt(OnyxParser* parser) {
    OnyxToken* while_token = expect_token(parser, Token_Type_Keyword_While);
    AstIfWhile* while_node = make_node(AstIfWhile, Ast_Kind_While);
    while_node->token = while_token;

    if (peek_token(1)->type == ':') {
        OnyxToken* local_sym = expect_token(parser, Token_Type_Symbol);
        while_node->local = make_local(parser->allocator, local_sym, NULL);

        expect_token(parser, ':');

        AstBinaryOp* assignment = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
        assignment->operation = Binary_Op_Assign;
        assignment->token = expect_token(parser, '=');
        assignment->left = (AstTyped *) while_node->local;
        assignment->right = parse_expression(parser, 0);

        while_node->assignment = assignment;
        expect_token(parser, ';');
    }

    while_node->cond = parse_expression(parser, 1);
    while_node->true_stmt = parse_block(parser);

    if (parser->curr->type == Token_Type_Keyword_Else) {
        consume_token(parser);
        while_node->false_stmt = parse_block(parser);
    }

    return while_node;
}

static AstFor* parse_for_stmt(OnyxParser* parser) {
    AstFor* for_node = make_node(AstFor, Ast_Kind_For);
    for_node->token = expect_token(parser, Token_Type_Keyword_For);

    if (parser->curr->type == '^') {
        consume_token(parser);
        for_node->by_pointer = 1;
    }

    OnyxToken* local_sym = expect_token(parser, Token_Type_Symbol);
    AstLocal* var_node = make_local(parser->allocator, local_sym, NULL);

    for_node->var = var_node;

    expect_token(parser, ':');
    for_node->iter = parse_expression(parser, 1);
    for_node->stmt = parse_block(parser);

    return for_node;
}

static AstSwitch* parse_switch_stmt(OnyxParser* parser) {
    AstSwitch* switch_node = make_node(AstSwitch, Ast_Kind_Switch);
    switch_node->token = expect_token(parser, Token_Type_Keyword_Switch);

    bh_arr_new(global_heap_allocator, switch_node->cases, 4);

    if (peek_token(1)->type == ':') {
        OnyxToken* local_sym = expect_token(parser, Token_Type_Symbol);
        switch_node->local = make_local(parser->allocator, local_sym, NULL);

        expect_token(parser, ':');

        AstBinaryOp* assignment = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
        assignment->operation = Binary_Op_Assign;
        assignment->token = expect_token(parser, '=');
        assignment->left = (AstTyped *) switch_node->local;
        assignment->right = parse_expression(parser, 0);

        switch_node->assignment = assignment;
        expect_token(parser, ';');
    }

    switch_node->expr = parse_expression(parser, 1);
    expect_token(parser, '{');

    while (parser->curr->type == Token_Type_Keyword_Case) {
        bh_arr(AstTyped *) case_values = NULL;
        bh_arr_new(global_heap_allocator, case_values, 1);

        expect_token(parser, Token_Type_Keyword_Case);
        if (parser->hit_unexpected_token) return switch_node;

        if (parse_possible_directive(parser, "default")) {
            switch_node->default_case = parse_block(parser);

            if (parser->curr->type != '}') {
                onyx_report_error(parser->curr->pos, "The #default case must be the last case in a switch statement.\n");
            }
            break;
        }

        AstTyped* value = parse_expression(parser, 1);
        bh_arr_push(case_values, value);
        while (parser->curr->type == ',') {
            if (parser->hit_unexpected_token) return switch_node;

            consume_token(parser);
            value = parse_expression(parser, 1);
            bh_arr_push(case_values, value);
        }

        AstBlock* block = parse_block(parser);

        AstSwitchCase sc_node;
        sc_node.block  = block;
        sc_node.values = case_values;

        bh_arr_push(switch_node->cases, sc_node);
    }

    expect_token(parser, '}');
    return switch_node;
}

static i32 parse_possible_compound_symbol_declaration(OnyxParser* parser, AstNode** ret) {
    u32 token_offset = 0;
    while (peek_token(token_offset)->type == Token_Type_Symbol) {
        token_offset += 1;

        if (peek_token(token_offset)->type != ',') break;
        token_offset += 1;
    }

    if (peek_token(token_offset)->type != ':') return 0;

    // At this point, we are sure it is a compound declaration.
    AstCompound* local_compound = make_node(AstCompound, Ast_Kind_Compound);
    bh_arr_new(global_heap_allocator, local_compound->exprs, token_offset / 2);

    AstLocal* first_local = NULL;
    AstLocal* prev_local  = NULL;

    while (parser->curr->type == Token_Type_Symbol) {
        if (parser->hit_unexpected_token) return 1;

        OnyxToken* local_sym = expect_token(parser, Token_Type_Symbol);
        AstLocal* new_local = make_local(parser->allocator, local_sym, NULL);

        if (prev_local == NULL) {
            first_local = new_local;
        } else {
            prev_local->next = (AstNode *) new_local;
        }
        prev_local = new_local;

        AstNode* sym_node = make_symbol(parser->allocator, local_sym);
        bh_arr_push(local_compound->exprs, (AstTyped *) sym_node);

        if (parser->curr->type == ',')
            expect_token(parser, ',');
    }

    expect_token(parser, ':');

    if (parser->curr->type == '=') {
        AstBinaryOp* assignment = make_binary_op(parser->allocator, Binary_Op_Assign, (AstTyped *) local_compound, NULL);
        assignment->token = expect_token(parser, '=');
        assignment->right = parse_compound_expression(parser, 0);

        prev_local->next = (AstNode *) assignment;

    } else {
        AstType* type_for_all = parse_type(parser);
        forll (AstLocal, local, first_local, next) {
            local->type_node = type_for_all;
        }
    }

    *ret = (AstNode *) first_local;
    return 1;
}

// Returns:
//     0 - if this was not a symbol declaration.
//     1 - if this was a local declaration.
//     2 - if this was binding declaration.
// ret is set to the statement to insert
// <symbol> : <type> = <expr>
// <symbol> : <type> : <expr>
// <symbol> := <expr>
// <symbol> :: <expr>
// <symbol> (, <symbol>)* : <type>
// <symbol> (, <symbol>)* := <expr>
static i32 parse_possible_symbol_declaration(OnyxParser* parser, AstNode** ret) {
    // Has to start with a symbol to be a declaration
    if (parser->curr->type != Token_Type_Symbol) return 0;

    // If the token after the symbol is a comma, assume this is a compound declaration.
    if (peek_token(1)->type == ',') {
        return parse_possible_compound_symbol_declaration(parser, ret);
    }

    if (peek_token(1)->type != ':')         return 0;

    OnyxToken* symbol = expect_token(parser, Token_Type_Symbol);
    expect_token(parser, ':');

    if (parser->curr->type == ':') {
        AstBlock* current_block = parser->block_stack[bh_arr_length(parser->block_stack) - 1];
        assert(current_block->binding_scope != NULL);

        AstBinding* binding = parse_top_level_binding(parser, symbol);
        if (parser->hit_unexpected_token) return 2;

        symbol_introduce(current_block->binding_scope, symbol, binding->node);
        return 2;
    }

    // NOTE: var: type
    AstType* type_node = NULL;
    if (parser->curr->type != '=') {
        type_node = parse_type(parser);
    }

    AstLocal* local = make_local(parser->allocator, symbol, type_node);
    *ret = (AstNode *) local;

    if (parser->curr->type == '=') {
        AstBinaryOp* assignment = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
        assignment->operation = Binary_Op_Assign;
        local->next = (AstNode *) assignment;
        assignment->token = parser->curr;
        consume_token(parser);

        AstTyped* expr = parse_expression(parser, 1);
        if (expr == NULL) return 1;
        assignment->right = expr;

        AstNode* left_symbol = make_node(AstNode, Ast_Kind_Symbol);
        left_symbol->token = symbol;
        assignment->left = (AstTyped *) left_symbol;
    }

    return 1;
}

// 'return' <expr>?
static AstReturn* parse_return_stmt(OnyxParser* parser) {
    AstReturn* return_node = make_node(AstReturn, Ast_Kind_Return);
    return_node->token = expect_token(parser, Token_Type_Keyword_Return);

    AstTyped* expr = NULL;

    if (parser->curr->type != ';') {
        expr = parse_compound_expression(parser, 0);

        if (expr == NULL || expr == (AstTyped *) &error_node) {
            return (AstReturn *) &error_node;
        } else {
            return_node->expr = expr;
        }
    }

    return return_node;
}

static AstNode* parse_use_stmt(OnyxParser* parser) {
    OnyxToken* use_token = expect_token(parser, Token_Type_Keyword_Use);

    if (parser->curr->type == Token_Type_Keyword_Package) {
        expect_token(parser, Token_Type_Keyword_Package);

        AstUsePackage* upack = make_node(AstUsePackage, Ast_Kind_Use_Package);
        upack->token = use_token;

        AstNode* pack_symbol = make_node(AstNode, Ast_Kind_Symbol);
        pack_symbol->token = expect_token(parser, Token_Type_Symbol);

        // CLEANUP: This is just gross.
        if (parser->curr->type == '.') {
            consume_token(parser);
            pack_symbol->token->length += 1;

            while (1) {
                if (parser->hit_unexpected_token) break;

                OnyxToken* symbol = expect_token(parser, Token_Type_Symbol);
                pack_symbol->token->length += symbol->length;

                if (parser->curr->type == '.') {
                    pack_symbol->token->length += 1;
                    consume_token(parser);
                } else {
                    break;
                }
            }
        }

        upack->package = (AstPackage *) pack_symbol;

        if (parser->curr->type == Token_Type_Keyword_As) {
            consume_token(parser);
            upack->alias = expect_token(parser, Token_Type_Symbol);
        }

        if (parser->curr->type == '{') {
            consume_token(parser);

            bh_arr_new(global_heap_allocator, upack->only, 4);

            while (parser->curr->type != '}') {
                if (parser->hit_unexpected_token) return NULL;

                AstAlias* alias = make_node(AstAlias, Ast_Kind_Alias);
                alias->token = expect_token(parser, Token_Type_Symbol);

                if (parser->curr->type == Token_Type_Keyword_As) {
                    consume_token(parser);
                    alias->alias = expect_token(parser, Token_Type_Symbol);
                } else {
                    alias->alias = alias->token;
                }

                bh_arr_push(upack->only, alias);

                if (parser->curr->type != '}')
                    expect_token(parser, ',');
            }

            consume_token(parser);
        }

        return (AstNode *) upack;

    } else {
        AstUse* use_node = make_node(AstUse, Ast_Kind_Use);
        use_node->token = use_token;
        use_node->expr = parse_expression(parser, 1);
        return (AstNode *) use_node;
    }
}

// <return> ;
// <block>
// <symbol_statement> ;
// <expr> ;
// <if>
// <while>
// 'break' ;
// 'continue' ;
static AstNode* parse_statement(OnyxParser* parser) {
    b32 needs_semicolon = 1;
    AstNode* retval = NULL;

    switch ((u16) parser->curr->type) {
        case Token_Type_Keyword_Return:
            retval = (AstNode *) parse_return_stmt(parser);
            break;

        case '{':
        case Token_Type_Empty_Block:
        case Token_Type_Keyword_Do:
            needs_semicolon = 0;
            retval = (AstNode *) parse_block(parser);
            break;

        case Token_Type_Symbol: {
            i32 symbol_res = parse_possible_symbol_declaration(parser, &retval);
            if (symbol_res == 2) needs_semicolon = 0;
            if (symbol_res != 0) break;
            
            // fallthrough
        }

        case '(':
        case '+':
        case '-':
        case '!':
        case '*':
        case '^':
        case Token_Type_Literal_Integer:
        case Token_Type_Literal_Float:
        case Token_Type_Literal_String:
            retval = (AstNode *) parse_compound_expression(parser, 1);
            break;

        case Token_Type_Keyword_If:
            needs_semicolon = 0;
            retval = (AstNode *) parse_if_stmt(parser);
            break;

        case Token_Type_Keyword_While:
            needs_semicolon = 0;
            retval = (AstNode *) parse_while_stmt(parser);
            break;

        case Token_Type_Keyword_For:
            needs_semicolon = 0;
            retval = (AstNode *) parse_for_stmt(parser);
            break;

        case Token_Type_Keyword_Switch:
            needs_semicolon = 0;
            retval = (AstNode *) parse_switch_stmt(parser);
            break;

        case Token_Type_Keyword_Break: {
            AstJump* bnode = make_node(AstJump, Ast_Kind_Jump);
            bnode->token = expect_token(parser, Token_Type_Keyword_Break);
            bnode->jump  = Jump_Type_Break;

            u64 count = 1;
            while (parser->curr->type == Token_Type_Keyword_Break) {
                consume_token(parser);
                count++;
            }
            bnode->count = count;

            retval = (AstNode *) bnode;
            break;
        }

        case Token_Type_Keyword_Continue: {
            AstJump* cnode = make_node(AstJump, Ast_Kind_Jump);
            cnode->token = expect_token(parser, Token_Type_Keyword_Continue);
            cnode->jump  = Jump_Type_Continue;

            u64 count = 1;
            while (parser->curr->type == Token_Type_Keyword_Continue) {
                consume_token(parser);
                count++;
            }
            cnode->count = count;

            retval = (AstNode *) cnode;
            break;
        }

        case Token_Type_Keyword_Fallthrough: {
            AstJump* cnode = make_node(AstJump, Ast_Kind_Jump);
            cnode->token = expect_token(parser, Token_Type_Keyword_Fallthrough);
            cnode->jump  = Jump_Type_Fallthrough;

            u64 count = 1;
            while (parser->curr->type == Token_Type_Keyword_Fallthrough) {
                consume_token(parser);
                count++;
            }
            cnode->count = count;

            retval = (AstNode *) cnode;
            break;
        }

        case Token_Type_Keyword_Defer: {
            needs_semicolon = 0;

            AstDefer* defer = make_node(AstDefer, Ast_Kind_Defer);
            defer->token = expect_token(parser, Token_Type_Keyword_Defer);
            defer->stmt  = parse_statement(parser);

            retval = (AstNode *) defer;
            break;
        }

        case Token_Type_Keyword_Use: {
            needs_semicolon = 0;

            retval = (AstNode *) parse_use_stmt(parser);
            break;
        }

        case '#': {
            if (parse_possible_directive(parser, "context_scope")) {
                AstLocal* context_tmp = make_local(parser->allocator, NULL, builtin_context_variable->type_node);

                AstBinaryOp* assignment = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
                assignment->operation = Binary_Op_Assign;
                assignment->left = (AstTyped *) context_tmp;
                assignment->right = builtin_context_variable;
                context_tmp->next = (AstNode *) assignment;

                AstBinaryOp* assignment2 = make_node(AstBinaryOp, Ast_Kind_Binary_Op);
                assignment2->operation = Binary_Op_Assign;
                assignment2->left = builtin_context_variable;
                assignment2->right = (AstTyped *) context_tmp;

                AstDefer* defer_node = make_node(AstDefer, Ast_Kind_Defer);
                defer_node->stmt = (AstNode *) assignment2;
                assignment->next = (AstNode *) defer_node;

                AstBlock* context_block = parse_block(parser);
                needs_semicolon = 0;
                defer_node->next = (AstNode *) context_block;

                retval = (AstNode *) context_tmp;
                break;
            }
        }

        default:
            break;
    }

    if (needs_semicolon) expect_token(parser, ';');

    return retval;
}

// '---'
// '{' <stmtlist> '}'
static AstBlock* parse_block(OnyxParser* parser) {
    AstBlock* block = make_node(AstBlock, Ast_Kind_Block);
    bh_arr_new(global_heap_allocator, block->allocate_exprs, 4);

    {
        Scope* parent_scope = parser->file_scope;
        if (!bh_arr_is_empty(parser->block_stack))
            parent_scope = bh_arr_last(parser->block_stack)->binding_scope;
        block->binding_scope = scope_create(parser->allocator, parent_scope, parser->curr->pos);
    }

    bh_arr_push(parser->block_stack, block);

    // NOTE: --- is for an empty block
    if (parser->curr->type == Token_Type_Empty_Block) {
        block->token = expect_token(parser, Token_Type_Empty_Block);
        bh_arr_pop(parser->block_stack);
        return block;
    }

    if (parser->curr->type == Token_Type_Keyword_Do) {
        block->token = expect_token(parser, Token_Type_Keyword_Do);
        block->body = parse_statement(parser);
        bh_arr_pop(parser->block_stack);
        return block;
    }

    if (parser->curr->type != '{') {
        expect_token(parser, '{');
        find_token(parser, '}');
        bh_arr_pop(parser->block_stack);
        return block;
    }
    block->token = expect_token(parser, '{');

    AstNode** next = &block->body;
    AstNode* stmt = NULL;
    while (parser->curr->type != '}') {
        if (parser->hit_unexpected_token) {
            bh_arr_pop(parser->block_stack);
            return block;
        }

        stmt = parse_statement(parser);

        if (stmt != NULL && stmt->kind != Ast_Kind_Error) {
            *next = stmt;

            while (stmt->next != NULL) stmt = stmt->next;
            next = &stmt->next;
        }
    }

    expect_token(parser, '}');

    bh_arr_pop(parser->block_stack);
    return block;
}

static void parse_polymorphic_variable(OnyxParser* parser, AstType*** next_insertion) {
    bh_arr(AstPolyParam) pv = NULL;

    if (parser->polymorph_context.poly_params == NULL)
        onyx_report_error(parser->curr->pos, "Polymorphic variable not valid here.");
    else
        pv = *parser->polymorph_context.poly_params;

    consume_token(parser);

    AstNode* symbol_node = make_node(AstNode, Ast_Kind_Symbol);
    symbol_node->token = expect_token(parser, Token_Type_Symbol);

    **next_insertion = (AstType *) symbol_node;
    *next_insertion = NULL;

    if (pv != NULL) {
        bh_arr_push(pv, ((AstPolyParam) {
            .kind     = PPK_Poly_Type,
            .poly_sym = symbol_node,

            // These will be filled out by function_params()
            .type_expr = NULL,
            .idx = -1,
        }));

        *parser->polymorph_context.poly_params = pv;
    }
}

static AstType* parse_compound_type(OnyxParser* parser) {
    AstType* first = parse_type(parser);

    if (parser->curr->type == ',') {
        AstCompoundType* ctype = make_node(AstCompoundType, Ast_Kind_Type_Compound);
        ctype->token = parser->curr;

        bh_arr_new(global_heap_allocator, ctype->types, 2);
        bh_arr_push(ctype->types, first);

        while (parser->curr->type == ',') {
            if (parser->hit_unexpected_token) return (AstType *) ctype;
            consume_token(parser);

            bh_arr_push(ctype->types, parse_type(parser));
        }

        return (AstType *) ctype;

    } else {
        return first;
    }
}

// <symbol>
// '^' <type>
static AstType* parse_type(OnyxParser* parser) {
    AstType* root = NULL;
    AstType** next_insertion = &root;

    while (1) {
        if (parser->hit_unexpected_token) return root;

        switch (parser->curr->type) {
            case '^': {
                AstPointerType* new = make_node(AstPointerType, Ast_Kind_Pointer_Type);
                new->flags |= Basic_Flag_Pointer;
                new->token = expect_token(parser, '^');

                *next_insertion = (AstType *) new;
                next_insertion = &new->elem;
                break;
            }

            case '[': {
                AstType *new;
                OnyxToken *open_bracket = expect_token(parser, '[');

                if (parser->curr->type == ']') {
                    new = make_node(AstSliceType, Ast_Kind_Slice_Type);
                    new->token = open_bracket;

                } else if (parser->curr->type == Token_Type_Dot_Dot) {
                    new = make_node(AstDynArrType, Ast_Kind_DynArr_Type);
                    new->token = open_bracket;
                    consume_token(parser);

                } else {
                    new = make_node(AstArrayType, Ast_Kind_Array_Type);
                    new->token = open_bracket;

                    if (parser->curr->type == '$') {
                        AstType** insertion = (AstType **) &((AstArrayType *) new)->count_expr;
                        parse_polymorphic_variable(parser, &insertion);
                    } else {
                        ((AstArrayType *) new)->count_expr = parse_expression(parser, 0);
                    }
                }

                expect_token(parser, ']');
                *next_insertion = (AstType *) new;
                next_insertion = &((AstSliceType *) new)->elem;
                break;
            }

            case Token_Type_Keyword_Proc: {
                OnyxToken* proc_token = expect_token(parser, Token_Type_Keyword_Proc);

                bh_arr(AstType *) params = NULL;
                bh_arr_new(global_scratch_allocator, params, 4);
                bh_arr_set_length(params, 0);

                expect_token(parser, '(');
                while (parser->curr->type != ')') {
                    if (parser->hit_unexpected_token) return root;

                    if (peek_token(1)->type == ':') {
                        expect_token(parser, Token_Type_Symbol);
                        expect_token(parser, ':');
                    }

                    AstType* param_type = parse_type(parser);
                    bh_arr_push(params, param_type);

                    if (parser->curr->type != ')')
                        expect_token(parser, ',');
                }
                consume_token(parser);

                AstType* return_type = (AstType *) &basic_type_void;
                if (parser->curr->type == Token_Type_Right_Arrow) {
                    consume_token(parser);
                    return_type = parse_type(parser);
                }

                i64 param_count = bh_arr_length(params);
                AstFunctionType* new = onyx_ast_node_new(parser->allocator,
                        sizeof(AstFunctionType) + sizeof(AstType*) * param_count,
                        Ast_Kind_Function_Type);
                new->token = proc_token;
                new->param_count = param_count;
                new->return_type = return_type;

                if (param_count > 0)
                    fori (i, 0, param_count) new->params[i] = params[i];

                *next_insertion = (AstType *) new;
                next_insertion = NULL;
                break;
            }

            case '$': {
                parse_polymorphic_variable(parser, &next_insertion);
                break;
            }

            case Token_Type_Symbol: {
                AstNode* symbol_node = make_node(AstNode, Ast_Kind_Symbol);
                symbol_node->token = expect_token(parser, Token_Type_Symbol);

                *next_insertion = (AstType *) symbol_node;

                while (parser->curr->type == '.') {
                    consume_token(parser);
                    AstFieldAccess* field = make_node(AstFieldAccess, Ast_Kind_Field_Access);
                    field->token = expect_token(parser, Token_Type_Symbol);
                    field->expr  = (AstTyped *) *next_insertion;

                    *next_insertion = (AstType *) field;
                }

                if (parser->curr->type == '(') {
                    OnyxToken* paren_token = expect_token(parser, '(');

                    bh_arr(AstNode *) params = NULL;
                    bh_arr_new(global_heap_allocator, params, 2);

                    while (parser->curr->type != ')') {
                        if (parser->hit_unexpected_token) break;

                        AstNode* t = (AstNode *) parse_type(parser);
                        bh_arr_push(params, t);

                        if (parser->curr->type != ')')
                            expect_token(parser, ',');
                    }
                    expect_token(parser, ')');

                    AstPolyCallType* pc_type = make_node(AstPolyCallType, Ast_Kind_Poly_Call_Type);
                    pc_type->token = paren_token;
                    pc_type->callee = *next_insertion;
                    pc_type->params = params;

                    *next_insertion = (AstType *) pc_type;
                }

                next_insertion = NULL;
                break;
            }

            case Token_Type_Keyword_Struct: {
                AstStructType* s_node = parse_struct(parser);
                *next_insertion = (AstType *) s_node;
                next_insertion = NULL;
                break;
            }

            case '#': {
                // :ValueDirectiveHack
                if (parse_possible_directive(parser, "value")) {
                // It is very weird to put these here.
                case Token_Type_Literal_Integer:
                case Token_Type_Literal_String:
                case Token_Type_Literal_Float:
                case Token_Type_Literal_True:
                case Token_Type_Literal_False:
                    *next_insertion = (AstType *) parse_expression(parser, 0);
                    next_insertion = NULL;
                    break;
                }

            }

            case '(': {
                expect_token(parser, '(');
                
                *next_insertion = parse_compound_type(parser);
                next_insertion = NULL;

                expect_token(parser, ')');
                break;
            }

            default:
                onyx_report_error(parser->curr->pos, "unexpected token '%b'.", parser->curr->text, parser->curr->length);
                consume_token(parser);
                break;
        }

        if (next_insertion == NULL) break;
    }

    return root;
}

static AstStructType* parse_struct(OnyxParser* parser) {
    OnyxToken *s_token = expect_token(parser, Token_Type_Keyword_Struct);

    AstStructType* s_node;
    AstPolyStructType* poly_struct = NULL;

    s_node = make_node(AstStructType, Ast_Kind_Struct_Type);
    s_node->token = s_token;

    if (parser->curr->type == '(') {
        consume_token(parser);

        bh_arr(AstPolyStructParam) poly_params = NULL;
        bh_arr_new(global_heap_allocator, poly_params, 1);

        while (parser->curr->type != ')') {
            if (parser->hit_unexpected_token) return NULL;

            OnyxToken* sym_token = expect_token(parser, Token_Type_Symbol);
            expect_token(parser, ':');

            AstType* param_type = parse_type(parser);

            bh_arr_push(poly_params, ((AstPolyStructParam) {
                .token = sym_token,
                .type_node = param_type,
                .type = NULL, 
            }));

            if (parser->curr->type != ')')
                expect_token(parser, ',');
        }
        expect_token(parser, ')');

        poly_struct = make_node(AstPolyStructType, Ast_Kind_Poly_Struct_Type);
        poly_struct->token = s_token;
        poly_struct->poly_params = poly_params;
        poly_struct->base_struct = s_node;
    }

    bh_arr_new(global_heap_allocator, s_node->members, 4);

    while (parser->curr->type == '#') {
        if (parser->hit_unexpected_token) return NULL;

        if (parse_possible_directive(parser, "union")) {
            s_node->flags |= Ast_Flag_Struct_Is_Union;
        }

        else if (parse_possible_directive(parser, "align")) {
            AstNumLit* numlit = parse_int_literal(parser);
            if (numlit == NULL) return NULL;

            s_node->min_alignment = numlit->value.i;
        }

        else if (parse_possible_directive(parser, "size")) {
            AstNumLit* numlit = parse_int_literal(parser);
            if (numlit == NULL) return NULL;

            s_node->min_size = numlit->value.i;
        }

        else {
            OnyxToken* directive_token = expect_token(parser, '#');
            OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

            onyx_report_error(directive_token->pos, "unknown directive '#%b'.", symbol_token->text, symbol_token->length);
        }
    }

    expect_token(parser, '{');
    while (parser->curr->type != '}') {
        if (parser->hit_unexpected_token) return s_node;

        AstStructMember* mem = make_node(AstStructMember, Ast_Kind_Struct_Member);

        if (parser->curr->type == Token_Type_Keyword_Use) {
            consume_token(parser);
            mem->flags |= Ast_Flag_Struct_Mem_Used;
        }

        mem->token = expect_token(parser, Token_Type_Symbol);
        expect_token(parser, ':');
        mem->type_node = parse_type(parser);

        if (parser->curr->type == '=') {
            consume_token(parser);
            mem->initial_value = parse_expression(parser, 0);
        }

        expect_token(parser, ';');

        bh_arr_push(s_node->members, mem);
    }

    expect_token(parser, '}');

    if (poly_struct != NULL) {
        // NOTE: Not a StructType
        return (AstStructType *) poly_struct;

    } else {
        return s_node;
    }
}

// e
// '(' (<symbol>: <type>,?)* ')'
static void parse_function_params(OnyxParser* parser, AstFunction* func) {
    expect_token(parser, '(');

    if (parser->curr->type == ')') {
        consume_token(parser);
        return;
    }

    AstParam curr_param = { 0 };

    u32 param_idx = 0;
    assert(parser->polymorph_context.poly_params != NULL);

    b32 param_use = 0;
    b32 param_is_baked = 0;
    OnyxToken* symbol;
    while (parser->curr->type != ')') {
        if (parser->hit_unexpected_token) return;

        if (parser->curr->type == Token_Type_Keyword_Use) {
            consume_token(parser);
            param_use = 1;
        }

        if (parser->curr->type == '$') {
            consume_token(parser);
            param_is_baked = 1;
        }

        symbol = expect_token(parser, Token_Type_Symbol);
        expect_token(parser, ':');

        curr_param.vararg_kind = VA_Kind_Not_VA;
        curr_param.local = make_local(parser->allocator, symbol, NULL);
        curr_param.local->kind = Ast_Kind_Param;

        if (param_use) {
            curr_param.local->flags |= Ast_Flag_Param_Use;
            param_use = 0;
        }

        if (parser->curr->type != '=') {
            if (parser->curr->type == Token_Type_Dot_Dot) {
                consume_token(parser);
                curr_param.vararg_kind = VA_Kind_Typed;

                if (parser->curr->type == '.') {
                    consume_token(parser);
                    curr_param.vararg_kind = VA_Kind_Untyped;
                }
            }

            i32 old_len = 0, new_len = 0;
            if (curr_param.vararg_kind != VA_Kind_Untyped) {
                // CLEANUP: This is mess and it is hard to follow what is going on here.
                // I think with recent rewrites, this should be easier to do.
                old_len = bh_arr_length(*parser->polymorph_context.poly_params);
                curr_param.local->type_node = parse_type(parser);
                new_len = bh_arr_length(*parser->polymorph_context.poly_params);

                if (curr_param.vararg_kind == VA_Kind_Typed) {
                    AstVarArgType* va_type = make_node(AstVarArgType, Ast_Kind_VarArg_Type);
                    va_type->elem = curr_param.local->type_node;
                    va_type->token = curr_param.local->type_node->token;
                    curr_param.local->type_node = (AstType *) va_type;
                }
            }

            i32 new_poly_params = new_len - old_len;
            fori (i, 0, new_poly_params) {
                (*parser->polymorph_context.poly_params)[old_len + i].type_expr = curr_param.local->type_node;
                (*parser->polymorph_context.poly_params)[old_len + i].idx = param_idx;
            }
        }

        if (parser->curr->type == '=' && curr_param.vararg_kind == VA_Kind_Not_VA) {
            consume_token(parser);

            curr_param.default_value = parse_expression(parser, 0);
        }

        bh_arr_push(func->params, curr_param);

        if (param_is_baked) {
            param_is_baked = 0;    

            assert(parser->polymorph_context.poly_params != NULL);

            bh_arr(AstPolyParam) pv = *parser->polymorph_context.poly_params;
            bh_arr_push(pv, ((AstPolyParam) {
                .kind = PPK_Baked_Value,
                .idx = param_idx,

                .poly_sym = (AstNode *) curr_param.local,
                .type_expr = curr_param.local->type_node,
            }));

            *parser->polymorph_context.poly_params = pv;
        }

        curr_param.default_value = NULL;

        if (parser->curr->type != ')')
            expect_token(parser, ',');

        param_idx++;
    }

    consume_token(parser); // Skip the )
    return;
}

// 'proc' <func_params> ('->' <type>)? <directive>* <block>
static AstFunction* parse_function_definition(OnyxParser* parser) {
    OnyxToken* proc_token = expect_token(parser, Token_Type_Keyword_Proc);

    if (parser->curr->type == '{') {
        AstOverloadedFunction* ofunc = make_node(AstOverloadedFunction, Ast_Kind_Overloaded_Function);
        ofunc->token = proc_token;

        bh_arr_new(global_heap_allocator, ofunc->overloads, 4);

        expect_token(parser, '{');
        while (parser->curr->type != '}') {
            if (parser->hit_unexpected_token) return (AstFunction *) ofunc;

            AstTyped* o_node = parse_expression(parser, 0);

            bh_arr_push(ofunc->overloads, o_node);

            if (parser->curr->type != '}')
                expect_token(parser, ',');
        }

        consume_token(parser);
        return (AstFunction *) ofunc;
    }

    AstFunction* func_def = make_node(AstFunction, Ast_Kind_Function);
    func_def->token = proc_token;
    func_def->operator_overload = -1;

    bh_arr_new(global_heap_allocator, func_def->allocate_exprs, 4);
    bh_arr_new(global_heap_allocator, func_def->params, 4);

    bh_arr(AstPolyParam) polymorphic_vars = NULL;
    bh_arr_new(global_heap_allocator, polymorphic_vars, 4);

    parser->polymorph_context.poly_params = &polymorphic_vars;
    parse_function_params(parser, func_def);
    parser->polymorph_context.poly_params = NULL;

    AstType* return_type = (AstType *) &basic_type_void;
    if (parser->curr->type == Token_Type_Right_Arrow) {
        expect_token(parser, Token_Type_Right_Arrow);

        return_type = parse_type(parser);
    }
    func_def->return_type = return_type;

    while (parser->curr->type == '#') {
        if (parse_possible_directive(parser, "add_overload")) {
            if (func_def->overloaded_function != NULL) {
                onyx_report_error(parser->curr->pos, "cannot have multiple #add_overload directives on a single procedure.");
                expect_token(parser, Token_Type_Symbol);

            } else {
                if (bh_arr_length(parser->block_stack) != 0) {
                    onyx_report_error(parser->curr->pos, "#add_overload cannot be placed on procedures inside of other scopes");
                }

                func_def->overloaded_function = (AstNode *) parse_expression(parser, 0);
            }
        }

        else if (parse_possible_directive(parser, "operator")) {
            BinaryOp op = binary_op_from_token_type(parser->curr->type);
            consume_token(parser);
            
            if (op == Binary_Op_Count) {
                onyx_report_error(parser->curr->pos, "Invalid binary operator.");
            } else {
                func_def->operator_overload = op;
            }
        }

        else if (parse_possible_directive(parser, "intrinsic")) {
            func_def->flags |= Ast_Flag_Intrinsic;

            if (parser->curr->type == Token_Type_Literal_String) {
                OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                func_def->intrinsic_name = str_token;
            }
        }

        else if (parse_possible_directive(parser, "foreign")) {
            func_def->foreign_module = expect_token(parser, Token_Type_Literal_String);
            func_def->foreign_name   = expect_token(parser, Token_Type_Literal_String);

            func_def->flags |= Ast_Flag_Foreign;
        }

        else if (parse_possible_directive(parser, "export")) {
            func_def->flags |= Ast_Flag_Exported;

            if (parser->curr->type == Token_Type_Literal_String) {
                OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                func_def->exported_name = str_token;
            }
        }

        // HACK: NullProcHack
        else if (parse_possible_directive(parser, "null")) {
            func_def->flags |= Ast_Flag_Proc_Is_Null;
        }

        else {
            OnyxToken* directive_token = expect_token(parser, '#');
            OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

            onyx_report_error(directive_token->pos, "unknown directive '#%b'.", symbol_token->text, symbol_token->length);
        }
    }

    func_def->body = parse_block(parser);

    if (bh_arr_length(polymorphic_vars) > 0) {
        AstPolyProc* pp = make_node(AstPolyProc, Ast_Kind_Polymorphic_Proc);
        pp->token = func_def->token;
        pp->poly_params = polymorphic_vars;
        pp->base_func = func_def;

        return (AstFunction *) pp;
    } else {
        bh_arr_free(polymorphic_vars);
        return func_def;
    }
}

// 'global' <type>
static AstTyped* parse_global_declaration(OnyxParser* parser) {
    AstGlobal* global_node = make_node(AstGlobal, Ast_Kind_Global);
    global_node->token = expect_token(parser, Token_Type_Keyword_Global);

    while (parser->curr->type == '#') {
        if (parse_possible_directive(parser, "foreign")) {
            global_node->foreign_module = expect_token(parser, Token_Type_Literal_String);
            global_node->foreign_name   = expect_token(parser, Token_Type_Literal_String);

            global_node->flags |= Ast_Flag_Foreign;
        }

        else if (parse_possible_directive(parser, "export")) {
            global_node->flags |= Ast_Flag_Exported;

            if (parser->curr->type == Token_Type_Literal_String) {
                OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                global_node->exported_name = str_token;
            }
        }

        else {
            OnyxToken* directive_token = expect_token(parser, '#');
            OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

            onyx_report_error(directive_token->pos, "unknown directive '#%b'.", symbol_token->text, symbol_token->length);
        }
    }

    global_node->type_node = parse_type(parser);

    add_node_to_process(parser, (AstNode *) global_node);

    return (AstTyped *) global_node;
}

static AstEnumType* parse_enum_declaration(OnyxParser* parser) {
    AstEnumType* enum_node = make_node(AstEnumType, Ast_Kind_Enum_Type);
    enum_node->token = expect_token(parser, Token_Type_Keyword_Enum);

    bh_arr_new(global_heap_allocator, enum_node->values, 4);

    while (parser->curr->type == '#') {
        if (parse_possible_directive(parser, "flags")) {
            enum_node->flags |= Ast_Flag_Enum_Is_Flags;
        } else {
            OnyxToken* directive_token = expect_token(parser, '#');
            OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

            onyx_report_error(directive_token->pos, "unknown directive '#%b'.", symbol_token->text, symbol_token->length);
        }
    }

    AstType* backing = (AstType *) &basic_type_u32;
    if (parser->curr->type == '(') {
        consume_token(parser);

        AstNode* backing_sym = make_node(AstNode, Ast_Kind_Symbol);
        backing_sym->token = expect_token(parser, Token_Type_Symbol);
        backing = (AstType *) backing_sym;

        expect_token(parser, ')');
    }
    enum_node->backing = backing;

    expect_token(parser, '{');

    while (parser->curr->type != '}') {
        if (parser->hit_unexpected_token) return enum_node;

        AstEnumValue* evalue = make_node(AstEnumValue, Ast_Kind_Enum_Value);
        evalue->token = expect_token(parser, Token_Type_Symbol);
        evalue->type_node = (AstType *) enum_node;

        if (parser->curr->type == ':') {
            consume_token(parser);
            expect_token(parser, ':');

            // TODO: Make this work for any expression.
            evalue->value = parse_int_literal(parser);
        }

        expect_token(parser, ';');

        bh_arr_push(enum_node->values, evalue);
    }

    expect_token(parser, '}');

    return enum_node;
}

// <proc>
// <global>
// <expr>
static AstTyped* parse_top_level_expression(OnyxParser* parser) {
    if (parser->curr->type == Token_Type_Keyword_Proc) {
        AstFunction* func_node = parse_function_definition(parser);

        add_node_to_process(parser, (AstNode *) func_node);

        return (AstTyped *) func_node;
    }
    else if (parser->curr->type == Token_Type_Keyword_Global) {
        return parse_global_declaration(parser);
    }
    else if (parser->curr->type == Token_Type_Keyword_Struct) {
        return (AstTyped *) parse_struct(parser);
    }
    else if (parse_possible_directive(parser, "type")) {
        AstTypeAlias* alias = make_node(AstTypeAlias, Ast_Kind_Type_Alias);
        alias->to = parse_type(parser);
        return (AstTyped *) alias;
    }
    else if (parser->curr->type == Token_Type_Keyword_Enum) {
        return (AstTyped *) parse_enum_declaration(parser);
    }
    else {
        return parse_expression(parser, 1);
    }
}

static AstBinding* parse_top_level_binding(OnyxParser* parser, OnyxToken* symbol) {
    expect_token(parser, ':');

    AstTyped* node = parse_top_level_expression(parser);
    if (parser->hit_unexpected_token || node == NULL)
        return NULL;

    if (node->kind == Ast_Kind_Function) {
        AstFunction* func = (AstFunction *) node;

        if (func->exported_name == NULL)
            func->exported_name = symbol;

        func->name = symbol;

    } else if (node->kind == Ast_Kind_Polymorphic_Proc) {
        AstPolyProc* proc = (AstPolyProc *) node;

        if (proc->base_func->exported_name == NULL)
            proc->base_func->exported_name = symbol;

        proc->base_func->name = symbol;

    } else if (node->kind == Ast_Kind_Global) {
        AstGlobal* global = (AstGlobal *) node;

        if (global->exported_name == NULL)
            global->exported_name = symbol;

        global->name = symbol;

    } else if (node->kind != Ast_Kind_Overloaded_Function
            && node->kind != Ast_Kind_StrLit) {

        if (node->kind == Ast_Kind_Struct_Type
                || node->kind == Ast_Kind_Enum_Type
                || node->kind == Ast_Kind_Poly_Struct_Type) {
            ((AstStructType *)node)->name = bh_aprintf(global_heap_allocator,
                "%b", symbol->text, symbol->length);
        }

        if (node->kind == Ast_Kind_Type_Alias) {
            node->token = symbol;
        }

        // HACK
        add_node_to_process(parser, (AstNode *) node);
    }

    AstBinding* binding = make_node(AstBinding, Ast_Kind_Binding);
    binding->token = symbol;
    binding->node = (AstNode *) node;

    return binding;
}

// 'use' <string>
// <symbol> :: <expr>
static AstNode* parse_top_level_statement(OnyxParser* parser) {
    AstFlags private_kind = 0;
    if (parse_possible_directive(parser, "private")) {
        private_kind = Ast_Flag_Private_Package;
    }

    else if (parse_possible_directive(parser, "private_file")) {
        private_kind = Ast_Flag_Private_File;
    }

    switch ((u16) parser->curr->type) {
        case Token_Type_Keyword_Use: {
            AstNode* use_node = parse_use_stmt(parser);
            add_node_to_process(parser, use_node);
            return NULL;
        }

        case Token_Type_Keyword_Proc:
            parse_top_level_expression(parser);
            return NULL;

        case Token_Type_Symbol: {
            OnyxToken* symbol = parser->curr;
            consume_token(parser);

            expect_token(parser, ':');

            if (parser->curr->type == ':') {
                AstBinding* binding = parse_top_level_binding(parser, symbol);

                if (binding != NULL) binding->node->flags |= private_kind;

                return (AstNode *) binding;

            } else {
                AstMemRes* memres = make_node(AstMemRes, Ast_Kind_Memres);
                memres->token = symbol;

                if (parser->curr->type == '=') {
                    consume_token(parser);
                    memres->initial_value = parse_expression(parser, 1);

                } else {
                    memres->type_node = parse_type(parser);

                    if (parser->curr->type == '=') {
                        consume_token(parser);
                        memres->initial_value = parse_expression(parser, 1);
                    }
                }

                memres->flags |= private_kind;

                add_node_to_process(parser, (AstNode *) memres);

                AstBinding* binding = make_node(AstBinding, Ast_Kind_Binding);
                binding->token = symbol;
                binding->node = (AstNode *) memres;

                return (AstNode *) binding;
            }
        }

        case '#': {
            while (parser->curr->type == '#') {
                OnyxToken* dir_token = parser->curr;

                if (parse_possible_directive(parser, "load")) {
                    AstInclude* include = make_node(AstInclude, Ast_Kind_Load_File);
                    include->token = dir_token;

                    OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                    if (str_token != NULL) {
                        token_toggle_end(str_token);
                        include->name = bh_strdup(parser->allocator, str_token->text);
                        token_toggle_end(str_token);
                    }

                    return (AstNode *) include;
                }
                else if (parse_possible_directive(parser, "load_path")) {
                    AstInclude* include = make_node(AstInclude, Ast_Kind_Load_Path);
                    include->token = dir_token;
                    
                    OnyxToken* str_token = expect_token(parser, Token_Type_Literal_String);
                    if (str_token != NULL) {
                        token_toggle_end(str_token);
                        include->name = bh_strdup(parser->allocator, str_token->text);
                        token_toggle_end(str_token);
                    }

                    return (AstNode *) include;
                }
                else {
                    OnyxToken* directive_token = expect_token(parser, '#');
                    OnyxToken* symbol_token = expect_token(parser, Token_Type_Symbol);

                    onyx_report_error(directive_token->pos, "unknown directive '#%b'.", symbol_token->text, symbol_token->length);
                    return NULL;
                }
            }
        }

        default: break;
    }

    expect_token(parser, ';');
    return NULL;
}

static AstPackage* parse_package_name(OnyxParser* parser) {
    AstPackage* package_node = make_node(AstPackage, Ast_Kind_Package);

    if (parser->curr->type != Token_Type_Keyword_Package) {
        Package *package = program_info_package_lookup_or_create(
            parser->program,
            "main",
            parser->program->global_scope,
            parser->allocator);

        package_node->token = NULL;
        package_node->package = package;
        return package_node;
    }

    char package_name[1024]; // CLEANUP: This could overflow, if someone decides to be dumb
                             // with their package names  - brendanfh   2020/12/06
    package_name[0] = 0;
    package_node->token = expect_token(parser, Token_Type_Keyword_Package);

    Package *package = NULL;

    while (1) {
        if (parser->hit_unexpected_token) return package_node;

        OnyxToken* symbol = expect_token(parser, Token_Type_Symbol);

        // This logic will need to accessible elsewhere
        token_toggle_end(symbol);
        strncat(package_name, symbol->text, 1023);
        token_toggle_end(symbol);

        Package *newpackage = program_info_package_lookup_or_create(
            parser->program,
            package_name,
            parser->program->global_scope,
            parser->allocator);

        if (package != NULL) {
            AstPackage* pnode = make_node(AstPackage, Ast_Kind_Package);
            pnode->token = symbol;
            pnode->package = newpackage;

            symbol_subpackage_introduce(package->scope, symbol, pnode);
        }

        package = newpackage;

        if (parser->curr->type == '.') {
            strncat(package_name, ".", 1023);
            consume_token(parser);
        } else {
            break;
        }
    }

    package_node->package = package;
    return package_node;
}




// NOTE: This returns a void* so I don't need to cast it everytime I use it
void* onyx_ast_node_new(bh_allocator alloc, i32 size, AstKind kind) {
    void* node = bh_alloc(alloc, size);

    memset(node, 0, size);
    *(AstKind *) node = kind;

    return node;
}

OnyxParser onyx_parser_create(bh_allocator alloc, OnyxTokenizer *tokenizer, ProgramInfo* program) {
    OnyxParser parser;

    parser.allocator = alloc;
    parser.tokenizer = tokenizer;
    parser.curr = tokenizer->tokens;
    parser.prev = NULL;
    parser.program = program;
    parser.hit_unexpected_token = 0;
    parser.block_stack = NULL;

    parser.results = (ParseResults) {
        .allocator = global_heap_allocator,

        .nodes_to_process = NULL,
    };

    parser.polymorph_context = (PolymorphicContext) {
        .root_node = NULL,
        .poly_params = NULL,
    };

    bh_arr_new(global_heap_allocator, parser.block_stack, 4);
    bh_arr_new(parser.results.allocator, parser.results.nodes_to_process, 4);

    return parser;
}

void onyx_parser_free(OnyxParser* parser) {
    bh_arr_free(parser->block_stack);
}

ParseResults onyx_parse(OnyxParser *parser) {
    // NOTE: Skip comments at the beginning of the file
    if (parser->curr->type == Token_Type_Comment)
        consume_token(parser);

    parser->package = parse_package_name(parser)->package;
    parser->file_scope = scope_create(parser->allocator, parser->package->private_scope, parser->tokenizer->tokens[0].pos);

    AstUsePackage* implicit_use_builtin = make_node(AstUsePackage, Ast_Kind_Use_Package);
    implicit_use_builtin->package = (AstPackage *) &builtin_package_node;
    add_node_to_process(parser, (AstNode *) implicit_use_builtin);

    while (parser->curr->type != Token_Type_End_Stream) {
        if (parser->hit_unexpected_token) return parser->results;

        AstNode* curr_stmt = parse_top_level_statement(parser);

        if (curr_stmt != NULL && curr_stmt != &error_node) {
            while (curr_stmt != NULL) {
                if (parser->hit_unexpected_token) return parser->results;

                switch (curr_stmt->kind) {
                    case Ast_Kind_Load_File:
                    case Ast_Kind_Load_Path:
                        add_node_to_process(parser, curr_stmt);
                        break;

                    case Ast_Kind_Binding: {
                        Scope* target_scope = parser->package->scope;

                        if (((AstBinding *) curr_stmt)->node->flags & Ast_Flag_Private_Package)
                            target_scope = parser->package->private_scope;
                        if (((AstBinding *) curr_stmt)->node->flags & Ast_Flag_Private_File)
                            target_scope = parser->file_scope;

                        symbol_introduce(target_scope,
                            ((AstBinding *) curr_stmt)->token,
                            ((AstBinding *) curr_stmt)->node);
                        break;
                    }

                    default: assert(("Invalid top level node", 0));
                }

                curr_stmt = curr_stmt->next;
            }
        }
    }

    return parser->results;
}
