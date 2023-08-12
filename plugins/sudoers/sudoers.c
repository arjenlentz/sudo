/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 1993-1996, 1998-2023 Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#ifdef __TANDEM
# include <floss.h>
#endif

#include <config.h>

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#ifdef HAVE_LOGIN_CAP_H
# include <login_cap.h>
# ifndef LOGIN_DEFROOTCLASS
#  define LOGIN_DEFROOTCLASS	"daemon"
# endif
# ifndef LOGIN_SETENV
#  define LOGIN_SETENV	0
# endif
#endif
#ifdef HAVE_SELINUX
# include <selinux/selinux.h>
#endif
#include <ctype.h>
#ifndef HAVE_GETADDRINFO
# include "compat/getaddrinfo.h"
#endif

#include "sudoers.h"
#include "check.h"
#include "auth/sudo_auth.h"
#include "sudo_iolog.h"

/*
 * Prototypes
 */
static int set_cmnd(void);
static bool init_vars(char * const *);
static bool set_loginclass(struct passwd *);
static bool set_runaspw(const char *, bool);
static bool set_runasgr(const char *, bool);
static bool tty_present(void);

/*
 * Globals
 */
struct sudoers_user_context user_ctx;
struct sudoers_runas_context runas_ctx;
struct passwd *list_pw;
unsigned int sudo_mode;

static char *prev_user;
static struct sudo_nss_list *snl;
static bool unknown_runas_uid;
static bool unknown_runas_gid;
static int cmnd_status = NOT_FOUND_ERROR;
static struct defaults_list initial_defaults = TAILQ_HEAD_INITIALIZER(initial_defaults);

#ifdef __linux__
static struct rlimit nproclimit;
#endif

/* XXX - must be extern for audit bits of sudo_auth.c */
int NewArgc;
char **NewArgv;
char **saved_argv;

#ifdef SUDOERS_LOG_CLIENT
# define remote_iologs	(!SLIST_EMPTY(&def_log_servers))
#else
# define remote_iologs	0
#endif

/*
 * Unlimit the number of processes since Linux's setuid() will
 * apply resource limits when changing uid and return EAGAIN if
 * nproc would be exceeded by the uid switch.
 */
static void
unlimit_nproc(void)
{
#ifdef __linux__
    struct rlimit rl;
    debug_decl(unlimit_nproc, SUDOERS_DEBUG_UTIL);

    if (getrlimit(RLIMIT_NPROC, &nproclimit) != 0)
	    sudo_warn("getrlimit(RLIMIT_NPROC)");
    rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_NPROC, &rl) != 0) {
	rl.rlim_cur = rl.rlim_max = nproclimit.rlim_max;
	if (setrlimit(RLIMIT_NPROC, &rl) != 0)
	    sudo_warn("setrlimit(RLIMIT_NPROC)");
    }
    debug_return;
#endif /* __linux__ */
}

/*
 * Restore saved value of RLIMIT_NPROC.
 */
static void
restore_nproc(void)
{
#ifdef __linux__
    debug_decl(restore_nproc, SUDOERS_DEBUG_UTIL);

    if (setrlimit(RLIMIT_NPROC, &nproclimit) != 0)
	sudo_warn("setrlimit(RLIMIT_NPROC)");

    debug_return;
#endif /* __linux__ */
}

/*
 * Re-initialize Defaults settings.
 * We do not warn, log or send mail for errors when reinitializing,
 * this would have already been done the first time through.
 */
static bool
sudoers_reinit_defaults(void)
{
    struct sudo_nss *nss, *nss_next;
    sudoers_logger_t logger = sudoers_error_hook;
    debug_decl(sudoers_reinit_defaults, SUDOERS_DEBUG_PLUGIN);

    if (!init_defaults()) {
	sudo_warnx("%s", U_("unable to initialize sudoers default values"));
	debug_return_bool(false);
    }

    /* It should not be possible for the initial defaults to fail to apply. */
    if (!update_defaults(NULL, &initial_defaults,
	    SETDEF_GENERIC|SETDEF_HOST|SETDEF_USER|SETDEF_RUNAS, false))
	debug_return_bool(false);

    /* Disable error logging while re-processing defaults. */
    sudoers_error_hook = NULL;

    TAILQ_FOREACH_SAFE(nss, snl, entries, nss_next) {
	/* Missing/invalid defaults is not a fatal error. */
	if (nss->getdefs(nss) != -1) {
	    (void)update_defaults(nss->parse_tree, NULL,
		SETDEF_GENERIC|SETDEF_HOST|SETDEF_USER|SETDEF_RUNAS, true);
	}
    }

    /* Restore error logging. */
    sudoers_error_hook = logger;

    /* No need to check the admin flag file multiple times. */
    if (ISSET(sudo_mode, MODE_POLICY_INTERCEPTED)) {
	free(def_admin_flag);
	def_admin_flag = NULL;
    }

    debug_return_bool(true);
}

int
sudoers_init(void *info, sudoers_logger_t logger, char * const envp[])
{
    struct sudo_nss *nss, *nss_next;
    int oldlocale, sources = 0;
    static int ret = -1;
    debug_decl(sudoers_init, SUDOERS_DEBUG_PLUGIN);

    /* Only initialize once. */
    if (snl != NULL)
	debug_return_int(ret);

    bindtextdomain("sudoers", LOCALEDIR);

    /* Hook up logging function for parse errors. */
    sudoers_error_hook = logger;

    /* Register fatal/fatalx callback. */
    sudo_fatal_callback_register(sudoers_cleanup);

    /* Initialize environment functions (including replacements). */
    if (!env_init(envp))
	debug_return_int(-1);

    /* Setup defaults data structures. */
    if (!init_defaults()) {
	sudo_warnx("%s", U_("unable to initialize sudoers default values"));
	debug_return_int(-1);
    }

    /* Parse info from front-end. */
    sudo_mode = sudoers_policy_deserialize_info(info, &initial_defaults);
    if (ISSET(sudo_mode, MODE_ERROR))
	debug_return_int(-1);

    if (!init_vars(envp))
	debug_return_int(-1);

    /* Parse nsswitch.conf for sudoers order. */
    snl = sudo_read_nss();

    /* LDAP or NSS may modify the euid so we need to be root for the open. */
    if (!set_perms(PERM_ROOT))
	debug_return_int(-1);

    /* Use the C locale unless another is specified in sudoers. */
    sudoers_setlocale(SUDOERS_LOCALE_SUDOERS, &oldlocale);
    sudo_warn_set_locale_func(sudoers_warn_setlocale);

    /* Update defaults set by front-end. */
    if (!update_defaults(NULL, &initial_defaults,
	    SETDEF_GENERIC|SETDEF_HOST|SETDEF_USER|SETDEF_RUNAS, false)) {
	goto cleanup;
    }

    /* Open and parse sudoers, set global defaults.  */
    TAILQ_FOREACH_SAFE(nss, snl, entries, nss_next) {
	if (nss->open(nss) == -1 || (nss->parse_tree = nss->parse(nss)) == NULL) {
	    TAILQ_REMOVE(snl, nss, entries);
	    continue;
	}
	sources++;

	/* Missing/invalid defaults is not a fatal error. */
	if (nss->getdefs(nss) == -1) {
	    log_warningx(SLOG_PARSE_ERROR|SLOG_NO_STDERR,
		N_("unable to get defaults from %s"), nss->source);
	} else {
	    (void)update_defaults(nss->parse_tree, NULL,
		SETDEF_GENERIC|SETDEF_HOST|SETDEF_USER|SETDEF_RUNAS, false);
	}
    }
    if (sources == 0) {
	sudo_warnx("%s", U_("no valid sudoers sources found, quitting"));
	goto cleanup;
    }

    /* Set login class if applicable (after sudoers is parsed). */
    if (set_loginclass(runas_ctx.pw ? runas_ctx.pw : user_ctx.pw))
	ret = true;

cleanup:
    mail_parse_errors();

    if (!restore_perms())
	ret = -1;

    /* Restore user's locale. */
    sudo_warn_set_locale_func(NULL);
    sudoers_setlocale(oldlocale, NULL);

    debug_return_int(ret);
}

