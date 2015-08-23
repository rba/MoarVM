#include "moar.h"
#include "internal.h"
#include "platform/mmap.h"


void MVM_jit_compiler_init(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *jg);
void MVM_jit_compiler_deinit(MVMThreadContext *tc, MVMJitCompiler *compiler);
MVMJitCode * MVM_jit_compiler_assemble(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *jg);
void MVM_jit_compile_expr_tree(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *graph, MVMJitExprTree *tree);

#define COPY_ARRAY(a, n) memcpy(MVM_malloc(n * sizeof(a[0])), a, n * sizeof(a[0]))

static const MVMuint16 MAGIC_BYTECODE[] = { MVM_OP_sp_jit_enter, 0 };

void MVM_jit_compiler_init(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitGraph *jg) {
    MVMint32  num_globals = MVM_jit_num_globals();
    /* Create dasm state */
    dasm_init(cl, 1);
    cl->dasm_globals = MVM_malloc(num_globals * sizeof(void*));
    dasm_setupglobal(cl, cl->dasm_globals, num_globals);
    dasm_setup(cl, MVM_jit_actions());
    /* Store graph we're compiling */
    cl->graph      = jg;
    /* next (internal) label to assign */
    cl->next_label = jg->labels_num;
    cl->label_max  = jg->labels_num + 8;
    /* space for dynamic labels */
    dasm_growpc(cl, cl->label_max);
}

void MVM_jit_compiler_deinit(MVMThreadContext *tc, MVMJitCompiler *cl) {
    MVM_free(cl->dasm_globals);
    dasm_free(cl);
}

MVMJitCode * MVM_jit_compile_graph(MVMThreadContext *tc, MVMJitGraph *jg) {
    MVMJitCompiler cl;
    MVMJitCode *code;
    MVMJitNode *node = jg->first_node;

    MVM_jit_log(tc, "Starting compilation\n");
    /* initialation */
    MVM_jit_compiler_init(tc, &cl, jg);
    /* generate code */
    MVM_jit_emit_prologue(tc, &cl, jg);
    while (node) {
        switch(node->type) {
        case MVM_JIT_NODE_LABEL:
            MVM_jit_emit_label(tc, &cl, jg, node->u.label.name);
            break;
        case MVM_JIT_NODE_PRIMITIVE:
            MVM_jit_emit_primitive(tc, &cl, jg, &node->u.prim);
            break;
        case MVM_JIT_NODE_BRANCH:
            MVM_jit_emit_branch(tc, &cl, jg, &node->u.branch);
            break;
        case MVM_JIT_NODE_CALL_C:
            MVM_jit_emit_call_c(tc, &cl, jg, &node->u.call);
            break;
        case MVM_JIT_NODE_GUARD:
            MVM_jit_emit_guard(tc, &cl, jg, &node->u.guard);
            break;
        case MVM_JIT_NODE_INVOKE:
            MVM_jit_emit_invoke(tc, &cl, jg, &node->u.invoke);
            break;
        case MVM_JIT_NODE_JUMPLIST:
            MVM_jit_emit_jumplist(tc, &cl, jg, &node->u.jumplist);
            break;
        case MVM_JIT_NODE_CONTROL:
            MVM_jit_emit_control(tc, &cl, jg, &node->u.control);
            break;
        case MVM_JIT_NODE_EXPR_TREE:
            MVM_jit_compile_expr_tree(tc, &cl, jg, node->u.tree);
            break;
        }
        node = node->next;
    }
    MVM_jit_emit_epilogue(tc, &cl, jg);

    /* Generate code */
    code = MVM_jit_compiler_assemble(tc, &cl, jg);

    /* Clear up the compiler */
    MVM_jit_compiler_deinit(tc, &cl);

    /* Logging for insight */
    if (tc->instance->jit_bytecode_dir) {
        MVM_jit_log_bytecode(tc, code);
    }
    if (tc->instance->jit_log_fh)
        fflush(tc->instance->jit_log_fh);
    return code;
}

