//
// Created by beyka on 3.2.17.
//

#include "NativeDecoder.h"
#include <string>

jmp_buf NativeDecoder::tile_buf;
jmp_buf NativeDecoder::strip_buf;
jmp_buf NativeDecoder::image_buf;
jmp_buf NativeDecoder::general_buf;

//Constructor for decoding from file descriptor
NativeDecoder::NativeDecoder(JNIEnv *e, jclass c, jint fd, jobject opts, jobject listener) {
    decodingMode = DECODE_MODE_FILE_DESCRIPTOR;

    availableMemory = 8000 * 8000 * 4; // use 244Mb restriction for decoding full image
    env = e;
    optionsObject = opts;
    listenerObject = listener;
    jFd = fd;

    origwidth = 0;
    origheight = 0;
    origorientation = 0;
    origcompressionscheme = 0;
    progressTotal = 0;
    invertRedAndBlue = false;

    boundX = boundY = boundWidth = boundHeight = -1;
    hasBounds = 0;

    preferedConfig = nullptr;
    image = nullptr;

    jBitmapOptionsClass = env->FindClass("org/beyka/tiffbitmapfactory/TiffBitmapFactory$Options");
    jIProgressListenerClass = env->FindClass("org/beyka/tiffbitmapfactory/IProgressListener");
    jThreadClass = env->FindClass("java/lang/Thread");
}

NativeDecoder::~NativeDecoder() {
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Destructor");
    if (image) {
        TIFFClose(image);
        image = nullptr;
    }

    //Release global reference for Bitmap.Config
    if (preferedConfig) {
        env->DeleteGlobalRef(preferedConfig);
        preferedConfig = nullptr;
    }

    if (jBitmapOptionsClass) {
        env->DeleteLocalRef(jBitmapOptionsClass);
        jBitmapOptionsClass = nullptr;
    }

    if (jIProgressListenerClass) {
        env->DeleteLocalRef(jIProgressListenerClass);
        jIProgressListenerClass = nullptr;
    }

    if (jThreadClass) {
        env->DeleteLocalRef(jThreadClass);
        jThreadClass = nullptr;
    }
}

jobject NativeDecoder::getBitmap() {
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "getBitmap");

    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act{};
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = generalErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    //check for error
    if (setjmp(NativeDecoder::general_buf)) {
        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }
        return nullptr;
    }

    //Get options from TiffBitmapFactory$Options
    jfieldID gOptions_ThrowExceptionFieldID = env->GetFieldID(jBitmapOptionsClass, "inThrowException", "Z");
    throwException = env->GetBooleanField(optionsObject, gOptions_ThrowExceptionFieldID);

    jfieldID gOptions_UseOrientationTagFieldID = env->GetFieldID(jBitmapOptionsClass, "inUseOrientationTag", "Z");
    useOrientationTag = env->GetBooleanField(optionsObject, gOptions_UseOrientationTagFieldID);

    jfieldID gOptions_sampleSizeFieldID = env->GetFieldID(jBitmapOptionsClass, "inSampleSize", "I");
    jint inSampleSize = env->GetIntField(optionsObject, gOptions_sampleSizeFieldID);
    if (inSampleSize != 1 && inSampleSize % 2 != 0) {
        const char *message = "inSampleSize should be power of 2\0";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
        if (throwException) {
            throwDecodeFileException(message);
        }
        return nullptr;
    }

    jfieldID gOptions_justDecodeBoundsFieldID = env->GetFieldID(jBitmapOptionsClass, "inJustDecodeBounds", "Z");
    jboolean inJustDecodeBounds = env->GetBooleanField(optionsObject, gOptions_justDecodeBoundsFieldID);

    jfieldID gOptions_invertRedAndBlueFieldID = env->GetFieldID(jBitmapOptionsClass, "inSwapRedBlueColors", "Z");
    invertRedAndBlue = env->GetBooleanField(optionsObject, gOptions_invertRedAndBlueFieldID);

    jfieldID gOptions_DirectoryCountFieldID = env->GetFieldID(jBitmapOptionsClass, "inDirectoryNumber", "I");
    jint inDirectoryNumber = env->GetIntField(optionsObject, gOptions_DirectoryCountFieldID);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "param directoryCount", inDirectoryNumber);

    jfieldID gOptions_AvailableMemoryFieldID = env->GetFieldID(jBitmapOptionsClass, "inAvailableMemory", "J");
    unsigned long inAvailableMemory = env->GetLongField(optionsObject, gOptions_AvailableMemoryFieldID);

    jfieldID gOptions_PreferedConfigFieldID = env->GetFieldID(jBitmapOptionsClass, "inPreferredConfig", "Lorg/beyka/tiffbitmapfactory/TiffBitmapFactory$ImageConfig;");
    jobject config = env->GetObjectField(optionsObject, gOptions_PreferedConfigFieldID);

    if (inAvailableMemory > 0) {
        availableMemory = inAvailableMemory;
    }

    if (config == nullptr) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "config is nullptr, creating default options");
        jclass bitmapConfig = env->FindClass("org/beyka/tiffbitmapfactory/TiffBitmapFactory$ImageConfig");
        jfieldID argb8888FieldID = env->GetStaticFieldID(bitmapConfig, "ARGB_8888", "Lorg/beyka/tiffbitmapfactory/TiffBitmapFactory$ImageConfig;");
        config = env->GetStaticObjectField(bitmapConfig, argb8888FieldID);
        env->DeleteLocalRef(bitmapConfig);
    }
    preferedConfig = env->NewGlobalRef(config);
    env->DeleteLocalRef(config);

    jfieldID gOptions_DecodeAreaFieldId = env->GetFieldID(jBitmapOptionsClass, "inDecodeArea", "Lorg/beyka/tiffbitmapfactory/DecodeArea;");
    jobject decodeArea = env->GetObjectField(optionsObject, gOptions_DecodeAreaFieldId);

    //if directory number < 0 set it to 0
    if (inDirectoryNumber < 0) inDirectoryNumber = 0;

    //Open tiff file
    const char *strPath = nullptr;
    if (decodingMode == DECODE_MODE_FILE_DESCRIPTOR) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "nativeTiffOpen", jFd);
        image = TIFFFdOpen(jFd, "", "r");
    } else if (decodingMode == DECODE_MODE_FILE_PATH) {
        strPath = env->GetStringUTFChars(jPath, nullptr);
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %s", "nativeTiffOpen", strPath);
        image = TIFFOpen(strPath, "r");
    }

    if (image == nullptr) {
        if (throwException) {
            throwCantOpenFileException();
        }

        if (decodingMode == DECODE_MODE_FILE_PATH) {
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t open bitmap path=%s", strPath);
            env->ReleaseStringUTFChars(jPath, strPath);
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t open file descriptor fd=%d", jFd);
        }
        return nullptr;
    } else {
        if (decodingMode == DECODE_MODE_FILE_PATH) {
            env->ReleaseStringUTFChars(jPath, strPath);
        }
    }
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Tiff is open");

    TIFFSetDirectory(image, inDirectoryNumber);
    TIFFGetField(image, TIFFTAG_IMAGEWIDTH, &origwidth);
    TIFFGetField(image, TIFFTAG_IMAGELENGTH, &origheight);

    //Read decode bounds if exists
    if (decodeArea) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Decode bounds present");
        jclass decodeAreaClass = env->FindClass("org/beyka/tiffbitmapfactory/DecodeArea");
        jfieldID xFieldID = env->GetFieldID(decodeAreaClass, "x", "I");
        jfieldID yFieldID = env->GetFieldID(decodeAreaClass, "y", "I");
        jfieldID widthFieldID = env->GetFieldID(decodeAreaClass, "width", "I");
        jfieldID heightFieldID = env->GetFieldID(decodeAreaClass, "height", "I");

        boundX = env->GetIntField(decodeArea, xFieldID);
        boundY = env->GetIntField(decodeArea, yFieldID);
        boundWidth = env->GetIntField(decodeArea, widthFieldID);
        boundHeight = env->GetIntField(decodeArea, heightFieldID);
        if (boundX >= origwidth - 1) {
            const char *message = "X of left top corner of decode area should be less than image width";
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
            if (throwException) {
                throwDecodeFileException(message);
            }
            env->DeleteLocalRef(decodeAreaClass);
            return nullptr;
        }
        if (boundY >= origheight - 1) {
            const char *message = "Y of left top corner of decode area should be less than image height";
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
            if (throwException) {
                throwDecodeFileException(message);
            }
            env->DeleteLocalRef(decodeAreaClass);
            return nullptr;
        }

        if (boundX < 0) boundX = 0;
        if (boundY < 0) boundY = 0;
        if (boundX + boundWidth >= origwidth) boundWidth = origwidth - boundX - 1;
        if (boundY + boundHeight >= origheight) boundHeight = origheight - boundY - 1;

        if (boundWidth < 1) {
            const char *message = "Width of decode area can\'t be less than 1";
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
            if (throwException) {
                throwDecodeFileException(message);
            }
            env->DeleteLocalRef(decodeAreaClass);
            return nullptr;
        }
        if (boundHeight < 1) {
            const char *message = "Height of decode area can\'t be less than 1";
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
            if (throwException) {
                throwDecodeFileException(message);
            }
            env->DeleteLocalRef(decodeAreaClass);
            return nullptr;
        }

        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "Decode X", boundX);
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "Decode Y", boundY);
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "Decode width", boundWidth);
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "Decode height", boundHeight);

        hasBounds = 1;
        env->DeleteLocalRef(decodeAreaClass);
        env->DeleteLocalRef(decodeArea);
    }

    jobject java_bitmap = nullptr;

    writeDataToOptions(inDirectoryNumber);

    if (!inJustDecodeBounds) {
        progressTotal = origwidth * origheight;
        sendProgress(0, progressTotal);
        java_bitmap = createBitmap(inSampleSize, inDirectoryNumber);
    }

    return java_bitmap;
}

