#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t pi;
    uint8_t pty;
    bool tp;
    bool ta;
    bool ms;
    char ps[9];
    char rt[65];
    bool ps_complete;
    bool rt_changed;
    bool rt_ab;
    bool has_data;
} rds_decoded_t;

void rds_decode_init(rds_decoded_t *rds);
void rds_decode_block(rds_decoded_t *rds, uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t err);
