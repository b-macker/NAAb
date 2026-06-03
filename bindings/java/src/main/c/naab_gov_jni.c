/*
 * JNI bridge for NAAb governance engine.
 *
 * Maps Java native methods to libnaab-governance C API functions.
 * Build: javac GovernanceEngine.java && javah org.naab.governance.GovernanceEngine
 *        cc -shared -o libnaab-governance-jni.so naab_gov_jni.c -I$JAVA_HOME/include \
 *           -I$JAVA_HOME/include/linux -lnaab-governance
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include "naab_governance.h"

/* Helper: convert jlong to engine handle */
static inline naab_gov_engine_t to_engine(jlong handle) {
    return (naab_gov_engine_t)(uintptr_t)handle;
}

JNIEXPORT jlong JNICALL
Java_org_naab_governance_GovernanceEngine_nativeCreate(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    return (jlong)(uintptr_t)naab_gov_create();
}

JNIEXPORT void JNICALL
Java_org_naab_governance_GovernanceEngine_nativeDestroy(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    naab_gov_destroy(to_engine(handle));
}

JNIEXPORT jint JNICALL
Java_org_naab_governance_GovernanceEngine_nativeLoadConfig(JNIEnv *env, jclass cls,
                                                           jlong handle, jstring path) {
    (void)cls;
    if (!path) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/NullPointerException"),
            "path must not be null");
        return -1;
    }
    const char *c_path = (*env)->GetStringUTFChars(env, path, NULL);
    if (!c_path) return -1;  /* H1 fix: OOM, exception already pending */
    jint rc = (jint)naab_gov_load_config(to_engine(handle), c_path);
    (*env)->ReleaseStringUTFChars(env, path, c_path);
    return rc;
}

JNIEXPORT jint JNICALL
Java_org_naab_governance_GovernanceEngine_nativeLoadConfigString(JNIEnv *env, jclass cls,
                                                                 jlong handle, jstring json) {
    (void)cls;
    if (!json) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/NullPointerException"),
            "json must not be null");
        return -1;
    }
    const char *c_json = (*env)->GetStringUTFChars(env, json, NULL);
    if (!c_json) return -1;  /* H1 fix: OOM */
    jint rc = (jint)naab_gov_load_config_string(to_engine(handle), c_json);
    (*env)->ReleaseStringUTFChars(env, json, c_json);
    return rc;
}

JNIEXPORT jboolean JNICALL
Java_org_naab_governance_GovernanceEngine_nativeIsActive(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    return naab_gov_is_active(to_engine(handle)) == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_naab_governance_GovernanceEngine_nativeScan(JNIEnv *env, jclass cls,
                                                     jlong handle, jstring language,
                                                     jstring code, jstring sourceFile,
                                                     jint startLine) {
    (void)cls;
    /* Null jstring checks — GetStringUTFChars(env, NULL, NULL) is UB */
    if (!language) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/NullPointerException"),
            "language must not be null");
        return NULL;
    }
    if (!code) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/NullPointerException"),
            "code must not be null");
        return NULL;
    }
    if (!sourceFile) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/NullPointerException"),
            "sourceFile must not be null");
        return NULL;
    }
    /* H1 fix: cascading NULL checks with cleanup on OOM */
    const char *c_lang = (*env)->GetStringUTFChars(env, language, NULL);
    if (!c_lang) return NULL;
    const char *c_code = (*env)->GetStringUTFChars(env, code, NULL);
    if (!c_code) {
        (*env)->ReleaseStringUTFChars(env, language, c_lang);
        return NULL;
    }
    const char *c_file = (*env)->GetStringUTFChars(env, sourceFile, NULL);
    if (!c_file) {
        (*env)->ReleaseStringUTFChars(env, language, c_lang);
        (*env)->ReleaseStringUTFChars(env, code, c_code);
        return NULL;
    }

    char *result = naab_gov_scan(to_engine(handle), c_lang, c_code, c_file, (int)startLine);

    (*env)->ReleaseStringUTFChars(env, language, c_lang);
    (*env)->ReleaseStringUTFChars(env, code, c_code);
    (*env)->ReleaseStringUTFChars(env, sourceFile, c_file);

    if (result == NULL) {
        const char *err = naab_gov_last_error(to_engine(handle));
        if (!(*env)->ExceptionCheck(env)) {
            (*env)->ThrowNew(env,
                (*env)->FindClass(env, "java/lang/RuntimeException"),
                err ? err : "Governance scan failed (null result)");
        }
        return NULL;
    }
    jstring jresult = (*env)->NewStringUTF(env, result);
    naab_gov_free_string(result);
    return jresult;
}

JNIEXPORT jboolean JNICALL
Java_org_naab_governance_GovernanceEngine_nativeWasBlocked(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    return naab_gov_was_blocked(to_engine(handle)) == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_naab_governance_GovernanceEngine_nativeReset(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    naab_gov_reset(to_engine(handle));
}

JNIEXPORT jint JNICALL
Java_org_naab_governance_GovernanceEngine_nativeResultCount(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    return (jint)naab_gov_result_count(to_engine(handle));
}

JNIEXPORT jstring JNICALL
Java_org_naab_governance_GovernanceEngine_nativeLastError(JNIEnv *env, jclass cls, jlong handle) {
    (void)cls;
    const char *err = naab_gov_last_error(to_engine(handle));
    return (*env)->NewStringUTF(env, err ? err : "");
}

JNIEXPORT jstring JNICALL
Java_org_naab_governance_GovernanceEngine_nativeVersion(JNIEnv *env, jclass cls) {
    (void)cls;
    return (*env)->NewStringUTF(env, naab_gov_version_string());
}
