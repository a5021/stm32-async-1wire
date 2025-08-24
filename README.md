# Non-Blocking DS18B20 Driver for STM32F103

A bare-metal, register-level driver for the DS18B20 temperature sensor. This driver uses a sophisticated hybrid architecture with a hardware timer (TIM1) and DMA to achieve precise 1-Wire protocol timing without any software delays, busy-waits, or interrupts.

## 🚀 Features

- Pure Bare-Metal: Direct register manipulation, no HAL or LL libraries.
- Zero Interrupts: Does not use any NVIC interrupts. Fully polled operation.
- Hardware Automation: Uses TIM1 Output Compare and Input Capture with DMA to automate waveform generation and data capture.
- State Machine Architecture: Event-driven operation controlled by hardware completion signals.
- Weak Function Callbacks: Customizable LED control and temperature reporting.
- CRC Validation: Automatic checksum verification of sensor data.

## 📋 Requirements

- Microcontroller: STM32F103C8T6 (Blue Pill) or compatible
- Sensor: DS18B20 digital temperature sensor
- Toolchain: GCC ARM or Keil with C99 support
- Clock Configuration: 72MHz system clock (configured by the driver)

## 📁 File Structure

```
├── demo.c              # Example application with UART output
├── ds18b20.c           # Main driver implementation
├── ds18b20.h           # Driver interface and constants
└── macro.h             # STM32 register access macros
```

## 🛠️ Hardware Connections

| STM32F103 Pin | Function     | DS18B20 Pin |
|---------------|--------------|-------------|
| PA8           | 1-Wire Data  | DQ (Data)   |
| 3.3V          | Power        | VDD         |
| GND           | Ground       | GND         |

Note: A 4.7kΩ pull-up resistor is required between the PA8 and 3.3V lines.

## 🚀 Quick Start

### 1. Include the Driver

```
#include "ds18b20.h"
```

### 2. Initialize the Driver

```
int main(void) {
    ds18b20_init();  // One-time initialization

    while (1) {
        ds18b20_poll();  // Call repeatedly from main loop
        // Other application code...
    }
}
```

### 3. Implement Callbacks (Optional)

```
// LED status indication
void ds18b20_led_control(unsigned action) {
    if (action) {
        // Turn LED on (measurement in progress)
        GPIOC->BSRR = GPIO_BSRR_BR13;
    } else {
        // Turn LED off (measurement complete)
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

// Temperature result handling
void ds18b20_temp_ready(int16_t temp) {
    if (temp >= -550 && temp <= 1250) {
        // Valid temperature in tenths of °C
        printf("Temperature: %d.%d°C\n", temp/10, abs(temp%10));
    } else {
        // Error condition
        switch (temp) {
            case DS18B20_TEMP_ERROR_NO_SENSOR:
                printf("Error: No sensor detected\n");
                break;
            case DS18B20_TEMP_ERROR_CRC_FAIL:
                printf("Error: CRC check failed\n");
                break;
        }
    }
}
```

## Building the Project with `make`

This project uses a `Makefile` to simplify building, cleaning, and
programming the STM32 firmware. Below are the available commands and
their purpose.

### **Prerequisites**

-   **Toolchain:** `arm-none-eabi-gcc` and related utilities (`objcopy`,
    `size`) must be installed.
-   **wget:** Required for downloading external dependencies.
-   **Programmer tools:**
    -   **ST-LINK:** `st-flash` (Linux/macOS) or `ST-LINK_CLI.exe`
        (Windows)
    -   **J-LINK:** `JFlashExe` or `JFlash.Exe`

### **Basic Usage**

Run `make` in the project root directory.

``` bash
make
```

This will: 1. Check and download required external dependencies (CMSIS
headers, startup code, linker script, etc.). 2. Compile the source files
and assemble the startup code. 3. Link everything into an ELF
executable. 4. Generate `.hex` and `.bin` files for flashing. 5. Print
the size of the final binary.

The default output directory is `build/`.

### **Common Targets**

  `make` or `make all`   Build the project (default target).

  `make debug`           Build with debug symbols (`-Og -g3`).

  `make clean`           Remove all build artifacts (object files, binaries,
                         map file).

  `make download-deps`   Download all missing external dependencies.

  `make clean-deps`      Remove downloaded external dependencies.

  `make gccversion`      Show the version of `arm-none-eabi-gcc`.

  `make help`            Display all available targets.