/*
 * Expand I/O log dir and file into a full path.
 * Returns the full I/O log path prefixed with "iolog_path=".
 * Sets user_ctx.iolog_file as a side effect.
 */
static char *
format_iolog_path(void)
{
    char dir[PATH_MAX], file[PATH_MAX];
    char *iolog_path = NULL;
    int oldlocale;
    bool ok;
    debug_decl(format_iolog_path, SUDOERS_DEBUG_PLUGIN);

    /* Use sudoers locale for strftime() */
    sudoers_setlocale(SUDOERS_LOCALE_SUDOERS, &oldlocale);
    ok = expand_iolog_path(def_iolog_dir, dir, sizeof(dir),
	&sudoers_iolog_path_escapes[1], NULL);
    if (ok) {
	ok = expand_iolog_path(def_iolog_file, file, sizeof(file),
	    &sudoers_iolog_path_escapes[0], dir);
    }
    sudoers_setlocale(oldlocale, NULL);
    if (!ok)
	goto done;

    if (asprintf(&iolog_path, "iolog_path=%s/%s", dir, file) == -1) {
	iolog_path = NULL;
	goto done;
    }

    /* Stash pointer to the I/O log for the event log. */
    user_ctx.iolog_path = iolog_path + sizeof("iolog_path=") - 1;
    user_ctx.iolog_file = user_ctx.iolog_path + 1 + strlen(dir);

done:
    debug_return_str(iolog_path);
}

static void
cb_lookup(const struct sudoers_parse_tree *parse_tree,
    const struct userspec *us, int user_match, const struct privilege *priv,
    int host_match, const struct cmndspec *cs, int date_match, int runas_match,
    int cmnd_match, void *closure)
{
    struct sudoers_match_info *info = closure;

    if (cmnd_match != UNSPEC) {
	info->us = us;
	info->priv = priv;
	info->cs = cs;
    }
}

/*
 * Find the command, perform a sudoers lookup, ask for a password as
 * needed, and perform post-lokup checks.  Logs success/failure.
 * This is used by the check, list and validate plugin methods.
 *
 * Returns true if allowed, false if denied, -1 on error and
 * -2 for usage error.
 */
