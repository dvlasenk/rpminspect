/*
 * Copyright (C) 2020  Red Hat, Inc.
 * Author(s):  David Cantrell <dcantrell@redhat.com>
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <limits.h>
#include <rpm/header.h>
#include <rpm/rpmtag.h>
#include <archive.h>
#include "queue.h"
#include "rpminspect.h"

/* Global variables */
static string_list_t *patches = NULL;
static bool initialized = false;
static bool reported = false;
static bool comparison = false;
static bool havedir = false;
static struct result_params params;

/*
 * Create a temporary file containing the patch, uncompress the patch
 * if necessary.  The caller must both unlink the created output file
 * and free the returned string (which is the path to the created
 * output file).
 */
static char *prep_patch(struct rpminspect *ri, const char *patch)
{
    int fd = -1;
    char *output = NULL;
    char *basepatch = NULL;
    static int mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    int r = -1;
    size_t size = 0;
    void *buf[BUFSIZ];
    struct archive *input = NULL;
    struct archive_entry *entry = NULL;

    assert(ri != NULL);
    assert(ri->workdir != NULL);
    assert(patch != NULL);

    /*
     * only create the patches subdirectory if we need it (avoids lots
     * of stat calls)
     */
    if (!havedir) {
        xasprintf(&output, "%s/%s", ri->worksubdir, HEADER_PATCHES);
        assert(output != NULL);
        DEBUG_PRINT("patches subdirectory=|%s|\n", output);

        if (mkdirp(output, mode) == -1) {
            warn("mkdirp(%s)", output);
            free(output);
            return NULL;
        }

        havedir = true;
        free(output);
    }

    /* create the output patch file that will be uncompressed */
    basepatch = rindex(patch, '/') + 1;
    assert(basepatch != NULL);
    xasprintf(&output, "%s/%s/%s.XXXXXX", ri->worksubdir, HEADER_PATCHES, basepatch);
    assert(output != NULL);
    DEBUG_PRINT("temporary patch file=|%s|\n", output);

    fd = mkstemp(output);

    if (fd == -1) {
        warn("mkstemp()");
        free(output);
        return NULL;
    }

    /*
     * Read in the patch file and uncompress it if necessary.  The
     * uncompressed data is written to the output file we created and
     * that is used for later diff(1) calls.  Use libarchive here so
     * we can handle a wide range of compression formats.
     */
    input = archive_read_new();

    /* initialize only compression filters in libarchive */
#if ARCHIVE_VERSION_NUMBER < 3000000
    archive_read_support_compression_bzip2(input);
    archive_read_support_compression_compress(input);
    archive_read_support_compression_gzip(input);
    archive_read_support_compression_grzip(input);
    archive_read_support_compression_lrzip(input);
    archive_read_support_compression_lz4(input);
    archive_read_support_compression_lzma(input);
    archive_read_support_compression_lzop(input);
    archive_read_support_compression_none(input);
#else
    archive_read_support_filter_bzip2(input);
    archive_read_support_filter_compress(input);
    archive_read_support_filter_gzip(input);
    archive_read_support_filter_grzip(input);
    archive_read_support_filter_lrzip(input);
    archive_read_support_filter_lz4(input);
    archive_read_support_filter_lzma(input);
    archive_read_support_filter_lzop(input);
    archive_read_support_filter_none(input);
#endif

    /*
     * add raw and empty to account for uncompressed patches and
     * compressed empty patches.
     */
    archive_read_support_format_raw(input);
    archive_read_support_format_empty(input);

    /* open the patch file, decompress, and write to output */
    r = archive_read_open_filename(input, patch, 16384);

    if (r != ARCHIVE_OK) {
        warn("archive_read_open_filename()");
        close(fd);
        free(output);
        return NULL;
    }

    r = archive_read_next_header(input, &entry);

    if (r != ARCHIVE_OK) {
        warn("archive_read_next_header()");
        close(fd);
        free(output);
        return NULL;
    }

    while (1) {
        size = archive_read_data(input, buf, BUFSIZ);

        if (size == 0) {
            break;
        }

        if (write(fd, buf, size) == -1) {
            warn("write()");
            close(fd);
            free(output);
            return NULL;
        }
    }

    archive_read_free(input);

    /* close up our uncompressed patch file */
    if (close(fd) == -1) {
        warn("close()");
        free(output);
        return NULL;
    }

    return output;
}

/* Returns true if this file is a Patch file */
static bool is_patch(const rpmfile_entry_t *file)
{
    bool ret = false;
    char *shortname = NULL;
    string_entry_t *entry = NULL;

    assert(file != NULL);

    /* Initialize the patches list once for the run */
    if (!initialized) {
        patches = get_rpm_header_string_array(file->rpm_header, RPMTAG_PATCH);
        initialized = true;
    }

    if (patches == NULL) {
        /* source RPM does not contain any Patch entries */
        return false;
    }

    /* The RPM header stores basenames */
    shortname = rindex(file->fullpath, '/') + 1;

    /* See if this file is a Patch file */
    TAILQ_FOREACH(entry, patches, items) {
        if (!strcmp(shortname, entry->data)) {
            return true;
        }
    }

    return ret;
}

