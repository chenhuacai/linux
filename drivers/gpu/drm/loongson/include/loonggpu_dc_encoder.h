#ifndef STREAM_ENCODER_H_
#define STREAM_ENCODER_H_

#include "loonggpu_dc_resource.h"

struct loonggpu_dc_encoder {
	struct encoder_resource *resource;
	struct loonggpu_dc *dc;
	bool has_ext_encoder;
};

int loonggpu_dc_encoder_hw_reset(struct loonggpu_dc_encoder *dc_encoder, bool signal_level, unsigned int delay_ms);
struct loonggpu_dc_encoder *dc_encoder_construct(struct loonggpu_dc *dc, struct encoder_resource *resource);
int loonggpu_dc_encoder_init(struct loonggpu_device *adev, int link_index);

#endif /* STREAM_ENCODER_H_ */
