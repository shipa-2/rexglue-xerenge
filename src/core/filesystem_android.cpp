/**
 * @file        rex/core/filesystem_android.cpp
 * @brief       Opening content:// URIs (the Storage Access Framework) on Android.
 *
 * A file the user picks through the system file picker is not a path but a
 * content URI; the only way in is ContentResolver.openFileDescriptor, from
 * Java. SDL keeps the JNI environment and the activity for us.
 */

#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <jni.h>

#include <string>

#include <SDL3/SDL_system.h>

#include <rex/filesystem.h>

namespace rex::filesystem {

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return -1;
  }
  int fd = -1;
  if (env->PushLocalFrame(16) != JNI_OK) {
    env->DeleteLocalRef(activity);
    return -1;
  }
  do {
    jclass context_class = env->GetObjectClass(activity);
    jmethodID get_resolver = env->GetMethodID(context_class, "getContentResolver",
                                              "()Landroid/content/ContentResolver;");
    jobject resolver = get_resolver ? env->CallObjectMethod(activity, get_resolver) : nullptr;
    if (env->ExceptionCheck() || !resolver) {
      break;
    }
    jclass uri_class = env->FindClass("android/net/Uri");
    jmethodID parse = uri_class ? env->GetStaticMethodID(uri_class, "parse",
                                                         "(Ljava/lang/String;)Landroid/net/Uri;")
                                : nullptr;
    if (!parse) {
      break;
    }
    const std::string uri_string(uri);
    jobject uri_object =
        env->CallStaticObjectMethod(uri_class, parse, env->NewStringUTF(uri_string.c_str()));
    if (env->ExceptionCheck() || !uri_object) {
      break;
    }
    jmethodID open = env->GetMethodID(
        env->GetObjectClass(resolver), "openFileDescriptor",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jobject descriptor =
        open ? env->CallObjectMethod(resolver, open, uri_object, env->NewStringUTF(mode)) : nullptr;
    if (env->ExceptionCheck() || !descriptor) {
      break;
    }
    // detachFd hands the descriptor over: closing the ParcelFileDescriptor no
    // longer closes it, and it is ours to close.
    jmethodID detach = env->GetMethodID(env->GetObjectClass(descriptor), "detachFd", "()I");
    if (detach) {
      fd = env->CallIntMethod(descriptor, detach);
    }
  } while (false);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    fd = -1;
  }
  env->PopLocalFrame(nullptr);
  env->DeleteLocalRef(activity);
  return fd;
}

}  // namespace rex::filesystem

#endif  // REX_PLATFORM_ANDROID
