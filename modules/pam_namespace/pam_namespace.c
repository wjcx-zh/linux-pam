/******************************************************************************
 * A module for Linux-PAM that will set the default namespace after
 * establishing a session via PAM.
 *
 * (C) Copyright IBM Corporation 2005
 * (C) Copyright Red Hat 2006
 * All Rights Reserved.
 *
 * Written by: Janak Desai <janak@us.ibm.com>
 * With Revisions by: Steve Grubb <sgrubb@redhat.com>
 * Derived from a namespace setup patch by Chad Sellers <cdselle@tycho.nsa.gov>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * IBM AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "pam_namespace.h"

/*
 * Copies the contents of ent into pent
 */
static int copy_ent(const struct polydir_s *ent, struct polydir_s *pent)
{
	unsigned int i;

	strcpy(pent->dir, ent->dir);
	strcpy(pent->instance_prefix, ent->instance_prefix);
	pent->method = ent->method;
	pent->num_uids = ent->num_uids;
	pent->exclusive = ent->exclusive;
	if (ent->num_uids) {
		uid_t *pptr, *eptr;

		pent->uid = (uid_t *) malloc(ent->num_uids * sizeof(uid_t));
		if (!(pent->uid)) {
			return -1;
		}
		for (i = 0, pptr = pent->uid, eptr = ent->uid; i < ent->num_uids;
				i++, eptr++, pptr++)
			*pptr = *eptr;
	} else
		pent->uid = NULL;
	return 0;
}

/*
 * Adds an entry for a polyinstantiated directory to the linked list of
 * polyinstantiated directories. It is called from process_line() while
 * parsing the namespace configuration file.
 */
static int add_polydir_entry(struct instance_data *idata,
	const struct polydir_s *ent)
{
    struct polydir_s *pent;
    int rc = 0;

    /*
     * Allocate an entry to hold information about a directory to
     * polyinstantiate, populate it with information from 2nd argument
     * and add the entry to the linked list of polyinstantiated
     * directories.
     */
    pent = (struct polydir_s *) malloc(sizeof(struct polydir_s));
	if (!pent) {
		rc = -1;
		goto out;
	}
    /* Make copy */
	rc = copy_ent(ent,pent);
	if(rc < 0)
		goto out_clean;

    /* Now attach to linked list */
    pent->next = NULL;
    if (idata->polydirs_ptr == NULL)
        idata->polydirs_ptr = pent;
    else {
        struct polydir_s *tail;

        tail = idata->polydirs_ptr;
        while (tail->next)
            tail = tail->next;
        tail->next = pent;
    }
    goto out;
out_clean:
	free(pent);
out:
	return rc;
}


/*
 * Deletes all the entries in the linked list.
 */
static void del_polydir_list(struct polydir_s *polydirs_ptr)
{
        struct polydir_s *dptr = polydirs_ptr;

	while (dptr) {
        	struct polydir_s *tptr = dptr;
		dptr = dptr->next;
            	free(tptr->uid);
		free(tptr);
	}
}

static void cleanup_data(pam_handle_t *pamh UNUSED , void *data, int err UNUSED)
{
	del_polydir_list(data);
}

/*
 * Called from parse_config_file, this function processes a single line
 * of the namespace configuration file. It skips over comments and incomplete
 * or malformed lines. It processes a valid line with information on
 * polyinstantiating a directory by populating appropriate fields of a
 * polyinstatiated directory structure and then calling add_polydir_entry to
 * add that entry to the linked list of polyinstantiated directories.
 */
static int process_line(char *line, const char *home,
			struct instance_data *idata)
{
    const char *dir, *instance_prefix;
    const char *method, *uids;
    char *tptr;
    struct polydir_s poly;
    int retval = 0;

    poly.uid = NULL;
    poly.num_uids = 0;
    poly.exclusive = 0;

    /*
     * skip the leading white space
     */
    while (*line && isspace(*line))
        line++;

    /*
     * Rip off the comments
     */
    tptr = strchr(line,'#');
    if (tptr)
        *tptr = '\0';

    /*
     * Rip off the newline char
     */
    tptr = strchr(line,'\n');
    if (tptr)
        *tptr = '\0';

    /*
     * Anything left ?
     */
    if (line[0] == 0)
        return 0;

    /*
     * Initialize and scan the five strings from the line from the
     * namespace configuration file.
     */
    dir = strtok_r(line, " \t", &tptr);
    if (dir == NULL) {
        pam_syslog(idata->pamh, LOG_NOTICE, "Invalid line missing polydir");
        goto skipping;
    }
    instance_prefix = strtok_r(NULL, " \t", &tptr);
    if (instance_prefix == NULL) {
        pam_syslog(idata->pamh, LOG_NOTICE, "Invalid line missing instance_prefix");
        goto skipping;
    }
    method = strtok_r(NULL, " \t", &tptr);
    if (method == NULL) {
        pam_syslog(idata->pamh, LOG_NOTICE, "Invalid line missing method");
        goto skipping;
    }

    /*
     * Only the uids field is allowed to be blank, to indicate no
     * override users for polyinstantiation of that directory. If
     * any of the other fields are blank, the line is incomplete so
     * skip it.
     */
    uids = strtok_r(NULL, " \t", &tptr);

    /*
     * If the directory being polyinstantiated is the home directory
     * of the user who is establishing a session, we have to swap
     * the "$HOME" string with the user's home directory that is
     * passed in as an argument.
     */
    if (strcmp(dir, "$HOME") == 0) {
	dir = home;
    }

    /*
     * Expand $HOME and $USER in instance dir prefix
     */
    if ((tptr = strstr(instance_prefix, "$USER")) != 0) {
	/* FIXME: should only support this if method is USER or BOTH */
	char *expanded = alloca(strlen(idata->user) + strlen(instance_prefix)-5+1);
	*tptr = 0;
	sprintf(expanded, "%s%s%s", instance_prefix, idata->user, tptr+5);
	instance_prefix = expanded;
    }
    if ((tptr = strstr(instance_prefix, "$HOME")) != 0) {
	char *expanded = alloca(strlen(home)+strlen(instance_prefix)-5+1);
	*tptr = 0;
	sprintf(expanded, "%s%s%s", instance_prefix, home, tptr+5);
	instance_prefix = expanded;
    }

    /*
     * Populate polyinstantiated directory structure with appropriate
     * pathnames and the method with which to polyinstantiate.
     */
    if (strlen(dir) >= sizeof(poly.dir)
	|| strlen(instance_prefix) >= sizeof(poly.instance_prefix)) {
	pam_syslog(idata->pamh, LOG_NOTICE, "Pathnames too long");
	goto skipping;
    }
    strcpy(poly.dir, dir);
    strcpy(poly.instance_prefix, instance_prefix);

