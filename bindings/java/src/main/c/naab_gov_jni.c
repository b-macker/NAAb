/*
 * JNI bridge for NAAb governance engine.
 *
 * Maps Java native methods to libnaab-governance C API functions.
 * Build: javac GovernanceEngine.java && javah org.naab.governance.GovernanceEngine
 *        cc -shared -o libnaab-governance-jni.so naab_gov_jni.c -I$JAVA_HOME/include \
 *           -I$JAVA_HOME/include/linux -lnaab-governance
 */

#include <jni.h>
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
    const char *c_path = (*env)->GetStringUTFChars(env, path, NULL);
    jint rc = (jint)naab_gov_load_config(to_engine(handle), c_path);
    (*env)->ReleaseStringUTFChars(env, path, c_path);
    return rc;
}

JNIEXPORT jint JNICALL
Java_org_naab_governance_GovernanceEngine_nativeLoadConfigString(JNIEnv *env, jclass cls,
                                                                 jlong handle, jstring json) {
    (void)cls;
    const char *c_json = (*env)->GetStringUTFChars(env, json, NULL);
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
    const char *c_lang = (*env)->GetStringUTFChars(env, language, NULL);
    const char *c_code = (*env)->GetStringUTFChars(env, code, NULL);
    const char *c_file = (*env)->GetStringUTFChars(env, sourceFile, NULL);

    char *result = naab_gov_scan(to_engine(handle), c_lang, c_code, c_file, (int)startLine);

    (*env)->ReleaseStringUTFChars(env, language, c_lang);
    (*env)->ReleaseStringUTFChars(env, code, c_code);
    (*env)->ReleaseStringUTFChars(env, sourceFile, c_file);

    if (result == NULL) {
        const char *err = naab_gov_last_error(to_engine(handle));
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/RuntimeException"),
            err ? err : "Governance scan failed (null result)");
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