static int
sudoers_check_common(int pwflag)
{
    struct sudoers_match_info match_info = { NULL };
    int oldlocale, ret = -1;
    unsigned int validated;
    time_t now;
    debug_decl(sudoers_check_common, SUDOERS_DEBUG_PLUGIN);

    /* If given the -P option, set the "preserve_groups" flag. */
    if (ISSET(sudo_mode, MODE_PRESERVE_GROUPS))
	def_preserve_groups = true;

    /* Find command in path and apply per-command Defaults. */
    cmnd_status = set_cmnd();
    if (cmnd_status == NOT_FOUND_ERROR)
	goto done;

    /* Is root even allowed to run sudo? */
    if (user_ctx.uid == 0 && !def_root_sudo) {
	/* Not an audit event (should it be?). */
	sudo_warnx("%s",
	    U_("sudoers specifies that root is not allowed to sudo"));
	ret = false;
	goto done;
    }

    /* Check for -C overriding def_closefrom. */
    if (user_ctx.closefrom >= 0 && user_ctx.closefrom != def_closefrom) {
	if (!def_closefrom_override) {
	    log_warningx(SLOG_NO_STDERR|SLOG_AUDIT,
		N_("user not allowed to override closefrom limit"));
	    sudo_warnx("%s", U_("you are not permitted to use the -C option"));
	    goto bad;
	}
	def_closefrom = user_ctx.closefrom;
    }

    /*
     * Check sudoers sources, using the locale specified in sudoers.
     */
    time(&now);
    sudoers_setlocale(SUDOERS_LOCALE_SUDOERS, &oldlocale);
    validated = sudoers_lookup(snl, user_ctx.pw, now, cb_lookup, &match_info,
	&cmnd_status, pwflag);
    sudoers_setlocale(oldlocale, NULL);
    if (ISSET(validated, VALIDATE_ERROR)) {
	/* The lookup function should have printed an error. */
	goto done;
    }

    if (match_info.us != NULL && match_info.us->file != NULL) {
	free(user_ctx.source);
	if (match_info.us->line != 0) {
	    if (asprintf(&user_ctx.source, "%s:%d:%d", match_info.us->file,
		    match_info.us->line, match_info.us->column) == -1)
		user_ctx.source = NULL;
	} else {
	    user_ctx.source = strdup(match_info.us->file);
	}
	if (user_ctx.source == NULL) {
	    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	    goto done;
	}
    }

    if (runas_ctx.cmnd == NULL) {
	if ((runas_ctx.cmnd = strdup(user_ctx.cmnd)) == NULL) {
	    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	    goto done;
	}
    }

    /* Defer uid/gid checks until after defaults have been updated. */
    if (unknown_runas_uid && !def_runas_allow_unknown_id) {
	log_warningx(SLOG_AUDIT, N_("unknown user %s"),
	    runas_ctx.pw->pw_name);
	goto done;
    }
    if (runas_ctx.gr != NULL) {
	if (unknown_runas_gid && !def_runas_allow_unknown_id) {
	    log_warningx(SLOG_AUDIT, N_("unknown group %s"),
		runas_ctx.gr->gr_name);
	    goto done;
	}
    }

    /* If no command line args and "shell_noargs" is not set, error out. */
    if (ISSET(sudo_mode, MODE_IMPLIED_SHELL) && !def_shell_noargs) {
	/* Not an audit event. */
	ret = -2; /* usage error */
	goto done;
    }

    /* Bail if a tty is required and we don't have one. */
    if (def_requiretty && !tty_present()) {
	log_warningx(SLOG_NO_STDERR|SLOG_AUDIT, N_("no tty"));
	sudo_warnx("%s", U_("sorry, you must have a tty to run sudo"));
	goto bad;
    }

    /* Check runas user's shell if running (or checking) a command. */
    if (ISSET(sudo_mode, MODE_RUN|MODE_CHECK)) {
	if (!check_user_shell(runas_ctx.pw)) {
	    log_warningx(SLOG_RAW_MSG|SLOG_AUDIT,
		N_("invalid shell for user %s: %s"),
		runas_ctx.pw->pw_name, runas_ctx.pw->pw_shell);
	    goto bad;
	}
    }

    /*
     * We don't reset the environment for sudoedit or if the user
     * specified the -E command line flag and they have setenv privs.
     */
    if (ISSET(sudo_mode, MODE_EDIT) ||
	(ISSET(sudo_mode, MODE_PRESERVE_ENV) && def_setenv))
	def_env_reset = false;

    /* Build a new environment that avoids any nasty bits. */
    if (!rebuild_env())
	goto bad;

    /* Require a password if sudoers says so.  */
    switch (check_user(validated, sudo_mode)) {
    case true:
	/* user authenticated successfully. */
	break;
    case false:
	/* Note: log_denial() calls audit for us. */
	if (!ISSET(validated, VALIDATE_SUCCESS)) {
	    /* Only display a denial message if no password was read. */
	    if (!log_denial(validated, def_passwd_tries <= 0))
		goto done;
	}
	goto bad;
    default:
	/* some other error, ret is -1. */
	goto done;
    }

    /* Check whether runas_ctx.chroot is permitted (if specified). */
    switch (check_user_runchroot()) {
    case true:
	break;
    case false:
	log_warningx(SLOG_NO_STDERR|SLOG_AUDIT,
	    N_("user not allowed to change root directory to %s"),
	    runas_ctx.chroot);
	sudo_warnx(U_("you are not permitted to use the -R option with %s"),
	    user_ctx.cmnd);
	goto bad;
    default:
	goto done;
    }

    /* Check whether runas_ctx.cwd is permitted (if specified). */
    switch (check_user_runcwd()) {
    case true:
	break;
    case false:
	log_warningx(SLOG_NO_STDERR|SLOG_AUDIT,
	    N_("user not allowed to change directory to %s"), runas_ctx.cwd);
	sudo_warnx(U_("you are not permitted to use the -D option with %s"),
	    user_ctx.cmnd);
	goto bad;
    default:
	goto done;
    }

    /* If run as root with SUDO_USER set, set user_ctx.pw to that user. */
    /* XXX - causes confusion when root is not listed in sudoers */
    if (ISSET(sudo_mode, MODE_RUN|MODE_EDIT) && prev_user != NULL) {
	if (user_ctx.uid == 0 && strcmp(prev_user, "root") != 0) {
	    struct passwd *pw;

	    if ((pw = sudo_getpwnam(prev_user)) != NULL) {
		    if (user_ctx.pw != NULL)
			sudo_pw_delref(user_ctx.pw);
		    user_ctx.pw = pw;
	    }
	}
    }

    /* If the user was not allowed to run the command we are done. */
    if (!ISSET(validated, VALIDATE_SUCCESS)) {
	/* Note: log_failure() calls audit for us. */
	if (!log_failure(validated, cmnd_status))
	    goto done;
	goto bad;
    }

    /* Create Ubuntu-style dot file to indicate sudo was successful. */
    if (create_admin_success_flag(user_ctx.pw) == -1)
	goto done;

    /* Finally tell the user if the command did not exist. */
    if (cmnd_status == NOT_FOUND_DOT) {
	audit_failure(NewArgv, N_("command in current directory"));
	sudo_warnx(U_("ignoring \"%s\" found in '.'\nUse \"sudo ./%s\" if this is the \"%s\" you wish to run."), user_ctx.cmnd, user_ctx.cmnd, user_ctx.cmnd);
	goto bad;
    } else if (cmnd_status == NOT_FOUND) {
	if (ISSET(sudo_mode, MODE_CHECK)) {
	    audit_failure(NewArgv, N_("%s: command not found"), NewArgv[1]);
	    sudo_warnx(U_("%s: command not found"), NewArgv[1]);
	} else {
	    audit_failure(NewArgv, N_("%s: command not found"), user_ctx.cmnd);
	    sudo_warnx(U_("%s: command not found"), user_ctx.cmnd);
	    if (strncmp(user_ctx.cmnd, "cd", 2) == 0 && (user_ctx.cmnd[2] == '\0' ||
		    isblank((unsigned char)user_ctx.cmnd[2]))) {
		sudo_warnx("%s",
		    U_("\"cd\" is a shell built-in command, it cannot be run directly."));
		sudo_warnx("%s",
		    U_("the -s option may be used to run a privileged shell."));
		sudo_warnx("%s",
		    U_("the -D option may be used to run a command in a specific directory."));
	    }
	}
	goto bad;
    }

    /* If user specified a timeout make sure sudoers allows it. */
    if (!def_user_command_timeouts && user_ctx.timeout > 0) {
	log_warningx(SLOG_NO_STDERR|SLOG_AUDIT,
	    N_("user not allowed to set a command timeout"));
	sudo_warnx("%s",
	    U_("sorry, you are not allowed set a command timeout"));
	goto bad;
    }

    /* If user specified env vars make sure sudoers allows it. */
    if (ISSET(sudo_mode, MODE_RUN) && !def_setenv) {
	if (ISSET(sudo_mode, MODE_PRESERVE_ENV)) {
	    log_warningx(SLOG_NO_STDERR|SLOG_AUDIT,
		N_("user not allowed to preserve the environment"));
	    sudo_warnx("%s",
		U_("sorry, you are not allowed to preserve the environment"));
	    goto bad;
	} else {
	    if (!validate_env_vars(user_ctx.env_vars))
		goto bad;
	}
    }

    ret = true;
    goto done;

bad:
    ret = false;
done:
    debug_return_int(ret);
}

static bool need_reinit;

/*
 * Check whether the user is allowed to run the specified command.
 * Returns true if allowed, false if denied, -1 on error and
 * -2 for usage error.
 */
