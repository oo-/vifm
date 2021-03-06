/* vifm
 * Copyright (C) 2012 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "variables.h"

#include <assert.h>
#include <ctype.h>
#include <stddef.h> /* size_t */
#include <stdio.h>
#include <stdlib.h> /* free() realloc() */
#include <string.h>

#include "../compat/reallocarray.h"
#include "../utils/env.h"
#include "../utils/macros.h"
#include "../utils/str.h"
#include "private/options.h"
#include "completion.h"
#include "parsing.h"
#include "text_buffer.h"
#include "var.h"

#define VAR_NAME_MAX 64
#define VAL_LEN_MAX 2048

/* Types of supported variables. */
typedef enum
{
	VT_ENVVAR,        /* Environment variable. */
	VT_ANY_OPTION,    /* Global and local options (if local exists). */
	VT_GLOBAL_OPTION, /* Global option. */
	VT_LOCAL_OPTION,  /* Local option. */
}
VariableType;

/* Supported operations. */
typedef enum
{
	VO_ASSIGN, /* Assigning a variable (=). */
	VO_APPEND, /* Appending to a string (.=). */
	VO_ADD,    /* Adding to numbers or composite values (+=). */
	VO_SUB,    /* Substructing from numbers or removing from composites (-=). */
}
VariableOperation;

typedef struct
{
	char *name;
	char *val;
	char *initial;
	int from_parent;
	int removed;
}
envvar_t;

const char ENV_VAR_NAME_FIRST_CHAR[] = "abcdefghijklmnopqrstuvwxyz"
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
const char ENV_VAR_NAME_CHARS[] = "abcdefghijklmnopqrstuvwxyz"
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

static void init_var(const char *env);
static int extract_name(const char **in, VariableType *type, size_t buf_len,
		char buf[]);
static int extract_op(const char **in, VariableOperation *vo);
static int parse_name(const char **in, const char first[], const char other[],
		size_t buf_len, char buf[]);
static void report_parsing_error(ParsingErrors error);
static int is_valid_op(const char name[], VariableType vt,
		VariableOperation vo);
static int perform_op(const char name[], VariableType vt,
		VariableOperation vo, const char value[]);
static void append_envvar(const char *name, const char *val);
static void set_envvar(const char *name, const char *val);
static int perform_opt_op(const char name[], VariableType vt,
		VariableOperation vo, const char value[]);
static envvar_t * get_record(const char *name);
static char * skip_non_whitespace(const char str[]);
static envvar_t * find_record(const char *name);
static void free_record(envvar_t *record);
static void clear_record(envvar_t *record);

static int initialized;
static envvar_t *vars;
static size_t nvars;

void
init_variables(void)
{
	int env_count;
	extern char **environ;

	if(nvars > 0)
		clear_variables();

	/* Count environment variables. */
	env_count = 0;
	while(environ[env_count] != NULL)
		env_count++;

	if(env_count > 0)
	{
		int i;

		/* Allocate memory for environment variables. */
		vars = reallocarray(NULL, env_count, sizeof(*vars));
		assert(vars != NULL && "Failed to allocate memory for env vars.");

		/* Initialize variable list. */
		i = 0;
		while(environ[i] != NULL)
		{
			init_var(environ[i]);
			i++;
		}
	}

	init_parser(&local_getenv);

	initialized = 1;
}

const char *
local_getenv(const char *envname)
{
	envvar_t *record = find_record(envname);
	return (record == NULL || record->removed) ? "" : record->val;
}

static void
init_var(const char *env)
{
	envvar_t *record;
	char name[VAR_NAME_MAX + 1];
	char *p = strchr(env, '=');
	assert(p != NULL);

	copy_str(name, MIN(sizeof(name), (size_t)(p - env + 1)), env);
	record = get_record(name);
	if(record == NULL)
		return;
	record->from_parent = 1;

	(void)replace_string(&record->initial, p + 1);
	(void)replace_string(&record->val, p + 1);
	if(record->initial == NULL || record->val == NULL)
	{
		free_record(record);
	}
}

void
clear_variables(void)
{
	size_t i;
	assert(initialized);

	/* Free memory. */
	for(i = 0U; i < nvars; ++i)
	{
		if(vars[i].name == NULL)
			continue;

		if(vars[i].from_parent)
		{
			set_envvar(vars[i].name, vars[i].initial);
		}
		else
		{
			env_remove(vars[i].name);
		}
		free_record(&vars[i]);
	}

	nvars = 0;
	free(vars);
	vars = NULL;
}