/*
 * Take the summary line from diffstat(1) and get the number of files
 * and number of lines modified.  The number of lines is the
 * combination of the number of insertions and deletions.
 */
static diffstat_t get_diffstat_counts(const char *summary)
{
    diffstat_t counts;
    string_list_t *output = NULL;
    string_list_t *fields = NULL;
    string_entry_t *entry = NULL;
    char *s = NULL;
    long int tally = 0;

    assert(summary != NULL);

    /* initialize our counts */
    counts.files = 0;
    counts.lines = 0;

    /* split the output on newlines so we can get the last line */
    output = strsplit(summary, "\n\r");

    if (output == NULL || TAILQ_EMPTY(output)) {
        warn("unable to split diffstat output");
        return counts;
    }

    /*
     * split in to fields that are comma-delimited
     * this string:
     *      | 2 files changed, 2 insertions(+), 2 deletions(-)|
     * will split in to these fields:
     *      | 2 files changed|
     *      | 2 insertions(+)|
     *      | 2 deletions(-)|
     */
    entry = TAILQ_LAST(output, string_entry_s);
    fields = strsplit(entry->data, ",");

    if (fields == NULL || TAILQ_EMPTY(fields)) {
        warn("unable to split diffstat summary line");
        list_free(output, free);
        return counts;
    }

    /* iterate over the fields and add up the counts */
    TAILQ_FOREACH(entry, fields, items) {
        s = entry->data;

        if (strcasestr(s, " changed")) {
            /* handles "file changed" and "files changed" */
            tally = strtol(s, 0, 10);

            if ((tally == LONG_MIN || tally == LONG_MAX) && errno == ERANGE) {
                warn("strtol(%s)", s);
                tally = 0;
            }

            counts.files += tally;
        } else if (strcasestr(s, " insertion") || strcasestr(s, " deletion") || strcasestr(s, " modification")) {
            /* handles all of the variations of line count changes */
            tally = strtol(s, 0, 10);

            if ((tally == LONG_MIN || tally == LONG_MAX) && errno == ERANGE) {
                warn("strtol(%s)", s);
                tally = 0;
            }

            counts.lines += tally;
        }
    }

    /* clean up */
    list_free(output, free);
    list_free(fields, free);

    return counts;
}

/* Main driver for the 'patches' inspection. */
static bool patches_driver(struct rpminspect *ri, rpmfile_entry_t *file)
{
    bool result = true;
    string_entry_t *entry = NULL;
    char *before_patch = NULL;
    char *after_patch = NULL;
    int exitcode;
    char *details = NULL;
    diffstat_t ds;
    struct stat sb;

    /* If we are not looking at a Patch file, bail. */
    if (!is_patch(file)) {
        return true;
    }

    /* Compare digests of source archive */
    params.file = file->localpath;

    /* If this patch is on the ignore list, skip */
    if (ri->patch_ignore_list != NULL && !TAILQ_EMPTY(ri->patch_ignore_list)) {
        TAILQ_FOREACH(entry, ri->patch_ignore_list, items) {
            if (!strcmp(file->localpath, entry->data)) {
                return true;
            }
        }
    }

    /* patches may be compressed, so uncompress them here for diff(1) */
    if (file->peer_file) {
        before_patch = prep_patch(ri, file->peer_file->fullpath);

        if (before_patch == NULL) {
            warn("unable to prepare patch: %s", file->peer_file->localpath);
            return false;
        }
    }

    after_patch = prep_patch(ri, file->fullpath);

    if (after_patch == NULL) {
        warn("unable to prepare patch: %s", file->localpath);
        return false;
    }

    /*
     * Ensure that all patches are at least 4 bytes in size, trapping
     * "empty patch" mistakes that have occurred when people are
     * generating multiple patches against multiple branches.
     */
    if (stat(after_patch, &sb) != 0) {
        warn("stat()");
        return false;
    }

    if (sb.st_size < 4) {
        xasprintf(&params.msg, _("Patch %s is under 4 bytes in size - is it corrupt?"), file->localpath);
        params.severity = RESULT_BAD;
        params.waiverauth = WAIVABLE_BY_ANYONE;
        params.details = NULL;
        params.remedy = REMEDY_PATCHES_CORRUPT;
        params.verb = VERB_FAILED;
        params.noun = _("corrupt patch ${FILE}");
        add_result(ri, &params);
        free(params.msg);

        reported = true;
        result = false;
    }

    /*
     * compare the patches if we have two builds
     * This just reports patches that change content.  It uses the INFO reporting level.
     */
    if (comparison && file->peer_file) {
        params.details = run_cmd(&exitcode, DIFF_CMD, "-q", before_patch, after_patch, NULL);
        free(params.details);
        params.details = NULL;

        if (exitcode) {
            /* the files differ, see if it's only whitespace changes */
            params.details = run_cmd(&exitcode, DIFF_CMD, "-u", "-w", "-I^#.*", before_patch, after_patch, NULL);

            if (exitcode) {
                /* more than whitespace changed */
                xasprintf(&params.msg, _("%s changed (%ld bytes -> %ld bytes)"), file->localpath, file->peer_file->st.st_size, file->st.st_size);
                params.severity = RESULT_INFO;
                params.waiverauth = NOT_WAIVABLE;
                params.verb = VERB_CHANGED;
                params.noun = _("patch file ${FILE}");

                /* use friendly names for the files in the diff(1) details */
                details = strreplace(params.details, before_patch, file->peer_file->localpath);
                assert(details != NULL);
                free(params.details);
                params.details = strreplace(details, after_patch, file->localpath);
                free(details);
                assert(params.details != NULL);

                /* report the findings */
                add_result(ri, &params);
                free(params.details);
                free(params.msg);
                params.details = NULL;
                params.msg = NULL;

                reported = true;
                result = true;
            }
        }
    } else if (comparison && file->peer_file == NULL) {
        xasprintf(&params.msg, _("New patch file `%s` appeared"), params.file);
        params.verb = VERB_ADDED;
        params.noun = _("patch file ${FILE}");
        add_result(ri, &params);
        free(params.msg);
        params.msg = NULL;

        reported = true;
        result = false;
    }

    /*
     * Collect diffstat(1) data and report based on thresholds.
     */
    params.details = run_cmd(&exitcode, DIFFSTAT_CMD, after_patch, NULL);

    if (exitcode == 0 && params.details != NULL) {
        ds = get_diffstat_counts(params.details);

        if ((ds.files > ri->patch_file_threshold) || (ds.lines > ri->patch_line_threshold)) {
            params.severity = RESULT_VERIFY;
            params.waiverauth = WAIVABLE_BY_ANYONE;
        }

        xasprintf(&params.msg, _("%s touches %ld file%s and as many as %ld line%s"), file->localpath, ds.files, (ds.files > 1) ? "s" : "", ds.lines, (ds.lines > 1) ? "s" : "");
        params.verb = VERB_CHANGED;
        params.noun = _("patch changes ${FILE}");
        add_result(ri, &params);
        free(params.msg);
        free(params.details);
        params.msg = NULL;
        params.details = NULL;

        reported = true;
    } else if (exitcode) {
        warn("unable to run %s on %s", DIFFSTAT_CMD, file->localpath);
        free(params.details);
        params.details = NULL;
    }

    /* clean up */
    free(before_patch);
    free(after_patch);

    return result;
}

