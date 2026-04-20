#include "rds_decode.h"
#include <string.h>

// RDS group type extraction
#define RDS_GROUP(b)  (((b) >> 12) & 0x0F)
#define RDS_VERSION(b) (((b) >> 11) & 0x01)  // 0=A, 1=B
#define RDS_TP(b)     (((b) >> 10) & 0x01)
#define RDS_PTY(b)    (((b) >> 5) & 0x1F)
#define RDS_TA(b)     (((b) >> 4) & 0x01)
#define RDS_MS(b)     (((b) >> 3) & 0x01)
#define RDS_PS_ADDR(b) ((b) & 0x03)

// Error threshold: skip blocks with uncorrectable errors
// err bits: [D_err, C_err, B_err, A_err] (2 bits each, 0=ok, 1-2=corrected, 3=uncorrectable)
#define BLOCK_ERR(err, idx) (((err) >> ((idx) * 2)) & 0x03)
#define BLOCK_OK(err, idx)  (BLOCK_ERR(err, idx) < 3)

static char rds_char(uint8_t c)
{
    // Basic RDS character mapping (ISO 646/8859-1 subset)
    if (c < 32) return ' ';
    if (c >= 32 && c <= 126) return (char)c;
    if (c == 0x8E) return 'a';  // ae -> a (simplified)
    if (c == 0x99) return 'o';  // oe -> o
    if (c == 0x9A) return 'u';  // ue -> u
    return '?';
}

void rds_decode_init(rds_decoded_t *rds)
{
    memset(rds, 0, sizeof(*rds));
    memset(rds->ps, ' ', 8);
    rds->ps[8] = '\0';
    memset(rds->rt, ' ', 64);
    rds->rt[64] = '\0';
}

void rds_decode_block(rds_decoded_t *rds, uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t err)
{
    // Check block A (PI) and B (group header) are valid
    if (!BLOCK_OK(err, 0) || !BLOCK_OK(err, 1)) return;

    rds->pi = a;
    rds->tp = RDS_TP(b);
    rds->pty = RDS_PTY(b);
    rds->has_data = true;

    uint8_t group = RDS_GROUP(b);
    bool version_b = RDS_VERSION(b);

    switch (group) {
    case 0: {
        // Group 0: Basic tuning
        rds->ta = RDS_TA(b);
        rds->ms = RDS_MS(b);

        if (BLOCK_OK(err, 3)) {
            uint8_t addr = RDS_PS_ADDR(b);
            // Block D contains 2 PS characters
            char c0 = rds_char((uint8_t)(d >> 8));
            char c1 = rds_char((uint8_t)(d & 0xFF));
            if (rds->ps[addr * 2] != c0 || rds->ps[addr * 2 + 1] != c1) {
                rds->ps[addr * 2] = c0;
                rds->ps[addr * 2 + 1] = c1;
                rds->ps_complete = true;
            }
        }
        break;
    }
    case 2: {
        // Group 2: RadioText
        if (!BLOCK_OK(err, 2) || !BLOCK_OK(err, 3)) break;

        bool ab = (b >> 4) & 0x01;
        uint8_t addr;

        if (version_b) {
            // 2B: 2 chars per group
            addr = (b & 0x0F) * 2;
            if (addr < 32) {
                rds->rt[addr] = rds_char((uint8_t)(d >> 8));
                rds->rt[addr + 1] = rds_char((uint8_t)(d & 0xFF));
                // Null-terminate if carriage return
                if (rds->rt[addr] == 0x0D) rds->rt[addr] = '\0';
                if (rds->rt[addr + 1] == 0x0D) rds->rt[addr + 1] = '\0';
            }
        } else {
            // 2A: 4 chars per group
            addr = (b & 0x0F) * 4;
            if (addr < 61) {
                rds->rt[addr] = rds_char((uint8_t)(c >> 8));
                rds->rt[addr + 1] = rds_char((uint8_t)(c & 0xFF));
                rds->rt[addr + 2] = rds_char((uint8_t)(d >> 8));
                rds->rt[addr + 3] = rds_char((uint8_t)(d & 0xFF));
                for (int i = 0; i < 4; i++) {
                    if (rds->rt[addr + i] == 0x0D) rds->rt[addr + i] = '\0';
                }
            }
        }

        if (ab != rds->rt_ab) {
            rds->rt_ab = ab;
            rds->rt_changed = true;
        }
        break;
    }
    default:
        break;
    }
}
