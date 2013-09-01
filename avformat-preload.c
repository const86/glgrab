#include "glgrab.h"
#include <libavformat/avformat.h>

void __attribute__((constructor)) init(void) {
	av_register_input_format(&glgrab_avformat);
}
