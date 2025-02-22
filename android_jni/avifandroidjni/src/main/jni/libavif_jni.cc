// Copyright 2022 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

#include <android/bitmap.h>
#include <android/log.h>
#include <cpu-features.h>
#include <jni.h>

#include <cstdio>
#include <memory>
#include <new>

#include "avif/avif.h"

#define LOG_TAG "avif_jni"
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define FUNC(RETURN_TYPE, NAME, ...)                                      \
  extern "C" {                                                            \
  JNIEXPORT RETURN_TYPE Java_org_aomedia_avif_android_AvifDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);                          \
  }                                                                       \
  JNIEXPORT RETURN_TYPE Java_org_aomedia_avif_android_AvifDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

namespace {

// RAII wrapper class that properly frees the decoder related objects on
// destruction.
struct AvifDecoderWrapper {
 public:
  AvifDecoderWrapper() = default;
  // Not copyable or movable.
  AvifDecoderWrapper(const AvifDecoderWrapper&) = delete;
  AvifDecoderWrapper& operator=(const AvifDecoderWrapper&) = delete;

  ~AvifDecoderWrapper() {
    if (decoder != nullptr) {
      avifDecoderDestroy(decoder);
    }
  }

  avifDecoder* decoder = nullptr;
};

bool CreateDecoderAndParse(AvifDecoderWrapper* const decoder,
                           const uint8_t* const buffer, int length,
                           int threads) {
  decoder->decoder = avifDecoderCreate();
  if (decoder->decoder == nullptr) {
    LOGE("Failed to create AVIF Decoder.");
    return false;
  }
  decoder->decoder->maxThreads = threads;
  decoder->decoder->ignoreXMP = AVIF_TRUE;
  decoder->decoder->ignoreExif = AVIF_TRUE;

  // Turn off 'clap' (clean aperture) property validation. The JNI wrapper
  // ignores the 'clap' property.
  decoder->decoder->strictFlags &= ~AVIF_STRICT_CLAP_VALID;
  // Allow 'pixi' (pixel information) property to be missing. Older versions of
  // libheif did not add the 'pixi' item property to AV1 image items (See
  // crbug.com/1198455).
  decoder->decoder->strictFlags &= ~AVIF_STRICT_PIXI_REQUIRED;

  avifResult res = avifDecoderSetIOMemory(decoder->decoder, buffer, length);
  if (res != AVIF_RESULT_OK) {
    LOGE("Failed to set AVIF IO to a memory reader.");
    return false;
  }
  res = avifDecoderParse(decoder->decoder);
  if (res != AVIF_RESULT_OK) {
    LOGE("Failed to parse AVIF image: %s.", avifResultToString(res));
    return false;
  }
  return true;
}

avifResult AvifImageToBitmap(JNIEnv* const env,
                             AvifDecoderWrapper* const decoder,
                             jobject bitmap) {
  AndroidBitmapInfo bitmap_info;
  if (AndroidBitmap_getInfo(env, bitmap, &bitmap_info) < 0) {
    LOGE("AndroidBitmap_getInfo failed.");
    return AVIF_RESULT_UNKNOWN_ERROR;
  }
  // Ensure that the bitmap is large enough to store the decoded image.
  if (bitmap_info.width < decoder->decoder->image->width ||
      bitmap_info.height < decoder->decoder->image->height) {
    LOGE(
        "Bitmap is not large enough to fit the image. Bitmap %dx%d Image "
        "%dx%d.",
        bitmap_info.width, bitmap_info.height, decoder->decoder->image->width,
        decoder->decoder->image->height);
    return AVIF_RESULT_UNKNOWN_ERROR;
  }
  // Ensure that the bitmap format is RGBA_8888, RGB_565 or RGBA_F16.
  if (bitmap_info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 &&
      bitmap_info.format != ANDROID_BITMAP_FORMAT_RGB_565 &&
      bitmap_info.format != ANDROID_BITMAP_FORMAT_RGBA_F16) {
    LOGE("Bitmap format (%d) is not supported.", bitmap_info.format);
    return AVIF_RESULT_NOT_IMPLEMENTED;
  }
  void* bitmap_pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &bitmap_pixels) !=
      ANDROID_BITMAP_RESULT_SUCCESS) {
    LOGE("Failed to lock Bitmap.");
    return AVIF_RESULT_UNKNOWN_ERROR;
  }
  avifRGBImage rgb_image;
  avifRGBImageSetDefaults(&rgb_image, decoder->decoder->image);
  if (bitmap_info.format == ANDROID_BITMAP_FORMAT_RGBA_F16) {
    rgb_image.depth = 16;
    rgb_image.isFloat = AVIF_TRUE;
  } else if (bitmap_info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
    rgb_image.format = AVIF_RGB_FORMAT_RGB_565;
    rgb_image.depth = 8;
  } else {
    rgb_image.depth = 8;
  }
  rgb_image.pixels = static_cast<uint8_t*>(bitmap_pixels);
  rgb_image.rowBytes = bitmap_info.stride;
  // Android always sees the Bitmaps as premultiplied with alpha when it renders
  // them:
  // https://developer.android.com/reference/android/graphics/Bitmap#setPremultiplied(boolean)
  rgb_image.alphaPremultiplied = AVIF_TRUE;
  const avifResult res = avifImageYUVToRGB(decoder->decoder->image, &rgb_image);
  AndroidBitmap_unlockPixels(env, bitmap);
  if (res != AVIF_RESULT_OK) {
    LOGE("Failed to convert YUV Pixels to RGB. Status: %d", res);
    return res;
  }
  return AVIF_RESULT_OK;
}

