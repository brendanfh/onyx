#define BH_DEBUG
#include "onyxparser.h"
#include "onyxutils.h"

// All of the `check` functions return a boolean that signals if an issue
// was reached while processing the node. These error booleans propagate
// up the call stack until they reach `check_entity`.

#define CHECK(kind, ...) do { \
    CheckStatus cs = check_ ## kind (__VA_ARGS__); \
    if (cs > Check_Errors_Start) return cs; \
    } while (0)

typedef enum CheckStatus {
    Check_Success,  // The node was successfully checked with out errors
    Check_Complete, // The node is done processing

    Check_Errors_Start,
    Check_Error,    // There was an error when checking the node
} CheckStatus;

CheckStatus check_block(AstBlock* block);
CheckStatus check_statement_chain(AstNode** start);
CheckStatus check_statement(AstNode** pstmt);
CheckStatus check_return(AstReturn* retnode);
CheckStatus check_if(AstIfWhile* ifnode);
CheckStatus check_while(AstIfWhile* whilenode);
CheckStatus check_for(AstFor* fornode);
CheckStatus check_switch(AstSwitch* switchnode);
CheckStatus check_call(AstCall* call);
CheckStatus check_binaryop(AstBinaryOp** pbinop, b32 assignment_is_ok);
CheckStatus check_unaryop(AstUnaryOp** punop);
CheckStatus check_struct_literal(AstStructLiteral* sl);
CheckStatus check_array_literal(AstArrayLiteral* al);
CheckStatus check_range_literal(AstRangeLiteral** range);
CheckStatus check_compound(AstCompound* compound);
CheckStatus check_expression(AstTyped** expr);
CheckStatus check_address_of(AstAddressOf* aof);
CheckStatus check_dereference(AstDereference* deref);
CheckStatus check_array_access(AstArrayAccess* expr);
CheckStatus check_field_access(AstFieldAccess** pfield);
CheckStatus check_method_call(AstBinaryOp** mcall);
CheckStatus check_size_of(AstSizeOf* so);
CheckStatus check_align_of(AstAlignOf* ao);
CheckStatus check_global(AstGlobal* global);
CheckStatus check_function(AstFunction* func);
CheckStatus check_overloaded_function(AstOverloadedFunction* func);
CheckStatus check_struct(AstStructType* s_node);
CheckStatus check_function_header(AstFunction* func);
CheckStatus check_memres_type(AstMemRes* memres);
CheckStatus check_memres(AstMemRes* memres);
CheckStatus check_type(AstType* type);

static inline void fill_in_type(AstTyped* node);

static inline void fill_in_array_count(AstType* type_node) {
    if (type_node == NULL) return;

    if (type_node->kind == Ast_Kind_Type_Alias) {
        fill_in_array_count(((AstTypeAlias *) type_node)->to);
    }

    if (type_node->kind == Ast_Kind_Array_Type) {
        if (((AstArrayType *) type_node)->count_expr) {
            check_expression(&((AstArrayType *) type_node)->count_expr);
            resolve_expression_type(((AstArrayType *) type_node)->count_expr);
        }
    }
}

static inline void fill_in_poly_call_args(AstType* type_node) {
    if (type_node == NULL) return;
    if (type_node->kind != Ast_Kind_Poly_Call_Type) return;

    AstPolyCallType* pctype = (AstPolyCallType *) type_node;

    bh_arr_each(AstNode *, param, pctype->params) {
        if (!node_is_type(*param)) {
            check_expression((AstTyped **) param);
            resolve_expression_type((AstTyped *) *param);
            fill_in_type((AstTyped *) *param);
        }
    }
}

static inline void fill_in_type(AstTyped* node) {
    fill_in_array_count(node->type_node);
    fill_in_poly_call_args(node->type_node);

    if (node->type == NULL)
        node->type = type_build_from_ast(context.ast_alloc, node->type_node);
}

// HACK: This should be baked into a structure, not a global variable.
static Type* expected_return_type = NULL;

CheckStatus check_return(AstReturn* retnode) {
    if (retnode->expr) {
        CHECK(expression, &retnode->expr);

        if (!type_check_or_auto_cast(&retnode->expr, expected_return_type)) {
            onyx_report_error(retnode->token->pos,
                    "Expected to return a value of type '%s', returning value of type '%s'.",
                    type_get_name(expected_return_type),
                    node_get_type_name(retnode->expr));
            return Check_Error;
        }

    } else {
        if (expected_return_type->Basic.size > 0) {
            onyx_report_error(retnode->token->pos,
                "Returning from non-void function without value. Expected a value of type '%s'.",
                type_get_name(expected_return_type));
            return Check_Error;
        }
    }

    return Check_Success;
}

CheckStatus check_if(AstIfWhile* ifnode) {
    if (ifnode->assignment != NULL) CHECK(statement, (AstNode **) &ifnode->assignment);

    CHECK(expression, &ifnode->cond);

    if (!type_is_bool(ifnode->cond->type)) {
        onyx_report_error(ifnode->cond->token->pos, "expected boolean type for condition");
        return Check_Error;
    }

    if (ifnode->true_stmt)  CHECK(statement, (AstNode **) &ifnode->true_stmt);
    if (ifnode->false_stmt) CHECK(statement, (AstNode **) &ifnode->false_stmt);

    return Check_Success;
}

CheckStatus check_while(AstIfWhile* whilenode) {
    if (whilenode->assignment != NULL) CHECK(statement, (AstNode **) &whilenode->assignment);

    CHECK(expression, &whilenode->cond);

    if (!type_is_bool(whilenode->cond->type)) {
        onyx_report_error(whilenode->cond->token->pos, "expected boolean type for condition");
        return Check_Error;
    }

    if (whilenode->true_stmt)  CHECK(statement, (AstNode **) &whilenode->true_stmt);
    if (whilenode->false_stmt) CHECK(statement, (AstNode **) &whilenode->false_stmt);

    return Check_Success;
}

CheckStatus check_for(AstFor* fornode) {
    CHECK(expression, &fornode->iter);
    resolve_expression_type(fornode->iter);

    Type* iter_type = fornode->iter->type;
    fornode->loop_type = For_Loop_Invalid;
    if (types_are_compatible(iter_type, &basic_types[Basic_Kind_I32])) {
        if (fornode->by_pointer) {
            onyx_report_error(fornode->var->token->pos, "Cannot iterate by pointer over a range.");
            return Check_Error;
        }

        AstNumLit* low_0    = make_int_literal(context.ast_alloc, 0);
        AstRangeLiteral* rl = make_range_literal(context.ast_alloc, (AstTyped *) low_0, fornode->iter);
        CHECK(range_literal, &rl);
        fornode->iter = (AstTyped *) rl;

        fornode->var->type = builtin_range_type_type->Struct.memarr[0]->type;
        fornode->var->flags |= Ast_Flag_Cannot_Take_Addr;
        fornode->loop_type = For_Loop_Range;
    }
    else if (types_are_compatible(iter_type, builtin_range_type_type)) {
        if (fornode->by_pointer) {
            onyx_report_error(fornode->var->token->pos, "Cannot iterate by pointer over a range.");
            return Check_Error;
        }

        // NOTE: Blindly copy the first range member's type which will
        // be the low value.                - brendanfh 2020/09/04
        fornode->var->type = builtin_range_type_type->Struct.memarr[0]->type;
        fornode->var->flags |= Ast_Flag_Cannot_Take_Addr;
        fornode->loop_type = For_Loop_Range;

    }
    else if (iter_type->kind == Type_Kind_Array) {
        if (fornode->by_pointer) fornode->var->type = type_make_pointer(context.ast_alloc, iter_type->Array.elem);
        else                     fornode->var->type = iter_type->Array.elem;

        fornode->loop_type = For_Loop_Array;
    }
    else if (iter_type->kind == Type_Kind_Slice) {
        if (fornode->by_pointer) fornode->var->type = iter_type->Slice.ptr_to_data;
        else                     fornode->var->type = iter_type->Slice.ptr_to_data->Pointer.elem;

        fornode->loop_type = For_Loop_Slice;

    }
    else if (iter_type->kind == Type_Kind_VarArgs) {
        if (fornode->by_pointer) {
            onyx_report_error(fornode->var->token->pos, "Cannot iterate by pointer over '%s'.", type_get_name(iter_type));
            return Check_Error;
        }

        fornode->var->type = iter_type->VarArgs.ptr_to_data->Pointer.elem;

        // NOTE: Slices are VarArgs are being treated the same here.
        fornode->loop_type = For_Loop_Slice;
    }
    else if (iter_type->kind == Type_Kind_DynArray) {
        if (fornode->by_pointer) fornode->var->type = iter_type->DynArray.ptr_to_data;
        else                     fornode->var->type = iter_type->DynArray.ptr_to_data->Pointer.elem;

        fornode->loop_type = For_Loop_DynArr;
    }
    else if (type_struct_constructed_from_poly_struct(iter_type, builtin_iterator_type)) {
        if (fornode->by_pointer) {
            onyx_report_error(fornode->var->token->pos, "Cannot iterate by pointer over an iterator.");
            return Check_Error;
        }

        // HACK: This assumes the Iterator type only has a single type argument.
        fornode->var->type = iter_type->Struct.poly_sln[0].type;
        fornode->loop_type = For_Loop_Iterator;
    }

    if (fornode->by_pointer)
        fornode->var->flags |= Ast_Flag_Cannot_Take_Addr;

    if (fornode->loop_type == For_Loop_Invalid) {
        onyx_report_error(fornode->iter->token->pos,
                "Cannot iterate over a '%s'.",
                type_get_name(iter_type));
        return Check_Error;
    }

    CHECK(block, fornode->stmt);

    return Check_Success;
}