jobject NativeDecoder::createBitmap(int inSampleSize, int directoryNumber) {
//Read Config from options. Use ordinal field from ImageConfig class
    jint configInt = ARGB_8888;
    if (preferedConfig) {
        jclass configClass = env->FindClass("org/beyka/tiffbitmapfactory/TiffBitmapFactory$ImageConfig");
        jfieldID ordinalFieldID = env->GetFieldID(configClass, "ordinal", "I");
        configInt = env->GetIntField(preferedConfig, ordinalFieldID);
        env->DeleteLocalRef(configClass);
    }

    int bitdepth = 1;
    TIFFGetField(image, TIFFTAG_BITSPERSAMPLE, &bitdepth);
    if (bitdepth != 1 && bitdepth != 4 && bitdepth != 8 && bitdepth != 16) {
        const char *err = "Only 1, 4, 8 and 16 bits per sample are supported";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }
        return nullptr;
    }

    int newBitmapWidth = 0;
    int newBitmapHeight = 0;

    jint *raster = nullptr;

    if (hasBounds) {
        switch (getDecodeMethod()) {
            case DECODE_METHOD_IMAGE:
                raster = getSampledRasterFromImageWithBounds(inSampleSize, &newBitmapWidth, &newBitmapHeight);
                break;
            case DECODE_METHOD_TILE:
                raster = getSampledRasterFromTileWithBounds(inSampleSize, &newBitmapWidth, &newBitmapHeight);
                break;
            case DECODE_METHOD_STRIP:
                raster = getSampledRasterFromStripWithBounds(inSampleSize, &newBitmapWidth, &newBitmapHeight);
                break;
        }
    } else {
        switch (getDecodeMethod()) {
            case DECODE_METHOD_IMAGE:
                raster = getSampledRasterFromImage(inSampleSize, &newBitmapWidth, &newBitmapHeight);
                break;
            case DECODE_METHOD_TILE:
                raster = getSampledRasterFromTile(inSampleSize, &newBitmapWidth, &newBitmapHeight);
                break;
            case DECODE_METHOD_STRIP:
                raster = getSampledRasterFromStrip(inSampleSize, &newBitmapWidth, &newBitmapHeight);
                break;
        }
    }

    if (raster == nullptr) {
        return nullptr;
    }

    // Convert ABGR to ARGB
    if (invertRedAndBlue) {
        int i = 0;
        int j = 0;
        int tmp = 0;
        for (i = 0; i < newBitmapHeight; i++) {
            for (j = 0; j < newBitmapWidth; j++) {
                tmp = raster[j + newBitmapWidth * i];
                raster[j + newBitmapWidth * i] = (tmp & 0xff000000) | ((tmp & 0x00ff0000) >> 16) | (tmp & 0x0000ff00) | ((tmp & 0xff) << 16);
            }
        }
    }

    sendProgress(progressTotal, progressTotal);

    if (checkStop()) {
        if (raster) {
            free(raster);
        }
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
        return nullptr;
    }

    //Class and field for Bitmap.Config
    jclass bitmapConfigClass = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID bitmapConfigField = nullptr;
    void *processedBuffer = nullptr;
    if (configInt == ARGB_8888) {
        processedBuffer = raster;
        bitmapConfigField = env->GetStaticFieldID(bitmapConfigClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    } else if (configInt == ALPHA_8) {
        processedBuffer = createBitmapAlpha8(raster, newBitmapWidth, newBitmapHeight);
        bitmapConfigField = env->GetStaticFieldID(bitmapConfigClass, "ALPHA_8", "Landroid/graphics/Bitmap$Config;");
    } else if (configInt == RGB_565) {
        processedBuffer = createBitmapRGB565(raster, newBitmapWidth, newBitmapHeight);
        bitmapConfigField = env->GetStaticFieldID(bitmapConfigClass, "RGB_565", "Landroid/graphics/Bitmap$Config;");
    }

    if (processedBuffer == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Error while decoding image");
        return nullptr;
    }

    //Create mutable bitmap
    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jmethodID methodid = env->GetStaticMethodID(bitmapClass, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");

    //BitmapConfig
    jobject config = env->GetStaticObjectField(bitmapConfigClass, bitmapConfigField);

    env->DeleteLocalRef(bitmapConfigClass);

    jobject java_bitmap = nullptr;

    if (checkStop()) {
        env->DeleteLocalRef(config);
        env->DeleteLocalRef(bitmapClass);
        if (processedBuffer) {
            free(processedBuffer);
        }
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
        return nullptr;
    }

    if (!useOrientationTag) {
        java_bitmap = env->CallStaticObjectMethod(bitmapClass, methodid, newBitmapWidth, newBitmapHeight, config);
    } else if (origorientation > 4) {
        java_bitmap = env->CallStaticObjectMethod(bitmapClass, methodid, newBitmapHeight, newBitmapWidth, config);
    } else {
        java_bitmap = env->CallStaticObjectMethod(bitmapClass, methodid, newBitmapWidth, newBitmapHeight, config);
    }

    //remove not used references
    env->DeleteLocalRef(config);
    env->DeleteLocalRef(bitmapClass);

    //Copy data to bitmap
    int ret;
    void *bitmapPixels;
    if ((ret = AndroidBitmap_lockPixels(env, java_bitmap, &bitmapPixels)) < 0) {
        //error
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Lock pixels failed");
        return nullptr;
    }
    int pixelsCount = newBitmapWidth * newBitmapHeight;

    if (configInt == ARGB_8888) {
        memcpy(bitmapPixels, (jint *) processedBuffer, sizeof(jint) * pixelsCount);
    } else if (configInt == ALPHA_8) {
        memcpy(bitmapPixels, (jbyte *) processedBuffer, sizeof(jbyte) * pixelsCount);
    } else if (configInt == RGB_565) {
        memcpy(bitmapPixels, (unsigned short *) processedBuffer, sizeof(unsigned short) * pixelsCount);
    }

    AndroidBitmap_unlockPixels(env, java_bitmap);

    //remove array
    free(processedBuffer);

    return java_bitmap;
}

jint *NativeDecoder::getSampledRasterFromStrip(int inSampleSize, int *bitmapWidth, int *bitmapHeight) {
    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = stripErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "width", origwidth);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "height", origheight);

    jint *pixels = nullptr;
    *bitmapWidth = origwidth / inSampleSize;
    *bitmapHeight = origheight / inSampleSize;
    uint32 pixelsBufferSize = *bitmapWidth * *bitmapHeight;
    int origImageBufferSize = origwidth * origheight;

    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "new width", *bitmapWidth);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "new height", *bitmapHeight);

    uint32 stripSize = TIFFStripSize(image);
    uint32 stripMax = TIFFNumberOfStrips(image);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "strip size ", stripSize);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "stripMax  ", stripMax);
    int rowPerStrip = -1;
    TIFFGetField(image, TIFFTAG_ROWSPERSTRIP, &rowPerStrip);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "rowsperstrip", rowPerStrip);

    unsigned long estimateMem = 0;
    estimateMem += (sizeof(jint) * pixelsBufferSize); //buffer for decoded pixels
    estimateMem += (origwidth * sizeof(uint32)); //work line for rotate strip
    estimateMem += (origwidth * rowPerStrip * sizeof(uint32) * 2); //current and next strips
    estimateMem += (sizeof(jint) * origwidth * 2); //bottom and top lines for reading pixel(matrixBottomLine, matrixTopLine)
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);
    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    pixels = (jint *) malloc(sizeof(jint) * pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    }

    auto *work_line_buf = (uint32 *) _TIFFmalloc(origwidth * sizeof(uint32));

    uint32 *raster;
    uint32 *rasterForBottomLine; // in this raster copy next strip for getting bottom line in matrix color selection
    if (rowPerStrip == -1 && stripMax == 1) {
        raster = (uint32 *) _TIFFmalloc(origImageBufferSize * sizeof(uint32));
        rasterForBottomLine = (uint32 *) _TIFFmalloc(origImageBufferSize * sizeof(uint32));
    } else {
        raster = (uint32 *) _TIFFmalloc(origwidth * rowPerStrip * sizeof(uint32));
        rasterForBottomLine = (uint32 *) _TIFFmalloc(origwidth * rowPerStrip * sizeof(uint32));
    }
    if (rowPerStrip == -1) {
        rowPerStrip = origheight;
    }

    int targetY = 0;
    int nextStripOffset = 0;
    int globalLineCounter = 0;

    auto *matrixTopLine = (uint32 *) malloc(sizeof(jint) * origwidth);
    auto *matrixBottomLine = (uint32 *) malloc(sizeof(jint) * origwidth);

    int isSecondRasterExist = 0;
    int ok = 1;
    uint32 rows_to_write = 0;

    //check for error
    if (setjmp(NativeDecoder::strip_buf)) {
        if (raster) {
            _TIFFfree(raster);
            raster = nullptr;
        }
        if (rasterForBottomLine) {
            _TIFFfree(rasterForBottomLine);
            rasterForBottomLine = nullptr;
        }
        if (matrixTopLine) {
            _TIFFfree(matrixTopLine);
            matrixTopLine = nullptr;
        }
        if (matrixBottomLine) {
            _TIFFfree(matrixBottomLine);
            matrixBottomLine = nullptr;
        }

        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }

        return nullptr;
    }

    for (int i = 0; i < stripMax * rowPerStrip; i += rowPerStrip) {
        sendProgress(i * origwidth, progressTotal);

        //if second raster is exist - copy it to work raster end decode next strip
        if (isSecondRasterExist) {
            _TIFFmemcpy(raster, rasterForBottomLine, origwidth * rowPerStrip * sizeof(uint32));

            //If next strip is exist - decode it, invert lines
            if (i + rowPerStrip < stripMax * rowPerStrip) {
                TIFFReadRGBAStrip(image, i + rowPerStrip, rasterForBottomLine);
                isSecondRasterExist = 1;

                rows_to_write = 0;
                if (i + rowPerStrip * 2 > origheight) {
                    rows_to_write = origheight - i - rowPerStrip;
                } else {
                    rows_to_write = rowPerStrip;
                }

                if (origorientation <= 4) {
                    for (int line = 0; line < rows_to_write / 2; line++) {
                        unsigned int *top_line, *bottom_line;

                        top_line = rasterForBottomLine + origwidth * line;
                        bottom_line = rasterForBottomLine + origwidth * (rows_to_write - line - 1);

                        _TIFFmemcpy(work_line_buf, top_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(bottom_line, work_line_buf, sizeof(unsigned int) * origwidth);
                    }
                }
            } else {
                isSecondRasterExist = 0;
            }
        } else {
            //if second raster is not exist - first processing - read first and second raster
            TIFFReadRGBAStrip(image, i, raster);
            //invert lines, because libtiff origin is bottom left instead of top left
            rows_to_write = 0;
            if (i + rowPerStrip > origheight) {
                rows_to_write = origheight - i;
            } else {
                rows_to_write = rowPerStrip;
            }

            if (origorientation <= 4) {
                for (int line = 0; line < rows_to_write / 2; line++) {
                    unsigned int *top_line, *bottom_line;

                    top_line = raster + origwidth * line;
                    bottom_line = raster + origwidth * (rows_to_write - line - 1);

                    _TIFFmemcpy(work_line_buf, top_line, sizeof(unsigned int) * origwidth);
                    _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * origwidth);
                    _TIFFmemcpy(bottom_line, work_line_buf, sizeof(unsigned int) * origwidth);
                }
            }

            //if next strip is exist - read it and invert lines
            if (i + rowPerStrip < origheight) {
                TIFFReadRGBAStrip(image, i + rowPerStrip, rasterForBottomLine);
                isSecondRasterExist = 1;

                //invert lines, because libtiff origin is bottom left instead of top left
                rows_to_write = 0;
                if (i + rowPerStrip * 2 > origheight) {
                    rows_to_write = origheight - i - rowPerStrip;
                } else {
                    rows_to_write = rowPerStrip;
                }
                if (origorientation <= 4) {
                    for (int line = 0; line < rows_to_write / 2; line++) {
                        unsigned int *top_line, *bottom_line;

                        top_line = rasterForBottomLine + origwidth * line;
                        bottom_line = rasterForBottomLine + origwidth * (rows_to_write - line - 1);

                        _TIFFmemcpy(work_line_buf, top_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(bottom_line, work_line_buf, sizeof(unsigned int) * origwidth);
                    }
                }
            }
        }

        if (inSampleSize == 1) {
            int byteToCopy = 0;
            if (i + rowPerStrip < origheight) {
                byteToCopy = sizeof(unsigned int) * rowPerStrip * origwidth;
            } else {
                byteToCopy = sizeof(unsigned int) * rows_to_write * origwidth;
            }
            int position = i * origwidth;
            memcpy(&pixels[position], raster, byteToCopy);
            //sendProgress(position, progressTotal);
        } else {
            if (isSecondRasterExist) {
                _TIFFmemcpy(matrixBottomLine, rasterForBottomLine /*+ lineAddrToCopyBottomLine * origwidth*/, sizeof(unsigned int) * origwidth);
            }
            for (int sourceStripY = 0; targetY < *bitmapHeight && sourceStripY < rowPerStrip; sourceStripY++) {
                if (checkStop()) {
                    if (raster) {
                        _TIFFfree(raster);
                        raster = nullptr;
                    }
                    if (rasterForBottomLine) {
                        _TIFFfree(rasterForBottomLine);
                        rasterForBottomLine = nullptr;
                    }
                    if (matrixTopLine) {
                        _TIFFfree(matrixTopLine);
                        matrixTopLine = nullptr;
                    }
                    if (matrixBottomLine) {
                        _TIFFfree(matrixBottomLine);
                        matrixBottomLine = nullptr;
                    }
                    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
                    return nullptr;
                }

                // if total line of source image is equal to inSampleSize*N then process this line
                if (globalLineCounter % inSampleSize == 0) {
                    for (int targetX = 0, sourceStripX = 0; targetX < *bitmapWidth; targetX++, sourceStripX += inSampleSize) {
                        jint crPix = applyFilterForStrip(sourceStripX, sourceStripY, raster, matrixTopLine, matrixBottomLine, rowPerStrip, globalLineCounter, isSecondRasterExist);
                        pixels[targetY * *bitmapWidth + targetX] = crPix;
                    }
                    targetY++;
                }
                //increment global source image line counter
                globalLineCounter++;

                if (sourceStripY == rowPerStrip - 1 && i + rowPerStrip < origheight) {
                    _TIFFmemcpy(matrixTopLine, raster + sourceStripY * origwidth, sizeof(unsigned int) * origwidth);
                }
            }
        }
    }
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Decoding finished. Free memory");

    //Close Buffers
    if (raster) {
        _TIFFfree(raster);
        raster = nullptr;
    }

    if (rasterForBottomLine) {
        _TIFFfree(rasterForBottomLine);
        rasterForBottomLine = nullptr;
    }

    if (matrixTopLine) {
        _TIFFfree(matrixTopLine);
        matrixTopLine = nullptr;
    }

    if (matrixBottomLine) {
        _TIFFfree(matrixBottomLine);
        matrixBottomLine = nullptr;
    }

    if (useOrientationTag) {
        uint32 buf;
        //fixOrientation(pixels, pixelsBufferSize, *bitmapWidth, *bitmapHeight);
        switch (origorientation) {
            case ORIENTATION_TOPLEFT:
            case ORIENTATION_TOPRIGHT:
                break;
            case ORIENTATION_BOTRIGHT:
            case ORIENTATION_BOTLEFT:
                flipPixelsVertical(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_LEFTTOP:
                rotateRaster(pixels, 90, bitmapWidth, bitmapHeight);
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
            case ORIENTATION_RIGHTTOP:
                rotateRaster(pixels, 270, bitmapWidth, bitmapHeight);
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
            case ORIENTATION_RIGHTBOT:
                rotateRaster(pixels, 90, bitmapWidth, bitmapHeight);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
            case ORIENTATION_LEFTBOT:
                rotateRaster(pixels, 270, bitmapWidth, bitmapHeight);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
        }
    } else if (origorientation == ORIENTATION_TOPRIGHT || origorientation == ORIENTATION_BOTRIGHT || origorientation == ORIENTATION_RIGHTTOP || origorientation == ORIENTATION_RIGHTBOT) {
        flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
    }

    return pixels;
}

jint *NativeDecoder::getSampledRasterFromStripWithBounds(int inSampleSize, int *bitmapWidth, int *bitmapHeight) {
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "getSampledRasterFromStripWithBounds");

    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = stripErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "width", origwidth);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "height", origheight);

    jint *pixels = nullptr;
    *bitmapWidth = origwidth / inSampleSize;
    *bitmapHeight = boundHeight / inSampleSize;//origheight / inSampleSize;
    uint32 pixelsBufferSize = *bitmapWidth * *bitmapHeight;
    int origImageBufferSize = origwidth * origheight;

    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "new width", *bitmapWidth);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "new height", *bitmapHeight);

    uint32 stripSize = TIFFStripSize(image);
    uint32 stripMax = TIFFNumberOfStrips(image);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "strip size ", stripSize);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "stripMax  ", stripMax);
    int rowPerStrip = -1;
    TIFFGetField(image, TIFFTAG_ROWSPERSTRIP, &rowPerStrip);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "rowsperstrip", rowPerStrip);

    unsigned long estimateMem = 0;
    estimateMem += (sizeof(jint) * pixelsBufferSize); //temp buffer for decoded pixels
    estimateMem += (sizeof(jint) * (boundWidth / inSampleSize) * (boundHeight / inSampleSize)); //final buffer that will store original image
    estimateMem += (origwidth * sizeof(uint32)); //work line for rotate strip
    estimateMem += (origwidth * rowPerStrip * sizeof(uint32) * 2); //current and next strips
    estimateMem += (sizeof(jint) * origwidth * 2); //bottom and top lines for reading pixel(matrixBottomLine, matrixTopLine)
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);
    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    progressTotal = pixelsBufferSize + (boundWidth / inSampleSize) * (boundHeight / inSampleSize);
    sendProgress(0, progressTotal);
    jlong processedProgress = 0;

    pixels = (jint *) malloc(sizeof(jint) * pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    }

    auto *work_line_buf = (uint32 *) _TIFFmalloc(origwidth * sizeof(uint32));

    uint32 *raster;
    uint32 *rasterForBottomLine; // in this raster copy next strip for getting bottom line in matrix color selection
    if (rowPerStrip == -1 && stripMax == 1) {
        raster = (uint32 *) _TIFFmalloc(origImageBufferSize * sizeof(uint32));
        rasterForBottomLine = (uint32 *) _TIFFmalloc(origImageBufferSize * sizeof(uint32));
    } else {
        raster = (uint32 *) _TIFFmalloc(origwidth * rowPerStrip * sizeof(uint32));
        rasterForBottomLine = (uint32 *) _TIFFmalloc(origwidth * rowPerStrip * sizeof(uint32));
    }
    if (rowPerStrip == -1) {
        rowPerStrip = origheight;
    }

    int targetY = 0;
    int nextStripOffset = 0;
    int globalLineCounter = 0;

    auto *matrixTopLine = (uint32 *) malloc(sizeof(jint) * origwidth);
    auto *matrixBottomLine = (uint32 *) malloc(sizeof(jint) * origwidth);

    int isSecondRasterExist = 0;
    int ok = 1;
    uint32 rows_to_write = 0;

    //check for error
    if (setjmp(NativeDecoder::strip_buf)) {
        if (raster) {
            _TIFFfree(raster);
            raster = nullptr;
        }
        if (rasterForBottomLine) {
            _TIFFfree(rasterForBottomLine);
            rasterForBottomLine = nullptr;
        }
        if (matrixTopLine) {
            _TIFFfree(matrixTopLine);
            matrixTopLine = nullptr;
        }
        if (matrixBottomLine) {
            _TIFFfree(matrixBottomLine);
            matrixBottomLine = nullptr;
        }

        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }

        return nullptr;
    }

    for (int i = 0; (i < stripMax * rowPerStrip || i > boundY + boundHeight); i += rowPerStrip) {

        if (i + rowPerStrip <= boundY) {
            continue;
        }
        if (i > boundY + boundHeight) {
            break;
        }

        sendProgress(processedProgress * *bitmapWidth, progressTotal);
        processedProgress += rowPerStrip / inSampleSize;

        //if second raster is exist - copy it to work raster end decode next strip
        if (isSecondRasterExist) {
            _TIFFmemcpy(raster, rasterForBottomLine, origwidth * rowPerStrip * sizeof(uint32));

            //If next strip is exist - decode it, invert lines
            if (i + rowPerStrip < stripMax * rowPerStrip) {
                TIFFReadRGBAStrip(image, i + rowPerStrip, rasterForBottomLine);
                isSecondRasterExist = 1;

                rows_to_write = 0;
                if (i + rowPerStrip * 2 > origheight) {
                    rows_to_write = origheight - i - rowPerStrip;
                } else {
                    rows_to_write = rowPerStrip;
                }

                if (origorientation <= 4) {
                    for (int line = 0; line < rows_to_write / 2; line++) {
                        unsigned int *top_line, *bottom_line;

                        top_line = rasterForBottomLine + origwidth * line;
                        bottom_line = rasterForBottomLine + origwidth * (rows_to_write - line - 1);

                        _TIFFmemcpy(work_line_buf, top_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(bottom_line, work_line_buf, sizeof(unsigned int) * origwidth);
                    }
                }
            } else {
                isSecondRasterExist = 0;
            }
        } else {
            //if second raster is not exist - first processing - read first and second raster
            TIFFReadRGBAStrip(image, i, raster);
            //invert lines, because libtiff origin is bottom left instead of top left
            rows_to_write = 0;
            if (i + rowPerStrip > origheight) {
                rows_to_write = origheight - i;
            } else {
                rows_to_write = rowPerStrip;
            }

            if (origorientation <= 4) {
                for (int line = 0; line < rows_to_write / 2; line++) {
                    unsigned int *top_line, *bottom_line;

                    top_line = raster + origwidth * line;
                    bottom_line = raster + origwidth * (rows_to_write - line - 1);

                    _TIFFmemcpy(work_line_buf, top_line, sizeof(unsigned int) * origwidth);
                    _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * origwidth);
                    _TIFFmemcpy(bottom_line, work_line_buf, sizeof(unsigned int) * origwidth);
                }
            }

            //if next strip is exist - read it and invert lines
            if (i + rowPerStrip < origheight) {
                TIFFReadRGBAStrip(image, i + rowPerStrip, rasterForBottomLine);
                isSecondRasterExist = 1;

                //invert lines, because libtiff origin is bottom left instead of top left
                rows_to_write = 0;
                if (i + rowPerStrip * 2 > origheight) {
                    rows_to_write = origheight - i - rowPerStrip;
                } else {
                    rows_to_write = rowPerStrip;
                }
                if (origorientation <= 4) {
                    for (int line = 0; line < rows_to_write / 2; line++) {
                        unsigned int *top_line, *bottom_line;

                        top_line = rasterForBottomLine + origwidth * line;
                        bottom_line = rasterForBottomLine + origwidth * (rows_to_write - line - 1);

                        _TIFFmemcpy(work_line_buf, top_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * origwidth);
                        _TIFFmemcpy(bottom_line, work_line_buf, sizeof(unsigned int) * origwidth);
                    }
                }
            }
        }

        if (isSecondRasterExist) {
            _TIFFmemcpy(matrixBottomLine, rasterForBottomLine /*+ lineAddrToCopyBottomLine * origwidth*/, sizeof(unsigned int) * origwidth);
        }
        for (int sourceStripY = 0; targetY < *bitmapHeight && sourceStripY < rowPerStrip; sourceStripY++) {
            if (checkStop()) {
                if (raster) {
                    _TIFFfree(raster);
                    raster = nullptr;
                }
                if (rasterForBottomLine) {
                    _TIFFfree(rasterForBottomLine);
                    rasterForBottomLine = nullptr;
                }
                if (matrixTopLine) {
                    _TIFFfree(matrixTopLine);
                    matrixTopLine = nullptr;
                }
                if (matrixBottomLine) {
                    _TIFFfree(matrixBottomLine);
                    matrixBottomLine = nullptr;
                }
                __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
                return nullptr;
            }

            if (i + sourceStripY >= boundY) {
                // if total line of source image is equal to inSampleSize*N then process this line
                if (globalLineCounter % inSampleSize == 0) {
                    for (int targetX = 0, sourceStripX = 0; targetX < *bitmapWidth; sourceStripX += inSampleSize) {
                        jint crPix;
                        if (inSampleSize == 1) {
                            crPix = raster[sourceStripY * origwidth + sourceStripX];
                        } else {
                            crPix = applyFilterForStrip(sourceStripX, sourceStripY, raster, matrixTopLine, matrixBottomLine, rowPerStrip, globalLineCounter, isSecondRasterExist);
                        }
                        pixels[targetY * *bitmapWidth + targetX] = crPix;
                        targetX++;
                    }
                    targetY++;
                }
                //increment global source image line counter
                globalLineCounter++;
            }

            if (sourceStripY == rowPerStrip - 1 && i + rowPerStrip < origheight) {
                _TIFFmemcpy(matrixTopLine, raster + sourceStripY * origwidth, sizeof(unsigned int) * origwidth);
            }
        }
    }
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Decoding finished. Free memory");

    //Close Buffers
    if (raster) {
        _TIFFfree(raster);
        raster = nullptr;
    }

    if (rasterForBottomLine) {
        _TIFFfree(rasterForBottomLine);
        rasterForBottomLine = nullptr;
    }

    if (matrixTopLine) {
        _TIFFfree(matrixTopLine);
        matrixTopLine = nullptr;
    }

    if (matrixBottomLine) {
        _TIFFfree(matrixBottomLine);
        matrixBottomLine = nullptr;
    }

    processedProgress *= *bitmapWidth;

    if (useOrientationTag) {
        uint32 buf;
        switch (origorientation) {
            case ORIENTATION_TOPLEFT:
            case ORIENTATION_TOPRIGHT:
                break;
            case ORIENTATION_BOTRIGHT:
            case ORIENTATION_BOTLEFT:
                flipPixelsVertical(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_LEFTTOP:
                rotateRaster(pixels, 90, bitmapWidth, bitmapHeight);
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
            case ORIENTATION_RIGHTTOP:
                rotateRaster(pixels, 270, bitmapWidth, bitmapHeight);
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
            case ORIENTATION_RIGHTBOT:
                rotateRaster(pixels, 90, bitmapWidth, bitmapHeight);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
            case ORIENTATION_LEFTBOT:
                rotateRaster(pixels, 270, bitmapWidth, bitmapHeight);
                buf = *bitmapWidth;
                *bitmapWidth = *bitmapHeight;
                *bitmapHeight = buf;
                break;
        }

    } else if (origorientation == ORIENTATION_TOPRIGHT || origorientation == ORIENTATION_BOTRIGHT || origorientation == ORIENTATION_RIGHTTOP || origorientation == ORIENTATION_RIGHTBOT) {
        flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
    }

    uint32 tmpPixelBufferSize = (boundWidth / inSampleSize) * (boundHeight / inSampleSize);

    estimateMem = (sizeof(jint) * pixelsBufferSize); //temp buffer for decoded pixels
    estimateMem += (sizeof(jint) * tmpPixelBufferSize); //final buffer that will store original image
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);
    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    jint *tmpPixels = (jint *) malloc(sizeof(jint) * tmpPixelBufferSize);
    uint32 startPosX = 0;

    if (useOrientationTag && (origorientation == ORIENTATION_TOPRIGHT || origorientation == ORIENTATION_BOTRIGHT
                              || origorientation == ORIENTATION_LEFTBOT || origorientation == ORIENTATION_RIGHTBOT)) {
        startPosX = *bitmapWidth - boundX / inSampleSize;
        for (int ox = startPosX, nx = 0; nx < boundWidth / inSampleSize; ox--, nx++) {
            sendProgress(processedProgress + nx * boundWidth / inSampleSize, progressTotal);
            for (int oy = 0, ny = 0; ny < boundHeight / inSampleSize; oy++, ny++) {
                if (useOrientationTag && (origorientation > 4)) {
                    tmpPixels[nx * (boundHeight / inSampleSize) + ny] = pixels[ox * *bitmapHeight + oy];
                } else {
                    tmpPixels[ny * (boundWidth / inSampleSize) + nx] = pixels[oy * *bitmapWidth + ox];
                }
            }
        }
    } else {
        startPosX = boundX / inSampleSize;
        for (int ox = startPosX, nx = 0; nx < boundWidth / inSampleSize; ox++, nx++) {
            sendProgress(processedProgress + nx * boundWidth / inSampleSize, progressTotal);
            for (int oy = 0, ny = 0; ny < boundHeight / inSampleSize; oy++, ny++) {
                if (useOrientationTag && (origorientation > 4)) {
                    tmpPixels[nx * (boundHeight / inSampleSize) + ny] = pixels[ox * *bitmapHeight + oy];
                } else {
                    tmpPixels[ny * (boundWidth / inSampleSize) + nx] = pixels[oy * *bitmapWidth + ox];
                }
            }
        }
    }

    free(pixels);
    pixels = tmpPixels;
    *bitmapWidth = boundWidth / inSampleSize;
    *bitmapHeight = boundHeight / inSampleSize;

    return pixels;
}

