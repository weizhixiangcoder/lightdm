/*
 * Copyright (C) 2010-2011 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <grp.h>

#include "session.h"
#include "console-kit.h"

struct SessionPrivate
{
    /* File to log to */
    gchar *log_file;

    /* TRUE if the log file should be owned by the user */
    gboolean log_file_as_user;

    /* Authentication for this session */
    PAMSession *authentication;

    /* Command to run for this session */
    gchar *command;

    /* ConsoleKit parameters for this session */
    GHashTable *console_kit_parameters;

    /* ConsoleKit cookie for the session */
    gchar *console_kit_cookie;

    /* TRUE if this is a greeter session */
    gboolean is_greeter;
};

G_DEFINE_TYPE (Session, session, PROCESS_TYPE);

void
session_set_log_file (Session *session, const gchar *filename, gboolean as_user)
{
    g_return_if_fail (session != NULL);
    g_free (session->priv->log_file);
    session->priv->log_file = g_strdup (filename);
    session->priv->log_file_as_user = as_user;
}

const gchar *
session_get_log_file (Session *session)
{
    g_return_val_if_fail (session != NULL, NULL);
    return session->priv->log_file;
}

void
session_set_authentication (Session *session, PAMSession *authentication)
{
    g_return_if_fail (session != NULL);
    session->priv->authentication = g_object_ref (authentication);
}

PAMSession *
session_get_authentication (Session *session)
{
    g_return_val_if_fail (session != NULL, NULL);
    return session->priv->authentication;
}

User *
session_get_user (Session *session)
{
    g_return_val_if_fail (session != NULL, NULL);
    return pam_session_get_user (session->priv->authentication);
}

void
session_set_is_greeter (Session *session, gboolean is_greeter)
{
    g_return_if_fail (session != NULL);
    session->priv->is_greeter = is_greeter;
}

gboolean
session_get_is_greeter (Session *session)
{
    g_return_val_if_fail (session != NULL, FALSE);
    return session->priv->is_greeter;
}

void
session_set_command (Session *session, const gchar *command)
{
    g_return_if_fail (session != NULL);

    g_free (session->priv->command);
    session->priv->command = g_strdup (command);
}

const gchar *
session_get_command (Session *session)
{
    g_return_val_if_fail (session != NULL, NULL);
    return session->priv->command;
}

static gchar *
get_absolute_command (const gchar *command)
{
    gchar **tokens;
    gchar *absolute_binary, *absolute_command = NULL;

    tokens = g_strsplit (command, " ", 2);

    absolute_binary = g_find_program_in_path (tokens[0]);
    if (absolute_binary)
    {
        if (tokens[1])
            absolute_command = g_strjoin (" ", absolute_binary, tokens[1], NULL);
        else
            absolute_command = g_strdup (absolute_binary);
    }
    g_free (absolute_binary);

    g_strfreev (tokens);

    return absolute_command;
}

static void
set_env_from_authentication (Session *session, PAMSession *authentication)
{
    gchar **pam_env;

    pam_env = pam_session_get_envlist (authentication);
    if (pam_env)
    {
        gchar *env_string;      
        int i;

        env_string = g_strjoinv (" ", pam_env);
        g_debug ("PAM returns environment '%s'", env_string);
        g_free (env_string);

        for (i = 0; pam_env[i]; i++)
        {
            gchar **pam_env_vars = g_strsplit (pam_env[i], "=", 2);
            if (pam_env_vars && pam_env_vars[0] && pam_env_vars[1])
                session_set_env (session, pam_env_vars[0], pam_env_vars[1]);
            else
                g_warning ("Can't parse PAM environment variable %s", pam_env[i]);
            g_strfreev (pam_env_vars);
        }
        g_strfreev (pam_env);
    }
}

void
session_set_env (Session *session, const gchar *name, const gchar *value)
{
    g_return_if_fail (session != NULL);
    g_return_if_fail (name != NULL);
    process_set_env (PROCESS (session), name, value);
}

const gchar *
session_get_env (Session *session, const gchar *name)
{
    g_return_val_if_fail (session != NULL, NULL);
    g_return_val_if_fail (name != NULL, NULL);
    return process_get_env (PROCESS (session), name);
}

void
session_set_console_kit_parameter (Session *session, const gchar *name, GVariant *value)
{
    g_return_if_fail (session != NULL);
    g_return_if_fail (name != NULL);

    g_hash_table_insert (session->priv->console_kit_parameters,
                         g_strdup (name), g_variant_ref_sink (value));
}

