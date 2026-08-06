#ifndef DRIVERS_AC97_H
#define DRIVERS_AC97_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AC97 NAM (Native Audio Mixer) registers — relative to NAM base (BAR0 + 0x80). */
#define AC97_REG_RESET         0x00
#define AC97_REG_MASTER_VOL    0x02
#define AC97_REG_MASTER_MONO   0x06
#define AC97_REG_PCBEEP_VOL    0x0A
#define AC97_REG_PHONE_VOL     0x0C
#define AC97_REG_MIC_VOL       0x0E
#define AC97_REG_LINE_IN_VOL   0x10
#define AC97_REG_CD_VOL        0x12
#define AC97_REG_AUX_VOL       0x14
#define AC97_REG_PCM_OUT_VOL   0x18
#define AC97_REG_RECORD_SEL    0x1A
#define AC97_REG_RECORD_GAIN   0x1C
#define AC97_REG_EXTENDED_ID   0x28
#define AC97_REG_EXTENDED_CTRL 0x2A
#define AC97_REG_POWERDOWN     0x2C

/* AC97 NABM (Native Audio Bus Master) registers — relative to NABM base (BAR0). */
#define AC97_NABM_PO_BDBAR    0x00
#define AC97_NABM_PO_CIV      0x04
#define AC97_NABM_PO_LVI      0x05
#define AC97_NABM_PO_SR       0x06
#define AC97_NABM_PO_PICB     0x08
#define AC97_NABM_PO_PIV      0x0A
#define AC97_NABM_PO_CR       0x0B
#define AC97_NABM_GLB_CTRL    0x2C
#define AC97_NABM_GLB_STAT    0x30
#define AC97_NABM_PO_VOL      0x32

#define AC97_PCI_CLASS        0x04
#define AC97_PCI_SUBCLASS     0x01

#define AC97_MAX_VOLUME       0x3F

typedef struct {
    pci_dev_t *pci_dev;
    uint16_t   nam_base;       /* NAM (mixer) I/O base — BAR0 + 0x80 */
    uint16_t   nabm_base;      /* NABM (bus master) I/O base — BAR0 */
    bool       initialized;
    uint8_t    master_volume;
} ac97_dev_t;

void     ac97_init(void *serial_dev);
bool     ac97_found(void);
uint8_t  ac97_get_master_volume(void);
void     ac97_set_master_volume(uint8_t vol);
void     ac97_mute(void);
void     ac97_unmute(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_AC97_H */