    poly.method = NONE;
    if (strcmp(method, "user") == 0) 
	    poly.method = USER;
    
    if (strcmp(method, "tmpdir") == 0) {
    	    poly.method = TMPDIR;
    	    if (sizeof(poly.instance_prefix) - strlen(poly.instance_prefix) < 7) {
    		pam_syslog(idata->pamh, LOG_NOTICE, "Pathnames too long");
    		goto skipping;
    	    }
    	    strcat(poly.instance_prefix, "XXXXXX");
    }
    
    if (strcmp(method, "tmpfs") == 0)
            poly.method = TMPFS;

#ifdef WITH_SELINUX
    if (strcmp(method, "level") == 0) {
        if (idata->flags & PAMNS_CTXT_BASED_INST)
            poly.method = LEVEL;
	else
            poly.method = USER;
    }

    if (strcmp(method, "context") == 0) {
        if (idata->flags & PAMNS_CTXT_BASED_INST)
            poly.method = CONTEXT;
	else
            poly.method = USER;
    }

#endif

    if (poly.method == NONE) {
        pam_syslog(idata->pamh, LOG_NOTICE, "Illegal method");
        goto skipping;
    }

    /*
     * Ensure that all pathnames are absolute path names.
     */
    if ((dir[0] != '/') || (poly.method != TMPFS && instance_prefix[0] != '/')) {
        pam_syslog(idata->pamh, LOG_NOTICE, "Pathnames must start with '/'");
        goto skipping;
    }
    if (strstr(dir, "..") || strstr(instance_prefix, "..")) {
        pam_syslog(idata->pamh, LOG_NOTICE, "Pathnames must not contain '..'");
        goto skipping;
    }

    /*
     * If the line in namespace.conf for a directory to polyinstantiate
     * contains a list of override users (users for whom polyinstantiation
     * is not performed), read the user ids, convert names into uids, and
     * add to polyinstantiated directory structure.
     */
    if (uids) {
        uid_t *uidptr;
        const char *ustr, *sstr;
        int count, i;
	
	if (*uids == '~') {
		poly.exclusive = 1;
		uids++;
	}
        for (count = 0, ustr = sstr = uids; sstr; ustr = sstr + 1, count++)
           sstr = strchr(ustr, ',');

        poly.num_uids = count;
        poly.uid = (uid_t *) malloc(count * sizeof (uid_t));
        uidptr = poly.uid;
        if (uidptr == NULL) {
            pam_syslog(idata->pamh, LOG_NOTICE, "out of memory");
            goto skipping;
        }

        ustr = uids;
        for (i = 0; i < count; i++) {
            struct passwd *pwd;

            tptr = strchr(ustr, ',');
            if (tptr)
                *tptr = '\0';

            pwd = pam_modutil_getpwnam(idata->pamh, ustr);
            if (pwd == NULL) {
        	pam_syslog(idata->pamh, LOG_ERR, "Unknown user %s in configuration", ustr);
        	poly.num_uids--;	
            } else {
                *uidptr = pwd->pw_uid;
                uidptr++;
            }
            ustr = tptr + 1;
        }
    }

    /*
     * Add polyinstantiated directory structure to the linked list
     * of all polyinstantiated directory structures.
     */
    if (add_polydir_entry(idata, &poly) < 0) {
        pam_syslog(idata->pamh, LOG_ERR, "Allocation Error");
        retval = PAM_SERVICE_ERR;
    }
    free(poly.uid);

    goto out;

skipping:
    if (idata->flags & PAMNS_IGN_CONFIG_ERR)
        retval = 0;
    else
        retval = PAM_SERVICE_ERR;
out:
    return retval;
}


/*
 * Parses /etc/security/namespace.conf file to build a linked list of
 * polyinstantiated directory structures of type polydir_s. Each entry
 * in the linked list contains information needed to polyinstantiate
 * one directory.
 */
static int parse_config_file(struct instance_data *idata)
{
    FILE *fil;
    char *home;
    struct passwd *cpwd;
    char *line = NULL;
    int retval;
    size_t len = 0;

    if (idata->flags & PAMNS_DEBUG)
        pam_syslog(idata->pamh, LOG_DEBUG, "Parsing config file %s",
		PAM_NAMESPACE_CONFIG);

    /*
     * Extract the user's home directory to resolve $HOME entries
     * in the namespace configuration file.
     */
    cpwd = pam_modutil_getpwnam(idata->pamh, idata->user);
    if (!cpwd) {
        pam_syslog(idata->pamh, LOG_ERR,
               "Error getting home dir for '%s'", idata->user);
        return PAM_SESSION_ERR;
    }
    home = strdupa(cpwd->pw_dir);

    /*
     * Open configuration file, read one line at a time and call
     * process_line to process each line.
     */
    fil = fopen(PAM_NAMESPACE_CONFIG, "r");
    if (fil == NULL) {
        pam_syslog(idata->pamh, LOG_ERR, "Error opening config file");
        return PAM_SERVICE_ERR;
    }

    /* Use unlocked IO */
    __fsetlocking(fil, FSETLOCKING_BYCALLER);

    /* loop reading the file */
    while (getline(&line, &len, fil) > 0) {
        retval = process_line(line, home, idata);
        if (retval) {
            pam_syslog(idata->pamh, LOG_ERR,
		"Error processing conf file line %s", line);
            fclose(fil);
            free(line);
            return PAM_SERVICE_ERR;
        }
    }
    fclose(fil);
    free(line);

    /* All done...just some debug stuff */
    if (idata->flags & PAMNS_DEBUG) {
        struct polydir_s *dptr = idata->polydirs_ptr;
        uid_t *iptr;
        uid_t i;

        pam_syslog(idata->pamh, LOG_DEBUG,
    	    dptr?"Configured poly dirs:":"No configured poly dirs");
        while (dptr) {
            pam_syslog(idata->pamh, LOG_DEBUG, "dir='%s' iprefix='%s' meth=%d",
		   dptr->dir, dptr->instance_prefix, dptr->method);
            for (i = 0, iptr = dptr->uid; i < dptr->num_uids; i++, iptr++)
                pam_syslog(idata->pamh, LOG_DEBUG, "override user %d ", *iptr);
            dptr = dptr->next;
        }
    }

    return PAM_SUCCESS;
}