const gchar *
session_get_console_kit_cookie (Session *session)
{
    g_return_val_if_fail (session != NULL, NULL);
    return session->priv->console_kit_cookie;
}

/* Set the LANG variable based on the chosen locale.  This is not a great
 * solution, as it will override the locale set in PAM (which is where it
 * should be set).  In the case of Ubuntu these will be overridden by setting
 * these variables in ~/.profile */
static void
set_locale (Session *session)
{
    User *user;
    const gchar *locale;

    user = pam_session_get_user (session->priv->authentication);
    locale = user_get_locale (user);
    if (locale)
    {
        g_debug ("Using locale %s", locale);
        session_set_env (session, "LANG", locale);
    }
}

/* Insert our own utility directory to PATH
 * This is to provide gdmflexiserver which provides backwards compatibility
 * with GDM.
 * Must be done after set_env_from_authentication because PAM sets PATH.
 * This can be removed when this is no longer required.
 */
static void
insert_utility_path (Session *session)
{
    const gchar *orig_path;

    orig_path = session_get_env (session, "PATH");
    if (orig_path)
    {
        gchar *path = g_strdup_printf ("%s:%s", PKGLIBEXEC_DIR, orig_path);
        session_set_env (session, "PATH", path);
        g_free (path);
    }
}

gboolean
session_start (Session *session)
{
    User *user;

    g_return_val_if_fail (session != NULL, FALSE);
    g_return_val_if_fail (session->priv->authentication != NULL, FALSE);
    g_return_val_if_fail (session->priv->command != NULL, FALSE);

    g_debug ("Launching session");

    user = pam_session_get_user (session->priv->authentication);
  
    /* Set POSIX variables */
    session_set_env (session, "PATH", "/usr/local/bin:/usr/bin:/bin");
    session_set_env (session, "USER", user_get_name (user));
    session_set_env (session, "LOGNAME", user_get_name (user));
    session_set_env (session, "HOME", user_get_home_directory (user));
    session_set_env (session, "SHELL", user_get_shell (user));

    return SESSION_GET_CLASS (session)->start (session);
}

static gboolean
session_real_start (Session *session)
{
    gboolean result;
    gchar *absolute_command;

    absolute_command = get_absolute_command (session->priv->command);
    if (!absolute_command)
    {
        g_debug ("Can't launch session %s, not found in path", session->priv->command);
        return FALSE;
    }
    process_set_command (PROCESS (session), absolute_command);
    g_free (absolute_command);

    pam_session_open (session->priv->authentication);

    /* Open ConsoleKit session */
    if (getuid () == 0)
    {
        GVariantBuilder parameters;
        User *user;
        GHashTableIter iter;
        gpointer key, value;

        user = pam_session_get_user (session->priv->authentication);

        g_variant_builder_init (&parameters, G_VARIANT_TYPE ("(a(sv))"));
        g_variant_builder_open (&parameters, G_VARIANT_TYPE ("a(sv)"));
        g_variant_builder_add (&parameters, "(sv)", "unix-user", g_variant_new_int32 (user_get_uid (user)));
        if (session->priv->is_greeter)
            g_variant_builder_add (&parameters, "(sv)", "session-type", g_variant_new_string ("LoginWindow"));
        g_hash_table_iter_init (&iter, session->priv->console_kit_parameters);
        while (g_hash_table_iter_next (&iter, &key, &value))
            g_variant_builder_add (&parameters, "(sv)", (gchar *) key, (GVariant *) value);

        g_free (session->priv->console_kit_cookie);
        session->priv->console_kit_cookie = ck_open_session (&parameters);
    }
    else
    {
        g_free (session->priv->console_kit_cookie);
        session->priv->console_kit_cookie = g_strdup (g_getenv ("XDG_SESSION_COOKIE"));
    }

    if (session->priv->console_kit_cookie)
        session_set_env (session, "XDG_SESSION_COOKIE", session->priv->console_kit_cookie);

    if (!SESSION_GET_CLASS (session)->setup (session))
        return FALSE;

    result = process_start (PROCESS (session));
  
    if (!result)
    {
        pam_session_close (session->priv->authentication);
        if (getuid () == 0 && session->priv->console_kit_cookie)
            ck_close_session (session->priv->console_kit_cookie);
    }  

    return result;
}

void
session_lock (Session *session)
{    
    g_return_if_fail (session != NULL);
    if (getuid () == 0)
        ck_lock_session (session->priv->console_kit_cookie);
}

