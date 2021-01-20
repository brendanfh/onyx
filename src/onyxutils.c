#define BH_DEBUG

#include "onyxutils.h"
#include "onyxlex.h"
#include "onyxastnodes.h"
#include "onyxerrors.h"
#include "onyxparser.h"
#include "onyxastnodes.h"
#include "onyxsempass.h"

bh_scratch global_scratch;
bh_allocator global_scratch_allocator;

bh_managed_heap global_heap;
bh_allocator global_heap_allocator;

//
// Program info and packages
//
void program_info_init(ProgramInfo* prog, bh_allocator alloc) {
    prog->global_scope = scope_create(alloc, NULL, (OnyxFilePos) { 0 });

    bh_table_init(alloc, prog->packages, 16);

    // NOTE: This will be initialized upon the first call to entity_heap_insert.
    prog->entities.entities = NULL;
}

Package* program_info_package_lookup(ProgramInfo* prog, char* package_name) {
    if (bh_table_has(Package *, prog->packages, package_name)) {
        return bh_table_get(Package *, prog->packages, package_name);
    } else {
        return NULL;
    }
}

Package* program_info_package_lookup_or_create(ProgramInfo* prog, char* package_name, Scope* parent_scope, bh_allocator alloc) {
    if (bh_table_has(Package *, prog->packages, package_name)) {
        return bh_table_get(Package *, prog->packages, package_name);

    } else {
        Package* package = bh_alloc_item(alloc, Package);

        char* pac_name = bh_alloc_array(alloc, char, strlen(package_name) + 1);
        memcpy(pac_name, package_name, strlen(package_name) + 1);

        package->name = pac_name;
        package->scope = scope_create(alloc, parent_scope, (OnyxFilePos) { 0 });
        package->private_scope = scope_create(alloc, package->scope, (OnyxFilePos) { 0 });

        bh_table_put(Package *, prog->packages, pac_name, package);

        return package;
    }
}


//
// Scoping
//
Scope* scope_create(bh_allocator a, Scope* parent, OnyxFilePos created_at) {
    Scope* scope = bh_alloc_item(a, Scope);
    scope->parent = parent;
    scope->created_at = created_at;
    scope->symbols = NULL;

    bh_table_init(global_heap_allocator, scope->symbols, 64);

    return scope;
}

void scope_include(Scope* target, Scope* source, OnyxFilePos pos) {
    bh_table_each_start(AstNode *, source->symbols);
        symbol_raw_introduce(target, (char *) key, pos, value);
    bh_table_each_end;
}

b32 symbol_introduce(Scope* scope, OnyxToken* tkn, AstNode* symbol) {
    token_toggle_end(tkn);

    b32 ret = symbol_raw_introduce(scope, tkn->text, tkn->pos, symbol);

    token_toggle_end(tkn);
    return ret;
}

b32 symbol_raw_introduce(Scope* scope, char* name, OnyxFilePos pos, AstNode* symbol) {
    if (strcmp(name, "_")) {
        if (bh_table_has(AstNode *, scope->symbols, name)) {
            if (bh_table_get(AstNode *, scope->symbols, name) != symbol) {
                onyx_report_error(pos, "Redeclaration of symbol '%s'.", name);
                return 0;
            }
            return 1;
        }
    }

    bh_table_put(AstNode *, scope->symbols, name, symbol);
    return 1;
}

void symbol_builtin_introduce(Scope* scope, char* sym, AstNode *node) {
    bh_table_put(AstNode *, scope->symbols, sym, node);
}

void symbol_subpackage_introduce(Scope* scope, OnyxToken* sym, AstPackage* package) {
    token_toggle_end(sym);

    if (bh_table_has(AstNode *, scope->symbols, sym->text)) {
        AstNode* maybe_package = bh_table_get(AstNode *, scope->symbols, sym->text);
        assert(maybe_package->kind == Ast_Kind_Package);
    } else {
        bh_table_put(AstNode *, scope->symbols, sym->text, (AstNode *) package);
    }

    token_toggle_end(sym);
}

AstNode* symbol_raw_resolve(Scope* start_scope, char* sym) {
    AstNode* res = NULL;
    Scope* scope = start_scope;

    while (res == NULL && scope != NULL) {
        if (bh_table_has(AstNode *, scope->symbols, sym)) {
            res = bh_table_get(AstNode *, scope->symbols, sym);
        } else {
            scope = scope->parent;
        }
    }

    if (res == NULL) return NULL;

    if (res->kind == Ast_Kind_Symbol) {
        return symbol_resolve(start_scope, res->token);
    }

    return res;
}

AstNode* symbol_resolve(Scope* start_scope, OnyxToken* tkn) {
    token_toggle_end(tkn);
    AstNode* res = symbol_raw_resolve(start_scope, tkn->text);

    if (res == NULL) {
        onyx_report_error(tkn->pos, "Unable to resolve symbol '%s'.", tkn->text);
        token_toggle_end(tkn);
        return &empty_node;
    }

    token_toggle_end(tkn);
    return res;
}

