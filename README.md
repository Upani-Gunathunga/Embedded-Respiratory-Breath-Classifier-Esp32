

---

# 🫁 Embedded Respiratory Breath Classifier (ESP32-S3 + INMP441)

This project demonstrates a **complete embedded machine learning pipeline** that classifies breathing patterns using an **ESP32-S3 microcontroller** and a **digital MEMS microphone (INMP441)**.

---

# 🔎 Overview

This project demonstrates a **complete embedded machine learning pipeline** that classifies breathing patterns using an **ESP32-S3 microcontroller** and a **digital MEMS microphone (INMP441)**.

The system captures respiratory audio signals in real time, extracts lightweight signal features, and performs **on-device inference** to classify breaths into three categories:

🔴 **Short breath**
🔵 **Normal breath**
🟢 **Long breath**

The classification result is displayed through dedicated **LED indicators** connected to the ESP32.

The project was designed as a **human–computer interaction prototype**, demonstrating how low-cost embedded systems can interpret physiological signals in real time.

---

# 🧠 System Architecture

The system consists of four main stages:

```
Respiratory Sound
        │
        ▼
🎤 INMP441 Digital MEMS Microphone
        │
        ▼
⚡ ESP32-S3 I2S Audio Acquisition
        │
        ▼
📊 Signal Feature Extraction
(RMS, ZCR, Variance, Duration)
        │
        ▼
🤖 Multiclass Logistic Regression
        │
        ▼
🫁 Breath Classification
        │
        ▼
💡 LED Feedback Output
```

All processing is performed **locally on the microcontroller** without any external compute resources.

---

# 🔧 Hardware Components

### 🖥️ Microcontroller

**ESP32-S3 N1R8**

Key reasons for selecting this board:

* ⚡ **240 MHz dual core processor**
* 💾 **large SRAM + optional PSRAM**
* 🔊 **native I2S peripheral for digital microphones**
* 🤖 **suitable for embedded ML workloads**

Initial development was attempted using a **NodeMCU ESP32**, but memory constraints limited buffer allocation required for respiratory signal capture.

The ESP32-S3 provided sufficient memory headroom for reliable audio buffering and processing.

---

### 🎤 Microphone

**INMP441 Digital MEMS microphone**

Reasons for selecting this sensor:

* 🔊 Native **I2S digital output**
* ⚙️ Eliminates need for analog ADC conditioning
* 🔇 Low noise floor
* 🔌 Direct compatibility with ESP32 I2S peripheral

This allows stable acquisition of respiratory sound signals without external analog filtering circuits.

---

# 🔌 Hardware Connections

| Component   | ESP32-S3 Pin |
| ----------- | ------------ |
| INMP441 WS  | GPIO 42      |
| INMP441 SCK | GPIO 41      |
| INMP441 SD  | GPIO 4       |

### 💡 LED Indicators

| Breath Type | LED Pin |
| ----------- | ------- |
| 🔴 Short    | GPIO 35 |
| 🔵 Normal   | GPIO 36 |
| 🟢 Long     | GPIO 37 |

---

# 📡 Signal Processing Pipeline

Respiratory signals are captured at:

**Sampling rate:**
`16 kHz`

This rate was selected because:

* 🫁 respiratory audio bandwidth is well below **8 kHz**
* 📐 satisfies **Nyquist sampling requirements**
* 💾 reduces memory requirements compared to higher rates

---

## 🫁 Breath Detection

Breath segments are detected using an **energy-based state machine**.

Three states are used:

```
IDLE
BREATH_STARTED
IN_BREATH
```

Detection is based on **RMS energy thresholds** computed from sliding audio windows.

The system automatically performs **ambient noise calibration** during startup to determine a dynamic silence threshold.

This improves robustness in different environments.

---

# 📊 Feature Extraction

To ensure compatibility with embedded hardware, only lightweight statistical features are used.

During the early design phase, I initially experimented with Mel-Frequency Cepstral Coefficients (MFCCs), which are commonly used in audio classification tasks such as speech recognition. A typical MFCC pipeline extracts around 13 coefficients per frame, often across multiple frames, which significantly increases both memory usage and computational complexity.

While MFCCs can capture detailed spectral information, implementing them on a microcontroller introduces several constraints:

additional FFT operations

higher RAM requirements for frame buffers

increased processing latency

larger model input dimensions

