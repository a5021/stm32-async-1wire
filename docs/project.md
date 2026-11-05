# Project

[Back to the README](../README.md)

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the full guide (bug reports via
the issue templates, feature requests, Code of Conduct, PR workflow). Quick
version:

1. Fork and create a feature branch (`git checkout -b fix/my-change`).
2. Make your changes and keep the host tests green: `make test` (includes the
   DS2431 suite) for the default F1 backend, or `make test-all-families` for
   every backend; `make test-<family>` runs one (`f0`, `f1`, `f3`, `f4`, `g0`,
   `g4`, `l4`, `h5`, `h7`).
3. Format touched sources with the repo's `.clang-format`
   (`clang-format --dry-run --Werror <files>` must pass), using the
   `clang-format` version CI pins — see the `format` job in
   `.github/workflows/ci.yml`, which is the source of truth; a different
   release can disagree about the same source.
4. Open a Pull Request — CI builds every target and runs the test suite.

## Support

For issues and questions, please open an issue on GitHub.

## References

- DS18B20 Datasheet  
  https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf

- STM32F103 Reference Manual  
  https://www.st.com/resource/en/reference_manual/cd00171190-stm32f101xx-stm32f102xx-stm32f103xx-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf

- STM32F030 Reference Manual  
  https://www.st.com/resource/en/reference_manual/rm0091-stm32f0x1stm32f0x2stm32f0x8-advanced-armbased-32bit-mcus-stmicroelectronics.pdf

- STM32G031 Reference Manual  
  https://www.st.com/resource/en/reference_manual/rm0444-stm32g0x1stm32g0x2-advanced-armbased-32bit-mcus-stmicroelectronics.pdf

- STM32F405/F407 Reference Manual (RM0090)
- STM32F401 Reference Manual (RM0368)
- STM32F446 Reference Manual (RM0390)
- STM32F303 Reference Manual (RM0316)

- 1-Wire Protocol Specification  
  https://www.analog.com/en/resources/technical-articles/1wire-communication.html
