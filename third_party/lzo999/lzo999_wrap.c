/* lzo999_wrap.c — stable size_t interface over lzo1x_999_compress so the
 * jade library never includes LZO headers (lzo_uint sizing stays private).
 */
#include <stddef.h>

#include "lzo/lzo1x.h"

size_t jade_lzo999_wrkmem_size(void) { return (size_t)LZO1X_999_MEM_COMPRESS; }

/* Returns 0 on success (LZO_E_OK). *out_len carries capacity in, size out. */
int jade_lzo1x_999_compress(const unsigned char* in, size_t in_len,
                            unsigned char* out, size_t* out_len, void* wrkmem) {
    lzo_uint ol = (lzo_uint)*out_len;
    int r = lzo1x_999_compress(in, (lzo_uint)in_len, out, &ol, (lzo_voidp)wrkmem);
    *out_len = (size_t)ol;
    return r;
}