static b32 add_case_to_switch_statement(AstSwitch* switchnode, u64 case_value, AstBlock* block, OnyxFilePos pos) {
    switchnode->min_case = bh_min(switchnode->min_case, case_value);
    switchnode->max_case = bh_max(switchnode->max_case, case_value);

    if (bh_imap_has(&switchnode->case_map, case_value)) {
        onyx_report_error(pos, "Multiple cases for values '%d'.", case_value);
        return 1;
    }

    bh_imap_put(&switchnode->case_map, case_value, (u64) block);
    return 0;
}

CheckStatus check_switch(AstSwitch* switchnode) {
    if (switchnode->assignment != NULL) CHECK(statement, (AstNode **) &switchnode->assignment);

    CHECK(expression, &switchnode->expr);
    Type* resolved_expr_type = resolve_expression_type(switchnode->expr);
    if (!type_is_integer(switchnode->expr->type) && switchnode->expr->type->kind != Type_Kind_Enum) {
        onyx_report_error(switchnode->expr->token->pos, "expected integer or enum type for switch expression");
        return Check_Error;
    }

    bh_imap_init(&switchnode->case_map, global_heap_allocator, bh_arr_length(switchnode->cases) * 2);

    switchnode->min_case = 0xffffffffffffffff;

    bh_arr_each(AstSwitchCase, sc, switchnode->cases) {
        CHECK(block, sc->block);

        bh_arr_each(AstTyped *, value, sc->values) {
            CHECK(expression, value);

            // :UnaryFieldAccessIsGross
            if ((*value)->kind == Ast_Kind_Unary_Field_Access) {
                type_check_or_auto_cast(value, resolved_expr_type);
            }

            if ((*value)->kind == Ast_Kind_Range_Literal) {
                AstRangeLiteral* rl = (AstRangeLiteral *) (*value);
                resolve_expression_type(rl->low);
                resolve_expression_type(rl->high);

                if (rl->low->kind != Ast_Kind_NumLit || rl->high->kind != Ast_Kind_NumLit) {
                    onyx_report_error(rl->token->pos, "case statement expected compile time known range.");
                    return Check_Error;
                }

                promote_numlit_to_larger((AstNumLit *) rl->low);
                promote_numlit_to_larger((AstNumLit *) rl->high);

                i64 lower = ((AstNumLit *) rl->low)->value.l;
                i64 upper = ((AstNumLit *) rl->high)->value.l;

                // NOTE: This is inclusive!!!!
                fori (case_value, lower, upper + 1) {
                    if (add_case_to_switch_statement(switchnode, case_value, sc->block, rl->token->pos))
                        return Check_Error;
                }

                continue;
            }

            if ((*value)->kind == Ast_Kind_Enum_Value) {
                (*value) = (AstTyped *) ((AstEnumValue *) (*value))->value;
            }

            if ((*value)->kind != Ast_Kind_NumLit) {
                onyx_report_error((*value)->token->pos, "case statement expected compile time known integer");
                return Check_Error;
            }

            resolve_expression_type((*value));
            promote_numlit_to_larger((AstNumLit *) (*value));

            if (add_case_to_switch_statement(switchnode, ((AstNumLit *) (*value))->value.l, sc->block, sc->block->token->pos))
                return Check_Error;
        }
    }

    if (switchnode->default_case)
        CHECK(block, switchnode->default_case);

    return 0;
}

CheckStatus check_arguments(Arguments* args) {
    bh_arr_each(AstTyped *, actual, args->values)
        CHECK(expression, actual);

    bh_arr_each(AstNamedValue *, named_value, args->named_values)
        CHECK(expression, &(*named_value)->value);

    return Check_Success;
}

CheckStatus check_argument(AstArgument** parg) {
    CHECK(expression, &(*parg)->value);
    (*parg)->type = (*parg)->value->type;

    return Check_Success;
}

static i32 non_baked_argument_count(Arguments* args) {
    i32 count = 0;

    bh_arr_each(AstTyped *, actual, args->values) {
        assert((*actual)->kind == Ast_Kind_Argument);
        if (!((AstArgument *) (*actual))->is_baked) count++;
    }

    bh_arr_each(AstNamedValue *, named_value, args->named_values) {
        assert((*named_value)->value->kind == Ast_Kind_Argument);
        if (!((AstArgument *) (*named_value)->value)->is_baked) count++;
    }

    return count;
}

typedef enum ArgState {
    AS_Expecting_Exact,
    AS_Expecting_Typed_VA,
    AS_Expecting_Untyped_VA,
} ArgState;