void
session_unlock (Session *session)
{    
    g_return_if_fail (session != NULL);
    if (getuid () == 0)
        ck_unlock_session (session->priv->console_kit_cookie);
}

gboolean
session_stop (Session *session)
{
    g_return_val_if_fail (session != NULL, TRUE);

    if (!process_get_is_running (PROCESS (session)))
        return TRUE;

    SESSION_GET_CLASS (session)->cleanup (session);
    process_signal (PROCESS (session), SIGTERM);

    return FALSE;
}

static gboolean
session_setup (Session *session)
{
    return TRUE;
}

static void
session_cleanup (Session *session)
{
}

static void
setup_log_file (Session *session)
{
    int fd;

    /* Redirect output to logfile */
    if (!session->priv->log_file)
        return;

    fd = g_open (session->priv->log_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        g_warning ("Failed to open log file %s: %s", session->priv->log_file, g_strerror (errno));
    else
    {
        dup2 (fd, STDOUT_FILENO);
        dup2 (fd, STDERR_FILENO);
        close (fd);
    }
}

static void
session_run (Process *process)
{
    Session *session = SESSION (process);
    User *user;
    int fd;

    /* Make input non-blocking */
    fd = g_open ("/dev/null", O_RDONLY);
    dup2 (fd, STDIN_FILENO);
    close (fd);

    /* Redirect output to logfile */
    if (!session->priv->log_file_as_user)
        setup_log_file (session);

    /* Make this process its own session */
    if (setsid () < 0)
        g_warning ("Failed to make process a new session: %s", strerror (errno));

    user = pam_session_get_user (session->priv->authentication);

    /* Change working directory */
    if (chdir (user_get_home_directory (user)) != 0)
    {
        g_warning ("Failed to change to home directory %s: %s", user_get_home_directory (user), strerror (errno));
        _exit (EXIT_FAILURE);
    }

    /* Change to this user */
    if (getuid () == 0)
    {
        if (initgroups (user_get_name (user), user_get_gid (user)) < 0)
        {
            g_warning ("Failed to initialize supplementary groups for %s: %s", user_get_name (user), strerror (errno));
            _exit (EXIT_FAILURE);
        }

        if (setgid (user_get_gid (user)) != 0)
        {
            g_warning ("Failed to set group ID to %d: %s", user_get_gid (user), strerror (errno));
            _exit (EXIT_FAILURE);
        }

        if (setuid (user_get_uid (user)) != 0)
        {
            g_warning ("Failed to set user ID to %d: %s", user_get_uid (user), strerror (errno));
            _exit (EXIT_FAILURE);
        }
    }

    /* Redirect output to logfile */
    if (session->priv->log_file_as_user)
        setup_log_file (session);

    /* Do PAM actions requiring session process */
    pam_session_setup (session->priv->authentication);
    set_env_from_authentication (session, session->priv->authentication);
    set_locale (session);
    insert_utility_path (session);

    PROCESS_CLASS (session_parent_class)->run (process);
}

static void
session_stopped (Process *process)
{
    Session *session = SESSION (process);

    pam_session_close (session->priv->authentication);
    if (getuid () == 0 && session->priv->console_kit_cookie)
        ck_close_session (session->priv->console_kit_cookie);

    PROCESS_CLASS (session_parent_class)->stopped (process);
}

static void
session_init (Session *session)
{
    session->priv = G_TYPE_INSTANCE_GET_PRIVATE (session, SESSION_TYPE, SessionPrivate);
    session->priv->console_kit_parameters = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_variant_unref);
    process_set_clear_environment (PROCESS (session), TRUE);
}

static void
session_finalize (GObject *object)
{
    Session *self;

    self = SESSION (object);

    g_free (self->priv->log_file);
    if (self->priv->authentication)
        g_object_unref (self->priv->authentication);
    g_free (self->priv->command);
    g_hash_table_unref (self->priv->console_kit_parameters);
    g_free (self->priv->console_kit_cookie);

    G_OBJECT_CLASS (session_parent_class)->finalize (object);
}

static void
session_class_init (SessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    ProcessClass *process_class = PROCESS_CLASS (klass);

    klass->start = session_real_start;
    klass->setup = session_setup;
    klass->cleanup = session_cleanup;
    process_class->run = session_run;
    process_class->stopped = session_stopped;
    object_class->finalize = session_finalize;

    g_type_class_add_private (klass, sizeof (SessionPrivate));
}
