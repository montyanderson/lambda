#ifndef LAMBDA_TA_H
#define LAMBDA_TA_H

#include <stddef.h>
#include <bearssl.h>

/* Load trust anchors from the system CA bundle (or $SSL_CERT_FILE).
 * Idempotent; returns 0 on success, -1 on failure with err filled. */
int ta_load(char *err, size_t errsz);

const br_x509_trust_anchor *ta_anchors(void);
size_t ta_count(void);

#endif
