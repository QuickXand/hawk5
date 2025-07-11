#include "si473x.h"
#include "../inc/dp32g030/gpio.h"
#include "../misc.h"
#include "../settings.h"
#include "../system.h"
#include "audio.h"
#include "eeprom.h"
#include "gpio.h"
#include "i2c.h"
#include "system.h"
#include "systick.h"
#include "uart.h"

static const uint8_t SI47XX_I2C_ADDR = 0x22;

#define RST_HIGH GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BK1080)
#define RST_LOW GPIO_SetBit(&GPIOB->DATA, GPIOB_PIN_BK1080)

RSQStatus rsqStatus;
static SsbMode currentSsbMode;

SI47XX_MODE si4732mode = SI47XX_FM;
uint16_t siCurrentFreq = 0;
bool isSi4732On = false;

static uint16_t fDiv() { return si4732mode == SI47XX_FM ? 1000 : 100; }

void SI47XX_ReadBuffer(uint8_t *buf, uint8_t size) {
  I2C_Start();
  I2C_Write(SI47XX_I2C_ADDR + 1);
  I2C_ReadBuffer(buf, size);
  I2C_Stop();
}

void SI47XX_WriteBuffer(uint8_t *buf, uint8_t size) {
  I2C_Start();
  I2C_Write(SI47XX_I2C_ADDR);
  I2C_WriteBuffer(buf, size);
  I2C_Stop();
}

bool SI47XX_IsSSB() {
  return si4732mode == SI47XX_USB || si4732mode == SI47XX_LSB;
}

void waitToSend() {
  uint8_t tmp = 0;
  do {
    SYSTICK_Delay250ns(1);
    SI47XX_ReadBuffer((uint8_t *)&tmp, 1);
  } while (!(tmp & STATUS_CTS));
}

#include "../ui/graphics.h" // X_X
void SI47XX_downloadPatch() {
  // Log("DL patch");
  FillRect(0, LCD_YCENTER - 4, LCD_WIDTH, 9, C_FILL);
  PrintMediumBoldEx(LCD_XCENTER, LCD_YCENTER + 3, POS_C, C_INVERT, "WAIT...");
  ST7565_Blit();

  uint8_t buf[64]; // 64 is optimal, more has no sense
  const uint16_t BUF_SIZE = ARRAY_SIZE(buf);
  const uint32_t PATCH_START = SETTINGS_GetEEPROMSize() - PATCH_SIZE;

  for (uint16_t offset = 0; offset < PATCH_SIZE; offset += BUF_SIZE) {
    const uint32_t rest = PATCH_SIZE - offset;
    const uint32_t eepromN = rest > BUF_SIZE ? BUF_SIZE : rest;
    EEPROM_ReadBuffer(PATCH_START + offset, buf, eepromN);

    for (uint16_t i = 0; i < eepromN; i += 8) {
      waitToSend();
      SI47XX_WriteBuffer(buf + i, 8);
    }
  }
  // Log("DL patch OK");
}

void SI47XX_SetProperty(uint16_t prop, uint16_t value) {
  waitToSend();
  uint8_t tmp[6] = {
      CMD_SET_PROPERTY, 0,           //
      prop >> 8,        prop & 0xff, //
      value >> 8,       value & 0xff //
  };
  SI47XX_WriteBuffer(tmp, 6);
  SYS_DelayMs(8); // irrespective of CTS coming up earlier than that
}

/* uint16_t SI47XX_GetProperty(uint16_t prop, bool *valid) {
  uint8_t response[4] = {0};
  uint8_t tmp[4] = {CMD_GET_PROPERTY, 0, prop >> 8, prop & 0xff};
  waitToSend();
  SI47XX_WriteBuffer(tmp, 4);
  SI47XX_ReadBuffer(response, 4);

  if (valid) {
    *valid = !(response[0] & STATUS_ERR);
  }

  return MAKE_WORD(response[2], response[3]);
} */

void RSQ_GET() {
  uint8_t cmd[2] = {CMD_FM_RSQ_STATUS, 0x01};
  if (si4732mode != SI47XX_FM) {
    cmd[0] = CMD_AM_RSQ_STATUS;
  }

  waitToSend();
  SI47XX_WriteBuffer(cmd, 2);
  SI47XX_ReadBuffer(rsqStatus.raw, si4732mode == SI47XX_FM ? 8 : 6);
}