CheckStatus check_call(AstCall* call) {
    // All the things that need to be done when checking a call node.
    //      1. Ensure the callee is not a symbol
    //      2. Check the callee expression (since it could be a variable or a field access, etc)
    //      3. Check all arguments
    //          * Cannot pass overloaded functions (ROBUSTNESS)
    //      4. If callee is an overloaded function, use the argument types to determine which overload is used.
    //      5. If callee is polymorphic, use the arguments type to generate a polymorphic function.
    //      7. Fill in arguments
    //      8. If callee is an intrinsic, turn call into an Intrinsic_Call node
    //      9. Check types of formal and actual params against each other, handling varargs

    CHECK(expression, &call->callee);
    check_arguments(&call->args);

    // SPEED CLEANUP: Keeping an original copy for basically no reason except that sometimes you
    // need to know the baked argument values in code generation.
    arguments_clone(&call->original_args, &call->args);

    if (call->callee->kind == Ast_Kind_Overloaded_Function) {
        call->callee = find_matching_overload_by_arguments(((AstOverloadedFunction *) call->callee)->overloads, &call->args);
        if (call->callee == NULL) {
            report_unable_to_match_overload(call);
            return Check_Error;
        }
    }

    if (call->callee->kind == Ast_Kind_Polymorphic_Proc) {
        call->callee = (AstTyped *) polymorphic_proc_lookup((AstPolyProc *) call->callee, PPLM_By_Arguments, &call->args, call->token);
        if (call->callee == NULL) return Check_Error;

        arguments_remove_baked(&call->args);
    }

    AstFunction* callee = (AstFunction *) call->callee;

    // NOTE: Build callee's type
    fill_in_type((AstTyped *) callee);
    if (callee->type == NULL) {
        onyx_report_error(call->token->pos, "There was an error with looking up the type of this function.");
        return Check_Error;
    }

    if (callee->type->kind != Type_Kind_Function) {
        onyx_report_error(call->token->pos,
                "Attempting to call something that is not a function, '%b'.",
                callee->token->text, callee->token->length);
        return Check_Error;
    }


    // CLEANUP maybe make function_get_expected_arguments?
    i32 non_vararg_param_count = (i32) callee->type->Function.param_count;
    if (non_vararg_param_count > 0 &&
        callee->type->Function.params[callee->type->Function.param_count - 1] == builtin_vararg_type_type)
        non_vararg_param_count--;

    i32 arg_count = bh_max(non_vararg_param_count, non_baked_argument_count(&call->args));
    arguments_ensure_length(&call->args, arg_count);

    char* err_msg = NULL;
    fill_in_arguments(&call->args, (AstNode *) callee, &err_msg);
    if (err_msg != NULL) {
        onyx_report_error(call->token->pos, err_msg);
        return Check_Error;
    }

    bh_arr(AstArgument *) arg_arr = (bh_arr(AstArgument *)) call->args.values;
    bh_arr_each(AstArgument *, arg, arg_arr) {
        if (*arg != NULL) continue;

        onyx_report_error(call->token->pos, "Not all arguments were given a value.");
        return Check_Error;
    }

    // NOTE: If we are calling an intrinsic function, translate the
    // call into an intrinsic call node.
    if (callee->flags & Ast_Flag_Intrinsic) {
        call->kind = Ast_Kind_Intrinsic_Call;
        call->callee = NULL;

        token_toggle_end(callee->intrinsic_name);
        char* intr_name = callee->intrinsic_name->text;

        if (bh_table_has(OnyxIntrinsic, intrinsic_table, intr_name)) {
            call->intrinsic = bh_table_get(OnyxIntrinsic, intrinsic_table, intr_name);

        } else {
            onyx_report_error(callee->token->pos, "Intrinsic not supported, '%s'.", intr_name);
            token_toggle_end(callee->intrinsic_name);
            return Check_Error;
        }

        token_toggle_end(callee->intrinsic_name);
    }

    call->type = callee->type->Function.return_type;
    call->va_kind = VA_Kind_Not_VA;

    Type **formal_params = callee->type->Function.params;

    Type* variadic_type = NULL;
    AstParam* variadic_param = NULL;

    ArgState arg_state = AS_Expecting_Exact;
    u32 arg_pos = 0;
    while (1) {
        switch (arg_state) {
            case AS_Expecting_Exact: {
                if (arg_pos >= callee->type->Function.param_count) goto type_checking_done;

                if (formal_params[arg_pos]->kind == Type_Kind_VarArgs) {
                    variadic_type = formal_params[arg_pos]->VarArgs.ptr_to_data->Pointer.elem;
                    variadic_param = &callee->params[arg_pos];
                    arg_state = AS_Expecting_Typed_VA;
                    continue;
                }

                // CLEANUP POTENTIAL BUG if the builtin_vararg_type_type is ever rebuilt
                if (formal_params[arg_pos] == builtin_vararg_type_type) {
                    arg_state = AS_Expecting_Untyped_VA;
                    continue;
                }

                if (arg_pos >= (u32) bh_arr_length(arg_arr)) goto type_checking_done;
                if (!type_check_or_auto_cast(&arg_arr[arg_pos]->value, formal_params[arg_pos])) {
                    onyx_report_error(arg_arr[arg_pos]->token->pos,
                            "The procedure '%s' expects a value of type '%s' for %d%s parameter, got '%s'.",
                            get_function_name(callee),
                            type_get_name(formal_params[arg_pos]),
                            arg_pos + 1,
                            bh_num_suffix(arg_pos + 1),
                            node_get_type_name(arg_arr[arg_pos]->value));
                    return Check_Error;
                }

                arg_arr[arg_pos]->va_kind = VA_Kind_Not_VA;
                break;
            }

            case AS_Expecting_Typed_VA: {
                call->va_kind = VA_Kind_Typed;

                if (arg_pos >= (u32) bh_arr_length(arg_arr)) goto type_checking_done;
                if (!type_check_or_auto_cast(&arg_arr[arg_pos]->value, variadic_type)) {
                    onyx_report_error(arg_arr[arg_pos]->token->pos,
                            "The procedure '%s' expects a value of type '%s' for the variadic parameter, '%b', got '%s'.",
                            get_function_name(callee),
                            type_get_name(variadic_type),
                            variadic_param->local->token->text,
                            variadic_param->local->token->length,
                            node_get_type_name(arg_arr[arg_pos]->value));
                    return Check_Error;
                }

                arg_arr[arg_pos]->va_kind = VA_Kind_Typed;
                break;
            }

            case AS_Expecting_Untyped_VA: {
                call->va_kind = VA_Kind_Untyped;

                if (arg_pos >= (u32) bh_arr_length(arg_arr)) goto type_checking_done;

                resolve_expression_type(arg_arr[arg_pos]->value);
                arg_arr[arg_pos]->va_kind = VA_Kind_Untyped;
                break;
            }
        }

        arg_pos++;
    }

type_checking_done:

    if (arg_pos < callee->type->Function.needed_param_count) {
        onyx_report_error(call->token->pos, "Too few arguments to function call.");
        return Check_Error;
    }

    if (arg_pos < (u32) arg_count) {
        onyx_report_error(call->token->pos, "Too many arguments to function call.");
        return Check_Error;
    }

    callee->flags |= Ast_Flag_Function_Used;

    return Check_Success;
}

static void report_bad_binaryop(AstBinaryOp* binop) {
    onyx_report_error(binop->token->pos, "Binary operator '%s' not understood for arguments of type '%s' and '%s'.",
            binaryop_string[binop->operation],
            node_get_type_name(binop->left),
            node_get_type_name(binop->right));
}

CheckStatus check_binop_assignment(AstBinaryOp* binop, b32 assignment_is_ok) {
    if (!assignment_is_ok) {
        onyx_report_error(binop->token->pos, "Assignment not valid in expression.");
        return Check_Error;
    }

    if (!is_lval((AstNode *) binop->left)) {
        onyx_report_error(binop->left->token->pos,
                "Cannot assign to '%b'.",
                binop->left->token->text, binop->left->token->length);
        return Check_Error;
    }

    if ((binop->left->flags & Ast_Flag_Const) != 0 && binop->left->type != NULL) {
        onyx_report_error(binop->token->pos,
                "Cannot assign to constant '%b.'.",
                binop->left->token->text, binop->left->token->length);
        return Check_Error;
    }

    if (binop->operation == Binary_Op_Assign) {
        // NOTE: Raw assignment

        // NOTE: This is the 'type inference' system. Very stupid, but very easy.
        // If a left operand has an unknown type, fill it in with the type of
        // the right hand side.
        if (binop->left->type == NULL) {
            resolve_expression_type(binop->right);

            if (binop->right->type == NULL) {
                onyx_report_error(binop->token->pos, "Could not resolve type of right hand side to infer.");
                return Check_Error;
            }

            if (binop->right->type->kind == Type_Kind_Compound) {
                AstCompound* lhs = (AstCompound *) binop->left;
                if (lhs->kind != Ast_Kind_Compound) {
                    onyx_report_error(binop->token->pos,
                            "Expected left hand side to have %d expressions.",
                            binop->right->type->Compound.count);
                    return Check_Error;
                }

                i32 expr_count = binop->right->type->Compound.count;
                if (bh_arr_length(lhs->exprs) != expr_count) {
                    onyx_report_error(binop->token->pos,
                            "Expected left hand side to have %d expressions.",
                            binop->right->type->Compound.count);
                    return Check_Error;
                }

                fori (i, 0, expr_count) {
                    lhs->exprs[i]->type = binop->right->type->Compound.types[i];
                }

                lhs->type = type_build_compound_type(context.ast_alloc, lhs);

            } else {
                binop->left->type = binop->right->type;
            }
        }

    } else {
        // NOTE: +=, -=, ...

        BinaryOp operation = -1;
        if      (binop->operation == Binary_Op_Assign_Add)      operation = Binary_Op_Add;
        else if (binop->operation == Binary_Op_Assign_Minus)    operation = Binary_Op_Minus;
        else if (binop->operation == Binary_Op_Assign_Multiply) operation = Binary_Op_Multiply;
        else if (binop->operation == Binary_Op_Assign_Divide)   operation = Binary_Op_Divide;
        else if (binop->operation == Binary_Op_Assign_Modulus)  operation = Binary_Op_Modulus;
        else if (binop->operation == Binary_Op_Assign_And)      operation = Binary_Op_And;
        else if (binop->operation == Binary_Op_Assign_Or)       operation = Binary_Op_Or;
        else if (binop->operation == Binary_Op_Assign_Xor)      operation = Binary_Op_Xor;
        else if (binop->operation == Binary_Op_Assign_Shl)      operation = Binary_Op_Shl;
        else if (binop->operation == Binary_Op_Assign_Shr)      operation = Binary_Op_Shr;
        else if (binop->operation == Binary_Op_Assign_Sar)      operation = Binary_Op_Sar;

        AstBinaryOp* new_right = make_binary_op(context.ast_alloc, operation, binop->left, binop->right);
        new_right->token = binop->token;
        CHECK(binaryop, &new_right, 0);

        binop->right = (AstTyped *) new_right;
        binop->operation = Binary_Op_Assign;
    }

    if (!type_check_or_auto_cast(&binop->right, binop->left->type)) {
        onyx_report_error(binop->token->pos,
                "Cannot assign value of type '%s' to a '%s'.",
                node_get_type_name(binop->right),
                node_get_type_name(binop->left));
        return Check_Error;
    }

    binop->type = &basic_types[Basic_Kind_Void];

    return Check_Success;
}

