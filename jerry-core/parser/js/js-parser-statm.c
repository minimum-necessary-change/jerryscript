/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "js-parser-internal.h"

#if ENABLED (JERRY_PARSER)
#include "jcontext.h"

#include "ecma-helpers.h"
#include "lit-char-helpers.h"

#if ENABLED (JERRY_ES2015_FOR_OF)
#if !ENABLED (JERRY_ES2015_BUILTIN_ITERATOR)
#error "For of support requires ES2015 iterator support"
#endif /* !ENABLED (JERRY_ES2015_BUILTIN_ITERATOR) */
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */


/** \addtogroup parser Parser
 * @{
 *
 * \addtogroup jsparser JavaScript
 * @{
 *
 * \addtogroup jsparser_stmt Statement parser
 * @{
 */

/**
 * @{
 * Strict mode string literal in directive prologues
 */
#define PARSER_USE_STRICT_LITERAL  "use strict"
#define PARSER_USE_STRICT_LENGTH   10
/** @} */

/**
 * Parser statement types.
 *
 * When a new statement is added, the following
 * functions may need to be updated as well:
 *
 *  - parser_statement_length()
 *  - parser_parse_break_statement()
 *  - parser_parse_continue_statement()
 *  - parser_free_jumps()
 *  - 'case LEXER_RIGHT_BRACE:' in parser_parse_statements()
 *  - 'if (context_p->token.type == LEXER_RIGHT_BRACE)' in parser_parse_statements()
 *  - 'switch (context_p->stack_top_uint8)' in parser_parse_statements()
 */
typedef enum
{
  PARSER_STATEMENT_START,
  PARSER_STATEMENT_BLOCK,
  PARSER_STATEMENT_LABEL,
  PARSER_STATEMENT_IF,
  PARSER_STATEMENT_ELSE,
  /* From switch -> for-in : break target statements */
  PARSER_STATEMENT_SWITCH,
  PARSER_STATEMENT_SWITCH_NO_DEFAULT,
  /* From do-while -> for->in : continue target statements */
  PARSER_STATEMENT_DO_WHILE,
  PARSER_STATEMENT_WHILE,
  PARSER_STATEMENT_FOR,
  /* From for->in -> try : instructions with context
   * Break and continue uses another instruction form
   * when crosses their borders. */
  PARSER_STATEMENT_FOR_IN,
#if ENABLED (JERRY_ES2015_FOR_OF)
  PARSER_STATEMENT_FOR_OF,
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
  PARSER_STATEMENT_WITH,
  PARSER_STATEMENT_TRY,
} parser_statement_type_t;

/**
 * Loop statement.
 */
typedef struct
{
  parser_branch_node_t *branch_list_p;    /**< list of breaks and continues targeting this statement */
} parser_loop_statement_t;

/**
 * Label statement.
 */
typedef struct
{
  lexer_lit_location_t label_ident;       /**< name of the label */
  parser_branch_node_t *break_list_p;     /**< list of breaks targeting this label */
} parser_label_statement_t;

/**
 * If/else statement.
 */
typedef struct
{
  parser_branch_t branch;                 /**< branch to the end */
} parser_if_else_statement_t;

/**
 * Switch statement.
 */
typedef struct
{
  parser_branch_t default_branch;         /**< branch to the default case */
  parser_branch_node_t *branch_list_p;    /**< branches of case statements */
} parser_switch_statement_t;

/**
 * Do-while statement.
 */
typedef struct
{
  uint32_t start_offset;                  /**< start byte code offset */
} parser_do_while_statement_t;

/**
 * While statement.
 */
typedef struct
{
  parser_branch_t branch;                 /**< branch to the end */
  lexer_range_t condition_range;          /**< condition part */
  uint32_t start_offset;                  /**< start byte code offset */
} parser_while_statement_t;

/**
 * For statement.
 */
typedef struct
{
  parser_branch_t branch;                 /**< branch to the end */
  lexer_range_t condition_range;          /**< condition part */
  lexer_range_t expression_range;         /**< increase part */
  uint32_t start_offset;                  /**< start byte code offset */
} parser_for_statement_t;

/**
 * For-in statement.
 */
typedef struct
{
  parser_branch_t branch;                 /**< branch to the end */
  uint32_t start_offset;                  /**< start byte code offset */
} parser_for_in_statement_t;

#if ENABLED (JERRY_ES2015_FOR_OF)
/**
 * For-of statement.
 */
typedef struct
{
  parser_branch_t branch;                 /**< branch to the end */
  uint32_t start_offset;                  /**< start byte code offset */
} parser_for_of_statement_t;
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */

/**
 * With statement.
 */
typedef struct
{
  parser_branch_t branch;                 /**< branch to the end */
} parser_with_statement_t;

/**
 * Lexer token types.
 */
typedef enum
{
  parser_try_block,                       /**< try block */
  parser_catch_block,                     /**< catch block */
  parser_finally_block,                   /**< finally block */
} parser_try_block_type_t;

/**
 * Try statement.
 */
typedef struct
{
  parser_try_block_type_t type;           /**< current block type */
  parser_branch_t branch;                 /**< branch to the end of the current block */
} parser_try_statement_t;

/**
 * Returns the data consumed by a statement. It can be used
 * to skip undesired frames on the stack during frame search.
 *
 * @return size consumed by a statement.
 */
static inline size_t
parser_statement_length (uint8_t type) /**< type of statement */
{
  static const uint8_t statement_lengths[] =
  {
    /* PARSER_STATEMENT_BLOCK */
    1,
    /* PARSER_STATEMENT_LABEL */
    (uint8_t) (sizeof (parser_label_statement_t) + 1),
    /* PARSER_STATEMENT_IF */
    (uint8_t) (sizeof (parser_if_else_statement_t) + 1),
    /* PARSER_STATEMENT_ELSE */
    (uint8_t) (sizeof (parser_if_else_statement_t) + 1),
    /* PARSER_STATEMENT_SWITCH */
    (uint8_t) (sizeof (parser_switch_statement_t) + sizeof (parser_loop_statement_t) + 1),
    /* PARSER_STATEMENT_SWITCH_NO_DEFAULT */
    (uint8_t) (sizeof (parser_switch_statement_t) + sizeof (parser_loop_statement_t) + 1),
    /* PARSER_STATEMENT_DO_WHILE */
    (uint8_t) (sizeof (parser_do_while_statement_t) + sizeof (parser_loop_statement_t) + 1),
    /* PARSER_STATEMENT_WHILE */
    (uint8_t) (sizeof (parser_while_statement_t) + sizeof (parser_loop_statement_t) + 1),
    /* PARSER_STATEMENT_FOR */
    (uint8_t) (sizeof (parser_for_statement_t) + sizeof (parser_loop_statement_t) + 1),
    /* PARSER_STATEMENT_FOR_IN */
    (uint8_t) (sizeof (parser_for_in_statement_t) + sizeof (parser_loop_statement_t) + 1),
#if ENABLED (JERRY_ES2015_FOR_OF)
    /* PARSER_STATEMENT_FOR_OF */
    (uint8_t) (sizeof (parser_for_of_statement_t) + sizeof (parser_loop_statement_t) + 1),
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
    /* PARSER_STATEMENT_WITH */
    (uint8_t) (sizeof (parser_with_statement_t) + 1),
    /* PARSER_STATEMENT_TRY */
    (uint8_t) (sizeof (parser_try_statement_t) + 1),
  };

  JERRY_ASSERT (type >= PARSER_STATEMENT_BLOCK && type <= PARSER_STATEMENT_TRY);

  return statement_lengths[type - PARSER_STATEMENT_BLOCK];
} /* parser_statement_length */

/**
 * Initialize a range from the current location.
 */
static inline void
parser_save_range (parser_context_t *context_p, /**< context */
                   lexer_range_t *range_p, /**< destination range */
                   const uint8_t *source_end_p) /**< source end */
{
  range_p->source_p = context_p->source_p;
  range_p->source_end_p = source_end_p;
  range_p->line = context_p->line;
  range_p->column = context_p->column;
} /* parser_save_range */

/**
 * Set the current location on the stack.
 */
static inline void
parser_set_range (parser_context_t *context_p, /**< context */
                  lexer_range_t *range_p) /**< destination range */
{
  context_p->source_p = range_p->source_p;
  context_p->source_end_p = range_p->source_end_p;
  context_p->line = range_p->line;
  context_p->column = range_p->column;
} /* parser_set_range */

/**
 * Initialize stack iterator.
 */
static inline void
parser_stack_iterator_init (parser_context_t *context_p, /**< context */
                            parser_stack_iterator_t *iterator) /**< iterator */
{
  iterator->current_p = context_p->stack.first_p;
  iterator->current_position = context_p->stack.last_position;
} /* parser_stack_iterator_init */

/**
 * Read the next byte from the stack.
 *
 * @return byte
 */
static inline uint8_t
parser_stack_iterator_read_uint8 (parser_stack_iterator_t *iterator) /**< iterator */
{
  JERRY_ASSERT (iterator->current_position > 0 && iterator->current_position <= PARSER_STACK_PAGE_SIZE);
  return iterator->current_p->bytes[iterator->current_position - 1];
} /* parser_stack_iterator_read_uint8 */

/**
 * Change last byte of the stack.
 */
static inline void
parser_stack_change_last_uint8 (parser_context_t *context_p, /**< context */
                                uint8_t new_value) /**< new value */
{
  parser_mem_page_t *page_p = context_p->stack.first_p;

  JERRY_ASSERT (page_p != NULL
                && context_p->stack_top_uint8 == page_p->bytes[context_p->stack.last_position - 1]);

  page_p->bytes[context_p->stack.last_position - 1] = new_value;
  context_p->stack_top_uint8 = new_value;
} /* parser_stack_change_last_uint8 */

/**
 * Parse expression enclosed in parens.
 */
static inline void
parser_parse_enclosed_expr (parser_context_t *context_p) /**< context */
{
  lexer_next_token (context_p);

  if (context_p->token.type != LEXER_LEFT_PAREN)
  {
    parser_raise_error (context_p, PARSER_ERR_LEFT_PAREN_EXPECTED);
  }

  lexer_next_token (context_p);
  parser_parse_expression (context_p, PARSE_EXPR);

  if (context_p->token.type != LEXER_RIGHT_PAREN)
  {
    parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_EXPECTED);
  }
  lexer_next_token (context_p);
} /* parser_parse_enclosed_expr */

/**
 * Parse var statement.
 */
static void
parser_parse_var_statement (parser_context_t *context_p) /**< context */
{
  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_VAR);

  while (true)
  {
    lexer_expect_identifier (context_p, LEXER_IDENT_LITERAL);
    JERRY_ASSERT (context_p->token.type == LEXER_LITERAL
                  && context_p->token.lit_location.type == LEXER_IDENT_LITERAL);

#if ENABLED (JERRY_DEBUGGER) || ENABLED (JERRY_LINE_INFO)
    parser_line_counter_t ident_line_counter = context_p->token.line;
#endif /* ENABLED (JERRY_DEBUGGER) || ENABLED (JERRY_LINE_INFO) */

    context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_VAR;

#if ENABLED (JERRY_ES2015_MODULE_SYSTEM)
    if (context_p->status_flags & PARSER_MODULE_STORE_IDENT)
    {
      context_p->module_identifier_lit_p = context_p->lit_object.literal_p;
      context_p->status_flags &= (uint32_t) ~(PARSER_MODULE_STORE_IDENT);
    }
#endif /* ENABLED (JERRY_ES2015_MODULE_SYSTEM) */

    lexer_next_token (context_p);

    if (context_p->token.type == LEXER_ASSIGN)
    {
#if ENABLED (JERRY_DEBUGGER)
      if ((JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
          && ident_line_counter != context_p->last_breakpoint_line)
      {
        parser_emit_cbc (context_p, CBC_BREAKPOINT_DISABLED);
        parser_flush_cbc (context_p);

        parser_append_breakpoint_info (context_p, JERRY_DEBUGGER_BREAKPOINT_LIST, ident_line_counter);

        context_p->last_breakpoint_line = ident_line_counter;
      }
#endif /* ENABLED (JERRY_DEBUGGER) */

#if ENABLED (JERRY_LINE_INFO)
      if (ident_line_counter != context_p->last_line_info_line)
      {
        parser_emit_line_info (context_p, ident_line_counter, false);
      }
#endif /* ENABLED (JERRY_LINE_INFO) */

      parser_emit_cbc_literal_from_token (context_p, CBC_PUSH_LITERAL);
      parser_parse_expression (context_p,
                               PARSE_EXPR_STATEMENT | PARSE_EXPR_NO_COMMA | PARSE_EXPR_HAS_LITERAL);
    }

    if (context_p->token.type != LEXER_COMMA)
    {
      break;
    }
  }
} /* parser_parse_var_statement */

