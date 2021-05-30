#include "bh.h"
#include "onyxastnodes.h"
#include "onyxutils.h"

static inline i32 entity_phase(Entity* e1) {
    if (e1->state <= Entity_State_Parse) return 1;
    if (e1->state <= Entity_State_Comptime_Check_Types) return 2;
    return 3;
}

// NOTE: Returns >0 if e1 should be processed after e2.
static i32 entity_compare(Entity* e1, Entity* e2) {
    i32 phase1 = entity_phase(e1);
    i32 phase2 = entity_phase(e2);

    if (phase1 != phase2 || phase1 != 2) {
        if (e1->state != e2->state)
            return (i32) e1->state - (i32) e2->state;
        else if (e1->type != e2->type)
            return (i32) e1->type - (i32) e2->type;
        else
            return (i32) (e1->micro_attempts - e2->micro_attempts);

    } else {
        return (i32) e1->macro_attempts - (i32) e2->macro_attempts;
    }
}

#define eh_parent(index) (((index) - 1) / 2)
#define eh_lchild(index) (((index) * 2) + 1)
#define eh_rchild(index) (((index) * 2) + 2)

static void eh_shift_up(EntityHeap* entities, i32 index) {
	while (index > 0 && entity_compare(entities->entities[eh_parent(index)], entities->entities[index]) > 0) {
		Entity* tmp = entities->entities[eh_parent(index)];
		entities->entities[eh_parent(index)] = entities->entities[index];			
		entities->entities[index] = tmp;

		index = eh_parent(index);
	}
}

static void eh_shift_down(EntityHeap* entities, i32 index) {
	i32 min_index = index;

	i32 l = eh_lchild(index);	
	if (l < bh_arr_length(entities->entities)
		&& entity_compare(entities->entities[l], entities->entities[min_index]) < 0) {
		min_index = l;
	}

	i32 r = eh_rchild(index);	
	if (r < bh_arr_length(entities->entities)
		&& entity_compare(entities->entities[r], entities->entities[min_index]) < 0) {
		min_index = r;
	}

	if (index != min_index) {
		Entity* tmp = entities->entities[min_index];
		entities->entities[min_index] = entities->entities[index];
		entities->entities[index] = tmp;

		eh_shift_down(entities, min_index);
	}
}

void entity_heap_init(EntityHeap* entities) {
    bh_arena_init(&entities->entity_arena, global_heap_allocator, 32 * 1024);
}

// Allocates the entity in the entity heap. Don't quite feel this is necessary...
Entity* entity_heap_register(EntityHeap* entities, Entity e) {
    bh_allocator alloc = bh_arena_allocator(&entities->entity_arena);
    Entity* entity = bh_alloc_item(alloc, Entity);
    *entity = e;
    entity->macro_attempts = 0;
    entity->micro_attempts = 0;
    entity->entered_in_queue = 0;

    return entity;
}

void entity_heap_insert_existing(EntityHeap* entities, Entity* e) {
    if (e->entered_in_queue) return;

	if (entities->entities == NULL) {
		bh_arr_new(global_heap_allocator, entities->entities, 128);
	}

	bh_arr_push(entities->entities, e);
	eh_shift_up(entities, bh_arr_length(entities->entities) - 1);
    e->entered_in_queue = 1;

    entities->state_count[e->state]++;
    entities->type_count[e->type]++;
    entities->all_count[e->state][e->type]++;
}

// nocheckin
// Temporary wrapper
void entity_heap_insert(EntityHeap* entities, Entity e) {
    Entity* entity = entity_heap_register(entities, e);
    entity_heap_insert_existing(entities, entity);
}

Entity* entity_heap_top(EntityHeap* entities) {
	return entities->entities[0];
}

void entity_heap_change_top(EntityHeap* entities, Entity* new_top) {
    entities->state_count[entities->entities[0]->state]--;
    entities->state_count[new_top->state]++;

    entities->type_count[entities->entities[0]->type]--;
    entities->type_count[new_top->type]++;

    entities->all_count[entities->entities[0]->state][entities->entities[0]->type]--;
    entities->all_count[new_top->state][new_top->type]++;
	
	entities->entities[0] = new_top;
	eh_shift_down(entities, 0);
}

void entity_heap_remove_top(EntityHeap* entities) {
    entities->state_count[entities->entities[0]->state]--;
    entities->type_count[entities->entities[0]->type]--;
    entities->all_count[entities->entities[0]->state][entities->entities[0]->type]--;
    entities->entities[0]->entered_in_queue = 0;

	entities->entities[0] = entities->entities[bh_arr_length(entities->entities) - 1];
	bh_arr_pop(entities->entities);
	eh_shift_down(entities, 0);
}