### **Programming the Device**

After building, you can program the firmware to the STM32 device using:

-   **ST-LINK:**

    ``` bash
    make program
    ```

    -   Uses `st-flash` on Linux/macOS or `ST-LINK_CLI.exe` on Windows.
    -   Writes the generated `.hex` file to the MCU flash and resets the
        device.

-   **J-LINK:**

    ``` bash
    make jprogram
    ```

    -   Uses Segger J-Link utilities (`JFlashExe` or `JFlash.Exe`).
    -   Automatically loads and programs the `.hex` file.

### **Configuration Notes**

-   **Target Name:** The firmware target name is `ds18b20_demo`.

-   **Build Directory:** Default is `build/`.

-   **Optimization Level:**

    -   **Release:** `-O3 -flto` (default).
    -   **Debug:** `-Og -g3`.

-   **MCU Flags:** Configured for `STM32F103xB` (Cortex-M3).

-   **Additional Defines:** You can pass extra preprocessor flags by
    setting `EXTRA_FLAGS`:

    ``` bash
    make EXTRA_FLAGS=MY_FEATURE
    ```

## ⚙️ Architecture

### Hybrid Hardware Automation

This driver uses an advanced technique that combines multiple hardware features:

1. Timer-Driven Sequences: TIM1 is configured in One-Pulse Mode (OPM). Each state machine step configures the timer for a specific operation (reset, write byte, read byte, wait) and starts it.
2. DMA for Data Transfer: DMA is used in two key ways:
   - Transmit (DMA1_Channel4): Feeds a pre-calculated sequence of Compare Register (CCR) values to TIM1->CCR1 to automatically generate the precise waveform for writing commands or bits.
   - Capture (DMA1_Channel3): Automatically stores values from the TIM1->CCR2 capture register into memory to record pulse timings during read operations or presence detection.
3. Update Event as Completion Signal: The core polling mechanism checks the Timer Update Flag (TIM1->SR UIF). This flag is set when the timer completes its one-pulse countdown, signaling that the autonomous hardware operation (e.g., sending a reset pulse, waiting 750ms) is finished.
4. True Zero-ISR Overhead: The ds18b20_poll() function checks this flag. When set, it clears the flag and advances the state machine to the next step. This makes the entire driver event-driven by hardware completion signals without using interrupts.

### Non-Blocking, Interrupt-Free Design Principles

1. No Software Delays: No `delay_us()` or similar functions.
2. No Interrupts: Does not configure or use the NVIC. Fully deterministic.
3. Hardware Completion Events: The state machine advances only when the hardware timer signals that its current automated task is complete.
4. Minimal CPU During Operations: The CPU is only actively involved to set up a hardware operation and to process the result once it completes.

### Hardware Resources Used

- TIM1 & Channels 1, 2, 4: The core timer resources.
  - CH1 (PWM Mode 2, Output Compare): Configured in PWM Mode 2, driving PA8 as the 1-Wire output on an active-low bus.
    - Each 1-Wire bit time slot is implemented as a single PWM period with a low (active) portion encoding the bit:
      - Short low (~1–2µs) → logical '1'
      - Long low (~60µs) → logical '0'
    - Reset (~480µs) is generated as an extended low period (active-low) within a ~960µs slot.
  - CH2 (Input Capture, Indirect mode): Shares the same PA8 pin internally. Used to capture presence pulses and read-slot timings after CH1 releases the bus to idle-high; DMA transfers CCR2 capture values to memory.
  - CH4: Used as a DMA trigger, feeding CCR1 duty cycles (for CH1 output) and facilitating capture operations.
- RCR (Repetition Counter Register): Key to the state machine operation. Instead of generating an Update Event on every period, RCR controls how many timer repetitions occur before UIF is set.
  - Example: RCR=15 → the timer generates 16 PWM slots (bits) via DMA, then asserts UIF once at the end, signaling software to proceed.
  - This allows grouping a full command (two bytes), the entire 72-bit read, or long delays into single hardware-driven transactions, freeing the CPU until completion.
- DMA1_Channel3: Peripheral-to-memory transfers from TIM1->CCR2 (captured timings).
- DMA1_Channel4: Memory-to-peripheral transfers to TIM1->CCR1 (PWM duty cycles).
- GPIO Pin: PA8 configured in alternate function open-drain; CH1 output and CH2 capture are multiplexed onto this single pin.