MVMJitCode * MVM_jit_compiler_assemble(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitGraph *jg) {
    MVMJitCode * code;
    MVMint32 i;
    char * memory;
    size_t codesize;

   /* compile the function */
    dasm_link(cl, &codesize);
    memory = MVM_platform_alloc_pages(codesize, MVM_PAGE_READ|MVM_PAGE_WRITE);
    dasm_encode(cl, memory);

    /* set memory readable + executable */
    MVM_platform_set_page_mode(memory, codesize, MVM_PAGE_READ|MVM_PAGE_EXEC);

    MVM_jit_log(tc, "Bytecode size: %"MVM_PRSz"\n", codesize);

    /* Create code segment */
    code = MVM_malloc(sizeof(MVMJitCode));
    code->func_ptr   = (void (*)(MVMThreadContext*,MVMCompUnit*,void*)) memory;
    code->size       = codesize;
    code->bytecode   = (MVMuint8*)MAGIC_BYTECODE;
    code->sf         = jg->sg->sf;

    /* Get the basic block labels */
    code->num_labels = jg->labels_num;
    code->labels = MVM_malloc(sizeof(void*) * code->num_labels);
    for (i = 0; i < code->num_labels; i++) {
        MVMint32 offset = dasm_getpclabel(cl, i);
        if (offset < 0)
            MVM_jit_log(tc, "Got negative offset for dynamic label %d\n", i);
        code->labels[i] = memory + offset;
    }

    /* Copy the deopts, inlines, and handlers. Because these use the
     * label index rather than the direct pointer, no fixup is
     * necessary */
    code->num_bbs      = jg->bbs_num;
    code->bb_labels    = COPY_ARRAY(jg->bbs, jg->bbs_num);

    code->num_deopts   = jg->deopts_num;
    code->deopts       = code->num_deopts ? COPY_ARRAY(jg->deopts, jg->deopts_num) : NULL;
    code->num_handlers = jg->handlers_num;
    code->handlers     = code->num_handlers ? COPY_ARRAY(jg->handlers, jg->handlers_alloc) : NULL;
    code->num_inlines  = jg->inlines_num;
    code->inlines      = code->num_inlines ? COPY_ARRAY(jg->inlines, jg->inlines_alloc) : NULL;

    return code;
}

void MVM_jit_destroy_code(MVMThreadContext *tc, MVMJitCode *code) {
    MVM_platform_free_pages(code->func_ptr, code->size);
    MVM_free(code->bb_labels);
    MVM_free(code->deopts);
    MVM_free(code->handlers);
    MVM_free(code->inlines);
    MVM_free(code);
}

/* Compile time labelling facility, as opposed to graph labels; these
 * don't need to be stored for access later */
static MVMint32 alloc_internal_label(MVMThreadContext *tc, MVMJitCompiler *cl, MVMint32 num) {
    MVMint32 next_label = cl->next_label;
    if (num + next_label >= cl->label_max) {
        /* Double the compile-time allocated labels */
        cl->label_max = cl->graph->labels_num + 2 * (cl->label_max - cl->graph->labels_num);
        dasm_growpc(cl, cl->label_max);
    }
    /* 'Allocate' num labels */
    cl->next_label += num;
    return next_label;
}



static void enter_conditional(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* Before entering a conditional, we should spill all we are going
     * to spill inside the conditional, unless on some future date
     * we're going to implement local spill-and-restore. But that's
     * relatively more complicated, so we don't. */
 }


static void leave_conditional(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* After leaving a conditional, if we ever choose to implement
     * spill-and-restore, we restore values here. More importantly,
     * values calculated within the conditional block are INVALIDATED
     * beyond that block, because you can't be sure they have been
     * calculated.
     *
     * NB: it is _values_ which are invalidated, and _registers_ which
     * are allocated and freed.
     * */

}


static void enter_call(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* Calls invalidate all caller-saved registers. Values that have
       not yet been spilled and are needed after the call will need to
       be spilled */
 }

static void leave_call(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* Post-call, we might implement restore, if the call was
       conditional. But that is something for another day */
}

static void ensure_register(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprValue *value) {
    /* Ensure that a value is loaded into a register */
    if (value->u.reg.num >= 0)
        return;
    MVM_jit_register_alloc(tc, cl, value);
    MVM_jit_emit_load(tc, cl, value);
}

static void release_registers(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* NYI */
}