/**
 * Parse function statement.
 */
static void
parser_parse_function_statement (parser_context_t *context_p) /**< context */
{
  uint32_t status_flags;
  lexer_literal_t *name_p;
  lexer_literal_t *literal_p;
  uint8_t no_reg_store;

  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_FUNCTION);

#if ENABLED (JERRY_DEBUGGER)
  parser_line_counter_t debugger_line = context_p->token.line;
  parser_line_counter_t debugger_column = context_p->token.column;
#endif /* ENABLED (JERRY_DEBUGGER) */

  lexer_expect_identifier (context_p, LEXER_IDENT_LITERAL);
  JERRY_ASSERT (context_p->token.type == LEXER_LITERAL
                && context_p->token.lit_location.type == LEXER_IDENT_LITERAL);

  if (context_p->lit_object.type == LEXER_LITERAL_OBJECT_ARGUMENTS)
  {
    context_p->status_flags |= PARSER_ARGUMENTS_NOT_NEEDED;
  }

  name_p = context_p->lit_object.literal_p;

#if ENABLED (JERRY_ES2015_MODULE_SYSTEM)
  if (context_p->status_flags & PARSER_MODULE_STORE_IDENT)
  {
    context_p->module_identifier_lit_p = context_p->lit_object.literal_p;
    context_p->status_flags &= (uint32_t) ~(PARSER_MODULE_STORE_IDENT);
  }
#endif /* ENABLED (JERRY_ES2015_MODULE_SYSTEM) */

  status_flags = PARSER_IS_FUNCTION | PARSER_IS_CLOSURE;
  if (context_p->lit_object.type != LEXER_LITERAL_OBJECT_ANY)
  {
    JERRY_ASSERT (context_p->lit_object.type == LEXER_LITERAL_OBJECT_EVAL
                  || context_p->lit_object.type == LEXER_LITERAL_OBJECT_ARGUMENTS);
    status_flags |= PARSER_HAS_NON_STRICT_ARG;
  }

#if ENABLED (JERRY_DEBUGGER)
  if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
  {
    jerry_debugger_send_string (JERRY_DEBUGGER_FUNCTION_NAME,
                                JERRY_DEBUGGER_NO_SUBTYPE,
                                name_p->u.char_p,
                                name_p->prop.length);

    /* Reset token position for the function. */
    context_p->token.line = debugger_line;
    context_p->token.column = debugger_column;
  }
#endif /* ENABLED (JERRY_DEBUGGER) */

  if (name_p->status_flags & LEXER_FLAG_INITIALIZED)
  {
    if (!(name_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT))
    {
      /* Overwrite the previous initialization. */
      ecma_compiled_code_t *compiled_code_p;

      literal_p = PARSER_GET_LITERAL ((size_t) (context_p->lit_object.index + 1));

      JERRY_ASSERT (literal_p->type == LEXER_FUNCTION_LITERAL
                    && literal_p->status_flags == 0);

      compiled_code_p = parser_parse_function (context_p, status_flags);
      util_free_literal (literal_p);

      literal_p->u.bytecode_p = compiled_code_p;
      lexer_next_token (context_p);
      return;
    }
  }
  else if (context_p->lit_object.index + 1 == context_p->literal_count)
  {
    /* The most common case: the literal is the last literal. */
    name_p->status_flags |= LEXER_FLAG_VAR | LEXER_FLAG_INITIALIZED;
    lexer_construct_function_object (context_p, status_flags);
    lexer_next_token (context_p);
    return;
  }

  /* Clone the literal at the end. */
  if (context_p->literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
  {
    parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
  }

  literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
  *literal_p = *name_p;
  no_reg_store = name_p->status_flags & (LEXER_FLAG_NO_REG_STORE | LEXER_FLAG_SOURCE_PTR);
  literal_p->status_flags = LEXER_FLAG_VAR | LEXER_FLAG_INITIALIZED | no_reg_store;

  name_p->type = LEXER_UNUSED_LITERAL;
  name_p->status_flags &= LEXER_FLAG_FUNCTION_ARGUMENT | LEXER_FLAG_SOURCE_PTR;
  /* Byte code references to this literal are
   * redirected to the newly allocated literal. */
  name_p->prop.index = context_p->literal_count;

  context_p->literal_count++;

  lexer_construct_function_object (context_p, status_flags);
  lexer_next_token (context_p);
} /* parser_parse_function_statement */

/**
 * Parse if statement (starting part).
 */
static void
parser_parse_if_statement_start (parser_context_t *context_p) /**< context */
{
  parser_if_else_statement_t if_statement;

  parser_parse_enclosed_expr (context_p);

  parser_emit_cbc_forward_branch (context_p,
                                  CBC_BRANCH_IF_FALSE_FORWARD,
                                  &if_statement.branch);

  parser_stack_push (context_p, &if_statement, sizeof (parser_if_else_statement_t));
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_IF);
  parser_stack_iterator_init (context_p, &context_p->last_statement);
} /* parser_parse_if_statement_start */

/**
 * Parse if statement (ending part).
 *
 * @return true  - if parsing an 'else' statement
 *         false - otherwise
 */
static bool
parser_parse_if_statement_end (parser_context_t *context_p) /**< context */
{
  parser_if_else_statement_t if_statement;
  parser_if_else_statement_t else_statement;
  parser_stack_iterator_t iterator;

  JERRY_ASSERT (context_p->stack_top_uint8 == PARSER_STATEMENT_IF);

  if (context_p->token.type != LEXER_KEYW_ELSE)
  {
    parser_stack_pop_uint8 (context_p);
    parser_stack_pop (context_p, &if_statement, sizeof (parser_if_else_statement_t));
    parser_stack_iterator_init (context_p, &context_p->last_statement);

    parser_set_branch_to_current_position (context_p, &if_statement.branch);

    return false;
  }

  parser_stack_change_last_uint8 (context_p, PARSER_STATEMENT_ELSE);
  parser_stack_iterator_init (context_p, &iterator);
  parser_stack_iterator_skip (&iterator, 1);
  parser_stack_iterator_read (&iterator, &if_statement, sizeof (parser_if_else_statement_t));

  parser_emit_cbc_forward_branch (context_p,
                                  CBC_JUMP_FORWARD,
                                  &else_statement.branch);

  parser_set_branch_to_current_position (context_p, &if_statement.branch);

  parser_stack_iterator_write (&iterator, &else_statement, sizeof (parser_if_else_statement_t));

  lexer_next_token (context_p);
  return true;
} /* parser_parse_if_statement_end */

/**
 * Parse with statement (starting part).
 */
static void
parser_parse_with_statement_start (parser_context_t *context_p) /**< context */
{
  parser_with_statement_t with_statement;

  if (context_p->status_flags & PARSER_IS_STRICT)
  {
    parser_raise_error (context_p, PARSER_ERR_WITH_NOT_ALLOWED);
  }

  parser_parse_enclosed_expr (context_p);

#ifndef JERRY_NDEBUG
  PARSER_PLUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_WITH_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

  context_p->status_flags |= PARSER_INSIDE_WITH | PARSER_LEXICAL_ENV_NEEDED;
  parser_emit_cbc_ext_forward_branch (context_p,
                                      CBC_EXT_WITH_CREATE_CONTEXT,
                                      &with_statement.branch);

  parser_stack_push (context_p, &with_statement, sizeof (parser_with_statement_t));
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_WITH);
  parser_stack_iterator_init (context_p, &context_p->last_statement);
} /* parser_parse_with_statement_start */

/**
 * Parse with statement (ending part).
 */
static void
parser_parse_with_statement_end (parser_context_t *context_p) /**< context */
{
  parser_with_statement_t with_statement;
  parser_stack_iterator_t iterator;

  JERRY_ASSERT (context_p->status_flags & PARSER_INSIDE_WITH);

  parser_stack_pop_uint8 (context_p);
  parser_stack_pop (context_p, &with_statement, sizeof (parser_with_statement_t));
  parser_stack_iterator_init (context_p, &context_p->last_statement);

  parser_flush_cbc (context_p);
  PARSER_MINUS_EQUAL_U16 (context_p->stack_depth, PARSER_WITH_CONTEXT_STACK_ALLOCATION);
#ifndef JERRY_NDEBUG
  PARSER_MINUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_WITH_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

  parser_emit_cbc (context_p, CBC_CONTEXT_END);
  parser_set_branch_to_current_position (context_p, &with_statement.branch);

  parser_stack_iterator_init (context_p, &iterator);

  while (true)
  {
    uint8_t type = parser_stack_iterator_read_uint8 (&iterator);

    if (type == PARSER_STATEMENT_START)
    {
      context_p->status_flags &= (uint32_t) ~PARSER_INSIDE_WITH;
      return;
    }

    if (type == PARSER_STATEMENT_WITH)
    {
      return;
    }

    parser_stack_iterator_skip (&iterator, parser_statement_length (type));
  }
} /* parser_parse_with_statement_end */

#if ENABLED (JERRY_ES2015_CLASS)
/**
 * Parse super class context like a with statement (starting part).
 */
void
parser_parse_super_class_context_start (parser_context_t *context_p) /**< context */
{
  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_EXTENDS
                || (context_p->status_flags & PARSER_CLASS_HAS_SUPER));
  parser_with_statement_t with_statement;

  if (context_p->token.type == LEXER_KEYW_EXTENDS)
  {
    lexer_next_token (context_p);

    /* NOTE: Currently there is no proper way to check whether the currently parsed expression
       is a valid lefthand-side expression or not, so we do not throw syntax error and parse
       the class extending value as an expression. */
    parser_parse_expression (context_p, PARSE_EXPR | PARSE_EXPR_NO_COMMA);
  }
  else
  {
    JERRY_ASSERT (context_p->status_flags & PARSER_CLASS_HAS_SUPER);
    parser_emit_cbc (context_p, CBC_PUSH_NULL);
    context_p->status_flags |= PARSER_CLASS_IMPLICIT_SUPER;
  }

#ifndef JERRY_NDEBUG
  PARSER_PLUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_SUPER_CLASS_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

  context_p->status_flags |= PARSER_CLASS_HAS_SUPER;
  parser_emit_cbc_ext_forward_branch (context_p,
                                      CBC_EXT_SUPER_CLASS_CREATE_CONTEXT,
                                      &with_statement.branch);

  parser_stack_push (context_p, &with_statement, sizeof (parser_with_statement_t));
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_WITH);
} /* parser_parse_super_class_context_start */

/**
 * Parse super class context like a with statement (ending part).
 */
void
parser_parse_super_class_context_end (parser_context_t *context_p, /**< context */
                                      bool is_statement) /**< true - if class is parsed as a statement
                                                          *   false - otherwise (as an expression) */
{
  parser_with_statement_t with_statement;
  parser_stack_pop_uint8 (context_p);
  parser_stack_pop (context_p, &with_statement, sizeof (parser_with_statement_t));

  parser_flush_cbc (context_p);
  PARSER_MINUS_EQUAL_U16 (context_p->stack_depth, PARSER_SUPER_CLASS_CONTEXT_STACK_ALLOCATION);
#ifndef JERRY_NDEBUG
  PARSER_MINUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_SUPER_CLASS_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

  if (is_statement)
  {
    parser_emit_cbc (context_p, CBC_CONTEXT_END);
  }
  else
  {
    parser_emit_cbc_ext (context_p, CBC_EXT_CLASS_EXPR_CONTEXT_END);
  }

  parser_set_branch_to_current_position (context_p, &with_statement.branch);
} /* parser_parse_super_class_context_end */
#endif /* ENABLED (JERRY_ES2015_CLASS) */

/**
 * Parse do-while statement (ending part).
 */
static void
parser_parse_do_while_statement_end (parser_context_t *context_p) /**< context */
{
  parser_loop_statement_t loop;

  JERRY_ASSERT (context_p->stack_top_uint8 == PARSER_STATEMENT_DO_WHILE);

  if (context_p->token.type != LEXER_KEYW_WHILE)
  {
    parser_raise_error (context_p, PARSER_ERR_WHILE_EXPECTED);
  }

  parser_stack_iterator_t iterator;
  parser_stack_iterator_init (context_p, &iterator);

  parser_stack_iterator_skip (&iterator, 1);
  parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));

  parser_set_continues_to_current_position (context_p, loop.branch_list_p);

  parser_parse_enclosed_expr (context_p);

  if (context_p->last_cbc_opcode != CBC_PUSH_FALSE)
  {
    cbc_opcode_t opcode = CBC_BRANCH_IF_TRUE_BACKWARD;
    if (context_p->last_cbc_opcode == CBC_LOGICAL_NOT)
    {
      context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
      opcode = CBC_BRANCH_IF_FALSE_BACKWARD;
    }
    else if (context_p->last_cbc_opcode == CBC_PUSH_TRUE)
    {
      context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
      opcode = CBC_JUMP_BACKWARD;
    }

    parser_do_while_statement_t do_while_statement;
    parser_stack_iterator_skip (&iterator, sizeof (parser_loop_statement_t));
    parser_stack_iterator_read (&iterator, &do_while_statement, sizeof (parser_do_while_statement_t));

    parser_emit_cbc_backward_branch (context_p, (uint16_t) opcode, do_while_statement.start_offset);
  }
  else
  {
    context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
  }

  parser_stack_pop (context_p, NULL, 1 + sizeof (parser_loop_statement_t) + sizeof (parser_do_while_statement_t));
  parser_stack_iterator_init (context_p, &context_p->last_statement);

  parser_set_breaks_to_current_position (context_p, loop.branch_list_p);
} /* parser_parse_do_while_statement_end */

