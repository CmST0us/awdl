#ifndef NAND_IGTK_H
#define NAND_IGTK_H
#include "backend.h"
/* Metadata survives an uncertain command completion; key bytes do not. */
struct wa_igtk {
  uint32_t owner, cipher, index;
  int attempted, installed, cleanup_failed;
};
int wa_igtk_install(struct wa_igtk *, struct wa_backend *, uint32_t features,
                    uint32_t index, const uint8_t *key, size_t len);
int wa_igtk_delete(struct wa_igtk *, struct wa_backend *);
#endif
