#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <stdlib.h>
#include <stdio.h>
#include <tiffio.h>
#include <unistd.h>
#include "NativeExceptions.h"
#include "NativeDecoder.h"

#ifndef _Included_org_beyka_tiffbitmapfactory_TiffBitmapFactory
#define _Included_org_beyka_tiffbitmapfactory_TiffBitmapFactory
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     com_example_beyka_tiffexample_TiffBitmapFactory
 * Method:    nativeDecodePath
 * Signature: (Ljava/lang/String;I)Landroid/graphics/Bitmap;
 */
JNIEXPORT jobject JNICALL Java_org_beyka_tiffbitmapfactory_TiffBitmapFactory_nativeDecodeFD
  (JNIEnv *, jclass, jint, jobject, jobject);

/*
 * Class:     com_example_beyka_tiffexample_TiffBitmapFactory
 * Method:    nativeCloseFd
 */
JNIEXPORT jobject JNICALL Java_org_beyka_tiffbitmapfactory_TiffBitmapFactory_nativeCloseFd
  (JNIEnv *, jclass, jint);

#ifdef __cplusplus
}
#endif
#endif