/**
 * Parse while statement (starting part).
 */
static void
parser_parse_while_statement_start (parser_context_t *context_p) /**< context */
{
  parser_while_statement_t while_statement;
  parser_loop_statement_t loop;

  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_WHILE);
  lexer_next_token (context_p);

  if (context_p->token.type != LEXER_LEFT_PAREN)
  {
    parser_raise_error (context_p, PARSER_ERR_LEFT_PAREN_EXPECTED);
  }

  parser_emit_cbc_forward_branch (context_p, CBC_JUMP_FORWARD, &while_statement.branch);

  JERRY_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);
  while_statement.start_offset = context_p->byte_code_size;

  /* The conditional part is processed at the end. */
  parser_scan_until (context_p, &while_statement.condition_range, LEXER_RIGHT_PAREN);
  lexer_next_token (context_p);

  loop.branch_list_p = NULL;

  parser_stack_push (context_p, &while_statement, sizeof (parser_while_statement_t));
  parser_stack_push (context_p, &loop, sizeof (parser_loop_statement_t));
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_WHILE);
  parser_stack_iterator_init (context_p, &context_p->last_statement);
} /* parser_parse_while_statement_start */

/**
 * Parse while statement (ending part).
 */
static void JERRY_ATTR_NOINLINE
parser_parse_while_statement_end (parser_context_t *context_p) /**< context */
{
  parser_while_statement_t while_statement;
  parser_loop_statement_t loop;
  lexer_token_t current_token;
  lexer_range_t range;
  cbc_opcode_t opcode;

  JERRY_ASSERT (context_p->stack_top_uint8 == PARSER_STATEMENT_WHILE);

  parser_stack_iterator_t iterator;
  parser_stack_iterator_init (context_p, &iterator);

  parser_stack_iterator_skip (&iterator, 1);
  parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));
  parser_stack_iterator_skip (&iterator, sizeof (parser_loop_statement_t));
  parser_stack_iterator_read (&iterator, &while_statement, sizeof (parser_while_statement_t));

  parser_save_range (context_p, &range, context_p->source_end_p);
  current_token = context_p->token;

  parser_set_branch_to_current_position (context_p, &while_statement.branch);
  parser_set_continues_to_current_position (context_p, loop.branch_list_p);

  parser_set_range (context_p, &while_statement.condition_range);
  lexer_next_token (context_p);

  parser_parse_expression (context_p, PARSE_EXPR);
  if (context_p->token.type != LEXER_EOS)
  {
    parser_raise_error (context_p, PARSER_ERR_INVALID_EXPRESSION);
  }

  opcode = CBC_BRANCH_IF_TRUE_BACKWARD;
  if (context_p->last_cbc_opcode == CBC_LOGICAL_NOT)
  {
    context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
    opcode = CBC_BRANCH_IF_FALSE_BACKWARD;
  }
  else if (context_p->last_cbc_opcode == CBC_PUSH_TRUE)
  {
    context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
    opcode = CBC_JUMP_BACKWARD;
  }

  parser_stack_pop (context_p, NULL, 1 + sizeof (parser_loop_statement_t) + sizeof (parser_while_statement_t));
  parser_stack_iterator_init (context_p, &context_p->last_statement);

  parser_emit_cbc_backward_branch (context_p, (uint16_t) opcode, while_statement.start_offset);
  parser_set_breaks_to_current_position (context_p, loop.branch_list_p);

  parser_set_range (context_p, &range);
  context_p->token = current_token;
} /* parser_parse_while_statement_end */

/**
 * Check whether the opcode is a valid LeftHandSide expression
 * and convert it back to an assignment.
 *
 * @return the compatible assignment opcode
 */
static uint16_t
parser_check_left_hand_side_expression (parser_context_t *context_p, /**< context */
                                        uint16_t opcode) /**< opcode to check */
{
  if (opcode == CBC_PUSH_LITERAL
      && context_p->last_cbc.literal_type == LEXER_IDENT_LITERAL)
  {
    context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
    return CBC_ASSIGN_SET_IDENT;
  }
  else if (opcode == CBC_PUSH_PROP)
  {
    context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
    return CBC_ASSIGN;
  }
  else if (opcode == CBC_PUSH_PROP_LITERAL)
  {
    context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
    return CBC_ASSIGN_PROP_LITERAL;
  }
  else if (opcode == CBC_PUSH_PROP_LITERAL_LITERAL)
  {
    context_p->last_cbc_opcode = CBC_PUSH_TWO_LITERALS;
    return CBC_ASSIGN;
  }
  else if (opcode == CBC_PUSH_PROP_THIS_LITERAL)
  {
    context_p->last_cbc_opcode = CBC_PUSH_THIS_LITERAL;
    return CBC_ASSIGN;
  }
  else
  {
    /* Invalid LeftHandSide expression. */
    parser_emit_cbc_ext (context_p, CBC_EXT_THROW_REFERENCE_ERROR);
    return CBC_ASSIGN;
  }

  return opcode;
} /* parser_check_left_hand_side_expression */

/**
 * Parse for statement (starting part).
 */
static void
parser_parse_for_statement_start (parser_context_t *context_p) /**< context */
{
  parser_loop_statement_t loop;
  lexer_range_t start_range;

  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_FOR);
  lexer_next_token (context_p);

  if (context_p->token.type != LEXER_LEFT_PAREN)
  {
    parser_raise_error (context_p, PARSER_ERR_LEFT_PAREN_EXPECTED);
  }

#if ENABLED (JERRY_ES2015_FOR_OF)
  lexer_token_type_t scan_token = LEXER_FOR_IN_OF;
#else /* !ENABLED (JERRY_ES2015_FOR_OF) */
  lexer_token_type_t scan_token = LEXER_KEYW_IN;
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */

  parser_scan_until (context_p, &start_range, scan_token);

  if (context_p->token.type == LEXER_KEYW_IN)
  {
    parser_for_in_statement_t for_in_statement;
    lexer_range_t range;

    lexer_next_token (context_p);
    parser_parse_expression (context_p, PARSE_EXPR);

    if (context_p->token.type != LEXER_RIGHT_PAREN)
    {
      parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_EXPECTED);
    }

#ifndef JERRY_NDEBUG
    PARSER_PLUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

    parser_emit_cbc_ext_forward_branch (context_p,
                                        CBC_EXT_FOR_IN_CREATE_CONTEXT,
                                        &for_in_statement.branch);

    JERRY_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);
    for_in_statement.start_offset = context_p->byte_code_size;

    parser_save_range (context_p, &range, context_p->source_end_p);
    parser_set_range (context_p, &start_range);
    lexer_next_token (context_p);

    if (context_p->token.type == LEXER_KEYW_VAR)
    {
      uint16_t literal_index;

      lexer_expect_identifier (context_p, LEXER_IDENT_LITERAL);
      JERRY_ASSERT (context_p->token.type == LEXER_LITERAL
                    && context_p->token.lit_location.type == LEXER_IDENT_LITERAL);

      context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_VAR;

      literal_index = context_p->lit_object.index;

      lexer_next_token (context_p);

      if (context_p->token.type == LEXER_ASSIGN)
      {
        parser_branch_t branch;

        /* Initialiser is never executed. */
        parser_emit_cbc_forward_branch (context_p, CBC_JUMP_FORWARD, &branch);
        lexer_next_token (context_p);
        parser_parse_expression (context_p,
                                 PARSE_EXPR_STATEMENT | PARSE_EXPR_NO_COMMA);
        parser_set_branch_to_current_position (context_p, &branch);
      }

      parser_emit_cbc_ext (context_p, CBC_EXT_FOR_IN_GET_NEXT);
      parser_emit_cbc_literal (context_p, CBC_ASSIGN_SET_IDENT, literal_index);
    }
    else
    {
      uint16_t opcode;

      parser_parse_expression (context_p, PARSE_EXPR);

      opcode = context_p->last_cbc_opcode;

      /* The CBC_EXT_FOR_IN_CREATE_CONTEXT flushed the opcode combiner. */
      JERRY_ASSERT (opcode != CBC_PUSH_TWO_LITERALS
                    && opcode != CBC_PUSH_THREE_LITERALS);

      opcode = parser_check_left_hand_side_expression (context_p, opcode);

      parser_emit_cbc_ext (context_p, CBC_EXT_FOR_IN_GET_NEXT);
      parser_flush_cbc (context_p);

      context_p->last_cbc_opcode = opcode;
    }

    if (context_p->token.type != LEXER_EOS)
    {
      parser_raise_error (context_p, PARSER_ERR_IN_EXPECTED);
    }

    parser_flush_cbc (context_p);
    parser_set_range (context_p, &range);
    lexer_next_token (context_p);

    loop.branch_list_p = NULL;

    parser_stack_push (context_p, &for_in_statement, sizeof (parser_for_in_statement_t));
    parser_stack_push (context_p, &loop, sizeof (parser_loop_statement_t));
    parser_stack_push_uint8 (context_p, PARSER_STATEMENT_FOR_IN);
    parser_stack_iterator_init (context_p, &context_p->last_statement);
  }
#if ENABLED (JERRY_ES2015_FOR_OF)
  else if (context_p->token.type == LEXER_LITERAL_OF)
  {
    parser_for_of_statement_t for_of_statement;
    lexer_range_t range;

    lexer_next_token (context_p);
    parser_parse_expression (context_p, PARSE_EXPR);

    if (context_p->token.type != LEXER_RIGHT_PAREN)
    {
      parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_EXPECTED);
    }

#ifndef JERRY_NDEBUG
    PARSER_PLUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_FOR_OF_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

    parser_emit_cbc_ext_forward_branch (context_p,
                                        CBC_EXT_FOR_OF_CREATE_CONTEXT,
                                        &for_of_statement.branch);

    JERRY_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);
    for_of_statement.start_offset = context_p->byte_code_size;

    parser_save_range (context_p, &range, context_p->source_end_p);
    parser_set_range (context_p, &start_range);
    lexer_next_token (context_p);

    if (context_p->token.type == LEXER_KEYW_VAR)
    {
      uint16_t literal_index;

      lexer_expect_identifier (context_p, LEXER_IDENT_LITERAL);
      JERRY_ASSERT (context_p->token.type == LEXER_LITERAL
                    && context_p->token.lit_location.type == LEXER_IDENT_LITERAL);

      context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_VAR;

      literal_index = context_p->lit_object.index;

      lexer_next_token (context_p);

      if (context_p->token.type == LEXER_ASSIGN)
      {
        parser_branch_t branch;

        /* Initialiser is never executed. */
        parser_emit_cbc_forward_branch (context_p, CBC_JUMP_FORWARD, &branch);
        lexer_next_token (context_p);
        parser_parse_expression (context_p,
                                 PARSE_EXPR_STATEMENT | PARSE_EXPR_NO_COMMA);
        parser_set_branch_to_current_position (context_p, &branch);
      }

      parser_emit_cbc_ext (context_p, CBC_EXT_FOR_OF_GET_NEXT);
      parser_emit_cbc_literal (context_p, CBC_ASSIGN_SET_IDENT, literal_index);
    }
    else
    {
      uint16_t opcode;

      parser_parse_expression (context_p, PARSE_EXPR);

      opcode = context_p->last_cbc_opcode;

      /* The CBC_EXT_FOR_OF_CREATE_CONTEXT flushed the opcode combiner. */
      JERRY_ASSERT (opcode != CBC_PUSH_TWO_LITERALS
                    && opcode != CBC_PUSH_THREE_LITERALS);

      opcode = parser_check_left_hand_side_expression (context_p, opcode);

      parser_emit_cbc_ext (context_p, CBC_EXT_FOR_OF_GET_NEXT);
      parser_flush_cbc (context_p);

      context_p->last_cbc_opcode = opcode;
    }

    if (context_p->token.type != LEXER_EOS)
    {
      parser_raise_error (context_p, PARSER_ERR_OF_EXPECTED);
    }

    parser_flush_cbc (context_p);
    parser_set_range (context_p, &range);
    lexer_next_token (context_p);

    loop.branch_list_p = NULL;

    parser_stack_push (context_p, &for_of_statement, sizeof (parser_for_of_statement_t));
    parser_stack_push (context_p, &loop, sizeof (parser_loop_statement_t));
    parser_stack_push_uint8 (context_p, PARSER_STATEMENT_FOR_OF);
    parser_stack_iterator_init (context_p, &context_p->last_statement);
  }
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
  else
  {
    parser_for_statement_t for_statement;

    start_range.source_end_p = context_p->source_end_p;
    parser_set_range (context_p, &start_range);
    lexer_next_token (context_p);

    if (context_p->token.type != LEXER_SEMICOLON)
    {
      if (context_p->token.type == LEXER_KEYW_VAR)
      {
        parser_parse_var_statement (context_p);
      }
      else
      {
        parser_parse_expression (context_p, PARSE_EXPR_STATEMENT);
      }

      if (context_p->token.type != LEXER_SEMICOLON)
      {
        parser_raise_error (context_p, PARSER_ERR_SEMICOLON_EXPECTED);
      }
    }

    parser_emit_cbc_forward_branch (context_p, CBC_JUMP_FORWARD, &for_statement.branch);

    JERRY_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);
    for_statement.start_offset = context_p->byte_code_size;

    /* The conditional and expression parts are processed at the end. */
    parser_scan_until (context_p, &for_statement.condition_range, LEXER_SEMICOLON);
    parser_scan_until (context_p, &for_statement.expression_range, LEXER_RIGHT_PAREN);
    lexer_next_token (context_p);

    loop.branch_list_p = NULL;

    parser_stack_push (context_p, &for_statement, sizeof (parser_for_statement_t));
    parser_stack_push (context_p, &loop, sizeof (parser_loop_statement_t));
    parser_stack_push_uint8 (context_p, PARSER_STATEMENT_FOR);
    parser_stack_iterator_init (context_p, &context_p->last_statement);
  }
} /* parser_parse_for_statement_start */

