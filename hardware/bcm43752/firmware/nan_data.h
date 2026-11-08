#ifndef BCM43752_NAN_DATA_H
#define BCM43752_NAN_DATA_H

/* The BSS cubby registration has WLC lifetime. Runtime NAN stop removes NDIs;
 * it must not free this context while WLC retains its cubby callback. */
int nan_data_context_attach(void *wlc, void *nan);
int nan_data_check_idle(void *nan);
int nan_data_deinitialize(void *wlc, void *nan);
int nan_data_register_callbacks(void *nan, unsigned index, unsigned subtype);
int nan_data_enable(void *nan, void *bss, unsigned index);
void *nan_data_create(void *wlc, void *dp, const void *request, int *status);
int nan_data_remove(void *dp, const unsigned char *address);

#endif
