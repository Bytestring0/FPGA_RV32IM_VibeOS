#include "qw_int_sample.h"
#include <string.h>

int qw_sample_greedy_bits(const void *binary32_logits,size_t n,uint32_t *token){
 const unsigned char *bytes=(const unsigned char *)binary32_logits;
 uint32_t best=0,best_key=0;
 if(!binary32_logits||!token||!n||n>QW_VOCAB)return QW_ARGUMENT;
 for(size_t i=0;i<n;i++){
  uint32_t bits,key;
  memcpy(&bits,bytes+i*sizeof bits,sizeof bits);
  if((bits&UINT32_C(0x7f800000))==UINT32_C(0x7f800000))return QW_NUMERIC;
  /* Canonicalize zero before ordering so its two encodings tie exactly.
   * Complement negative words and flip the sign bit of nonnegative words:
   * the unsigned key then increases with every finite binary32 value. */
  if((bits&UINT32_C(0x7fffffff))==0)bits=0;
  key=(bits&UINT32_C(0x80000000))?~bits:(bits^UINT32_C(0x80000000));
  if(key>best_key){best_key=key;best=(uint32_t)i;}
 }
 *token=best;
 return QW_OK;
}
