#include <Arduino.h>
#include <math.h>
#include "driver/i2s_std.h"   // ← NEW API (replaces driver/i2s.h)
#include "esp_task_wdt.h"

// ================================================================
// ESP32-S3 PSRAM CONFIGURATION
// ================================================================
//#define USE_PSRAM

// ================================================================
// PIN DEFINITIONS
// ================================================================
#define LED_SHORT   35
#define LED_NORMAL  36
#define LED_LONG    37

#define I2S_WS      42
#define I2S_SCK     41
#define I2S_SD      4

#define SAMPLE_RATE 16000

// ================================================================
// GLOBAL I2S HANDLE  
// ================================================================
i2s_chan_handle_t rx_handle = NULL;

// ================================================================
// BREATH DETECTION PARAMETERS
// ================================================================
float silenceThreshold = 300;

#define MIN_BREATH_MS       450//WAS 400
#define MAX_BREATH_MS       7000 //WAS 5000

#ifdef USE_PSRAM
  #define BREATH_BUFFER_SIZE 64000 //was 56000
#else
  #define BREATH_BUFFER_SIZE 40000
  int16_t breathBuffer[BREATH_BUFFER_SIZE];
#endif

// ================================================================
// STATE MACHINE
// ================================================================
enum State { IDLE, BREATH_STARTED, IN_BREATH };
State currentState = IDLE;

int breathIndex = 0;
unsigned long breathStartTime = 0;
int silenceCounter = 0;

// ================================================================
// MODEL PARAMETERS
// ================================================================
// const float mean_vec[4] PROGMEM = {0.26185937, 0.00825264, 0.07683448, 2.62583244};
// const float std_vec[4]  PROGMEM = {0.09554418, 0.01111823, 0.06159434, 1.25329837};

// const float W[3][4] PROGMEM = {
//   {-0.25747891, -0.10149495,  0.11036784,  11.17836034},
//   {-0.04449949, -0.04169355,  0.06119079,   0.44938457},
//   { 0.30197839,  0.14318851, -0.17155863, -11.62774491}
// };

// const float b[3] PROGMEM = {4.91956736, 3.97820881, -8.89777616};

const float mean_vec[4] PROGMEM = {0.26078758f, 0.00892260f, 0.07609676f, 2.63492390f};
const float std_vec[4]  PROGMEM = {0.09454386f, 0.01364107f, 0.06044945f, 1.28179849f};

const float W[3][4] PROGMEM = {
  {-0.19919221f, 0.07847493f, 0.21512109f, 12.09137675f},
  {-0.13389020f, 0.00250019f, 0.09119507f, 0.57239060f},
  {0.33308241f, -0.08097512f, -0.30631616f, -12.66376735f}
};

const float b[3] PROGMEM = {5.29232334f, 4.32183731f, -9.61416064f};

// const float mean_vec[4] PROGMEM = {0.26088294f, 0.00887919f, 0.07595453f, 2.62248132f};
// const float std_vec[4]  PROGMEM = {0.09383518f, 0.01328329f, 0.05953778f, 1.28353815f};

// const float W[3][4] PROGMEM = {
//   {-0.29044635f, 0.11533511f, 0.26408396f, 12.10132978f},
//   {0.15226498f, 0.05643382f, -0.09089151f, 0.30046687f},
//   {0.13818138f, -0.17176893f, -0.17319245f, -12.40179666f}
// };

// const float b[3] PROGMEM = {3.54597491f, 3.61518528f, -7.16116018f};



// Update your labels[] array in loop() to match class order above:

// ================================================================
// MEMORY MANAGEMENT
// ================================================================
bool initPSRAMBuffer() {
#ifdef USE_PSRAM
  if (psramFound()) {
    breathBuffer = (int16_t*)ps_malloc(BREATH_BUFFER_SIZE * sizeof(int16_t));
    yield();
    if (breathBuffer == NULL) {
      Serial.println("Failed to allocate PSRAM buffer!");
      return false;
    }
    Serial.printf("Allocated %d bytes in PSRAM\n", BREATH_BUFFER_SIZE * sizeof(int16_t));
    return true;
  } else {
    Serial.println("PSRAM not available!");
    return false;
  }
#else
  Serial.println("Using normal RAM buffer");
  return true;
#endif
}

// ================================================================
// FEATURE COMPUTATION
// ================================================================
float computeRMS(int16_t *samples, int N) {
  int64_t sumSquares = 0;
  for (int i = 0; i < N; i++)
    sumSquares += (int64_t)samples[i] * samples[i];
  return sqrt(sumSquares / (float)N);
}

float computeZCR(int16_t *samples, int N) {
  int crossings = 0;
  for (int i = 1; i < N; i++) {
    if ((samples[i] >= 0 && samples[i-1] < 0) ||
        (samples[i] < 0  && samples[i-1] >= 0))
      crossings++;
  }
  return crossings / (float)N;
}

