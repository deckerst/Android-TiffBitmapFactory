/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <stdlib.h>
#include <stdio.h>
#include <tiffio.h>
#include "fcntl.h"
#include "unistd.h"
#include <ctime>
#include "NativeExceptions.h"
/* Header for class org_beyka_tiffbitmapfactory_TiffSaver */

#ifndef _Included_org_beyka_tiffbitmapfactory_TiffSaver
#define _Included_org_beyka_tiffbitmapfactory_TiffSaver
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     org_beyka_tiffbitmapfactory_TiffSaver
 * Method:    save
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT jboolean JNICALL Java_org_beyka_tiffbitmapfactory_TiffSaver_save
  (JNIEnv *, jclass, jstring, jint, jobject, jobject, jboolean);

unsigned char *convertArgbToBilevel(uint32 *, jint, jint);

char *getCreationDate();

char *concat(const char *, const char *);

#ifdef __cplusplus
}
#endif
#endif
