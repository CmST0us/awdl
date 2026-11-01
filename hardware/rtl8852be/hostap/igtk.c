#include "igtk.h"
#include <errno.h>
#include <string.h>
int wa_igtk_install(struct wa_igtk *s, struct wa_backend *b, uint32_t features,
                    uint32_t index, const uint8_t *key, size_t len) {
  if (!(features & WA_NAN_FEAT_LOCAL_IGTK_CAM)) return -EOPNOTSUPP;
  if (index != 4 && index != 5) return -EOPNOTSUPP;
  if (!key || len != 16) return -EINVAL;
  if (s->attempted) return -EBUSY;
  *s = (struct wa_igtk){.owner = WA_NAN_KEY_OWNER_LOCAL_NMI_IGTK,
                       .cipher = 0x000fac06, .index = index, .attempted = 1};
  int r = wa_backend_owned_key(b, WA_NAN_OP_KEY_ADD, s->owner, NULL,
                               s->cipher, s->index, key, len);
  if (!r) s->installed = 1;
  return r;
}
int wa_igtk_delete(struct wa_igtk *s, struct wa_backend *b) {
  if (!s->attempted) return 0;
  int r = wa_backend_owned_key(b, WA_NAN_OP_KEY_DEL, s->owner, NULL,
                               s->cipher, s->index, NULL, 0);
  if (r) s->cleanup_failed = 1;
  else memset(s, 0, sizeof(*s));
  return r;
}