float computeVariance(int16_t *samples, int N) {
  int64_t sum = 0, sumSquares = 0;
  for (int i = 0; i < N; i++) {
    sum       += samples[i];
    sumSquares += (int64_t)samples[i] * samples[i];
  }
  float mean = sum / (float)N;
  return (sumSquares / (float)N) - (mean * mean);
}

float findMaxAmplitude(int16_t *samples, int N) {
  int16_t maxVal = 0;
  for (int i = 0; i < N; i++) {
    int16_t absVal = abs(samples[i]);
    if (absVal > maxVal) maxVal = absVal;
  }
  return (float)maxVal;
}

void normalizeBuffer(int16_t *samples, int N) {
  float maxAmp = findMaxAmplitude(samples, N);
  if (maxAmp > 0) {
    float scale = 32767.0 / maxAmp;
    for (int i = 0; i < N; i++)
      samples[i] = (int16_t)(samples[i] * scale);
  }
}

// ================================================================
// I2S INITIALIZATION  (new ESP-IDF API, no clock errors)
// ================================================================
void initI2S() {
  // Step 1: create RX channel
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  if (err != ESP_OK) {
    Serial.printf("Failed to create I2S channel: %d\n", err);
    return;
  }

  // Step 2: configure standard I2S mode for INMP441
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK,
      .ws   = (gpio_num_t)I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_SD,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv   = false,
      },
    },
  };

  err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
  if (err != ESP_OK) {
    Serial.printf("Failed to init I2S std mode: %d\n", err);
    return;
  }

  // Step 3: enable
  err = i2s_channel_enable(rx_handle);
  if (err != ESP_OK) {
    Serial.printf("Failed to enable I2S channel: %d\n", err);
    return;
  }

  // Step 4: flush DMA warm-up (INMP441 needs several reads to stabilise)
  int32_t flush[512];
  size_t br;
  for (int i = 0; i < 10; i++)
    i2s_channel_read(rx_handle, flush, sizeof(flush), &br, portMAX_DELAY);

  Serial.println("I2S initialized successfully");
}

// ================================================================
// PIN VERIFICATION
// ================================================================
void verifyPins() {
  Serial.println("=== PIN ASSIGNMENT VERIFICATION ===");
  Serial.printf("LED_SHORT  (Red):    GPIO%d\n", LED_SHORT);
  Serial.printf("LED_NORMAL (Green):  GPIO%d\n", LED_NORMAL);
  Serial.printf("LED_LONG   (Blue):   GPIO%d\n", LED_LONG);
  Serial.println();
  Serial.printf("I2S_WS  (Word Select):  GPIO%d\n", I2S_WS);
  Serial.printf("I2S_SCK (Clock):        GPIO%d\n", I2S_SCK);
  Serial.printf("I2S_SD  (Data):         GPIO%d\n", I2S_SD);
  Serial.println("===================================\n");

  Serial.println("Testing LEDs...");
  digitalWrite(LED_SHORT,  HIGH); delay(200); digitalWrite(LED_SHORT,  LOW);
  digitalWrite(LED_NORMAL, HIGH); delay(200); digitalWrite(LED_NORMAL, LOW);
  digitalWrite(LED_LONG,   HIGH); delay(200); digitalWrite(LED_LONG,   LOW);
  Serial.println("LED test complete.");
}

// ================================================================
// CALIBRATION
// ================================================================
void calibrateThreshold() {
  Serial.println("\n=== CALIBRATION START ===");
  Serial.println("Stay completely silent for 5 seconds...");
  delay(5000);

  const int calibrationWindows = 200;
  float totalRMS = 0, maxRMS = 0;

  for (int i = 0; i < calibrationWindows; i++) {
    int32_t raw[256];
    int16_t buffer[256];
    size_t bytesRead;

    // ← only i2s_channel_read from here on
    i2s_channel_read(rx_handle, raw, sizeof(raw), &bytesRead, portMAX_DELAY);

    int samples = bytesRead / sizeof(int32_t);
    if (samples == 0) continue;

    for (int j = 0; j < samples; j++)
      buffer[j] = (int16_t)(raw[j] >> 14);   // ← consistent shift

    float rms = computeRMS(buffer, samples);
    Serial.printf("buf[0]=%d  RMS=%.2f\n", buffer[0], rms);

    totalRMS += rms;
    if (rms > maxRMS) maxRMS = rms;
    delay(10);
  }

  float avgRMS = totalRMS / calibrationWindows;
  silenceThreshold = maxRMS * 4.0;   // 2× peak ambient noise
  if (silenceThreshold < 120) silenceThreshold = 120;  // hard floor

  // silenceThreshold = maxRMS * 4.0;   // 2× peak ambient noise
  // if (silenceThreshold < 80) silenceThreshold = 80;  // hard floor

  Serial.println("=== CALIBRATION DONE ===");
  Serial.printf("Average RMS: %.2f\n", avgRMS);
  Serial.printf("Max RMS:     %.2f\n", maxRMS);
  Serial.printf("Threshold:   %.2f\n\n", silenceThreshold);
}