void scope_clear(Scope* scope) {
    bh_table_clear(scope->symbols);
}


//
// Polymorphic things
//
static void ensure_polyproc_cache_is_created(AstPolyProc* pp) {
    if (pp->concrete_funcs == NULL) {
        bh_table_init(global_heap_allocator, pp->concrete_funcs, 16);
    }
}

static void insert_poly_slns_into_scope(Scope* scope, bh_arr(AstPolySolution) slns) {
    bh_arr_each(AstPolySolution, sln, slns) {
        AstNode *node = NULL;

        switch (sln->kind) {
            case PSK_Type:
                node = onyx_ast_node_new(semstate.node_allocator, sizeof(AstTypeRawAlias), Ast_Kind_Type_Raw_Alias);
                ((AstTypeRawAlias *) node)->token = sln->poly_sym->token;
                ((AstTypeRawAlias *) node)->to = sln->type;
                break;

            case PSK_Value:
                // CLEANUP: Maybe clone this?
                assert(sln->value->flags & Ast_Flag_Comptime);
                node = (AstNode *) sln->value;
                break;
        }

        symbol_introduce(scope, sln->poly_sym->token, node);
    }
}

// NOTE: This might return a volatile string. Do not store it without copying it.
static char* build_poly_solution_key(AstPolySolution* sln) {
    static char buffer[128];

    if (sln->kind == PSK_Type) {
        return (char *) type_get_unique_name(sln->type);
    }

    else if (sln->kind == PSK_Value) {
        fori (i, 0, 128) buffer[i] = 0;

        if (sln->value->kind == Ast_Kind_NumLit) {
            strncat(buffer, "NUMLIT:", 127);
            strncat(buffer, bh_bprintf("%l", ((AstNumLit *) sln->value)->value.l), 127);

        } else {
            // HACK: For now, the value pointer is just used. This means that
            // sometimes, even through the solution is the same, it won't be
            // stored the same.
            bh_snprintf(buffer, 128, "%p", sln->value);
        }

        return buffer;
    }

    return NULL;
}

// NOTE: This returns a volatile string. Do not store it without copying it.
static char* build_poly_slns_unique_key(bh_arr(AstPolySolution) slns) {
    static char key_buf[1024];
    fori (i, 0, 1024) key_buf[i] = 0;

    bh_arr_each(AstPolySolution, sln, slns) {
        token_toggle_end(sln->poly_sym->token);

        strncat(key_buf, sln->poly_sym->token->text, 1023);
        strncat(key_buf, "=", 1023);
        strncat(key_buf, build_poly_solution_key(sln), 1023);
        strncat(key_buf, ";", 1023);

        token_toggle_end(sln->poly_sym->token);
    }

    return key_buf;
}

// NOTE: This function adds a solidified function to the entity heap for it to be processed
// later. It optionally can start the function header entity at the code generation state if
// the header has already been processed.
static b32 add_solidified_function_entities(AstSolidifiedFunction solidified_func, b32 header_already_processed) {
    solidified_func.func->flags |= Ast_Flag_Function_Used;
    solidified_func.func->flags |= Ast_Flag_From_Polymorphism;

    EntityState header_start_state = Entity_State_Resolve_Symbols;
    if (header_already_processed) header_start_state = Entity_State_Code_Gen;

    Entity func_header_entity = {
        .state = header_start_state,
        .type = Entity_Type_Function_Header,
        .function = solidified_func.func,
        .package = NULL,
        .scope = solidified_func.poly_scope,
    };

    entity_bring_to_state(&func_header_entity, Entity_State_Code_Gen);
    if (onyx_has_errors()) return 0;

    Entity func_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Function,
        .function = solidified_func.func,
        .package = NULL,
        .scope = solidified_func.poly_scope,
    };

    entity_heap_insert(&semstate.program->entities, func_header_entity);
    entity_heap_insert(&semstate.program->entities, func_entity);

    return 1;
}

// NOTE: This function is responsible for taking all of the information about generating
// a new polymorphic variant, and producing a solidified function. It optionally can only
// generate the header of the function, which is useful for cases such as checking if a
// set of arguments works for a polymorphic overload option.
static AstSolidifiedFunction generate_solidified_function(
    AstPolyProc* pp,
    bh_arr(AstPolySolution) slns,
    OnyxToken* tkn,
    b32 header_only) {

    AstSolidifiedFunction solidified_func;

    // NOTE: Use the position of token if one was provided, otherwise just use NULL.
    OnyxFilePos poly_scope_pos = { 0 };
    if (tkn) poly_scope_pos = tkn->pos;

    solidified_func.poly_scope = scope_create(semstate.node_allocator, pp->poly_scope, poly_scope_pos);
    insert_poly_slns_into_scope(solidified_func.poly_scope, slns);

    if (header_only) {
        solidified_func.func = (AstFunction *) clone_function_header(semstate.node_allocator, pp->base_func);
        solidified_func.func->flags |= Ast_Flag_Incomplete_Body;

    } else {
        solidified_func.func = (AstFunction *) ast_clone(semstate.node_allocator, pp->base_func);
    }

    solidified_func.func->flags |= Ast_Flag_From_Polymorphism;
    solidified_func.func->generated_from = tkn;

    // HACK: Remove the baked parameters from the function defintion so they can be
    // resolved in the poly scope above the function. This does feel kinda of gross
    // and I would love an alternative to tell it to just "skip" the parameter, but
    // that is liable to breaking because it is one more thing to remember.
    //                                             - brendanfh 2021/01/18
    u32 removed_params = 0;
    bh_arr_each(AstPolyParam, param, pp->poly_params) {
        if (param->kind != PPK_Baked_Value) continue;

        bh_arr_deleten(solidified_func.func->params, param->idx - removed_params, 1);
        removed_params++;
    }

    return solidified_func;
}