// NOTE(Brendan Hansen): Uses the entity heap in the context structure
void add_entities_for_node(bh_arr(Entity *) *target_arr, AstNode* node, Scope* scope, Package* package) {
#define ENTITY_INSERT(_ent) {                                   \
    Entity* entity = entity_heap_register(entities, _ent);       \
    if (target_arr) {                                           \
        bh_arr(Entity *) __tmp_arr = *target_arr;               \
        bh_arr_push(__tmp_arr, entity);                         \
        *target_arr = __tmp_arr;                                \
    } else {                                                    \
        entity_heap_insert_existing(entities, entity);          \
    }                                                           \
    }

    EntityHeap* entities = &context.entities;
    
    Entity ent;
    ent.state = Entity_State_Resolve_Symbols;
    ent.package = package;
    ent.scope   = scope;
    
    switch (node->kind) {
        case Ast_Kind_Load_File: {
            ent.state = Entity_State_Parse;
            ent.type = Entity_Type_Load_File;
            ent.include = (AstInclude *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Load_Path: {
            ent.state = Entity_State_Parse;
            ent.type = Entity_Type_Load_Path;
            ent.include = (AstInclude *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Binding: {
            ent.state   = Entity_State_Introduce_Symbols;
            ent.type    = Entity_Type_Binding;
            ent.binding = (AstBinding *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Function: {
            if ((node->flags & Ast_Flag_Foreign) != 0) {
                ent.type     = Entity_Type_Foreign_Function_Header;
                ent.function = (AstFunction *) node;
                ENTITY_INSERT(ent);
                
            } else {
                ent.type     = Entity_Type_Function_Header;
                ent.function = (AstFunction *) node;
                ENTITY_INSERT(ent);
                
                ent.type     = Entity_Type_Function;
                ent.function = (AstFunction *) node;
                ENTITY_INSERT(ent);
            }
            break;
        }
        
        case Ast_Kind_Overloaded_Function: {
            ent.type                = Entity_Type_Overloaded_Function;
            ent.overloaded_function = (AstOverloadedFunction *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Global: {
            if ((node->flags & Ast_Flag_Foreign) != 0) {
                ent.type   = Entity_Type_Foreign_Global_Header;
                ent.global = (AstGlobal *) node;
                ENTITY_INSERT(ent);
                
            } else {
                ent.type   = Entity_Type_Global_Header;
                ent.global = (AstGlobal *) node;
                ENTITY_INSERT(ent);
                
                ent.type   = Entity_Type_Global;
                ent.global = (AstGlobal *) node;
                ENTITY_INSERT(ent);
            }
            break;
        }
        
        case Ast_Kind_StrLit: {
            ent.type   = Entity_Type_String_Literal;
            ent.strlit = (AstStrLit *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_File_Contents: {
            ent.type = Entity_Type_File_Contents;
            ent.file_contents = (AstFileContents *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Struct_Type: {
            ent.type = Entity_Type_Struct_Member_Default;
            ent.type_alias = (AstType *) node;
            ENTITY_INSERT(ent);
            // fallthrough
        }
        
        case Ast_Kind_Poly_Struct_Type:
        case Ast_Kind_Type_Alias: {
            ent.type = Entity_Type_Type_Alias;
            ent.type_alias = (AstType *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Enum_Type: {
            ent.type = Entity_Type_Enum;
            ent.enum_type = (AstEnumType *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Use: {
            if (((AstUse *) node)->expr->kind == Ast_Kind_Package) {
                ent.state = Entity_State_Comptime_Resolve_Symbols;
                ent.type = Entity_Type_Use_Package;
            } else {
                ent.type = Entity_Type_Use;
            }

            ent.use = (AstUse *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Memres: {
            ent.type = Entity_Type_Memory_Reservation_Type;
            ent.mem_res = (AstMemRes *) node;
            ENTITY_INSERT(ent);
            
            ent.type = Entity_Type_Memory_Reservation;
            ent.mem_res = (AstMemRes *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        case Ast_Kind_Polymorphic_Proc: {
            ent.type = Entity_Type_Polymorphic_Proc;
            ent.poly_proc = (AstPolyProc *) node;
            ENTITY_INSERT(ent);
            break;
        }

        case Ast_Kind_Static_If: {
            ent.state = Entity_State_Comptime_Resolve_Symbols;
            ent.type = Entity_Type_Static_If;
            ent.static_if = (AstStaticIf *) node;
            ENTITY_INSERT(ent);
            break;
        }

        case Ast_Kind_Directive_Error: {
            ent.state = Entity_State_Error;
            ent.type = Entity_Type_Error;
            ent.error = (AstDirectiveError *) node;
            ENTITY_INSERT(ent);
            break;   
        }

        case Ast_Kind_Directive_Export:
        case Ast_Kind_Directive_Add_Overload:
        case Ast_Kind_Directive_Operator: {
            ent.type = Entity_Type_Process_Directive;
            ent.expr = (AstTyped *) node;
            ENTITY_INSERT(ent);
            break;
        }
        
        default: {
            ent.type = Entity_Type_Expression;
            ent.expr = (AstTyped *) node;
            ENTITY_INSERT(ent);
            break;
        }
    }
}
