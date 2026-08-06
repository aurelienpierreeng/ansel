/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef DT_COMMON_FOLDER_SURVEY_H
#define DT_COMMON_FOLDER_SURVEY_H

#include <glib.h>

/** Conf key holding the ordered list of style names auto-applied to studio
 * captures, separated by DT_FOLDER_SURVEY_STYLES_SEPARATOR. Style names may
 * contain any printable character, so the separator is a control character
 * that cannot appear in a name and stays on one line in the conf file. */
#define DT_FOLDER_SURVEY_STYLES_CONF_KEY "studio_capture/styles"
#define DT_FOLDER_SURVEY_STYLES_SEPARATOR "\x1f"

/**
 * @brief Initialize the folder survey state from the persisted configuration.
 *
 * Monitoring is NOT started: it must be requested with dt_folder_survey_start()
 * or through dt_folder_survey_propose_resume().
 */
void dt_folder_survey_init();

/**
 * @brief Validate the persisted configuration and start monitoring.
 *
 * All settings are read from the `folder_survey` conf keys. Validation
 * failures are reported through dt_control_log().
 *
 * @return int 0 when monitoring started, non-zero on invalid configuration.
 */
int dt_folder_survey_start();

/**
 * @brief User-requested stop: end monitoring and clear the persisted session
 * marker so the next application start does not propose to resume.
 */
void dt_folder_survey_halt();

/**
 * @brief TRUE while the periodic folder scan is running.
 */
gboolean dt_folder_survey_is_active();

/**
 * @brief Check, without any side effect, whether the persisted configuration
 * has everything dt_folder_survey_start() requires to succeed.
 *
 * @param message receives a translated, static explanation when the
 * configuration is incomplete or invalid; left untouched otherwise. Safe to
 * display directly in the GUI (e.g. as a status label or tooltip).
 * @return gboolean TRUE when dt_folder_survey_start() would succeed.
 */
gboolean dt_folder_survey_can_start(const char **message);

/**
 * @brief TRUE when the previous application session quit while monitoring.
 */
gboolean dt_folder_survey_session_was_active();

/**
 * @brief Build the expanded destination path of a sample file from the current
 * configuration, for GUI preview purposes.
 *
 * @return char* newly allocated preview path, or NULL when the configuration
 * cannot produce a valid destination. Free with dt_free().
 */
char *dt_folder_survey_destination_preview();

/**
 * @brief Count files in the surveyed folder that are unknown to the persisted
 * baseline, i.e. files that appeared while the application was closed.
 */
int dt_folder_survey_count_new_files();

/**
 * @brief Record every file currently in the surveyed folder as already handled,
 * so restarting the survey will not import them.
 */
void dt_folder_survey_absorb_new_files();

/**
 * @brief Propose to resume an interrupted studio session at application start.
 *
 * When the previous session quit while monitoring, ask the user whether to
 * resume; on acceptance, switch to the Studio Capture view, then ask whether
 * files that appeared meanwhile should be imported now (with an optional
 * delete-source-after-verified-copy). Call after the GUI and views are ready.
 *
 * @return gboolean always G_SOURCE_REMOVE, so it can be scheduled with g_idle_add().
 */
gboolean dt_folder_survey_propose_resume();

/**
 * @brief Stop new scans before control workers begin shutting down.
 *
 * The persisted session marker keeps its current state so an active session
 * can be proposed for resume on the next start.
 */
void dt_folder_survey_stop();

/**
 * @brief Release folder survey state after control workers have stopped.
 */
void dt_folder_survey_cleanup();
#endif // DT_COMMON_FOLDER_SURVEY_H