// ================================================================
// MEMORY STATUS
// ================================================================
void printMemoryStatus() {
  Serial.println("\n=== MEMORY STATUS ===");
  Serial.printf("Free Heap:      %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Min Free Heap:  %d bytes\n", ESP.getMinFreeHeap());
  Serial.printf("Max Alloc Heap: %d bytes\n", ESP.getMaxAllocHeap());
  Serial.printf("Buffer size:    %d samples (%d bytes)\n",
                BREATH_BUFFER_SIZE, BREATH_BUFFER_SIZE * sizeof(int16_t));
  Serial.println("=====================\n");
}

// ================================================================
// BREATH DETECTION
// ================================================================
bool detectAndProcessBreath() {
  if (breathIndex >= BREATH_BUFFER_SIZE) {
    Serial.println("Buffer overflow protection triggered");
    currentState = IDLE;
    breathIndex = 0;
    return false;
  }

  static int32_t i2sRaw[256];
  static int16_t windowBuffer[256];
  size_t bytesRead;

  // ← only i2s_channel_read
  i2s_channel_read(rx_handle, i2sRaw, sizeof(i2sRaw), &bytesRead, portMAX_DELAY);
  int samplesRead = bytesRead / sizeof(int32_t);
  if (samplesRead == 0) return false;

  for (int i = 0; i < samplesRead; i++)
    windowBuffer[i] = (int16_t)(i2sRaw[i] >> 14);   // ← consistent shift

  float windowRMS = computeRMS(windowBuffer, samplesRead);

  switch (currentState) {
    case IDLE: {
      static int triggerCount = 0;
      if (windowRMS > silenceThreshold) {
        if (++triggerCount > 4) {
          currentState    = BREATH_STARTED;
          breathIndex     = 0;
          breathStartTime = millis();
          silenceCounter  = 0;
          triggerCount    = 0;
          Serial.println("Breath STARTED");
        }
      } else {
        triggerCount = 0;
      }
      break;
    }

    case BREATH_STARTED:
    
    // case IN_BREATH: {
    //   if (millis() - breathStartTime > MAX_BREATH_MS) {
    //     Serial.println("Max duration reached");
    //     currentState = IDLE;
    //     return true;
    //   }

    //   int toCopy = min(samplesRead, BREATH_BUFFER_SIZE - breathIndex);
    //   for (int i = 0; i < toCopy; i++)
    //     breathBuffer[breathIndex++] = windowBuffer[i];

    //   if (windowRMS < silenceThreshold) {
    //     silenceCounter += samplesRead;

    //     // How long have we been breathing so far?
    //     unsigned long soFar = millis() - breathStartTime;

    //     // Silence required scales with breath duration
    //     // SHORT  < 1.2s → 0.3s silence to end
    //     // NORMAL 1.2–2.5s → 0.5s silence to end  
    //     // LONG   > 2.5s → 1.0s silence to end (allows mid-breath hold)
    //     uint32_t silenceRequired;
    //     if (soFar > 2500) {
    //       silenceRequired = SAMPLE_RATE * 1.0;   // 16000 samples = 1.0s
    //     } else if (soFar > 1200) {
    //       silenceRequired = SAMPLE_RATE * 0.5;   // 8000 samples  = 0.5s
    //     } else {
    //       silenceRequired = SAMPLE_RATE * 0.3;   // 4800 samples  = 0.3s
    //     }

    //     if (silenceCounter > silenceRequired) {
    //       unsigned long breathDuration = millis() - breathStartTime;
    //       if (breathDuration >= MIN_BREATH_MS) {
    //         Serial.printf("Breath ENDED (silence) | soFar=%lums silenceReq=%u\n",
    //                       soFar, silenceRequired);
    //         currentState = IDLE;
    //         return true;
    //       } else {
    //         Serial.println("Too short, resetting");
    //         currentState = IDLE;
    //         breathIndex  = 0;
    //       }
    //     }
    //   } else {
    //     silenceCounter = 0;
    //     currentState   = IN_BREATH;
    //   }
    //   break;
    // }
    // }
    // return false;
    // }


    case IN_BREATH: {
      if (millis() - breathStartTime > MAX_BREATH_MS) {
        Serial.println("Max duration reached");
        currentState = IDLE;
        return true;
      }

      int toCopy = min(samplesRead, BREATH_BUFFER_SIZE - breathIndex);
      for (int i = 0; i < toCopy; i++)
        breathBuffer[breathIndex++] = windowBuffer[i];

      if (windowRMS < silenceThreshold) {
        silenceCounter += samplesRead;
        if (silenceCounter > (SAMPLE_RATE * 0.4)) { //WAS 0.2
          unsigned long breathDuration = millis() - breathStartTime;
          if (breathDuration >= MIN_BREATH_MS) {
            Serial.println("Breath ENDED (silence)");
            currentState = IDLE;
            return true;
          } else {
            Serial.println("Too short, resetting");
            currentState = IDLE;
            breathIndex  = 0;
          }
        }
      } else {
        silenceCounter = 0;
        currentState   = IN_BREATH;
      }
      break;
    }
  }
  return false;
}

