/* include/linux/touch_wake.h */

#ifndef _LINUX_TOUCH_WAKE_H
#define _LINUX_TOUCH_WAKE_H

#include <linux/input.h>
#include "../../drivers/sensorhub/stm/ssp.h"

#define TOUCHWAKE_DEBUG_PRINT

void powerkey_pressed(void);
void powerkey_released(void);
void touch_press(void);
bool device_is_suspended(void);
bool touchwake_active(void);

#endif