int
let_variables(const char *cmd)
{
	char name[VAR_NAME_MAX + 1];
	int error;
	var_t res_var;
	char *str_val;
	ParsingErrors parsing_error;
	VariableType type;
	VariableOperation op;

	assert(initialized);

	if(extract_name(&cmd, &type, sizeof(name), name) != 0)
	{
		return 1;
	}

	cmd = skip_whitespace(cmd);

	if(extract_op(&cmd, &op) != 0)
	{
		return -1;
	}

	cmd = skip_whitespace(cmd);

	parsing_error = parse(cmd, &res_var);
	if(parsing_error != PE_NO_ERROR)
	{
		report_parsing_error(parsing_error);
		return -1;
	}

	if(get_last_position() != NULL && *get_last_position() != '\0')
	{
		vle_tb_append_linef(vle_err, "%s: %s", "Incorrect :let statement",
				"trailing characters");
		return -1;
	}

	if(!is_valid_op(name, type, op))
	{
		vle_tb_append_linef(vle_err, "Wrong variable type for this operation");
		return -1;
	}

	str_val = var_to_string(res_var);

	error = perform_op(name, type, op, str_val);

	free(str_val);
	var_free(res_var);

	return error;
}

/* Extracts name from the string.  Returns zero on success, otherwise non-zero
 * is returned. */
static int
extract_name(const char **in, VariableType *type, size_t buf_len, char buf[])
{
	int error;

	/* Copy variable name. */
	if(**in == '$')
	{
		++*in;
		error = (parse_name(in, ENV_VAR_NAME_FIRST_CHAR, ENV_VAR_NAME_CHARS,
				buf_len, buf) != 0);
		*type = VT_ENVVAR;
	}
	else if(**in == '&')
	{
		++*in;

		*type = VT_ANY_OPTION;
		if(((*in)[0] == 'l' || (*in)[0] == 'g') && (*in)[1] == ':')
		{
			*type = (*in[0] == 'l') ? VT_LOCAL_OPTION : VT_GLOBAL_OPTION;
			*in += 2;
		}

		error = (parse_name(in, OPT_NAME_FIRST_CHAR, OPT_NAME_CHARS, buf_len,
					buf) != 0);
	}
	else
	{
		/* Currently we support only environment variables and options. */
		vle_tb_append_line(vle_err, "Incorrect variable type");
		return -1;
	}
	if(error)
	{
		vle_tb_append_line(vle_err, "Incorrect variable name");
		return -1;
	}

	/* Test for empty variable name. */
	if(buf[0] == '\0')
	{
		vle_tb_append_linef(vle_err, "%s: %s", "Unsupported variable name",
				"empty name");
		return -1;
	}

	return 0;
}

/* Extracts operation from the string.  Sets operation advancing the
 * pointer if needed.  Returns zero on success, otherwise non-zero returned. */
static int
extract_op(const char **in, VariableOperation *vo)
{
	/* Check first operator char and skip it. */
	if(**in == '.')
	{
		++*in;
		*vo = VO_APPEND;
	}
	else if(**in == '+')
	{
		*vo = VO_ADD;
		++*in;
	}
	else if(**in == '-')
	{
		*vo = VO_SUB;
		++*in;
	}
	else
	{
		*vo = VO_ASSIGN;
	}

	/* Check for equal sign and skip it. */
	if(**in != '=')
	{
		vle_tb_append_linef(vle_err, "%s: '=' expected at %s",
				"Incorrect :let statement", *in);
		return 1;
	}
	++*in;

	return 0;
}

/* Parses name of the form `first { other }`.  Returns zero on success,
 * otherwise non-zero is returned. */
static int
parse_name(const char **in, const char first[], const char other[],
		size_t buf_len, char buf[])
{
	if(buf_len == 0UL || !char_is_one_of(first, **in))
	{
		return 1;
	}

	buf[0] = '\0';

	do
	{
		strcatch(buf, **in);
		++*in;
	}
	while(--buf_len > 1UL && char_is_one_of(other, **in));

	return 0;
}