static void prepare_tile(MVMThreadContext *tc, MVMJitTreeTraverser *traverser, MVMJitExprTree *tree, MVMint32 node) {
    MVMJitCompiler *cl = traverser->data;
    switch (tree->nodes[node]) {
    case MVM_JIT_WHEN:
        {
            /* Before we enter this node, mark us as entering a conditional */
            enter_conditional(tc, cl, tree, node);
            MVMint32 cond = tree->nodes[node+1];
            if (tree->nodes[cond] == MVM_JIT_ANY) {
                /* WHEN ANY requires two labels. One label for
                   skipping our block (tne normal WHEN label) and one
                   for entering it */
                tree->info[node].internal_label = alloc_internal_label(tc, cl, 2);
                tree->info[cond].internal_label = tree->info[node].internal_label;
            } else if (tree->nodes[cond] == MVM_JIT_ALL) {
                /* WHEN ALL requires just one label, because the
                 * short-circuit label of ALL is the same as the
                 * normal WHEN skip label */
                tree->info[node].internal_label = alloc_internal_label(tc, cl, 1);
                tree->info[cond].internal_label = tree->info[node].internal_label;
            } else {
                tree->info[node].internal_label = alloc_internal_label(tc, cl, 1);
            }
        }
        break;
    case MVM_JIT_IF:
    case MVM_JIT_EITHER:
        {
            MVMint32 cond = tree->nodes[node+1];
            enter_conditional(tc, cl, tree, node);
            if (tree->nodes[cond] == MVM_JIT_ALL)  {
                /* IF ALL short-circuits into the right block, which is simply labeled with the normal IF label */
                tree->info[node].internal_label = alloc_internal_label(tc, cl, 2);
                tree->info[cond].internal_label = tree->info[node].internal_label;
            } else if(tree->nodes[cond] == MVM_JIT_ANY) {
                /* IF ANY short-circuits into the left block, meaning I need an additional label */
                tree->info[node].internal_label = alloc_internal_label(tc, cl, 3);
                tree->info[cond].internal_label = tree->info[node].internal_label;
            } else {
                tree->info[node].internal_label = alloc_internal_label(tc, cl, 2);
            }
        }
        if (tree->nodes[node] == MVM_JIT_IF) {
            /* IF node yields either a left or right branch
               value. Preferably, we may achieve this without copying. */
            MVMint32 left  = tree->nodes[node+2];
            MVMint32 right = tree->nodes[node+3];
        }
        break;
    case MVM_JIT_ALL:
        {
            MVMint32 i, nchild = tree->nodes[node + 1], first_child = node + 2;
            /* Because of short-circuitng behavior, ALL is much like
             * nested WHEN, which means it's a conditional in
             * itself */
            enter_conditional(tc, cl, tree, node);
            for (i = 0; i < nchild; i++) {
                MVMint32 cond = tree->nodes[first_child+i];
                if (tree->nodes[cond] == MVM_JIT_ALL) {
                    /* Nested ALL short-circuits the same way as plain ALL */
                    tree->info[cond].internal_label = tree->info[node].internal_label;
                } else if (tree->nodes[cond] == MVM_JIT_ANY) {
                    /* Nested ANY short-circuits to continued evaluation if succesful,
                     * or out of ALL if not, thus we require a new label */
                    tree->info[cond].internal_label = alloc_internal_label(tc, cl, 1);
                }
            }
        }
        break;
    case MVM_JIT_ANY:
        {
            /* To deal with nested ANY/ALL combinations, we need to propagate and assign labels */
            MVMint32 i, nchild = tree->nodes[node + 1], first_child = node + 2;
            for (i = 0; i < nchild; i++) {
                MVMint32 cond = tree->nodes[first_child+i];
                if (tree->nodes[cond] == MVM_JIT_ALL) {
                    /* ANY ALL - if we reach the end of execution, that means ALL was valid,
                     * which implies the ANY should short-circuit. On the other hand, if ALL
                     * is not valid, it should jump beyond the short-circuit branch. Hence, we
                     * require a new label */
                    tree->info[cond].internal_label = alloc_internal_label(tc, cl, 1);
                } else if (tree->nodes[cond] == MVM_JIT_ANY) {
                    /* ANY ANY - is clearly equivalent to ANY */
                    tree->info[cond].internal_label = tree->info[node].internal_label;
                }
            }
        }
        break;
    case MVM_JIT_CALL:
        /* Before we prepare the entire call, be sure to spill values needed afterwards */
        enter_call(tc, cl, tree, node);
        break;
    default:
        break;
    }
}