### State Machine Flow (hardware-timed; polled on UIF)

Kickstart behavior
- After ds18b20_init(), the timer update flag (UIF) is already set. This ensures the very first call to ds18b20_poll() advances the state machine immediately without any extra priming step.

- IDLE (state 0)
  - Immediately falls through into START with no events required. Prepares context (ctx.fill_union = -1), ensures LED is off.
  - Set state=1.

- START (state 1)
  - LED on. Run reset_bus():
    - CH1 issues active-low reset pulse (~480µs within ~960µs slot).
    - CH2 (indirect input) captures presence timing into ctx.edge[0..1] via DMA from CCR2.
  - Set state=2.

- CONVERT (state 2)
  - On UIF, check_presence() with ctx.edge[].
    - If present: send_command(conv_cmd) “Skip ROM 0xCC + Convert T 0x44” via CH1+DMA (16 slots, RCR=15); set state=3.
    - Else: report NO_SENSOR; start 5s pause; set state=0.

- WAIT (state 3)
  - On UIF: wait_conversion() schedules ~750ms (start_timer(62500,11)); set state=4.

- CONTINUE (state 4)
  - On UIF: run reset_bus() again; set state=5.

- REQUEST (state 5)
  - On UIF, check_presence().
    - If present: send_command(read_cmd) “Skip ROM 0xCC + Read 0xBE” via CH1+DMA (16 slots); set state=6.
    - Else: report NO_SENSOR; start 5s pause; set state=0.

- READ (state 6)
  - On UIF: read_data() schedules 72 slots (RCR=71; ARR≈62µs). CH1 emits ~1µs active-low kick at each slot start and then releases; CH2 captures sensor pulse timing; DMA fills ctx.pulse[72]. Set state=7.

- DECODE (state 7)
  - On UIF: decode_scratchpad() from ctx.pulse[] into ctx.scratchpad[], LED off; verify CRC; report temperature or CRC_FAIL; start 5s pause; set state=0.

#### Detailed State Descriptions

| State Number | State Name     | Description                                                                                                                              | Next State(s)                                                                                               |
| :----------- | :------------- | :--------------------------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------- |
| **0**        | **IDLE**       | Initial state. Immediately falls through into START with no events required; prepares the context for a new measurement cycle by initializing the data union. | State 1 (START)                                                                                             |
| **1**        | **START**      | Begins the measurement cycle. Turns on the user LED (if implemented) and initiates a **1-Wire bus reset** sequence to detect devices.     | State 2 (CONVERT)                                                                                           |
| **2**        | **CONVERT**    | Checks if a DS18B20 responded correctly to the reset. If present, sends the **Convert T (`0x44`)** command. If not, reports an error.     | State 3 (WAIT) on success.<br/>State 0 (IDLE) after pause on error.                                         |
| **3**        | **WAIT**       | Starts a non-blocking timer delay (~750 ms) to wait for the temperature conversion inside the DS18B20 to complete.                       | State 4 (CONTINUE)                                                                                          |
| **4**        | **CONTINUE**   | Initiates a second **1-Wire bus reset** sequence to prepare for reading the converted data.                                              | State 5 (REQUEST)                                                                                           |
| **5**        | **REQUEST**    | Checks for the DS18B20's presence again. If present, sends the **Read Scratchpad (`0xBE`)** command. If not, reports an error.            | State 6 (READ) on success.<br/>State 0 (IDLE) after pause on error.                                         |
| **6**        | **READ**       | Reads the **9 bytes of scratchpad data** (including CRC) from the sensor using precise pulse-width measurement via timer input capture. | State 7 (DECODE)                                                                                            |
| **7**        | **DECODE**     | **Decodes** the captured pulse widths into data bytes, validates the **CRC**, converts the raw temperature, and reports the result. Turns off the user LED. Starts a pause before the next cycle. | State 0 (IDLE) after pause.                                                                                 |

## 📊 API Reference

### Core Functions

```
void ds18b20_init(void);
```
Initialize the DS18B20 driver. Configures the system clock to 72MHz, enables peripherals (GPIOA, TIM1, DMA1), and sets up the timer prescaler for 1µs resolution. This function does NOT start the state machine.

