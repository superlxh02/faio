#ifdef FAIO_ENTRY_SOURCE_OPTION
#error "Entry source compile definitions must not leak into other sources"
#endif
int entry_helper_value() { return 42; }