void SI47XX_SetVolume(uint8_t volume) {
  if (volume > 63)
    volume = 63;
  SI47XX_SetProperty(PROP_RX_VOLUME, volume);
}

void setAvcAmMaxGain(uint8_t gain) {
  if (gain < 12 || gain > 90)
    return;
  SI47XX_SetProperty(PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, gain * 340);
}

void enableRDS(void) {
  // Enable and configure RDS reception
  if (si4732mode == SI47XX_FM) {
    SI47XX_SetProperty(PROP_FM_RDS_INT_SOURCE, FLG_RDSRECV);
    // Set the FIFO high-watermark to 12 RDS blocks, which is safe even for
    // old chips, yet large enough to improve performance.
    SI47XX_SetProperty(PROP_FM_RDS_INT_FIFO_COUNT, 12);
    SI47XX_SetProperty(
        PROP_FM_RDS_CONFIG,
        ((FLG_BLETHA_35 | FLG_BLETHB_35 | FLG_BLETHC_35 | FLG_BLETHD_35) << 8) |
            FLG_RDSEN);
  };
}

void SI47XX_SetAutomaticGainControl(uint8_t AGCDIS, uint8_t AGCIDX) {
  SI47XX_AgcOverrride agc;

  uint8_t cmd;

  if (si4732mode == SI47XX_FM)
    cmd = CMD_FM_AGC_OVERRIDE;
  else
    cmd = CMD_AM_AGC_OVERRIDE; // both for AM and SSB

  agc.arg.DUMMY = 0; // ARG1: bits 7:1 Always write to 0;
  agc.arg.AGCDIS = AGCDIS;
  agc.arg.AGCIDX = AGCIDX;

  waitToSend();

  uint8_t cmd2[] = {cmd, agc.raw[0], agc.raw[1]};
  SI47XX_WriteBuffer(cmd2, 3);
}

void SI47XX_PowerUp() {
  RST_HIGH;
  uint8_t cmd[3] = {CMD_POWER_UP, FLG_XOSCEN | FUNC_FM, OUT_ANALOG};
  if (si4732mode == SI47XX_AM) {
    cmd[1] = FLG_XOSCEN | FUNC_AM;
  }
  waitToSend();
  SI47XX_WriteBuffer(cmd, 3);
  SYS_DelayMs(500);

  isSi4732On = true;

  AUDIO_ToggleSpeaker(true);
  SI47XX_SetVolume(63);

  if (si4732mode == SI47XX_FM) {
    enableRDS();
  } else if (si4732mode == SI47XX_AM) {
    Log("set AM settings");
    SI47XX_SetAutomaticGainControl(1, 0);
    SI47XX_SetProperty(PROP_AM_SOFT_MUTE_MAX_ATTENUATION, 0);
    SI47XX_SetProperty(PROP_AM_AGC_RELEASE_RATE, 20);
    setAvcAmMaxGain(40);
  }
  SI47XX_SetFreq(siCurrentFreq);
}

void SI47XX_SsbSetup(SI47XX_SsbFilterBW AUDIOBW, uint8_t SBCUTFLT,
                     uint8_t AVC_DIVIDER, uint8_t AVCEN, uint8_t SMUTESEL,
                     uint8_t DSP_AFCDIS) {
  currentSsbMode.param.SBCUTFLT = SBCUTFLT;
  currentSsbMode.param.AVC_DIVIDER = AVC_DIVIDER;
  currentSsbMode.param.AVCEN = AVCEN;
  currentSsbMode.param.SMUTESEL = SMUTESEL;
  currentSsbMode.param.DSP_AFCDIS = DSP_AFCDIS;
  currentSsbMode.param.AUDIOBW = AUDIOBW;
  SI47XX_SetProperty(PROP_SSB_MODE,
                     (currentSsbMode.raw[1] << 8) | currentSsbMode.raw[0]);
}