/**
 * Parse for statement (ending part).
 */
static void JERRY_ATTR_NOINLINE
parser_parse_for_statement_end (parser_context_t *context_p) /**< context */
{
  parser_for_statement_t for_statement;
  parser_loop_statement_t loop;
  lexer_token_t current_token;
  lexer_range_t range;
  cbc_opcode_t opcode;

  JERRY_ASSERT (context_p->stack_top_uint8 == PARSER_STATEMENT_FOR);

  parser_stack_iterator_t iterator;
  parser_stack_iterator_init (context_p, &iterator);

  parser_stack_iterator_skip (&iterator, 1);
  parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));
  parser_stack_iterator_skip (&iterator, sizeof (parser_loop_statement_t));
  parser_stack_iterator_read (&iterator, &for_statement, sizeof (parser_for_statement_t));

  parser_save_range (context_p, &range, context_p->source_end_p);
  current_token = context_p->token;

  parser_set_range (context_p, &for_statement.expression_range);
  lexer_next_token (context_p);

  parser_set_continues_to_current_position (context_p, loop.branch_list_p);

  if (context_p->token.type != LEXER_EOS)
  {
    parser_parse_expression (context_p, PARSE_EXPR_STATEMENT);

    if (context_p->token.type != LEXER_EOS)
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_EXPRESSION);
    }
  }

  parser_set_branch_to_current_position (context_p, &for_statement.branch);

  parser_set_range (context_p, &for_statement.condition_range);
  lexer_next_token (context_p);

  if (context_p->token.type != LEXER_EOS)
  {
    parser_parse_expression (context_p, PARSE_EXPR);

    if (context_p->token.type != LEXER_EOS)
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_EXPRESSION);
    }

    opcode = CBC_BRANCH_IF_TRUE_BACKWARD;
    if (context_p->last_cbc_opcode == CBC_LOGICAL_NOT)
    {
      context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
      opcode = CBC_BRANCH_IF_FALSE_BACKWARD;
    }
    else if (context_p->last_cbc_opcode == CBC_PUSH_TRUE)
    {
      context_p->last_cbc_opcode = PARSER_CBC_UNAVAILABLE;
      opcode = CBC_JUMP_BACKWARD;
    }
  }
  else
  {
    opcode = CBC_JUMP_BACKWARD;
  }

  parser_stack_pop (context_p, NULL, 1 + sizeof (parser_loop_statement_t) + sizeof (parser_for_statement_t));
  parser_stack_iterator_init (context_p, &context_p->last_statement);

  parser_emit_cbc_backward_branch (context_p, (uint16_t) opcode, for_statement.start_offset);
  parser_set_breaks_to_current_position (context_p, loop.branch_list_p);

  parser_set_range (context_p, &range);
  context_p->token = current_token;
} /* parser_parse_for_statement_end */

/**
 * Parse switch statement (starting part).
 */
static void JERRY_ATTR_NOINLINE
parser_parse_switch_statement_start (parser_context_t *context_p) /**< context */
{
  parser_switch_statement_t switch_statement;
  parser_loop_statement_t loop;
  parser_stack_iterator_t iterator;
  lexer_range_t switch_body_start;
  lexer_range_t unused_range;
  bool switch_case_was_found;
  bool default_case_was_found;
  parser_branch_node_t *cases_p = NULL;

  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_SWITCH);

  parser_parse_enclosed_expr (context_p);

  if (context_p->token.type != LEXER_LEFT_BRACE)
  {
    parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_EXPECTED);
  }

  parser_save_range (context_p, &switch_body_start, context_p->source_end_p);
  lexer_next_token (context_p);

  if (context_p->token.type == LEXER_RIGHT_BRACE)
  {
    /* Unlikely case, but possible. */
    parser_emit_cbc (context_p, CBC_POP);
    parser_flush_cbc (context_p);
    parser_stack_push_uint8 (context_p, PARSER_STATEMENT_BLOCK);
    parser_stack_iterator_init (context_p, &context_p->last_statement);
    return;
  }

  if (context_p->token.type != LEXER_KEYW_CASE
      && context_p->token.type != LEXER_KEYW_DEFAULT)
  {
    parser_raise_error (context_p, PARSER_ERR_INVALID_SWITCH);
  }

  /* The reason of using an iterator is error management. If an error
   * occures, parser_free_jumps() free all data. However, the branches
   * created by parser_emit_cbc_forward_branch_item() would not be freed.
   * To free these branches, the current switch data is always stored
   * on the stack. If any change happens, this data is updated. Updates
   * are done using the iterator. */

  switch_statement.branch_list_p = NULL;
  loop.branch_list_p = NULL;

  parser_stack_push (context_p, &switch_statement, sizeof (parser_switch_statement_t));
  parser_stack_iterator_init (context_p, &iterator);
  parser_stack_push (context_p, &loop, sizeof (parser_loop_statement_t));
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_SWITCH);
  parser_stack_iterator_init (context_p, &context_p->last_statement);

  switch_case_was_found = false;
  default_case_was_found = false;

#if ENABLED (JERRY_LINE_INFO)
  uint32_t last_line_info_line = context_p->last_line_info_line;
#endif /* ENABLED (JERRY_LINE_INFO) */

  while (true)
  {
    parser_scan_until (context_p, &unused_range, LEXER_KEYW_CASE);

    if (context_p->token.type == LEXER_KEYW_DEFAULT)
    {
      if (default_case_was_found)
      {
        parser_raise_error (context_p, PARSER_ERR_MULTIPLE_DEFAULTS_NOT_ALLOWED);
      }

      lexer_next_token (context_p);
      if (context_p->token.type != LEXER_COLON)
      {
        parser_raise_error (context_p, PARSER_ERR_COLON_EXPECTED);
      }

      default_case_was_found = true;
    }
    else if (context_p->token.type == LEXER_KEYW_CASE
             || context_p->token.type == LEXER_RIGHT_BRACE)
    {
      if (switch_case_was_found)
      {
        parser_branch_node_t *new_case_p;
        uint16_t opcode = CBC_BRANCH_IF_STRICT_EQUAL;

        if (context_p->token.type != LEXER_KEYW_CASE)
        {
          /* We don't duplicate the value for the last case. */
          parser_emit_cbc (context_p, CBC_STRICT_EQUAL);
          opcode = CBC_BRANCH_IF_TRUE_FORWARD;
        }
        new_case_p = parser_emit_cbc_forward_branch_item (context_p, opcode, NULL);
        if (cases_p == NULL)
        {
          switch_statement.branch_list_p = new_case_p;
          parser_stack_iterator_write (&iterator, &switch_statement, sizeof (parser_switch_statement_t));
        }
        else
        {
          cases_p->next_p = new_case_p;
        }
        cases_p = new_case_p;
      }

      /* End of switch statement. */
      if (context_p->token.type == LEXER_RIGHT_BRACE)
      {
        break;
      }

      lexer_next_token (context_p);

#if ENABLED (JERRY_LINE_INFO)
      if (context_p->token.line != context_p->last_line_info_line)
      {
        parser_emit_line_info (context_p, context_p->token.line, true);
      }
#endif /* ENABLED (JERRY_LINE_INFO) */

      parser_parse_expression (context_p, PARSE_EXPR);

      if (context_p->token.type != LEXER_COLON)
      {
        parser_raise_error (context_p, PARSER_ERR_COLON_EXPECTED);
      }
      switch_case_was_found = true;
    }

    lexer_next_token (context_p);
  }

  JERRY_ASSERT (switch_case_was_found || default_case_was_found);

#if ENABLED (JERRY_LINE_INFO)
  context_p->last_line_info_line = last_line_info_line;
#endif /* ENABLED (JERRY_LINE_INFO) */

  if (!switch_case_was_found)
  {
    /* There was no case statement, so the expression result
     * of the switch must be popped from the stack */
    parser_emit_cbc (context_p, CBC_POP);
  }

  parser_emit_cbc_forward_branch (context_p, CBC_JUMP_FORWARD, &switch_statement.default_branch);
  parser_stack_iterator_write (&iterator, &switch_statement, sizeof (parser_switch_statement_t));

  if (!default_case_was_found)
  {
    parser_stack_change_last_uint8 (context_p, PARSER_STATEMENT_SWITCH_NO_DEFAULT);
  }

  parser_set_range (context_p, &switch_body_start);
  lexer_next_token (context_p);
} /* parser_parse_switch_statement_start */

/**
 * Parse try statement (ending part).
 */
static void
parser_parse_try_statement_end (parser_context_t *context_p) /**< context */
{
  parser_try_statement_t try_statement;
  parser_stack_iterator_t iterator;

  JERRY_ASSERT (context_p->stack_top_uint8 == PARSER_STATEMENT_TRY);

  parser_stack_iterator_init (context_p, &iterator);
  parser_stack_iterator_skip (&iterator, 1);
  parser_stack_iterator_read (&iterator, &try_statement, sizeof (parser_try_statement_t));

  lexer_next_token (context_p);

  if (try_statement.type == parser_finally_block)
  {
    parser_flush_cbc (context_p);
    PARSER_MINUS_EQUAL_U16 (context_p->stack_depth, PARSER_TRY_CONTEXT_STACK_ALLOCATION);
#ifndef JERRY_NDEBUG
    PARSER_MINUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_TRY_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

    parser_emit_cbc (context_p, CBC_CONTEXT_END);
    parser_set_branch_to_current_position (context_p, &try_statement.branch);
  }
  else
  {
    parser_set_branch_to_current_position (context_p, &try_statement.branch);

    if (try_statement.type == parser_catch_block)
    {
      if (context_p->token.type != LEXER_KEYW_FINALLY)
      {
        parser_flush_cbc (context_p);
        PARSER_MINUS_EQUAL_U16 (context_p->stack_depth, PARSER_TRY_CONTEXT_STACK_ALLOCATION);
#ifndef JERRY_NDEBUG
        PARSER_MINUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_TRY_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

        parser_emit_cbc (context_p, CBC_CONTEXT_END);
        parser_flush_cbc (context_p);
        try_statement.type = parser_finally_block;
      }
    }
    else if (try_statement.type == parser_try_block
             && context_p->token.type != LEXER_KEYW_CATCH
             && context_p->token.type != LEXER_KEYW_FINALLY)
    {
      parser_raise_error (context_p, PARSER_ERR_CATCH_FINALLY_EXPECTED);
    }
  }

  if (try_statement.type == parser_finally_block)
  {
    parser_stack_pop (context_p, NULL, (uint32_t) (sizeof (parser_try_statement_t) + 1));
    parser_stack_iterator_init (context_p, &context_p->last_statement);
    return;
  }

  if (context_p->token.type == LEXER_KEYW_CATCH)
  {
    uint16_t literal_index;

    lexer_next_token (context_p);

    if (context_p->token.type != LEXER_LEFT_PAREN)
    {
      parser_raise_error (context_p, PARSER_ERR_LEFT_PAREN_EXPECTED);
    }

    lexer_expect_identifier (context_p, LEXER_IDENT_LITERAL);
    JERRY_ASSERT (context_p->token.type == LEXER_LITERAL
                  && context_p->token.lit_location.type == LEXER_IDENT_LITERAL);

    context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
    context_p->status_flags |= PARSER_LEXICAL_ENV_NEEDED;

    literal_index = context_p->lit_object.index;

    lexer_next_token (context_p);

    if (context_p->token.type != LEXER_RIGHT_PAREN)
    {
      parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_EXPECTED);
    }

    lexer_next_token (context_p);

    if (context_p->token.type != LEXER_LEFT_BRACE)
    {
      parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_EXPECTED);
    }

    try_statement.type = parser_catch_block;
    parser_emit_cbc_ext_forward_branch (context_p,
                                        CBC_EXT_CATCH,
                                        &try_statement.branch);

    parser_emit_cbc_literal (context_p, CBC_ASSIGN_SET_IDENT, literal_index);
    parser_flush_cbc (context_p);
  }
  else
  {
    JERRY_ASSERT (context_p->token.type == LEXER_KEYW_FINALLY);

    lexer_next_token (context_p);

    if (context_p->token.type != LEXER_LEFT_BRACE)
    {
      parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_EXPECTED);
    }

    try_statement.type = parser_finally_block;
    parser_emit_cbc_ext_forward_branch (context_p,
                                        CBC_EXT_FINALLY,
                                        &try_statement.branch);
  }

  lexer_next_token (context_p);
  parser_stack_iterator_write (&iterator, &try_statement, sizeof (parser_try_statement_t));
} /* parser_parse_try_statement_end */

