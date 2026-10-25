#ifndef ANDROID_LOG_H
#define ANDROID_LOG_H
#define ANDROID_LOG_INFO 4
static inline int __android_log_write(int p, const char *t, const char *m) {(void)p;(void)t;(void)m;return 0;}
#endif
