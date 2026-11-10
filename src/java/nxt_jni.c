
/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_auto_config.h>

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <nxt_unit.h>
#include <nxt_unit_field.h>

#include "nxt_jni.h"


static jclass     nxt_java_NoSuchElementException_class;
static jclass     nxt_java_IOException_class;
static jclass     nxt_java_IllegalStateException_class;
static jclass     nxt_java_File_class;
static jmethodID  nxt_java_File_ctor;

static inline char nxt_java_lowcase(char c);


int
nxt_java_jni_init(JNIEnv *env)
{
    jclass  cls;

    cls = (*env)->FindClass(env, "java/util/NoSuchElementException");
    if (cls == NULL) {
        return NXT_UNIT_ERROR;
    }

    nxt_java_NoSuchElementException_class = (*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);


    cls = (*env)->FindClass(env, "java/io/IOException");
    if (cls == NULL) {
        (*env)->DeleteGlobalRef(env, nxt_java_NoSuchElementException_class);
        return NXT_UNIT_ERROR;
    }

    nxt_java_IOException_class = (*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);


    cls = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (cls == NULL) {
        (*env)->DeleteGlobalRef(env, nxt_java_NoSuchElementException_class);
        (*env)->DeleteGlobalRef(env, nxt_java_IOException_class);
        return NXT_UNIT_ERROR;
    }

    nxt_java_IllegalStateException_class = (*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);


    cls = (*env)->FindClass(env, "java/io/File");
    if (cls == NULL) {
        (*env)->DeleteGlobalRef(env, nxt_java_NoSuchElementException_class);
        (*env)->DeleteGlobalRef(env, nxt_java_IOException_class);
        (*env)->DeleteGlobalRef(env, nxt_java_IllegalStateException_class);
        return NXT_UNIT_ERROR;
    }

    nxt_java_File_class = (*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);


    nxt_java_File_ctor = (*env)->GetMethodID(env, nxt_java_File_class, "<init>",
                                             "(Ljava/lang/String;)V");
    if (nxt_java_File_ctor == NULL) {
        (*env)->DeleteGlobalRef(env, nxt_java_NoSuchElementException_class);
        (*env)->DeleteGlobalRef(env, nxt_java_IOException_class);
        (*env)->DeleteGlobalRef(env, nxt_java_IllegalStateException_class);
        (*env)->DeleteGlobalRef(env, nxt_java_File_class);
        return NXT_UNIT_ERROR;
    }

    return NXT_UNIT_OK;
}


void
nxt_java_throw_NoSuchElementException(JNIEnv *env, const char *msg)
{
    (*env)->ThrowNew(env, nxt_java_NoSuchElementException_class, msg);
}


void
nxt_java_throw_IOException(JNIEnv *env, const char *msg)
{
    (*env)->ThrowNew(env, nxt_java_IOException_class, msg);
}


void
nxt_java_throw_IllegalStateException(JNIEnv *env, const char *msg)
{
    (*env)->ThrowNew(env, nxt_java_IllegalStateException_class, msg);
}


nxt_unit_field_t *
nxt_java_findHeader(nxt_unit_field_t *f, nxt_unit_field_t *end,
    const char *name, uint8_t name_len)
{
    const char  *field_name;

    for (/* void */ ; f < end; f++) {
        if (f->skip != 0 || f->name_length != name_len) {
            continue;
        }

        field_name = nxt_unit_sptr_get(&f->name);

        if (nxt_java_strcaseeq(name, field_name, name_len)) {
            return f;
        }
    }

    return NULL;
}


int
nxt_java_strcaseeq(const char *str1, const char *str2, int len)
{
    char        c1, c2;
    const char  *end1;

    end1 = str1 + len;

    while (str1 < end1) {
        c1 = *str1++;
        c2 = *str2++;

        /* Equal bytes need no case folding. */

        if (c1 != c2
            && nxt_java_lowcase(c1) != nxt_java_lowcase(c2))
        {
            return 0;
        }
    }

    return 1;
}


static inline char
nxt_java_lowcase(char c)
{
    return (c >= 'A' && c <= 'Z') ? c | 0x20 : c;
}


/*
 * Decodes UTF-8 to UTF-16.  Returns the number of UTF-16 units, or -1 if
 * the bytes are not valid UTF-8.  An overlong form, such as the 0xC0 0x80
 * of the modified UTF-8 of JNI, and an encoded surrogate are not valid.
 */

static int32_t
nxt_java_utf8_decode(jchar *dst, const unsigned char *p, uint32_t len)
{
    jchar                *d;
    uint32_t             u, min, n;
    const unsigned char  *end;

    d = dst;
    end = p + len;

    while (p < end) {
        u = *p++;

        if (u < 0x80) {
            *d++ = (jchar) u;
            continue;
        }

        if (u >= 0xC2 && u <= 0xDF) {
            n = 1;
            min = 0x80;
            u &= 0x1F;

        } else if (u >= 0xE0 && u <= 0xEF) {
            n = 2;
            min = 0x800;
            u &= 0x0F;

        } else if (u >= 0xF0 && u <= 0xF4) {
            n = 3;
            min = 0x10000;
            u &= 0x07;

        } else {
            return -1;
        }

        if ((uint32_t) (end - p) < n) {
            return -1;
        }

        while (n-- != 0) {
            if ((*p & 0xC0) != 0x80) {
                return -1;
            }

            u = (u << 6) | (*p++ & 0x3F);
        }

        if (u < min || u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) {
            return -1;
        }

        if (u >= 0x10000) {
            u -= 0x10000;
            *d++ = (jchar) (0xD800 | (u >> 10));
            *d++ = (jchar) (0xDC00 | (u & 0x3FF));

        } else {
            *d++ = (jchar) u;
        }
    }

    return (int32_t) (d - dst);
}


/*
 * Makes a String of "len" bytes.  The bytes need no NUL at the end and may
 * hold NUL.  If all of them are valid UTF-8, they are decoded as UTF-8.
 * Else all of them are decoded as ISO-8859-1, the encoding that the Servlet
 * API uses for headers, so each byte gives one char and no byte is lost.
 */

jstring
nxt_java_newString(JNIEnv *env, const char *str, uint32_t len)
{
    jchar     buf[256], *chars;
    int32_t   n;
    jstring   res;
    uint32_t  i;

    /* A byte gives at most one UTF-16 unit. */

    if (len <= sizeof(buf) / sizeof(buf[0])) {
        chars = buf;

    } else {
        chars = malloc(len * sizeof(jchar));
        if (chars == NULL) {
            return NULL;
        }
    }

    n = nxt_java_utf8_decode(chars, (const unsigned char *) str, len);

    if (n < 0) {
        for (i = 0; i < len; i++) {
            chars[i] = (unsigned char) str[i];
        }

        n = len;
    }

    res = (*env)->NewString(env, chars, n);

    if (chars != buf) {
        free(chars);
    }

    return res;
}