/*
 * Main driver for the 'patches' inspection.
 */
bool inspect_patches(struct rpminspect *ri)
{
    bool result = true;
    bool have_source = false;
    rpmpeer_entry_t *peer = NULL;
    rpmfile_entry_t *file = NULL;

    assert(ri != NULL);

    init_result_params(&params);
    params.header = HEADER_PATCHES;

    /* Check for source package */
    TAILQ_FOREACH(peer, ri->peers, items) {
        if (headerIsSource(peer->after_hdr)) {
            have_source = true;

            if (headerIsSource(peer->before_hdr)) {
                comparison = true;
            }

            break;
        }
    }

    /* If no source found, we are not looking at source packages */
    if (!have_source) {
        params.severity = RESULT_INFO;
        params.waiverauth = NOT_WAIVABLE;
        xasprintf(&params.msg, _("No source packages available, skipping inspection."));
        add_result(ri, &params);
        free(params.msg);
        return result;
    }

    /* Set result type based on version difference */
    if (is_rebase(ri) || !comparison) {
        /* versions changed */
        params.severity = RESULT_INFO;
        params.waiverauth = NOT_WAIVABLE;
        params.remedy = NULL;
    } else {
        /* versions are the same, likely maintenance */
        params.severity = RESULT_VERIFY;
        params.waiverauth = WAIVABLE_BY_ANYONE;
        params.remedy = NULL;
    }

    /* Run the main inspection */
    TAILQ_FOREACH(peer, ri->peers, items) {
        /* Only look at the files in SRPMs */
        if (!headerIsSource(peer->after_hdr)) {
            continue;
        }

        /* On the off chance the SRPM is empty, just ignore */
        if (peer->after_files == NULL || TAILQ_EMPTY(peer->after_files)) {
            continue;
        }

        /* Iterate over the SRPM files */
        TAILQ_FOREACH(file, peer->after_files, items) {
            if (!patches_driver(ri, file)) {
                result = false;
            }
        }

        /* Report any removed patch files from the SRPM */
        if (peer->before_files) {
            TAILQ_FOREACH(file, peer->before_files, items) {
                if (file->peer_file == NULL) {
                    xasprintf(&params.msg, _("Patch file `%s` removed"), file->localpath);
                    add_result(ri, &params);
                    free(params.msg);
                    result = false;
                }
            }
        }
    }

    /* Sound the everything-is-ok alarm if everything is, in fact, ok */
    if (result && !reported) {
        params.severity = RESULT_OK;
        params.waiverauth = NOT_WAIVABLE;
        params.remedy = NULL;
        add_result(ri, &params);
    }

    /* Our static list of PatchN: spec file members, dump it */
    list_free(patches, free);

    return result;
}