/*
 * This funtion returns true if a given uid is present in the polyinstantiated
 * directory's list of override uids. If the uid is one of the override
 * uids for the polyinstantiated directory, polyinstantiation is not
 * performed for that user for that directory.
 * If exclusive is set the returned values are opposite.
 */
static int ns_override(struct polydir_s *polyptr, struct instance_data *idata,
		uid_t uid)
{
    unsigned int i;

    if (idata->flags & PAMNS_DEBUG)
    	pam_syslog(idata->pamh, LOG_DEBUG,
		"Checking for ns override in dir %s for uid %d",
		polyptr->dir, uid);

    for (i = 0; i < polyptr->num_uids; i++) {
        if (uid == polyptr->uid[i]) {
            return !polyptr->exclusive;
        }
    }

    return polyptr->exclusive;
}

/*
 * md5hash generates a hash of the passed in instance directory name.
 */
static char *md5hash(const char *instname, struct instance_data *idata)
{
    int i;
    char *md5inst = NULL;
    char *to;
    unsigned char inst_digest[MD5_DIGEST_LENGTH];

    /*
     * Create MD5 hashes for instance pathname.
     */

    MD5((const unsigned char *)instname, strlen(instname), inst_digest);

    if ((md5inst = malloc(MD5_DIGEST_LENGTH * 2 + 1)) == NULL) {
        pam_syslog(idata->pamh, LOG_ERR, "Unable to allocate buffer");
        return NULL;
    }

    to = md5inst;
    for (i = 0; i < MD5_DIGEST_LENGTH; i++) {
        snprintf(to, 3, "%02x", (unsigned int)inst_digest[i]);
        to += 2;
    }

    return md5inst;
}

#ifdef WITH_SELINUX
static int form_context(const struct polydir_s *polyptr,
		security_context_t *i_context, security_context_t *origcon,
		struct instance_data *idata)
{
	int rc = PAM_SUCCESS;
	security_context_t scon = NULL;
	security_class_t tclass;

	/*
	 * Get the security context of the directory to polyinstantiate.
	 */
	rc = getfilecon(polyptr->dir, origcon);
	if (rc < 0 || *origcon == NULL) {
		pam_syslog(idata->pamh, LOG_ERR,
				"Error getting poly dir context, %m");
		return PAM_SESSION_ERR;
	}

	if (polyptr->method == USER) return PAM_SUCCESS;

	rc = getexeccon(&scon);
	if (rc < 0 || scon == NULL) {
		pam_syslog(idata->pamh, LOG_ERR, 
			   "Error getting exec context, %m");
		return PAM_SESSION_ERR;
	}

	/*
	 * If polyinstantiating based on security context, get current
	 * process security context, get security class for directories,
	 * and ask the policy to provide security context of the
	 * polyinstantiated instance directory.
	 */

	if (polyptr->method == CONTEXT) {
		tclass = string_to_security_class("dir");

		if (security_compute_member(scon, *origcon, tclass,
					i_context) < 0) {
			pam_syslog(idata->pamh, LOG_ERR,
					"Error computing poly dir member context");
			freecon(scon);
			return PAM_SESSION_ERR;
		} else if (idata->flags & PAMNS_DEBUG)
			pam_syslog(idata->pamh, LOG_DEBUG,
					"member context returned by policy %s", *i_context);
		freecon(scon);
		return PAM_SUCCESS;
	}

	/*
	 * If polyinstantiating based on security level, get current
	 * process security context, get security class for directories,
	 * and change the directories MLS Level to match process.
	 */

	if (polyptr->method == LEVEL) {
		context_t scontext = NULL;
		context_t fcontext = NULL;
		rc = PAM_SESSION_ERR;

		scontext = context_new(scon);
		if (! scontext) {
			pam_syslog(idata->pamh, LOG_ERR, "out of memory");
			goto fail;
		}
		fcontext = context_new(*origcon);
		if (! fcontext) {
			pam_syslog(idata->pamh, LOG_ERR, "out of memory");
			goto fail;
		}
		if (context_range_set(fcontext, context_range_get(scontext)) != 0) {
			pam_syslog(idata->pamh, LOG_ERR, "Unable to set MLS Componant of context");
			goto fail;
		}
		*i_context=strdup(context_str(fcontext));
		if (! *i_context) {
			pam_syslog(idata->pamh, LOG_ERR, "out of memory");
			goto fail;
		}

		rc = PAM_SUCCESS;
 fail:
		context_free(scontext);
		context_free(fcontext);
		freecon(scon);
		return rc;
	}
	/* Should never get here */
	return PAM_SUCCESS;
}
#endif

/*
 * poly_name returns the name of the polyinstantiated instance directory
 * based on the method used for polyinstantiation (user, context or both)
 * In addition, the function also returns the security contexts of the
 * original directory to polyinstantiate and the polyinstantiated instance
 * directory.
 */
#ifdef WITH_SELINUX
static int poly_name(const struct polydir_s *polyptr, char **i_name,
	security_context_t *i_context, security_context_t *origcon,
        struct instance_data *idata)
#else
static int poly_name(const struct polydir_s *polyptr, char **i_name,
	struct instance_data *idata)