//Apply filter to pixel
jint NativeDecoder::applyFilterForStrip(int x, int y, const uint32 *raster, const unsigned int *matrixTopLine, const unsigned int *matrixBottomLine, int rowPerStrip, int globalLineCounter, int isSecondRasterExist) const {
    jint crPix = raster[y * origwidth + x];
    int sum = 1;

    int alpha = colorMask & crPix >> 24;
    int red = colorMask & crPix >> 16;
    int green = colorMask & crPix >> 8;
    int blue = colorMask & crPix;

    //topleft
    if (x - 1 >= 0 && y - 1 >= 0) {
        crPix = raster[(y - 1) * origwidth + x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    } else if (x - 1 >= 0 && y - 1 == -1 && globalLineCounter > 0) {
        crPix = matrixTopLine[x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //top
    if (y - 1 >= 0) {
        crPix = raster[(y - 1) * origwidth + x];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    } else if (y - 1 == -1 && globalLineCounter > 0) {
        crPix = matrixTopLine[x];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    // topright
    if (x + 1 < origwidth && y - 1 >= 0) {
        crPix = raster[(y - 1) * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    } else if (x + 1 < origwidth && y - 1 == -1 && globalLineCounter > 0) {
        crPix = matrixTopLine[x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    //right
    if (x + 1 < origwidth) {
        crPix = raster[y * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    //bottomright
    if (x + 1 < origwidth && y + 1 < rowPerStrip) {
        crPix = raster[(y + 1) * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    } else if (x + 1 < origwidth && y + 1 == rowPerStrip && isSecondRasterExist) {
        crPix = matrixBottomLine[x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    //bottom
    if (y + 1 < rowPerStrip) {
        crPix = raster[(y + 1) * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    } else if (y + 1 == rowPerStrip && isSecondRasterExist) {
        crPix = matrixBottomLine[x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    //bottomleft
    if (x - 1 >= 0 && y + 1 < rowPerStrip) {
        crPix = raster[(y + 1) * origwidth + x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    } else if (x - 1 >= 0 && y + 1 == rowPerStrip && isSecondRasterExist) {
        crPix = matrixBottomLine[x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    //left
    if (x - 1 >= 0) {
        crPix = raster[y * origwidth + x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    red /= sum;
    if (red > 255) red = 255;
    if (red < 0) red = 0;

    green /= sum;
    if (green > 255) green = 255;
    if (green < 0) green = 0;

    blue /= sum;
    if (blue > 255) blue = 255;
    if (blue < 0) blue = 0;

    alpha /= sum;
    if (alpha > 255) alpha = 255;
    if (alpha < 0) alpha = 0;

    crPix = (alpha << 24) | (red << 16) | (green << 8) | (blue);
    return crPix;
}

jint *NativeDecoder::getSampledRasterFromTile(int inSampleSize, int *bitmapWidth, int *bitmapHeight) {
    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = tileErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    jint *pixels = nullptr;
    *bitmapWidth = origwidth / inSampleSize;
    *bitmapHeight = origheight / inSampleSize;
    uint32 pixelsBufferSize = *bitmapWidth * *bitmapHeight;

    uint32 tileWidth = 0, tileHeight = 0;
    TIFFGetField(image, TIFFTAG_TILEWIDTH, &tileWidth);
    TIFFGetField(image, TIFFTAG_TILELENGTH, &tileHeight);

    unsigned long estimateMem = 0;
    estimateMem += (sizeof(jint) * pixelsBufferSize); //buffer for decoded pixels
    estimateMem += (tileWidth * tileHeight * sizeof(uint32)) * 3; //current, left and right tiles buffers
    estimateMem += (tileWidth * sizeof(uint32)); //work line for rotate tile
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);
    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    pixels = (jint *) malloc(sizeof(jint) * pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    }

    uint32 row, column;

    //main worker tile
    auto *rasterTile = (uint32 *) _TIFFmalloc(tileWidth * tileHeight * sizeof(uint32));
    //left tile
    auto *rasterTileLeft = (uint32 *) _TIFFmalloc(tileWidth * tileHeight * sizeof(uint32));
    //right tile
    auto *rasterTileRight = (uint32 *) _TIFFmalloc(tileWidth * tileHeight * sizeof(uint32));

    auto *work_line_buf = (uint32 *) _TIFFmalloc(tileWidth * sizeof(uint32));

    //this variable calculate processed pixels for x and y direction to make right offsets at the begining of next tile
    //offset calculated from condition globalProcessed % inSampleSize should be 0
    uint32 globalProcessedX = 0;
    uint32 globalProcessedY = 0;

    //check for error
    if (setjmp(NativeDecoder::tile_buf)) {
        if (rasterTile) {
            _TIFFfree(rasterTile);
            rasterTile = nullptr;
        }
        if (rasterTileLeft) {
            _TIFFfree(rasterTileLeft);
            rasterTileLeft = nullptr;
        }
        if (rasterTileRight) {
            _TIFFfree(rasterTileRight);
            rasterTileRight = nullptr;
        }
        if (work_line_buf) {
            _TIFFfree(work_line_buf);
            work_line_buf = nullptr;
        }

        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }

        return nullptr;
    }

    for (row = 0; row < origheight; row += tileHeight) {
        short leftTileExists = 0;
        short rightTileExists = 0;
        for (column = 0; column < origwidth; column += tileWidth) {
            sendProgress(row * origwidth + column, progressTotal);

            //If not first column - we should have previous tile - copy it to left tile buffer
            if (column != 0) {
                _TIFFmemcpy(rasterTileLeft, rasterTile, tileWidth * tileHeight * sizeof(uint32));
                leftTileExists = 1;
            } else {
                leftTileExists = 0;
            }

            //if current column + tile width is less than origin width - we have right tile - copy it to current tile and read next tile to rasterTileRight buffer
            if (column + tileWidth < origwidth && rightTileExists) {
                _TIFFmemcpy(rasterTile, rasterTileRight, tileWidth * tileHeight * sizeof(uint32));
                TIFFReadRGBATile(image, column + tileWidth, row, rasterTileRight);
                rightTileExists = 1;
            } else if (column + tileWidth < origwidth) {
                //have right tile but this is first tile in row, so need to read raster and right raster
                TIFFReadRGBATile(image, column + tileWidth, row, rasterTileRight);
                TIFFReadRGBATile(image, column, row, rasterTile);
                rightTileExists = 1;

                //in that case we also need to invert lines in rasterTile
                switch (origorientation) {
                    case 1:
                    case 5:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 2:
                    case 6:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 3:
                    case 7:
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                }
            } else {
                //otherwise we haven't right tile buffer, so we should read tile to current buffer
                TIFFReadRGBATile(image, column, row, rasterTile);
                rightTileExists = 0;
            }

            //if we have right tile - current tile already rotated and we need to rotate only right tile
            if (rightTileExists) {
                switch (origorientation) {
                    case 1:
                    case 5:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        break;
                    case 2:
                    case 6:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        break;
                    case 3:
                    case 7:
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        break;
                }
            } else {
                //otherwise - current tile not rotated so rotate it
                //tile orig is on bottom left - should change lines
                switch (origorientation) {
                    case 1:
                    case 5:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 2:
                    case 6:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 3:
                    case 7:
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                }
            }

            if (inSampleSize == 1) {
                int rowHasPixels = 0;
                for (int th = 0, bh = 0; th < tileHeight; th++) {
                    for (int tw = 0, bw = 0; tw < tileWidth; tw++) {
                        uint32 srcPosition = th * tileWidth + tw;
                        if (rasterTile[srcPosition] != 0) {
                            int position = 0;
                            if (origorientation <= 4) {
                                position = (row + bh) * *bitmapWidth + column + bw;
                            } else {
                                position = (column + bw) * *bitmapHeight + row + bh;
                            }
                            pixels[position] = rasterTile[srcPosition];
                            rowHasPixels = 1;
                            bw++;
                        }
                    }
                    if (rowHasPixels) {
                        bh++;
                        rowHasPixels = 0;
                    }
                }
            } else {
                //Tile could begin from not filled pixel(pixel[x,y] == 0). This variables allow to calculate begining of filled pixels
                int tileStartDataX = -1;
                int tileStartDataY = -1;

                for (int origTileY = 0, pixY = row / inSampleSize; origTileY < tileHeight && pixY < *bitmapHeight; origTileY++) {
                    if (checkStop()) {
                        if (rasterTile) {
                            _TIFFfree(rasterTile);
                            rasterTile = nullptr;
                        }
                        if (rasterTileLeft) {
                            _TIFFfree(rasterTileLeft);
                            rasterTileLeft = nullptr;
                        }
                        if (rasterTileRight) {
                            _TIFFfree(rasterTileRight);
                            rasterTileRight = nullptr;
                        }
                        if (work_line_buf) {
                            _TIFFfree(work_line_buf);
                            work_line_buf = nullptr;
                        }
                        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
                        return nullptr;
                    }

                    if (tileStartDataY != -1 && globalProcessedY % inSampleSize != 0) {
                        if (tileStartDataY != -1) {
                            globalProcessedY++;
                        }
                    } else {
                        for (int origTileX = 0, pixX = column / inSampleSize; origTileX < tileWidth && pixX < *bitmapWidth; origTileX++) {
                            if (tileStartDataX != -1 && globalProcessedX % inSampleSize != 0) {
                                if (tileStartDataX != -1) {
                                    globalProcessedX++;
                                }
                            } else {
                                if (rasterTile[origTileY * tileWidth + origTileX] != 0) {
                                    if (tileStartDataX == -1) {
                                        tileStartDataX = origTileX;
                                    }
                                    if (tileStartDataY == -1) {
                                        tileStartDataY = origTileY;
                                    }

                                    jint crPix = applyFilterForTile(origTileX, origTileY, rasterTile, rasterTileLeft, rasterTileRight, tileWidth, tileHeight, leftTileExists, rightTileExists);

                                    int position;
                                    if (origorientation <= 4) {
                                        position = pixY * *bitmapWidth + pixX;
                                    } else {
                                        position = pixX * *bitmapHeight + pixY;
                                    }
                                    pixels[position] = crPix;
                                } else {
                                    if (tileStartDataX != -1) tileStartDataX = -1;
                                    if (tileStartDataY != -1) tileStartDataY = -1;
                                }

                                if (tileStartDataX != -1) {
                                    pixX++;
                                    globalProcessedX++;
                                }
                            }
                        }
                        if (tileStartDataY != -1) {
                            pixY++;
                            globalProcessedY++;
                        }
                    }
                }
            }
        }
    }

    if (rasterTile) {
        _TIFFfree(rasterTile);
        rasterTile = nullptr;
    }
    if (rasterTileLeft) {
        _TIFFfree(rasterTileLeft);
        rasterTileLeft = nullptr;
    }
    if (rasterTileRight) {
        _TIFFfree(rasterTileRight);
        rasterTileRight = nullptr;
    }
    if (work_line_buf) {
        _TIFFfree(work_line_buf);
        work_line_buf = nullptr;
    }

    if (useOrientationTag) {
        switch (origorientation) {
            case ORIENTATION_TOPLEFT:
            case ORIENTATION_LEFTTOP:
                break;
            case ORIENTATION_TOPRIGHT:
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_RIGHTTOP:
                flipPixelsHorizontal(*bitmapHeight, *bitmapWidth, pixels);
                break;
            case ORIENTATION_BOTRIGHT:
            case ORIENTATION_RIGHTBOT:
                rotateRaster(pixels, 180, bitmapWidth, bitmapHeight);
                break;
            case ORIENTATION_BOTLEFT:
                flipPixelsVertical(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_LEFTBOT:
                flipPixelsVertical(*bitmapHeight, *bitmapWidth, pixels);
                break;
        }
    } else {
        if (origorientation > 4) {
            uint32 buf = *bitmapWidth;
            *bitmapWidth = *bitmapHeight;
            *bitmapHeight = buf;
            rotateRaster(pixels, 90, bitmapWidth, bitmapHeight);
            flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
        }
    }

    return pixels;
}

jint *NativeDecoder::getSampledRasterFromTileWithBounds(int inSampleSize, int *bitmapWidth, int *bitmapHeight) {
    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = tileErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    //First read all tiles that are on necessary area

    uint32 tileWidth = 0, tileHeight = 0;
    TIFFGetField(image, TIFFTAG_TILEWIDTH, &tileWidth);
    TIFFGetField(image, TIFFTAG_TILELENGTH, &tileHeight);

    //find first and last tile to process
    auto firstTileX = (uint32) (boundX / tileWidth);
    auto firstTileY = (uint32) (boundY / tileHeight);

    uint32 lastTileX = (uint32) ((boundX + boundWidth) / tileWidth) + 1;
    uint32 lastTileY = (uint32) ((boundY + boundHeight) / tileHeight) + 1;

    jint *pixels = nullptr;
    *bitmapWidth = /*boundWidth*/ (lastTileX - firstTileX) * tileWidth / inSampleSize;//origwidth / inSampleSize;
    *bitmapHeight = /*boundHeight*/ (lastTileY - firstTileY) * tileHeight / inSampleSize;//origheight / inSampleSize;
    uint32 pixelsBufferSize = *bitmapWidth * *bitmapHeight;

    unsigned long estimateMem = 0;
    estimateMem += (sizeof(jint) * pixelsBufferSize); //buffer for decoded pixels
    estimateMem += (tileWidth * tileHeight * sizeof(uint32)) * 3; //current, left and right tiles buffers
    estimateMem += (tileWidth * sizeof(uint32)); //work line for rotate tile
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);
    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    pixels = (jint *) malloc(sizeof(jint) * pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    }

    progressTotal = pixelsBufferSize + (boundWidth / inSampleSize) * (boundHeight / inSampleSize);
    sendProgress(0, progressTotal);
    jlong processedProgress = 0;

    uint32 row, column, rowDest, columnDest;

    //main worker tile
    auto *rasterTile = (uint32 *) _TIFFmalloc(tileWidth * tileHeight * sizeof(uint32));
    //left tile
    auto *rasterTileLeft = (uint32 *) _TIFFmalloc(tileWidth * tileHeight * sizeof(uint32));
    //right tile
    auto *rasterTileRight = (uint32 *) _TIFFmalloc(tileWidth * tileHeight * sizeof(uint32));

    auto *work_line_buf = (uint32 *) _TIFFmalloc(tileWidth * sizeof(uint32));

    //check for error
    if (setjmp(NativeDecoder::tile_buf)) {
        if (rasterTile) {
            _TIFFfree(rasterTile);
            rasterTile = nullptr;
        }
        if (rasterTileLeft) {
            _TIFFfree(rasterTileLeft);
            rasterTileLeft = nullptr;
        }
        if (rasterTileRight) {
            _TIFFfree(rasterTileRight);
            rasterTileRight = nullptr;
        }
        if (work_line_buf) {
            _TIFFfree(work_line_buf);
            work_line_buf = nullptr;
        }

        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }

        return nullptr;
    }

    //this variable calculate processed pixels for x and y direction to make right offsets at the begining of next tile
    //offset calculated from condition globalProcessed % inSampleSize should be 0
    uint32 globalProcessedX = 0;
    uint32 globalProcessedY = 0;

    uint32 progressRow = 0;
    uint32 progressColumn = 0;

    rowDest = columnDest = 0;
    for (row = firstTileY * tileHeight; row < lastTileY * tileHeight; row += tileHeight, progressRow += tileHeight) {
        columnDest = 0;
        short leftTileExists = 0;
        short rightTileExists = 0;
        for (column = firstTileX * tileWidth; column < lastTileX * tileWidth; column += tileWidth, progressColumn += tileWidth) {
            processedProgress = progressRow * *bitmapWidth + progressColumn;
            sendProgress(processedProgress, progressTotal);

            //If not first column - we should have previous tile - copy it to left tile buffer
            if (column != firstTileY) {
                _TIFFmemcpy(rasterTileLeft, rasterTile, tileWidth * tileHeight * sizeof(uint32));
                leftTileExists = 1;
            } else {
                leftTileExists = 0;
            }
            //if current column + tile width is less than origin width - we have right tile - copy it to current tile and read next tile to rasterTileRight buffer
            if (column + tileWidth < origwidth && rightTileExists) {
                _TIFFmemcpy(rasterTile, rasterTileRight, tileWidth * tileHeight * sizeof(uint32));
                TIFFReadRGBATile(image, column + tileWidth, row, rasterTileRight);
                rightTileExists = 1;
            } else if (column + tileWidth < origwidth) {
                //have right tile but this is first tile in row, so need to read raster and right raster
                TIFFReadRGBATile(image, column + tileWidth, row, rasterTileRight);
                TIFFReadRGBATile(image, column, row, rasterTile);
                rightTileExists = 1;

                //in that case we also need to invert lines in rasterTile
                switch (origorientation) {
                    case 1:
                    case 5:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 2:
                    case 6:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 3:
                    case 7:
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                }
            } else {
                //otherwise we haven't right tile buffer, so we should read tile to current buffer
                TIFFReadRGBATile(image, column, row, rasterTile);
                rightTileExists = 0;
            }

            //if we have right tile - current tile already rotated and we need to rotate only right tile
            if (rightTileExists) {
                switch (origorientation) {
                    case 1:
                    case 5:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        break;
                    case 2:
                    case 6:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        break;
                    case 3:
                    case 7:
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTileRight, work_line_buf);
                        break;
                }
            } else {
                //otherwise - current tile not rotated so rotate it
                //tile orig is on bottom left - should change lines
                switch (origorientation) {
                    case 1:
                    case 5:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 2:
                    case 6:
                        rotateTileLinesVertical(tileHeight, tileWidth, rasterTile, work_line_buf);
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                    case 3:
                    case 7:
                        rotateTileLinesHorizontal(tileHeight, tileWidth, rasterTile, work_line_buf);
                        break;
                }
            }

            //Tile could begin from not filled pixel(pixel[x,y] == 0). This variables allow to calculate begining of filled pixels
            int tileStartDataX = -1;
            int tileStartDataY = -1;

            for (int origTileY = 0, pixY = rowDest / inSampleSize; origTileY < tileHeight && pixY < *bitmapHeight; origTileY++) {
                if (checkStop()) {
                    if (rasterTile) {
                        _TIFFfree(rasterTile);
                        rasterTile = nullptr;
                    }
                    if (rasterTileLeft) {
                        _TIFFfree(rasterTileLeft);
                        rasterTileLeft = nullptr;
                    }
                    if (rasterTileRight) {
                        _TIFFfree(rasterTileRight);
                        rasterTileRight = nullptr;
                    }
                    if (work_line_buf) {
                        _TIFFfree(work_line_buf);
                        work_line_buf = nullptr;
                    }
                    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
                    return nullptr;
                }

                if (tileStartDataY != -1 && globalProcessedY % inSampleSize != 0) {
                    if (tileStartDataY != -1) {
                        globalProcessedY++;
                    }
                } else {
                    for (int origTileX = 0, pixX = columnDest / inSampleSize; origTileX < tileWidth && pixX < *bitmapWidth; origTileX++) {
                        if (tileStartDataX != -1 && globalProcessedX % inSampleSize != 0) {
                            if (tileStartDataX != -1) {
                                globalProcessedX++;
                            }
                        } else {
                            if (rasterTile[origTileY * tileWidth + origTileX] != 0) {
                                if (tileStartDataX == -1) {
                                    tileStartDataX = origTileX;
                                }
                                if (tileStartDataY == -1) {
                                    tileStartDataY = origTileY;
                                }

                                jint crPix;
                                if (inSampleSize == 1) {
                                    crPix = rasterTile[origTileY * tileWidth + origTileX];
                                } else {
                                    crPix = applyFilterForTile(origTileX, origTileY, rasterTile, rasterTileLeft, rasterTileRight, tileWidth, tileHeight, leftTileExists, rightTileExists);
                                }

                                int position;
                                if (origorientation <= 4) {
                                    position = pixY * *bitmapWidth + pixX;
                                } else {
                                    position = pixX * *bitmapHeight + pixY;
                                }
                                pixels[position] = crPix;
                            } else {
                                if (tileStartDataX != -1) tileStartDataX = -1;
                                if (tileStartDataY != -1) tileStartDataY = -1;
                            }

                            if (tileStartDataX != -1) {
                                pixX++;
                                globalProcessedX++;
                            }

                        }
                    }
                    if (tileStartDataY != -1) {
                        pixY++;
                        globalProcessedY++;
                    }
                }
            }
            columnDest += tileWidth;
        }
        rowDest += tileHeight;
    }

    if (rasterTile) {
        _TIFFfree(rasterTile);
        rasterTile = nullptr;
    }
    if (rasterTileLeft) {
        _TIFFfree(rasterTileLeft);
        rasterTileLeft = nullptr;
    }
    if (rasterTileRight) {
        _TIFFfree(rasterTileRight);
        rasterTileRight = nullptr;
    }
    if (work_line_buf) {
        _TIFFfree(work_line_buf);
        work_line_buf = nullptr;
    }

    //Copy necessary pixels to new array if orientation <=4
    uint32 tmpPixelBufferSize = (boundWidth / inSampleSize) * (boundHeight / inSampleSize);

    estimateMem = (sizeof(jint) * pixelsBufferSize); //buffer for decoded pixels
    estimateMem += (sizeof(jint) * tmpPixelBufferSize); //finall buffer
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);
    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    if (origorientation <= 4) {
        jint *tmpPixels = (jint *) malloc(sizeof(jint) * tmpPixelBufferSize);
        uint32 startPosX = boundX % tileWidth / inSampleSize;//(firstTileX * tileWidth - tileWidth + boundX) / inSampleSize;
        uint32 startPosY = boundY % tileHeight / inSampleSize;//(firstTileY * tileHeight - tileHeight + boundY) /inSampleSize;
        for (int ox = startPosX, nx = 0; nx < boundWidth / inSampleSize; ox++, nx++) {
            sendProgress(processedProgress + nx * (boundHeight / inSampleSize), progressTotal);
            for (int oy = startPosY, ny = 0; ny < boundHeight / inSampleSize; oy++, ny++) {
                tmpPixels[ny * (boundWidth / inSampleSize) + nx] = pixels[oy * *bitmapWidth + ox];
            }
        }

        free(pixels);
        pixels = tmpPixels;
        *bitmapWidth = boundWidth / inSampleSize;
        *bitmapHeight = boundHeight / inSampleSize;
    }

    if (useOrientationTag) {
        switch (origorientation) {
            case ORIENTATION_TOPLEFT:
            case ORIENTATION_LEFTTOP:
                break;
            case ORIENTATION_TOPRIGHT:
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_RIGHTTOP:
                flipPixelsHorizontal(*bitmapHeight, *bitmapWidth, pixels);
                break;
            case ORIENTATION_BOTRIGHT:
            case ORIENTATION_RIGHTBOT:
                rotateRaster(pixels, 180, bitmapWidth, bitmapHeight);
                break;
            case ORIENTATION_BOTLEFT:
                flipPixelsVertical(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_LEFTBOT:
                flipPixelsVertical(*bitmapHeight, *bitmapWidth, pixels);
                break;
        }
    } else {
        if (origorientation > 4) {
            uint32 buf = *bitmapWidth;
            *bitmapWidth = *bitmapHeight;
            *bitmapHeight = buf;
            rotateRaster(pixels, 90, bitmapWidth, bitmapHeight);
            flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
        }
    }

    //Copy necessary pixels to new array if orientation >4
    if (origorientation > 4) {
        jint *tmpPixels = (jint *) malloc(sizeof(jint) * tmpPixelBufferSize);
        uint32 startPosX = boundX % tileWidth / inSampleSize;
        uint32 startPosY = boundY % tileHeight / inSampleSize;
        for (int ox = startPosX, nx = 0; nx < boundWidth / inSampleSize; ox++, nx++) {
            sendProgress(processedProgress + nx * (boundHeight / inSampleSize), progressTotal);
            for (int oy = startPosY, ny = 0; ny < boundHeight / inSampleSize; oy++, ny++) {
                if (useOrientationTag) {
                    tmpPixels[nx * (boundHeight / inSampleSize) + ny] = pixels[ox * *bitmapHeight + oy];
                } else {
                    tmpPixels[ny * (boundWidth / inSampleSize) + nx] = pixels[oy * *bitmapWidth + ox];
                }
            }
        }

        free(pixels);
        pixels = tmpPixels;
        *bitmapWidth = boundWidth / inSampleSize;
        *bitmapHeight = boundHeight / inSampleSize;
    }

    return pixels;
}

//Apply filter to pixel
jint NativeDecoder::applyFilterForTile(int x, int y, const uint32 *rasterTile, const uint32 *rasterTileLeft, const uint32 *rasterTileRight, uint32 tileWidth, uint32 tileHeight, short leftTileExists, short rightTileExists) const {
    jint crPix = rasterTile[y * tileWidth + x];
    int sum = 1;

    int alpha = colorMask & crPix >> 24;
    int red = colorMask & crPix >> 16;
    int green = colorMask & crPix >> 8;
    int blue = colorMask & crPix;

    //using kernel 3x3

    //topleft
    if (x - 1 >= 0 && y - 1 >= 0) {
        crPix = rasterTile[(y - 1) * tileWidth + x - 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    } else if (y - 1 >= 0 && leftTileExists) {
        crPix = rasterTileLeft[(y - 1) * tileWidth + tileWidth - 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    //top
    if (y - 1 >= 0) {
        crPix = rasterTile[(y - 1) * tileWidth + x];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    // topright
    if (x + 1 < tileWidth && y - 1 >= 0) {
        crPix = rasterTile[(y - 1) * tileWidth + x + 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    } else if (y - 1 >= 0 && rightTileExists) {
        crPix = rasterTileRight[(y - 1) * tileWidth];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    //right
    if (x + 1 < tileWidth) {
        crPix = rasterTile[y * tileWidth + x + 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    } else if (rightTileExists) {
        crPix = rasterTileRight[y * tileWidth];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    //bottomright
    if (x + 1 < tileWidth && y + 1 < tileHeight) {
        crPix = rasterTile[(y + 1) * tileWidth + x + 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    } else if (y + 1 < tileHeight && rightTileExists) {
        crPix = rasterTileRight[(y + 1) * tileWidth];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    //bottom
    if (y + 1 < tileHeight) {
        crPix = rasterTile[(y + 1) * tileWidth + x];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    //bottomleft
    if (x - 1 >= 0 && y + 1 < tileHeight) {
        crPix = rasterTile[(y + 1) * tileWidth + x - 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    } else if (y + 1 < tileHeight && leftTileExists) {
        crPix = rasterTileLeft[(y + 1) * tileWidth + tileWidth - 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    //left
    if (x - 1 >= 0) {
        crPix = rasterTile[y * tileWidth + x - 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    } else if (leftTileExists) {
        crPix = rasterTileLeft[y * tileWidth + tileWidth - 1];
        if (crPix != 0) {
            red += colorMask & crPix >> 16;
            green += colorMask & crPix >> 8;
            blue += colorMask & crPix;
            alpha += colorMask & crPix >> 24;
            sum++;
        }
    }

    red /= sum;
    if (red > 255) red = 255;
    if (red < 0) red = 0;

    green /= sum;
    if (green > 255) green = 255;
    if (green < 0) green = 0;

    blue /= sum;
    if (blue > 255) blue = 255;
    if (blue < 0) blue = 0;

    alpha /= sum;
    if (alpha > 255) alpha = 255;
    if (alpha < 0) alpha = 0;

    crPix = (alpha << 24) | (red << 16) | (green << 8) | (blue);
    return crPix;
}

jint *NativeDecoder::getSampledRasterFromImage(int inSampleSize, int *bitmapWidth, int *bitmapHeight) {
    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = imageErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    //buffer size for decoding tiff image in RGBA format
    int origBufferSize = origwidth * origheight * sizeof(unsigned int);

    *bitmapWidth = origwidth / inSampleSize;
    *bitmapHeight = origheight / inSampleSize;
    //buffer size for creating scaled image;
    uint32 pixelsBufferSize = *bitmapWidth * *bitmapHeight * sizeof(jint);

    /**Estimate usage of memory for decoding*/
    unsigned long estimateMem = origBufferSize;//origBufferSize - size of decoded RGBA image
    if (inSampleSize > 1) {
        estimateMem += pixelsBufferSize; //if inSmapleSize greater than 1 we need aditional vevory for scaled image
    }
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);

    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    unsigned int *origBuffer = nullptr;

    origBuffer = (unsigned int *) _TIFFmalloc(origBufferSize);
    if (origBuffer == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for origBuffer");
        return nullptr;
    }

    jint *pixels = nullptr;

    //check for error
    if (setjmp(NativeDecoder::image_buf)) {
        if (origBuffer) {
            _TIFFfree(origBuffer);
            origBuffer = nullptr;
        }
        if (pixels) {
            free(pixels);
            pixels = nullptr;
        }

        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }

        return nullptr;
    }

    if (0 == TIFFReadRGBAImageOriented(image, origwidth, origheight, origBuffer, ORIENTATION_TOPLEFT, 0)) {
        free(origBuffer);
        const char *message = "Error reading image";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
        if (throwException) {
            throwDecodeFileException(message);
        }
        return nullptr;
    }

    if (inSampleSize == 1) {
        // Use buffer as is.
        pixels = (jint *) origBuffer;
    } else {
        // Sample the buffer.
        pixels = (jint *) malloc(pixelsBufferSize);
        if (pixels == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
            return nullptr;
        } else {
            for (int targetY = 0, sourceY = 0; targetY < *bitmapHeight; targetY++, sourceY += inSampleSize) {

                sendProgress(sourceY * origwidth, progressTotal);

                if (checkStop()) {
                    //TODO clear memory
                    if (origBuffer) {
                        _TIFFfree(origBuffer);
                        origBuffer = nullptr;
                    }
                    if (pixels) {
                        free(pixels);
                        pixels = nullptr;
                    }
                    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
                    return nullptr;
                }

                for (int targetX = 0, sourceX = 0; targetX < *bitmapWidth; targetX++, sourceX += inSampleSize) {
                    jint crPix = applyFilterForImage(sourceX, sourceY, origBuffer);
                    pixels[targetY * *bitmapWidth + targetX] = crPix;
                }
            }
        }

        //Close Buffer
        if (origBuffer) {
            _TIFFfree(origBuffer);
            origBuffer = nullptr;
        }
    }

    if (useOrientationTag) {
        fixOrientation(pixels, pixelsBufferSize, *bitmapWidth, *bitmapHeight);
    } else {
        switch (origorientation) {
            case ORIENTATION_TOPLEFT:
            case ORIENTATION_LEFTTOP:
                break;
            case ORIENTATION_TOPRIGHT:
            case ORIENTATION_RIGHTTOP:
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_BOTRIGHT:
            case ORIENTATION_RIGHTBOT:
                rotateRaster(pixels, 180, bitmapWidth, bitmapHeight);
                break;
            case ORIENTATION_BOTLEFT:
            case ORIENTATION_LEFTBOT:
                rotateRaster(pixels, 180, bitmapWidth, bitmapHeight);
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                break;
        }
    }

    return pixels;
}

jint *NativeDecoder::getSampledRasterFromImageWithBounds(int inSampleSize, int *bitmapWidth, int *bitmapHeight) {
    //init signal handler for catch SIGSEGV error that could be raised in libtiff
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = imageErrorHandler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &act, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t setup signal handler. Working without errors catching mechanism");
    }

    //buffer size for decoding tiff image in RGBA format
    int origBufferSize = origwidth * origheight * sizeof(unsigned int);

    *bitmapWidth = boundWidth / inSampleSize;//origwidth / inSampleSize;
    *bitmapHeight = boundHeight / inSampleSize;//origheight / inSampleSize;
    //buffer size for creating scaled image;
    uint32 pixelsBufferSize = *bitmapWidth * *bitmapHeight * sizeof(jint);

    /**Estimate usage of memory for decoding*/
    unsigned long estimateMem = origBufferSize;//origBufferSize - size of decoded RGBA image
    //if (inSampleSize > 1) {
    estimateMem += pixelsBufferSize; //if inSampleSize greater than 1 we need additional memory for scaled image
    //}
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "estimateMem", estimateMem);

    if (estimateMem > availableMemory) {
        if (throwException) {
            throw_not_enough_memory_exception(env, availableMemory, estimateMem);
        }
        return nullptr;
    }

    unsigned int *origBuffer = nullptr;
    jint *pixels = nullptr;

    //check for error
    if (setjmp(NativeDecoder::image_buf)) {
        if (origBuffer) {
            _TIFFfree(origBuffer);
            origBuffer = nullptr;
        }
        if (pixels) {
            free(pixels);
            pixels = nullptr;
        }

        const char *err = "Caught SIGSEGV signal(Segmentation fault or invalid memory reference)";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", err);
        if (throwException) {
            throwDecodeFileException(err);
        }

        return nullptr;
    }

    origBuffer = (unsigned int *) _TIFFmalloc(origBufferSize);
    if (origBuffer == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for origBuffer");
        return nullptr;
    }

    if (0 == TIFFReadRGBAImageOriented(image, origwidth, origheight, origBuffer, ORIENTATION_TOPLEFT, 0)) {
        free(origBuffer);
        const char *message = "Error reading image";
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "%s", message);
        if (throwException) {
            throwDecodeFileException(message);
        }
        return nullptr;
    }

    progressTotal = boundWidth / inSampleSize * boundHeight / inSampleSize;

    // Sample the buffer.
    pixels = (jint *) malloc(pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    } else {
        for (int targetY = 0, sourceY = boundY; targetY < *bitmapHeight; targetY++, sourceY += inSampleSize) {

            sendProgress(sourceY * boundWidth, progressTotal);

            if (checkStop()) {
                //TODO clear memory
                if (origBuffer) {
                    _TIFFfree(origBuffer);
                    origBuffer = nullptr;
                }
                if (pixels) {
                    free(pixels);
                    pixels = nullptr;
                }
                __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
                return nullptr;
            }

            for (int targetX = 0, sourceX = boundX; targetX < *bitmapWidth; targetX++, sourceX += inSampleSize) {
                jint crPix;
                if (inSampleSize == 1) {
                    crPix = origBuffer[sourceY * origwidth + sourceX];
                } else {
                    crPix = applyFilterForImage(sourceX, sourceY, origBuffer);
                }
                pixels[targetY * *bitmapWidth + targetX] = crPix;
            }
        }
    }

    //Close Buffer
    if (origBuffer) {
        _TIFFfree(origBuffer);
        origBuffer = nullptr;
    }

    if (useOrientationTag) {
        fixOrientation(pixels, pixelsBufferSize, *bitmapWidth, *bitmapHeight);
    } else {
        uint32 buf;
        switch (origorientation) {
            case ORIENTATION_TOPLEFT:
            case ORIENTATION_LEFTTOP:
                break;
            case ORIENTATION_TOPRIGHT:
            case ORIENTATION_RIGHTTOP:
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                break;
            case ORIENTATION_BOTRIGHT:
            case ORIENTATION_RIGHTBOT:
                rotateRaster(pixels, 180, bitmapWidth, bitmapHeight);
                break;
            case ORIENTATION_BOTLEFT:
            case ORIENTATION_LEFTBOT:
                rotateRaster(pixels, 180, bitmapWidth, bitmapHeight);
                flipPixelsHorizontal(*bitmapWidth, *bitmapHeight, pixels);
                break;
        }
    }

    return pixels;
}

//Apply filter to pixel
jint NativeDecoder::applyFilterForImage(int x, int y, const unsigned int *raster) const {
    jint crPix = raster[y * origwidth + x];
    int sum = 1;

    int alpha = colorMask & crPix >> 24;
    int red = colorMask & crPix >> 16;
    int green = colorMask & crPix >> 8;
    int blue = colorMask & crPix;

    //using kernel 3x3

    //topleft
    if (x - 1 >= 0 && y - 1 >= 0) {
        crPix = raster[(y - 1) * origwidth + x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //top
    if (y - 1 >= 0) {
        crPix = raster[(y - 1) * origwidth + x];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    // topright
    if (x + 1 < origwidth && y - 1 >= 0) {
        crPix = raster[(y - 1) * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //right
    if (x + 1 < origwidth) {
        crPix = raster[y * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //bottomright
    if (x + 1 < origwidth && y + 1 < origheight) {
        crPix = raster[(y + 1) * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //bottom
    if (y + 1 < origheight) {
        crPix = raster[(y + 1) * origwidth + x + 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //bottomleft
    if (x - 1 >= 0 && y + 1 < origheight) {
        crPix = raster[(y + 1) * origwidth + x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }
    //left
    if (x - 1 >= 0) {
        crPix = raster[y * origwidth + x - 1];
        red += colorMask & crPix >> 16;
        green += colorMask & crPix >> 8;
        blue += colorMask & crPix;
        alpha += colorMask & crPix >> 24;
        sum++;
    }

    red /= sum;
    if (red > 255) red = 255;
    if (red < 0) red = 0;

    green /= sum;
    if (green > 255) green = 255;
    if (green < 0) green = 0;

    blue /= sum;
    if (blue > 255) blue = 255;
    if (blue < 0) blue = 0;

    alpha /= sum;
    if (alpha > 255) alpha = 255;
    if (alpha < 0) alpha = 0;

    return (alpha << 24) | (red << 16) | (green << 8) | (blue);
}

int NativeDecoder::getDecodeMethod() {
    int method = -1;
    uint32 tileWidth, tileHeight;
    int readTW = 0, readTH = 0;
    readTW = TIFFGetField(image, TIFFTAG_TILEWIDTH, &tileWidth);
    readTH = TIFFGetField(image, TIFFTAG_TILELENGTH, &tileHeight);
    if (tileWidth > 0 && tileHeight > 0 && readTH > 0 && readTW > 0) {
        method = DECODE_METHOD_TILE;
    } else {
        int rowPerStrip = -1;
        TIFFGetField(image, TIFFTAG_ROWSPERSTRIP, &rowPerStrip);
        uint32 stripSize = TIFFStripSize(image);
        uint32 stripMax = TIFFNumberOfStrips(image);
        int estimate = origwidth * 3;
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "RPS", rowPerStrip);
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "stripSize", stripSize);
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "stripMax", stripMax);
        if (rowPerStrip != -1 && stripSize > 0 && stripMax > 1 && rowPerStrip < origheight) {
            method = DECODE_METHOD_STRIP;
        } else {
            method = DECODE_METHOD_IMAGE;
        }
    }

    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "Decode method", method);
    return method;
}

void NativeDecoder::rotateTileLinesVertical(uint32 tileHeight, uint32 tileWidth, uint32 *whatRotate, uint32 *bufferLine) {
    for (int line = 0; line < tileHeight / 2; line++) {
        unsigned int *top_line, *bottom_line;
        top_line = whatRotate + tileWidth * line;
        bottom_line = whatRotate + tileWidth * (tileHeight - line - 1);
        _TIFFmemcpy(bufferLine, top_line, sizeof(unsigned int) * tileWidth);
        _TIFFmemcpy(top_line, bottom_line, sizeof(unsigned int) * tileWidth);
        _TIFFmemcpy(bottom_line, bufferLine, sizeof(unsigned int) * tileWidth);
    }
}

void NativeDecoder::rotateTileLinesHorizontal(uint32 tileHeight, uint32 tileWidth, uint32 *whatRotate, uint32 *bufferLine) {
    uint32 buf;
    for (int y = 0; y < tileHeight; y++) {
        for (int x = 0; x < tileWidth / 2; x++) {
            buf = whatRotate[y * tileWidth + x];
            whatRotate[y * tileWidth + x] = whatRotate[y * tileWidth + tileWidth - x - 1];
            whatRotate[y * tileWidth + tileWidth - x - 1] = buf;
        }
    }
}

void NativeDecoder::flipPixelsVertical(uint32 width, uint32 height, jint *raster) {
    jint *bufferLine = (jint *) malloc(sizeof(jint) * width);
    for (int line = 0; line < height / 2; line++) {
        jint *top_line, *bottom_line;
        top_line = raster + width * line;
        bottom_line = raster + width * (height - line - 1);
        _TIFFmemcpy(bufferLine, top_line, sizeof(jint) * width);
        _TIFFmemcpy(top_line, bottom_line, sizeof(jint) * width);
        _TIFFmemcpy(bottom_line, bufferLine, sizeof(jint) * width);
    }
    free(bufferLine);
}

void NativeDecoder::flipPixelsHorizontal(uint32 width, uint32 height, jint *raster) {
    jint buf;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width / 2; x++) {
            buf = raster[y * width + x];
            raster[y * width + x] = raster[y * width + width - x - 1];
            raster[y * width + width - x - 1] = buf;
        }
    }
}

void NativeDecoder::rotateRaster(jint *raster, int angle, int *width, int *height) {
    int rotatedWidth = *width;
    int rotatedHeight = *height;
    int numberOf90s = angle / 90;
    if (numberOf90s % 2 != 0) {
        int tmp = rotatedWidth;
        rotatedWidth = rotatedHeight;
        rotatedHeight = tmp;
    }

    jint *rotated = (jint *) malloc(sizeof(jint) * rotatedWidth * rotatedHeight);//new int[rotatedWidth * rotatedHeight];

    for (int h = 0; h < *height; ++h) {
        for (int w = 0; w < *width; ++w) {
            uint32 item = raster[h * *width + w];
            int x = 0;
            int y = 0;
            switch (numberOf90s % 4) {
                case 0:
                    x = w;
                    y = h;
                    break;
                case 1:
                    x = (*height - h - 1);
                    y = (rotatedHeight - 1) - (*width - w - 1);
                    break;
                case 2:
                    x = (*width - w - 1);
                    y = (*height - h - 1);
                    break;
                case 3:
                    x = (rotatedWidth - 1) - (*height - h - 1);
                    y = (*width - w - 1);
                    break;
            }

            rotated[y * rotatedWidth + x] = item;
        }
    }

    *width = rotatedWidth;
    *height = rotatedHeight;

    memcpy(raster, rotated, sizeof(jint) * *width * *height);

    free(rotated);
}

void NativeDecoder::fixOrientation(jint *pixels, uint32 pixelsBufferSize, int bitmapWidth, int bitmapHeight) {
    if (origorientation > 4) {
        unsigned int size = bitmapHeight * bitmapWidth - 1;
        jint t;
        unsigned long long next;
        unsigned long long cycleBegin;
        bool *barray = (bool *) malloc(sizeof(bool) * pixelsBufferSize);
        for (int x = 0; x < size; x++) { barray[x] = false; }
        barray[0] = barray[size] = true;
        unsigned long long k = 1;

        switch (origorientation) {
            case ORIENTATION_LEFTTOP:
            case ORIENTATION_RIGHTBOT:
                while (k < size) {
                    cycleBegin = k;
                    t = pixels[k];
                    do {
                        next = (k * bitmapHeight) % size;
                        jint buf = pixels[next];
                        pixels[next] = t;
                        t = buf;
                        barray[k] = true;
                        k = next;
                    } while (k != cycleBegin);
                    for (k = 1; k < size && barray[k]; k++);
                }
                break;
            case ORIENTATION_LEFTBOT:
            case ORIENTATION_RIGHTTOP:
                while (k < size) {
                    cycleBegin = k;
                    t = pixels[k];
                    do {
                        next = (k * bitmapHeight) % size;
                        jint buf = pixels[next];
                        pixels[next] = t;
                        t = buf;
                        barray[k] = true;
                        k = next;
                    } while (k != cycleBegin);
                    for (k = 1; k < size && barray[k]; k++);
                }
                //flip horizontally
                for (int j = 0, j1 = bitmapWidth - 1; j < bitmapWidth / 2; j++, j1--) {
                    for (int i = 0; i < bitmapHeight; i++) {
                        jint tmp = pixels[j * bitmapHeight + i];
                        pixels[j * bitmapHeight + i] = pixels[j1 * bitmapHeight + i];
                        pixels[j1 * bitmapHeight + i] = tmp;
                    }
                }
                //flip vertically
                for (int i = 0, i1 = bitmapHeight - 1; i < bitmapHeight / 2; i++, i1--) {
                    for (int j = 0; j < bitmapWidth; j++) {
                        jint tmp = pixels[j * bitmapHeight + i];
                        pixels[j * bitmapHeight + i] = pixels[j * bitmapHeight + i1];
                        pixels[j * bitmapHeight + i1] = tmp;
                    }
                }
                break;
        }
        free(barray);
    }
}

jbyte *NativeDecoder::createBitmapAlpha8(jint *raster, int bitmapWidth, int bitmapHeight) {
    jbyte *pixels = nullptr;
    int pixelsBufferSize = bitmapWidth * bitmapHeight;
    pixels = (jbyte *) malloc(sizeof(jbyte) * pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    }

    for (int i = 0; i < bitmapWidth; i++) {
        if (checkStop()) {
            if (pixels) {
                free(pixels);
                pixels = nullptr;
            }
            __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
            return nullptr;
        }
        for (int j = 0; j < bitmapHeight; j++) {
            uint32 crPix = raster[j * bitmapWidth + i];
            int alpha = colorMask & crPix >> 24;
            pixels[j * bitmapWidth + i] = alpha;
        }
    }

    //Close Buffer
    if (raster) {
        _TIFFfree(raster);
        raster = nullptr;
    }

    return pixels;
}

unsigned short *NativeDecoder::createBitmapRGB565(jint *buffer, int bitmapWidth, int bitmapHeight) {
    unsigned short *pixels = nullptr;
    int pixelsBufferSize = bitmapWidth * bitmapHeight;
    pixels = (unsigned short *) malloc(sizeof(unsigned short) * pixelsBufferSize);
    if (pixels == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "Can\'t allocate memory for temp buffer");
        return nullptr;
    }

    for (int i = 0; i < bitmapWidth; i++) {
        if (checkStop()) {
            if (pixels) {
                free(pixels);
                pixels = nullptr;
            }
            __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", "Thread stopped");
            return nullptr;
        }

        for (int j = 0; j < bitmapHeight; j++) {
            jint crPix = buffer[j * bitmapWidth + i];
            int blue = colorMask & crPix >> 16;
            int green = colorMask & crPix >> 8;
            int red = colorMask & crPix;

            unsigned char B = (blue >> 3);
            unsigned char G = (green >> 2);
            unsigned char R = (red >> 3);

            jint curPix = (R << 11) | (G << 5) | B;

            pixels[j * bitmapWidth + i] = curPix;
        }
    }

    //Close Buffer
    if (buffer) {
        _TIFFfree(buffer);
        buffer = nullptr;
    }
    return pixels;
}

int NativeDecoder::getDirectoryCount() {
    int dircount = 0;
    do {
        dircount++;
    } while (TIFFReadDirectory(image));
    return dircount;
}

void NativeDecoder::writeDataToOptions(int directoryNumber) {
    TIFFSetDirectory(image, directoryNumber);
    jfieldID gOptions_outDirectoryCountFieldId = env->GetFieldID(jBitmapOptionsClass, "outDirectoryCount", "I");
    int dircount = getDirectoryCount();
    env->SetIntField(optionsObject, gOptions_outDirectoryCountFieldId, dircount);

    TIFFSetDirectory(image, directoryNumber);
    TIFFGetField(image, TIFFTAG_IMAGEWIDTH, &origwidth);
    TIFFGetField(image, TIFFTAG_IMAGELENGTH, &origheight);

    //Getting image orientation and createing ImageOrientation enum
    TIFFGetField(image, TIFFTAG_ORIENTATION, &origorientation);
    //If orientation field is empty - use ORIENTATION_TOPLEFT
    if (origorientation == 0) {
        origorientation = ORIENTATION_TOPLEFT;
    }
    jclass gOptions_ImageOrientationClass = env->FindClass("org/beyka/tiffbitmapfactory/Orientation");
    jfieldID gOptions_ImageOrientationFieldId = nullptr;
    bool flipHW = false;
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "Orientation", origorientation);
    switch (origorientation) {
        case ORIENTATION_TOPLEFT:
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "TOP_LEFT", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_TOPRIGHT:
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "TOP_RIGHT", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_BOTRIGHT:
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "BOT_RIGHT", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_BOTLEFT:
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "BOT_LEFT", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_LEFTTOP:
            flipHW = true;
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "LEFT_TOP", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_RIGHTTOP:
            flipHW = true;
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "RIGHT_TOP", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_RIGHTBOT:
            flipHW = true;
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "RIGHT_BOT", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
        case ORIENTATION_LEFTBOT:
            flipHW = true;
            gOptions_ImageOrientationFieldId = env->GetStaticFieldID(gOptions_ImageOrientationClass, "LEFT_BOT", "Lorg/beyka/tiffbitmapfactory/Orientation;");
            break;
    }
    if (gOptions_ImageOrientationFieldId != nullptr) {
        jobject gOptions_ImageOrientationObj = env->GetStaticObjectField(gOptions_ImageOrientationClass, gOptions_ImageOrientationFieldId);

        //Set outImageOrientation field to options object
        jfieldID gOptions_outImageOrientationField = env->GetFieldID(jBitmapOptionsClass, "outImageOrientation", "Lorg/beyka/tiffbitmapfactory/Orientation;");
        env->SetObjectField(optionsObject, gOptions_outImageOrientationField, gOptions_ImageOrientationObj);
    }

    //Get resolution variables
    /*
    jfieldID gOptions_outDirectoryCountFieldId = env->GetFieldID(jOptionsClass,
                "outDirectoryCount", "I");
            int dircount = getDirectoryCount();
            env->SetIntField(optionsObject, gOptions_outDirectoryCountFieldId, dircount);
    */
    float xresolution, yresolution;
    uint16 resunit;
    TIFFGetField(image, TIFFTAG_XRESOLUTION, &xresolution);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %f", "xres", xresolution);
    jfieldID gOptions_outXResolutionFieldID = env->GetFieldID(jBitmapOptionsClass, "outXResolution", "F");
    env->SetFloatField(optionsObject, gOptions_outXResolutionFieldID, xresolution);
    TIFFGetField(image, TIFFTAG_YRESOLUTION, &yresolution);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %f", "yres", yresolution);
    jfieldID gOptions_outYResolutionFieldID = env->GetFieldID(jBitmapOptionsClass, "outYResolution", "F");
    env->SetFloatField(optionsObject, gOptions_outYResolutionFieldID, yresolution);
    TIFFGetField(image, TIFFTAG_RESOLUTIONUNIT, &resunit);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "resunit", resunit);
    jclass gOptions_ResolutionUnitClass = env->FindClass("org/beyka/tiffbitmapfactory/ResolutionUnit");
    jfieldID gOptions_ResolutionUnitFieldId = nullptr;
    switch (resunit) {
        case RESUNIT_INCH:
            gOptions_ResolutionUnitFieldId = env->GetStaticFieldID(gOptions_ResolutionUnitClass, "INCH", "Lorg/beyka/tiffbitmapfactory/ResolutionUnit;");
            break;
        case RESUNIT_CENTIMETER:
            gOptions_ResolutionUnitFieldId = env->GetStaticFieldID(gOptions_ResolutionUnitClass, "CENTIMETER", "Lorg/beyka/tiffbitmapfactory/ResolutionUnit;");
            break;
        case RESUNIT_NONE:
        default:
            gOptions_ResolutionUnitFieldId = env->GetStaticFieldID(gOptions_ResolutionUnitClass, "NONE", "Lorg/beyka/tiffbitmapfactory/ResolutionUnit;");
            break;
    }
    if (gOptions_ResolutionUnitFieldId != nullptr) {
        jobject gOptions_ResolutionUnitObj = env->GetStaticObjectField(gOptions_ResolutionUnitClass, gOptions_ResolutionUnitFieldId);

        //Set resolution unit field to options object
        jfieldID gOptions_outResUnitField = env->GetFieldID(jBitmapOptionsClass, "outResolutionUnit", "Lorg/beyka/tiffbitmapfactory/ResolutionUnit;");
        env->SetObjectField(optionsObject, gOptions_outResUnitField, gOptions_ResolutionUnitObj);
    }

    //Get image planar config
    int planarConfig = 0;
    TIFFGetField(image, TIFFTAG_PLANARCONFIG, &planarConfig);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "planar config", planarConfig);
    jclass gOptions_PlanarConfigClass = env->FindClass("org/beyka/tiffbitmapfactory/PlanarConfig");
    jfieldID gOptions_PlanarConfigFieldId = nullptr;
    switch (planarConfig) {
        case PLANARCONFIG_CONTIG:
            gOptions_PlanarConfigFieldId = env->GetStaticFieldID(gOptions_PlanarConfigClass, "CONTIG", "Lorg/beyka/tiffbitmapfactory/PlanarConfig;");
            break;
        case PLANARCONFIG_SEPARATE:
            gOptions_PlanarConfigFieldId = env->GetStaticFieldID(gOptions_PlanarConfigClass, "SEPARATE", "Lorg/beyka/tiffbitmapfactory/PlanarConfig;");
            break;
    }
    if (gOptions_PlanarConfigFieldId != nullptr) {
        jobject gOptions_PlanarConfigObj = env->GetStaticObjectField(gOptions_PlanarConfigClass, gOptions_PlanarConfigFieldId);

        jfieldID gOptions_outPlanarConfigField = env->GetFieldID(jBitmapOptionsClass, "outPlanarConfig", "Lorg/beyka/tiffbitmapfactory/PlanarConfig;");
        env->SetObjectField(optionsObject, gOptions_outPlanarConfigField, gOptions_PlanarConfigObj);
    }

    //Getting image compression scheme and createing CompressionScheme enum
    TIFFGetField(image, TIFFTAG_COMPRESSION, &origcompressionscheme);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "compression", origcompressionscheme);

    jclass gOptions_ImageCompressionClass = env->FindClass("org/beyka/tiffbitmapfactory/CompressionScheme");
    jfieldID gOptions_ImageCompressionFieldId = nullptr;
    switch (origcompressionscheme) {
        case COMPRESSION_NONE:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "NONE", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_CCITTRLE:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "CCITTRLE", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_CCITTFAX3:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "CCITTFAX3", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_CCITTFAX4:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "CCITTFAX4", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_LZW:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "LZW", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_JPEG:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "JPEG", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_PACKBITS:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "PACKBITS", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_DEFLATE:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "DEFLATE", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        case COMPRESSION_ADOBE_DEFLATE:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "ADOBE_DEFLATE", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
        default:
            gOptions_ImageCompressionFieldId = env->GetStaticFieldID(gOptions_ImageCompressionClass, "OTHER", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
            break;
    }
    if (gOptions_ImageCompressionFieldId != nullptr) {
        jobject gOptions_ImageCompressionObj = env->GetStaticObjectField(gOptions_ImageCompressionClass, gOptions_ImageCompressionFieldId);

        //Set outImageOrientation field to options object
        jfieldID gOptions_outCompressionSchemeField = env->GetFieldID(jBitmapOptionsClass, "outCompressionScheme", "Lorg/beyka/tiffbitmapfactory/CompressionScheme;");
        env->SetObjectField(optionsObject, gOptions_outCompressionSchemeField,
                            gOptions_ImageCompressionObj);
    }

    jfieldID gOptions_OutCurDirNumberFieldID = env->GetFieldID(jBitmapOptionsClass, "outCurDirectoryNumber", "I");
    env->SetIntField(optionsObject, gOptions_OutCurDirNumberFieldID, directoryNumber);
    if (!flipHW) {
        jfieldID gOptions_outWidthFieldId = env->GetFieldID(jBitmapOptionsClass, "outWidth", "I");
        env->SetIntField(optionsObject, gOptions_outWidthFieldId, origwidth);

        jfieldID gOptions_outHeightFieldId = env->GetFieldID(jBitmapOptionsClass, "outHeight", "I");
        env->SetIntField(optionsObject, gOptions_outHeightFieldId, origheight);
    } else {
        jfieldID gOptions_outWidthFieldId = env->GetFieldID(jBitmapOptionsClass, "outWidth", "I");
        env->SetIntField(optionsObject, gOptions_outWidthFieldId, origheight);

        jfieldID gOptions_outHeightFieldId = env->GetFieldID(jBitmapOptionsClass, "outHeight", "I");
        env->SetIntField(optionsObject, gOptions_outHeightFieldId, origwidth);
    }

    int tagRead = 0;

    int bitPerSample = 0;
    tagRead = TIFFGetField(image, TIFFTAG_BITSPERSAMPLE, &bitPerSample);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "bit per sample", bitPerSample);
        jfieldID gOptions_outBitPerSampleFieldID = env->GetFieldID(jBitmapOptionsClass, "outBitsPerSample", "I");
        env->SetIntField(optionsObject, gOptions_outBitPerSampleFieldID, bitPerSample);
    }

    int samplePerPixel = 0;
    tagRead = TIFFGetField(image, TIFFTAG_SAMPLESPERPIXEL, &samplePerPixel);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "sample per pixel", samplePerPixel);
        jfieldID gOptions_outSamplePerPixelFieldID = env->GetFieldID(jBitmapOptionsClass, "outSamplePerPixel", "I");
        env->SetIntField(optionsObject, gOptions_outSamplePerPixelFieldID, samplePerPixel);
    }

    //Tile size
    int tileWidth = 0;
    tagRead = TIFFGetField(image, TIFFTAG_TILEWIDTH, &tileWidth);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "tile width", tileWidth);
        jfieldID gOptions_outTileWidthFieldID = env->GetFieldID(jBitmapOptionsClass, "outTileWidth", "I");
        env->SetIntField(optionsObject, gOptions_outTileWidthFieldID, tileWidth);
    }
    int tileHeight = 0;
    tagRead = TIFFGetField(image, TIFFTAG_TILELENGTH, &tileHeight);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "tile height", tileHeight);
        jfieldID gOptions_outTileHeightFieldID = env->GetFieldID(jBitmapOptionsClass, "outTileHeight", "I");
        env->SetIntField(optionsObject, gOptions_outTileHeightFieldID, tileHeight);
    }

    //row per strip
    int rowPerStrip = 0;
    tagRead = TIFFGetField(image, TIFFTAG_ROWSPERSTRIP, &rowPerStrip);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "row per strip", rowPerStrip);
        jfieldID gOptions_outRowPerStripFieldID = env->GetFieldID(jBitmapOptionsClass, "outRowPerStrip", "I");
        env->SetIntField(optionsObject, gOptions_outRowPerStripFieldID, rowPerStrip);
    }

    //strip size
    uint32 stripSize = TIFFStripSize(image);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "strip size", stripSize);
    jfieldID gOptions_outStripSizeFieldID = env->GetFieldID(jBitmapOptionsClass, "outStripSize", "I");
    env->SetIntField(optionsObject, gOptions_outStripSizeFieldID, stripSize);

    //strip max
    uint32 stripMax = TIFFNumberOfStrips(image);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "number of strips", stripMax);
    jfieldID gOptions_outStripMaxFieldID = env->GetFieldID(jBitmapOptionsClass, "outNumberOfStrips", "I");
    env->SetIntField(optionsObject, gOptions_outStripMaxFieldID, stripMax);

    //photometric
    int photometric = 0;
    TIFFGetField(image, TIFFTAG_PHOTOMETRIC, &photometric);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "photometric", photometric);
    jclass gOptions_PhotometricClass = env->FindClass("org/beyka/tiffbitmapfactory/Photometric");
    jfieldID gOptions_PhotometricFieldId = nullptr;
    switch (photometric) {
        case PHOTOMETRIC_MINISWHITE:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "MINISWHITE", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_MINISBLACK:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "MINISBLACK", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_RGB:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "RGB", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_PALETTE:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "PALETTE", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_MASK:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "MASK", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_SEPARATED:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "SEPARATED", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_YCBCR:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "YCBCR", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_CIELAB:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "CIELAB", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_ICCLAB:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "ICCLAB", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_ITULAB:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "ITULAB", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_LOGL:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "LOGL", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        case PHOTOMETRIC_LOGLUV:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "LOGLUV", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
        default:
            gOptions_PhotometricFieldId = env->GetStaticFieldID(gOptions_PhotometricClass, "OTHER", "Lorg/beyka/tiffbitmapfactory/Photometric;");
            break;
    }
    if (gOptions_PhotometricFieldId != nullptr) {
        jobject gOptions_PhotometricObj = env->GetStaticObjectField(
                gOptions_PhotometricClass,
                gOptions_PhotometricFieldId);

        jfieldID gOptions_outPhotometricField = env->GetFieldID(jBitmapOptionsClass, "outPhotometric", "Lorg/beyka/tiffbitmapfactory/Photometric;");
        env->SetObjectField(optionsObject, gOptions_outPhotometricField, gOptions_PhotometricObj);
    }

    //FillOrder
    int fillOrder = 0;
    TIFFGetField(image, TIFFTAG_FILLORDER, &fillOrder);
    __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s %d", "fill Order", fillOrder);
    jclass gOptions_FillOrderClass = env->FindClass("org/beyka/tiffbitmapfactory/FillOrder");
    jfieldID gOptions_FillOrderFieldId = nullptr;
    switch (fillOrder) {
        case FILLORDER_MSB2LSB:
            gOptions_FillOrderFieldId = env->GetStaticFieldID(gOptions_FillOrderClass, "MSB2LSB", "Lorg/beyka/tiffbitmapfactory/FillOrder;");
            break;
        case FILLORDER_LSB2MSB:
            gOptions_FillOrderFieldId = env->GetStaticFieldID(gOptions_FillOrderClass, "LSB2MSB", "Lorg/beyka/tiffbitmapfactory/FillOrder;");
            break;
    }
    if (gOptions_FillOrderFieldId != nullptr) {
        jobject gOptions_FillOrderObj = env->GetStaticObjectField(gOptions_FillOrderClass, gOptions_FillOrderFieldId);

        jfieldID gOptions_outFillOrderField = env->GetFieldID(jBitmapOptionsClass, "outFillOrder", "Lorg/beyka/tiffbitmapfactory/FillOrder;");
        env->SetObjectField(optionsObject, gOptions_outFillOrderField, gOptions_FillOrderObj);
    }

    //Author
    const char *artist;
    tagRead = TIFFGetField(image, TIFFTAG_ARTIST, &artist);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", artist);
        jstring jauthor = charsToJString(artist);//env->NewStringUTF(artist);
        jfieldID gOptions_outAuthorFieldId = env->GetFieldID(jBitmapOptionsClass, "outAuthor", "Ljava/lang/String;");
        env->SetObjectField(optionsObject, gOptions_outAuthorFieldId, jauthor);
        env->DeleteLocalRef(jauthor);
        //free(artist);
    }

    //Copyright
    const char *copyright;
    tagRead = TIFFGetField(image, TIFFTAG_COPYRIGHT, &copyright);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", copyright);
        jstring jcopyright = charsToJString(copyright);//env->NewStringUTF(copyright);
        jfieldID gOptions_outCopyrightFieldId = env->GetFieldID(jBitmapOptionsClass, "outCopyright", "Ljava/lang/String;");
        env->SetObjectField(optionsObject, gOptions_outCopyrightFieldId, jcopyright);
        env->DeleteLocalRef(jcopyright);
        //free(copyright);
    }

    //ImageDescription
    const char *imgDescr;
    tagRead = TIFFGetField(image, TIFFTAG_IMAGEDESCRIPTION, &imgDescr);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", imgDescr);
        jstring jimgDescr = charsToJString(imgDescr);//env->NewStringUTF(imgDescr);
        jfieldID gOptions_outimgDescrFieldId = env->GetFieldID(jBitmapOptionsClass, "outImageDescription", "Ljava/lang/String;");
        env->SetObjectField(optionsObject, gOptions_outimgDescrFieldId, jimgDescr);
        env->DeleteLocalRef(jimgDescr);
        //free(imgDescr);
    }

    //Software
    const char *software;
    tagRead = TIFFGetField(image, TIFFTAG_SOFTWARE, &software);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", software);
        jstring jsoftware = charsToJString(software);//env->NewStringUTF(software);
        jfieldID gOptions_outsoftwareFieldId = env->GetFieldID(jBitmapOptionsClass, "outSoftware", "Ljava/lang/String;");
        env->SetObjectField(optionsObject, gOptions_outsoftwareFieldId, jsoftware);
        env->DeleteLocalRef(jsoftware);
        //free(software);
    }

    //DateTime
    const char *datetime;
    tagRead = TIFFGetField(image, TIFFTAG_DATETIME, &datetime);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", datetime);
        jstring jdatetime = charsToJString(datetime);//env->NewStringUTF(datetime);
        jfieldID gOptions_outdatetimeFieldId = env->GetFieldID(jBitmapOptionsClass, "outDatetime", "Ljava/lang/String;");
        env->SetObjectField(optionsObject, gOptions_outdatetimeFieldId, jdatetime);
        env->DeleteLocalRef(jdatetime);
        //free(datetime);
    }

    //Host Computer
    const char *host;
    tagRead = TIFFGetField(image, TIFFTAG_HOSTCOMPUTER, &host);
    if (tagRead == 1) {
        __android_log_print(ANDROID_LOG_DEBUG, "NativeTiffDecoder", "%s", host);
        jstring jhost = charsToJString(host);//env->NewStringUTF(host);
        jfieldID gOptions_outhostFieldId = env->GetFieldID(jBitmapOptionsClass, "outHostComputer", "Ljava/lang/String;");
        env->SetObjectField(optionsObject, gOptions_outhostFieldId, jhost);
        env->DeleteLocalRef(jhost);
        //free(host);
    }
}

jstring NativeDecoder::charsToJString(const char *chars) {
    std::string str(chars);
    jbyteArray array = env->NewByteArray(str.size());
    env->SetByteArrayRegion(array, 0, str.size(), (const jbyte *) str.c_str());
    jstring strEncode = env->NewStringUTF("UTF-8");
    jclass cls = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "([BLjava/lang/String;)V");
    auto object = (jstring) env->NewObject(cls, ctor, array, strEncode);
    return object;
    //return nullptr;
}

jboolean NativeDecoder::checkStop() {
    jmethodID methodID = env->GetStaticMethodID(jThreadClass, "interrupted", "()Z");
    jboolean interupted = env->CallStaticBooleanMethod(jThreadClass, methodID);

    jboolean stop;
    if (optionsObject) {
        jfieldID stopFieldId = env->GetFieldID(jBitmapOptionsClass, "isStoped", "Z");
        stop = env->GetBooleanField(optionsObject, stopFieldId);
    } else {
        stop = JNI_FALSE;
    }

    return interupted || stop;
}

void NativeDecoder::sendProgress(jlong current, jlong total) {
    if (listenerObject != nullptr) {
        jmethodID methodid = env->GetMethodID(jIProgressListenerClass, "reportProgress", "(JJ)V");
        env->CallVoidMethod(listenerObject, methodid, current, total);
    }
}

void NativeDecoder::tileErrorHandler(int code, siginfo_t *siginfo, void *sc) {
    __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "tileErrorHandler");
    longjmp(tile_buf, 1);
}

void NativeDecoder::stripErrorHandler(int code, siginfo_t *siginfo, void *sc) {
    __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "stripErrorHandler");
    longjmp(strip_buf, 1);
}

void NativeDecoder::imageErrorHandler(int code, siginfo_t *siginfo, void *sc) {
    __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "imageErrorHandler");
    longjmp(image_buf, 1);
}

void NativeDecoder::generalErrorHandler(int code, siginfo_t *siginfo, void *sc) {
    __android_log_print(ANDROID_LOG_ERROR, "NativeTiffDecoder", "generalErrorHandler");
    longjmp(general_buf, 1);
}

void NativeDecoder::throwDecodeFileException(const char *message) {
    jstring adinf = env->NewStringUTF(message);
    if (decodingMode == DECODE_MODE_FILE_PATH) {
        throw_decode_file_exception(env, jPath, adinf);
    } else if (decodingMode == DECODE_MODE_FILE_DESCRIPTOR) {
        throw_decode_file_exception_fd(env, jFd, adinf);
    }
    env->DeleteLocalRef(adinf);
}

void NativeDecoder::throwCantOpenFileException() {
    if (decodingMode == DECODE_MODE_FILE_PATH) {
        throw_cant_open_file_exception(env, jPath);
    } else if (decodingMode == DECODE_MODE_FILE_DESCRIPTOR) {
        throw_cant_open_file_exception_fd(env, jFd);
    }
}
