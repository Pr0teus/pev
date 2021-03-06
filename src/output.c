/*
	pev - the PE file analyzer toolkit

	output.c - Symbols and APIs to be used to output data in multiple formats.

	Copyright (C) 2012 - 2014 pev authors

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "output.h"
#include "output_plugin.h"
#include "stack.h"
#include "compat/strlcat.h"
#include "compat/sys/queue.h"
#include <stdlib.h>
#include <stdbool.h>

//
// Global variables
//

#define FORMAT_ID_FOR_TEXT 3

static bool g_is_document_open = false;
static const format_t *g_format = NULL;
static STACK_TYPE *g_scope_stack = NULL;
static int g_argc = 0;
static char **g_argv = NULL;
static char *g_cmdline = NULL;

typedef struct _format_entry {
	const format_t *format;
	SLIST_ENTRY(_format_entry) entries;
} format_entry_t;

static SLIST_HEAD(_format_t_list, _format_entry) g_registered_formats = SLIST_HEAD_INITIALIZER(g_registered_formats);

//
// Definition of internal functions
//

static format_entry_t *output_lookup_format_entry_by_id(format_id_t id) {
	format_entry_t *entry;
	SLIST_FOREACH(entry, &g_registered_formats, entries) {
		if (entry->format->id == id)
			return entry;
	}

	return NULL;
}

static const format_t *output_lookup_format_by_id(format_id_t id) {
	const format_entry_t *entry = output_lookup_format_entry_by_id(id);
	if (entry == NULL)
		return NULL;

	return entry->format;
}

static char *str_array_join(char *strings[], size_t count, char delimiter) {
	if (strings == NULL || strings[0] == NULL)
		return strdup("");

	// Count how much memory the resulting string is going to need,
	// considering delimiters for each string. The last delimiter will
	// be a NULL terminator;
	size_t result_length = 0;
	for (size_t i = 0; i < count; i++) {
		result_length += strlen(strings[i]) + 1;
	}

	// Allocate the resulting string.
	char *result = malloc(result_length);
	if (result == NULL)
		return NULL; // Return NULL because it failed miserably!

	// Null terminate it.
	result[--result_length] = '\0';

	// Join all strings.
	char ** current_string = strings;
	char * current_char = current_string[0];
	for (size_t i = 0; i < result_length; i++) {
		if (*current_char != '\0') {
			result[i] = *current_char++;
		} else {
			// Reached the end of a string. Add a delimiter and move to the next one.
			result[i] = delimiter;
			current_string++;
			current_char = current_string[0];
		}
	}

	return result;
}

static void output_unregister_all_formats(void) {
	while (!SLIST_EMPTY(&g_registered_formats)) {
		format_entry_t *entry = SLIST_FIRST(&g_registered_formats);
		SLIST_REMOVE_HEAD(&g_registered_formats, entries);
		free(entry);
	}
}

//
// API
//

int output_plugin_register_format(const format_t *format) {
	format_entry_t *entry = malloc(sizeof *entry);
	if (entry == NULL) {
		//fprintf(stderr, "output: allocation failed for format entry\n");
		return -1;
	}

	memset(entry, 0, sizeof *entry);

	entry->format = format;
	SLIST_INSERT_HEAD(&g_registered_formats, entry, entries);

	return 0;
}

void output_plugin_unregister_format(const format_t *format) {
	format_entry_t *entry = output_lookup_format_entry_by_id(format->id);
	if (entry == NULL)
		return;

	SLIST_REMOVE(&g_registered_formats, entry, _format_entry, entries);
	free(entry);
}

void output(const char *key, const char *value) {
	output_keyval(key, value);
}

void output_init(void) {
	g_format = output_lookup_format_by_id(FORMAT_ID_FOR_TEXT);
	g_scope_stack = STACK_ALLOC(15);
	if (g_scope_stack == NULL)
		abort();
}

void output_term(void) {
	if (g_cmdline != NULL)
		free(g_cmdline);

	// TODO(jweyrich): Should we loop to pop + close + output every scope?
	if (g_scope_stack != NULL)
		free(g_scope_stack);

	output_unregister_all_formats();
}

const char *output_cmdline(void) {
	return g_cmdline;
}

void output_set_cmdline(int argc, char *argv[]) {
	g_argc = argc;
	g_argv = argv;
	g_cmdline = str_array_join(g_argv, g_argc, ' ');
	if (g_cmdline == NULL) {
		fprintf(stderr, "output: allocation failed for str_array_join\n");
		abort();
	}
	//printf("cmdline = %s\n", g_cmdline);
}

const format_t *output_format(void) {
	return g_format;
}

const format_t *output_parse_format(const char *format_name) {
	const format_t *format = NULL;

	format_entry_t *entry;
	SLIST_FOREACH(entry, &g_registered_formats, entries) {
		// TODO(jweyrich): Should we use strcasecmp? Conforms to 4.4BSD and POSIX.1-2001, but not to C89 nor C99.
		if (strcmp(format_name, entry->format->name) == 0) {
			format = entry->format;
			break;
		}
	}

	return format;
}

void output_set_format(const format_t *format) {
	g_format = format;
}

int output_set_format_by_name(const char *format_name) {
	const format_t *format = output_parse_format(format_name);
	if (format == NULL)
		return -1;

	output_set_format(format);

	return 0;
}

size_t output_available_formats(char *buffer, size_t size, char separator) {
	size_t total_available = 0;
	size_t consumed = 0;
	bool truncated = false;

	memset(buffer, 0, size);

	format_entry_t *entry;
	SLIST_FOREACH(entry, &g_registered_formats, entries) {
		if (!truncated) {
			const char *format_name = entry->format->name;

			consumed = bsd_strlcat(buffer, format_name, size);
			if (consumed > size) {
				// TODO(jweyrich): Handle truncation.
				total_available++;
				truncated = true;
				continue;
			}

			if (consumed < size - 1) {
				buffer[consumed++] = separator;
			}
		}

		total_available++;
	}

	buffer[consumed - 1] = '\0';

	return total_available;
}

void output_open_document(void) {
	output_open_document_with_name(NULL);
}

void output_open_document_with_name(const char *document_name) {
	assert(g_format != NULL);
	// Cannot open a new document while there's one already open.
	assert(!g_is_document_open);

	const char *key = document_name;
	const char *value = NULL;
	const output_type_e type = OUTPUT_TYPE_DOCUMENT_OPEN;
	const uint16_t level = 0;

	if (g_format != NULL)
		g_format->output_fn(g_format, type, level, key, value);

	g_is_document_open = true;
}

void output_close_document(void) {
	assert(g_format != NULL);
	// Closing a document without first opening it is an error.
	assert(g_is_document_open);

	const char *key = NULL;
	const char *value = NULL;
	const output_type_e type = OUTPUT_TYPE_DOCUMENT_CLOSE;
	const uint16_t level = 0;

	if (g_format != NULL)
		g_format->output_fn(g_format, type, level, key, value);

	g_is_document_open = false;
}

void output_open_scope(const char *scope_name) {
	assert(g_format != NULL);

	const char *key = scope_name;
	const char *value = NULL;
	const output_type_e type = OUTPUT_TYPE_SCOPE_OPEN;
	const uint16_t doc_level = g_is_document_open ? 1 : 0;
	const uint16_t scope_level = STACK_COUNT(g_scope_stack);

	if (g_format != NULL)
		g_format->output_fn(g_format, type, doc_level + scope_level, key, value);

	int ret = STACK_PUSH(g_scope_stack, (void *)scope_name);
	if (ret < 0)
		abort();
}

void output_close_scope(void) {
	assert(g_format != NULL);

	const char *scope_name = NULL;
	int ret = STACK_POP(g_scope_stack, (void *)&scope_name);
	if (ret < 0) {
		fprintf(stderr, "output: cannot close a scope that has not been opened.\n");
		abort();
	}

	const char *key = scope_name;
	const char *value = NULL;
	const output_type_e type = OUTPUT_TYPE_SCOPE_CLOSE;
	const uint16_t doc_level = g_is_document_open ? 1 : 0;
	const uint16_t scope_level = STACK_COUNT(g_scope_stack);

	if (g_format != NULL)
		g_format->output_fn(g_format, type, doc_level + scope_level, key, value);
}

void output_keyval(const char *key, const char *value) {
	assert(g_format != NULL);

	const output_type_e type = OUTPUT_TYPE_ATTRIBUTE;
	const uint16_t doc_level = g_is_document_open ? 1 : 0;
	const uint16_t scope_level = STACK_COUNT(g_scope_stack);

	if (g_format != NULL)
		g_format->output_fn(g_format, type, doc_level + scope_level, key, value);
}