#endif
{
    int rc;
    char *hash = NULL;
#ifdef WITH_SELINUX
    security_context_t rawcon = NULL;
#endif

    *i_name = NULL;
#ifdef WITH_SELINUX
    *i_context = NULL;
    *origcon = NULL;
    if ((idata->flags & PAMNS_SELINUX_ENABLED) &&
	(rc=form_context(polyptr, i_context, origcon, idata)) != PAM_SUCCESS) {
	    return rc;
    }
#endif

    rc = PAM_SESSION_ERR;
    /*
     * Set the name of the polyinstantiated instance dir based on the
     * polyinstantiation method.
     */
    switch (polyptr->method) {
        case USER:
	    if (asprintf(i_name, "%s", idata->user) < 0) {
		*i_name = NULL;
		goto fail;
	    }	    
    	    break;

#ifdef WITH_SELINUX
    	case LEVEL:
        case CONTEXT:
	    if (selinux_trans_to_raw_context(*i_context, &rawcon) < 0) {
		pam_syslog(idata->pamh, LOG_ERR, "Error translating directory context");
		goto fail;
	    }    	     
	    if (asprintf(i_name, "%s_%s", rawcon, idata->user) < 0) {
		*i_name = NULL;
		goto fail;
	    }
    	    break;

#endif /* WITH_SELINUX */

	case TMPDIR:
	case TMPFS:
	    if ((*i_name=strdup("")) == NULL)
	    	goto fail;
	    return PAM_SUCCESS;

    	default:
    	    if (idata->flags & PAMNS_DEBUG)
    	        pam_syslog(idata->pamh, LOG_ERR, "Unknown method");
    	    goto fail;
    }

    if (idata->flags & PAMNS_DEBUG)
        pam_syslog(idata->pamh, LOG_DEBUG, "poly_name %s", *i_name);

    if ((idata->flags & PAMNS_GEN_HASH) || strlen(*i_name) > NAMESPACE_MAX_DIR_LEN) {
        hash = md5hash(*i_name, idata);
        if (hash == NULL) {
    	    goto fail;
        }
        if (idata->flags & PAMNS_GEN_HASH) {
    	    free(*i_name);
	    *i_name = hash;
	    hash = NULL;
        } else {
    	    char *newname;
    	    if (asprintf(&newname, "%.*s_%s", NAMESPACE_MAX_DIR_LEN-1-(int)strlen(hash),
    		*i_name, hash) < 0) {
    		goto fail;
    	    }
    	    free(*i_name);
    	    *i_name = newname;
        }
    }
    rc = PAM_SUCCESS;
    
fail:
    free(hash);
#ifdef WITH_SELINUX
    freecon(rawcon);
#endif
    if (rc != PAM_SUCCESS) {
#ifdef WITH_SELINUX
	freecon(*i_context);
	*i_context = NULL;
	freecon(*origcon);
	*origcon = NULL;
#endif
	free(*i_name);
	*i_name = NULL;
    }
    return rc;
}

static int check_inst_parent(char *ipath, struct instance_data *idata)
{
	struct stat instpbuf;
	char *inst_parent, *trailing_slash;
	/*
	 * stat the instance parent path to make sure it exists
	 * and is a directory. Check that its mode is 000 (unless the
	 * admin explicitly instructs to ignore the instance parent
	 * mode by the "ignore_instance_parent_mode" argument).
	 */
	inst_parent = (char *) malloc(strlen(ipath)+1);
	if (!inst_parent) {
		pam_syslog(idata->pamh, LOG_ERR, "Error allocating pathname string");
		return PAM_SESSION_ERR;
	}

	strcpy(inst_parent, ipath);
	trailing_slash = strrchr(inst_parent, '/');
	if (trailing_slash)
		*trailing_slash = '\0';

	if (stat(inst_parent, &instpbuf) < 0) {
		pam_syslog(idata->pamh, LOG_ERR, "Error stating %s, %m", inst_parent);
		free(inst_parent);
		return PAM_SESSION_ERR;
	}

	/*
	 * Make sure we are dealing with a directory
	 */
	if (!S_ISDIR(instpbuf.st_mode)) {
		pam_syslog(idata->pamh, LOG_ERR, "Instance parent %s is not a dir",
				inst_parent);
		free(inst_parent);
		return PAM_SESSION_ERR;
	}

	if ((idata->flags & PAMNS_IGN_INST_PARENT_MODE) == 0) {
		if (instpbuf.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO)) {
			pam_syslog(idata->pamh, LOG_ERR, "Mode of inst parent %s not 000",
					inst_parent);
			free(inst_parent);
			return PAM_SESSION_ERR;
		}
	}
	free(inst_parent);
	return PAM_SUCCESS;
}

/*
* Check to see if there is a namespace initialization script in
* the /etc/security directory. If such a script exists
* execute it and pass directory to polyinstantiate and instance
* directory as arguments.
*/
static int inst_init(const struct polydir_s *polyptr, const char *ipath,
	   struct instance_data *idata, int newdir)
{
	pid_t rc, pid;
	sighandler_t osighand = NULL;
	int status;

	osighand = signal(SIGCHLD, SIG_DFL);
	if (osighand == SIG_ERR) {
		pam_syslog(idata->pamh, LOG_ERR, "Cannot set signal value");
		rc = PAM_SESSION_ERR;
		goto out;
	}

	if (access(NAMESPACE_INIT_SCRIPT, F_OK) == 0) {
		if (access(NAMESPACE_INIT_SCRIPT, X_OK) < 0) {
			if (idata->flags & PAMNS_DEBUG)
				pam_syslog(idata->pamh, LOG_ERR,
						"Namespace init script not executable");
			rc = PAM_SESSION_ERR;
			goto out;
		} else {
			pid = fork();
			if (pid == 0) {
#ifdef WITH_SELINUX
				if (idata->flags & PAMNS_SELINUX_ENABLED) {
					if (setexeccon(NULL) < 0)
						exit(1);
				}
#endif
				if (execl(NAMESPACE_INIT_SCRIPT, NAMESPACE_INIT_SCRIPT,
					polyptr->dir, ipath, newdir?"1":"0", idata->user, (char *)NULL) < 0)
					exit(1);
			} else if (pid > 0) {
				while (((rc = waitpid(pid, &status, 0)) == (pid_t)-1) &&
						(errno == EINTR));
				if (rc == (pid_t)-1) {
					pam_syslog(idata->pamh, LOG_ERR, "waitpid failed- %m");
					rc = PAM_SESSION_ERR;
					goto out;
				}
				if (!WIFEXITED(status) || WIFSIGNALED(status) > 0) {
					pam_syslog(idata->pamh, LOG_ERR,
							"Error initializing instance");
					rc = PAM_SESSION_ERR;
					goto out;
				}
			} else if (pid < 0) {
				pam_syslog(idata->pamh, LOG_ERR,
						"Cannot fork to run namespace init script, %m");
				rc = PAM_SESSION_ERR;
				goto out;
			}
		}
	}
	rc = PAM_SUCCESS;
out:
   (void) signal(SIGCHLD, osighand);

   return rc;
}

/*
 * Create polyinstantiated instance directory (ipath).
 */
#ifdef WITH_SELINUX
static int create_dirs(struct polydir_s *polyptr, char *ipath,
        security_context_t icontext, security_context_t ocontext,
	struct instance_data *idata)
#else
static int create_dirs(struct polydir_s *polyptr, char *ipath,
	struct instance_data *idata)
