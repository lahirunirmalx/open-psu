/**
 * psu_probe - minimal CLI utility to exercise the driver layer.
 *
 * Usage:
 *   psu_probe                     # list registered drivers
 *   psu_probe <driver-id> <device> [baud] [seconds]
 *
 * Examples:
 *   psu_probe modbus-bridge /dev/ttyUSB0
 *   psu_probe modbus-bridge /dev/ttyUSB0 115200 5
 *
 * Prints channel state once per second so you can verify a driver opens,
 * polls, and exposes the right capabilities - without dragging in SDL2.
 */

#include "drivers/registry.h"
#include "platform/platform.h"
#include "psu_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void list_drivers(void) {
    size_t n;
    const psu_driver_factory_t *const *all = psu_drivers_list(&n);
    printf("Registered drivers (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        printf("  %-16s  %s\n      %s\n",
               all[i]->id, all[i]->display_name, all[i]->description);
    }
}

static void print_caps(const psu_caps_t *c) {
    printf("Model:    %s\n", c->model_name);
    printf("Channels: %d   Vmax: %.2f V   Imax: %.3f A\n",
           c->n_channels, c->v_max, c->i_max);
    printf("Optional: tracking=%d mppt=%d ovp=%d temp=%d in_v=%d runtime=%d energy=%d\n",
           c->supports_tracking, c->supports_mppt, c->supports_ovp,
           c->supports_temperature, c->supports_input_voltage,
           c->supports_runtime, c->supports_energy);
}

static void print_channel(int ch, const psu_channel_state_t *s) {
    if (!s->valid) {
        printf("  ch%d: (no data yet)\n", ch);
        return;
    }
    printf("  ch%d: set=%.2fV/%.3fA  out=%.2fV %.3fA %.2fW  %s  %s  T=%.1f°C\n",
           ch, s->set_v, s->set_a, s->out_v, s->out_a, s->out_p,
           s->out_on ? "ON " : "OFF",
           s->cv_mode ? "CV" : "CC",
           s->temp_c);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        list_drivers();
        return 0;
    }

    const char *id     = argv[1];
    const char *device = (argc >= 3) ? argv[2] : NULL;
    int   baud         = (argc >= 4) ? atoi(argv[3]) : 0;
    int   seconds      = (argc >= 5) ? atoi(argv[4]) : 5;

    const psu_driver_factory_t *f = psu_drivers_find(id);
    if (!f) {
        fprintf(stderr, "unknown driver id: %s\n\n", id);
        list_drivers();
        return 1;
    }
    if (!device) {
        fprintf(stderr, "usage: psu_probe %s <device> [baud] [seconds]\n", id);
        return 1;
    }
    if (baud <= 0) baud = f->default_baud;

    psu_driver_t *d = f->open(device, baud);
    if (!d) {
        fprintf(stderr, "failed to open driver '%s' on %s @ %d baud\n",
                id, device, baud);
        return 1;
    }

    print_caps(&d->caps);
    printf("Polling for %d seconds...\n", seconds);

    for (int i = 0; i < seconds; i++) {
        pl_sleep_ms(1000);
        printf("[t=%d] connected=%d\n", i + 1, d->is_connected(d) ? 1 : 0);
        for (int ch = 1; ch <= d->caps.n_channels; ch++) {
            psu_channel_state_t s;
            d->get_channel(d, ch, &s);
            print_channel(ch, &s);
        }
        if (d->get_stats) {
            uint32_t rx, err;
            d->get_stats(d, &rx, &err);
            printf("  stats: rx=%u err=%u\n", rx, err);
        }
    }

    d->close(d);
    return 0;
}
