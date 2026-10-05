/*
 * auth.c — PAM (pam_start / pam_authenticate / pam_acct_mgmt).
 *
 * Servicio PAM: variable de entorno MCS_PAM_SERVICE, por omisión
 * "monitoreo" (se instala con `make install-pam`, ver server/pam/monitoreo).
 * Perfil ADMIN: el usuario pertenece al grupo MCS_ADMIN_GROUP (por omisión
 * "mcs-admin"); cualquier otro usuario autenticado recibe el perfil VIEWER.
 *
 * Si se compila con -DMCS_AUTH_STUB (objetivo `make stub`) se usa un
 * verificador de prueba que NO consulta el sistema; existe solo para las
 * pruebas automatizadas de tests/ y nunca forma parte del binario normal.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "protocol.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MCS_AUTH_STUB
#include <security/pam_appl.h>
#endif

const char *auth_pam_service(void)
{
    const char *s = getenv("MCS_PAM_SERVICE");
    return (s && *s) ? s : "monitoreo";
}

const char *auth_admin_group(void)
{
    const char *g = getenv("MCS_ADMIN_GROUP");
    return (g && *g) ? g : "mcs-admin";
}

#ifdef MCS_AUTH_STUB

const char *auth_backend(void) { return "stub"; }

int auth_check(const char *user, const char *password, uint8_t *profile,
               char *why, size_t why_cap)
{
    (void)why; (void)why_cap;
    if (strcmp(password, "test") != 0)
        return AUTH_DENIED;
    *profile = strcmp(user, "admin") == 0 ? PROFILE_ADMIN : PROFILE_VIEWER;
    return AUTH_OK;
}

#else

const char *auth_backend(void) { return "pam"; }

/* Conversación PAM: responde con la contraseña a los prompts sin eco. */
static int conv_fn(int num_msg, const struct pam_message **msg,
                   struct pam_response **resp, void *appdata)
{
    const char *password = appdata;
    struct pam_response *r;

    if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
        return PAM_CONV_ERR;
    r = calloc((size_t)num_msg, sizeof *r);
    if (!r)
        return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; i++) {
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            r[i].resp = strdup(password);
            if (!r[i].resp) goto fail;
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            break;
        default:
            goto fail;
        }
    }
    *resp = r;
    return PAM_SUCCESS;

fail:
    for (int i = 0; i < num_msg; i++)
        free(r[i].resp);
    free(r);
    return PAM_CONV_ERR;
}

/* 1 si `user` pertenece al grupo `group` (primario o suplementario). */
static int user_in_group(const char *user, const char *group)
{
    struct passwd pw, *pwp = NULL;
    struct group gr, *grp = NULL;
    char pwbuf[4096], grbuf[16384];
    gid_t groups[256];
    int ngroups = 256;

    if (getpwnam_r(user, &pw, pwbuf, sizeof pwbuf, &pwp) != 0 || !pwp)
        return 0;
    if (getgrnam_r(group, &gr, grbuf, sizeof grbuf, &grp) != 0 || !grp)
        return 0;
    if (getgrouplist(user, pw.pw_gid, groups, &ngroups) < 0)
        return 0;
    for (int i = 0; i < ngroups; i++)
        if (groups[i] == gr.gr_gid)
            return 1;
    return 0;
}

int auth_check(const char *user, const char *password, uint8_t *profile,
               char *why, size_t why_cap)
{
    struct pam_conv conv = { conv_fn, (void *)password };
    pam_handle_t *pamh = NULL;
    int rc;

    rc = pam_start(auth_pam_service(), user, &conv, &pamh);
    if (rc != PAM_SUCCESS) {
        snprintf(why, why_cap, "pam_start: %s", pam_strerror(pamh, rc));
        return AUTH_INTERNAL;
    }

    rc = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
    if (rc == PAM_SUCCESS)
        rc = pam_acct_mgmt(pamh, PAM_DISALLOW_NULL_AUTHTOK);

    int result;
    switch (rc) {
    case PAM_SUCCESS:
        result = AUTH_OK;
        break;
    case PAM_AUTH_ERR:
    case PAM_USER_UNKNOWN:
    case PAM_MAXTRIES:
    case PAM_PERM_DENIED:
    case PAM_ACCT_EXPIRED:
    case PAM_NEW_AUTHTOK_REQD:
    case PAM_CRED_INSUFFICIENT:
        result = AUTH_DENIED;
        break;
    default:
        snprintf(why, why_cap, "pam: %s", pam_strerror(pamh, rc));
        result = AUTH_INTERNAL;
        break;
    }
    pam_end(pamh, rc);

    if (result == AUTH_OK)
        *profile = user_in_group(user, auth_admin_group()) ? PROFILE_ADMIN
                                                            : PROFILE_VIEWER;
    return result;
}

#endif