#endif
{
	struct stat statbuf, newstatbuf;
	int rc, fd;
	int newdir = 0;

    /*
     * stat the directory to polyinstantiate, so its owner-group-mode
     * can be propagated to instance directory
     */
	rc = PAM_SUCCESS;
    if (stat(polyptr->dir, &statbuf) < 0) {
        pam_syslog(idata->pamh, LOG_ERR, "Error stating %s, %m",
		polyptr->dir);
        return PAM_SESSION_ERR;
    }

    /*
     * Make sure we are dealing with a directory
     */
    if (!S_ISDIR(statbuf.st_mode)) {
	pam_syslog(idata->pamh, LOG_ERR, "poly dir %s is not a dir",
		polyptr->dir);
        return PAM_SESSION_ERR;
    }

	/*
	 * Check to make sure instance parent is valid.
	 */
	if (check_inst_parent(ipath, idata))
		return PAM_SESSION_ERR;

    /*
     * Create instance directory and set its security context to the context
     * returned by the security policy. Set its mode and ownership
     * attributes to match that of the original directory that is being
     * polyinstantiated.
     */
    
    if (polyptr->method == TMPDIR) {
    	if (mkdtemp(polyptr->instance_prefix) == NULL) {
            pam_syslog(idata->pamh, LOG_ERR, "Error creating temporary instance %s, %m",
			polyptr->instance_prefix);
	    polyptr->method = NONE; /* do not clean up! */
	    return PAM_SESSION_ERR;
    	}
	/* copy the actual directory name to ipath */
	strcpy(ipath, polyptr->instance_prefix);
    } else if (mkdir(ipath, S_IRUSR) < 0) {
        if (errno == EEXIST)
            goto inst_init;
        else {
            pam_syslog(idata->pamh, LOG_ERR, "Error creating %s, %m",
			ipath);
            return PAM_SESSION_ERR;
        }
    }

    newdir = 1;
    /* Open a descriptor to it to prevent races */
    fd = open(ipath, O_DIRECTORY | O_RDONLY);
    if (fd < 0) {
	pam_syslog(idata->pamh, LOG_ERR, "Error opening %s, %m", ipath);
	rmdir(ipath);
	return PAM_SESSION_ERR;
    }
#ifdef WITH_SELINUX
    /* If SE Linux is disabled, no need to label it */
    if (idata->flags & PAMNS_SELINUX_ENABLED) {
        /* If method is USER, icontext is NULL */
        if (icontext) {
            if (fsetfilecon(fd, icontext) < 0) {
                pam_syslog(idata->pamh, LOG_ERR,
			"Error setting context of %s to %s", ipath, icontext);
                close(fd);
		rmdir(ipath);
                return PAM_SESSION_ERR;
            }
        } else {
            if (fsetfilecon(fd, ocontext) < 0) {
                pam_syslog(idata->pamh, LOG_ERR,
			"Error setting context of %s to %s", ipath, ocontext);
		close(fd);
		rmdir(ipath);
                return PAM_SESSION_ERR;
            }
        }
    }
#endif
    if (fstat(fd, &newstatbuf) < 0) {
        pam_syslog(idata->pamh, LOG_ERR, "Error stating %s, %m",
		ipath);
	rmdir(ipath);
        return PAM_SESSION_ERR;
    }
    if (newstatbuf.st_uid != statbuf.st_uid ||
			 newstatbuf.st_gid != statbuf.st_gid) {
        if (fchown(fd, statbuf.st_uid, statbuf.st_gid) < 0) {
            pam_syslog(idata->pamh, LOG_ERR,
			"Error changing owner for %s, %m",
			ipath);
	    close(fd);
	    rmdir(ipath);
            return PAM_SESSION_ERR;
        }
    }
    if (fchmod(fd, statbuf.st_mode & 07777) < 0) {
        pam_syslog(idata->pamh, LOG_ERR, "Error changing mode for %s, %m",
			ipath);
	close(fd);
	rmdir(ipath);
        return PAM_SESSION_ERR;
    }
    close(fd);

    /*
     * Check to see if there is a namespace initialization script in
     * the /etc/security directory. If such a script exists
     * execute it and pass directory to polyinstantiate and instance
     * directory as arguments.
     */

inst_init:
    rc = inst_init(polyptr, ipath, idata, newdir);
    return rc;
}


/*
 * This function performs the namespace setup for a particular directory
 * that is being polyinstantiated. It creates an MD5 hash of instance
 * directory, calls create_dirs to create it with appropriate
 * security attributes, and performs bind mount to setup the process
 * namespace.
 */
static int ns_setup(struct polydir_s *polyptr,
	struct instance_data *idata)
{
    int retval = 0;
    char *inst_dir = NULL;
    char *instname = NULL;
#ifdef WITH_SELINUX
    security_context_t instcontext = NULL, origcontext = NULL;
#endif

    if (idata->flags & PAMNS_DEBUG)
        pam_syslog(idata->pamh, LOG_DEBUG,
               "Set namespace for directory %s", polyptr->dir);

    if (polyptr->method == TMPFS) {
	if (mount("tmpfs", polyptr->dir, "tmpfs", 0, NULL) < 0) {
	    pam_syslog(idata->pamh, LOG_ERR, "Error mounting tmpfs on %s, %m",
        	polyptr->dir);
            return PAM_SESSION_ERR;
	}
	/* we must call inst_init after the mount in this case */
	return inst_init(polyptr, "tmpfs", idata, 1);
    }

    /*
     * Obtain the name of instance pathname based on the
     * polyinstantiation method and instance context returned by
     * security policy.
     */
#ifdef WITH_SELINUX
    retval = poly_name(polyptr, &instname, &instcontext,
			&origcontext, idata);
#else
    retval = poly_name(polyptr, &instname, idata);
#endif

    if (retval) {
        pam_syslog(idata->pamh, LOG_ERR, "Error getting instance name");
        goto error_out;
    } else {
#ifdef WITH_SELINUX
        if ((idata->flags & PAMNS_DEBUG) &&
            (idata->flags & PAMNS_SELINUX_ENABLED))
            pam_syslog(idata->pamh, LOG_DEBUG, "Inst ctxt %s Orig ctxt %s",
		 instcontext, origcontext);
#endif
    }

    if (asprintf(&inst_dir, "%s%s", polyptr->instance_prefix, instname) < 0)
	goto error_out;

    if (idata->flags & PAMNS_DEBUG)
        pam_syslog(idata->pamh, LOG_DEBUG, "instance_dir %s",
		inst_dir);

    /*
     * Create instance directory with appropriate security
     * contexts, owner, group and mode bits.
     */
#ifdef WITH_SELINUX
    retval = create_dirs(polyptr, inst_dir, instcontext,
			 origcontext, idata);
#else
    retval = create_dirs(polyptr, inst_dir, idata);
#endif

    if (retval < 0) {
        pam_syslog(idata->pamh, LOG_ERR, "Error creating instance dir");
        goto error_out;
    }

