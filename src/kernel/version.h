/*
 * VibeCagOS — single source of truth for the version strings.
 * (Previously duplicated in kernel.c and procfs.c and hard-coded a third time;
 * tests/run.sh greps the user-visible form, so keep them in lock-step here.)
 */
#ifndef VIBECAGOS_VERSION_H
#define VIBECAGOS_VERSION_H

#define VIBECAGOS_VERSION "0.6.0"
#define VIBECAGOS_BUILD   "2026-10-03"

#endif