CheckStatus check_binaryop_compare(AstBinaryOp** pbinop) {
    AstBinaryOp* binop = *pbinop;

    // :UnaryFieldAccessIsGross
    if (binop->left->kind == Ast_Kind_Unary_Field_Access || binop->right->kind == Ast_Kind_Unary_Field_Access) {
        if      (type_check_or_auto_cast(&binop->left, binop->right->type));
        else if (type_check_or_auto_cast(&binop->right, binop->left->type));
        else {
            report_bad_binaryop(binop);
            return Check_Error;
        }

        binop->type = &basic_types[Basic_Kind_Bool];
        return Check_Success;
    }

    if (   type_is_structlike_strict(binop->left->type)
        || type_is_structlike_strict(binop->right->type)) {
        report_bad_binaryop(binop);
        return Check_Error;
    }

    // HACK: Since ^... to rawptr is a one way conversion, strip any pointers
    // away so they can be compared as expected
    Type* ltype = binop->left->type;
    Type* rtype = binop->right->type;

    if (ltype->kind == Type_Kind_Pointer) ltype = &basic_types[Basic_Kind_Rawptr];
    if (rtype->kind == Type_Kind_Pointer) rtype = &basic_types[Basic_Kind_Rawptr];

    if (!types_are_compatible(ltype, rtype)) {
        b32 left_ac  = node_is_auto_cast((AstNode *) binop->left);
        b32 right_ac = node_is_auto_cast((AstNode *) binop->right);

        if (left_ac && right_ac) {
            onyx_report_error(binop->token->pos, "Cannot have auto cast on both sides of binary operator.");
            return Check_Error;
        }
        else if (type_check_or_auto_cast(&binop->left, rtype));
        else if (type_check_or_auto_cast(&binop->right, ltype));
        else {
            onyx_report_error(binop->token->pos,
                    "Cannot compare '%s' to '%s'.",
                    type_get_name(ltype),
                    type_get_name(rtype));
            return Check_Error;
        }
    }

    binop->type = &basic_types[Basic_Kind_Bool];
    if (binop->flags & Ast_Flag_Comptime) {
        // NOTE: Not a binary op
        *pbinop = (AstBinaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) binop);
    }

    return Check_Success;
}

CheckStatus check_binaryop_bool(AstBinaryOp** pbinop) {
    AstBinaryOp* binop = *pbinop;

    if (!type_is_bool(binop->left->type) || !type_is_bool(binop->right->type)) {
        report_bad_binaryop(binop);
        return Check_Error;
    }

    binop->type = &basic_types[Basic_Kind_Bool];

    if (binop->flags & Ast_Flag_Comptime) {
        // NOTE: Not a binary op
        *pbinop = (AstBinaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) binop);
    }
    return Check_Success;
}

static AstCall* binaryop_try_operator_overload(AstBinaryOp* binop) {
    if (bh_arr_length(operator_overloads[binop->operation]) == 0) return NULL;

    Arguments args = ((Arguments) { NULL, NULL });
    bh_arr_new(global_heap_allocator, args.values, 2);
    bh_arr_push(args.values, binop->left);
    bh_arr_push(args.values, binop->right);

    AstTyped* overload = find_matching_overload_by_arguments(operator_overloads[binop->operation], &args);
    if (overload == NULL) {
        bh_arr_free(args.values);
        return NULL;
    }

    AstCall* implicit_call = onyx_ast_node_new(context.ast_alloc, sizeof(AstCall), Ast_Kind_Call);
    implicit_call->token = binop->token;
    implicit_call->callee = overload;
    implicit_call->va_kind = VA_Kind_Not_VA;

    bh_arr_each(AstTyped *, arg, args.values)
        *arg = (AstTyped *) make_argument(context.ast_alloc, *arg);

    implicit_call->args = args;
    return implicit_call;
}

CheckStatus check_binaryop(AstBinaryOp** pbinop, b32 assignment_is_ok) {
    AstBinaryOp* binop = *pbinop;

    CHECK(expression, &binop->left);
    CHECK(expression, &binop->right);

    if ((binop->left->flags & Ast_Flag_Comptime) && (binop->right->flags & Ast_Flag_Comptime)) {
        binop->flags |= Ast_Flag_Comptime;
    }

    if (binop_is_assignment(binop->operation)) return check_binop_assignment(binop, assignment_is_ok);

    // NOTE: Try operator overloading before checking everything else.
    if (binop->left->type->kind != Type_Kind_Basic || binop->right->type->kind != Type_Kind_Basic) {
        AstCall *implicit_call = binaryop_try_operator_overload(binop);

        if (implicit_call != NULL) {
            CHECK(call, implicit_call);

            // NOTE: Not a binary op
            *pbinop = (AstBinaryOp *) implicit_call;
            return Check_Success;
        }
    }

    // NOTE: Comparision operators and boolean operators are handled separately.
    if (binop_is_compare(binop->operation)) 
        return check_binaryop_compare(pbinop);
    if (binop->operation == Binary_Op_Bool_And || binop->operation == Binary_Op_Bool_Or)
        return check_binaryop_bool(pbinop);

    // NOTE: The left side cannot be compound.
    //       The right side always is numeric.
    //       The left side cannot be rawptr.
    if (type_is_compound(binop->left->type))  goto bad_binaryop;
    if (!type_is_numeric(binop->right->type)) goto bad_binaryop;
    if (type_is_rawptr(binop->left->type)) {
        onyx_report_error(binop->token->pos, "Cannot operate on a 'rawptr'. Cast it to a another pointer type first.");
        return Check_Error;
    }

    // NOTE: Handle basic pointer math.
    if (type_is_pointer(binop->left->type)) {
        if (binop->operation != Binary_Op_Add && binop->operation != Binary_Op_Minus) goto bad_binaryop;

        resolve_expression_type(binop->right);
        if (!type_is_integer(binop->right->type)) goto bad_binaryop;

        AstNumLit* numlit = make_int_literal(context.ast_alloc, type_size_of(binop->left->type->Pointer.elem));
        numlit->token = binop->right->token;
        numlit->type = binop->right->type;

        AstBinaryOp* binop_node = make_binary_op(context.ast_alloc, Binary_Op_Multiply, binop->right, (AstTyped *) numlit);
        binop_node->token = binop->token;
        CHECK(binaryop, &binop_node, 0);

        binop->right = (AstTyped *) binop_node;
        binop->type = binop->left->type;
        binop->right->type = binop->left->type;
    }

    if (!types_are_compatible(binop->left->type, binop->right->type)) {
        b32 left_ac  = node_is_auto_cast((AstNode *) binop->left);
        b32 right_ac = node_is_auto_cast((AstNode *) binop->right);

        if (left_ac && right_ac) {
            onyx_report_error(binop->token->pos, "Cannot have auto cast on both sides of binary operator.");
            return Check_Error;
        }
        else if (type_check_or_auto_cast(&binop->left, binop->right->type));
        else if (type_check_or_auto_cast(&binop->right, binop->left->type));
        else {
            onyx_report_error(binop->token->pos,
                    "Mismatched types for binary operation '%s'. left: '%s', right: '%s'.",
                    binaryop_string[binop->operation],
                    node_get_type_name(binop->left),
                    node_get_type_name(binop->right));
            return Check_Error;
        }
    }

    static const u8 binop_allowed[Binary_Op_Count] = {
        /* Add */             Basic_Flag_Numeric | Basic_Flag_Pointer,
        /* Minus */           Basic_Flag_Numeric | Basic_Flag_Pointer,
        /* Multiply */        Basic_Flag_Numeric,
        /* Divide */          Basic_Flag_Numeric,
        /* Modulus */         Basic_Flag_Integer,

        /* Equal */           Basic_Flag_Ordered,
        /* Not_Equal */       Basic_Flag_Ordered,
        /* Less */            Basic_Flag_Ordered,
        /* Less_Equal */      Basic_Flag_Ordered,
        /* Greater */         Basic_Flag_Ordered,
        /* Greater_Equal */   Basic_Flag_Ordered,

        /* And */             Basic_Flag_Integer,
        /* Or */              Basic_Flag_Integer,
        /* Xor */             Basic_Flag_Integer,
        /* Shl */             Basic_Flag_Integer,
        /* Shr */             Basic_Flag_Integer,
        /* Sar */             Basic_Flag_Integer,

        /* Bool_And */        Basic_Flag_Boolean,
        /* Bool_Or */         Basic_Flag_Boolean,

        /* Assign_Start */    0,
        /* Assign */          0,
        /* Assign_Add */      0,
        /* Assign_Minus */    0,
        /* Assign_Multiply */ 0,
        /* Assign_Divide */   0,
        /* Assign_Modulus */  0,
        /* Assign_And */      0,
        /* Assign_Or */       0,
        /* Assign_Xor */      0,
        /* Assign_Shl */      0,
        /* Assign_Shr */      0,
        /* Assign_Sar */      0,
        /* Assign_End */      0,

        /* Pipe */            0,
        /* Range */           0,
    };

    binop->type = binop->left->type;

    enum BasicFlag effective_flags = 0;
    switch (binop->type->kind) {
        case Type_Kind_Basic:   effective_flags = binop->type->Basic.flags; break;
        case Type_Kind_Pointer: effective_flags = Basic_Flag_Pointer;       break;
        case Type_Kind_Enum:    effective_flags = Basic_Flag_Integer;       break;
    }

    if ((binop_allowed[binop->operation] & effective_flags) == 0) goto bad_binaryop;

    if (binop->flags & Ast_Flag_Comptime) {
        // NOTE: Not a binary op
        *pbinop = (AstBinaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) binop);
    }
    return Check_Success;