    /*
     * Bind mount instance directory on top of the polyinstantiated
     * directory to provide an instance of polyinstantiated directory
     * based on polyinstantiated method.
     */
    if (mount(inst_dir, polyptr->dir, NULL, MS_BIND, NULL) < 0) {
        pam_syslog(idata->pamh, LOG_ERR, "Error mounting %s on %s, %m",
                   inst_dir, polyptr->dir);
        goto error_out;
    }

    goto cleanup;

    /*
     * various error exit points. Free allocated memory and set return
     * value to indicate a pam session error.
     */
error_out:
    retval = PAM_SESSION_ERR;

cleanup:
    free(inst_dir);
    free(instname);
#ifdef WITH_SELINUX
    freecon(instcontext);
    freecon(origcontext);
#endif
    return retval;
}


/*
 * This function checks to see if the current working directory is
 * inside the directory passed in as the first argument.
 */
static int cwd_in(char *dir, struct instance_data *idata)
{
    int retval = 0;
    char cwd[PATH_MAX];

    if (getcwd(cwd, PATH_MAX) == NULL) {
        pam_syslog(idata->pamh, LOG_ERR, "Can't get current dir, %m");
        return -1;
    }

    if (strncmp(cwd, dir, strlen(dir)) == 0) {
        if (idata->flags & PAMNS_DEBUG)
            pam_syslog(idata->pamh, LOG_DEBUG, "cwd is inside %s", dir);
        retval = 1;
    } else {
        if (idata->flags & PAMNS_DEBUG)
            pam_syslog(idata->pamh, LOG_DEBUG, "cwd is outside %s", dir);
    }

    return retval;
}

static int cleanup_tmpdirs(struct instance_data *idata)
{
    struct polydir_s *pptr;
    pid_t rc, pid;
    sighandler_t osighand = NULL;
    int status;

    osighand = signal(SIGCHLD, SIG_DFL);
    if (osighand == SIG_ERR) {
	pam_syslog(idata->pamh, LOG_ERR, "Cannot set signal value");
	rc = PAM_SESSION_ERR;
	goto out;
    }

    for (pptr = idata->polydirs_ptr; pptr; pptr = pptr->next) {
	if (pptr->method == TMPDIR && access(pptr->instance_prefix, F_OK) == 0) {
	    pid = fork();
	    if (pid == 0) {
#ifdef WITH_SELINUX
		if (idata->flags & PAMNS_SELINUX_ENABLED) {
		    if (setexeccon(NULL) < 0)
			exit(1);
		}
#endif
		if (execl("/bin/rm", "/bin/rm", "-rf", pptr->instance_prefix, (char *)NULL) < 0)
			exit(1);
	    } else if (pid > 0) {
		while (((rc = waitpid(pid, &status, 0)) == (pid_t)-1) &&
		    (errno == EINTR));
		if (rc == (pid_t)-1) {
		    pam_syslog(idata->pamh, LOG_ERR, "waitpid failed- %m");
		    rc = PAM_SESSION_ERR;
		    goto out;
		}
		if (!WIFEXITED(status) || WIFSIGNALED(status) > 0) {
		    pam_syslog(idata->pamh, LOG_ERR,
		    	"Error removing %s", pptr->instance_prefix);
		}
	    } else if (pid < 0) {
		pam_syslog(idata->pamh, LOG_ERR,
			"Cannot fork to run namespace init script, %m");
		rc = PAM_SESSION_ERR;
		goto out;
	    }
        }
    }

    rc = PAM_SUCCESS;
out:
    signal(SIGCHLD, osighand);
    return rc;
}

/*
 * This function checks to see if polyinstantiation is needed for any
 * of the directories listed in the configuration file. If needed,
 * cycles through all polyinstantiated directory entries and calls
 * ns_setup to setup polyinstantiation for each one of them.
 */
