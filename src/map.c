/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2018 Joe Watkins                                       |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
*/

#ifndef HAVE_INSPECTOR_MAP
#define HAVE_INSPECTOR_MAP

#include "php.h"
#include "php_inspector.h"

#ifndef GC_ADDREF
#	define GC_ADDREF(g) ++GC_REFCOUNT(g)
#endif

typedef void (*copy_constructor_t)(void *);

static zend_always_inline void* php_inspector_map_dup(void *ptr, size_t num, size_t size, copy_constructor_t constructor) {
	void *dup;

	if (!num) {
		return NULL;
	}

	dup = (void*) ecalloc(num, size);

	memcpy(dup, ptr, num * size);

	if (constructor) {
		char *begin = (char*) dup,
		     *end   = ((char*) begin) + (num * size);

		while (begin < end) {
			constructor(
				(void*) begin);
			begin += size;
		}
	}

	return dup;
}

#if ZEND_USE_ABS_JMP_ADDR
static zend_always_inline void php_inspector_map_opcode(zend_op *opline) {
	switch (opline->opcode) {
		case ZEND_JMP:
		case ZEND_FAST_CALL:
#if PHP_VERSION_ID < 70300
		case ZEND_DECLARE_ANON_CLASS:
		case ZEND_DECLARE_ANON_INHERITED_CLASS:
#endif
			opline->op1.jmp_addr = copy + (opline->op1.jmp_addr - opcodes);
			break;

		case ZEND_JMPZNZ:
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
		case ZEND_JMP_SET:
		case ZEND_COALESCE:
		case ZEND_NEW:
		case ZEND_FE_RESET_R:
		case ZEND_FE_RESET_RW:
		case ZEND_ASSERT_CHECK:
			opline->op2.jmp_addr = copy + (opline->op2.jmp_addr - opcodes);
			break;
#if PHP_VERSION_ID >= 70300
		case ZEND_CATCH:
			if (!(opline->extended_value & ZEND_LAST_CATCH)) {
				opline->op2.jmp_addr = copy + (opline->op2.jmp_addr - opcodes);
			}
		break;
#endif
	}
}
#endif

static zend_always_inline void php_inspector_map_arginfo(zend_arg_info *info) {
	if (info->name) {
		zend_string_addref(info->name);
	}

#if PHP_VERSION_ID <= 70200
	if (info->class_name) {
		zend_string_addref(info->class_name);
	}
#else
	if (ZEND_TYPE_IS_SET(info->type) && ZEND_TYPE_IS_CLASS(info->type)) {
		zend_string_addref(ZEND_TYPE_NAME(info->type));
	}
#endif
}

static zend_always_inline void php_inspector_map_string_addref(zend_string *zs) {
	zend_string_addref(zs);
}

static zend_always_inline void php_inspector_map_zval_addref(zval *zv) {
	Z_TRY_ADDREF_P(zv);
}

static zend_always_inline void php_inspector_map_construct(zend_op_array *mapped) {
	mapped->refcount = emalloc(sizeof(uint32_t));

	(*mapped->refcount) = 1;

	//if (mapped->filename)
	//	zend_string_addref(mapped->filename);

	if (mapped->function_name)
		zend_string_addref(mapped->function_name);

	if (mapped->doc_comment)
		zend_string_addref(mapped->doc_comment);

	mapped->try_catch_array = (zend_try_catch_element*) php_inspector_map_dup(
		mapped->try_catch_array, mapped->last_try_catch, sizeof(zend_try_catch_element), NULL);

	mapped->live_range = (zend_live_range*) php_inspector_map_dup(
		mapped->live_range, mapped->last_live_range, sizeof(zend_live_range), NULL);

	mapped->vars    = (zend_string**) php_inspector_map_dup(
		mapped->vars, mapped->last_var, sizeof(zend_string*),
		(copy_constructor_t) php_inspector_map_string_addref);

	mapped->literals = (zval*) php_inspector_map_dup(
		mapped->literals, mapped->last_literal, sizeof(zval),
		(copy_constructor_t) php_inspector_map_zval_addref);

	mapped->opcodes = (zend_op*) php_inspector_map_dup(
		mapped->opcodes, mapped->last, sizeof(zend_op),
#if ZEND_USE_ABS_JMP_ADDR
		(copy_constructor_t) php_inspector_map_opcode);
#else
		NULL);
#endif

	if (mapped->static_variables) {
		if (!(GC_FLAGS(mapped->static_variables) & IS_ARRAY_IMMUTABLE)) {
			GC_ADDREF(mapped->static_variables);
		}
	}

	if (mapped->num_args) {

		zend_arg_info *info = mapped->arg_info;
		uint32_t       end = mapped->num_args;

		if (mapped->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
			info--;
			end++;
		}

		if (mapped->fn_flags & ZEND_ACC_VARIADIC) {
			end++;
		}

		mapped->arg_info = php_inspector_map_dup(
			info, end, sizeof(zend_arg_info), 
			(copy_constructor_t) php_inspector_map_arginfo);

		if (mapped->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
			mapped->arg_info++;
		}
	}

	mapped->run_time_cache = NULL;
}

zend_op_array* php_inspector_map_create(zend_op_array *source) {
	zend_op_array *mapped;

	if (!ZEND_USER_CODE(source->type)) {
		return source;
	}

	mapped = (zend_op_array*) php_inspector_map_dup(
		source, 1, sizeof(zend_op_array), 
		(copy_constructor_t) php_inspector_map_construct);

	return mapped;
}

#endif