avifResult DecodeNextImage(JNIEnv* const env, AvifDecoderWrapper* const decoder,
                           jobject bitmap) {
  avifResult res = avifDecoderNextImage(decoder->decoder);
  if (res != AVIF_RESULT_OK) {
    LOGE("Failed to decode AVIF image. Status: %d", res);
    return res;
  }
  return AvifImageToBitmap(env, decoder, bitmap);
}

avifResult DecodeNthImage(JNIEnv* const env, AvifDecoderWrapper* const decoder,
                          uint32_t n, jobject bitmap) {
  avifResult res = avifDecoderNthImage(decoder->decoder, n);
  if (res != AVIF_RESULT_OK) {
    LOGE("Failed to decode AVIF image. Status: %d", res);
    return res;
  }
  return AvifImageToBitmap(env, decoder, bitmap);
}

int getThreadCount(int threads) {
  return (threads == 0) ? android_getCpuCount() : threads;
}

}  // namespace

jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return -1;
  }
  return JNI_VERSION_1_6;
}

FUNC(jboolean, isAvifImage, jobject encoded, int length) {
  const uint8_t* const buffer =
      static_cast<const uint8_t*>(env->GetDirectBufferAddress(encoded));
  const avifROData avif = {buffer, static_cast<size_t>(length)};
  return avifPeekCompatibleFileType(&avif);
}

FUNC(jboolean, getInfo, jobject encoded, int length, jobject info) {
  const uint8_t* const buffer =
      static_cast<const uint8_t*>(env->GetDirectBufferAddress(encoded));
  AvifDecoderWrapper decoder;
  if (!CreateDecoderAndParse(&decoder, buffer, length, /*threads=*/1)) {
    return false;
  }
  const jclass info_class =
      env->FindClass("org/aomedia/avif/android/AvifDecoder$Info");
  const jfieldID width = env->GetFieldID(info_class, "width", "I");
  const jfieldID height = env->GetFieldID(info_class, "height", "I");
  const jfieldID depth = env->GetFieldID(info_class, "depth", "I");
  const jfieldID alpha_present =
      env->GetFieldID(info_class, "alphaPresent", "Z");
  env->SetIntField(info, width, decoder.decoder->image->width);
  env->SetIntField(info, height, decoder.decoder->image->height);
  env->SetIntField(info, depth, decoder.decoder->image->depth);
  env->SetBooleanField(info, alpha_present, decoder.decoder->alphaPresent);
  return true;
}

FUNC(jboolean, decode, jobject encoded, int length, jobject bitmap,
     jint threads) {
  if (threads < 0) {
    LOGE("Invalid value for threads (%d).", threads);
    return false;
  }
  const uint8_t* const buffer =
      static_cast<const uint8_t*>(env->GetDirectBufferAddress(encoded));
  AvifDecoderWrapper decoder;
  if (!CreateDecoderAndParse(&decoder, buffer, length,
                             getThreadCount(threads))) {
    return false;
  }
  return DecodeNextImage(env, &decoder, bitmap) == AVIF_RESULT_OK;
}

