/*
 * Copyright (C) 2020  Red Hat, Inc.
 * Red Hat Author(s):  David Cantrell <dcantrell@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <sys/queue.h>
#include "rpminspect.h"

/**
 * @brief Given an RPM spec file, read all of the macro definitions in
 * to a list.  The macros are read in to the pair_list_t in the struct
 * rpminspect for the entire program run.  If it has already been read
 * in, this function just returns the number of keys.
 *
 * To force a reread, clear the cached table using free_pair() on the
 * macros member of the struct rpminspect.
 *
 * @param ri The struct rpminspect instance for the program run.
 * @param specfile The path to the RPM spec file.
 * @return Number of keys or -1 on error.
 */
int get_specfile_macros(struct rpminspect *ri, const char *specfile)
{
    int n = 0;
    string_entry_t *specline = NULL;
    string_list_t *spec = NULL;
    string_list_t *fields = NULL;
    string_entry_t *entry = NULL;
    pair_entry_t *pair = NULL;

    assert(ri != NULL);
    assert(specfile != NULL);

    /* if macros are read, return the number we have */
    if (ri->macros && !TAILQ_EMPTY(ri->macros)) {
        TAILQ_FOREACH(pair, ri->macros, items) {
            n++;
        }

        return n;
    }

    /* Read in the spec file first */
    spec = read_file(specfile);
    assert(spec != NULL);

    /* find all the macro lines */
    TAILQ_FOREACH(specline, spec, items) {
        /* we made it to the changelog, ignore everything from here on */
        if (strprefix(specline->data, SPEC_SECTION_CHANGELOG)) {
            break;
        }

        /* skip non-macro definition lines */
        if (!strstr(specline->data, SPEC_MACRO_DEFINE" ") && !strstr(specline->data, SPEC_MACRO_GLOBAL" ")) {
            continue;
        }

        /* trim line endings */
        specline->data[strcspn(specline->data, "\r\n")] = 0;

        /* break up fields */
        fields = strsplit(specline->data, " ");

        if (list_len(fields) != 3) {
            /* not a macro line */
            DEBUG_PRINT("ignoring macro line (possibly a function): '%s'\n", specline->data);
            list_free(fields, free);
            continue;
        }

        /* initialize the macros list if necessary */
        if (ri->macros == NULL) {
            ri->macros = calloc(1, sizeof(*ri->macros));
            assert(ri->macros != NULL);
            TAILQ_INIT(ri->macros);
        }

        /* add the macro */
        pair = calloc(1, sizeof(*pair));
        assert(pair != NULL);

        /* verify the first element is %define or %global */
        entry = TAILQ_FIRST(fields);
        assert((strcmp(entry->data, SPEC_MACRO_DEFINE) == 0 || strcmp(entry->data, SPEC_MACRO_GLOBAL) == 0));
        TAILQ_REMOVE(fields, entry, items);
        free(entry->data);
        free(entry);

        /* the macro name */
        entry = TAILQ_FIRST(fields);

        if (strsuffix(entry->data, ")")) {
            /* ignore macro functions */
            list_free(fields, free);
            continue;
        }

        TAILQ_REMOVE(fields, entry, items);
        pair->key = entry->data;
        free(entry);

        /* the macro value */
        entry = TAILQ_FIRST(fields);
        TAILQ_REMOVE(fields, entry, items);
        pair->value = entry->data;
        free(entry);

        DEBUG_PRINT("adding macro '%s' with value=|%s|\n", pair->key, pair->value);
        TAILQ_INSERT_TAIL(ri->macros, pair, items);
        n++;
        free(fields);
    }

    /* clean up */
    list_free(spec, free);

    return n;
}

/**
 * @brief Given a string, collect any RPM spec file macros used in the
 * string.  Return a string_list_t containing the macros found.  In
 * the case of conditional macros, the leading '?' is excluded.
 * Macros are expressed as %{macroname} or %{?macroname}.  For
 * example, the following string:
 *
 *     %{main_release}.%{pre_release}%{?dist}
 *
 * Returns a string_list_t with members "main_release", "pre_release",
 * and "dist".  The caller is responsible for freeing the returned
 * string_list_t.
 *
 * @param s String to scan for macros.
 * @return List of found macro names.
 */
string_list_t *get_macros(const char *s)
{
    bool found = false;
    string_list_t *fields = NULL;
    string_entry_t *entry = NULL;
    string_list_t *macros = NULL;
    string_entry_t *newmacro = NULL;

    if (s == NULL) {
        return NULL;
    }

    /* first split on all braces */
    fields = strsplit(s, "{}");

    if (list_len(fields) == 0) {
        return NULL;
    }

    /* collect all the macro names */
    TAILQ_FOREACH(entry, fields, items) {
        if (!strcmp(entry->data, "%")) {
            found = true;
            continue;
        }

        if (found) {
            if (macros == NULL) {
                macros = calloc(1, sizeof(*macros));
                TAILQ_INIT(macros);
            }

            newmacro = calloc(1, sizeof(*newmacro));

            /* macros might be conditional, ignore the '?' */
            if (entry->data && entry->data[0] == '?') {
                newmacro->data = strdup(entry->data + 1);
            } else {
                newmacro->data = strdup(entry->data);
            }

            DEBUG_PRINT("newmacro=|%s|\n", newmacro->data);
            TAILQ_INSERT_TAIL(macros, newmacro, items);
            found = false;
        }
    }

    list_free(fields, free);
    return macros;
}