void SI47XX_PatchPowerUp() {
  RST_HIGH;
  uint8_t cmd[3] = {CMD_POWER_UP, 0b00110001, OUT_ANALOG};
  waitToSend();
  SI47XX_WriteBuffer(cmd, 3);
  SYS_DelayMs(60);

  isSi4732On = true;

  SI47XX_downloadPatch();

  SI47XX_SsbSetup(2, 1, 0, 1, 0, 1);
  setAvcAmMaxGain(42);

  AUDIO_ToggleSpeaker(true);
  SI47XX_SetVolume(63);

  SI47XX_SetFreq(siCurrentFreq);
  SI47XX_SetProperty(PROP_SSB_SOFT_MUTE_MAX_ATTENUATION, 0);
  SI47XX_SetProperty(PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, 0x7800);

  si4732mode = SI47XX_USB; // FIXME: modulation must be set before power on to
                           // prevent repowering
}

void SI47XX_SetSsbBandwidth(SI47XX_SsbFilterBW bw) {
  SI47XX_SsbSetup(bw, 1, 0, 1, 0, 1);
}

void SI47XX_Seek(bool up, bool wrap) {
  uint8_t seekOpt = (up ? FLG_SEEKUP : 0) | (wrap ? FLG_WRAP : 0);
  uint8_t cmd[6] = {CMD_FM_SEEK_START, seekOpt, 0x00, 0x00, 0x00, 0x00};

  if (si4732mode == SI47XX_AM) {
    cmd[0] = CMD_AM_SEEK_START;
    cmd[5] = (siCurrentFreq > 1800) ? 1 : 0;
  }

  waitToSend();
  SI47XX_WriteBuffer(cmd, si4732mode == SI47XX_FM ? 2 : 6);
}

uint32_t SI47XX_getFrequency(bool *valid) {
  uint8_t response[4] = {0};
  uint8_t cmd[1] = {CMD_FM_TUNE_STATUS};

  if (si4732mode == SI47XX_AM) {
    cmd[0] = CMD_AM_TUNE_STATUS;
  }

  waitToSend();
  SI47XX_WriteBuffer(cmd, 1);
  SI47XX_ReadBuffer(response, 4);

  if (valid) {
    *valid = (response[1] & STATUS_VALID);
  }

  return ((response[2] << 8) | response[3]) * fDiv();
}

void SI47XX_PowerDown() {
  AUDIO_ToggleSpeaker(false);
  uint8_t cmd[1] = {CMD_POWER_DOWN};

  waitToSend();
  SI47XX_WriteBuffer(cmd, 1);
  SYSTICK_Delay250ns(10);
  RST_LOW;
  isSi4732On = false;
  siCurrentFreq = 0;
}

void SI47XX_SwitchMode(SI47XX_MODE mode) {
  if (si4732mode != mode) {
    bool wasSSB = SI47XX_IsSSB();
    si4732mode = mode;
    if (mode == SI47XX_USB || mode == SI47XX_LSB) {
      if (!wasSSB) {
        SI47XX_PowerDown();
        SI47XX_PatchPowerUp();
      }
    } else {
      SI47XX_PowerDown();
      SI47XX_PowerUp();
    }
  }
}

void SI47XX_SetFreq(uint16_t freq) {
  if (siCurrentFreq == freq) {
    return;
  }
  uint8_t hb = (freq >> 8) & 0xFF;
  uint8_t lb = freq & 0xFF;

  bool isSW = freq > 1800;

  uint8_t size = 5;
  uint8_t cmd[6] = {CMD_FM_TUNE_FREQ, 0x00, hb, lb, 0, 0};

  if (si4732mode == SI47XX_FM || si4732mode == SI47XX_AM) {
    cmd[1] = 0x01; // FAST
  } else {
    cmd[1] = si4732mode == SI47XX_USB ? 0b10000000 : 0b01000000;
  }

  if (si4732mode != SI47XX_FM) {
    cmd[0] = CMD_AM_TUNE_FREQ;
    size = 6; // was 5
  }

  if (si4732mode == SI47XX_AM && isSW) {
    cmd[5] = 1;
  }

  waitToSend();
  SI47XX_WriteBuffer(cmd, size);
  siCurrentFreq = freq;
  Log("SI f=%u", siCurrentFreq);
}