bad_binaryop:
    report_bad_binaryop(binop);

    return Check_Error;
}

CheckStatus check_unaryop(AstUnaryOp** punop) {
    AstUnaryOp* unaryop = *punop;

    CHECK(expression, &unaryop->expr);

    if (unaryop->operation != Unary_Op_Negate) {
        resolve_expression_type(unaryop->expr);
    }

    if (unaryop->operation == Unary_Op_Cast) {
        char* err;
        if (!cast_is_legal(unaryop->expr->type, unaryop->type, &err)) {
            onyx_report_error(unaryop->token->pos, "Cast Error: %s", err);
            return Check_Error;
        }
    } else {
        unaryop->type = unaryop->expr->type;
    }

    if (unaryop->operation == Unary_Op_Not) {
        if (!type_is_bool(unaryop->expr->type)) {
            onyx_report_error(unaryop->token->pos,
                    "Bool negation operator expected bool type, got '%s'.",
                    node_get_type_name(unaryop->expr));
            return Check_Error;
        }
    }

    if (unaryop->operation == Unary_Op_Bitwise_Not) {
        if (!type_is_integer(unaryop->expr->type)) {
            onyx_report_error(unaryop->token->pos,
                    "Bitwise operator expected integer type, got '%s'.",
                    node_get_type_name(unaryop->expr));
            return Check_Error;
        }
    }

    if (unaryop->expr->flags & Ast_Flag_Comptime) {
        unaryop->flags |= Ast_Flag_Comptime;
        // NOTE: Not a unary op
        *punop = (AstUnaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) unaryop);
    }

    return Check_Success;
}

CheckStatus check_struct_literal(AstStructLiteral* sl) {
    if (sl->type == NULL) {
        if (sl->stnode == NULL) return Check_Success;

        if (!node_is_type((AstNode *) sl->stnode)) {
            onyx_report_error(sl->token->pos, "Type used for struct literal is not a type.");
            return Check_Error;
        }

        fill_in_type((AstTyped *) sl);
        if (sl->type == NULL) return Check_Error;
    }

    if (!type_is_structlike_strict(sl->type)) {
        onyx_report_error(sl->token->pos,
                "'%s' is not constructable using a struct literal.",
                type_get_name(sl->type));
        return Check_Error;
    }

    i32 mem_count = type_structlike_mem_count(sl->type);
    arguments_ensure_length(&sl->args, mem_count);

    char* err_msg = NULL;
    if (!fill_in_arguments(&sl->args, (AstNode *) sl, &err_msg)) {
        onyx_report_error(sl->token->pos, err_msg);
        
        bh_arr_each(AstTyped *, value, sl->args.values) {
            if (*value == NULL) {
                i32 member_idx = value - sl->args.values; // Pointer subtraction hack
                StructMember smem;
                type_lookup_member_by_idx(sl->type, member_idx, &smem);

                onyx_report_error(sl->token->pos,
                    "Value not given for %d%s member, '%s', for type '%s'.",
                    member_idx + 1, bh_num_suffix(member_idx + 1),
                    smem.name, type_get_name(sl->type));
            }
        }

        return Check_Error;
    }

    AstTyped** actual = sl->args.values;
    StructMember smem;

    sl->flags |= Ast_Flag_Comptime;

    fori (i, 0, mem_count) {
        CHECK(expression, actual);

        // NOTE: Not checking the return on this function because
        // this for loop is bounded by the number of members in the
        // type.
        type_lookup_member_by_idx(sl->type, i, &smem);
        Type* formal = smem.type;

        if (!type_check_or_auto_cast(actual, formal)) {
            onyx_report_error(sl->token->pos,
                    "Mismatched types for %d%s member named '%s', expected '%s', got '%s'.",
                    i + 1, bh_num_suffix(i + 1),
                    smem.name,
                    type_get_name(formal),
                    node_get_type_name(*actual));
            return Check_Error;
        }

        sl->flags &= ((*actual)->flags & Ast_Flag_Comptime) | (sl->flags &~ Ast_Flag_Comptime);
        actual++;
    }

    return Check_Success;
}

CheckStatus check_array_literal(AstArrayLiteral* al) {
    if ((al->flags & Ast_Flag_Array_Literal_Typed) == 0) {
        if (al->atnode == NULL) return Check_Success;

        if (!node_is_type((AstNode *) al->atnode)) {
            onyx_report_error(al->token->pos, "Array type is not a type.");
            return Check_Error;
        }

        fill_in_type((AstTyped *) al);
        if (al->type == NULL) return Check_Error;

        al->type = type_make_array(context.ast_alloc, al->type, bh_arr_length(al->values));
        if (al->type == NULL || al->type->kind != Type_Kind_Array) {
            onyx_report_error(al->token->pos, "Expected array type for array literal. This is a compiler bug.");
            return Check_Error;
        }

        al->flags |= Ast_Flag_Array_Literal_Typed;
    }

    if (al->type->Array.count != (u32) bh_arr_length(al->values)) {
        onyx_report_error(al->token->pos, "Wrong array size (%d) for number of values (%d).",
            al->type->Array.count, bh_arr_length(al->values));
        return Check_Error;
    }

    al->flags |= Ast_Flag_Comptime;

    Type* elem_type = al->type->Array.elem;
    bh_arr_each(AstTyped *, expr, al->values) {
        CHECK(expression, expr);

        al->flags &= ((*expr)->flags & Ast_Flag_Comptime) | (al->flags &~ Ast_Flag_Comptime);

        if (!type_check_or_auto_cast(expr, elem_type)) {
            onyx_report_error((*expr)->token->pos, "Mismatched types for value of in array, expected '%s', got '%s'.",
                type_get_name(elem_type),
                node_get_type_name(*expr));
            return Check_Error;
        }
    }

    return Check_Success;
}

CheckStatus check_range_literal(AstRangeLiteral** prange) {
    AstRangeLiteral* range = *prange;
    CHECK(expression, &range->low);
    CHECK(expression, &range->high);

    Type* expected_range_type = builtin_range_type_type;
    StructMember smem;

    type_lookup_member(expected_range_type, "low", &smem);
    if (!type_check_or_auto_cast(&range->low, smem.type)) {
        onyx_report_error(range->token->pos,
            "Expected left side of range to be a 32-bit integer, got '%s'.",
            node_get_type_name(range->low));
        return Check_Error;
    }

    type_lookup_member(expected_range_type, "high", &smem);
    if (!type_check_or_auto_cast(&range->high, smem.type)) {
        onyx_report_error(range->token->pos,
            "Expected right side of range to be a 32-bit integer, got '%s'.",
            node_get_type_name(range->high));
        return Check_Error;
    }

    if (range->step == NULL) {
        type_lookup_member(expected_range_type, "step", &smem);
        assert(smem.initial_value != NULL);
        CHECK(expression, smem.initial_value);

        range->step = *smem.initial_value;
    }

    return Check_Success;
}

CheckStatus check_compound(AstCompound* compound) {
    bh_arr_each(AstTyped *, expr, compound->exprs) {
        CHECK(expression, expr);
    }

    compound->type = type_build_compound_type(context.ast_alloc, compound);
    return Check_Success;
}

CheckStatus check_address_of(AstAddressOf* aof) {
    CHECK(expression, &aof->expr);

    if ((aof->expr->kind != Ast_Kind_Array_Access
            && aof->expr->kind != Ast_Kind_Dereference
            && aof->expr->kind != Ast_Kind_Field_Access
            && aof->expr->kind != Ast_Kind_Memres
            && aof->expr->kind != Ast_Kind_Local)
            || (aof->expr->flags & Ast_Flag_Cannot_Take_Addr) != 0) {
        onyx_report_error(aof->token->pos, "Cannot take the address of something that is not an l-value.");
        return Check_Error;
    }

    aof->expr->flags |= Ast_Flag_Address_Taken;

    aof->type = type_make_pointer(context.ast_alloc, aof->expr->type);

    return Check_Success;
}