/* Logical negation of MVMJitExprOp flags */
static enum MVMJitExprOp negate_flag(MVMThreadContext *tc, enum MVMJitExprOp op) {
    switch(op) {
    case MVM_JIT_LT:
        return MVM_JIT_GE;
    case MVM_JIT_LE:
        return MVM_JIT_GT;
    case MVM_JIT_EQ:
        return MVM_JIT_NE;
    case MVM_JIT_NE:
        return MVM_JIT_EQ;
    case MVM_JIT_GE:
        return MVM_JIT_LT;
    case MVM_JIT_GT:
        return MVM_JIT_LE;
    case MVM_JIT_NZ:
        return MVM_JIT_ZR;
    case MVM_JIT_ZR:
        return MVM_JIT_NZ;
    default:
        MVM_oops(tc, "Not a flag!");
    }
}

static void compile_labels(MVMThreadContext *tc, MVMJitTreeTraverser *traverser, MVMJitExprTree *tree, MVMint32 node, MVMint32 i) {
    MVMJitCompiler *cl = traverser->data;
    switch (tree->nodes[node]) {
    case MVM_JIT_WHEN:
        {
            MVMint32 cond  = tree->nodes[node+1];
            MVMint32 label = tree->info[node].internal_label;
            MVMint32 flag  = tree->nodes[cond];
            /* post-condition */
            if (i == 0) {
                if (flag == MVM_JIT_ALL) {
                    /* Do nothing, shortcircuit of ALL has skipped the
                       conditional block */
                } else if (flag == MVM_JIT_ANY) {
                    /* If ANY hasn't short-circuited into the
                       conditional block, jump beyond */
                    MVMJitBranch branch;
                    branch.ins  = NULL;
                    branch.dest = label + 1;
                    MVM_jit_emit_branch(tc, cl, cl->graph, &branch);
                    /* Emit label for the conditional block entry */
                    MVM_jit_emit_label(tc, cl, cl->graph, label);
                } else {
                    /* Other conditionals do not short-circuit, hence
                       require an explicit conditional branch */
                    MVM_jit_emit_conditional_branch(tc, cl, negate_flag(tc, flag), label);
                }
            } else {
                if (flag == MVM_JIT_ANY) {
                    /* WHEN ANY skip label is label + 1 because label
                       + 0 is necessary to enter the conditional
                       block */
                    MVM_jit_emit_label(tc, cl, cl->graph, label + 1);
                } else {
                    /* That's not true of any other condition */
                    MVM_jit_emit_label(tc, cl, cl->graph, label);
                }
            }
        }
        break;
    case MVM_JIT_IF:
    case MVM_JIT_EITHER:
        {
            MVMint32 cond  = tree->nodes[node+1];
            MVMint32 label = tree->info[node].internal_label;
            MVMint32 flag = tree->nodes[cond];
            if (i == 0) {
                if (flag == MVM_JIT_ALL) {
                    /* Like WHEN ALL, IF ALL short circuits into the
                       alternative block */
                } else if (flag == MVM_JIT_ANY) {
                    /* Like WHEN ANY, branch into the alternative
                     * block and emit a label for the conditional
                     * block */
                    MVMJitBranch branch;
                    branch.ins  = NULL;
                    branch.dest = label + 1;
                    MVM_jit_emit_branch(tc, cl, cl->graph, &branch);
                    MVM_jit_emit_label(tc, cl, cl->graph, label);
                } else {
                    MVM_jit_emit_conditional_branch(tc, cl, negate_flag(tc, flag), label);
                }
            } else if (i == 1) {
                if (flag == MVM_JIT_ANY) {
                    /* IF ANY offsets the branch label by one */
                    MVMJitBranch branch;
                    branch.ins   = NULL;
                    branch.dest = label + 2;
                    MVM_jit_emit_branch(tc, cl, cl->graph, &branch);
                    MVM_jit_emit_label(tc, cl, cl->graph, label + 1);
                } else {
                    MVMJitBranch branch;
                    branch.ins   = NULL;
                    branch.dest = label + 1;
                    MVM_jit_emit_branch(tc, cl, cl->graph, &branch);
                    MVM_jit_emit_label(tc, cl, cl->graph, label);
                }
            } else {
                if (flag == MVM_JIT_ANY) {
                    MVM_jit_emit_label(tc, cl, cl->graph, label + 2);
                } else {
                    MVM_jit_emit_label(tc, cl, cl->graph, label + 1);
                }
            }
        }
        break;
    case MVM_JIT_ALL:
        {
            MVMint32 cond = tree->nodes[node+2+i];
            MVMint32 label = tree->info[node].internal_label;
            if (tree->nodes[cond] == MVM_JIT_ALL) {
                /* Nested ALL short-circuits */
            } else if (tree->nodes[cond] == MVM_JIT_ANY) {
                /* If ANY reached it's end, that means it's false. So branch out */
                MVMJitBranch branch;
                branch.ins  = NULL;
                branch.dest = label;
                MVM_jit_emit_branch(tc, cl, cl->graph, &branch);
                /* And if ANY short-circuits we should continue the evaluation of ALL */
                MVM_jit_emit_label(tc, cl, cl->graph, tree->info[cond].internal_label);
            } else {
                /* Flag should be negated (if NOT condition we want to
                   short-circuit, otherwise we continue) */
                MVMint32 flag = negate_flag(tc, tree->nodes[cond]);
                MVM_jit_emit_conditional_branch(tc, cl, flag, label);
            }
        }
        break;
    case MVM_JIT_ANY:
        {
            MVMint32 cond  = tree->nodes[node+2+i];
            MVMint32 label = tree->info[node].internal_label;
            if (tree->nodes[cond] == MVM_JIT_ALL) {
                /* If ALL was succesful, we can branch out */
                MVMJitBranch branch;
                branch.ins  = NULL;
                branch.dest = label;
                MVM_jit_emit_branch(tc, cl, cl->graph, &branch);
                /* If not, it should short-circuit to continued evaluation */
                MVM_jit_emit_label(tc, cl, cl->graph, tree->info[cond].internal_label);
            } else if (tree->nodes[cond] == MVM_JIT_ANY) {
                /* Nothing to do here, since nested ANY already short-circuits */
            } else {
                /* Normal evaluation should short-circuit on truth values */
                MVM_jit_emit_conditional_branch(tc, cl, tree->nodes[cond], label);
            }
        }
        break;
    default:
        break;
    }

}