// ================================================================
// LOGISTIC REGRESSION INFERENCE
// ================================================================
int predictBreath(int16_t *samples, int N, float durationSec) {
  normalizeBuffer(samples, N);

  float rms  = computeRMS(samples, N) / 32767.0;
  float zcr  = computeZCR(samples, N);
  float var  = computeVariance(samples, N) / (32767.0 * 32767.0);
  float features[4] = {rms, zcr, var, durationSec};

  float normalized[4];
  for (int i = 0; i < 4; i++) {
    normalized[i] = (features[i] - pgm_read_float(&mean_vec[i]))
                    / pgm_read_float(&std_vec[i]);
  }

  float scores[3] = {0, 0, 0};
  for (int c = 0; c < 3; c++) {
    scores[c] = pgm_read_float(&b[c]);
    for (int f = 0; f < 4; f++)
      scores[c] += pgm_read_float(&W[c][f]) * normalized[f];
  }

  int bestClass = 0;
  for (int i = 1; i < 3; i++)
    if (scores[i] > scores[bestClass]) bestClass = i;

  return bestClass;
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("========================================");
  Serial.println("ESP32-S3 Respiratory Sound Classifier");
  Serial.println("========================================");

  pinMode(LED_SHORT,  OUTPUT); digitalWrite(LED_SHORT,  LOW);
  pinMode(LED_NORMAL, OUTPUT); digitalWrite(LED_NORMAL, LOW);
  pinMode(LED_LONG,   OUTPUT); digitalWrite(LED_LONG,   LOW);

  verifyPins();
  initPSRAMBuffer();
  printMemoryStatus();

  // ← I2S init (new API, no clock errors)
  initI2S();

  // print first 10 raw values + max
  {
    int32_t raw[256];
    size_t br;
    i2s_channel_read(rx_handle, raw, sizeof(raw), &br, portMAX_DELAY);
    Serial.printf("I2S bytes read: %d\n", br);
    Serial.print("First 10 raw: ");
    for (int i = 0; i < 10; i++) Serial.printf("%ld ", raw[i]);
    Serial.println();

    int32_t maxVal = 0;
    for (int i = 0; i < 256; i++)
      if (abs(raw[i]) > maxVal) maxVal = abs(raw[i]);
    Serial.printf("Max raw value: %ld\n\n", maxVal);
  }

  calibrateThreshold();
  printMemoryStatus();

  Serial.println("System ready. Start breathing near microphone...");
  Serial.println("Short breath: <1.2s, Normal: 1.2-2.5s, Long: >2.5s");
}

// ================================================================
// MAIN LOOP
// ================================================================
void loop() {
  if (detectAndProcessBreath()) {
    float durationSec = breathIndex / (float)SAMPLE_RATE;
    Serial.printf("\nBreath detected! Samples: %d, Duration: %.2f sec\n",
                  breathIndex, durationSec);

    if (breathIndex < (SAMPLE_RATE * 0.5)) {
      Serial.println("Too short to classify, ignoring.");
    } else {
      int classification = predictBreath(breathBuffer, breathIndex, durationSec);
      // Class order: 0=long  1=normal  2=short  

      digitalWrite(LED_LONG,  classification == 0);
      digitalWrite(LED_NORMAL, classification == 1);
      digitalWrite(LED_SHORT,   classification == 2);

      ///const char* labels[] = {"SHORT", "NORMAL", "LONG"};
      const char* labels[] = {"LONG", "NORMAL", "SHORT"};
      Serial.printf("Classification: %s\n", labels[classification]);

      
      unsigned long ledOnTime = millis();
      while (millis() - ledOnTime < 2000) {
        int32_t dummy[256];
        size_t br;
        i2s_channel_read(rx_handle, dummy, sizeof(dummy), &br, 10);
      }

      digitalWrite(LED_SHORT,  LOW);
      digitalWrite(LED_NORMAL, LOW);
      digitalWrite(LED_LONG,   LOW);
    }

    breathIndex = 0;
  }
  
  if (breathIndex >= BREATH_BUFFER_SIZE) {
    Serial.println("Emergency reset breathIndex");
    breathIndex  = 0;
    currentState = IDLE;
  }

  delay(1);
}