static void ensure_solidified_function_has_body(AstPolyProc* pp, AstSolidifiedFunction solidified_func) {
    if (solidified_func.func->flags & Ast_Flag_Incomplete_Body) {
        clone_function_body(semstate.node_allocator, solidified_func.func, pp->base_func);

        // HACK: I'm asserting that this function should return without an error, because
        // the only case where it can return an error is if there was a problem with the
        // header. This should never be the case in this situation, since the header would
        // have to have successfully passed type checking before it would become a solidified
        // procedure.
        assert(add_solidified_function_entities(solidified_func, 1));

        solidified_func.func->flags &= ~Ast_Flag_Incomplete_Body;
    }
}

// NOTE: These are temporary data structures used to represent the pattern matching system
// of polymoprhic type resolution.
typedef struct PolySolveResult {
    PolySolutionKind kind;
    union {
        AstTyped* value;
        Type*     actual;
    };
} PolySolveResult;

typedef struct PolySolveElem {
    AstType* type_expr;

    PolySolutionKind kind;
    union {
        AstTyped* value;
        Type*     actual;
    };
} PolySolveElem;

// NOTE: The job of this function is to solve for the type/value that belongs in a
// polymoprhic variable. This function takes in three arguments:
//  * The symbol node of the polymorphic parameter being searched for
//  * The type expression that should contain the symbol node it is some where
//  * The actual type to pattern match against
//
// This function utilizes a basic breadth-first search of the type_expr and actual type
// trees, always moving along them in parallel, so when the target is reached (if it is
// ever reached), the "actual" is the matched type/value.
static PolySolveResult solve_poly_type(AstNode* target, AstType* type_expr, Type* actual) {
    bh_arr(PolySolveElem) elem_queue = NULL;
    bh_arr_new(global_heap_allocator, elem_queue, 4);

    PolySolveResult result = { PSK_Undefined, { NULL } };

    bh_arr_push(elem_queue, ((PolySolveElem) {
        .type_expr = type_expr,
        .kind = PSK_Type,
        .actual    = actual
    }));

    while (!bh_arr_is_empty(elem_queue)) {
        PolySolveElem elem = elem_queue[0];
        bh_arr_deleten(elem_queue, 0, 1);

        if (elem.type_expr == (AstType *) target) {
            result.kind = elem.kind;

            assert(elem.kind != PSK_Undefined);
            if (result.kind == PSK_Type)  result.actual = elem.actual;
            if (result.kind == PSK_Value) result.value = elem.value;
            break;
        }

        if (elem.kind != PSK_Type) continue;

        switch (elem.type_expr->kind) {
            case Ast_Kind_Pointer_Type: {
                if (elem.actual->kind != Type_Kind_Pointer) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstPointerType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->Pointer.elem,
                }));
                break;
            }

            case Ast_Kind_Array_Type: {
                if (elem.actual->kind != Type_Kind_Array) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = (AstType*) ((AstArrayType *) elem.type_expr)->count_expr,
                    .kind = PSK_Value,
                    .value = (AstTyped *) make_int_literal(semstate.node_allocator, elem.actual->Array.count)
                }));

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstArrayType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->Array.elem,
                }));
                break;
            }

            case Ast_Kind_Slice_Type: {
                if (elem.actual->kind != Type_Kind_Slice) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstSliceType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->Slice.ptr_to_data->Pointer.elem,
                }));
                break;
            }

            case Ast_Kind_DynArr_Type: {
                if (elem.actual->kind != Type_Kind_DynArray) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstDynArrType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->DynArray.ptr_to_data->Pointer.elem,
                }));
                break;
            }

            case Ast_Kind_VarArg_Type:
                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstVarArgType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = actual,
                }));
                break;

            case Ast_Kind_Function_Type: {
                if (elem.actual->kind != Type_Kind_Function) break;

                AstFunctionType* ft = (AstFunctionType *) elem.type_expr;

                fori (i, 0, (i64) ft->param_count) {
                    bh_arr_push(elem_queue, ((PolySolveElem) {
                        .type_expr = ft->params[i],
                        .kind = PSK_Type,
                        .actual = elem.actual->Function.params[i],
                    }));
                }

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ft->return_type,
                    .kind = PSK_Type,
                    .actual = elem.actual->Function.return_type,
                }));

                break;
            }

            case Ast_Kind_Poly_Call_Type: {
                if (elem.actual->kind != Type_Kind_Struct) break;
                if (bh_arr_length(elem.actual->Struct.poly_sln) != bh_arr_length(((AstPolyCallType *) elem.type_expr)->params)) break;

                AstPolyCallType* pt = (AstPolyCallType *) elem.type_expr;

                fori (i, 0, bh_arr_length(pt->params)) {
                    PolySolutionKind kind = elem.actual->Struct.poly_sln[i].kind;
                    if (kind == PSK_Type) {
                        bh_arr_push(elem_queue, ((PolySolveElem) {
                            .kind = kind,
                            .type_expr = (AstType *) pt->params[i],
                            .actual = elem.actual->Struct.poly_sln[i].type,
                        }));
                    } else {
                        bh_arr_push(elem_queue, ((PolySolveElem) {
                            .kind = kind,
                            .type_expr = (AstType *) pt->params[i],
                            .value = elem.actual->Struct.poly_sln[i].value,
                        }));
                    }
                }

                break;
            }

            case Ast_Kind_Type_Compound: {
                if (elem.actual->kind != Type_Kind_Compound) break;
                if (bh_arr_length(elem.actual->Compound.types) != bh_arr_length(((AstCompoundType *) elem.type_expr)->types)) break;

                AstCompoundType* ct = (AstCompoundType *) elem.type_expr;

                fori (i, 0, bh_arr_length(ct->types)) {
                    bh_arr_push(elem_queue, ((PolySolveElem) {
                        .kind = PSK_Type,
                        .type_expr = ct->types[i],
                        .actual = elem.actual->Compound.types[i],
                    }));
                }

                break;
            }

            default: break;
        }
    }

    bh_arr_free(elem_queue);

    return result;
}

