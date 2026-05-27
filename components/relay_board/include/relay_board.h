#pragma once

#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

int relay_board_init(const irrigation_config_t *config);
int relay_board_all_off(void);
int relay_board_set_line(void *user_ctx, uint8_t line_id, bool on);

#ifdef __cplusplus
}
#endif