FUNC(jlong, createDecoder, jobject encoded, jint length, jint threads) {
  if (threads < 0) {
    LOGE("Invalid value for threads (%d).", threads);
    return 0;
  }
  const uint8_t* const buffer =
      static_cast<const uint8_t*>(env->GetDirectBufferAddress(encoded));
  std::unique_ptr<AvifDecoderWrapper> decoder(new (std::nothrow)
                                                  AvifDecoderWrapper());
  if (decoder == nullptr) {
    return 0;
  }
  if (!CreateDecoderAndParse(decoder.get(), buffer, length,
                             getThreadCount(threads))) {
    return 0;
  }
  const jclass avif_decoder_class =
      env->FindClass("org/aomedia/avif/android/AvifDecoder");
  const jfieldID width_id = env->GetFieldID(avif_decoder_class, "width", "I");
  const jfieldID height_id = env->GetFieldID(avif_decoder_class, "height", "I");
  const jfieldID depth_id = env->GetFieldID(avif_decoder_class, "depth", "I");
  const jfieldID alpha_present_id =
      env->GetFieldID(avif_decoder_class, "alphaPresent", "Z");
  const jfieldID frame_count_id =
      env->GetFieldID(avif_decoder_class, "frameCount", "I");
  const jfieldID repetition_count_id =
      env->GetFieldID(avif_decoder_class, "repetitionCount", "I");
  const jfieldID frame_durations_id =
      env->GetFieldID(avif_decoder_class, "frameDurations", "[D");
  env->SetIntField(thiz, width_id, decoder->decoder->image->width);
  env->SetIntField(thiz, height_id, decoder->decoder->image->height);
  env->SetIntField(thiz, depth_id, decoder->decoder->image->depth);
  env->SetBooleanField(thiz, alpha_present_id, decoder->decoder->alphaPresent);
  env->SetIntField(thiz, repetition_count_id,
                   decoder->decoder->repetitionCount);
  const int frameCount = decoder->decoder->imageCount;
  env->SetIntField(thiz, frame_count_id, frameCount);
  // This native array is needed because setting one element at a time to a Java
  // array from the JNI layer is inefficient.
  std::unique_ptr<double[]> native_durations(
      new (std::nothrow) double[frameCount]);
  if (native_durations == nullptr) {
    return 0;
  }
  for (int i = 0; i < frameCount; ++i) {
    avifImageTiming timing;
    if (avifDecoderNthImageTiming(decoder->decoder, i, &timing) !=
        AVIF_RESULT_OK) {
      return 0;
    }
    native_durations[i] = timing.duration;
  }
  jdoubleArray durations = env->NewDoubleArray(frameCount);
  if (durations == nullptr) {
    return 0;
  }
  env->SetDoubleArrayRegion(durations, /*start=*/0, frameCount,
                            native_durations.get());
  env->SetObjectField(thiz, frame_durations_id, durations);
  return reinterpret_cast<jlong>(decoder.release());
}

FUNC(jint, nextFrame, jlong jdecoder, jobject bitmap) {
  AvifDecoderWrapper* const decoder =
      reinterpret_cast<AvifDecoderWrapper*>(jdecoder);
  return DecodeNextImage(env, decoder, bitmap);
}

FUNC(jint, nextFrameIndex, jlong jdecoder) {
  AvifDecoderWrapper* const decoder =
      reinterpret_cast<AvifDecoderWrapper*>(jdecoder);
  return decoder->decoder->imageIndex + 1;
}

FUNC(jint, nthFrame, jlong jdecoder, jint n, jobject bitmap) {
  AvifDecoderWrapper* const decoder =
      reinterpret_cast<AvifDecoderWrapper*>(jdecoder);
  return DecodeNthImage(env, decoder, n, bitmap);
}

FUNC(jstring, resultToString, jint result) {
  return env->NewStringUTF(avifResultToString(static_cast<avifResult>(result)));
}

FUNC(jstring, versionString) {
  char codec_versions[256];
  avifCodecVersions(codec_versions);
  char version_string[512];
  snprintf(version_string, sizeof(version_string),
           "libavif: %s. Codecs: %s. libyuv: %d.", avifVersion(),
           codec_versions, avifLibYUVVersion());
  return env->NewStringUTF(version_string);
}

FUNC(void, destroyDecoder, jlong jdecoder) {
  AvifDecoderWrapper* const decoder =
      reinterpret_cast<AvifDecoderWrapper*>(jdecoder);
  delete decoder;
}
