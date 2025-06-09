#include "radio.h"
#include "dcs.h"
#include "driver/bk1080.h"
#include "driver/bk4819.h"
#include "driver/si473x.h"
#include "helper/channels.h"
#include <string.h>

// Диапазоны для BK4819
static const FreqBand bk4819_bands[] = {
    {
        .min_freq = BK4819_F_MIN,
        .max_freq = BK4819_F_MAX,
        .available_mods = {MOD_FM, MOD_AM, MOD_LSB, MOD_USB},
        .available_bandwidths =
            {
                BK4819_FILTER_BW_6k,  //  "U 6K"
                BK4819_FILTER_BW_7k,  //  "U 7K",
                BK4819_FILTER_BW_9k,  //  "N 9k",
                BK4819_FILTER_BW_10k, //  "N 10k",
                BK4819_FILTER_BW_12k, //  "W 12k",
                BK4819_FILTER_BW_14k, //  "W 14k",
                BK4819_FILTER_BW_17k, //  "W 17k",
                BK4819_FILTER_BW_20k, //  "W 20k",
                BK4819_FILTER_BW_23k, //  "W 23k",
                BK4819_FILTER_BW_26k, //	"W 26k",
            },
    },
    // ... другие диапазоны
};

// Диапазоны для SI4732
static const FreqBand si4732_bands[] = {
    {
        .min_freq = SI47XX_F_MIN,
        .max_freq = SI47XX_F_MAX,
        .available_mods = {SI47XX_AM, SI47XX_LSB, SI47XX_USB},
        .available_bandwidths =
            {
                SI47XX_BW_6_kHz,
                SI47XX_BW_4_kHz,
                SI47XX_BW_3_kHz,
                SI47XX_BW_2_kHz,
                SI47XX_BW_1_kHz,
                SI47XX_BW_1_8_kHz,
                SI47XX_BW_2_5_kHz,
            },
    },
    {
        .min_freq = SI47XX_F_MIN,
        .max_freq = SI47XX_F_MAX,
        .available_mods = {SI47XX_LSB, SI47XX_USB},
        .available_bandwidths =
            {
                SI47XX_SSB_BW_1_2_kHz,
                SI47XX_SSB_BW_2_2_kHz,
                SI47XX_SSB_BW_3_kHz,
                SI47XX_SSB_BW_4_kHz,
                SI47XX_SSB_BW_0_5_kHz,
                SI47XX_SSB_BW_1_0_kHz,
            },
    },
    {
        .min_freq = SI47XX_FM_F_MIN,
        .max_freq = SI47XX_FM_F_MAX,
        .available_mods = {SI47XX_FM},
        .available_bandwidths = {},
    },
};

static void RADIO_EnableCxCSS(VFOContext *ctx) {
  switch (ctx->tx_state.code.type) {
  case CODE_TYPE_CONTINUOUS_TONE:
    BK4819_SetCTCSSFrequency(CTCSS_Options[ctx->tx_state.code.value]);
    break;
  case CODE_TYPE_DIGITAL:
  case CODE_TYPE_REVERSE_DIGITAL:
    BK4819_SetCDCSSCodeWord(DCS_GetGolayCodeWord(ctx->tx_state.code.type,
                                                 ctx->tx_state.code.value));
    break;
  default:
    BK4819_ExitSubAu();
    break;
  }
}

// Инициализация VFO
void RADIO_Init(VFOContext *ctx, Radio radio_type) {
  memset(ctx, 0, sizeof(VFOContext));
  ctx->radio_type = radio_type;

  // Установка диапазона по умолчанию
  switch (radio_type) {
  case RADIO_BK4819:
    ctx->current_band = &bk4819_bands[0];
    ctx->frequency = 145500000; // 145.5 МГц (диапазон FM)
    break;
  case RADIO_SI4732:
    ctx->current_band = &si4732_bands[0];
    ctx->frequency = 7100000; // 7.1 МГц (диапазон AM)
    break;
  default:
    break;
  }
}

