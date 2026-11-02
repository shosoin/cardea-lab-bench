/* Lab bench — status LED: off unless signalling. See lab_led.c. */
#ifndef LAB_LED_H
#define LAB_LED_H

typedef enum { LAB_LED_OK, LAB_LED_WARN, LAB_LED_ERR } lab_led_event_t;

void lab_led_init(void);
/* `times` short, dim flashes (blocks the caller for ~200 ms each), then off. */
void lab_led_flash(lab_led_event_t ev, int times);

#endif