CheckStatus check_dereference(AstDereference* deref) {
    CHECK(expression, &deref->expr);

    if (!type_is_pointer(deref->expr->type)) {
        onyx_report_error(deref->token->pos, "Cannot dereference non-pointer value.");
        return Check_Error;
    }

    if (deref->expr->type == basic_type_rawptr.type) {
        onyx_report_error(deref->token->pos, "Cannot dereference 'rawptr'. Cast to another pointer type first.");
        return Check_Error;
    }

    deref->type = deref->expr->type->Pointer.elem;

    return Check_Success;
}

CheckStatus check_array_access(AstArrayAccess* aa) {
    CHECK(expression, &aa->addr);
    CHECK(expression, &aa->expr);

    if (!type_is_array_accessible(aa->addr->type)) {
        onyx_report_error(aa->token->pos,
                "Expected pointer or array type for left of array access, got '%s'.",
                node_get_type_name(aa->addr));
        return Check_Error;
    }

    if (types_are_compatible(aa->expr->type, builtin_range_type_type)) {
        Type *of = NULL;
        if (aa->addr->type->kind == Type_Kind_Pointer)
            of = aa->addr->type->Pointer.elem;
        else if (aa->addr->type->kind == Type_Kind_Array)
            of = aa->addr->type->Array.elem;
        else {
            // FIXME: Slice creation should be allowed for slice types and dynamic array types, like it
            // is below, but this code doesn't look at that.
            onyx_report_error(aa->token->pos, "Invalid type for left of slice creation.");
            return Check_Error;
        }

        aa->kind = Ast_Kind_Slice;
        aa->type = type_make_slice(context.ast_alloc, of);
        aa->elem_size = type_size_of(of);

        return Check_Success;
    }

    resolve_expression_type(aa->expr);
    if (aa->expr->type->kind != Type_Kind_Basic
            || (aa->expr->type->Basic.kind != Basic_Kind_I32 && aa->expr->type->Basic.kind != Basic_Kind_U32)) {
        onyx_report_error(aa->token->pos,
            "Expected type u32 or i32 for index, got '%s'.",
            node_get_type_name(aa->expr));
        return Check_Error;
    }

    if (aa->addr->type->kind == Type_Kind_Pointer)
        aa->type = aa->addr->type->Pointer.elem;
    else if (aa->addr->type->kind == Type_Kind_Array)
        aa->type = aa->addr->type->Array.elem;
    else if (aa->addr->type->kind == Type_Kind_Slice
            || aa->addr->type->kind == Type_Kind_DynArray
            || aa->addr->type->kind == Type_Kind_VarArgs) {
        // If we are accessing on a slice or a dynamic array, implicitly add a field access for the data member
        StructMember smem;
        type_lookup_member(aa->addr->type, "data", &smem);

        AstFieldAccess* fa = make_field_access(context.ast_alloc, aa->addr, "data");
        fa->type   = smem.type;
        fa->offset = smem.offset;
        fa->idx    = smem.idx;

        aa->addr = (AstTyped *) fa;
        aa->type = aa->addr->type->Pointer.elem;
    }
    else {
        onyx_report_error(aa->token->pos, "Invalid type for left of array access.");
        return Check_Error;
    }

    aa->elem_size = type_size_of(aa->type);

    return Check_Success;
}

CheckStatus check_field_access(AstFieldAccess** pfield) {
    AstFieldAccess* field = *pfield;
    CHECK(expression, &field->expr);
    if (field->expr->type == NULL) {
        onyx_report_error(field->token->pos, "Unable to deduce type of expression for accessing field.");
        return Check_Error;
    }

    if (!type_is_structlike(field->expr->type)) {
        onyx_report_error(field->token->pos,
            "Cannot access field '%b' on '%s'. Type is not a struct.",
            field->token->text,
            field->token->length,
            node_get_type_name(field->expr));
        return Check_Error;
    }

    if (!is_lval((AstNode *) field->expr)) {
        onyx_report_error(field->token->pos,
            "Cannot access field '%b'. Expression is not an lval.",
            field->token->text,
            field->token->length);
        return Check_Error;
    }

    StructMember smem;
    if (field->token != NULL && field->field == NULL) {
        token_toggle_end(field->token);
        // CLEANUP: Duplicating the string here isn't the best for effiency,
        // but it fixes a lot of bugs, so here we are.
        //                                      - brendanfh  2020/12/08
        field->field = bh_strdup(context.ast_alloc, field->token->text);
        token_toggle_end(field->token);
    }

    if (!type_lookup_member(field->expr->type, field->field, &smem)) {
        AstType* type_node = field->expr->type->ast_type;
        AstNode* n = try_symbol_raw_resolve_from_node((AstNode *) type_node, field->field);
        if (n) {
            *pfield = (AstFieldAccess *) n;
            return Check_Success;
        }

        onyx_report_error(field->token->pos,
            "Field '%s' does not exists on '%s'.",
            field->field,
            node_get_type_name(field->expr));
        return Check_Error;   
    }

    field->offset = smem.offset;
    field->idx = smem.idx;
    field->type = smem.type;

    return Check_Success;
}

CheckStatus check_method_call(AstBinaryOp** mcall) {
    CHECK(expression, &(*mcall)->left);

    AstTyped* implicit_argument = (*mcall)->left;
    
    // Implicitly take the address of the value if it is not already a pointer type.
    // This could be weird to think about semantically so some testing with real code
    // would be good.                                      - brendanfh 2020/02/05
    if (implicit_argument->type->kind != Type_Kind_Pointer)
        implicit_argument = (AstTyped *) make_address_of(context.ast_alloc, implicit_argument);
    
    implicit_argument = (AstTyped *) make_argument(context.ast_alloc, implicit_argument);

    // Symbol resolution should have ensured that this is call node.
    AstCall* call_node = (AstCall *) (*mcall)->right;
    assert(call_node->kind == Ast_Kind_Call);

    bh_arr_insertn(call_node->args.values, 0, 1);
    call_node->args.values[0] = implicit_argument;

    CHECK(call, call_node);
    call_node->next = (*mcall)->next;

    *mcall = (AstBinaryOp *) call_node;
    return Check_Success;
}

CheckStatus check_size_of(AstSizeOf* so) {
    fill_in_array_count(so->so_ast_type);

    so->so_type = type_build_from_ast(context.ast_alloc, so->so_ast_type);
    if (so->so_type == NULL) {
        onyx_report_error(so->token->pos, "Error with type used here.");
        return Check_Error;
    }
    so->size = type_size_of(so->so_type);

    return Check_Success;
}

CheckStatus check_align_of(AstAlignOf* ao) {
    fill_in_array_count(ao->ao_ast_type);

    ao->ao_type = type_build_from_ast(context.ast_alloc, ao->ao_ast_type);
    if (ao->ao_type == NULL) {
        onyx_report_error(ao->token->pos, "Error with type used here.");
        return Check_Error;
    }
    ao->alignment = type_alignment_of(ao->ao_type);

    return Check_Success;
}

