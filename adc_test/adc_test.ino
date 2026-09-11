/*
 * GPIO19 / ADC diagnostic for the ESP32-S3.
 *
 *   GPIO19 = ADC2 channel 8 on the ESP32-S3.
 *   GPIO19 / GPIO20 = native USB D- / D+ (USB-Serial-JTAG).
 *
 * This sketch does NOT use epdiy and does NOT touch the e-paper.
 *
 * It prints to BOTH the UART0 port (GPIO43/44) and the native USB port, so you
 * see output whichever port your serial monitor is on.
 *
 * IMPORTANT
 * ---------
 * The ESP32-S3 native USB (USB-Serial-JTAG) uses GPIO19 and GPIO20.  While
 * that peripheral is active, GPIO19 is driven by the USB PHY and can NOT be
 * read as an ADC input - your button voltage never reaches the ADC.
 *
 * If your serial monitor is the native USB port (you see the "ESP-ROM:..."
 * boot log there), that is exactly what is happening.  Use the scan line to
 * test an ADC1 pin (GPIO1..GPIO10) instead - those are never used by USB.
 */

#include <Arduino.h>
#include <stdarg.h>

/* 1 = also print on the native USB port.  This is needed to see anything if
 *     your monitor is the USB port, but it keeps GPIO19 busy (see above). */
#define USE_USB_SERIAL 1

#define ADC_PIN 19

static const uint8_t scan_pins[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,           /* ADC1 - safe with USB */
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,  /* ADC2 - 19/20 shared with USB */
};
#define SCAN_COUNT (sizeof(scan_pins) / sizeof(scan_pins[0]))

static void logf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Serial.print(buf);
#if USE_USB_SERIAL && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    !ARDUINO_USB_CDC_ON_BOOT
    if (USBSerial) {
        USBSerial.print(buf);
    }
#endif
}

void setup() {
    Serial.begin(115200);
#if USE_USB_SERIAL && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    !ARDUINO_USB_CDC_ON_BOOT
    USBSerial.begin(115200);
#endif
    delay(500);
    analogReadResolution(12);

    logf("\n=== GPIO19 ADC diagnostic (ESP32-S3, ADC2_CH8) ===\n");

    /* Digital pull-up / pull-down probe. */
    pinMode(ADC_PIN, INPUT_PULLUP);
    delay(10);
    int pu = digitalRead(ADC_PIN);
    pinMode(ADC_PIN, INPUT_PULLDOWN);
    delay(10);
    int pd = digitalRead(ADC_PIN);
    logf("probe: pull-up=%d pull-down=%d\n", pu, pd);
    if (pu == 1 && pd == 0) {
        logf("  -> GPIO19 floats: nothing is driving it at rest.\n");
    } else {
        logf("  -> GPIO19 is driven externally (USB-Serial-JTAG?).\n");
    }

    logf("\nGPIO19 raw/mV/digital, plus a full ADC pin scan:\n");
}

void loop() {
    static int n = 0;

    int raw = analogRead(ADC_PIN);
    uint32_t mv = analogReadMilliVolts(ADC_PIN);
    int d = digitalRead(ADC_PIN);
    logf("GPIO19: raw=%4d  mv=%4lu  digital=%d\n", raw, (unsigned long)mv, d);

    if (++n % 4 == 0) {
        logf("scan:");
        for (unsigned i = 0; i < SCAN_COUNT; i++) {
            logf(" %d=%d", scan_pins[i], analogRead(scan_pins[i]));
            if (i == 9) {
                logf("  |");
            }
        }
        logf("\n");
    }

    delay(250);
}