void SI47XX_SetAMFrontendAGC(uint8_t minGainIdx, uint8_t attnBackup) {
  SI47XX_SetProperty(PROP_AM_FRONTEND_AGC_CONTROL,
                     minGainIdx << 8 | attnBackup);
}

void SI47XX_SetBandwidth(SI47XX_FilterBW AMCHFLT, bool AMPLFLT) {
  SI47XX_BW_Config cfg = {0};
  cfg.param.AMCHFLT = AMCHFLT;
  cfg.param.AMPLFLT = AMPLFLT;
  SI47XX_SetProperty(PROP_AM_CHANNEL_FILTER, (cfg.raw[1] << 8) | cfg.raw[0]);
}

void SI47XX_ReadRDS(uint8_t buf[13]) {
  uint8_t cmd[2] = {CMD_FM_RDS_STATUS, RDS_STATUS_ARG1_CLEAR_INT};
  waitToSend();
  SI47XX_WriteBuffer(cmd, 2);
  SI47XX_ReadBuffer(buf, 13);
}

void SI47XX_SetSeekFmLimits(uint32_t bottom, uint32_t top) {
  uint16_t divider = fDiv();
  bottom /= divider;
  top /= divider;
  SI47XX_SetProperty(PROP_FM_SEEK_BAND_BOTTOM, bottom);
  SI47XX_SetProperty(PROP_FM_SEEK_BAND_TOP, top);
}

void SI47XX_SetSeekAmLimits(uint32_t bottom, uint32_t top) {
  uint16_t divider = fDiv();
  bottom /= divider;
  top /= divider;
  SI47XX_SetProperty(PROP_AM_SEEK_BAND_BOTTOM, bottom);
  SI47XX_SetProperty(PROP_AM_SEEK_BAND_TOP, top);
}

void SI47XX_SetSeekFmSpacing(uint32_t spacing) {
  spacing /= fDiv();
  SI47XX_SetProperty(PROP_FM_SEEK_FREQ_SPACING, spacing);
}

void SI47XX_SetSeekAmSpacing(uint32_t spacing) {
  spacing /= fDiv();
  SI47XX_SetProperty(PROP_AM_SEEK_FREQ_SPACING, spacing);
}

void SI47XX_SetSeekFmRssiThreshold(uint16_t value) {
  SI47XX_SetProperty(PROP_FM_SEEK_TUNE_RSSI_THRESHOLD, value);
}

void SI47XX_SetSeekAmRssiThreshold(uint16_t value) {
  SI47XX_SetProperty(PROP_AM_SEEK_TUNE_RSSI_THRESHOLD, value);
}

void SI47XX_SetBFO(int16_t bfo) { SI47XX_SetProperty(PROP_SSB_BFO, bfo); }

void SI47XX_TuneTo(uint32_t f) {
  if (SI47XX_IsSSB()) {
    int64_t bfo = ((int64_t)(siCurrentFreq * fDiv()) - (int64_t)f) * 10;
    if (bfo > -16000 && bfo < 16000) {
      SI47XX_SetBFO(bfo);
      f = siCurrentFreq * fDiv();
    } else {
      SI47XX_SetBFO(0);
    }
  }
  f /= fDiv();
  if (si4732mode == SI47XX_FM) {
    f -= f % 5;
  }
  SI47XX_SetFreq(f);
}

void SI47xx_GetStatus(uint8_t intack, uint8_t cancel) {
  SI47XX_TuneStatus status;

  uint8_t cmd = CMD_FM_TUNE_STATUS;

  if (si4732mode == SI47XX_AM) {
    cmd = CMD_AM_TUNE_STATUS;
  }

  status.arg.INTACK = intack;
  status.arg.CANCEL = cancel;
  status.arg.RESERVED2 = 0;

  uint8_t cmdA[2] = {cmd, status.raw};

  uint8_t response[4] = {0};

  waitToSend();
  SI47XX_WriteBuffer(cmdA, 2);
  SI47XX_ReadBuffer(response, 4);

  /* if (valid) {
    *valid = !(response[0] & STATUS_ERR);
  } */
}
