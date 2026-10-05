/*
 * auth.h — autenticación de clientes administradores contra las cuentas del
 * sistema operativo mediante PAM, y asignación de perfil por pertenencia a
 * un grupo Unix. El servidor no guarda usuarios ni contraseñas propios
 * (requerimiento 6).
 */
#ifndef MCS_AUTH_H
#define MCS_AUTH_H

#include <stddef.h>
#include <stdint.h>

enum { AUTH_OK = 0, AUTH_DENIED = 1, AUTH_INTERNAL = -1 };

/* Nombre del backend ("pam" o "stub") y del servicio/grupo, para el log. */
const char *auth_backend(void);
const char *auth_pam_service(void);
const char *auth_admin_group(void);

/*
 * Verifica las credenciales. En éxito escribe el perfil en *profile.
 * En AUTH_INTERNAL, `why` recibe una descripción para el log del servidor.
 */
int auth_check(const char *user, const char *password, uint8_t *profile,
               char *why, size_t why_cap);

#endif
