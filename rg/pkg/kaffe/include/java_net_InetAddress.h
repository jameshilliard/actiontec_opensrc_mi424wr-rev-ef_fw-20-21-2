/* DO NOT EDIT THIS FILE - it is machine generated */
#include <native.h>

#ifndef _Included_java_net_InetAddress
#define _Included_java_net_InetAddress

#ifdef __cplusplus
extern "C" {
#endif

/* Header for class java_net_InetAddress */

typedef struct Hjava_net_InetAddress {
  /* Fields from java/lang/Object: */
  Hjava_lang_Object base;

  /* Fields from java/net/InetAddress: */
#undef java_net_InetAddress_serialVersionUID
#define java_net_InetAddress_serialVersionUID 3286316764910316507LL
#undef java_net_InetAddress_DEFAULT_CACHE_SIZE
#define java_net_InetAddress_DEFAULT_CACHE_SIZE 89L
#undef java_net_InetAddress_DEFAULT_CACHE_PERIOD
#define java_net_InetAddress_DEFAULT_CACHE_PERIOD 240L
#undef java_net_InetAddress_DEFAULT_CACHE_PURGE_PCT
#define java_net_InetAddress_DEFAULT_CACHE_PURGE_PCT 30L
#undef java_net_InetAddress_LOCALHOST_NAME
#define java_net_InetAddress_LOCALHOST_NAME "localhost"
  jint address;
  HArrayOfByte* addr;
  struct Hjava_lang_String* hostName;
  jlong lookup_time;
  jint family;
} Hjava_net_InetAddress;


#ifdef __cplusplus
}
#endif

#endif