int
sudoers_check_cmnd(int argc, char * const argv[], char *env_add[],
    void *closure)
{
    char *iolog_path = NULL;
    mode_t cmnd_umask = ACCESSPERMS;
    int ret = -1;
    debug_decl(sudoers_check_cmnd, SUDOERS_DEBUG_PLUGIN);

    sudo_warn_set_locale_func(sudoers_warn_setlocale);

    if (argc == 0) {
	sudo_warnx("%s", U_("no command specified"));
	debug_return_int(-1);
    }

    if (need_reinit) {
	/* Was previous command intercepted? */
	if (ISSET(sudo_mode, MODE_RUN) && def_intercept)
	    SET(sudo_mode, MODE_POLICY_INTERCEPTED);

	/* Only certain mode flags are legal for intercepted commands. */
	if (ISSET(sudo_mode, MODE_POLICY_INTERCEPTED))
	    sudo_mode &= MODE_INTERCEPT_MASK;

	/* Re-initialize defaults if we are called multiple times. */
	if (!sudoers_reinit_defaults())
	    debug_return_int(-1);
    }
    need_reinit = true;

    unlimit_nproc();

    if (!set_perms(PERM_INITIAL))
	goto bad;

    /* Environment variables specified on the command line. */
    if (env_add != NULL && env_add[0] != NULL)
	user_ctx.env_vars = env_add;

    /*
     * Make a local copy of argc/argv, with special handling for the
     * '-i' option.  We also allocate an extra slot for bash's --login.
     */
    if (NewArgv != NULL && NewArgv != saved_argv) {
	sudoers_gc_remove(GC_PTR, NewArgv);
	free(NewArgv);
    }
    NewArgv = reallocarray(NULL, (size_t)argc + 2, sizeof(char *));
    if (NewArgv == NULL) {
	sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	goto error;
    }
    sudoers_gc_add(GC_PTR, NewArgv);
    memcpy(NewArgv, argv, (size_t)argc * sizeof(char *));
    NewArgc = argc;
    NewArgv[NewArgc] = NULL;
    if (ISSET(sudo_mode, MODE_LOGIN_SHELL) && runas_ctx.pw != NULL) {
	NewArgv[0] = strdup(runas_ctx.pw->pw_shell);
	if (NewArgv[0] == NULL) {
	    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	    goto error;
	}
	sudoers_gc_add(GC_PTR, NewArgv[0]);
    }

    ret = sudoers_check_common(0);
    if (ret != true)
	goto done;

    if (!remote_iologs) {
	if (iolog_enabled && def_iolog_file && def_iolog_dir) {
	    if ((iolog_path = format_iolog_path()) == NULL) {
		if (!def_ignore_iolog_errors)
		    goto error;
		/* Unable to expand I/O log path, disable I/O logging. */
		def_log_input = false;
		def_log_output = false;
		def_log_stdin = false;
		def_log_stdout = false;
		def_log_stderr = false;
		def_log_ttyin = false;
		def_log_ttyout = false;
	    }
	}
    }

    /*
     * Set umask based on sudoers.
     * If user's umask is more restrictive, OR in those bits too
     * unless umask_override is set.
     */
    if (def_umask != ACCESSPERMS) {
	cmnd_umask = def_umask;
	if (!def_umask_override)
	    cmnd_umask |= user_ctx.umask;
    }

    if (ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
	char *p;

	/* Convert /bin/sh -> -sh so shell knows it is a login shell */
	if ((p = strrchr(NewArgv[0], '/')) == NULL)
	    p = NewArgv[0];
	*p = '-';
	NewArgv[0] = p;

	/*
	 * Newer versions of bash require the --login option to be used
	 * in conjunction with the -c option even if the shell name starts
	 * with a '-'.  Unfortunately, bash 1.x uses -login, not --login
	 * so this will cause an error for that.
	 */
	if (NewArgc > 1 && strcmp(NewArgv[0], "-bash") == 0 &&
	    strcmp(NewArgv[1], "-c") == 0) {
	    /* We allocated extra space for the --login above. */
	    memmove(&NewArgv[2], &NewArgv[1], (size_t)NewArgc * sizeof(char *));
	    NewArgv[1] = (char *)"--login";
	    NewArgc++;
	}

#if defined(_AIX) || (defined(__linux__) && !defined(HAVE_PAM))
	/* Insert system-wide environment variables. */
	if (!read_env_file(_PATH_ENVIRONMENT, true, false))
	    sudo_warn("%s", _PATH_ENVIRONMENT);
#endif
#ifdef HAVE_LOGIN_CAP_H
	/* Set environment based on login class. */
	if (runas_ctx.class) {
	    login_cap_t *lc = login_getclass(runas_ctx.class);
	    if (lc != NULL) {
		setusercontext(lc, runas_ctx.pw, runas_ctx.pw->pw_uid,
		    LOGIN_SETPATH|LOGIN_SETENV);
		login_close(lc);
	    }
	}
#endif /* HAVE_LOGIN_CAP_H */
    }

    /* Insert system-wide environment variables. */
    if (def_restricted_env_file) {
	if (!read_env_file(def_restricted_env_file, false, true))
	    sudo_warn("%s", def_restricted_env_file);
    }
    if (def_env_file) {
	if (!read_env_file(def_env_file, false, false))
	    sudo_warn("%s", def_env_file);
    }

    /* Insert user-specified environment variables. */
    if (!insert_env_vars(user_ctx.env_vars)) {
	sudo_warnx("%s",
	    U_("error setting user-specified environment variables"));
	goto error;
    }

    /* Note: must call audit before uid change. */
    if (ISSET(sudo_mode, MODE_EDIT)) {
	const char *env_editor = NULL;
	char **edit_argv;
	int edit_argc;

	sudoedit_nfiles = NewArgc - 1;
	free(runas_ctx.cmnd);
	runas_ctx.cmnd = find_editor(sudoedit_nfiles, NewArgv + 1,
	    &edit_argc, &edit_argv, NULL, &env_editor);
	if (runas_ctx.cmnd == NULL) {
	    switch (errno) {
	    case ENOENT:
		audit_failure(NewArgv, N_("%s: command not found"),
		    env_editor ? env_editor : def_editor);
		sudo_warnx(U_("%s: command not found"),
		    env_editor ? env_editor : def_editor);
		goto error;
	    case EINVAL:
		if (def_env_editor && env_editor != NULL) {
		    /* User tried to do something funny with the editor. */
		    log_warningx(SLOG_NO_STDERR|SLOG_AUDIT|SLOG_SEND_MAIL,
			"invalid user-specified editor: %s", env_editor);
		    goto error;
		}
		FALLTHROUGH;
	    default:
		goto error;
	    }
	}
	/* find_editor() already g/c'd edit_argv[] */
	if (NewArgv != saved_argv) {
	    sudoers_gc_remove(GC_PTR, NewArgv);
	    free(NewArgv);
	}
	NewArgv = edit_argv;
	NewArgc = edit_argc;

	/* We want to run the editor with the unmodified environment. */
	env_swap_old();
    }

    /* Save the initial command and argv so we have it for exit logging. */
    if (user_ctx.cmnd_saved == NULL) {
	user_ctx.cmnd_saved = strdup(runas_ctx.cmnd);
	if (user_ctx.cmnd_saved == NULL) {
	    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	    goto error;
	}
	saved_argv = NewArgv;
    }

    ret = true;
    goto done;

bad:
    ret = false;
    goto done;

error:
    ret = -1;

done:
    mail_parse_errors();

    if (def_group_plugin)
	group_plugin_unload();
    reset_parser();

    if (ret == -1) {
	/* Free stashed copy of the environment. */
	(void)env_init(NULL);

	/* Free locally-allocated strings. */
	free(iolog_path);
    } else {
	/* Store settings to pass back to front-end. */
	if (!sudoers_policy_store_result(ret, NewArgv, env_get(), cmnd_umask,
		iolog_path, closure))
	    ret = -1;
    }

    if (!rewind_perms())
	ret = -1;

    restore_nproc();

    sudo_warn_set_locale_func(NULL);

    debug_return_int(ret);
}

