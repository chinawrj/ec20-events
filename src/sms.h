#ifndef EC20_SMS_H
#define EC20_SMS_H
#include <stddef.h>
int sms_decode(const char *hex, char *out, size_t cap);
#endif
