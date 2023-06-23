using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

#include "NativeTiffBitmapFactory.h"

JNIEXPORT jobject
JNICALL Java_org_beyka_tiffbitmapfactory_TiffBitmapFactory_nativeDecodeFD
        (JNIEnv *env, jclass clazz, jint fd, jobject options, jobject listener) {

    auto *decoder = new NativeDecoder(env, clazz, fd, options, listener);
    jobject java_bitmap = decoder->getBitmap();
    delete(decoder);

    return java_bitmap;
}

#ifdef __cplusplus
}
#endif