/*
 * Validate the user and update their timestamp file entry.
 * Returns true if allowed, false if denied, -1 on error and
 * -2 for usage error.
 */
int
sudoers_validate_user(void)
{
    int ret = -1;
    debug_decl(sudoers_validate_user, SUDOERS_DEBUG_PLUGIN);

    sudo_warn_set_locale_func(sudoers_warn_setlocale);

    unlimit_nproc();

    if (!set_perms(PERM_INITIAL))
	goto done;

    NewArgv = reallocarray(NULL, 2, sizeof(char *));
    if (NewArgv == NULL) {
	sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	goto done;
    }
    sudoers_gc_add(GC_PTR, NewArgv);
    NewArgv[0] = (char *)"validate";
    NewArgv[1] = NULL;
    NewArgc = 2;

    ret = sudoers_check_common(I_VERIFYPW);

done:
    mail_parse_errors();

    if (def_group_plugin)
	group_plugin_unload();
    reset_parser();
    env_init(NULL);

    if (!rewind_perms())
	ret = -1;

    restore_nproc();

    sudo_warn_set_locale_func(NULL);

    debug_return_int(ret);
}

/*
 * List a user's privileges or check whether a specific command may be run.
 * Returns true if allowed, false if denied, -1 on error and
 * -2 for usage error.
 */
int
sudoers_list(int argc, char * const argv[], const char *list_user, bool verbose)
{
    int ret = -1;
    debug_decl(sudoers_list, SUDOERS_DEBUG_PLUGIN);

    sudo_warn_set_locale_func(sudoers_warn_setlocale);

    unlimit_nproc();

    if (!set_perms(PERM_INITIAL))
	goto done;

    if (list_user) {
	list_pw = sudo_getpwnam(list_user);
	if (list_pw == NULL) {
	    sudo_warnx(U_("unknown user %s"), list_user);
	    goto done;
	}
    }

    NewArgv = reallocarray(NULL, (size_t)argc + 2, sizeof(char *));
    if (NewArgv == NULL) {
	sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	goto done;
    }
    sudoers_gc_add(GC_PTR, NewArgv);
    NewArgv[0] = (char *)"list";
    if (argc != 0)
	memcpy(NewArgv + 1, argv, (size_t)argc * sizeof(char *));
    NewArgc = argc + 1;
    NewArgv[NewArgc] = NULL;

    ret = sudoers_check_common(I_LISTPW);
    if (ret != true)
	goto done;

    if (ISSET(sudo_mode, MODE_CHECK))
	ret = display_cmnd(snl, list_pw ? list_pw : user_ctx.pw, verbose);
    else
	ret = display_privs(snl, list_pw ? list_pw : user_ctx.pw, verbose);

done:
    mail_parse_errors();

    if (list_pw != NULL) {
	sudo_pw_delref(list_pw);
	list_pw = NULL;
    }
    if (def_group_plugin)
	group_plugin_unload();
    reset_parser();
    env_init(NULL);

    if (!rewind_perms())
	ret = -1;

    restore_nproc();

    sudo_warn_set_locale_func(NULL);

    debug_return_int(ret);
}

/*
 * Initialize timezone and fill in user_ctx.
 */
static bool
init_vars(char * const envp[])
{
    char * const * ep;
    bool unknown_user = false;
    debug_decl(init_vars, SUDOERS_DEBUG_PLUGIN);

    if (!sudoers_initlocale(setlocale(LC_ALL, NULL), def_sudoers_locale)) {
	sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	debug_return_bool(false);
    }

#define MATCHES(s, v)	\
    (strncmp((s), (v), sizeof(v) - 1) == 0 && (s)[sizeof(v) - 1] != '\0')

    for (ep = envp; *ep; ep++) {
	switch (**ep) {
	    case 'K':
		if (MATCHES(*ep, "KRB5CCNAME="))
		    user_ctx.ccname = *ep + sizeof("KRB5CCNAME=") - 1;
		break;
	    case 'P':
		if (MATCHES(*ep, "PATH="))
		    user_ctx.path = *ep + sizeof("PATH=") - 1;
		break;
	    case 'S':
		if (MATCHES(*ep, "SUDO_PROMPT=")) {
		    /* Don't override "sudo -p prompt" */
		    if (user_ctx.prompt == NULL)
			user_ctx.prompt = *ep + sizeof("SUDO_PROMPT=") - 1;
		    break;
		}
		if (MATCHES(*ep, "SUDO_USER="))
		    prev_user = *ep + sizeof("SUDO_USER=") - 1;
		break;
	    }
    }
#undef MATCHES

    /*
     * Get a local copy of the user's passwd struct and group list if we
     * don't already have them.
     */
    if (user_ctx.pw == NULL) {
	if ((user_ctx.pw = sudo_getpwnam(user_ctx.name)) == NULL) {
	    /*
	     * It is not unusual for users to place "sudo -k" in a .logout
	     * file which can cause sudo to be run during reboot after the
	     * YP/NIS/NIS+/LDAP/etc daemon has died.
	     */
	    if (sudo_mode == MODE_KILL || sudo_mode == MODE_INVALIDATE) {
		sudo_warnx(U_("unknown user %s"), user_ctx.name);
		debug_return_bool(false);
	    }

	    /* Need to make a fake struct passwd for the call to log_warningx(). */
	    user_ctx.pw = sudo_mkpwent(user_ctx.name, user_ctx.uid,
		user_ctx.gid, NULL, NULL);
	    unknown_user = true;
	}
    }
    if (user_ctx.gid_list == NULL)
	user_ctx.gid_list = sudo_get_gidlist(user_ctx.pw, ENTRY_TYPE_ANY);

    /* Store initialize permissions so we can restore them later. */
    if (!set_perms(PERM_INITIAL))
	debug_return_bool(false);

    /* Set parse callbacks */
    set_callbacks();

    /* It is now safe to use log_warningx() and set_perms() */
    if (unknown_user) {
	log_warningx(SLOG_SEND_MAIL, N_("unknown user %s"), user_ctx.name);
	debug_return_bool(false);
    }

    /*
     * Set runas passwd/group entries based on command line or sudoers.
     * Note that if runas_group was specified without runas_user we
     * run the command as the invoking user.
     */
    if (runas_ctx.group != NULL) {
	if (!set_runasgr(runas_ctx.group, false))
	    debug_return_bool(false);
	if (!set_runaspw(runas_ctx.user ?
		runas_ctx.user : user_ctx.name, false))
	    debug_return_bool(false);
    } else {
	if (!set_runaspw(runas_ctx.user ?
		runas_ctx.user : def_runas_default, false))
	    debug_return_bool(false);
    }

    debug_return_bool(true);
}