```
void ds18b20_poll(void);
```
The Core Driver Function: Must be called from the main loop. It checks the Timer Update Flag (UIF). If the flag is set, it means the hardware has finished the previous operation (e.g., sending a command, waiting for conversion). The function then clears the flag and advances the internal state machine to the next step. The driver's state is persistent, so this function can be called at any rate without risk of getting stuck.

### Weak Callbacks

```
void ds18b20_led_control(unsigned action);
```
Called to indicate measurement status (LED control). action is 1 for start, 0 for stop.

```
void ds18b20_temp_ready(int16_t temp);
```
Called when a temperature measurement is complete or an error occurs.

### Error Codes

- DS18B20_TEMP_ERROR_NO_SENSOR: No sensor detected on the bus.
- DS18B20_TEMP_ERROR_CRC_FAIL: Data corruption detected via CRC mismatch.
- DS18B20_TEMP_ERROR_GENERIC: Unspecified communication error.

## 📈 Performance

- Time to result (one measurement): ~0.76 s (750 ms conversion + protocol overhead)
- Inter-measurement pause: 5 s (configurable)
- Precision: 0.1°C resolution
- Accuracy: ±0.5°C (typical)
- CPU Usage: Minimal; CPU is free to perform other tasks during waits.

## 🔧 Configuration

### Timing Constants

Adjustable in ds18b20.c:
```
#define RESET_PULSE_MIN       480U    // µs
#define RESET_PULSE_MAX       540U    // µs
#define ONE_PULSE                1    // µs
#define ZERO_PULSE              60    // µs
```

### Pin Configuration

Modify in ds18b20.c:
```
// Change PA8 to desired pin
GPIOA->CRH |= GPIO_CRH_CNF8_0 | GPIO_CRH_MODE8_1;
```

## 🐛 Troubleshooting

### Common Issues

1. "No sensor detected" or "CRC check failed" errors
   - Cause: The most common cause is electrical. The presence pulse captured by the DMA/timer did not meet the timing criteria, or noise corrupted the data during the 72-bit read.
   - Fix:
     - Check all wiring connections.
     - Ensure a 4.7kΩ pull-up resistor is between the PA8 (DQ) line and 3.3V.
     - Verify stable power is supplied to the DS18B20 sensor.
     - Keep data lines short to minimize noise and capacitance.

2. Temperature readings are infrequent
   - Cause: The ds18b20_poll() function is called slowly from the main loop. The driver operates correctly but advances through its states (e.g., the 750ms conversion wait) at a slower pace.
   - Fix: This is often not a problem if a slow update rate is acceptable. If faster updates are needed, ensure the main loop runs frequently and avoids other blocking code. The driver itself is non-blocking and will not cause this slowdown.

### Debugging Tips

- Monitor the State Variable: Check the value of ctx.current_state in a debugger to see the current step in the communication sequence.
- Check the Update Flag: Read the TIM1->SR register. If the driver seems idle, a set UIF bit indicates a completed operation is waiting to be processed by ds18b20_poll().
- Inspect the GPIO: Use an oscilloscope on PA8 to verify the 1-Wire waveforms. Look for:
  - A clean ~480µs reset pulse (MCU pulls low, then releases).
  - A presence pulse ~60-240µs after the reset pulse (sensor pulls low).
  - Precise "write" slots: a short ~1–2µs low for a '1', a long ~60µs low for a '0'.
- Inspect Captured Data: Examine the ctx.edge[] array after a reset or the ctx.pulse[] array after a read to see the raw timing data the driver is using to detect presence and decode bits.

## 📄 License

This project is released under the MIT License. See the LICENSE file for details.

## 🤝 Contributing

1. Fork the repository
2. Create your feature branch (git checkout -b feature/AmazingFeature)
3. Commit your changes (git commit -m 'Add some AmazingFeature')
4. Push to the branch (git push origin feature/AmazingFeature)
5. Open a Pull Request

## 📞 Support

For issues and questions, please open an issue on GitHub.

## 📚 References

- DS18B20 Datasheet  
  https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf

- STM32F103 Reference Manual  
  https://www.st.com/resource/en/reference_manual/cd00171190-stm32f101xx-stm32f102xx-stm32f103xx-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf

- 1-Wire Protocol Specification  
  https://www.maximintegrated.com/en/design/technical-documents/tutorials/1/1796.html
