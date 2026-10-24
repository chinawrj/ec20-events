/* Deterministic malformed-input exercise; synthetic bytes only. Run with ASan/UBSan. */
#include "../src/sms.h"
#include <stdint.h>
#include <stdio.h>
static uint32_t seed=0xEC202026;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void){char hex[1101],out[1025];const char *digits="0123456789ABCDEF";
 for(unsigned i=0;i<20000;i++){size_t n=rnd()%1100;for(size_t j=0;j<n;j++)hex[j]=digits[rnd()%16];hex[n]=0;(void)sms_decode(hex,out,1+rnd()%sizeof out);}
 puts("20,000 synthetic malformed PDU cases completed");return 0;}