/**
 * Parse default statement.
 */
static void
parser_parse_default_statement (parser_context_t *context_p) /**< context */
{
  parser_stack_iterator_t iterator;
  parser_switch_statement_t switch_statement;

  if (context_p->stack_top_uint8 != PARSER_STATEMENT_SWITCH
      && context_p->stack_top_uint8 != PARSER_STATEMENT_SWITCH_NO_DEFAULT)
  {
    parser_raise_error (context_p, PARSER_ERR_DEFAULT_NOT_IN_SWITCH);
  }

  lexer_next_token (context_p);
  /* Already checked in parser_parse_switch_statement_start. */
  JERRY_ASSERT (context_p->token.type == LEXER_COLON);
  lexer_next_token (context_p);

  parser_stack_iterator_init (context_p, &iterator);
  parser_stack_iterator_skip (&iterator, 1 + sizeof (parser_loop_statement_t));
  parser_stack_iterator_read (&iterator, &switch_statement, sizeof (parser_switch_statement_t));

  parser_set_branch_to_current_position (context_p, &switch_statement.default_branch);
} /* parser_parse_default_statement */

/**
 * Parse case statement.
 */
static void
parser_parse_case_statement (parser_context_t *context_p) /**< context */
{
  parser_stack_iterator_t iterator;
  parser_switch_statement_t switch_statement;
  lexer_range_t dummy_range;
  parser_branch_node_t *branch_p;

  if (context_p->stack_top_uint8 != PARSER_STATEMENT_SWITCH
      && context_p->stack_top_uint8 != PARSER_STATEMENT_SWITCH_NO_DEFAULT)
  {
    parser_raise_error (context_p, PARSER_ERR_CASE_NOT_IN_SWITCH);
  }

  parser_scan_until (context_p, &dummy_range, LEXER_COLON);
  lexer_next_token (context_p);

  parser_stack_iterator_init (context_p, &iterator);
  parser_stack_iterator_skip (&iterator, 1 + sizeof (parser_loop_statement_t));
  parser_stack_iterator_read (&iterator, &switch_statement, sizeof (parser_switch_statement_t));

  /* Free memory after the case statement is found. */

  branch_p = switch_statement.branch_list_p;
  JERRY_ASSERT (branch_p != NULL);
  switch_statement.branch_list_p = branch_p->next_p;
  parser_stack_iterator_write (&iterator, &switch_statement, sizeof (parser_switch_statement_t));

  parser_set_branch_to_current_position (context_p, &branch_p->branch);
  parser_free (branch_p, sizeof (parser_branch_node_t));
} /* parser_parse_case_statement */

/**
 * Parse break statement.
 */
static void
parser_parse_break_statement (parser_context_t *context_p) /**< context */
{
  parser_stack_iterator_t iterator;
  cbc_opcode_t opcode = CBC_JUMP_FORWARD;

  lexer_next_token (context_p);
  parser_stack_iterator_init (context_p, &iterator);

  if (!(context_p->token.flags & LEXER_WAS_NEWLINE)
      && context_p->token.type == LEXER_LITERAL
      && context_p->token.lit_location.type == LEXER_IDENT_LITERAL)
  {
    /* The label with the same name is searched on the stack. */
    while (true)
    {
      uint8_t type = parser_stack_iterator_read_uint8 (&iterator);
      if (type == PARSER_STATEMENT_START)
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_BREAK_LABEL);
      }

      if (type == PARSER_STATEMENT_FOR_IN
#if ENABLED (JERRY_ES2015_FOR_OF)
          || type == PARSER_STATEMENT_FOR_OF
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
          || type == PARSER_STATEMENT_WITH
          || type == PARSER_STATEMENT_TRY)
      {
        opcode = CBC_JUMP_FORWARD_EXIT_CONTEXT;
      }

      if (type == PARSER_STATEMENT_LABEL)
      {
        parser_label_statement_t label_statement;

        parser_stack_iterator_skip (&iterator, 1);
        parser_stack_iterator_read (&iterator, &label_statement, sizeof (parser_label_statement_t));

        if (lexer_compare_identifier_to_current (context_p, &label_statement.label_ident))
        {
          label_statement.break_list_p = parser_emit_cbc_forward_branch_item (context_p,
                                                                              (uint16_t) opcode,
                                                                              label_statement.break_list_p);
          parser_stack_iterator_write (&iterator, &label_statement, sizeof (parser_label_statement_t));
          lexer_next_token (context_p);
          return;
        }
        parser_stack_iterator_skip (&iterator, sizeof (parser_label_statement_t));
      }
      else
      {
        parser_stack_iterator_skip (&iterator, parser_statement_length (type));
      }
    }
  }

  /* The first switch or loop statement is searched. */
  while (true)
  {
    uint8_t type = parser_stack_iterator_read_uint8 (&iterator);
    if (type == PARSER_STATEMENT_START)
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_BREAK);
    }

    if (type == PARSER_STATEMENT_FOR_IN
#if ENABLED (JERRY_ES2015_FOR_OF)
        || type == PARSER_STATEMENT_FOR_OF
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
        || type == PARSER_STATEMENT_WITH
        || type == PARSER_STATEMENT_TRY)
    {
      opcode = CBC_JUMP_FORWARD_EXIT_CONTEXT;
    }

    if (type == PARSER_STATEMENT_SWITCH
        || type == PARSER_STATEMENT_SWITCH_NO_DEFAULT
        || type == PARSER_STATEMENT_DO_WHILE
        || type == PARSER_STATEMENT_WHILE
        || type == PARSER_STATEMENT_FOR
#if ENABLED (JERRY_ES2015_FOR_OF)
        || type == PARSER_STATEMENT_FOR_OF
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
        || type == PARSER_STATEMENT_FOR_IN)
    {
      parser_loop_statement_t loop;

      parser_stack_iterator_skip (&iterator, 1);
      parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));
      loop.branch_list_p = parser_emit_cbc_forward_branch_item (context_p,
                                                                (uint16_t) opcode,
                                                                loop.branch_list_p);
      parser_stack_iterator_write (&iterator, &loop, sizeof (parser_loop_statement_t));
      return;
    }

    parser_stack_iterator_skip (&iterator, parser_statement_length (type));
  }
} /* parser_parse_break_statement */

/**
 * Parse continue statement.
 */
static void
parser_parse_continue_statement (parser_context_t *context_p) /**< context */
{
  parser_stack_iterator_t iterator;
  cbc_opcode_t opcode = CBC_JUMP_FORWARD;

  lexer_next_token (context_p);
  parser_stack_iterator_init (context_p, &iterator);

  if (!(context_p->token.flags & LEXER_WAS_NEWLINE)
      && context_p->token.type == LEXER_LITERAL
      && context_p->token.lit_location.type == LEXER_IDENT_LITERAL)
  {
    parser_stack_iterator_t loop_iterator;
    bool for_in_of_was_seen = false;

    loop_iterator.current_p = NULL;

    /* The label with the same name is searched on the stack. */
    while (true)
    {
      uint8_t type = parser_stack_iterator_read_uint8 (&iterator);

      if (type == PARSER_STATEMENT_START)
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_CONTINUE_LABEL);
      }

      /* Only those labels are checked, whose are label of a loop. */
      if (loop_iterator.current_p != NULL && type == PARSER_STATEMENT_LABEL)
      {
        parser_label_statement_t label_statement;

        parser_stack_iterator_skip (&iterator, 1);
        parser_stack_iterator_read (&iterator, &label_statement, sizeof (parser_label_statement_t));

        if (lexer_compare_identifier_to_current (context_p, &label_statement.label_ident))
        {
          parser_loop_statement_t loop;

          parser_stack_iterator_skip (&loop_iterator, 1);
          parser_stack_iterator_read (&loop_iterator, &loop, sizeof (parser_loop_statement_t));
          loop.branch_list_p = parser_emit_cbc_forward_branch_item (context_p,
                                                                    (uint16_t) opcode,
                                                                    loop.branch_list_p);
          loop.branch_list_p->branch.offset |= CBC_HIGHEST_BIT_MASK;
          parser_stack_iterator_write (&loop_iterator, &loop, sizeof (parser_loop_statement_t));
          lexer_next_token (context_p);
          return;
        }
        parser_stack_iterator_skip (&iterator, sizeof (parser_label_statement_t));
        continue;
      }

#if ENABLED (JERRY_ES2015_FOR_OF)
      bool is_for_in_of_statement = (type == PARSER_STATEMENT_FOR_IN) || (type == PARSER_STATEMENT_FOR_OF);
#else /* !ENABLED (JERRY_ES2015_FOR_OF) */
      bool is_for_in_of_statement = (type == PARSER_STATEMENT_FOR_IN);
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */

      if (type == PARSER_STATEMENT_WITH
          || type == PARSER_STATEMENT_TRY
          || for_in_of_was_seen)
      {
        opcode = CBC_JUMP_FORWARD_EXIT_CONTEXT;
      }
      else if (is_for_in_of_statement)
      {
        for_in_of_was_seen = true;
      }

      if (type == PARSER_STATEMENT_DO_WHILE
          || type == PARSER_STATEMENT_WHILE
          || type == PARSER_STATEMENT_FOR
#if ENABLED (JERRY_ES2015_FOR_OF)
          || type == PARSER_STATEMENT_FOR_OF
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
          || type == PARSER_STATEMENT_FOR_IN)
      {
        loop_iterator = iterator;
      }
      else
      {
        loop_iterator.current_p = NULL;
      }

      parser_stack_iterator_skip (&iterator, parser_statement_length (type));
    }
  }

  /* The first loop statement is searched. */
  while (true)
  {
    uint8_t type = parser_stack_iterator_read_uint8 (&iterator);
    if (type == PARSER_STATEMENT_START)
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_CONTINUE);
    }

    if (type == PARSER_STATEMENT_DO_WHILE
        || type == PARSER_STATEMENT_WHILE
        || type == PARSER_STATEMENT_FOR
#if ENABLED (JERRY_ES2015_FOR_OF)
        || type == PARSER_STATEMENT_FOR_OF
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
        || type == PARSER_STATEMENT_FOR_IN)
    {
      parser_loop_statement_t loop;

      parser_stack_iterator_skip (&iterator, 1);
      parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));
      loop.branch_list_p = parser_emit_cbc_forward_branch_item (context_p,
                                                                (uint16_t) opcode,
                                                                loop.branch_list_p);
      loop.branch_list_p->branch.offset |= CBC_HIGHEST_BIT_MASK;
      parser_stack_iterator_write (&iterator, &loop, sizeof (parser_loop_statement_t));
      return;
    }

    if (type == PARSER_STATEMENT_WITH
        || type == PARSER_STATEMENT_TRY)
    {
      opcode = CBC_JUMP_FORWARD_EXIT_CONTEXT;
    }

    parser_stack_iterator_skip (&iterator, parser_statement_length (type));
  }
} /* parser_parse_continue_statement */

#if ENABLED (JERRY_ES2015_MODULE_SYSTEM)
/**
 * Parse import statement.
 * Note: See 15.2.2
 */