CheckStatus check_expression(AstTyped** pexpr) {
    AstTyped* expr = *pexpr;
    if (expr->kind > Ast_Kind_Type_Start && expr->kind < Ast_Kind_Type_End) {
        return Check_Success;
    }

    if (expr->kind == Ast_Kind_Polymorphic_Proc) {
        // Polymoprhic procedures do not need to be checked. Their concrete instantiations
        // will be checked when they are created.
        return Check_Success;
    }

    fill_in_type(expr);

    CheckStatus retval = Check_Success;
    switch (expr->kind) {
        case Ast_Kind_Binary_Op: retval = check_binaryop((AstBinaryOp **) pexpr, 0); break;
        case Ast_Kind_Unary_Op:  retval = check_unaryop((AstUnaryOp **) pexpr); break;

        case Ast_Kind_Call:     retval = check_call((AstCall *) expr); break;
        case Ast_Kind_Argument: retval = check_argument((AstArgument **) pexpr); break;
        case Ast_Kind_Block:    retval = check_block((AstBlock *) expr); break;

        case Ast_Kind_Symbol:
            onyx_report_error(expr->token->pos,
                    "Symbol was unresolved in symbol resolution phase, '%b'. This is definitely a compiler bug.",
                    expr->token->text, expr->token->length);
            retval = Check_Error;
            break;

        case Ast_Kind_Param:
            if (expr->type == NULL) {
                onyx_report_error(expr->token->pos, "Parameter with unknown type. You should hopefully never see this.");
                retval = Check_Error;
            }
            break;

        case Ast_Kind_Local: break;

        case Ast_Kind_Address_Of:    retval = check_address_of((AstAddressOf *) expr); break;
        case Ast_Kind_Dereference:   retval = check_dereference((AstDereference *) expr); break;
        case Ast_Kind_Slice:
        case Ast_Kind_Array_Access:  retval = check_array_access((AstArrayAccess *) expr); break;
        case Ast_Kind_Field_Access:  retval = check_field_access((AstFieldAccess **) pexpr); break;
        case Ast_Kind_Method_Call:   retval = check_method_call((AstBinaryOp **) pexpr); break;
        case Ast_Kind_Size_Of:       retval = check_size_of((AstSizeOf *) expr); break;
        case Ast_Kind_Align_Of:      retval = check_align_of((AstAlignOf *) expr); break;
        case Ast_Kind_Range_Literal: retval = check_range_literal((AstRangeLiteral **) pexpr); break;

        case Ast_Kind_Global:
            if (expr->type == NULL) {
                onyx_report_error(expr->token->pos, "Global with unknown type.");
                retval = Check_Error;
            }
            break;

        case Ast_Kind_NumLit:
            assert(expr->type != NULL);
            break;

        case Ast_Kind_Struct_Literal:
            retval = check_struct_literal((AstStructLiteral *) expr);
            break;

        case Ast_Kind_Array_Literal:
            retval = check_array_literal((AstArrayLiteral *) expr);
            break;

        case Ast_Kind_Function:
            // NOTE: Will need something like this at some point
            // AstFunction* func = (AstFunction *) expr;
            // bh_arr_each(AstParam, param, func->params) {
            //     if (param->default_value != NULL) {
            //         onyx_message_add(Msg_Type_Literal,
            //                 func->token->pos,
            //                 "cannot use functions with default parameters in this way");
            //         retval = 1;
            //         break;
            //     }
            // }

            expr->flags |= Ast_Flag_Function_Used;
            break;

        case Ast_Kind_Directive_Solidify:
            *pexpr = (AstTyped *) ((AstDirectiveSolidify *) expr)->resolved_proc;
            break;

        case Ast_Kind_Compound:
            CHECK(compound, (AstCompound *) expr);
            break;

        case Ast_Kind_StrLit: break;
        case Ast_Kind_File_Contents: break;
        case Ast_Kind_Overloaded_Function: break;
        case Ast_Kind_Enum_Value: break;
        case Ast_Kind_Memres: break;
        case Ast_Kind_Polymorphic_Proc: break;
        case Ast_Kind_Package: break;
        case Ast_Kind_Error: break;
        case Ast_Kind_Unary_Field_Access: break;
        
        // NOTE: The only way to have an Intrinsic_Call node is to have gone through the
        // checking of a call node at least once.
        case Ast_Kind_Intrinsic_Call: break;

        default:
            retval = Check_Error;
            onyx_report_error(expr->token->pos, "UNEXPECTED INTERNAL COMPILER ERROR");
            DEBUG_HERE;
            break;
    }

    return retval;
}

CheckStatus check_global(AstGlobal* global) {
    fill_in_type((AstTyped *) global);

    if (global->type == NULL) {
        onyx_report_error(global->token->pos,
                "Unable to resolve type for global '%b'.",
                global->exported_name->text,
                global->exported_name->length);

        return Check_Error;
    }

    return Check_Success;
}

CheckStatus check_statement(AstNode** pstmt) {
    AstNode* stmt = *pstmt;

    switch (stmt->kind) {
        case Ast_Kind_Jump:       return Check_Success;

        case Ast_Kind_Return:     return check_return((AstReturn *) stmt);
        case Ast_Kind_If:         return check_if((AstIfWhile *) stmt);
        case Ast_Kind_While:      return check_while((AstIfWhile *) stmt);
        case Ast_Kind_For:        return check_for((AstFor *) stmt);
        case Ast_Kind_Switch:     return check_switch((AstSwitch *) stmt);
        case Ast_Kind_Block:      return check_block((AstBlock *) stmt);
        case Ast_Kind_Defer:      return check_statement(&((AstDefer *) stmt)->stmt);

        case Ast_Kind_Binary_Op:
            CHECK(binaryop, (AstBinaryOp **) pstmt, 1);
            (*pstmt)->flags |= Ast_Flag_Expr_Ignored;
            return Check_Success;

        // NOTE: Local variable declarations used to be removed after the symbol
        // resolution phase because long long ago, all locals needed to be known
        // in a block in order to efficiently allocate enough space and registers
        // for them all. Now with LocalAllocator, this is no longer necessary.
        // Therefore, locals stay in the tree and need to be passed along.
        case Ast_Kind_Local: return Check_Success;

        default:
            CHECK(expression, (AstTyped **) pstmt);
            (*pstmt)->flags |= Ast_Flag_Expr_Ignored;
            return Check_Success;
    }
}

CheckStatus check_statement_chain(AstNode** start) {
    while (*start) {
        CHECK(statement, start);
        start = &(*start)->next;
    }

    return Check_Success;
}

CheckStatus check_block(AstBlock* block) {
    CHECK(statement_chain, &block->body);

    // CLEANUP: There will need to be some other method of 
    // checking the following conditions.
    //
    // bh_arr_each(AstTyped *, value, block->allocate_exprs) {
    //     fill_in_type(*value);

    //     if ((*value)->kind == Ast_Kind_Local) {
    //         if ((*value)->type == NULL) {
    //             onyx_report_error((*value)->token->pos,
    //                     "Unable to resolve type for local '%b'.",
    //                     (*value)->token->text, (*value)->token->length);
    //             return Check_Error;
    //         }

    //         if ((*value)->type->kind == Type_Kind_Compound) {
    //             onyx_report_error((*value)->token->pos,
    //                     "Compound type not allowed as local variable type. Try splitting this into multiple variables.");
    //             return Check_Error;
    //         }
    //     }
    // }

    return Check_Success;
}

CheckStatus check_function(AstFunction* func) {
    if (func->flags & Ast_Flag_Already_Checked) return Check_Success;
    
    expected_return_type = func->type->Function.return_type;
    if (func->body) {
        CheckStatus status = check_block(func->body);
        if (status != Check_Success && func->generated_from)
            onyx_report_error(func->generated_from->pos, "Error in polymorphic procedure generated from this location.");

        return status;
    }
    
    func->flags |= Ast_Flag_Already_Checked;

    return Check_Success;
}

CheckStatus check_overloaded_function(AstOverloadedFunction* func) {
    bh_arr_each(AstTyped *, node, func->overloads) {
        if (   (*node)->kind != Ast_Kind_Function
            && (*node)->kind != Ast_Kind_Polymorphic_Proc
            && (*node)->kind != Ast_Kind_Overloaded_Function) {
            onyx_report_error((*node)->token->pos, "Overload option not procedure. Got '%s'",
                onyx_ast_node_kind_string((*node)->kind));

            return Check_Error;
        }
    }

    return Check_Success;
}

CheckStatus check_struct(AstStructType* s_node) {
    bh_arr_each(AstStructMember *, smem, s_node->members) {
        if ((*smem)->type_node == NULL && (*smem)->initial_value != NULL) {
            CHECK(expression, &(*smem)->initial_value);
            fill_in_type((*smem)->initial_value);
            (*smem)->type = resolve_expression_type((*smem)->initial_value);

            if ((*smem)->type == NULL) {
                onyx_report_error((*smem)->initial_value->token->pos, "Unable to deduce type of initial value. This is probably a compiler bug.");
                return Check_Error;
            }
        }
    }

    // NOTE: fills in the stcache
    type_build_from_ast(context.ast_alloc, (AstType *) s_node);
    if (s_node->stcache == NULL) return Check_Error;

    bh_arr_each(StructMember *, smem, s_node->stcache->Struct.memarr) {
        if ((*smem)->type->kind == Type_Kind_Compound) {
            onyx_report_error(s_node->token->pos, "Compound types are not allowed as struct member types.");
            return Check_Error;
        }
    }

    return Check_Success;
}

CheckStatus check_struct_defaults(AstStructType* s_node) {
    bh_arr_each(StructMember *, smem, s_node->stcache->Struct.memarr) {
        if ((*smem)->initial_value && *(*smem)->initial_value) {
            CHECK(expression, (*smem)->initial_value);

            if (!type_check_or_auto_cast((*smem)->initial_value, (*smem)->type)) {
                onyx_report_error((*(*smem)->initial_value)->token->pos,
                        "Mismatched type for initial value, expected '%s', got '%s'.",
                        type_get_name((*smem)->type),
                        type_get_name((*(*smem)->initial_value)->type));
                return Check_Error;
            }

            resolve_expression_type(*(*smem)->initial_value);
        }
    }

    return Check_Success;
}