Since the goal of this project was to run real-time inference entirely on the ESP32-S3, I prioritized a feature set that could be computed efficiently using simple arithmetic operations.

Therefore, the final design uses four lightweight statistical features that are computationally inexpensive while still capturing key characteristics of breathing sounds.

These features can be calculated using basic signal statistics, making them well suited for resource-constrained embedded systems.

Four features are extracted per breath:


---

### 1️⃣ RMS Energy

Measures signal intensity:

```
RMS = sqrt(sum(x²) / N)
```

Breathing intensity correlates with airflow energy.

---

### 2️⃣ Zero Crossing Rate (ZCR)

Measures signal frequency changes.

```
ZCR = sign changes / N
```

Respiratory sounds exhibit characteristic oscillation patterns.

---

### 3️⃣ Signal Variance

Captures amplitude variability within the breathing cycle.

---

### 4️⃣ Breath Duration

Duration of the detected breathing event in seconds.

This feature strongly differentiates the three breathing classes.

---

# 🤖 Machine Learning Model

A **multiclass logistic regression model** is used for classification.

Reasons for choosing this model:

* ⚡ extremely lightweight
* 🎯 deterministic inference
* 💾 small memory footprint
* 🔧 easy deployment on microcontrollers
* 🚫 no need for floating-point intensive neural networks

The model is exported directly into C arrays:

```
mean vector
standard deviation vector
weight matrix
bias vector
```

These parameters are embedded in the firmware and evaluated using simple linear algebra.

---

# 📚 Dataset

Training data comes from the publicly available:

**Respiratory Sound Database**

Total recordings: **920**

The dataset contains annotated respiratory cycles recorded from multiple patients.

Only **clean breathing cycles without wheezes or crackles** were used to train the classifier.

This ensures the model learns **breath duration characteristics rather than pathological noise patterns**.

---

# 📈 Dataset Statistics

Total extracted cycles:

**3616**

| Class  | Samples |
| ------ | ------- |
| Long   | 1725    |
| Normal | 1532    |
| Short  | 359     |

---

# 🧪 Model Performance

Test accuracy:

**97%**

Confusion Matrix:

```
Long   → 339 correct
Normal → 289 correct
Short  → 72 correct
```

Precision and recall remain high across all classes.

Short breaths are slightly underrepresented in the dataset, which explains the lower precision for that class.

---

# ⚡ Embedded Inference

During runtime the microcontroller performs the following steps:

1️⃣ detect breath segment
2️⃣ normalize audio signal
3️⃣ compute statistical features
4️⃣ normalize features using training mean/std
5️⃣ compute linear logits
6️⃣ select class with highest score

All inference is executed in **real time on the ESP32-S3**.

No external compute resources are required.

---

# 🎥 Demonstration

Three test scenarios were recorded:

🔴 Short breath
🟢 Normal breath
🔵 Long breath

Each scenario activates the corresponding LED indicator.

Videos demonstrating the system are included in the repository.

---

# 💾 Memory Considerations

Breath signals are stored in a circular buffer.

Buffer size:

```
40,000 samples
```

At a sampling rate of **16 kHz**, this supports breath durations up to approximately **2.5 seconds** with safety margin.

Memory planning was an important design consideration when choosing the microcontroller.

---

# ⚙️ Calibration

At startup the system performs **ambient noise calibration**.

Procedure:

1️⃣ system records background noise
2️⃣ calculates RMS statistics
3️⃣ sets a silence threshold dynamically

This improves detection reliability in different environments.

---

# ⚠️ Limitations

This system currently:

* detects only breath duration patterns
* does not classify pathological sounds
* assumes microphone is positioned close to the mouth

Environmental noise can affect detection performance.

---

# 🚀 Future Improvements

Possible extensions include:

### 🩺 Respiratory disease detection

Detect:

* wheezing
* crackles
* abnormal breathing rhythms

### 🧠 Deep learning models

Deploy tiny neural networks using:

**TensorFlow Lite Micro**

### 📱 Smartphone integration

Transmit breath metrics via **BLE** to mobile health apps.

### 🏥 Clinical monitoring

Use for remote respiratory monitoring in **telemedicine environments**.

---

# 📌 Conclusion

This project demonstrates how **embedded machine learning can enable real-time physiological signal interpretation on low-cost hardware**.

The complete pipeline—from data acquisition to inference—runs locally on a microcontroller, making the system suitable for **portable and resource-constrained healthcare applications**.

---


