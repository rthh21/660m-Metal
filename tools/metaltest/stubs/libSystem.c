// Link-time stub only (never shipped): exports the libSystem symbols rmb_metaltest imports.
void printf(void) {}
void fflush(void) {}
void usleep(void) {}
void memset(void) {}
void memcpy(void) {}
void bzero(void) {}
void exit(void) {}
void stub_binder(void) __asm__("dyld_stub_binder");
void stub_binder(void) {}