CheckStatus check_function_header(AstFunction* func) {
    b32 expect_default_param = 0;
    b32 has_had_varargs = 0;

    bh_arr_each(AstParam, param, func->params) {
        AstLocal* local = param->local;

        if (expect_default_param && param->default_value == NULL) {
            onyx_report_error(local->token->pos,
                    "All parameters must have default values after the first default valued parameter.");
            return Check_Error;
        }

        if (has_had_varargs && param->vararg_kind != VA_Kind_Not_VA) {
            onyx_report_error(local->token->pos,
                    "Can only have one param that is of variable argument type.");
            return Check_Error;
        }

        if (has_had_varargs && param->vararg_kind != VA_Kind_Not_VA) {
            onyx_report_error(local->token->pos,
                    "Variable arguments must be last in parameter list");
            return Check_Error;
        }

        if (param->vararg_kind == VA_Kind_Untyped) {
            // HACK
            if (builtin_vararg_type_type == NULL)
                builtin_vararg_type_type = type_build_from_ast(context.ast_alloc, builtin_vararg_type);

            local->type = builtin_vararg_type_type;
        }

        if (param->default_value != NULL) {
            if (param->vararg_kind != VA_Kind_Not_VA) {
                onyx_report_error(local->token->pos, "Variadic arguments cannot have default values.");
                return Check_Error;
            }

            CHECK(expression, &param->default_value);

            if (local->type_node == NULL && local->type == NULL) {
                local->type = resolve_expression_type(param->default_value);
            }

            expect_default_param = 1;
        }

        if (local->type_node != NULL) CHECK(type, local->type_node);

        fill_in_type((AstTyped *) local);

        if (local->type == NULL) {
            onyx_report_error(param->local->token->pos,
                    "Unable to resolve type for parameter, '%b'.\n",
                    local->token->text,
                    local->token->length);
            return Check_Error;
        }

        if (local->type->kind == Type_Kind_Compound) {
            onyx_report_error(param->local->token->pos, "Compound types are not allowed as parameter types. Try splitting this into multiple parameters.");
            return Check_Error;
        }

        // NOTE: I decided to make parameter default values not type checked against
        // the actual parameter type. The actual type checking will happen in check_call
        // when the default value is used as an argument and then has to be checked against
        // the parameter type                                  - brendanfh 2021/01/06
        // if (param->default_value != NULL) {
        //     if (!type_check_or_auto_cast(&param->default_value, param->local->type)) {
        //         onyx_report_error(param->local->token->pos,
        //                 "Expected default value of type '%s', was of type '%s'.",
        //                 type_get_name(param->local->type),
        //                 type_get_name(param->default_value->type));
        //         return Check_Error;
        //     }
        // }

        if (param->vararg_kind != VA_Kind_Not_VA) has_had_varargs = 1;

        if (local->type->kind != Type_Kind_Array
            && type_size_of(local->type) == 0) {
            onyx_report_error(local->token->pos, "Function parameters cannot have zero-width types.");
            return Check_Error;
        }
    }

    if (func->return_type != NULL) CHECK(type, func->return_type);

    func->type = type_build_function_type(context.ast_alloc, func);

    if ((func->flags & Ast_Flag_Exported) != 0) {
        if ((func->flags & Ast_Flag_Foreign) != 0) {
            onyx_report_error(func->token->pos, "exporting a foreign function");
            return Check_Error;
        }

        if ((func->flags & Ast_Flag_Intrinsic) != 0) {
            onyx_report_error(func->token->pos, "exporting a intrinsic function");
            return Check_Error;
        }

        if (func->exported_name == NULL) {
            onyx_report_error(func->token->pos, "exporting function without a name");
            return Check_Error;
        }
    }

    return Check_Success;
}

CheckStatus check_memres_type(AstMemRes* memres) {
    fill_in_type((AstTyped *) memres);
    return Check_Success;
}

CheckStatus check_memres(AstMemRes* memres) {
    if (memres->initial_value != NULL) {
        CHECK(expression, &memres->initial_value);
        resolve_expression_type(memres->initial_value);

        if ((memres->initial_value->flags & Ast_Flag_Comptime) == 0) {
            onyx_report_error(memres->initial_value->token->pos, "Top level expressions must be compile time known.");
            return Check_Error;
        }

        if (memres->type != NULL) {
            Type* memres_type = memres->type;
            if (!type_check_or_auto_cast(&memres->initial_value, memres_type)) {
                onyx_report_error(memres->token->pos,
                        "Cannot assign value of type '%s' to a '%s'.",
                        node_get_type_name(memres->initial_value),
                        type_get_name(memres_type));
                return Check_Error;
            }

        } else {
            memres->type = memres->initial_value->type;
        }
    }

    return Check_Success;
}

CheckStatus check_type(AstType* type) {
    while (type->kind == Ast_Kind_Type_Alias)
        type = ((AstTypeAlias *) type)->to;

    if (type->kind == Ast_Kind_Poly_Call_Type) {
        AstPolyCallType* pc_node = (AstPolyCallType *) type;

        bh_arr_each(AstNode *, param, pc_node->params) {
            if (!node_is_type(*param)) {
                CHECK(expression, (AstTyped **) param);
                resolve_expression_type((AstTyped *) *param);
            }
        }
    }

    return Check_Success;
}

CheckStatus check_static_if(AstStaticIf* static_if) {
    CheckStatus result = check_expression(&static_if->cond);

    if (result > Check_Errors_Start || !(static_if->cond->flags & Ast_Flag_Comptime)) {
        onyx_report_error(static_if->token->pos, "Expected this condition to be compile time known.");
        return Check_Error;
    }

    if (!type_is_bool(static_if->cond->type)) {
        onyx_report_error(static_if->token->pos, "Expected this condition to be a boolean value.");
        return Check_Error;
    }

    AstNumLit* condition_value = (AstNumLit *) static_if->cond;
    assert(condition_value->kind == Ast_Kind_NumLit); // This should be right, right?

    if (context.options->print_static_if_results)
        bh_printf("Static if statement at %s:%d:%d resulted in %s\n",
            static_if->token->pos.filename,
            static_if->token->pos.line,
            static_if->token->pos.column,
            condition_value->value.i ? "true" : "false");

    if (condition_value->value.i) {
        bh_arr_each(Entity *, ent, static_if->true_entities) {
            entity_heap_insert_existing(&context.entities, *ent);
        }

    } else {
        bh_arr_each(Entity *, ent, static_if->false_entities) {
            entity_heap_insert_existing(&context.entities, *ent);
        }
    }

    return Check_Complete;
}

CheckStatus check_node(AstNode* node) {
    switch (node->kind) {
        case Ast_Kind_Function:             return check_function((AstFunction *) node);
        case Ast_Kind_Overloaded_Function:  return check_overloaded_function((AstOverloadedFunction *) node);
        case Ast_Kind_Block:                return check_block((AstBlock *) node);
        case Ast_Kind_Return:               return check_return((AstReturn *) node);
        case Ast_Kind_If:                   return check_if((AstIfWhile *) node);
        case Ast_Kind_While:                return check_while((AstIfWhile *) node);
        case Ast_Kind_Call:                 return check_call((AstCall *) node);
        case Ast_Kind_Binary_Op:            return check_binaryop((AstBinaryOp **) &node, 1);
        default:                            return check_expression((AstTyped **) &node);
    }
}

void check_entity(Entity* ent) {
    CheckStatus cs = Check_Success;

    switch (ent->type) {
        case Entity_Type_Foreign_Function_Header:
        case Entity_Type_Function_Header:
            cs = check_function_header(ent->function);
            break;

        case Entity_Type_Function:
            cs = check_function(ent->function);
            break;

        case Entity_Type_Overloaded_Function:
            cs = check_overloaded_function(ent->overloaded_function);
            break;

        case Entity_Type_Global:
            cs = check_global(ent->global);
            break;

        case Entity_Type_Expression:
            cs = check_expression(&ent->expr);
            resolve_expression_type(ent->expr);
            break;

        case Entity_Type_Type_Alias:
            if (ent->type_alias->kind == Ast_Kind_Struct_Type)
                cs = check_struct((AstStructType *) ent->type_alias);
            else
                cs = check_type(ent->type_alias);
            break;

        case Entity_Type_Struct_Member_Default:
            cs = check_struct_defaults((AstStructType *) ent->type_alias);
            break;

        case Entity_Type_Memory_Reservation_Type:
            cs = check_memres_type(ent->mem_res);
            break;

        case Entity_Type_Memory_Reservation:
            cs = check_memres(ent->mem_res);
            break;

        case Entity_Type_Static_If:
            cs = check_static_if(ent->static_if);
            break;

        default: break;
    }

    if (cs == Check_Success)  ent->state = Entity_State_Code_Gen;
    if (cs == Check_Complete) ent->state = Entity_State_Finalized;
    // else if (cs == Check_Yield) {
    //     ent->attempts++;
    // }
}