// NOTE: The job of this function is to take a polymorphic parameter and a set of arguments
// and solve for the argument that matches the parameter. This is needed because polymorphic
// procedure resolution has to happen before the named arguments are placed in their correct
// positions.
static AstTyped* lookup_param_in_arguments(AstPolyProc* pp, AstPolyParam* param, Arguments* args, char** err_msg) {
    bh_arr(AstTyped *) arg_arr = args->values;
    bh_arr(AstNamedValue *) named_values = args->named_values;

    // NOTE: This check is safe because currently the arguments given without a name
    // always map to the beginning indidies of the argument array.
    if (param->idx >= (u64) bh_arr_length(arg_arr)) {
        OnyxToken* param_name = pp->base_func->params[param->idx].local->token;

        bh_arr_each(AstNamedValue *, named_value, named_values) {
            if (token_equals(param_name, (*named_value)->token)) {
                return (AstTyped *) (*named_value)->value;
            }
        }

        // CLEANUP
        if (err_msg) *err_msg = "Not enough arguments to polymorphic procedure. This error message may not be entirely right.";

    } else {
        return (AstTyped *) arg_arr[param->idx];
    }

    return NULL;
}

// NOTE: The job of this function is to solve for type of AstPolySolution using the provided
// information. It is asssumed that the "param" is of kind PPK_Poly_Type. This function uses
// either the arguments provided, or a function type to compare against to pattern match for
// the type that the parameter but be.
static void solve_for_polymorphic_param_type(PolySolveResult* resolved, AstPolyProc* pp, AstPolyParam* param, PolyProcLookupMethod pp_lookup, ptr actual, char** err_msg) {
    Type* actual_type = NULL;

    switch (pp_lookup) {
        case PPLM_By_Arguments: {
            Arguments* args = (Arguments *) actual;

            AstTyped* typed_param = lookup_param_in_arguments(pp, param, args, err_msg);
            if (typed_param == NULL) return;

            actual_type = resolve_expression_type(typed_param);
            if (actual_type == NULL) return;

            break;
        }

        case PPLM_By_Function_Type: {
            Type* ft = (Type *) actual;
            if (param->idx >= ft->Function.param_count) {
                if (err_msg) *err_msg = "Incompatible polymorphic argument to function parameter.";
                return;
            }

            actual_type = ft->Function.params[param->idx];
            break;
        }

        default: return;
    }

    *resolved = solve_poly_type(param->poly_sym, param->type_expr, actual_type);
}

