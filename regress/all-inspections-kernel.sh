#!/bin/sh
#
# Find the two most recent kernel builds for the latest Fedora tag,
# download them, then run all rpminspect inspections on those kernel
# builds and log the entire run.  The purpose of this script is to
# give a general idea of expected runtime on the inspections and
# downloading.
#
# Copyright David Cantrell <dcantrell@redhat.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#

PATH=/bin:/usr/bin
PKG=kernel
CWD="$(pwd)"
RPMINSPECT=${RPMINSPECT:-rpminspect}
TIMECMD="/usr/bin/time"
TIMEOPTS="-v"
TMPDIR="$(mktemp -d -p /var/tmp -t "$(basename "$0" .sh)".XXXXXX)"
trap 'rm -rf "${TMPDIR}"' EXIT

# Make sure we have additional commands available
for cmd in ${RPMINSPECT} ${TIMECMD} koji ; do
    ${cmd} --help >&- 2>&-
    if [ $? -eq 127 ]; then
        echo "*** Missing ${cmd}, exiting." >&2
        exit 1
    fi
done

# Determine the latest Fedora build tag
LATEST_TAG="$(koji list-tags | grep -E '^f[0-9]+$' | sort | tail -n 1)"

# Spelling change to map the koji tag to the dist tag
DIST_TAG="$(echo "${LATEST_TAG}" | sed -e 's/f/fc/g')"

# Get the builds
BEFORE_BUILD="$(koji list-builds --package=${PKG} | grep "\.${DIST_TAG}" | grep -E ' COMPLETE$' | tail -n 2 | head -n 1 | cut -d ' ' -f 1)"
AFTER_BUILD="$(koji list-builds --package=${PKG} | grep "\.${DIST_TAG}" | grep -E ' COMPLETE$' | tail -n 1 | cut -d ' ' -f 1)"

# Fetch builds so they don't have to be downloaded for each inspection
cd "${TMPDIR}" || exit
${TIMECMD} ${TIMEOPTS} "${RPMINSPECT}" -f -v -w "${TMPDIR}" "${BEFORE_BUILD}" 2>&1 | tee "${CWD}"/"${PKG}"-download-before-ALL.log
${TIMECMD} ${TIMEOPTS} "${RPMINSPECT}" -f -v -w "${TMPDIR}" "${AFTER_BUILD}" 2>&1 | tee "${CWD}"/"${PKG}"-download-after-ALL.log

# Run and log the inspections
${TIMECMD} ${TIMEOPTS} "${RPMINSPECT}" -T ALL -v "${BEFORE_BUILD}" "${AFTER_BUILD}" 2>&1 | tee "${CWD}"/"${PKG}"-ALL.log