static int setup_namespace(struct instance_data *idata, enum unmnt_op unmnt)
{
    int retval = 0, need_poly = 0, changing_dir = 0;
    char *cptr, *fptr, poly_parent[PATH_MAX];
    struct polydir_s *pptr;
    uid_t req_uid;
    const void *ruser_name;
    struct passwd *pwd;

    if (idata->flags & PAMNS_DEBUG)
        pam_syslog(idata->pamh, LOG_DEBUG, "Set up namespace for pid %d",
		getpid());

    retval = pam_get_item(idata->pamh, PAM_RUSER, &ruser_name);
    if (ruser_name == NULL || retval != PAM_SUCCESS) {
	retval = PAM_SUCCESS;
	req_uid = getuid();
    } else {
        pwd = pam_modutil_getpwnam(idata->pamh, ruser_name);
        if (pwd != NULL) {
    	    req_uid = pwd->pw_uid;
        } else {
	    req_uid = getuid();
        }
    }

    /*
     * Cycle through all polyinstantiated directory entries to see if
     * polyinstantiation is needed at all.
     */
    for (pptr = idata->polydirs_ptr; pptr; pptr = pptr->next) {
        if (ns_override(pptr, idata, idata->uid)) {
    	    if (unmnt == NO_UNMNT || ns_override(pptr, idata, req_uid)) {
        	if (idata->flags & PAMNS_DEBUG)
        	    pam_syslog(idata->pamh, LOG_DEBUG,
			"Overriding poly for user %d for dir %s",
			idata->uid, pptr->dir);
	    } else {
        	if (idata->flags & PAMNS_DEBUG)
            	    pam_syslog(idata->pamh, LOG_DEBUG,
			"Need unmount ns for user %d for dir %s",
			idata->uid, pptr->dir);
		need_poly = 1;
		break;
	    }
            continue;
        } else {
            if (idata->flags & PAMNS_DEBUG)
                pam_syslog(idata->pamh, LOG_DEBUG,
			"Need poly ns for user %d for dir %s",
			idata->uid, pptr->dir);
            need_poly = 1;
            break;
        }
    }

    /*
     * If polyinstnatiation is needed, call the unshare system call to
     * disassociate from the parent namespace.
     */
    if (need_poly) {
	if (pam_set_data(idata->pamh, NAMESPACE_POLYDIR_DATA, idata->polydirs_ptr,
    		cleanup_data) != PAM_SUCCESS) {
    	    pam_syslog(idata->pamh, LOG_ERR,
    	    	"Unable to set namespace data");
    	    return PAM_SYSTEM_ERR;
    	}
        if (unshare(CLONE_NEWNS) < 0) {
		pam_set_data(idata->pamh, NAMESPACE_POLYDIR_DATA, NULL, NULL);
		pam_syslog(idata->pamh, LOG_ERR,
		"Unable to unshare from parent namespace, %m");
            return PAM_SESSION_ERR;
        }
    } else {
    	del_polydir_list(idata->polydirs_ptr);
        return PAM_SUCCESS;
    }

    /*
     * Again cycle through all polyinstantiated directories, this time,
     * call ns_setup to setup polyinstantiation for a particular entry.
     */
    for (pptr = idata->polydirs_ptr; pptr; pptr = pptr->next) {
	enum unmnt_op dir_unmnt = unmnt;
        if (ns_override(pptr, idata, idata->uid)) {
    	    if (unmnt == NO_UNMNT || ns_override(pptr, idata, req_uid)) {
    		continue;
	    } else {
		dir_unmnt = UNMNT_ONLY;
	    }
	}
	if (idata->flags & PAMNS_DEBUG)
                pam_syslog(idata->pamh, LOG_DEBUG,
			"Setting poly ns for user %d for dir %s",
                      idata->uid, pptr->dir);

	if ((dir_unmnt == UNMNT_REMNT) || (dir_unmnt == UNMNT_ONLY)) {
                /*
                 * Check to see if process current directory is in the
                 * bind mounted instance_parent directory that we are trying to
                 * umount
                 */
                if ((changing_dir = cwd_in(pptr->dir, idata)) < 0) {
                    retval = PAM_SESSION_ERR;
                    goto out;
                } else if (changing_dir) {
                    if (idata->flags & PAMNS_DEBUG)
                        pam_syslog(idata->pamh, LOG_DEBUG, "changing cwd");

                    /*
                     * Change current working directory to the parent of
                     * the mount point, that is parent of the orig
                     * directory where original contents of the polydir
                     * are available from
                     */
                    strcpy(poly_parent, pptr->dir);
    	            fptr = strchr(poly_parent, '/');
        	    cptr = strrchr(poly_parent, '/');
        	    if (fptr && cptr && (fptr == cptr))
        	        strcpy(poly_parent, "/");
        	    else if (cptr)
        	        *cptr = '\0';
                    if (chdir(poly_parent) < 0) {
                        pam_syslog(idata->pamh, LOG_ERR,
				"Can't chdir to %s, %m", poly_parent);
                    }
                }

                if (umount(pptr->dir) < 0) {
            	    int saved_errno = errno;
            	    pam_syslog(idata->pamh, LOG_ERR, "Unmount of %s failed, %m",
                    	pptr->dir);
            	    if (saved_errno != EINVAL) {
                	retval = PAM_SESSION_ERR;
                	goto out;
                    }
                } else if (idata->flags & PAMNS_DEBUG)
                    pam_syslog(idata->pamh, LOG_DEBUG, "Umount succeeded %s",
				pptr->dir);
	}

	if (dir_unmnt != UNMNT_ONLY) {
                retval = ns_setup(pptr, idata);
                if (retval != PAM_SUCCESS)
                     break;
        }
    }
out:
    if (retval != PAM_SUCCESS)
    	cleanup_tmpdirs(idata);
    return retval;
}


/*
 * Orig namespace. This function is called from when closing a pam
 * session. If authorized, it unmounts instance directory.
 */
static int orig_namespace(struct instance_data *idata)
{
    struct polydir_s *pptr;

    if (idata->flags & PAMNS_DEBUG)
        pam_syslog(idata->pamh, LOG_DEBUG, "orig namespace for pid %d",
		getpid());

    /*
     * Cycle through all polyinstantiated directories from the namespace
     * configuration file to see if polyinstantiation was performed for
     * this user for each of the entry. If it was, try and unmount
     * appropriate polyinstantiated instance directories.
     */
    for (pptr = idata->polydirs_ptr; pptr; pptr = pptr->next) {
        if (ns_override(pptr, idata, idata->uid))
            continue;
        else {
            if (idata->flags & PAMNS_DEBUG)
                pam_syslog(idata->pamh, LOG_DEBUG,
			"Unmounting instance dir for user %d & dir %s",
                       idata->uid, pptr->dir);

            if (umount(pptr->dir) < 0) {
                pam_syslog(idata->pamh, LOG_ERR, "Unmount of %s failed, %m",
                       pptr->dir);
                return PAM_SESSION_ERR;
            } else if (idata->flags & PAMNS_DEBUG)
                pam_syslog(idata->pamh, LOG_DEBUG, "Unmount of %s succeeded",
			pptr->dir);
	}
    }

    cleanup_tmpdirs(idata);
    return 0;
}


#ifdef WITH_SELINUX
/*
 * This function checks if the calling program has requested context
 * change by calling setexeccon(). If context change is not requested
 * then it does not make sense to polyinstantiate based on context.
 * The return value from this function is used when selecting the
 * polyinstantiation method. If context change is not requested then
 * the polyinstantiation method is set to USER, even if the configuration
 * file lists the method as "context" or "both".
 */
static int ctxt_based_inst_needed(void)
{
    security_context_t scon = NULL;
    int rc = 0;

    rc = getexeccon(&scon);
    if (rc < 0 || scon == NULL)
        return 0;
    else {
        freecon(scon);
        return 1;
    }
}
#endif


/*
 * Entry point from pam_open_session call.
 */