/* Appends error message with details to the error stream. */
static void
report_parsing_error(ParsingErrors error)
{
	switch(error)
	{
		case PE_NO_ERROR:
			/* Not an error. */
			break;
		case PE_INVALID_EXPRESSION:
			vle_tb_append_linef(vle_err, "%s: %s", "Invalid expression",
					get_last_position());
			break;
		case PE_INVALID_SUBEXPRESSION:
			vle_tb_append_linef(vle_err, "%s: %s", "Invalid subexpression",
					get_last_position());
			break;
		case PE_MISSING_QUOTE:
			vle_tb_append_linef(vle_err, "%s: %s",
					"Invalid :let expression (missing quote)", get_last_position());
			break;
		case PE_INTERNAL:
			vle_tb_append_line(vle_err, "Internal error");
			break;
	}
}

/* Validates operation on a specific variable type.  Returns non-zero for valid
 * operation, otherwise zero is returned. */
static int
is_valid_op(const char name[], VariableType vt, VariableOperation vo)
{
	opt_t *opt;

	if(vt == VT_ENVVAR)
	{
		return (vo == VO_ASSIGN || vo == VO_APPEND);
	}

	opt = find_option(name, OPT_GLOBAL);
	if(opt == NULL)
	{
		/* Let this error be handled somewhere else. */
		return 1;
	}

	if(opt->type == OPT_BOOL)
	{
		return 0;
	}

	if(opt->type == OPT_STR)
	{
		return (vo == VO_ASSIGN || vo == VO_APPEND);
	}

	return (vo == VO_ASSIGN || vo == VO_ADD || vo == VO_SUB);
}

/* Performs operation on a value.  Returns zero on success, otherwise non-zero
 * is returned. */
static int
perform_op(const char name[], VariableType vt, VariableOperation vo,
		const char value[])
{
	if(vt == VT_ENVVAR)
	{
		/* Update environment variable. */
		if(vo == VO_APPEND)
		{
			append_envvar(name, value);
		}
		else
		{
			set_envvar(name, value);
		}
		return 0;
	}

	/* Update an option. */

	if(vt == VT_ANY_OPTION || vt == VT_LOCAL_OPTION)
	{
		if(perform_opt_op(name, vt, vo, value) != 0)
		{
			return 1;
		}
	}

	if(vt == VT_ANY_OPTION || vt == VT_GLOBAL_OPTION)
	{
		if(perform_opt_op(name, VT_GLOBAL_OPTION, vo, value) != 0)
		{
			return 1;
		}
	}

	return 0;
}

static void
append_envvar(const char *name, const char *val)
{
	envvar_t *record;
	char *p;

	record = find_record(name);
	if(record == NULL)
	{
		set_envvar(name, val);
		return;
	}

	p = realloc(record->val, strlen(record->val) + strlen(val) + 1);
	if(p == NULL)
	{
		vle_tb_append_line(vle_err, "Not enough memory");
		return;
	}
	record->val = p;

	strcat(record->val, val);
	env_set(name, record->val);
}

static void
set_envvar(const char *name, const char *val)
{
	envvar_t *record;
	char *p;

	record = get_record(name);
	if(record == NULL)
	{
		vle_tb_append_line(vle_err, "Not enough memory");
		return;
	}

	p = strdup(val);
	if(p == NULL)
	{
		vle_tb_append_line(vle_err, "Not enough memory");
		return;
	}
	free(record->val);
	record->val = p;
	env_set(name, val);
}

/* Performs operation on an option.  Returns zero on success, otherwise non-zero
 * is returned. */
static int
perform_opt_op(const char name[], VariableType vt, VariableOperation vo,
		const char value[])
{
	OPT_SCOPE scope = (vt == VT_ANY_OPTION || vt == VT_LOCAL_OPTION)
	                ? OPT_LOCAL
	                : OPT_GLOBAL;

	opt_t *const opt = find_option(name, scope);
	if(opt == NULL)
	{
		if(vt == VT_ANY_OPTION)
		{
			return 0;
		}

		vle_tb_append_linef(vle_err, "Unknown %s option name: %s",
				(scope == OPT_LOCAL) ? "local" : "global", name);
		return 1;
	}

	switch(vo)
	{
		case VO_ASSIGN:
			if(set_set(opt, value) != 0)
			{
				return 1;
			}
			break;
		case VO_ADD:
		case VO_APPEND:
			if(set_add(opt, value) != 0)
			{
				return 1;
			}
			break;
		case VO_SUB:
			if(set_remove(opt, value) != 0)
			{
				return 1;
			}
			break;
	}
	return 0;
}