// NOTE: The job of this function is to look through the arguments provided and find a matching
// value that is to be baked into the polymorphic procedures poly-scope. It expected that param
// will be of kind PPK_Baked_Value.
// CLEANUP: This function is kind of gross at the moment, because it handles different cases for
// the argument kind. When type expressions (type_expr) become first-class types in the type
// system, this code should be able to be a lot cleaner.
static void solve_for_polymorphic_param_value(PolySolveResult* resolved, AstPolyProc* pp, AstPolyParam* param, PolyProcLookupMethod pp_lookup, ptr actual, char** err_msg) {
    if (pp_lookup != PPLM_By_Arguments) {
        *err_msg = "Function type cannot be used to solved for baked parameter value.";
        return;
    }

    Arguments* args = (Arguments *) actual;
    AstTyped* value = lookup_param_in_arguments(pp, param, args, err_msg);
    if (value == NULL) return;

    // HACK: Storing the original value because if this was an AstArgument, we need to flag
    // it as baked if it is determined that the argument is of the correct kind and type.
    AstTyped* orig_value = value;
    if (value->kind == Ast_Kind_Argument) value = ((AstArgument *) value)->value;

    if (param->type_expr == (AstType *) &type_expr_symbol) {
        if (!node_is_type((AstNode *) value)) {
            *err_msg = "Expected type expression.";
            return;
        }

        Type* resolved_type = type_build_from_ast(semstate.node_allocator, (AstType *) value);
        *resolved = ((PolySolveResult) { PSK_Type, .actual = resolved_type });

    } else {
        if ((value->flags & Ast_Flag_Comptime) == 0) {
            *err_msg = "Expected compile-time known argument.";
            return;
        }

        if (param->type == NULL)
            param->type = type_build_from_ast(semstate.node_allocator, param->type_expr);

        if (!type_check_or_auto_cast(&value, param->type)) {
            *err_msg = bh_aprintf(global_scratch_allocator,
                    "The procedure '%s' expects a value of type '%s' for %d%s parameter, got '%s'.",
                    get_function_name(pp->base_func),
                    type_get_name(param->type),
                    param->idx + 1,
                    bh_num_suffix(param->idx + 1),
                    node_get_type_name(value));
            return;
        }

        *resolved = ((PolySolveResult) { PSK_Value, value });
    }

    if (orig_value->kind == Ast_Kind_Argument) {
        ((AstArgument *) orig_value)->is_baked = 1;
    }
}

// NOTE: The job of this function is to take a polymorphic procedure, as well as a method of
// solving for the polymorphic variables, in order to return an array of the solutions for all
// of the polymorphic variables.
static bh_arr(AstPolySolution) find_polymorphic_slns(AstPolyProc* pp, PolyProcLookupMethod pp_lookup, ptr actual, char** err_msg) {
    bh_arr(AstPolySolution) slns = NULL;
    bh_arr_new(global_heap_allocator, slns, bh_arr_length(pp->poly_params));

    // NOTE: "known solutions" are given through a '#solidify' directive. If this polymorphic
    // procedure is the result of a partially applied solidification, this array will be non-
    // empty and these solutions will be used.
    bh_arr_each(AstPolySolution, known_sln, pp->known_slns) bh_arr_push(slns, *known_sln);

    bh_arr_each(AstPolyParam, param, pp->poly_params) {

        // NOTE: First check to see if this polymorphic variable was already specified in the
        // known solutions.
        b32 already_solved = 0;
        bh_arr_each(AstPolySolution, known_sln, pp->known_slns) {
            if (token_equals(param->poly_sym->token, known_sln->poly_sym->token)) {
                already_solved = 1;
                break;
            }
        }
        if (already_solved) continue;

        // NOTE: Solve for the polymoprhic parameter's value
        PolySolveResult resolved = { PSK_Undefined };
        switch (param->kind) {
            case PPK_Poly_Type:   solve_for_polymorphic_param_type (&resolved, pp, param, pp_lookup, actual, err_msg); break;
            case PPK_Baked_Value: solve_for_polymorphic_param_value(&resolved, pp, param, pp_lookup, actual, err_msg); break;

            default: if (err_msg) *err_msg = "Invalid polymorphic parameter kind. This is a compiler bug.";
        }
        
        switch (resolved.kind) {
            case PSK_Undefined:
                // NOTE: If no error message has been assigned to why this polymorphic parameter
                // resolution was unsuccessful, provide a basic dummy one.
                if (err_msg && *err_msg == NULL)
                    *err_msg = bh_aprintf(global_scratch_allocator,
                        "Unable to solve for polymoprhic variable '%b'.",
                        param->poly_sym->token->text,
                        param->poly_sym->token->length);

                goto sln_not_found;

            case PSK_Type:
                bh_arr_push(slns, ((AstPolySolution) {
                    .kind     = PSK_Type,
                    .poly_sym = param->poly_sym,
                    .type     = resolved.actual,
                }));
                break;

            case PSK_Value:
                bh_arr_push(slns, ((AstPolySolution) {
                    .kind     = PSK_Value,
                    .poly_sym = param->poly_sym,
                    .value    = resolved.value,
                }));
                break;
        }
    }

    return slns;

    sln_not_found:
    bh_arr_free(slns);
    return NULL;
}

