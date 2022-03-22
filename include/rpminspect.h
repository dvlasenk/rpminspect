/*
 * Copyright 2019 Red Hat, Inc.
 * Author(s): David Cantrell <dcantrell@redhat.com>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see
 * <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * This header includes the API definition for librpminspect.
 */

#include <stdbool.h>
#include <sys/types.h>
#include <signal.h>
#include <regex.h>
#include <rpm/header.h>
#include <curl/curl.h>

#ifdef _WITH_LIBCAP
#include <sys/capability.h>
#endif

#include "constants.h"
#include "types.h"
#include "inspect.h"
#include "results.h"
#include "output.h"
#include "readelf.h"

#ifndef _LIBRPMINSPECT_RPMINSPECT_H
#define _LIBRPMINSPECT_RPMINSPECT_H

/* Debugging mode toggle */
extern bool debug_mode;

/* List of all inspections (inspect.c) */
extern struct inspect inspections[];

/* List of all output format types (output.c) */
extern struct format formats[];

/* Terminal resize indicator */
extern volatile sig_atomic_t terminal_resized;

/* Macros */
#ifdef NDEBUG
/* Don't create unused variables if not using assert() */
#define xasprintf(dest, ...) {                         \
    *(dest) = NULL;                                    \
    asprintf((dest), __VA_ARGS__);                     \
}
#else
#define xasprintf(dest, ...) {                         \
    int _xasprintf_result;                             \
    *(dest) = NULL;                                    \
    _xasprintf_result = asprintf((dest), __VA_ARGS__); \
    assert(_xasprintf_result != -1);                   \
}
#endif

#ifdef GETTEXT_DOMAIN
#include <libintl.h>
#include <locale.h>

#define _(STRING) gettext(STRING)
#define N_(STRING) ngettext(STRING)
#else
#define _(STRING) STRING
#define N_(STRING) STRING
#endif

/*
 * Simple debugging printf.  Sends output to stderr if debugging
 * mode is enabled at runtime.
 */
