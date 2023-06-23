//
// Created by alexeyba on 09.11.15.
//

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TIFFEXAMPLE_NATIVEEXCEPTIONS_H
#define TIFFEXAMPLE_NATIVEEXCEPTIONS_H

#include <jni.h>

void throw_not_enough_memory_exception(JNIEnv *, int, int);
void throw_decode_file_exception(JNIEnv *, jstring, jstring);
void throw_decode_file_exception_fd(JNIEnv *, jint, jstring);
void throw_cant_open_file_exception(JNIEnv *, jstring);
void throw_cant_open_file_exception_fd(JNIEnv *, jint);

#endif //TIFFEXAMPLE_NATIVEEXCEPTIONS_H

#ifdef __cplusplus
}
#endif