// NOTE: The job of this function is to be a wrapper to other functions, providing an error
// message if a solution could not be found. This can't be merged with polymoprhic_proc_solidify
// because polymoprhic_proc_try_solidify uses the aforementioned function.
AstFunction* polymorphic_proc_lookup(AstPolyProc* pp, PolyProcLookupMethod pp_lookup, ptr actual, OnyxToken* tkn) {
    ensure_polyproc_cache_is_created(pp);

    char *err_msg = NULL;
    bh_arr(AstPolySolution) slns = find_polymorphic_slns(pp, pp_lookup, actual, &err_msg);
    if (slns == NULL) {
        if (err_msg != NULL) onyx_report_error(tkn->pos, err_msg);
        else                 onyx_report_error(tkn->pos, "Some kind of error occured when generating a polymorphic procedure. You hopefully will not see this");

        return NULL;
    }

    AstFunction* result = polymorphic_proc_solidify(pp, slns, tkn);
    
    bh_arr_free(slns);
    return result;
}

AstFunction* polymorphic_proc_solidify(AstPolyProc* pp, bh_arr(AstPolySolution) slns, OnyxToken* tkn) {
    ensure_polyproc_cache_is_created(pp);

    // NOTE: Check if a version of this polyproc has already been created.
    char* unique_key = build_poly_slns_unique_key(slns);
    if (bh_table_has(AstSolidifiedFunction, pp->concrete_funcs, unique_key)) {
        AstSolidifiedFunction solidified_func = bh_table_get(AstSolidifiedFunction, pp->concrete_funcs, unique_key);

        // NOTE: If this solution was originally created from a "build_only_header" call, then the body
        // will not have been or type checked, or anything. This ensures that the body is copied, the
        // entities are created and entered into the pipeline.
        ensure_solidified_function_has_body(pp, solidified_func);

        // NOTE: Again, if this came from a "build_only_header" call, then there was no known token and
        // the "generated_from" member will be null. It is best to set it here so errors reported in that
        // function can report where the polymorphic instantiation occurred.
        if (solidified_func.func->generated_from == NULL)
            solidified_func.func->generated_from = tkn;

        return solidified_func.func;
    }

    AstSolidifiedFunction solidified_func = generate_solidified_function(pp, slns, tkn, 0);

    // NOTE: Cache the function for later use, reducing duplicate functions.
    bh_table_put(AstSolidifiedFunction, pp->concrete_funcs, unique_key, solidified_func);

    if (!add_solidified_function_entities(solidified_func, 0)) {
        onyx_report_error(tkn->pos, "Error in polymorphic procedure header generated from this call site.");
        return NULL;
    }

    return solidified_func.func;
}

// NOTE: This can return either a AstFunction or an AstPolyProc, depending if enough parameters were
// supplied to remove all the polymorphic variables from the function.
AstNode* polymorphic_proc_try_solidify(AstPolyProc* pp, bh_arr(AstPolySolution) slns, OnyxToken* tkn) {
    i32 valid_argument_count = 0;

    bh_arr_each(AstPolySolution, sln, slns) {
        b32 found_match = 0;

        bh_arr_each(AstPolyParam, param, pp->poly_params) {
            if (token_equals(sln->poly_sym->token, param->poly_sym->token)) {
                found_match = 1;
                break;
            }
        }

        if (found_match) {
            valid_argument_count++;
        } else {
            onyx_report_error(tkn->pos, "'%b' is not a type variable of '%b'.",
                sln->poly_sym->token->text, sln->poly_sym->token->length,
                pp->token->text, pp->token->length);
            return (AstNode *) pp;
        }
    }

    if (valid_argument_count == bh_arr_length(pp->poly_params)) {
        return (AstNode *) polymorphic_proc_solidify(pp, slns, tkn);

    } else {
        // HACK: Some of these initializations assume that the entity for this polyproc has
        // made it through the symbol resolution phase.
        //                                                    - brendanfh 2020/12/25
        AstPolyProc* new_pp = onyx_ast_node_new(semstate.node_allocator, sizeof(AstPolyProc), Ast_Kind_Polymorphic_Proc);
        new_pp->token = pp->token;                            // TODO: Change this to be the solidify->token
        new_pp->base_func = pp->base_func;
        new_pp->poly_scope = new_pp->poly_scope;
        new_pp->flags = pp->flags;
        new_pp->poly_params = pp->poly_params;

        ensure_polyproc_cache_is_created(pp);
        new_pp->concrete_funcs = pp->concrete_funcs;

        new_pp->known_slns = NULL;
        bh_arr_new(global_heap_allocator, new_pp->known_slns, bh_arr_length(pp->known_slns) + bh_arr_length(slns));

        bh_arr_each(AstPolySolution, sln, pp->known_slns) bh_arr_push(new_pp->known_slns, *sln);
        bh_arr_each(AstPolySolution, sln, slns)           bh_arr_push(new_pp->known_slns, *sln);

        return (AstNode *) new_pp;
    }
}