/*
 * Fill in user_ctx.cmnd and user_ctx.cmnd_stat variables.
 * Does not fill in user_ctx.cmnd_base.
 */
int
set_cmnd_path(const char *runchroot)
{
    const char *cmnd_in;
    char *cmnd_out = NULL;
    char *path = user_ctx.path;
    int ret, pivot_fds[2];
    debug_decl(set_cmnd_path, SUDOERS_DEBUG_PLUGIN);

    cmnd_in = ISSET(sudo_mode, MODE_CHECK) ? NewArgv[1] : NewArgv[0];

    free(user_ctx.cmnd_list);
    user_ctx.cmnd_list = NULL;
    free(user_ctx.cmnd);
    user_ctx.cmnd = NULL;
    canon_path_free(user_ctx.cmnd_dir);
    user_ctx.cmnd_dir = NULL;
    if (def_secure_path && !user_is_exempt())
	path = def_secure_path;

    /* Pivot root. */
    if (runchroot != NULL) {
	if (!pivot_root(runchroot, pivot_fds))
	    goto error;
    }

    if (!set_perms(PERM_RUNAS))
	goto error;
    ret = find_path(cmnd_in, &cmnd_out, user_ctx.cmnd_stat, path,
	def_ignore_dot, NULL);
    if (!restore_perms())
	goto error;
    if (ret == NOT_FOUND) {
	/* Failed as root, try as invoking user. */
	if (!set_perms(PERM_USER))
	    goto error;
	ret = find_path(cmnd_in, &cmnd_out, user_ctx.cmnd_stat, path,
	    def_ignore_dot, NULL);
	if (!restore_perms())
	    goto error;
    }

    if (cmnd_out != NULL) {
	char *slash = strrchr(cmnd_out, '/');
	if (slash != NULL) {
	    *slash = '\0';
	    user_ctx.cmnd_dir = canon_path(cmnd_out);
	    if (user_ctx.cmnd_dir == NULL && errno == ENOMEM)
		goto error;
	    *slash = '/';
	}
    }

    if (ISSET(sudo_mode, MODE_CHECK))
	user_ctx.cmnd_list = cmnd_out;
    else
	user_ctx.cmnd = cmnd_out;

    /* Restore root. */
    if (runchroot != NULL)
	(void)unpivot_root(pivot_fds);

    debug_return_int(ret);
error:
    if (runchroot != NULL)
	(void)unpivot_root(pivot_fds);
    free(cmnd_out);
    debug_return_int(NOT_FOUND_ERROR);
}

/*
 * Fill in user_ctx.cmnd, user_ctx.cmnd_stat and cmnd_status variables.
 * Does not fill in user_ctx.cmnd_base.
 */
void
set_cmnd_status(const char *runchroot)
{
    cmnd_status = set_cmnd_path(runchroot);
}

/*
 * Fill in user_ctx.cmnd, user_ctx.cmnd_args, user_ctx.cmnd_base and
 * user_ctx.cmnd_stat variables and apply any command-specific defaults entries.
 */
static int
set_cmnd(void)
{
    struct sudo_nss *nss;
    int ret = FOUND;
    debug_decl(set_cmnd, SUDOERS_DEBUG_PLUGIN);

    /* Allocate user_ctx.cmnd_stat for find_path() and match functions. */
    free(user_ctx.cmnd_stat);
    user_ctx.cmnd_stat = calloc(1, sizeof(struct stat));
    if (user_ctx.cmnd_stat == NULL) {
	sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	debug_return_int(NOT_FOUND_ERROR);
    }

    /* Re-initialize for when we are called multiple times. */
    free(runas_ctx.cmnd);
    runas_ctx.cmnd = NULL;

    if (ISSET(sudo_mode, MODE_RUN|MODE_EDIT|MODE_CHECK)) {
	if (!ISSET(sudo_mode, MODE_EDIT)) {
	    const char *runchroot = runas_ctx.chroot;
	    if (runchroot == NULL && def_runchroot != NULL &&
		    strcmp(def_runchroot, "*") != 0)
		runchroot = def_runchroot;

	    ret = set_cmnd_path(runchroot);
	    if (ret == NOT_FOUND_ERROR) {
		if (errno == ENAMETOOLONG) {
		    audit_failure(NewArgv, N_("command too long"));
		}
		log_warning(0, "%s", NewArgv[0]);
		debug_return_int(ret);
	    }
	}

	/* set user_ctx.cmnd_args */
	free(user_ctx.cmnd_args);
	user_ctx.cmnd_args = NULL;
	if (ISSET(sudo_mode, MODE_CHECK)) {
	    if (NewArgc > 2) {
		/* Skip the command being listed in NewArgv[1]. */
		user_ctx.cmnd_args = strvec_join(NewArgv + 2, ' ', NULL);
		if (user_ctx.cmnd_args == NULL)
		    debug_return_int(NOT_FOUND_ERROR);
	    }
	} else if (NewArgc > 1) {
	    if (ISSET(sudo_mode, MODE_SHELL|MODE_LOGIN_SHELL) &&
		    ISSET(sudo_mode, MODE_RUN)) {
		/*
		 * When running a command via a shell, the sudo front-end
		 * escapes potential meta chars.  We unescape non-spaces
		 * for sudoers matching and logging purposes.
		 * TODO: move escaping to the policy plugin instead
		 */
		user_ctx.cmnd_args = strvec_join(NewArgv + 1, ' ', strlcpy_unescape);
	    } else {
		user_ctx.cmnd_args = strvec_join(NewArgv + 1, ' ', NULL);
	    }
	    if (user_ctx.cmnd_args == NULL)
		debug_return_int(NOT_FOUND_ERROR);
	}
    }
    if (user_ctx.cmnd == NULL) {
	user_ctx.cmnd = strdup(NewArgv[0]);
	if (user_ctx.cmnd == NULL)
	    debug_return_int(NOT_FOUND_ERROR);
    }
    user_ctx.cmnd_base = sudo_basename(user_ctx.cmnd);

    /* Convert "sudo sudoedit" -> "sudoedit" */
    if (ISSET(sudo_mode, MODE_RUN) && strcmp(user_ctx.cmnd_base, "sudoedit") == 0) {
	char *new_cmnd;

	CLR(sudo_mode, MODE_RUN);
	SET(sudo_mode, MODE_EDIT);
	sudo_warnx("%s", U_("sudoedit doesn't need to be run via sudo"));
	if ((new_cmnd = strdup("sudoedit")) == NULL) {
	    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	    debug_return_int(NOT_FOUND_ERROR);
	}
	free(user_ctx.cmnd);
	user_ctx.cmnd_base = user_ctx.cmnd = new_cmnd;
    }

    TAILQ_FOREACH(nss, snl, entries) {
	/* Missing/invalid defaults is not a fatal error. */
	(void)update_defaults(nss->parse_tree, NULL, SETDEF_CMND, false);
    }

    debug_return_int(ret);
}