// Проверка параметра для текущего диапазона
bool RADIO_IsParamValid(VFOContext *ctx, ParamType param, uint32_t value) {
  const FreqBand *band = ctx->current_band;
  if (!band)
    return false;

  switch (param) {
  case PARAM_FREQUENCY:
    return (value >= band->min_freq && value <= band->max_freq);
  case PARAM_MODULATION:
    for (size_t i = 0; i < sizeof(band->available_mods); i++) {
      if (band->available_mods[i] == (ModulationType)value)
        return true;
    }
    return false;
  case PARAM_BANDWIDTH:
    for (size_t i = 0; i < sizeof(band->available_bandwidths); i++) {
      if (band->available_bandwidths[i] == (uint16_t)value)
        return true;
    }
    return false;
  default:
    return true; // Остальные параметры не зависят от диапазона
  }
}

// Установка параметра (всегда uint32_t!)
void RADIO_SetParam(VFOContext *ctx, ParamType param, uint32_t value) {
  if (!RADIO_IsParamValid(ctx, param, value))
    return;

  switch (param) {
  case PARAM_FREQUENCY:
    ctx->frequency = value;
    break;
  case PARAM_MODULATION:
    ctx->modulation = (ModulationType)value;
    break;
  case PARAM_BANDWIDTH:
    ctx->bandwidth = (uint16_t)value;
    break;
  case PARAM_VOLUME:
    ctx->volume = (uint8_t)value;
    break;
  default:
    return;
  }
  ctx->dirty[param] = true;
}

// Применение настроек
void RADIO_ApplySettings(VFOContext *ctx) {
  switch (ctx->radio_type) {
  case RADIO_BK4819:
    if (ctx->dirty[PARAM_FREQUENCY]) {
      BK4819_SetFrequency(ctx->frequency);
      ctx->dirty[PARAM_FREQUENCY] = false;
    }
    if (ctx->dirty[PARAM_MODULATION]) {
      BK4819_SetModulation(ctx->modulation);
      ctx->dirty[PARAM_MODULATION] = false;
    }
    break;

  case RADIO_SI4732:
    if (ctx->dirty[PARAM_FREQUENCY]) {
      SI47XX_TuneTo(ctx->frequency);
      ctx->dirty[PARAM_FREQUENCY] = false;
    }
    break;
  case RADIO_BK1080:
    break;
  }
}

void RADIO_ToggleTXEX(bool on, uint32_t txF, uint8_t power, bool paEnabled) {
  bool lastOn = gTxState == TX_ON;
  if (gTxState == on) {
    return;
  }

  gTxState = on ? RADIO_GetTXState(txF) : TX_UNKNOWN;

  if (gTxState == TX_ON) {
    RADIO_ToggleRX(false);

  } else if (lastOn) {
  }
}

// Начать передачу
void RADIO2_StartTX(VFOContext *ctx) {
  if (ctx->tx_state.is_active)
    return;

  uint8_t power = ctx->tx_state.power_level;

  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

  BK4819_TuneTo(ctx->tx_state.frequency, true);

  BOARD_ToggleRed(gSettings.brightness > 1);
  BK4819_PrepareTransmit();

  SYS_DelayMs(10);
  BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, paEnabled);
  SYS_DelayMs(5);
  BK4819_SetupPowerAmplifier(power, ctx->tx_state.frequency);
  SYS_DelayMs(10);

  RADIO_EnableCxCSS();
  ctx->tx_state.is_active = true;
}

// Завершить передачу
void RADIO2_StopTX(VFOContext *ctx) {
  if (!ctx->tx_state.is_active)
    return;

  BK4819_ExitDTMF_TX(true); // also prepares to tx ste

  sendEOT();
  toggleBK1080SI4732(false);

  ctx->tx_state.is_active = false;
  BOARD_ToggleRed(false);
  BK4819_TurnsOffTones_TurnsOnRX();

  gCurrentTxPower = 0;
  BK4819_SetupPowerAmplifier(0, 0);
  BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

  setupToneDetection();
  BK4819_TuneTo(ctx->frequency, true);
}