AstFunction* polymorphic_proc_build_only_header(AstPolyProc* pp, PolyProcLookupMethod pp_lookup, ptr actual) {
    bh_arr(AstPolySolution) slns = find_polymorphic_slns(pp, pp_lookup, actual, NULL);
    if (slns == NULL) return NULL;

    ensure_polyproc_cache_is_created(pp);

    char* unique_key = build_poly_slns_unique_key(slns);
    if (bh_table_has(AstSolidifiedFunction, pp->concrete_funcs, unique_key)) {
        AstSolidifiedFunction solidified_func = bh_table_get(AstSolidifiedFunction, pp->concrete_funcs, unique_key);
        return solidified_func.func;
    }

    // NOTE: This function is only going to have the header of it correctly created.
    // Nothing should happen to this function's body or else the original will be corrupted.
    //                                                      - brendanfh 2021/01/10
    AstSolidifiedFunction solidified_func = generate_solidified_function(pp, slns, NULL, 1);

    Entity func_header_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Function_Header,
        .function = solidified_func.func,
        .package = NULL,
        .scope = solidified_func.poly_scope,
    };

    entity_bring_to_state(&func_header_entity, Entity_State_Code_Gen);
    if (onyx_has_errors()) {
        onyx_clear_errors();
        return NULL;
    }

    // NOTE: Cache the function for later use, only if it didn't have errors in its header.
    bh_table_put(AstSolidifiedFunction, pp->concrete_funcs, unique_key, solidified_func);
    
    return solidified_func.func;
}

char* build_poly_struct_name(AstPolyStructType* ps_type, Type* cs_type) {
    char name_buf[256];
    fori (i, 0, 256) name_buf[i] = 0;

    strncat(name_buf, ps_type->name, 255);
    strncat(name_buf, "(", 255);
    bh_arr_each(AstPolySolution, ptype, cs_type->Struct.poly_sln) {
        if (ptype != cs_type->Struct.poly_sln)
            strncat(name_buf, ", ", 255);

        // This logic will have to be other places as well.

        switch (ptype->kind) {
            case PSK_Undefined: assert(0); break;
            case PSK_Type:      strncat(name_buf, type_get_name(ptype->type), 255); break;
            case PSK_Value: {
                // FIX
                if (ptype->value->kind == Ast_Kind_NumLit) {
                    AstNumLit* nl = (AstNumLit *) ptype->value;
                    if (type_is_integer(nl->type)) {
                        strncat(name_buf, bh_bprintf("%l", nl->value.l), 127);
                    } else {
                        strncat(name_buf, "numlit (FIX ME)", 127);
                    }
                } else {
                    strncat(name_buf, "<expr>", 127);
                }

                break;
            }
        }
    }
    strncat(name_buf, ")", 255);

    return bh_aprintf(global_heap_allocator, "%s", name_buf);
}

AstStructType* polymorphic_struct_lookup(AstPolyStructType* ps_type, bh_arr(AstPolySolution) slns, OnyxFilePos pos) {
    // @Cleanup
    assert(ps_type->scope != NULL);

    if (ps_type->concrete_structs == NULL) {
        bh_table_init(global_heap_allocator, ps_type->concrete_structs, 16);
    }

    if (bh_arr_length(slns) != bh_arr_length(ps_type->poly_params)) {
        onyx_report_error(pos, "Wrong number of arguments for '%s'. Expected %d, got %d",
            ps_type->name,
            bh_arr_length(ps_type->poly_params),
            bh_arr_length(slns));

        return NULL;
    }

    i32 i = 0;
    bh_arr_each(AstPolySolution, sln, slns) {
        sln->poly_sym = (AstNode *) &ps_type->poly_params[i];
        
        PolySolutionKind expected_kind = PSK_Undefined;
        if ((AstNode *) ps_type->poly_params[i].type_node == &type_expr_symbol) {
            expected_kind = PSK_Type;
        } else {
            expected_kind = PSK_Value;
        }

        if (sln->kind != expected_kind) {
            if (expected_kind == PSK_Type) 
                onyx_report_error(pos, "Expected type expression for %d%s argument.", i + 1, bh_num_suffix(i + 1));

            if (expected_kind == PSK_Value)
                onyx_report_error(pos, "Expected value expression of type '%s' for %d%s argument.",
                    type_get_name(ps_type->poly_params[i].type),
                    i + 1, bh_num_suffix(i + 1));

            return NULL;
        }

        if (sln->kind == PSK_Value) {
            if ((sln->value->flags & Ast_Flag_Comptime) == 0) {
                onyx_report_error(pos,
                    "Expected compile-time known argument for '%b'.",
                    sln->poly_sym->token->text,
                    sln->poly_sym->token->length);
                return NULL;
            }

            if (!types_are_compatible(sln->value->type, ps_type->poly_params[i].type)) {
                onyx_report_error(pos, "Expected compile-time argument of type '%s', got '%s'.",
                    type_get_name(ps_type->poly_params[i].type),
                    type_get_name(sln->value->type));
                return NULL;
            }
        }

        i++;
    }

    char* unique_key = build_poly_slns_unique_key(slns);
    if (bh_table_has(AstStructType *, ps_type->concrete_structs, unique_key)) {
        return bh_table_get(AstStructType *, ps_type->concrete_structs, unique_key);
    }

    scope_clear(ps_type->scope);
    insert_poly_slns_into_scope(ps_type->scope, slns);

    AstStructType* concrete_struct = (AstStructType *) ast_clone(semstate.node_allocator, ps_type->base_struct);
    bh_table_put(AstStructType *, ps_type->concrete_structs, unique_key, concrete_struct);

    Entity struct_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Type_Alias,
        .type_alias = (AstType *) concrete_struct,
        .package = NULL,
        .scope = ps_type->scope,
    };
    Entity struct_default_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Struct_Member_Default,
        .type_alias = (AstType *) concrete_struct,
        .package = NULL,
        .scope = ps_type->scope,
    };

    entity_bring_to_state(&struct_entity, Entity_State_Code_Gen);
    entity_bring_to_state(&struct_default_entity, Entity_State_Code_Gen);
 
    if (onyx_has_errors()) {
        onyx_report_error(pos, "Error in creating polymoprhic struct instantiation here.");
        return NULL;
    }

    Type* cs_type = type_build_from_ast(semstate.node_allocator, (AstType *) concrete_struct);
    cs_type->Struct.poly_sln = NULL;
    bh_arr_new(global_heap_allocator, cs_type->Struct.poly_sln, bh_arr_length(slns));

    fori (i, 0, bh_arr_length(slns)) bh_arr_push(cs_type->Struct.poly_sln, slns[i]);

    cs_type->Struct.name = build_poly_struct_name(ps_type, cs_type);
    return concrete_struct;
}