/* Searches for variable and creates new record if it didn't existed. */
static envvar_t *
get_record(const char *name)
{
	envvar_t *p = NULL;
	size_t i;

	/* search for existent variable */
	for(i = 0U; i < nvars; ++i)
	{
		if(vars[i].name == NULL)
			p = &vars[i];
		else if(strcmp(vars[i].name, name) == 0)
			return &vars[i];
	}

	if(p == NULL)
	{
		/* try to reallocate list of variables */
		p = realloc(vars, sizeof(*vars)*(nvars + 1));
		if(p == NULL)
			return NULL;
		vars = p;
		p = &vars[nvars];
		nvars++;
	}

	/* initialize new record */
	p->initial = strdup("");
	p->name = strdup(name);
	p->val = strdup("");
	p->from_parent = 0;
	p->removed = 0;
	if(p->initial == NULL || p->name == NULL || p->val == NULL)
	{
		free_record(p);
		return NULL;
	}
	return p;
}

int
unlet_variables(const char *cmd)
{
	int error = 0;
	assert(initialized);

	while(*cmd != '\0')
	{
		envvar_t *record;

		char name[VAR_NAME_MAX + 1];
		char *p;
		int envvar = 1;

		/* Check if it's environment variable. */
		if(*cmd != '$')
			envvar = 0;
		else
			cmd++;

		/* Copy variable name. */
		p = name;
		while(*cmd != '\0' && char_is_one_of(ENV_VAR_NAME_CHARS, *cmd) &&
				(size_t)(p - name) < sizeof(name) - 1)
		{
			*p++ = *cmd++;
		}
		*p = '\0';

		if(*cmd != '\0' && !isspace(*cmd))
		{
			vle_tb_append_line(vle_err, "Trailing characters");
			error++;
			break;
		}

		cmd = skip_whitespace(cmd);

		/* Currently we support only environment variables. */
		if(!envvar)
		{
			vle_tb_append_linef(vle_err, "%s: %s", "Unsupported variable type", name);

			cmd = skip_non_whitespace(cmd);
			error++;
			continue;
		}

		/* Test for empty variable name. */
		if(name[0] == '\0')
		{
			vle_tb_append_linef(vle_err, "%s: %s", "Unsupported variable name",
					"empty name");
			error++;
			continue;
		}

		record = find_record(name);
		if(record == NULL || record->removed)
		{
			vle_tb_append_linef(vle_err, "%s: %s", "No such variable", name);
			error++;
			continue;
		}

		if(record->from_parent)
			record->removed = 1;
		else
			free_record(record);
		env_remove(name);
	}

	return error;
}

/* Skips consecutive non-whitespace characters.  Returns pointer to the next
 * character in the str. */
static char *
skip_non_whitespace(const char str[])
{
	while(!isspace(*str) && *str != '\0')
	{
		++str;
	}
	return (char *)str;
}

/* searches for existent variable */
static envvar_t *
find_record(const char *name)
{
	size_t i;
	for(i = 0U; i < nvars; ++i)
	{
		if(vars[i].name != NULL && stroscmp(vars[i].name, name) == 0)
			return &vars[i];
	}
	return NULL;
}

static void
free_record(envvar_t *record)
{
	free(record->initial);
	free(record->name);
	free(record->val);

	clear_record(record);
}

static void
clear_record(envvar_t *record)
{
	record->initial = NULL;
	record->name = NULL;
	record->val = NULL;
}

void
complete_variables(const char *cmd, const char **start)
{
	size_t i;
	size_t len;
	assert(initialized && "Variables unit is not initialized.");

	/* currently we support only environment variables */
	if(*cmd != '$')
	{
		*start = cmd;
		vle_compl_add_match(cmd);
		return;
	}
	cmd++;
	*start = cmd;

	/* add all variables that start with given beginning */
	len = strlen(cmd);
	for(i = 0U; i < nvars; ++i)
	{
		if(vars[i].name == NULL)
			continue;
		if(vars[i].removed)
			continue;
		if(strnoscmp(vars[i].name, cmd, len) == 0)
			vle_compl_add_match(vars[i].name);
	}
	vle_compl_finish_group();
	vle_compl_add_last_match(cmd);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
