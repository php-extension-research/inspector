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

#ifndef HAVE_INSPECTOR_FILE
#define HAVE_INSPECTOR_FILE

#include "php.h"

#include "zend_exceptions.h"
#include "zend_interfaces.h"

#include "php_inspector.h"

#include "strings.h"
#include "reflection.h"
#include "instruction.h"
#include "function.h"
#include "break.h"
#include "map.h"

zend_class_entry *php_inspector_file_ce;

zend_op_array* (*zend_compile_function)(zend_file_handle *, int);

static zend_always_inline zend_bool php_inspector_file_executing(zend_string *file, zend_execute_data *frame) {
	while (frame && (!frame->func || !ZEND_USER_CODE(frame->func->type))) {
		frame = frame->prev_execute_data;
	}

	if (frame) {
		return zend_string_equals(frame->func->op_array.filename, file);
	}
	
	return 0;
}

static PHP_METHOD(InspectorFile, __construct)
{
	php_inspector_function_t *function = php_inspector_function_from(getThis());
	zend_string *file = NULL;

	if (zend_parse_parameters_throw(ZEND_NUM_ARGS(), "S", &file) != SUCCESS) {
		return;
	}

	if (php_inspector_file_executing(file, execute_data)) {
		zend_throw_exception_ex(reflection_exception_ptr, 0,
			"cannot inspect currently executing file");
		return;
	}

	php_inspector_table_insert(
		PHP_INSPECTOR_ROOT_PENDING, 
		PHP_INSPECTOR_TABLE_FILE, 
		file, getThis());
}

static PHP_METHOD(InspectorFile, isPending)
{
	php_inspector_function_t *function =
		php_inspector_function_from(getThis());

	RETURN_BOOL(function->function == NULL);
}

static PHP_METHOD(InspectorFile, isExpired)
{
	php_inspector_function_t *function =
		php_inspector_function_from(getThis());

	RETURN_BOOL(function->expired);
}

static PHP_METHOD(InspectorFile, purge)
{
	HashTable *filters = NULL;
	zend_string *included;
	
	if (zend_parse_parameters_throw(ZEND_NUM_ARGS(), "|H", &filters) != SUCCESS) {
		return;
	}

	ZEND_HASH_FOREACH_STR_KEY(&EG(included_files), included) {
		HashTable *registered = php_inspector_table(
			PHP_INSPECTOR_ROOT_REGISTERED, 
			PHP_INSPECTOR_TABLE_FILE, 
			included, 0);
		zval *object;

		if (!registered) {
			zend_hash_del(
				&EG(included_files), included);
			continue;
		}

		ZEND_HASH_FOREACH_VAL(registered, object) {
			php_inspector_function_t *function =
				php_inspector_function_from(object);
		
			function->function = NULL;

			php_inspector_table_insert(
				PHP_INSPECTOR_ROOT_PENDING, 	
				PHP_INSPECTOR_TABLE_FILE, 
				included, object);
		} ZEND_HASH_FOREACH_END();

		php_inspector_table_drop(
			PHP_INSPECTOR_ROOT_REGISTERED,
			PHP_INSPECTOR_TABLE_FUNCTION, 
			included);

		zend_hash_del(&EG(included_files), included);
	} ZEND_HASH_FOREACH_END();
}

ZEND_BEGIN_ARG_INFO_EX(InspectorFile_purge_arginfo, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, filter, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(InspectorFile_construct_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, file)
ZEND_END_ARG_INFO()

#if PHP_VERSION_ID >= 70200
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(InspectorFile_state_arginfo, 0, 0, _IS_BOOL, 0)
#else
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(InspectorFile_state_arginfo, 0, 0, _IS_BOOL, NULL, 0)
#endif
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(InspectorFile_stateChanged_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

static zend_function_entry php_inspector_file_methods[] = {
	PHP_ME(InspectorFile, __construct, InspectorFile_construct_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(InspectorFile, isPending, InspectorFile_state_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(InspectorFile, isExpired, InspectorFile_state_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(InspectorFile, purge, InspectorFile_purge_arginfo, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	PHP_FE_END
};

int php_inspector_file_resolve(zval *zv, zend_function *ops) {
	php_inspector_function_t *function = 
		php_inspector_function_from(zv);
	zend_function *onResolve = zend_hash_find_ptr(
		&Z_OBJCE_P(zv)->function_table, PHP_INSPECTOR_STRING_ONRESOLVE);

	if (function->function) {
		php_inspector_breaks_purge(function->function);
	}

	function->function = ops;
	function->expired = 0;

	php_inspector_instruction_cache_flush(zv, NULL);

	if (ZEND_USER_CODE(onResolve->type)) {
		zval rv;

		ZVAL_NULL(&rv);

		zend_call_method_with_0_params(zv, Z_OBJCE_P(zv), &onResolve, "onresolve", &rv);

		if (Z_REFCOUNTED(rv)) {
			zval_ptr_dtor(&rv);
		}

		php_inspector_table_insert(
			PHP_INSPECTOR_ROOT_REGISTERED,
			PHP_INSPECTOR_TABLE_FILE,
			ops->op_array.filename, zv);
	}

	function->expired = 1;

	return ZEND_HASH_APPLY_REMOVE;
}

static zend_op_array* php_inspector_compile(zend_file_handle *fh, int type) {
	zend_op_array *function = zend_compile_function(fh, type);

	if (UNEXPECTED(!function || !function->filename)) {
		return function;
	}

	if (UNEXPECTED(php_inspector_table(
				PHP_INSPECTOR_ROOT_PENDING, 
				PHP_INSPECTOR_TABLE_FILE, 
				function->filename, 0))) {

		php_inspector_map_create(function);
	}

	return function;
}

PHP_MINIT_FUNCTION(inspector_file) 
{
	zend_class_entry ce;

	INIT_NS_CLASS_ENTRY(ce, "Inspector", "InspectorFile", php_inspector_file_methods);

	php_inspector_file_ce = zend_register_internal_class_ex(&ce, php_inspector_function_ce);

	return SUCCESS;
}

PHP_RINIT_FUNCTION(inspector_file)
{
	zend_compile_function = zend_compile_file;
	zend_compile_file = php_inspector_compile;

	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(inspector_file)
{
	zend_compile_file = zend_compile_function;

	return SUCCESS;
}
#endif