void entity_bring_to_state(Entity* ent, EntityState state) {
    while (ent->state != state) {
        switch (ent->state) {
            case Entity_State_Resolve_Symbols: symres_entity(ent); break;
            case Entity_State_Check_Types:     check_entity(ent);  break;
            case Entity_State_Code_Gen:        emit_entity(ent);   break;

            default: return;
        }

        if (onyx_has_errors()) return;
    }
}


//
// Arguments resolving
//
static i32 lookup_idx_by_name(AstNode* provider, char* name) {
    switch (provider->kind) {
        case Ast_Kind_Struct_Literal: {
            AstStructLiteral* sl = (AstStructLiteral *) provider;
            assert(sl->type);

            StructMember s;
            if (!type_lookup_member(sl->type, name, &s)) return -1;
            if (s.included_through_use) return -1;

            return s.idx;
        }

        case Ast_Kind_Function: {
            AstFunction* func = (AstFunction *) provider;

            i32 param_idx = -1;
            i32 idx = 0;
            bh_arr_each(AstParam, param, func->params) {
                if (token_text_equals(param->local->token, name)) {
                    param_idx = idx;
                    break;
                }

                idx++;
            }

            return param_idx;
        }

        default: return -1;
    }
}

static AstNode* lookup_default_value_by_idx(AstNode* provider, i32 idx) {
    switch (provider->kind) {
        case Ast_Kind_Struct_Literal: {
            AstStructLiteral* sl = (AstStructLiteral *) provider;
            assert(sl->type);

            if (sl->type->kind == Type_Kind_Struct) {
                bh_arr(StructMember *) memarr = sl->type->Struct.memarr;
                if (idx >= bh_arr_length(memarr)) return NULL;

                return (AstNode *) *memarr[idx]->initial_value;
            }

            return NULL;
        }

        case Ast_Kind_Function: {
            AstFunction* func = (AstFunction *) provider;

            AstTyped* default_value = func->params[idx].default_value;
            if (default_value == NULL) return NULL;

            AstArgument* arg = make_argument(semstate.node_allocator, default_value);
            return (AstNode *) arg;
        }

        default: return NULL;
    }
}

// NOTE: The values array can be partially filled out, and is the resulting array.
// Returns if all the values were filled in.
b32 fill_in_arguments(Arguments* args, AstNode* provider, char** err_msg) {
    if (args->named_values != NULL) {
        bh_arr_each(AstNamedValue *, p_named_value, args->named_values) {
            AstNamedValue* named_value = *p_named_value;

            token_toggle_end(named_value->token);
            i32 idx = lookup_idx_by_name(provider, named_value->token->text);
            if (idx == -1) {
                if (err_msg) *err_msg = bh_aprintf(global_scratch_allocator, "'%s' is not a valid named parameter here.", named_value->token->text);
                token_toggle_end(named_value->token);
                return 0;
            }

            assert(idx < bh_arr_length(args->values));

            if (args->values[idx] != NULL) {
                if (err_msg) *err_msg = bh_aprintf(global_scratch_allocator, "Multiple values given for parameter named '%s'.", named_value->token->text);
                token_toggle_end(named_value->token);
                return 0;
            }

            args->values[idx] = named_value->value;
            token_toggle_end(named_value->token);
        }
    }

    b32 success = 1;
    fori (idx, 0, bh_arr_length(args->values)) {
        if (args->values[idx] == NULL) args->values[idx] = (AstTyped *) lookup_default_value_by_idx(provider, idx);
        if (args->values[idx] == NULL) success = 0;
    }

    return success;
}