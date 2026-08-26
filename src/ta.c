/* Build BearSSL trust anchors from a PEM CA bundle on disk.
 * Same approach as BearSSL's own tools/certs.c, with fixed static pools
 * instead of heap allocation. */

#include "ta.h"
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static br_x509_trust_anchor g_tas[LAMBDA_TA_MAX];
static size_t g_ntas;
static unsigned char g_pool[LAMBDA_TA_POOL]; /* dn + key bytes */
static size_t g_pool_used;
static int g_loaded;

static const char *bundle_candidates[] = {
    "/etc/ssl/certs/ca-certificates.crt", /* debian, arch, ubuntu */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* fedora, rhel */
    "/etc/ssl/ca-bundle.pem",             /* opensuse */
    "/etc/ssl/cert.pem",                  /* macos, alpine, openbsd */
    NULL,
};

static unsigned char *pool_dup(const void *p, size_t n)
{
    if (g_pool_used + n > sizeof g_pool)
        return NULL;
    unsigned char *q = g_pool + g_pool_used;
    memcpy(q, p, n);
    g_pool_used += n;
    return q;
}

static void dn_append(void *ctx, const void *data, size_t len)
{
    buf_append((buf *)ctx, (const char *)data, len);
}

/* decode one DER certificate into a trust anchor; 0 on success */
static int ta_from_der(const unsigned char *der, size_t len,
                       br_x509_trust_anchor *ta)
{
    br_x509_decoder_context dc;
    static char dn_store[4096];
    buf dn;
    br_x509_pkey *pk;

    buf_attach(&dn, dn_store, sizeof dn_store);
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc) != 0 || dn.overflow)
        return -1;

    ta->dn.data = pool_dup(dn.data, dn.len);
    if (!ta->dn.data)
        return -1;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;

    switch (pk->key_type) {
    case BR_KEYTYPE_RSA:
        ta->pkey.key_type = BR_KEYTYPE_RSA;
        ta->pkey.key.rsa.n = pool_dup(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = pool_dup(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        return (ta->pkey.key.rsa.n && ta->pkey.key.rsa.e) ? 0 : -1;
    case BR_KEYTYPE_EC:
        ta->pkey.key_type = BR_KEYTYPE_EC;
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = pool_dup(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        return ta->pkey.key.ec.q ? 0 : -1;
    default:
        return -1;
    }
}

static void der_append(void *ctx, const void *data, size_t len)
{
    buf_append((buf *)ctx, (const char *)data, len);
}

static int load_bundle(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    br_pem_decoder_context pc;
    static char der_store[8192]; /* one DER cert */
    buf der;
    int in_cert = 0;

    buf_attach(&der, der_store, sizeof der_store);
    g_ntas = 0;
    g_pool_used = 0;
    br_pem_decoder_init(&pc);

    char fbuf[8192];
    size_t n;
    while ((n = fread(fbuf, 1, sizeof fbuf, f)) > 0) {
        size_t off = 0;
        while (off < n) {
            off += br_pem_decoder_push(&pc, fbuf + off, n - off);
            switch (br_pem_decoder_event(&pc)) {
            case 0:
                break;
            case BR_PEM_BEGIN_OBJ:
                in_cert =
                    strstr(br_pem_decoder_name(&pc), "CERTIFICATE") != NULL &&
                    strstr(br_pem_decoder_name(&pc), "CRL") == NULL;
                if (in_cert) {
                    buf_reset(&der);
                    br_pem_decoder_setdest(&pc, der_append, &der);
                } else {
                    br_pem_decoder_setdest(&pc, NULL, NULL);
                }
                break;
            case BR_PEM_END_OBJ:
                if (in_cert && der.len > 0 && !der.overflow &&
                    g_ntas < LAMBDA_TA_MAX) {
                    if (ta_from_der((unsigned char *)der.data, der.len,
                                    &g_tas[g_ntas]) == 0)
                        g_ntas++;
                    /* skip certs we can't decode or fit */
                }
                in_cert = 0;
                break;
            default: /* BR_PEM_ERROR: skip garbage, restart decoder */
                br_pem_decoder_init(&pc);
                in_cert = 0;
                break;
            }
        }
    }
    fclose(f);
    return g_ntas > 0 ? 0 : -1;
}

int ta_load(char *err, size_t errsz)
{
    if (g_loaded)
        return 0;

    const char *env = getenv("SSL_CERT_FILE");
    if (env && *env) {
        if (load_bundle(env) == 0) {
            g_loaded = 1;
            return 0;
        }
        snprintf(err, errsz, "cannot load CA bundle from $SSL_CERT_FILE (%s)",
                 env);
        return -1;
    }
    for (int i = 0; bundle_candidates[i]; i++) {
        if (load_bundle(bundle_candidates[i]) == 0) {
            g_loaded = 1;
            return 0;
        }
    }
    snprintf(err, errsz,
             "no CA bundle found (set SSL_CERT_FILE to a PEM bundle)");
    return -1;
}

const br_x509_trust_anchor *ta_anchors(void)
{
    return g_tas;
}

size_t ta_count(void)
{
    return g_ntas;
}