static void
parser_parse_import_statement (parser_context_t *context_p) /**< parser context */
{
  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_IMPORT);

  parser_module_check_request_place (context_p);
  parser_module_context_init ();

  ecma_module_node_t module_node;
  memset (&module_node, 0, sizeof (ecma_module_node_t));
  context_p->module_current_node_p = &module_node;

  lexer_next_token (context_p);

  /* Check for a ModuleSpecifier*/
  if (context_p->token.type != LEXER_LITERAL
      || context_p->token.lit_location.type != LEXER_STRING_LITERAL)
  {
    if (!(context_p->token.type == LEXER_LEFT_BRACE
          || context_p->token.type == LEXER_MULTIPLY
          || (context_p->token.type == LEXER_LITERAL && context_p->token.lit_location.type == LEXER_IDENT_LITERAL)))
    {
      parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_MULTIPLY_LITERAL_EXPECTED);
    }

    if (context_p->token.type == LEXER_LITERAL)
    {
      /* Handle ImportedDefaultBinding */
      lexer_construct_literal_object (context_p, &context_p->token.lit_location, LEXER_IDENT_LITERAL);

      ecma_string_t *local_name_p = ecma_new_ecma_string_from_utf8 (context_p->lit_object.literal_p->u.char_p,
                                                                    context_p->lit_object.literal_p->prop.length);

      if (parser_module_check_duplicate_import (context_p, local_name_p))
      {
        ecma_deref_ecma_string (local_name_p);
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_IMPORT_BINDING);
      }

      ecma_string_t *import_name_p = ecma_get_magic_string (LIT_MAGIC_STRING_DEFAULT);
      parser_module_add_names_to_node (context_p, import_name_p, local_name_p);

      ecma_deref_ecma_string (local_name_p);
      ecma_deref_ecma_string (import_name_p);

      lexer_next_token (context_p);

      if (context_p->token.type != LEXER_COMMA
          && !lexer_compare_raw_identifier_to_current (context_p, "from", 4))
      {
        parser_raise_error (context_p, PARSER_ERR_FROM_COMMA_EXPECTED);
      }

      if (context_p->token.type == LEXER_COMMA)
      {
        lexer_next_token (context_p);
        if (context_p->token.type != LEXER_MULTIPLY
            && context_p->token.type != LEXER_LEFT_BRACE)
        {
          parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_MULTIPLY_EXPECTED);
        }
      }
    }

    if (context_p->token.type == LEXER_MULTIPLY)
    {
      /* NameSpaceImport*/
      lexer_next_token (context_p);
      if (context_p->token.type != LEXER_LITERAL
          || !lexer_compare_raw_identifier_to_current (context_p, "as", 2))
      {
        parser_raise_error (context_p, PARSER_ERR_AS_EXPECTED);
      }

      lexer_next_token (context_p);
      if (context_p->token.type != LEXER_LITERAL)
      {
        parser_raise_error (context_p, PARSER_ERR_IDENTIFIER_EXPECTED);
      }

      lexer_construct_literal_object (context_p, &context_p->token.lit_location, LEXER_IDENT_LITERAL);

      ecma_string_t *local_name_p = ecma_new_ecma_string_from_utf8 (context_p->lit_object.literal_p->u.char_p,
                                                                    context_p->lit_object.literal_p->prop.length);

      if (parser_module_check_duplicate_import (context_p, local_name_p))
      {
        ecma_deref_ecma_string (local_name_p);
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_IMPORT_BINDING);
      }

      ecma_string_t *import_name_p = ecma_get_magic_string (LIT_MAGIC_STRING_ASTERIX_CHAR);

      parser_module_add_names_to_node (context_p, import_name_p, local_name_p);
      ecma_deref_ecma_string (local_name_p);
      ecma_deref_ecma_string (import_name_p);

      lexer_next_token (context_p);
    }
    else if (context_p->token.type == LEXER_LEFT_BRACE)
    {
      /* Handle NamedImports */
      parser_module_parse_import_clause (context_p);
    }

    if (context_p->token.type != LEXER_LITERAL || !lexer_compare_raw_identifier_to_current (context_p, "from", 4))
    {
      parser_raise_error (context_p, PARSER_ERR_FROM_EXPECTED);
    }
    lexer_next_token (context_p);
  }

  parser_module_handle_module_specifier (context_p);
  parser_module_add_import_node_to_context (context_p);

  context_p->module_current_node_p = NULL;
} /* parser_parse_import_statement */

/**
 * Parse export statement.
 */
static void
parser_parse_export_statement (parser_context_t *context_p) /**< context */
{
  JERRY_ASSERT (context_p->token.type == LEXER_KEYW_EXPORT);

  parser_module_check_request_place (context_p);
  parser_module_context_init ();

  ecma_module_node_t module_node;
  memset (&module_node, 0, sizeof (ecma_module_node_t));
  context_p->module_current_node_p = &module_node;

  lexer_next_token (context_p);
  switch (context_p->token.type)
  {
    case LEXER_KEYW_DEFAULT:
    {
      lexer_range_t range;
      parser_save_range (context_p, &range, context_p->source_end_p);

      context_p->status_flags |= PARSER_MODULE_STORE_IDENT;

      lexer_next_token (context_p);
      if (context_p->token.type == LEXER_KEYW_CLASS)
      {
        context_p->status_flags |= PARSER_MODULE_DEFAULT_CLASS_OR_FUNC;
        parser_parse_class (context_p, true);
      }
      else if (context_p->token.type == LEXER_KEYW_FUNCTION)
      {
        context_p->status_flags |= PARSER_MODULE_DEFAULT_CLASS_OR_FUNC;
        parser_parse_function_statement (context_p);
      }
      else
      {
        /* Assignment expression */
        parser_set_range (context_p, &range);

        /* 15.2.3.5 Use the synthetic name '*default*' as the identifier. */
        lexer_construct_literal_object (context_p,
                                        (lexer_lit_location_t *) &lexer_default_literal,
                                        lexer_default_literal.type);
        context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_VAR;

        context_p->token.lit_location.type = LEXER_IDENT_LITERAL;
        parser_emit_cbc_literal_from_token (context_p, CBC_PUSH_LITERAL);

        context_p->module_identifier_lit_p = context_p->lit_object.literal_p;

        /* Fake an assignment to the default identifier */
        context_p->token.type = LEXER_ASSIGN;

        parser_parse_expression (context_p,
                                 PARSE_EXPR_STATEMENT | PARSE_EXPR_NO_COMMA | PARSE_EXPR_HAS_LITERAL);
      }

      ecma_string_t *name_p = ecma_new_ecma_string_from_utf8 (context_p->module_identifier_lit_p->u.char_p,
                                                              context_p->module_identifier_lit_p->prop.length);
      ecma_string_t *export_name_p = ecma_get_magic_string (LIT_MAGIC_STRING_DEFAULT);

      if (parser_module_check_duplicate_export (context_p, export_name_p))
      {
        ecma_deref_ecma_string (name_p);
        ecma_deref_ecma_string (export_name_p);
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_EXPORT_IDENTIFIER);
      }

      parser_module_add_names_to_node (context_p,
                                       export_name_p,
                                       name_p);
      ecma_deref_ecma_string (name_p);
      ecma_deref_ecma_string (export_name_p);
      break;
    }
    case LEXER_MULTIPLY:
    {
      lexer_next_token (context_p);
      if (!(context_p->token.type == LEXER_LITERAL
            && lexer_compare_raw_identifier_to_current (context_p, "from", 4)))
      {
        parser_raise_error (context_p, PARSER_ERR_FROM_EXPECTED);
      }

      lexer_next_token (context_p);
      parser_module_handle_module_specifier (context_p);
      break;
    }
    case LEXER_KEYW_VAR:
    {
      context_p->status_flags |= PARSER_MODULE_STORE_IDENT;
      parser_parse_var_statement (context_p);
      ecma_string_t *name_p = ecma_new_ecma_string_from_utf8 (context_p->module_identifier_lit_p->u.char_p,
                                                              context_p->module_identifier_lit_p->prop.length);

      if (parser_module_check_duplicate_export (context_p, name_p))
      {
        ecma_deref_ecma_string (name_p);
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_EXPORT_IDENTIFIER);
      }

      parser_module_add_names_to_node (context_p,
                                       name_p,
                                       name_p);
      ecma_deref_ecma_string (name_p);
      break;
    }
    case LEXER_KEYW_CLASS:
    {
      context_p->status_flags |= PARSER_MODULE_STORE_IDENT;
      parser_parse_class (context_p, true);
      ecma_string_t *name_p = ecma_new_ecma_string_from_utf8 (context_p->module_identifier_lit_p->u.char_p,
                                                              context_p->module_identifier_lit_p->prop.length);

      if (parser_module_check_duplicate_export (context_p, name_p))
      {
        ecma_deref_ecma_string (name_p);
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_EXPORT_IDENTIFIER);
      }

      parser_module_add_names_to_node (context_p,
                                       name_p,
                                       name_p);
      ecma_deref_ecma_string (name_p);
      break;
    }
    case LEXER_KEYW_FUNCTION:
    {
      context_p->status_flags |= PARSER_MODULE_STORE_IDENT;
      parser_parse_function_statement (context_p);
      ecma_string_t *name_p = ecma_new_ecma_string_from_utf8 (context_p->module_identifier_lit_p->u.char_p,
                                                              context_p->module_identifier_lit_p->prop.length);

      if (parser_module_check_duplicate_export (context_p, name_p))
      {
        ecma_deref_ecma_string (name_p);
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_EXPORT_IDENTIFIER);
      }

      parser_module_add_names_to_node (context_p,
                                       name_p,
                                       name_p);
      ecma_deref_ecma_string (name_p);
      break;
    }
    case LEXER_LEFT_BRACE:
    {
      parser_module_parse_export_clause (context_p);

      if (context_p->token.type == LEXER_LITERAL
          && lexer_compare_raw_identifier_to_current (context_p, "from", 4))
      {
        lexer_next_token (context_p);
        parser_module_handle_module_specifier (context_p);
      }
      break;
    }
    default:
    {
      parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_MULTIPLY_LITERAL_EXPECTED);
      break;
    }
  }

  context_p->status_flags &= (uint32_t) ~(PARSER_MODULE_DEFAULT_CLASS_OR_FUNC | PARSER_MODULE_STORE_IDENT);
  parser_module_add_export_node_to_context (context_p);
  context_p->module_current_node_p = NULL;
} /* parser_parse_export_statement */
#endif /* ENABLED (JERRY_ES2015_MODULE_SYSTEM) */

/**
 * Parse label statement.
 */
static void
parser_parse_label (parser_context_t *context_p) /**< context */
{
  parser_stack_iterator_t iterator;
  parser_label_statement_t label_statement;

  parser_stack_iterator_init (context_p, &iterator);

  while (true)
  {
    uint8_t type = parser_stack_iterator_read_uint8 (&iterator);
    if (type == PARSER_STATEMENT_START)
    {
      break;
    }

    if (type == PARSER_STATEMENT_LABEL)
    {
      parser_stack_iterator_skip (&iterator, 1);
      parser_stack_iterator_read (&iterator, &label_statement, sizeof (parser_label_statement_t));
      parser_stack_iterator_skip (&iterator, sizeof (parser_label_statement_t));

      if (lexer_compare_identifier_to_current (context_p, &label_statement.label_ident))
      {
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_LABEL);
      }
    }
    else
    {
      parser_stack_iterator_skip (&iterator, parser_statement_length (type));
    }
  }

  label_statement.label_ident = context_p->token.lit_location;
  label_statement.break_list_p = NULL;
  parser_stack_push (context_p, &label_statement, sizeof (parser_label_statement_t));
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_LABEL);
  parser_stack_iterator_init (context_p, &context_p->last_statement);
} /* parser_parse_label */

/**
 * Parse statements.
 */