#define DEBUG_PRINT(...)                                         \
    if (debug_mode) {                                            \
        fprintf(stderr, "debug: %s (%d): ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                            \
        fflush(stderr);                                          \
    }

/*
 * Types of exit codes from the program.
 */
enum {
    RI_SUCCESS = 0,              /* everything ok */
    RI_INSPECTION_FAILURE = 1,   /* inspections failed */
    RI_PROGRAM_ERROR = 2,        /* program errored in some way */
    RI_MISSING_PROFILE = 3,      /* specified profile not found */
    RI_INSUFFICIENT_SPACE = 4    /* insufficient disk space */
};

/*
 * Build identifier strings (used in paths)
 * The enum values map to the build_desc array index.
 */
enum { BEFORE_BUILD, AFTER_BUILD };

/*
 * Supported checksum types.
 */
enum checksum {
    NULLSUM,
    MD5SUM,
    SHA1SUM,
    SHA224SUM,
    SHA256SUM,
    SHA384SUM,
    SHA512SUM
};

/* Common functions */

/* init.c */
bool init_fileinfo(struct rpminspect *);
bool init_caps(struct rpminspect *);
bool init_rebaseable(struct rpminspect *);
bool init_politics(struct rpminspect *ri);
bool init_security(struct rpminspect *ri);
bool init_icons(struct rpminspect *ri);
struct rpminspect *init_rpminspect(struct rpminspect *, const char *, const char *);

/* free.c */
void free_string_map(string_map_t *table);
void free_regex(regex_t *regex);
void free_rpminspect(struct rpminspect *);
void free_deprules(deprule_list_t *list);

/* pairfuncs.c */
void free_pair(pair_list_t *list);

/* listfuncs.c */
char *list_to_string(const string_list_t *, const char *);
char **list_to_array(const string_list_t *);
string_map_t * list_to_table(const string_list_t *);
string_list_t * list_difference(const string_list_t *, const string_list_t *);
string_list_t * list_intersection(const string_list_t *, const string_list_t *);
string_list_t * list_union(const string_list_t *, const string_list_t *);
string_list_t * list_symmetric_difference(const string_list_t *, const string_list_t *);
typedef void (*list_entry_data_free_func)(void *);
void list_free(string_list_t *, list_entry_data_free_func);
size_t list_len(const string_list_t *);
string_list_t * list_sort(const string_list_t *);
string_list_t * list_copy(const string_list_t *);
string_list_t *list_from_array(const char **);
bool list_contains(const string_list_t *, const char *);
string_list_t *list_add(string_list_t *list, const char *s);

/* local.c */
bool is_local_build(const char *workdir, const char *build, const bool fetch_only);
bool is_local_rpm(struct rpminspect *, const char *);

/* koji.c */
koji_buildlist_t *init_koji_buildlist(void);
void free_koji_buildlist(koji_buildlist_t *);
koji_rpmlist_t *init_koji_rpmlist(void);
void free_koji_rpmlist_entry(koji_rpmlist_entry_t *);
void free_koji_rpmlist(koji_rpmlist_t *);
void init_koji_build(struct koji_build *);
void free_koji_build(struct koji_build *);
void init_koji_descendent(koji_task_entry_t *);
void init_koji_task(struct koji_task *);
void free_koji_task_entry(koji_task_entry_t *);
void free_koji_task(struct koji_task *);
struct koji_build *get_koji_build(struct rpminspect *, const char *);
struct koji_task *get_koji_task(struct rpminspect *, const char *);
string_list_t *get_all_arches(const struct rpminspect *);
bool allowed_arch(const struct rpminspect *, const char *);

/* kmods.c */
#ifdef _WITH_LIBKMOD
bool compare_module_parameters(const struct kmod_list *, const struct kmod_list *, string_list_t **, string_list_t **);
bool compare_module_dependencies(const struct kmod_list *, const struct kmod_list *, string_list_t **, string_list_t **);
kernel_alias_data_t *gather_module_aliases(const char *module_name, const struct kmod_list *modinfo_list);
void free_module_aliases(kernel_alias_data_t *);
bool compare_module_aliases(kernel_alias_data_t *, kernel_alias_data_t *, module_alias_callback, void *);
string_list_t *get_kmod_values(const char *, const char *);
#endif

/* mkdirp.c */
int mkdirp(const char *, mode_t);

/* rmtree.c */
int rmtree(const char *, const bool, const bool);

/* strfuncs.c */
bool strprefix(const char *, const char *);
bool strsuffix(const char *, const char *);
int printwrap(const char *, const size_t, const unsigned int, FILE *);
bool versioned_match(const char *, Header, const char *, Header);
char *strseverity(const severity_t);
severity_t getseverity(const char *, const severity_t);
char *strwaiverauth(const waiverauth_t);
char *strreplace(const char *, const char *, const char *);
char *strxmlescape(const char *);
char *strappend(char *, ...);
string_list_t *strsplit(const char *, const char *);
const char *strtype(const mode_t mode);
char *strshorten(const char *s, size_t width);

/**
 * @brief Return descriptive string for the given exit code.
 *
 * Return the descriptive string for the specified program exit code.
 * Unknown exit codes return "".  The caller must not free this
 * string.
 *
 * @param exitcode The program exit code.
 * @return Descriptive string for the specified exit code (do not
 *         free!)
 */
const char *strexitcode(int exitcode);

/* badwords.c */
bool has_bad_word(const char *, const string_list_t *);

/* copyfile.c */
int copyfile(const char *, const char *, bool, bool);

/* rpm.c */
int init_librpm(struct rpminspect *ri);
Header get_rpm_header(struct rpminspect *, const char *);
char *get_rpmtag_str(Header, rpmTagVal);
char *get_nevr(Header);
char *get_nevra(Header);
const char *get_rpm_header_arch(Header);
string_list_t *get_rpm_header_string_array(Header h, rpmTagVal tag);
char *get_rpm_header_value(const rpmfile_entry_t *file, rpmTag tag);

/* peers.c */
rpmpeer_t *init_rpmpeer(void);
void free_rpmpeer(rpmpeer_t *);
void add_peer(rpmpeer_t **, int, bool, const char *, Header);

/**
 * @brief Iterate over all packages and extract them.
 *
 * Called internally when the program has verified enough disk space
 * exists to extract the RPM packages and begin inspections.  The
 * extraction step previously happened in add_peer() but has been
 * moved to this function to allow for a disk space check before
 * extraction begins.  If fetchonly is true, this function is a no-op
 * and returns 0 immediately.
 *
 * @param ri The main rpminspect object
 * @param fetchonly True if rpminspect is running in fetch-only mode
 * @return 0 on success, -1 on failure
 */
int extract_peers(struct rpminspect *ri, bool fetchonly);

/* files.c */
void free_files(rpmfile_t *files);
rpmfile_t * extract_rpm(const char *, Header, char **output_dir);
bool process_file_path(const rpmfile_entry_t *, regex_t *, regex_t *);
void find_file_peers(rpmfile_t *, rpmfile_t *);
bool is_debug_or_build_path(const char *);

/* tty.c */
size_t tty_width(void);

/* results.c */
void init_result_params(struct result_params *);
results_t *init_results(void);
void free_results(results_t *);
void add_result_entry(results_t **, struct result_params *);
void add_result(struct rpminspect *, struct result_params *);
bool suppressed_results(const results_t *results, const char *header, const severity_t suppress);

/* output.c */
const char *format_desc(unsigned int);

/* output_text.c */
void output_text(const results_t *, const char *, const severity_t, const severity_t);

/* output_json.c */
void output_json(const results_t *, const char *, const severity_t, const severity_t);

/* output_xunit.c */
void output_xunit(const results_t *, const char *, const severity_t, const severity_t);

/* output_summary.c */
void output_summary(const results_t *results, const char *dest, const severity_t threshold, const severity_t suppress);

/* unpack.c */
int unpack_archive(const char *, const char *, const bool);

/* magic.c */
char *mime_type(const char *);
char *get_mime_type(rpmfile_entry_t *);
bool is_text_file(rpmfile_entry_t *);

/* checksums.c */
char *compute_checksum(const char *, mode_t *, enum checksum);
char *checksum(rpmfile_entry_t *);

/* runcmd.c */
char *run_cmd_vpe(int *exitcode, const char *workdir, char **argv);
char *run_cmd(int *, const char *, const char *, ...) __attribute__((__sentinel__));
void free_argv_table(struct rpminspect *ri, string_list_map_t *table);
char **build_argv(const char *cmd);
void free_argv(char **argv);

/* fileinfo.c */
bool match_fileinfo_mode(struct rpminspect *, const rpmfile_entry_t *, const char *, const char *);
bool match_fileinfo_owner(struct rpminspect *, const rpmfile_entry_t *, const char *, const char *, const char *, const char *);
bool match_fileinfo_group(struct rpminspect *, const rpmfile_entry_t *, const char *, const char *, const char *, const char *);
caps_filelist_entry_t *get_caps_entry(struct rpminspect *, const char *, const char *);

/* flags.c */
bool process_inspection_flag(const char *, const bool, uint64_t *);

/* debug.c */
void set_debug_mode(bool);
void dump_cfg(const struct rpminspect *);

/* readfile.c */
void *read_file_bytes(const char *path, off_t *len);
string_list_t *read_file(const char *);

/* release.c */
char *read_release(const rpmfile_t *);
const char *get_before_rel(struct rpminspect *);
const char *get_after_rel(struct rpminspect *);

/* builds.c */
int gather_builds(struct rpminspect *, bool);

/* macros.c */
void load_macros(struct rpminspect *ri);
string_list_t *get_macros(const char *);
int get_specfile_macros(struct rpminspect *, const char *);

/* inspect_elf.c */
/*
 * NOTE: these functions are not static so we can more easily have
 * unit tests for them in the test suite.
 */
bool is_execstack_present(Elf *elf);
bool is_execstack_valid(Elf *elf, uint64_t flags);
bool is_stack_executable(Elf *elf, uint64_t flags);
bool is_pic_ok(Elf *elf);
bool has_bind_now(Elf *elf);
bool has_executable_program(Elf *elf);
bool has_relro(Elf *elf);
uint64_t get_execstack_flags(Elf *elf);
bool has_textrel(Elf *elf);
bool find_no_pic(Elf *elf, string_list_t **user_data);
bool find_pic(Elf *elf, string_list_t **user_data);
bool find_all(Elf *elf, string_list_t **user_data);

/* bytes.c */
/**
 * Given a byte array of a specified length, convert it to a NUL
 * terminated string and return a pointer to the string to the caller.
 * The caller is responsible for freeing the memory associated with
 * this string.
 *
 * @param array The byte array to convert.
 * @param len Number of elements in the byte array.
 * @return Newly allocated string representation of the byte array.
 */
char *bytes_to_str(unsigned char *array, size_t len);

/* paths.c */
/**
 * @brief Return the before build debuginfo package path where the
 * package was extracted for rpminspect.  The path must match the
 * architecture provided.
 *
 * IMPORTANT: Do not free the returned string.
 *
 * @param ri The struct rpminspect for the program.
 * @param binarch The required debuginfo architecture.
 * @param subpkg Optional subpackage name to get the debuginfo for.
 * @return Full path to the extract before build debuginfo package, or
 * NULL if not found.
 */
const char *get_before_debuginfo_path(struct rpminspect *ri, const char *binarch, const char *subpkg);

/**
 * @brief Return the after build debuginfo package path where the
 * package was extracted for rpminspect.  The path must match the
 * architecture provided.
 *
 * IMPORTANT: Do not free the returned string.
 *
 * @param ri The struct rpminspect for the program.
 * @param binarch The required debuginfo architecture.
 * @param subpkg Optional subpackage name to get the debuginfo for.
 * @return Full path to the extract after build debuginfo package, or
 * NULL if not found.
 */
const char *get_after_debuginfo_path(struct rpminspect *ri, const char *binarch, const char *subpkg);
bool usable_path(const char *path);
bool match_path(const char *pattern, const char *root, const char *needle);

/**
 * @brief Given a path and struct rpminspect, determine if the path
 * should be ignored or not.
 *
 * @param ri The struct rpminspect for the program.  @param path The
 * relative path to check (i.e., localpath).  @param root The root
 * directory, optional (pass NULL to use '/').  @return True if path
 * should be ignored, false otherwise.
 */
bool ignore_path(const struct rpminspect *ri, const char *inspection, const char *path, const char *root);

/* rebase.c */
/**
 * @brief Determine if the program is inspecting a rebase build.  That
 * is, the package name of the before and after build match but the
 * versions are different.  If there is no before package, then this
 * function returns true.
 *
 * @param ri The struct rpminspect structure for the program.
 * @return True if the packages are a rebase, false otherwise
 */
bool is_rebase(struct rpminspect *ri);

/* arches.c */
void init_arches(struct rpminspect *ri);

/* abi.c */
void add_abi_argument(string_list_map_t *table, const char *arg, const char *path, const Header hdr);
size_t count_abi_entries(const string_list_t *contents);
abi_t *read_abi(const char *vendor_data_dir, const char *product_release);
void free_abi(abi_t *list);
string_list_t *get_abi_suppressions(const struct rpminspect *ri, const char *suppression_file);
string_list_map_t *get_abi_dir_arg(struct rpminspect *ri, const size_t size, const char *suffix, const char *arg, const char *path, const int type);

/* uncompress.c */
char *uncompress_file(struct rpminspect *ri, const char *infile, const char *subdir);

/* filecmp.c */
int filecmp(const char *x, const char *y);

/* abspath.c */
char *abspath(const char *path);

/* diags.c */
string_list_t *gather_diags(struct rpminspect *ri, const char *progname, const char *progver);

/* secrule.c */
severity_t get_secrule_result_severity(struct rpminspect *ri, const rpmfile_entry_t *file, const int type);

/* deprules.c */
deprule_list_t *gather_deprules(Header hdr);
void find_deprule_peers(deprule_list_t *before, deprule_list_t *after);
const char *get_deprule_desc(const dep_type_t type);
dep_op_t get_dep_operator(const rpmsenseFlags f);
const char *get_deprule_operator_desc(const dep_op_t operator);
bool deprules_match(const deprule_entry_t *a, const deprule_entry_t *b);
char *strdeprule(const deprule_entry_t *deprule);

/* delta.c */
char *get_file_delta(const char *a, const char *b);

/* fs.c */
/**
 * @brief Return available space in bytes for the given path.
 *
 * Simple function that just wraps statvfs() and returns the available
 * disk space for the given path.  On error, you get a warn() and the
 * function returns 0 so the caller needs to handle that.  The reason
 * it returns 0 is that nearly every instance of the caller handling
 * an error from this function would be the same handling as if there
 * was not enough space available.
 *
 * @param path The filesystem path to check.
 * @return Number of bytes available.
 */
unsigned long get_available_space(const char *path);

/* curl.c */
/**
 * @brief Download helper for libcurl
 *
 * Downloads src using libcurl and writes it to dst.  If verbose is
 * true, displays a progress bar reporting the download progress.
 *
 * @param verbose True to display progress bar
 * @param src URL to download
 * @param dst Full path to the local destination (including filename)
 */
void curl_get_file(const bool verbose, const char *src, const char *dst);

/**
 * @brief Get the size of the file at the URL specified
 *
 * Return the size of the file specified by the URL, but do not
 * download it.
 *
 * @param src URL to the file
 * @return The size of the file
 */
curl_off_t curl_get_size(const char *src);

/**
 * @brief Return true if the URL specified is to an RPM package
 *
 * This function just looks at the URL in question and makes sure it's
 * a URL and then the ending to check for '.rpm'.  It's not perfect,
 * but for talking to build systems this is probably sufficient.
 *
 * @param url The URL
 * @return True if the URL is an RPM, false otherwise.
 */
bool is_remote_rpm(const char *url);

/* humansize.c */
/**
 * @brief Return human-readable size for the bytes given.
 *
 * Given a number of bytes, return a human-readable string
 * representing that size.  The caller must free the returned string.
 *
 * @param bytes Number of bytes to convert.
 * @return Allocated string containing human-readable size; caller
 *         must free.
 */
char *human_size(const unsigned long bytes);

#endif
