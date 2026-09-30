/* The protocol list libavformat/protocols.c includes. The vendored one names
 * ff_file_protocol unconditionally, but file.c defines it only where the
 * platform config sets CONFIG_FILE_PROTOCOL - the Linux, macOS and Windows
 * aarch64 configs do not, and every ARM64 build failed to load for want of the
 * symbol. Nothing opens a URL here anyway (playback reads through a custom
 * AVIOContext), so the list follows the config. */
static const URLProtocol * const url_protocols[] = {
#if CONFIG_FILE_PROTOCOL
    &ff_file_protocol,
#endif
    NULL };
