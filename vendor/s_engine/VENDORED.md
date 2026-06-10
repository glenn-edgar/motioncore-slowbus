# vendor/s_engine — s_engine runtime (M-port subset)

Origin: `motioncore-prototype/s_engine/runtime/` @ 141cc7a (2026-06-10).
The `.c/.h` runtime only (was reached via `$(TOP)/../s_engine/runtime` in the
old motioncore Makefile; now vendored here so `app/samd21_client` builds out of
this repo alone).

`app/samd21_client` compiles the M-port subset (see its Makefile `SENGINE_SRCS`):
`s_engine_module.c eval node rom_init exception event_queue se_dict_hash
se_dict_string`. Deliberately excludes the loader/printf-init/global-registration/
stack files and the `.cc` C++ helpers.