void
parser_parse_statements (parser_context_t *context_p) /**< context */
{
  /* Statement parsing cannot be nested. */
  JERRY_ASSERT (context_p->last_statement.current_p == NULL);
  parser_stack_push_uint8 (context_p, PARSER_STATEMENT_START);
  parser_stack_iterator_init (context_p, &context_p->last_statement);

#if ENABLED (JERRY_DEBUGGER)
  /* Set lexical enviroment for the debugger. */
  if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
  {
    context_p->status_flags |= PARSER_LEXICAL_ENV_NEEDED;
    context_p->last_breakpoint_line = 0;
  }
#endif /* ENABLED (JERRY_DEBUGGER) */

#if ENABLED (JERRY_LINE_INFO)
  if (JERRY_CONTEXT (resource_name) != ECMA_VALUE_UNDEFINED)
  {
    parser_emit_cbc_ext (context_p, CBC_EXT_RESOURCE_NAME);
    parser_flush_cbc (context_p);
  }
  context_p->last_line_info_line = 0;
#endif /* ENABLED (JERRY_LINE_INFO) */

  while (context_p->token.type == LEXER_LITERAL
         && context_p->token.lit_location.type == LEXER_STRING_LITERAL)
  {
    lexer_lit_location_t lit_location;
    uint32_t status_flags = context_p->status_flags;

    JERRY_ASSERT (context_p->stack_depth == 0);

    lit_location = context_p->token.lit_location;

    if (lit_location.length == PARSER_USE_STRICT_LENGTH
        && !lit_location.has_escape
        && memcmp (PARSER_USE_STRICT_LITERAL, lit_location.char_p, PARSER_USE_STRICT_LENGTH) == 0)
    {
      context_p->status_flags |= PARSER_IS_STRICT;
    }

    lexer_next_token (context_p);

    if (context_p->token.type != LEXER_SEMICOLON
        && context_p->token.type != LEXER_RIGHT_BRACE
        && (!(context_p->token.flags & LEXER_WAS_NEWLINE)
            || LEXER_IS_BINARY_OP_TOKEN (context_p->token.type)
            || context_p->token.type == LEXER_LEFT_PAREN
            || context_p->token.type == LEXER_LEFT_SQUARE
            || context_p->token.type == LEXER_DOT))
    {
      /* The string is part of an expression statement. */
      context_p->status_flags = status_flags;

#if ENABLED (JERRY_DEBUGGER)
      if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
      {
        JERRY_ASSERT (context_p->last_breakpoint_line == 0);

        parser_emit_cbc (context_p, CBC_BREAKPOINT_DISABLED);
        parser_flush_cbc (context_p);

        parser_append_breakpoint_info (context_p, JERRY_DEBUGGER_BREAKPOINT_LIST, context_p->token.line);

        context_p->last_breakpoint_line = context_p->token.line;
      }
#endif /* ENABLED (JERRY_DEBUGGER) */
#if ENABLED (JERRY_LINE_INFO)
      parser_emit_line_info (context_p, context_p->token.line, false);
#endif /* ENABLED (JERRY_LINE_INFO) */

      lexer_construct_literal_object (context_p, &lit_location, LEXER_STRING_LITERAL);
      parser_emit_cbc_literal_from_token (context_p, CBC_PUSH_LITERAL);
      /* The extra_value is used for saving the token. */
      context_p->token.extra_value = context_p->token.type;
      context_p->token.type = LEXER_EXPRESSION_START;
      break;
    }

#if ENABLED (JERRY_PARSER_DUMP_BYTE_CODE)
    if (context_p->is_show_opcodes
        && !(status_flags & PARSER_IS_STRICT)
        && (context_p->status_flags & PARSER_IS_STRICT))
    {
      JERRY_DEBUG_MSG ("  Note: switch to strict mode\n\n");
    }
#endif /* ENABLED (JERRY_PARSER_DUMP_BYTE_CODE) */

    if (context_p->token.type == LEXER_SEMICOLON)
    {
      lexer_next_token (context_p);
    }

    /* The last directive prologue can be the result of the script. */
    if (!(context_p->status_flags & PARSER_IS_FUNCTION)
        && (context_p->token.type != LEXER_LITERAL
            || context_p->token.lit_location.type != LEXER_STRING_LITERAL))
    {
      lexer_construct_literal_object (context_p, &lit_location, LEXER_STRING_LITERAL);
      parser_emit_cbc_literal_from_token (context_p, CBC_PUSH_LITERAL);
      parser_emit_cbc (context_p, CBC_POP_BLOCK);
      parser_flush_cbc (context_p);
    }
  }

  if (context_p->status_flags & PARSER_IS_STRICT
      && context_p->status_flags & PARSER_HAS_NON_STRICT_ARG)
  {
    parser_raise_error (context_p, PARSER_ERR_NON_STRICT_ARG_DEFINITION);
  }

  while (context_p->token.type != LEXER_EOS
         || context_p->stack_top_uint8 != PARSER_STATEMENT_START)
  {
#ifndef JERRY_NDEBUG
    JERRY_ASSERT (context_p->stack_depth == context_p->context_stack_depth);
#endif /* !JERRY_NDEBUG */

#if ENABLED (JERRY_DEBUGGER)
    if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED
        && context_p->token.line != context_p->last_breakpoint_line
        && context_p->token.type != LEXER_SEMICOLON
        && context_p->token.type != LEXER_LEFT_BRACE
        && context_p->token.type != LEXER_RIGHT_BRACE
        && context_p->token.type != LEXER_KEYW_VAR
        && context_p->token.type != LEXER_KEYW_FUNCTION
        && context_p->token.type != LEXER_KEYW_CASE
        && context_p->token.type != LEXER_KEYW_DEFAULT)
    {
      parser_emit_cbc (context_p, CBC_BREAKPOINT_DISABLED);
      parser_flush_cbc (context_p);

      parser_append_breakpoint_info (context_p, JERRY_DEBUGGER_BREAKPOINT_LIST, context_p->token.line);

      context_p->last_breakpoint_line = context_p->token.line;
    }
#endif /* ENABLED (JERRY_DEBUGGER) */

#if ENABLED (JERRY_LINE_INFO)
    if (context_p->token.line != context_p->last_line_info_line
        && context_p->token.type != LEXER_SEMICOLON
        && context_p->token.type != LEXER_LEFT_BRACE
        && context_p->token.type != LEXER_RIGHT_BRACE
        && context_p->token.type != LEXER_KEYW_VAR
        && context_p->token.type != LEXER_KEYW_FUNCTION
        && context_p->token.type != LEXER_KEYW_CASE
        && context_p->token.type != LEXER_KEYW_DEFAULT)
    {
      parser_emit_line_info (context_p, context_p->token.line, true);
    }
#endif /* ENABLED (JERRY_LINE_INFO) */

    switch (context_p->token.type)
    {
      case LEXER_SEMICOLON:
      {
        break;
      }

      case LEXER_RIGHT_BRACE:
      {
        if (context_p->stack_top_uint8 == PARSER_STATEMENT_LABEL
            || context_p->stack_top_uint8 == PARSER_STATEMENT_IF
            || context_p->stack_top_uint8 == PARSER_STATEMENT_ELSE
            || context_p->stack_top_uint8 == PARSER_STATEMENT_DO_WHILE
            || context_p->stack_top_uint8 == PARSER_STATEMENT_WHILE
            || context_p->stack_top_uint8 == PARSER_STATEMENT_FOR
            || context_p->stack_top_uint8 == PARSER_STATEMENT_FOR_IN
#if ENABLED (JERRY_ES2015_FOR_OF)
            || context_p->stack_top_uint8 == PARSER_STATEMENT_FOR_OF
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
            || context_p->stack_top_uint8 == PARSER_STATEMENT_WITH)
        {
          parser_raise_error (context_p, PARSER_ERR_STATEMENT_EXPECTED);
        }
        break;
      }

      case LEXER_LEFT_BRACE:
      {
        parser_stack_push_uint8 (context_p, PARSER_STATEMENT_BLOCK);
        parser_stack_iterator_init (context_p, &context_p->last_statement);
        lexer_next_token (context_p);
        continue;
      }

      case LEXER_KEYW_VAR:
      {
        parser_parse_var_statement (context_p);
        break;
      }

#if ENABLED (JERRY_ES2015_CLASS)
      case LEXER_KEYW_CLASS:
      {
        parser_parse_class (context_p, true);
        continue;
      }
#endif /* ENABLED (JERRY_ES2015_CLASS) */

#if ENABLED (JERRY_ES2015_MODULE_SYSTEM)
      case LEXER_KEYW_IMPORT:
      {
        parser_parse_import_statement (context_p);
        break;
      }

      case LEXER_KEYW_EXPORT:
      {
        parser_parse_export_statement (context_p);
        break;
      }
#endif /* ENABLED (JERRY_ES2015_MODULE_SYSTEM) */

      case LEXER_KEYW_FUNCTION:
      {
        parser_parse_function_statement (context_p);
        continue;
      }

      case LEXER_KEYW_IF:
      {
        parser_parse_if_statement_start (context_p);
        continue;
      }

      case LEXER_KEYW_SWITCH:
      {
        parser_parse_switch_statement_start (context_p);
        continue;
      }

      case LEXER_KEYW_DO:
      {
        parser_do_while_statement_t do_while_statement;
        parser_loop_statement_t loop;

        JERRY_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);

        do_while_statement.start_offset = context_p->byte_code_size;
        loop.branch_list_p = NULL;

        parser_stack_push (context_p, &do_while_statement, sizeof (parser_do_while_statement_t));
        parser_stack_push (context_p, &loop, sizeof (parser_loop_statement_t));
        parser_stack_push_uint8 (context_p, PARSER_STATEMENT_DO_WHILE);
        parser_stack_iterator_init (context_p, &context_p->last_statement);
        lexer_next_token (context_p);
        continue;
      }

      case LEXER_KEYW_WHILE:
      {
        parser_parse_while_statement_start (context_p);
        continue;
      }

      case LEXER_KEYW_FOR:
      {
        parser_parse_for_statement_start (context_p);
        continue;
      }

      case LEXER_KEYW_WITH:
      {
        parser_parse_with_statement_start (context_p);
        continue;
      }

      case LEXER_KEYW_TRY:
      {
        parser_try_statement_t try_statement;

        lexer_next_token (context_p);

        if (context_p->token.type != LEXER_LEFT_BRACE)
        {
          parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_EXPECTED);
        }

#ifndef JERRY_NDEBUG
        PARSER_PLUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_TRY_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

        try_statement.type = parser_try_block;
        parser_emit_cbc_ext_forward_branch (context_p,
                                            CBC_EXT_TRY_CREATE_CONTEXT,
                                            &try_statement.branch);

        parser_stack_push (context_p, &try_statement, sizeof (parser_try_statement_t));
        parser_stack_push_uint8 (context_p, PARSER_STATEMENT_TRY);
        parser_stack_iterator_init (context_p, &context_p->last_statement);
        lexer_next_token (context_p);
        continue;
      }

      case LEXER_KEYW_DEFAULT:
      {
        parser_parse_default_statement (context_p);
        continue;
      }

      case LEXER_KEYW_CASE:
      {
        parser_parse_case_statement (context_p);
        continue;
      }

      case LEXER_KEYW_BREAK:
      {
        parser_parse_break_statement (context_p);
        break;
      }

      case LEXER_KEYW_CONTINUE:
      {
        parser_parse_continue_statement (context_p);
        break;
      }

      case LEXER_KEYW_THROW:
      {
        lexer_next_token (context_p);
        if (context_p->token.flags & LEXER_WAS_NEWLINE)
        {
          parser_raise_error (context_p, PARSER_ERR_EXPRESSION_EXPECTED);
        }
        parser_parse_expression (context_p, PARSE_EXPR);
        parser_emit_cbc (context_p, CBC_THROW);
        break;
      }

      case LEXER_KEYW_RETURN:
      {
        if (!(context_p->status_flags & PARSER_IS_FUNCTION))
        {
          parser_raise_error (context_p, PARSER_ERR_INVALID_RETURN);
        }

        lexer_next_token (context_p);

        if ((context_p->token.flags & LEXER_WAS_NEWLINE)
            || context_p->token.type == LEXER_SEMICOLON
            || context_p->token.type == LEXER_RIGHT_BRACE)
        {
#if ENABLED (JERRY_ES2015_CLASS)
          if (JERRY_UNLIKELY (PARSER_IS_CLASS_CONSTRUCTOR_SUPER (context_p->status_flags)))
          {
            if (context_p->status_flags & PARSER_CLASS_IMPLICIT_SUPER)
            {
              parser_emit_cbc (context_p, CBC_PUSH_THIS);
            }
            else
            {
              parser_emit_cbc_ext (context_p, CBC_EXT_PUSH_CONSTRUCTOR_THIS);
            }
            parser_emit_cbc (context_p, CBC_RETURN);
          }
          else
          {
#endif /* ENABLED (JERRY_ES2015_CLASS) */
            parser_emit_cbc (context_p, CBC_RETURN_WITH_BLOCK);
#if ENABLED (JERRY_ES2015_CLASS)
          }
#endif /* ENABLED (JERRY_ES2015_CLASS) */
          break;
        }

        parser_parse_expression (context_p, PARSE_EXPR);

        bool return_with_literal = (context_p->last_cbc_opcode == CBC_PUSH_LITERAL);
#if ENABLED (JERRY_ES2015_CLASS)
        return_with_literal = return_with_literal && !PARSER_IS_CLASS_CONSTRUCTOR_SUPER (context_p->status_flags);
#endif /* ENABLED (JERRY_ES2015_CLASS) */

        if (return_with_literal)
        {
          context_p->last_cbc_opcode = CBC_RETURN_WITH_LITERAL;
        }
        else
        {
#if ENABLED (JERRY_ES2015_CLASS)
          if (JERRY_UNLIKELY (PARSER_IS_CLASS_CONSTRUCTOR_SUPER (context_p->status_flags)))
          {
            parser_emit_cbc_ext (context_p, CBC_EXT_CONSTRUCTOR_RETURN);
          }
          else
          {
#endif /* ENABLED (JERRY_ES2015_CLASS) */
            parser_emit_cbc (context_p, CBC_RETURN);
#if ENABLED (JERRY_ES2015_CLASS)
          }
#endif /* ENABLED (JERRY_ES2015_CLASS) */
        }
        break;
      }

      case LEXER_KEYW_DEBUGGER:
      {
#if ENABLED (JERRY_DEBUGGER)
        /* This breakpoint location is not reported to the
         * debugger, so it is impossible to disable it. */
        if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
        {
          parser_emit_cbc (context_p, CBC_BREAKPOINT_ENABLED);
        }
#endif /* ENABLED (JERRY_DEBUGGER) */
        lexer_next_token (context_p);
        break;
      }
      case LEXER_LITERAL:
      {
        if (context_p->token.lit_location.type == LEXER_IDENT_LITERAL
            && lexer_check_next_character (context_p, LIT_CHAR_COLON))
        {
          parser_parse_label (context_p);
          lexer_next_token (context_p);
          JERRY_ASSERT (context_p->token.type == LEXER_COLON);
          lexer_next_token (context_p);
          continue;
        }
        /* FALLTHRU */
      }

      default:
      {
        int options = PARSE_EXPR_BLOCK;

        if (context_p->status_flags & PARSER_IS_FUNCTION)
        {
          options = PARSE_EXPR_STATEMENT;
        }

        if (context_p->token.type == LEXER_EXPRESSION_START)
        {
          /* Restore the token type form the extra_value. */
          context_p->token.type = context_p->token.extra_value;
          options |= PARSE_EXPR_HAS_LITERAL;
        }

        parser_parse_expression (context_p, options);
        break;
      }
    }

    parser_flush_cbc (context_p);

    if (context_p->token.type == LEXER_RIGHT_BRACE)
    {
      if (context_p->stack_top_uint8 == PARSER_STATEMENT_BLOCK)
      {
        parser_stack_pop_uint8 (context_p);
        parser_stack_iterator_init (context_p, &context_p->last_statement);
        lexer_next_token (context_p);
      }
      else if (context_p->stack_top_uint8 == PARSER_STATEMENT_SWITCH
               || context_p->stack_top_uint8 == PARSER_STATEMENT_SWITCH_NO_DEFAULT)
      {
        int has_default = (context_p->stack_top_uint8 == PARSER_STATEMENT_SWITCH);
        parser_loop_statement_t loop;
        parser_switch_statement_t switch_statement;

        parser_stack_pop_uint8 (context_p);
        parser_stack_pop (context_p, &loop, sizeof (parser_loop_statement_t));
        parser_stack_pop (context_p, &switch_statement, sizeof (parser_switch_statement_t));
        parser_stack_iterator_init (context_p, &context_p->last_statement);

        JERRY_ASSERT (switch_statement.branch_list_p == NULL);

        if (!has_default)
        {
          parser_set_branch_to_current_position (context_p, &switch_statement.default_branch);
        }

        parser_set_breaks_to_current_position (context_p, loop.branch_list_p);
        lexer_next_token (context_p);
      }
      else if (context_p->stack_top_uint8 == PARSER_STATEMENT_TRY)
      {
        parser_parse_try_statement_end (context_p);
      }
      else if (context_p->stack_top_uint8 == PARSER_STATEMENT_START)
      {
        if (context_p->status_flags & PARSER_IS_CLOSURE)
        {
          parser_stack_pop_uint8 (context_p);
          context_p->last_statement.current_p = NULL;
          JERRY_ASSERT (context_p->stack_depth == 0);
#ifndef JERRY_NDEBUG
          JERRY_ASSERT (context_p->context_stack_depth == 0);
#endif /* !JERRY_NDEBUG */
          /* There is no lexer_next_token here, since the
           * next token belongs to the parent context. */

#if ENABLED (JERRY_ES2015_CLASS)
          if (JERRY_UNLIKELY (PARSER_IS_CLASS_CONSTRUCTOR_SUPER (context_p->status_flags)))
          {
            if (context_p->status_flags & PARSER_CLASS_IMPLICIT_SUPER)
            {
              parser_emit_cbc (context_p, CBC_PUSH_THIS);
            }
            else
            {
              parser_emit_cbc_ext (context_p, CBC_EXT_PUSH_CONSTRUCTOR_THIS);
            }
            parser_emit_cbc (context_p, CBC_RETURN);
            parser_flush_cbc (context_p);
          }
#endif /* ENABLED (JERRY_ES2015_CLASS) */
          return;
        }
        parser_raise_error (context_p, PARSER_ERR_INVALID_RIGHT_SQUARE);
      }
    }
    else if (context_p->token.type == LEXER_SEMICOLON)
    {
      lexer_next_token (context_p);
    }
    else if (context_p->token.type != LEXER_EOS
             && !(context_p->token.flags & LEXER_WAS_NEWLINE))
    {
      parser_raise_error (context_p, PARSER_ERR_SEMICOLON_EXPECTED);
    }

    while (true)
    {
      switch (context_p->stack_top_uint8)
      {
        case PARSER_STATEMENT_LABEL:
        {
          parser_label_statement_t label;

          parser_stack_pop_uint8 (context_p);
          parser_stack_pop (context_p, &label, sizeof (parser_label_statement_t));
          parser_stack_iterator_init (context_p, &context_p->last_statement);

          parser_set_breaks_to_current_position (context_p, label.break_list_p);
          continue;
        }

        case PARSER_STATEMENT_IF:
        {
          if (parser_parse_if_statement_end (context_p))
          {
            break;
          }
          continue;
        }

        case PARSER_STATEMENT_ELSE:
        {
          parser_if_else_statement_t else_statement;

          parser_stack_pop_uint8 (context_p);
          parser_stack_pop (context_p, &else_statement, sizeof (parser_if_else_statement_t));
          parser_stack_iterator_init (context_p, &context_p->last_statement);

          parser_set_branch_to_current_position (context_p, &else_statement.branch);
          continue;
        }

        case PARSER_STATEMENT_DO_WHILE:
        {
          parser_parse_do_while_statement_end (context_p);
          if (context_p->token.type == LEXER_SEMICOLON)
          {
            lexer_next_token (context_p);
          }
          continue;
        }

        case PARSER_STATEMENT_WHILE:
        {
          parser_parse_while_statement_end (context_p);
          continue;
        }

        case PARSER_STATEMENT_FOR:
        {
          parser_parse_for_statement_end (context_p);
          continue;
        }

        case PARSER_STATEMENT_FOR_IN:
        {
          parser_for_in_statement_t for_in_statement;
          parser_loop_statement_t loop;

          parser_stack_pop_uint8 (context_p);
          parser_stack_pop (context_p, &loop, sizeof (parser_loop_statement_t));
          parser_stack_pop (context_p, &for_in_statement, sizeof (parser_for_in_statement_t));
          parser_stack_iterator_init (context_p, &context_p->last_statement);

          parser_set_continues_to_current_position (context_p, loop.branch_list_p);

          parser_flush_cbc (context_p);
          PARSER_MINUS_EQUAL_U16 (context_p->stack_depth, PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION);
#ifndef JERRY_NDEBUG
          PARSER_MINUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

          parser_emit_cbc_ext_backward_branch (context_p,
                                               CBC_EXT_BRANCH_IF_FOR_IN_HAS_NEXT,
                                               for_in_statement.start_offset);

          parser_set_breaks_to_current_position (context_p, loop.branch_list_p);
          parser_set_branch_to_current_position (context_p, &for_in_statement.branch);
          continue;
        }
#if ENABLED (JERRY_ES2015_FOR_OF)
        case PARSER_STATEMENT_FOR_OF:
        {
          parser_for_of_statement_t for_of_statement;
          parser_loop_statement_t loop;

          parser_stack_pop_uint8 (context_p);
          parser_stack_pop (context_p, &loop, sizeof (parser_loop_statement_t));
          parser_stack_pop (context_p, &for_of_statement, sizeof (parser_for_of_statement_t));
          parser_stack_iterator_init (context_p, &context_p->last_statement);

          parser_set_continues_to_current_position (context_p, loop.branch_list_p);

          parser_flush_cbc (context_p);
          PARSER_MINUS_EQUAL_U16 (context_p->stack_depth, PARSER_FOR_OF_CONTEXT_STACK_ALLOCATION);
#ifndef JERRY_NDEBUG
          PARSER_MINUS_EQUAL_U16 (context_p->context_stack_depth, PARSER_FOR_OF_CONTEXT_STACK_ALLOCATION);
#endif /* !JERRY_NDEBUG */

          parser_emit_cbc_ext_backward_branch (context_p,
                                               CBC_EXT_BRANCH_IF_FOR_OF_HAS_NEXT,
                                               for_of_statement.start_offset);

          parser_set_breaks_to_current_position (context_p, loop.branch_list_p);
          parser_set_branch_to_current_position (context_p, &for_of_statement.branch);
          continue;
        }
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */

        case PARSER_STATEMENT_WITH:
        {
          parser_parse_with_statement_end (context_p);
          continue;
        }

        default:
        {
          break;
        }
      }
      break;
    }
  }

  JERRY_ASSERT (context_p->stack_depth == 0);
