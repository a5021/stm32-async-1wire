# Contributing to Non-Blocking DS18B20 Driver

First off, thank you for taking the time to contribute! This is a
small, focused bare-metal driver, and every issue and pull request
makes it better.

## Code of Conduct

This project and everyone participating in it is governed by the
[Code of Conduct](CODE_OF_CONDUCT.md). By participating you agree to
uphold this code.

## How can I contribute?

### Reporting bugs

Before creating a bug report, please check the [Troubleshooting](README.md#troubleshooting)
section of the README — most hardware issues are wiring-related.

When creating an issue, use the **Bug Report** template and include:

- Your hardware setup (board, sensor, wiring, pull-up resistor)
- Clock configuration (HSE+PLL 72 MHz or `HSI_8MHZ=1`)
- Build output and any UART diagnostics you see
- What you expected to happen vs. what actually happened

### Requesting features

Use the **Feature Request** template. Explain the use case and, if
possible, how it fits the driver's design goals (non-blocking,
interrupt-free, minimal CPU usage).

### Submitting code

1. Fork the repository and create a feature branch:
   `git checkout -b feature/AmazingFeature`
2. Make your changes. Follow the existing code style:
   - 4-space indentation, K&R braces, left-aligned pointer stars
     (see `.clang-format`)
   - Keep the driver non-blocking and interrupt-free
   - Register-level access only — no HAL/LL
3. Make sure `make` builds cleanly (it builds with `-Wall -Werror`),
   and `make HSI_8MHZ=1` for the HSI variant.
4. Run the code quality checks used in CI:
   - `clang-format --dry-run --Werror inc/ds18b20.h src/ds18b20.c src/demo.c`
   - `cppcheck --enable=warning,style,performance,portability ...`
5. Update the README and the [CHANGELOG](CHANGELOG.md) if your change
   affects behavior or usage.
6. Commit your changes with a concise, descriptive message.
7. Push to the branch and open a Pull Request using the PR template.

## Style guide

- Follow the existing naming conventions in `inc/` and `src/`.
- Keep functions short and focused on a single responsibility.
- Prefer explicit register access via the macros in `macro.h`.
- Do not introduce `delay_us()` calls or interrupt handlers.

## Build checklist

```bash
make            # release build (must succeed with -Werror)
make HSI_8MHZ=1 # HSI variant
make debug      # optional, for debugging
```

Thanks again for helping improve this driver!
