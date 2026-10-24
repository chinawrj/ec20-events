#include "sms.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const uint16_t gsm[128] = {
    64,  163, 36,  165, 232, 233, 249, 236, 242, 199, 10,  216, 248, 13,  197,
    229, 916, 95,  934, 915, 923, 937, 928, 936, 931, 920, 926, 27,  198, 230,
    223, 201, 32,  33,  34,  35,  164, 37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  161, 65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
    75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    90,  196, 214, 209, 220, 167, 191, 97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 228, 246, 241, 252, 224};
static int digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static unsigned extension(unsigned c) {
  switch (c) {
  case 10:
    return 12;
  case 20:
    return '^';
  case 40:
    return '{';
  case 41:
    return '}';
  case 47:
    return '\\';
  case 60:
    return '[';
  case 61:
    return '~';
  case 62:
    return ']';
  case 64:
    return '|';
  case 101:
    return 8364;
  default:
    return 0xfffd;
  }
}
static int utf(char *out, size_t cap, size_t *n, unsigned u) {
  unsigned char b[4];
  size_t k;
  if (u == 0)
    u = 0xfffd; /* no embedded NUL in notifications */
  if (u < 128) {
    b[0] = u;
    k = 1;
  } else if (u < 2048) {
    b[0] = 192 + (u >> 6);
    b[1] = 128 + (u & 63);
    k = 2;
  } else if (u < 65536) {
    b[0] = 224 + (u >> 12);
    b[1] = 128 + ((u >> 6) & 63);
    b[2] = 128 + (u & 63);
    k = 3;
  } else {
    b[0] = 240 + (u >> 18);
    b[1] = 128 + ((u >> 12) & 63);
    b[2] = 128 + ((u >> 6) & 63);
    b[3] = 128 + (u & 63);
    k = 4;
  }
  if (*n + k >= cap)
    return -1;
  memcpy(out + *n, b, k);
  *n += k;
  out[*n] = 0;
  return 0;
}
static int unpack(const unsigned char *b, size_t bytes, size_t bits,
                  size_t count, char *out, size_t cap, size_t *n) {
  int esc = 0;
  for (size_t i = 0; i < count; i++) {
    size_t bit = bits + 7 * i;
    if (bit + 7 > bytes * 8)
      return -1;
    size_t j = bit / 8;
    unsigned v = b[j];
    if (j + 1 < bytes)
      v |= (unsigned)b[j + 1] << 8;
    v = (v >> (bit % 8)) & 127;
    if (esc) {
      if (utf(out, cap, n, extension(v)))
        return -1;
      esc = 0;
    } else if (v == 27)
      esc = 1;
    else if (utf(out, cap, n, gsm[v]))
      return -1;
  }
  return esc ? utf(out, cap, n, 0xfffd) : 0;
}
int sms_decode(const char *hex, char *out, size_t cap) {
  unsigned char b[512];
  size_t len = strlen(hex), n = 0, p;
  if (!cap)
    return -1;
  out[0] = 0;
  if (!len || len % 2 || len > sizeof(b) * 2)
    return -1;
  for (size_t i = 0; i < len / 2; i++) {
    int a = digit(hex[2 * i]), c = digit(hex[2 * i + 1]);
    if (a < 0 || c < 0)
      return -1;
    b[i] = (unsigned char)(a * 16 + c);
  }
  len /= 2;
  p = 1 + b[0];
  if (p + 3 > len)
    return -1;
  unsigned first = b[p++], count = b[p++], toa = b[p++];
  if ((first & 3) != 0)
    return -1;
  size_t address = (count + 1) / 2;
  if (count > 40 || p + address + 10 > len)
    return -1;
  int written = snprintf(out, cap, "From: ");
  if (written < 0 || (size_t)written >= cap)
    return -1;
  n = (size_t)written;
  if (((toa >> 4) & 7) == 5) {
    if (unpack(b + p, address, 0, count * 4 / 7, out, cap, &n))
      return -1;
  } else {
    if (((toa >> 4) & 7) == 1 && utf(out, cap, &n, '+'))
      return -1;
    for (unsigned i = 0; i < count; i++) {
      unsigned v = (b[p + i / 2] >> (4 * (i % 2))) & 15;
      if (v == 15)
        return -1;
      if (utf(out, cap, &n, "0123456789*#abc"[v]))
        return -1;
    }
  }
  p += address;
  p++;
  unsigned dcs = b[p++];
  p += 7;
  unsigned udl = b[p++];
  int kind = -1;
  if (dcs < 128 && !(dcs & 32))
    kind = (dcs >> 2) & 3;
  else if ((dcs >> 4) == 14)
    kind = 2;
  else if ((dcs >> 4) == 12 || (dcs >> 4) == 13)
    kind = 0;
  else if ((dcs >> 4) == 15)
    kind = (dcs >> 2) & 1;
  size_t bytes = kind == 0 ? (udl * 7 + 7) / 8 : udl;
  if (p + bytes != len)
    return -1;
  size_t offset = 0;
  if (first & 64) {
    if (!bytes)
      return -1;
    offset = 1 + b[p];
    if (offset > bytes)
      return -1;
    size_t j = 1;
    unsigned ref = 0, part = 0, total = 0;
    int national = 0;
    while (j < offset) {
      if (j + 2 > offset)
        return -1;
      unsigned id = b[p + j++], sz = b[p + j++];
      if (j + sz > offset)
        return -1;
      if (id == 0 && sz == 3) {
        ref = b[p + j];
        total = b[p + j + 1];
        part = b[p + j + 2];
      }
      if (id == 8 && sz == 4) {
        ref = b[p + j] * 256 + b[p + j + 1];
        total = b[p + j + 2];
        part = b[p + j + 3];
      }
      if (id == 0x24 || id == 0x25)
        national = 1;
      j += sz;
    }
    if (national)
      kind = -1; /* do not misdecode national shift tables */
    written = total ? snprintf(out + n, cap - n, " [part %u/%u, ref %u]", part,
                               total, ref)
                    : snprintf(out + n, cap - n, " [UDH]");
    if (written < 0 || (size_t)written >= cap - n)
      return -1;
    n += (size_t)written;
  }
  if (utf(out, cap, &n, '\n'))
    return -1;
  if (kind == 0) {
    size_t skip = (offset * 8 + 6) / 7;
    if (skip > udl)
      return -1;
    return unpack(b + p, bytes, skip * 7, udl - skip, out, cap, &n);
  }
  if (kind == 2) {
    if ((bytes - offset) % 2)
      return -1;
    for (size_t j = offset; j < bytes; j += 2) {
      unsigned u = b[p + j] * 256 + b[p + j + 1];
      if (u >= 0xd800 && u <= 0xdbff && j + 3 < bytes) {
        unsigned lo = b[p + j + 2] * 256 + b[p + j + 3];
        if (lo >= 0xdc00 && lo <= 0xdfff) {
          u = 0x10000 + (u - 0xd800) * 1024 + lo - 0xdc00;
          j += 2;
        } else
          u = 0xfffd;
      } else if (u >= 0xd800 && u <= 0xdfff)
        u = 0xfffd;
      if (utf(out, cap, &n, u))
        return -1;
    }
    return 0;
  }
  written = snprintf(out + n, cap - n, "[binary/unsupported DCS 0x%02X] %s",
                     dcs, hex + (p + offset) * 2);
  return written >= 0 && (size_t)written < cap - n ? 0 : -1;
}
