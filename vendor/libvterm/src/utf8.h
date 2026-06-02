#undef utf8_seqlen
#undef fill_utf8

#ifndef VTERM_UTF8_PREFIX
#define VTERM_UTF8_PREFIX vterm
#endif

#define VTERM_UTF8_JOIN2(a, b) a##b
#define VTERM_UTF8_JOIN(a, b) VTERM_UTF8_JOIN2(a, b)
#define VTERM_UTF8_NAME(name) VTERM_UTF8_JOIN(VTERM_UTF8_PREFIX, _##name)
#define utf8_seqlen VTERM_UTF8_NAME(utf8_seqlen)
#define fill_utf8 VTERM_UTF8_NAME(fill_utf8)

/* The following functions copied and adapted from libtermkey
 *
 * http://www.leonerd.org.uk/code/libtermkey/
 */
static inline unsigned int utf8_seqlen(long codepoint)
{
  if(codepoint < 0x0000080) return 1;
  if(codepoint < 0x0000800) return 2;
  if(codepoint < 0x0010000) return 3;
  if(codepoint < 0x0200000) return 4;
  if(codepoint < 0x4000000) return 5;
  return 6;
}

/* Does NOT NUL-terminate the buffer */
static int fill_utf8(long codepoint, char *str)
{
  int nbytes = utf8_seqlen(codepoint);

  // This is easier done backwards
  int b = nbytes;
  while(b > 1) {
    b--;
    str[b] = 0x80 | (codepoint & 0x3f);
    codepoint >>= 6;
  }

  switch(nbytes) {
    case 1: str[0] =        (codepoint & 0x7f); break;
    case 2: str[0] = 0xc0 | (codepoint & 0x1f); break;
    case 3: str[0] = 0xe0 | (codepoint & 0x0f); break;
    case 4: str[0] = 0xf0 | (codepoint & 0x07); break;
    case 5: str[0] = 0xf8 | (codepoint & 0x03); break;
    case 6: str[0] = 0xfc | (codepoint & 0x01); break;
  }

  return nbytes;
}
/* end copy */