static int
open_file(const char *path, int flags)
{
    int fd;
    debug_decl(open_file, SUDOERS_DEBUG_PLUGIN);

    if (!set_perms(PERM_SUDOERS))
	debug_return_int(-1);

    fd = open(path, flags);
    if (fd == -1 && errno == EACCES && geteuid() != ROOT_UID) {
	/*
	 * If we tried to open sudoers as non-root but got EACCES,
	 * try again as root.
	 */
	int serrno = errno;
	if (restore_perms() && set_perms(PERM_ROOT))
	    fd = open(path, flags);
	errno = serrno;
    }
    if (!restore_perms()) {
	/* unable to change back to root */
	if (fd != -1) {
	    close(fd);
	    fd = -1;
	}
    }

    debug_return_int(fd);
}

/*
 * Open sudoers file and check mode/owner/type.
 * Returns a handle to the sudoers file or NULL on error.
 */
FILE *
open_sudoers(const char *path, char **outfile, bool doedit, bool *keepopen)
{
    char fname[PATH_MAX];
    FILE *fp = NULL;
    struct stat sb;
    int error, fd;
    debug_decl(open_sudoers, SUDOERS_DEBUG_PLUGIN);

    fd = sudo_open_conf_path(path, fname, sizeof(fname), open_file);
    error = sudo_secure_fd(fd, S_IFREG, sudoers_file_uid(), sudoers_file_gid(),
	&sb);
    switch (error) {
    case SUDO_PATH_SECURE:
	/*
	 * Make sure we can read the file so we can present the
	 * user with a reasonable error message (unlike the lexer).
	 */
	if ((fp = fdopen(fd, "r")) == NULL) {
	    log_warning(SLOG_PARSE_ERROR, N_("unable to open %s"), fname);
	} else {
	    fd = -1;
	    if (sb.st_size != 0 && fgetc(fp) == EOF) {
		log_warning(SLOG_PARSE_ERROR, N_("unable to read %s"), fname);
		fclose(fp);
		fp = NULL;
	    } else {
		/* Rewind fp and set close on exec flag. */
		rewind(fp);
		(void)fcntl(fileno(fp), F_SETFD, 1);
		if (outfile != NULL) {
                    *outfile = sudo_rcstr_dup(fname);
		    if (*outfile == NULL) {
			sudo_warnx(U_("%s: %s"), __func__,
			    U_("unable to allocate memory"));
			fclose(fp);
			fp = NULL;
		    }
		}
	    }
	}
	break;
    case SUDO_PATH_MISSING:
	log_warning(SLOG_PARSE_ERROR, N_("unable to open %s"), path);
	break;
    case SUDO_PATH_BAD_TYPE:
	log_warningx(SLOG_PARSE_ERROR, N_("%s is not a regular file"), fname);
	break;
    case SUDO_PATH_WRONG_OWNER:
	log_warningx(SLOG_PARSE_ERROR,
	    N_("%s is owned by uid %u, should be %u"), fname,
	    (unsigned int)sb.st_uid, (unsigned int)sudoers_file_uid());
	break;
    case SUDO_PATH_WORLD_WRITABLE:
	log_warningx(SLOG_PARSE_ERROR, N_("%s is world writable"), fname);
	break;
    case SUDO_PATH_GROUP_WRITABLE:
	log_warningx(SLOG_PARSE_ERROR,
	    N_("%s is owned by gid %u, should be %u"), fname,
	    (unsigned int)sb.st_gid, (unsigned int)sudoers_file_gid());
	break;
    default:
	sudo_warnx("%s: internal error, unexpected error %d", __func__, error);
	break;
    }

    if (fp == NULL && fd != -1)
	close(fd);

    debug_return_ptr(fp);
}

#ifdef HAVE_LOGIN_CAP_H
static bool
set_loginclass(struct passwd *pw)
{
    const unsigned int errflags = SLOG_RAW_MSG;
    login_cap_t *lc;
    bool ret = true;
    debug_decl(set_loginclass, SUDOERS_DEBUG_PLUGIN);

    if (!def_use_loginclass)
	goto done;

    if (runas_ctx.class && strcmp(runas_ctx.class, "-") != 0) {
	if (user_ctx.uid != 0 && pw->pw_uid != 0) {
	    sudo_warnx(U_("only root can use \"-c %s\""), runas_ctx.class);
	    ret = false;
	    goto done;
	}
    } else {
	runas_ctx.class = pw->pw_class;
	if (!runas_ctx.class || !*runas_ctx.class)
	    runas_ctx.class = (char *)
		((pw->pw_uid == 0) ? LOGIN_DEFROOTCLASS : LOGIN_DEFCLASS);
    }

    /* Make sure specified login class is valid. */
    lc = login_getclass(runas_ctx.class);
    if (!lc || !lc->lc_class || strcmp(lc->lc_class, runas_ctx.class) != 0) {
	/*
	 * Don't make it an error if the user didn't specify the login
	 * class themselves.  We do this because if login.conf gets
	 * corrupted we want the admin to be able to use sudo to fix it.
	 */
	log_warningx(errflags, N_("unknown login class %s"), runas_ctx.class);
	def_use_loginclass = false;
	if (runas_ctx.class)
	    ret = false;
    }
    login_close(lc);
done:
    debug_return_bool(ret);
}
#else
static bool
set_loginclass(struct passwd *pw)
{
    return true;
}
#endif /* HAVE_LOGIN_CAP_H */

