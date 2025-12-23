//
// Created by beyka on 3.2.17.
//

#ifndef TIFFSAMPLE_NATIVEDECODER_H
#define TIFFSAMPLE_NATIVEDECODER_H

#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <cstdlib>
#include <cstdio>
#include <tiffio.h>
#include <csignal>
#include <csetjmp>
#include "NativeExceptions.h"

class NativeDecoder {
public:
    explicit NativeDecoder(JNIEnv *, jclass, jint, jobject, jobject);

    ~NativeDecoder();

    jobject getBitmap();

private:
    //constants
    static uint const colorMask = 0xFF;
    static int const ARGB_8888 = 2;
    static int const RGB_565 = 4;
    static int const ALPHA_8 = 8;

    static int const DECODE_METHOD_IMAGE = 1;
    static int const DECODE_METHOD_TILE = 2;
    static int const DECODE_METHOD_STRIP = 3;

    static int const DECODE_MODE_FILE_PATH = 1;
    static int const DECODE_MODE_FILE_DESCRIPTOR = 2;

    //decoding mode
    int decodingMode;

    //fields
    JNIEnv *env;

    static jmp_buf tile_buf;
    static jmp_buf strip_buf;
    static jmp_buf image_buf;
    static jmp_buf general_buf;

    jobject optionsObject;
    jobject listenerObject;
    jclass jIProgressListenerClass;
    jclass jBitmapOptionsClass;
    jclass jThreadClass = nullptr;
    jint jFd;
    jstring jPath;
    jboolean throwException;
    jboolean useOrientationTag;
    TIFF *image;
    jlong progressTotal;
    int origwidth;
    int origheight;
    short origorientation;
    int origcompressionscheme;
    jobject preferedConfig;
    jboolean invertRedAndBlue;
    jint boundX;
    jint boundY;
    jint boundWidth;
    jint boundHeight;
    char hasBounds;
    unsigned long availableMemory;

    //methods
    int getDirectoryCount();

    void writeDataToOptions(int);

    jobject createBitmap(int, int);

    jint *getSampledRasterFromImage(int, int *, int *);

    jint *getSampledRasterFromImageWithBounds(int, int *, int *);

    jint applyFilterForImage(int x, int y, const unsigned int *raster) const;

    jint *getSampledRasterFromStrip(int, int *, int *);

    jint *getSampledRasterFromStripWithBounds(int, int *, int *);

    bool needStripVerticalFlip();

    jint applyFilterForStrip(int x, int y, const uint32 *raster, const unsigned int *matrixTopLine, const unsigned int *matrixBottomLine, int rowPerStrip, int globalLineCounter, int isSecondRasterExist) const;

    jint *getSampledRasterFromTile(int, int *, int *);

    jint *getSampledRasterFromTileWithBounds(int, int *, int *);

    jint applyFilterForTile(int x, int y, const uint32 *rasterTile, const uint32 *rasterTileLeft, const uint32 *rasterTileRight, uint32 tileWidth, uint32 tileHeight, short leftTileExists, short rightTileExists) const;

    int getDecodeMethod();

    void rotateTileLinesVertical(uint32, uint32, uint32 *, uint32 *);

    void rotateTileLinesHorizontal(uint32, uint32, uint32 *, uint32 *);

    void flipPixelsVertical(uint32, uint32, jint *);

    void flipPixelsVerticalWithBuffer(uint32, uint32, uint32 *, uint32 *);

    void flipPixelsHorizontal(uint32, uint32, jint *);

    void fixOrientation(jint *, uint32, int, int);

    void rotateRaster(jint *, int, int *, int *);

    jbyte *createBitmapAlpha8(jint *, int, int);

    unsigned short *createBitmapRGB565(jint *, int, int);

    jstring charsToJString(const char *);

    jboolean checkStop();

    void sendProgress(jlong, jlong);

    //throwing exceptions
    void throwDecodeFileException(const char *);

    void throwCantOpenFileException();

    static void tileErrorHandler(int code, siginfo_t *siginfo, void *sc);

    static void stripErrorHandler(int code, siginfo_t *siginfo, void *sc);

    static void imageErrorHandler(int code, siginfo_t *siginfo, void *sc);

    static void generalErrorHandler(int code, siginfo_t *siginfo, void *sc);
};

#endif //TIFFSAMPLE_NATIVEDECODER_H