PAM_EXTERN int pam_sm_open_session(pam_handle_t *pamh, int flags UNUSED,
                                   int argc, const char **argv)
{
    int i, retval;
    struct instance_data idata;
    char *user_name;
    struct passwd *pwd;
    enum unmnt_op unmnt = NO_UNMNT;

    /* init instance data */
    idata.flags = 0;
    idata.polydirs_ptr = NULL;
    idata.pamh = pamh;
#ifdef WITH_SELINUX
    if (is_selinux_enabled())
        idata.flags |= PAMNS_SELINUX_ENABLED;
    if (ctxt_based_inst_needed()) 
        idata.flags |= PAMNS_CTXT_BASED_INST;
#endif

    /* Parse arguments. */
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "debug") == 0)
            idata.flags |= PAMNS_DEBUG;
        if (strcmp(argv[i], "gen_hash") == 0)
            idata.flags |= PAMNS_GEN_HASH;
        if (strcmp(argv[i], "ignore_config_error") == 0)
            idata.flags |= PAMNS_IGN_CONFIG_ERR;
        if (strcmp(argv[i], "ignore_instance_parent_mode") == 0)
            idata.flags |= PAMNS_IGN_INST_PARENT_MODE;
        if (strcmp(argv[i], "unmnt_remnt") == 0)
            unmnt = UNMNT_REMNT;
        if (strcmp(argv[i], "unmnt_only") == 0)
            unmnt = UNMNT_ONLY;
	if (strcmp(argv[i], "require_selinux") == 0) {
		if (~(idata.flags & PAMNS_SELINUX_ENABLED)) {
        		pam_syslog(idata.pamh, LOG_ERR,
		    "selinux_required option given and selinux is disabled");
			return PAM_SESSION_ERR;
		}
	}
    }
    if (idata.flags & PAMNS_DEBUG)
        pam_syslog(idata.pamh, LOG_DEBUG, "open_session - start");

    /*
     * Lookup user and fill struct items
     */
    retval = pam_get_item(idata.pamh, PAM_USER, (void*) &user_name );
    if ( user_name == NULL || retval != PAM_SUCCESS ) {
        pam_syslog(idata.pamh, LOG_ERR, "Error recovering pam user name");
        return PAM_SESSION_ERR;
    }

    pwd = pam_modutil_getpwnam(idata.pamh, user_name);
    if (!pwd) {
        pam_syslog(idata.pamh, LOG_ERR, "user unknown '%s'", user_name);
        return PAM_SESSION_ERR;
    }

    /*
     * Add the user info to the instance data so we can refer to them later.
     */
    idata.user[0] = 0;
    strncat(idata.user, user_name, sizeof(idata.user) - 1);
    idata.uid = pwd->pw_uid;

    /*
     * Parse namespace configuration file which lists directories to
     * polyinstantiate, directory where instance directories are to
     * be created and the method used for polyinstantiation.
     */
    retval = parse_config_file(&idata);
    if (retval != PAM_SUCCESS) {
	del_polydir_list(idata.polydirs_ptr);
        return PAM_SESSION_ERR;
    }

    if (idata.polydirs_ptr) {
        retval = setup_namespace(&idata, unmnt);
        if (idata.flags & PAMNS_DEBUG) {
            if (retval)
                pam_syslog(idata.pamh, LOG_DEBUG,
			"namespace setup failed for pid %d", getpid());
            else
                pam_syslog(idata.pamh, LOG_DEBUG,
			"namespace setup ok for pid %d", getpid());
        }
    } else if (idata.flags & PAMNS_DEBUG)
        pam_syslog(idata.pamh, LOG_DEBUG, "Nothing to polyinstantiate");

    if (retval != PAM_SUCCESS)
	del_polydir_list(idata.polydirs_ptr);
    return retval;
}


/*
 * Entry point from pam_close_session call.
 */
PAM_EXTERN int pam_sm_close_session(pam_handle_t *pamh, int flags UNUSED,
                                    int argc, const char **argv)
{
    int i, retval;
    struct instance_data idata;
    char *user_name;
    struct passwd *pwd;
    void *polyptr;

    /* init instance data */
    idata.flags = 0;
    idata.polydirs_ptr = NULL;
    idata.pamh = pamh;
#ifdef WITH_SELINUX
    if (is_selinux_enabled())
        idata.flags |= PAMNS_SELINUX_ENABLED;
    if (ctxt_based_inst_needed())
        idata.flags |= PAMNS_CTXT_BASED_INST;
#endif

    /* Parse arguments. */
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "debug") == 0)
            idata.flags |= PAMNS_DEBUG;
        if (strcmp(argv[i], "ignore_config_error") == 0)
            idata.flags |= PAMNS_IGN_CONFIG_ERR;
        if (strcmp(argv[i], "no_unmount_on_close") == 0)
            idata.flags |= PAMNS_NO_UNMOUNT_ON_CLOSE;
    }

    if (idata.flags & PAMNS_DEBUG)
        pam_syslog(idata.pamh, LOG_DEBUG, "close_session - start");

    /*
     * For certain trusted programs such as newrole, open session
     * is called from a child process while the parent perfoms
     * close session and pam end functions. For these commands
     * pam_close_session should not perform the unmount of the
     * polyinstantiatied directory because it will result in
     * undoing of parents polyinstantiatiaion. These commands
     * will invoke pam_namespace with the "no_unmount_on_close"
     * argument.
     */
    if (idata.flags & PAMNS_NO_UNMOUNT_ON_CLOSE) {
	if (idata.flags & PAMNS_DEBUG)
	    pam_syslog(idata.pamh, LOG_DEBUG, "close_session - sucessful");
        return PAM_SUCCESS;
    }

    /* 
     * Lookup user and fill struct items
     */
    retval = pam_get_item(idata.pamh, PAM_USER, (void*) &user_name );
    if ( user_name == NULL || retval != PAM_SUCCESS ) {
        pam_syslog(idata.pamh, LOG_ERR, "Error recovering pam user name");
        return PAM_SESSION_ERR;
    }

    pwd = pam_modutil_getpwnam(idata.pamh, user_name);
    if (!pwd) {
        pam_syslog(idata.pamh, LOG_ERR, "user unknown '%s'", user_name);
        return PAM_SESSION_ERR;
    }

    /*
     * Add the user info to the instance data so we can refer to them later.
     */
    idata.user[0] = 0;
    strncat(idata.user, user_name, sizeof(idata.user) - 1);
    idata.uid = pwd->pw_uid;

    retval = pam_get_data(idata.pamh, NAMESPACE_POLYDIR_DATA, (const void **)&polyptr);
    if (retval != PAM_SUCCESS || polyptr == NULL)
    	/* nothing to reset */
    	return PAM_SUCCESS;
    	
    idata.polydirs_ptr = polyptr;

    if (idata.flags & PAMNS_DEBUG)
        pam_syslog(idata.pamh, LOG_DEBUG, "Resetting namespace for pid %d",
		getpid());

    retval = orig_namespace(&idata);
    if (idata.flags & PAMNS_DEBUG) {
        if (retval)
            pam_syslog(idata.pamh, LOG_DEBUG,
		"resetting namespace failed for pid %d", getpid());
        else
            pam_syslog(idata.pamh, LOG_DEBUG,
		"resetting namespace ok for pid %d", getpid());
    }

    pam_set_data(idata.pamh, NAMESPACE_POLYDIR_DATA, NULL, NULL);
    
    return PAM_SUCCESS;
}

#ifdef PAM_STATIC

/* static module data */

struct pam_module _pam_namespace_modstruct = {
     "pam_namespace",
     NULL,
     NULL,
     NULL,
     pam_sm_open_session,
     pam_sm_close_session,
     NULL
};
#endif