/*
 * Get passwd entry for the user we are going to run commands as
 * and store it in runas_ctx.pw.  By default, commands run as "root".
 */
static bool
set_runaspw(const char *user, bool quiet)
{
    struct passwd *pw = NULL;
    debug_decl(set_runaspw, SUDOERS_DEBUG_PLUGIN);

    unknown_runas_uid = false;
    if (*user == '#') {
	const char *errstr;
	uid_t uid = sudo_strtoid(user + 1, &errstr);
	if (errstr == NULL) {
	    if ((pw = sudo_getpwuid(uid)) == NULL) {
		unknown_runas_uid = true;
		pw = sudo_fakepwnam(user, user_ctx.gid);
	    }
	}
    }
    if (pw == NULL) {
	if ((pw = sudo_getpwnam(user)) == NULL) {
	    if (!quiet)
		log_warningx(SLOG_AUDIT, N_("unknown user %s"), user);
	    debug_return_bool(false);
	}
    }
    if (runas_ctx.pw != NULL)
	sudo_pw_delref(runas_ctx.pw);
    runas_ctx.pw = pw;
    debug_return_bool(true);
}

/*
 * Get group entry for the group we are going to run commands as
 * and store it in runas_ctx.gr.
 */
static bool
set_runasgr(const char *group, bool quiet)
{
    struct group *gr = NULL;
    debug_decl(set_runasgr, SUDOERS_DEBUG_PLUGIN);

    unknown_runas_gid = false;
    if (*group == '#') {
	const char *errstr;
	gid_t gid = sudo_strtoid(group + 1, &errstr);
	if (errstr == NULL) {
	    if ((gr = sudo_getgrgid(gid)) == NULL) {
		unknown_runas_gid = true;
		gr = sudo_fakegrnam(group);
	    }
	}
    }
    if (gr == NULL) {
	if ((gr = sudo_getgrnam(group)) == NULL) {
	    if (!quiet)
		log_warningx(SLOG_AUDIT, N_("unknown group %s"), group);
	    debug_return_bool(false);
	}
    }
    if (runas_ctx.gr != NULL)
	sudo_gr_delref(runas_ctx.gr);
    runas_ctx.gr = gr;
    debug_return_bool(true);
}

/*
 * Callback for runas_default sudoers setting.
 */
bool
cb_runas_default(const char *file, int line, int column,
    const union sudo_defs_val *sd_un, int op)
{
    debug_decl(cb_runas_default, SUDOERS_DEBUG_PLUGIN);

    /* Only reset runaspw if user didn't specify one. */
    if (runas_ctx.user == NULL && runas_ctx.group == NULL)
	debug_return_bool(set_runaspw(sd_un->str, true));
    debug_return_bool(true);
}

/*
 * Free memory allocated for struct sudoers_user_context.
 */
static void
sudoers_user_ctx_free(void)
{
    debug_decl(sudoers_user_ctx_free, SUDOERS_DEBUG_PLUGIN);

    /* Free remaining references to password and group entries. */
    if (user_ctx.pw != NULL)
	sudo_pw_delref(user_ctx.pw);
    if (user_ctx.gid_list != NULL)
	sudo_gidlist_delref(user_ctx.gid_list);

    /* Free dynamic contents of user_ctx. */
    free(user_ctx.cwd);
    free(user_ctx.name);
    free(user_ctx.gids);
    if (user_ctx.ttypath != NULL)
	free(user_ctx.ttypath);
    else
	free(user_ctx.tty);
    if (user_ctx.shost != user_ctx.host)
	    free(user_ctx.shost);
    free(user_ctx.host);
    free(user_ctx.cmnd);
    canon_path_free(user_ctx.cmnd_dir);
    free(user_ctx.cmnd_args);
    free(user_ctx.cmnd_list);
    free(user_ctx.cmnd_saved);
    free(user_ctx.source);
    free(user_ctx.cmnd_stat);
    memset(&user_ctx, 0, sizeof(user_ctx));

    debug_return;
}

/*
 * Free memory allocated for struct sudoers_runas_context.
 */
static void
sudoers_runas_ctx_free(void)
{
    debug_decl(sudoers_runas_ctx_free, SUDOERS_DEBUG_PLUGIN);

    /* Free remaining references to password and group entries. */
    if (runas_ctx.pw != NULL)
	sudo_pw_delref(runas_ctx.pw);
    if (runas_ctx.gr != NULL)
	sudo_gr_delref(runas_ctx.gr);

    /* Free dynamic contents of runas_ctx. */
    free(runas_ctx.cmnd);
    if (runas_ctx.shost != runas_ctx.host)
	    free(runas_ctx.shost);
    free(runas_ctx.host);
#ifdef HAVE_SELINUX
    free(runas_ctx.role);
    free(runas_ctx.type);
#endif
#ifdef HAVE_APPARMOR
    free(runas_ctx.apparmor_profile);
#endif
#ifdef HAVE_PRIV_SET
    free(runas_ctx.privs);
    free(runas_ctx.limitprivs);
#endif
    memset(&runas_ctx, 0, sizeof(runas_ctx));

    debug_return;
}

/*
 * Cleanup hook for sudo_fatal()/sudo_fatalx()
 * Also called at policy close time.
 */
void
sudoers_cleanup(void)
{
    struct sudo_nss *nss;
    struct defaults *def;
    debug_decl(sudoers_cleanup, SUDOERS_DEBUG_PLUGIN);

    if (snl != NULL) {
	TAILQ_FOREACH(nss, snl, entries) {
	    nss->close(nss);
	}
	snl = NULL;
	reset_parser();
    }
    while ((def = TAILQ_FIRST(&initial_defaults)) != NULL) {
	TAILQ_REMOVE(&initial_defaults, def, entries);
	free(def->var);
	free(def->val);
	free(def);
    }
    need_reinit = false;
    if (def_group_plugin)
	group_plugin_unload();
    sudoers_user_ctx_free();
    sudoers_runas_ctx_free();
    sudo_freepwcache();
    sudo_freegrcache();
    canon_path_free_cache();

    /* We must free the cached environment before running g/c. */
    env_init(NULL);

    /* Run garbage collector. */
    sudoers_gc_run();

    /* Clear globals */
    list_pw = NULL;
    saved_argv = NULL;
    NewArgv = NULL;
    NewArgc = 0;
    prev_user = NULL;

    debug_return;
}

static bool
tty_present(void)
{
    debug_decl(tty_present, SUDOERS_DEBUG_PLUGIN);
    
    if (user_ctx.tcpgid == 0 && user_ctx.ttypath == NULL) {
	/* No job control or terminal, check /dev/tty. */
	int fd = open(_PATH_TTY, O_RDWR);
	if (fd == -1)
	    debug_return_bool(false);
	close(fd);
    }
    debug_return_bool(true);
}