#if MVM_JIT_ARCH == MVM_JIT_ARCH_X64
/* Localized definitions winning! */
static MVMint8 x64_gpr_args[] = {
    X64_ARG_GPR(MVM_JIT_REGNAME)
};


static MVMint8 x64_sse_args[] = {
    X64_ARG_SSE(MVM_JIT_REGNAME)
};

#if MVM_JIT_PLATFORM == MVM_JIT_PLATFORM_POSIX
static void compile_arglist(MVMThreadContext *tc, MVMJitCompiler *compiler,
                            MVMJitExprTree *tree, MVMint32 node) {
    /* TODO implement this */
}
#else
static void compile_arglist(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                            MVMint32 node) {
    MVMint32 i, nchild = tree->nodes[node+1], first_child = node+2;
    /* TODO implement this too */
}
#endif

#else
/* No such architecture */
static void compile_arglist(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                            MVMint32 node) {
    MVM_oops(tc, "compile_arglist NYI for this architecture");
}
#endif


static void compile_tile(MVMThreadContext *tc, MVMJitTreeTraverser *traverser, MVMJitExprTree *tree, MVMint32 node) {
    MVMJitCompiler *cl = traverser->data;
    MVMJitExprNodeInfo *info = &tree->info[node];
    const MVMJitTile *tile   = info->tile;
    MVMJitExprValue *values[8];
    MVMint32 first_child = node + 1;
    MVMint32 nchild = info->op_info->nchild < 0 ? tree->nodes[first_child++] : info->op_info->nchild;
    MVMJitExprNode *args = tree->nodes + first_child + nchild;
    MVMint32 i;
    values[0] = &tree->info[node].value;
    switch (tree->nodes[node]) {
    case MVM_JIT_CALL:
        /* we should have a tile */
        if (tile == NULL || tile->rule == NULL) {
            MVM_oops(tc, "Tile without a rule!");
        }
        /* We don't really need preparation, compile_arglist did that */
        MVM_jit_tile_get_values(tc, tree, node, tile->path, values+1);
        tile->rule(tc, cl, tree, node, values, args);
        /* Update return value */
        if (args[0] == MVM_JIT_VOID) {
            values[0]->u.reg.num = -1;
        } else if (args[0] == MVM_JIT_NUM) {
            values[0]->u.reg.num = MVM_JIT_RETVAL_NUM;
            values[0]->u.reg.cls = MVM_JIT_REGCLS_NUM;
        } else {
            values[0]->u.reg.num = MVM_JIT_RETVAL_GPR;
            values[0]->u.reg.cls = MVM_JIT_REGCLS_GPR;
        }
        /* post call work */
        leave_call(tc, cl, tree, node);
        break;
    case MVM_JIT_ARGLIST:
        compile_arglist(tc, cl, tree, node);
        break;
    case MVM_JIT_WHEN:
    case MVM_JIT_EITHER:
        /* WHEN and EITHER do not yield a value */
        leave_conditional(tc, cl, tree, node);
        values[0]->u.reg.num = -1;
        break;
    case MVM_JIT_IF:
        /* IF does, though */
        {
            MVMint32 left = tree->nodes[node+2], right = tree->nodes[node+3];
            MVMint8 reg_a = tree->info[left].value.u.reg.num, reg_b = tree->info[right].value.u.reg.num;
            if (reg_a == reg_b) {
                /* happy case */
                values[0]->u.reg.num = reg_a;
            } else {
                /* not so happy case. emit a copy */
                MVM_jit_emit_copy(tc, reg_a, reg_b);
            }
            leave_conditional(tc, cl, tree, node);
        }
        break;
        default:
        {
            if (tile->rule == NULL)
                return;

            MVM_jit_tile_get_values(tc, tree, node, tile->path, values+1);

            for (i = 0; i < tile->num_values; i++) {
                /* TODO - ensure all values that are typed registers,
                 * are placed into registers! */
                if (values[i+1]->type == MVM_JIT_REG) {
                    ensure_register(tc, cl, values[i+1]);
                }
            }

            if (tile->vtype == MVM_JIT_REG) {
                /* allocate a register */
                if (values[1]->type == MVM_JIT_REG && values[1]->last_use == node) {
                    values[0]->u.reg.num = values[1]->u.reg.num;
                    values[0]->u.reg.cls = values[1]->u.reg.cls;
                    /* Free register so it can be reused */
                    MVM_jit_register_release(tc, cl, values[0]->u.reg.cls, values[0]->u.reg.num);
                } else {
                    values[0]->u.reg.num = MVM_jit_register_alloc(tc, cl, MVM_JIT_X64_GPR);
                    values[0]->u.reg.cls = MVM_JIT_X64_GPR;
                }
                MVM_jit_register_use(tc, cl, values[0]->u.reg.cls, values[0]->u.reg.num, node);
            }
            values[0]->type = tile->vtype;
            tile->rule(tc, cl, tree, node, values, args);
        }
    }
    /* TODO implement register release afterwards */
    release_registers(tc, cl, tree, node);
}

void MVM_jit_compile_expr_tree(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *jg, MVMJitExprTree *tree) {
    MVMJitTreeTraverser traverser;

    traverser.preorder  = &prepare_tile;
    traverser.inorder   = &compile_labels;
    traverser.postorder = &compile_tile;

    /* First stage, tile the tree */
    MVM_jit_tile_expr_tree(tc, tree);
    /* Second stage, compile the tree (and be done with it) */
    MVM_jit_expr_tree_traverse(tc, tree, &traverser);
}


/* Enter the JIT code segment. The label is a continuation point where control
 * is resumed after the frame is properly setup. */
void MVM_jit_enter_code(MVMThreadContext *tc, MVMCompUnit *cu,
                        MVMJitCode *code) {
    void *label = tc->cur_frame->jit_entry_label;
    code->func_ptr(tc, cu, label);
}