#ifndef JERRY_NDEBUG
  JERRY_ASSERT (context_p->context_stack_depth == 0);
#endif /* !JERRY_NDEBUG */

  parser_stack_pop_uint8 (context_p);
  context_p->last_statement.current_p = NULL;

  if (context_p->status_flags & PARSER_IS_CLOSURE)
  {
    parser_raise_error (context_p, PARSER_ERR_STATEMENT_EXPECTED);
  }
} /* parser_parse_statements */

/**
 * Free jumps stored on the stack if a parse error is occured.
 */
void JERRY_ATTR_NOINLINE
parser_free_jumps (parser_stack_iterator_t iterator) /**< iterator position */
{
  while (true)
  {
    uint8_t type = parser_stack_iterator_read_uint8 (&iterator);
    parser_branch_node_t *branch_list_p = NULL;

    switch (type)
    {
      case PARSER_STATEMENT_START:
      {
        return;
      }

      case PARSER_STATEMENT_LABEL:
      {
        parser_label_statement_t label;

        parser_stack_iterator_skip (&iterator, 1);
        parser_stack_iterator_read (&iterator, &label, sizeof (parser_label_statement_t));
        parser_stack_iterator_skip (&iterator, sizeof (parser_label_statement_t));
        branch_list_p = label.break_list_p;
        break;
      }

      case PARSER_STATEMENT_SWITCH:
      case PARSER_STATEMENT_SWITCH_NO_DEFAULT:
      {
        parser_switch_statement_t switch_statement;
        parser_loop_statement_t loop;

        parser_stack_iterator_skip (&iterator, 1);
        parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));
        parser_stack_iterator_skip (&iterator, sizeof (parser_loop_statement_t));
        parser_stack_iterator_read (&iterator, &switch_statement, sizeof (parser_switch_statement_t));
        parser_stack_iterator_skip (&iterator, sizeof (parser_switch_statement_t));

        branch_list_p = switch_statement.branch_list_p;
        while (branch_list_p != NULL)
        {
          parser_branch_node_t *next_p = branch_list_p->next_p;
          parser_free (branch_list_p, sizeof (parser_branch_node_t));
          branch_list_p = next_p;
        }
        branch_list_p = loop.branch_list_p;
        break;
      }

      case PARSER_STATEMENT_DO_WHILE:
      case PARSER_STATEMENT_WHILE:
      case PARSER_STATEMENT_FOR:
      case PARSER_STATEMENT_FOR_IN:
#if ENABLED (JERRY_ES2015_FOR_OF)
      case PARSER_STATEMENT_FOR_OF:
#endif /* ENABLED (JERRY_ES2015_FOR_OF) */
      {
        parser_loop_statement_t loop;

        parser_stack_iterator_skip (&iterator, 1);
        parser_stack_iterator_read (&iterator, &loop, sizeof (parser_loop_statement_t));
        parser_stack_iterator_skip (&iterator, parser_statement_length (type) - 1);
        branch_list_p = loop.branch_list_p;
        break;
      }

      default:
      {
        parser_stack_iterator_skip (&iterator, parser_statement_length (type));
        continue;
      }
    }

    while (branch_list_p != NULL)
    {
      parser_branch_node_t *next_p = branch_list_p->next_p;
      parser_free (branch_list_p, sizeof (parser_branch_node_t));
      branch_list_p = next_p;
    }
  }
} /* parser_free_jumps */

/**
 * @}
 * @}
 * @}
 */

#endif /* ENABLED (JERRY_PARSER) */
